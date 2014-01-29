#include "raild.h"
#include <luajit-2.0/lua.h>
#include <luajit-2.0/lualib.h>
#include <luajit-2.0/lauxlib.h>

/*
 * Everything Lua-related
 *
 * Lua is used as an embedded scripting language to control the circuit logic.
 *
 * The C-side of Raild handles everything related to low-level communication
 * with RailHub and TCP/IP API clients and emits events to Lua code to notify
 * changes. Lua scripts can then act accordingly.
 */

// Black-magic for the embedded stdlib.lua file
// This Lua script is embbeded inside the Raild executable file at compile time
extern char _binary_src_stdlib_lua_start; // Pointer to the first caracter of the Lua script
extern char _binary_src_stdlib_lua_size;  // This variable address is the length of the script

// Declare a new Lua CFunction to be called from Lua code
#define API_DECL(name) static int lualib_##name(lua_State *L)

// Creates the library entry for loading every functions into Lua state
#define API_LINK(name) { #name, lualib_##name }

// The Lua VM object
lua_State *L;

//
// Safely run a Lua function
//
static int call(int nargs, int nres) {
	if(lua_pcall(L, nargs, nres, 0) != 0) {
		printf("error: %s\n", lua_tostring(L, -1));
		return 0;
	}

	return 1;
}

//
// Setup everything Lua-related
//
void setup_lua(const char *main) {
	// Setup the lua engine
	printf("[LUA]\t Init Lua engine\n");
	L = luaL_newstate();

	// Open standards libraries
	luaL_openlibs(L);

	// Load the Raild API
	lua_pushvalue(L, LUA_GLOBALSINDEX);
	lualib_register(L);
	lua_pop(L, 1);

	// Black-magic
	// This load the embedded stdlib.lua file
	// As previously noted, the address (and not the value) of the ..._size
	// variable is the length of the embedded script file.
	if(luaL_loadbuffer(L, &_binary_src_stdlib_lua_start, (size_t) &_binary_src_stdlib_lua_size, "stdlib") != 0) {
		printf("load: %s\n", lua_tostring(L, -1));
		exit(1);
	}
	call(0, 0);

	// Load the main script if provided
	if(main) {
		printf("[LUA]\t Loading local script: %s\n", main);

		// Load and run
		if(luaL_loadfile(L, main) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
			printf("load: %s\n", lua_tostring(L, -1));
			exit(1);
		}
	}
}

//
// Timer handler
//
void lua_handle_timer(raild_event *event) {
	// Fetch the function associated with this timer
	lua_pushlightuserdata(L, event);
	lua_gettable(L, LUA_REGISTRYINDEX);

	// Call it
	call(0, 0);
}

//
// Prepares an event for dispatching
//
static int event_prepare(const char *ev) {
	// Fetch the event function from global variables
	lua_getfield(L, LUA_GLOBALSINDEX, ev);

	// lua_getfield returned nil
	// This function does not exist
	if(lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}

	return 1;
}

//
// --- Lua Events ---
//

//
// Ready event
// Fired ever time the RailHub sends a READY opcode
//
int lua_onready() {
	if(!event_prepare("OnReady")) return 0;
	return call(0, 0);
}

//
// Disconnect event
// Fired when RailHub failed to send keep-alive opcode and
// is now considered disconnected
//
int lua_ondisconnect() {
	if(!event_prepare("OnDisconnect")) return 0;
	return call(0, 0);
}

//
// Sensor changed event
// Fired when any of the 24 sensors changes state
//
int lua_onsensorchanged(int sensorid, bool state) {
	if(!event_prepare("OnSensorChanged")) return 0;
	lua_pushnumber(L, sensorid);
	lua_pushboolean(L, state);
	return call(2, 0);
}

//
// Switch changed event
// Fired when any of the 8 switches changes state
//
int lua_onswitchchanged(int switchid, bool state) {
	if(!event_prepare("OnSwitchChanged")) return 0;
	lua_pushnumber(L, switchid);
	lua_pushboolean(L, state);
	return call(2, 0);
}

