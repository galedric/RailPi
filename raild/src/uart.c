#include "raild.h"
#include <fcntl.h>
#include <termios.h>
#include <hub_opcodes.h>

/**
 * Handle UART communication between Raild and RailHub
 */

#ifndef UART_DEBUG
#define UART_DEBUG  0
#endif

#if UART_DEBUG
#define TRACE(msg) logger("UART", "trace: " msg);
#else
#define TRACE(msg)
#endif

static int   uart0_filestream = -1;
static rbyte buffer[256];

static bool keep_alive_missing = false;

typedef enum {
    UART_PROCESS_DISPATCH,
    UART_PROCESS_SENSORS1,
    UART_PROCESS_SENSORS2,
    UART_PROCESS_SENSORS3,
    UART_PROCESS_SWITCHES
} uart_process_state;

static uart_process_state state = UART_PROCESS_DISPATCH;

static void uart_put(unsigned char data) {
    static unsigned char buffer[1];
    buffer[0] = data;
    write(uart0_filestream, buffer, 1);
}

void uart_reset() {
    state = UART_PROCESS_DISPATCH;
    uart_put(RESET);
}

void setup_uart() {
    logger("UART", "Init UART channel");

    //OPEN THE UART
    //The flags (defined in fcntl.h):
    //    Access modes (use 1 of these):
    //        O_RDONLY - Open for reading only.
    //        O_RDWR - Open for reading and writing.
    //        O_WRONLY - Open for writing only.
    //
    //    O_NDELAY / O_NONBLOCK (same function) - Enables nonblocking mode. When set read requests on the file can return immediately with a failure status
    //                                            if there is no input immediately available (instead of blocking). Likewise, write requests can also return
    //                                            immediately with a failure status if the output can't be written immediately.
    //
    //    O_NOCTTY - When set and path identifies a terminal device, open() shall not cause the terminal device to become the controlling terminal for the process.
    uart0_filestream = open("/dev/ttyAMA0", O_RDWR | O_NOCTTY | O_NONBLOCK);        //Open in non blocking read/write mode
    if(uart0_filestream == -1) {
        //ERROR - CAN'T OPEN SERIAL PORT
        logger_error("Unable to open UART.  Ensure it is not in use by another application");
        exit(1);
    }

    //CONFIGURE THE UART
    //The flags (defined in /usr/include/termios.h - see http://pubs.opengroup.org/onlinepubs/007908799/xsh/termios.h.html):
    //    Baud rate:- B1200, B2400, B4800, B9600, B19200, B38400, B57600, B115200, B230400, B460800, B500000, B576000, B921600, B1000000, B1152000, B1500000, B2000000, B2500000, B3000000, B3500000, B4000000
    //    CSIZE:- CS5, CS6, CS7, CS8
    //    CLOCAL - Ignore modem status lines
    //    CREAD - Enable receiver
    //    IGNPAR = Ignore characters with parity errors
    //    ICRNL - Map CR to NL on input (Use for ASCII comms where you want to auto correct end of line characters - don't use for bianry comms!)
    //    PARENB - Parity enable
    //    PARODD - Odd parity (else even)
    struct termios options;
    tcgetattr(uart0_filestream, &options);
    options.c_cflag = B115200 | CS8 | CLOCAL | CREAD;        //<Set baud rate
    options.c_iflag = IGNPAR;
    options.c_oflag = 0;
    options.c_lflag = 0;
    tcflush(uart0_filestream, TCIFLUSH);
    tcsetattr(uart0_filestream, TCSANOW, &options);

    raild_epoll_add(uart0_filestream, RAILD_EV_UART);
    raild_timer_create(500, 500, RAILD_EV_UART_TIMER);

    uart_reset();
}

static void uart_process(rbyte *buffer, int len) {
    for(int i = 0; i < len; i++) {
        rbyte c = buffer[i];
        switch(state) {
            case UART_PROCESS_DISPATCH:
                switch(c) {
                    case HELLO:
                        TRACE("HELLO");
                        set_hub_readiness(false);
                        uart_put(SET_SWITCHES);
                        uart_put(get_hub_state(RHUB_SWITCHES));
                    break;

                    case READY:
                        TRACE("READY");
                        set_hub_readiness(true);
                    break;

                    case SENSORS_1: TRACE("SENSORS_1"); state = UART_PROCESS_SENSORS1; break;
                    case SENSORS_2: TRACE("SENSORS_2"); state = UART_PROCESS_SENSORS2; break;
                    case SENSORS_3: TRACE("SENSORS_3"); state = UART_PROCESS_SENSORS3; break;
                    case SWITCHES:  TRACE("SENSORS_4"); state = UART_PROCESS_SWITCHES; break;

                    case KEEP_ALIVE:
                        TRACE("KEEP_ALIVE");
                        keep_alive_missing = false;
                        uart_put(KEEP_ALIVE);
                    break;

                    default:
                        printf("Unknown opcode from RailHub: 0x%02x\n", (unsigned char) c);
                }
                break;

            case UART_PROCESS_SENSORS1:
                set_hub_state(RHUB_SENSORS1, c);
                state = UART_PROCESS_DISPATCH;
                break;

            case UART_PROCESS_SENSORS2:
                set_hub_state(RHUB_SENSORS2, c);
                state = UART_PROCESS_DISPATCH;
                break;

            case UART_PROCESS_SENSORS3:
                set_hub_state(RHUB_SENSORS3, c);
                state = UART_PROCESS_DISPATCH;
                break;

            case UART_PROCESS_SWITCHES:
                set_hub_state(RHUB_SWITCHES, c);
                state = UART_PROCESS_DISPATCH;
                break;

            default:
                logger("UART", "Input processor is in an unknown state. Aborting.");
                exit(1);
        }
    }
}

void uart_handle_timer(raild_event *event) {
    if(get_hub_readiness()) {
        if(!keep_alive_missing) {
            keep_alive_missing = true;
        } else {
            logger("UART", "RailHub gone!");
            set_hub_readiness(false);
        }
    } else {
        uart_reset();
    }
}

void uart_handle_event(raild_event *event) {
    int len = read(uart0_filestream, (void *) buffer, 256);

    if(len == EAGAIN || len == 0) {
        return;
    } else if(len < 0) {
        perror("(uart) read");
        exit(1);
    } else {
        uart_process(buffer, len);
    }
}

void uart_setswitch_on(rbyte sid) {
    uart_put(SET_SWITCH_ON);
    uart_put(sid);
}

void uart_setswitch_off(rbyte sid) {
    uart_put(SET_SWITCH_OFF);
    uart_put(sid);
}

void uart_setpower(bool state) {
    if(state) {
        uart_put(POWER_ON);
    } else {
        uart_put(POWER_OFF);
    }
}
