-- Wrap the __rd_bind() function
local bind = __rd_bind
__rd_bind  = nil

-- Alias the original print() as trace()
local trace = print

-------------------------------------------------------------------------------
-- Contexts manager
-------------------------------------------------------------------------------
local GetCtx, CtxClass, SwitchCtx, RestoreCtx
local RegisterDealloc
do
    local ctxs = {}             -- List of every context currently available
    local ctx  = 0              -- Currently enabled context
    local freehandlers = {}     -- Dealloc handlers

    -- New context allocated context allocations
    bind("AllocContext", function(ctx, class)
        -- Check if context is not already allocated
        if ctxs[ctx] then
            error("attempt to alloc an already allocated context: " .. ctx .. " " .. class)
        end

        -- Allocate
        ctxs[ctx] = class
    end)

    -- Tracks context deallocations
    bind("DeallocContext", function(ctx)
        -- Check if context is allocated
        if not ctxs[ctx] then
            error("attempt to dealloc a not allocated context: " .. ctx)
        end

        -- Run cleaners
        for i = 1, #freehandlers do
            freehandlers[i](ctx)
        end

        -- Dealloc
        ctxs[ctx] = nil
    end)

    -- Returns the current context
    function GetCtx()
        return ctx or 0
    end

    -- Returns the class of a given context
    function CtxClass(ctx)
        return ctxs[ctx] or "UNKNOWN"
    end

    -- Registers a context deallocation handler
    function RegisterDealloc(handler)
        table.insert(freehandlers, handler)
    end

    --
    -- Context stack management
    --

    -- The context stack
    -- Every context switch push the old context on it and
    -- context restores pops its value from it
    local ctxStack = {}

    -- Checks if a given context class is script-allowed
    -- Only API-Client and Internal are allowed contexts
    local classes_whitelist = { ["API_CLIENT"] = true, ["INTERNAL"] = true }
    local function IsCtxAllowed(ctx)
        return classes_whitelist[CtxClass(ctx)] or false
    end

    -- Switch to a new context and store the old one on the stack
    function SwitchCtx(new_ctx)
        -- Checks that the targeted context is allowed as a
        -- script context
        if IsCtxAllowed(new_ctx) then
            ctxStack[#ctxStack + 1] = GetCtx()
            ctx = new_ctx
        else
            error("attempt to switch to a forbidden context type")
        end
    end

    -- Restore the old context from the stack
    function RestoreCtx()
        local top = #ctxStack

        -- Checks that the stack is not empty
        if top > 0 then
            ctx = ctxStack[top]
            table.remove(ctxStack, top)
        else
            error("attempt to restore the previous context despite an empty context stack")
        end
    end

    bind("SwitchContext", SwitchCtx)
    bind("RestoreCtx", RestoreCtx)
end

-------------------------------------------------------------------------------
-- Output wrapper
-------------------------------------------------------------------------------
do
    -- void send(data, fd)
    local send_internal = __rd_send
    __rd_send = nil

    -- Print is now context-aware and send() data
    -- to API client when called from an API context
    function print(...)
        if CtxClass(GetCtx()) ~= "API_CLIENT" then
            -- This is not an API context
            trace(...)
        else
            -- List of every arguments
            local n = select("#", ...)
            local a = {...}

            -- Build the output string part-by-part
            local buffer = {}
            for i = 1, n do
                buffer[#buffer + 1] = tostring(a[i])
            end

            -- Concat & send
            send_internal(table.concat(buffer, "\t") .. "\r\n", GetCtx())
        end
    end

    -- Raw send function
    function send(...)
        -- List of every arguments
        local n = select("#", ...)
        local a = {...}

        -- Build the output string part-by-part
        local buffer = {}
        for i = 1, n do
            buffer[#buffer + 1] = tostring(a[i])
        end

        -- Concat & send
        send_internal(table.concat(buffer, ""), GetCtx());
    end
end

-------------------------------------------------------------------------------
-- EventEmitter
-------------------------------------------------------------------------------
do
    -- List of all registered event emitters
    local emitters = {}

    -- Set emitters as weak values
    setmetatable(emitters, { ["__mode"] = "k"})

    function EventEmitter(self)
        -- Optional parameter
        self = self or {}

        -- List of registered event handlers
        local events = {}

        --
        -- Emits an event
        --
        function self.Emit(event, ...)
            -- This event has nothing registered to it
            if not events[event] then return end

            -- Loops over every handlers
            for idx, handler in ipairs(events[event]) do
                -- Safe call, error in one handler should not prevent
                -- others to be run correctly
                SwitchCtx(handler.ctx)
                local success, error = pcall(handler.fn, ...)
                RestoreCtx()
                if not success then
                    print("[LUA]\t Error while dispatching event: " .. error)
                end
                -- Delete once handler
                if handler.once then
                    table.remove(events[event], idx)
                end
            end
        end

        --
        -- Attaches a new handler to an event
        --
        local function on(event, fn, persistent, once)
            -- First time an event is registered
            if not events[event] then
                events[event] = {}
            end

            -- Adds this function to the event handler
            -- table along with context informations
            table.insert(events[event], {
                ctx = GetCtx(),
                fn  = fn,
                persistent = persistent,
                once = once
            })

        end

        function self.On(event, fn, persistent)
            return on(event, fn, persistent, false)
        end

        function self.Once(event, fn, persistent)
            return on(event, fn, persistent, true)
        end

        --
        -- Detach a function previously registered with On
        --
        -- If the fn argument is nil, disables every handlers
        -- registered from the current context on this event
        --
        function self.Off(event, fn)
            -- Nothing registered to this event, so obviously
            -- nothing to remove...
            if not events[event] then return end

            -- Current script context
            local ctx = GetCtx();

            -- Check matching handler
            for idx, handler in ipairs(events[event]) do
                if fn then
                    if handler.fn == fn then
                        table.remove(events[event], idx)
                    end
                elseif handler.ctx == ctx then
                    table.remove(events[event], idx)
                end
            end
        end

        --
        -- Function called when a file descriptor is deallocated
        --
        -- This functions checks every handlers and removes those
        -- registered from that context
        --
        emitters[self] = function(ctx)
            for _, handlers in pairs(events) do
                for idx, handler in ipairs(handlers) do
                    if handler.ctx == ctx then
                        if handler.persistent then
                            -- Handler is persistent, inherited by ctx 0
                            handler.ctx = 0
                        else
                            table.remove(handlers, idx)
                        end
                    end
                end
            end
        end

        return self
    end

    --
    -- Cleanup every emitters
    --
    RegisterDealloc(function(ctx)
        for emitter, cleanup in pairs(emitters) do
            cleanup(ctx)
        end
    end)
end

-------------------------------------------------------------------------------
-- C-Events
-------------------------------------------------------------------------------
do
    -- EventEmitter object for C-events
    local cEvents = EventEmitter()

    -- Global API
    bind("DispatchEvent", cEvents.Emit)
    On = cEvents.On
    Off = cEvents.Off
end

-------------------------------------------------------------------------------
-- Timers
-------------------------------------------------------------------------------
do
    -- tid create_timer(initial, interval, fn)
    -- Creates a new timer firing after `initial` milliseconds
    -- and then every `interval` milliseconds, running `fn`
    local create_timer = __rd_create_timer
    __rd_create_timer = nil

    -- void cancel_timer(tid)
    -- Cancel a timer from a user-data returned by create_timer()
    local cancel_timer = __rd_cancel_timer
    __rd_cancel_timer = nil

    -- void unregister_timer(tid)
    -- Unregister the callback function
    local unregister_timer = __rd_unregister_timer
    __rd_unregister_timer = nil

    -- List of all defined timers
    local timers = {}
    local soft_tid = 0

    -- Create a new timer that will fire for the first time after `initial`
    -- milliseconds and then every `interval` milliseconds, calling the
    -- callback function `fn`
    function CreateTimer(initial, interval, fn)
        -- Current context
        local ctx = GetCtx()

        -- Force a minimum delay for the timer to fire at least one time
        if initial < 1 then
            initial = 1
        end

        -- Create the timer
        local tid = create_timer(initial, interval, function()
            -- Enter context before execution
            SwitchCtx(ctx)

            -- Exec the timer callback function
            local success, error = pcall(fn)
            if not success then
                print(error)
            end

            -- Restore old context after execution
            RestoreCtx()
        end)

        -- Allocate a new soft id for this timer
        -- and save the context of this timer
        soft_tid = soft_tid + 1
        timers[tid] = {
            ["ctx"] = ctx,
            ["tid"] = tid,
            ["soft_tid"] = soft_tid
        }

        return timers[tid]
    end

    local function cancelTimerInternal(tid)
        if not timers[tid] then return end
        timers[tid] = nil                 -- delete the Lua reference
        cancel_timer(tid)                 -- cancel the timer
        unregister_timer(tid)             -- unregister the callback
    end

    -- Cancel a not-yet-fired timer
    function CancelTimer(timer)
        if type(timer) ~= "table"
        or not timers[timer.tid]
        or timers[timer.tid].soft_tid ~= timer.soft_tid then
            return
        end
        cancelTimerInternal(timer.tid)
    end

    -- Cleanup after automatic collection of one-time timers
    bind("DeleteTimer", function(tid)
        if not timers[tid] then return end
        timers[tid] = nil        -- delete the Lua reference
        unregister_timer(tid)    -- unregisters the callback
        -- There is no need to cancel the timer as this is
        -- an auto-collected timer already expired.
    end)

    -- Tracks context deallocations and cancel timers associated
    -- with these contexts
    RegisterDealloc(function(ctx)
        for tid, timer in pairs(timers) do
            if timer.ctx == ctx then
                cancelTimerInternal(tid)
            end
        end
    end)
end

-------------------------------------------------------------------------------
-- Loading helper
-------------------------------------------------------------------------------
function load(file)
    local f, e = loadfile(file)
    if f then
        f()
    else
        print("Error loading file: " .. e)
    end
end