//
// --- LUA C API ---
// C functions to be called by Lua code
//

//
// exit()
// A way for Lua scripts to kill the whole process
//
API_DECL(exit) {
	printf("[LUA]\t Script killed the main process!\n");
	exit(2);
}

//
// HubReady()
// Returns the readiness of RailHub
//
API_DECL(HubReady) {
	lua_pushboolean(L, get_hub_readiness());
	return 1;
}

//
// TimerCreate(initial, interval, fn)
// Creates a new timer ticking after 'initial' millisecond and the
// every 'interval' millisecond until canceled
// If interval == 0, this is a one-time timer automatically canceled
// after its first tick
//
API_DECL(CreateTimer) {
	// Check arguments
	int initial  = luaL_checknumber(L, 1);
	int interval = luaL_checknumber(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);

	// Create the timer
	raild_event *event = raild_timer_create(initial, interval, RAILD_EV_LUA_TIMER);

	// Store the callback function
	lua_pushlightuserdata(L, event);
	lua_pushvalue(L, 3);
	lua_settable(L, LUA_REGISTRYINDEX);

	// Return the timer event object as light user data
	lua_pushlightuserdata(L, event);
	return 1;
}

//
// TimerCancel(timer)
// Cancels the timer
//
API_DECL(CancelTimer) {
	luaL_checktype(L, 1, LUA_TLIGHTUSERDATA);
	raild_event *event = (raild_event *) lua_touserdata(L, 1);

	// Ensure the timer still exists
	lua_pushlightuserdata(L, event);
	lua_gettable(L, LUA_REGISTRYINDEX);
	if(lua_isnil(L, -1)) {
		luaL_error(L, "attempt to cancel an already canceled timer");
	}

	// Effectively removes it
	raild_timer_delete(event);

	// Remove the callback function from the registry
	lua_pushlightuserdata(L, event);
	lua_pushnil(L);
	lua_settable(L, LUA_REGISTRYINDEX);

	return 0;
}

//
// GetSwitch(switch_id)
// Return the cached state for the requested switch
//
API_DECL(GetSwitch) {
	int sid = luaL_checknumber(L, 1);
	if(sid < 1 || sid > 8) {
		luaL_error(L, "out of bounds switch id");
	}

	lua_pushboolean(L, get_hub_state(RHUB_SWITCHES) & (1 << (sid - 1)));
	return 1;
}

//
// SetSwitch(switch_id, new_state)
// Request a switch state modification
//
API_DECL(SetSwitch) {
	int  sid   = luaL_checknumber(L, 1);
	bool state = lua_toboolean(L, 2);

	if(sid < 1 || sid > 8) {
		luaL_error(L, "out of bounds switch id");
	}

	if(state) {
		uart_setswitch_on(sid - 1);
	} else {
		uart_setswitch_off(sid - 1);
	}

	return 0;
}

//
// GetSensor(sensor_id)
// Return the cached state for the requested sensor
//
API_DECL(GetSensor) {
	int sid = luaL_checknumber(L, 1);

	if(sid < 1 || sid > 24) {
		luaL_error(L, "out of bounds switch id");
	} else if(sid < 9) {
		lua_pushboolean(L, get_hub_state(RHUB_SENSORS1) & (1 << (sid - 1)));
	} else if(sid < 17) {
		lua_pushboolean(L, get_hub_state(RHUB_SENSORS2) & (1 << (sid - 9)));
	} else {
		lua_pushboolean(L, get_hub_state(RHUB_SENSORS3) & (1 << (sid - 17)));
	}

	return 1;
}

//
// Library object
//
luaL_Reg raild_api[] = {
	API_LINK(exit),
	API_LINK(HubReady),
	API_LINK(CreateTimer),
	API_LINK(CancelTimer),
	API_LINK(GetSwitch),
	API_LINK(SetSwitch),
	API_LINK(GetSensor),
	{ NULL, NULL }
};

//
// Register the raild lua library
//
void lualib_register() {
	luaL_register(L, NULL, raild_api);
}

