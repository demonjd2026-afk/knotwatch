/* Message protocol shared by the two demo processes.
 *
 * The demo is a deliberately flawed two-process design:
 *
 *   brake_ctl    --READ_SPEED-->  wheel_sensor     (the intended hierarchy)
 *   wheel_sensor --FAULT------->  brake_ctl        (the flaw: a send back up)
 *
 * The second arrow is only taken on a rare sensor fault. When it is, both
 * single-threaded processes wait on each other forever. */
#ifndef KW_DEMO_PROTO_H
#define KW_DEMO_PROTO_H

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#define DEMO_SENSOR_NAME "kw_demo_wheel_sensor"
#define DEMO_BRAKE_NAME "kw_demo_brake_ctl"

/* Application message types must lie above the range the OS reserves. */
#define DEMO_MSG_READ_SPEED (_IO_MAX + 1)
#define DEMO_MSG_FAULT (_IO_MAX + 2)

#define DEMO_PULSE_TICK _PULSE_CODE_MINAVAIL

/* Second scenario: a lock held across a send.
 *
 *   motor_ctl thread 1  locks its state, then --LOG--> logger
 *   logger              on a rare fault --CONFIG--> motor_ctl
 *   motor_ctl thread 2  receives CONFIG and needs the same lock
 *
 * Thread 1 waits for the logger, the logger waits for thread 2, and thread 2
 * waits for the lock thread 1 holds. */
#define DEMO_LOGGER_NAME "kw_demo_logger"
#define DEMO_MOTOR_NAME "kw_demo_motor_ctl"
#define DEMO_MSG_LOG (_IO_MAX + 3)
#define DEMO_MSG_CONFIG (_IO_MAX + 4)

typedef struct {
    uint16_t type;
    uint16_t reserved;
    int32_t value;
} demo_msg;

/* Receive buffer large enough for our messages, pulses and the connect
 * message that name_open() sends. */
typedef union {
    uint16_t type;
    struct _pulse pulse;
    demo_msg msg;
    char pad[256];
} demo_rx;

#endif
