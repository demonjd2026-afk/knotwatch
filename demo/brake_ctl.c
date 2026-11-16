/* Demo client: a single-threaded control loop.
 *
 *   brake_ctl
 *
 * One thread waits on one channel for two things: a 100 ms timer pulse, on
 * which it asks wheel_sensor for the wheel speed, and fault reports from the
 * sensor. While it waits for the sensor's reply it cannot receive anything,
 * which is what makes the sensor's fault report deadlock (see demo_proto.h). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

#include "demo_proto.h"

int main(void)
{
    name_attach_t *attach = name_attach(NULL, DEMO_BRAKE_NAME, 0);
    if (attach == NULL) {
        perror("brake_ctl: name_attach");
        return 1;
    }

    int sensor = -1;
    for (int tries = 0; sensor == -1 && tries < 100; tries++) {
        sensor = name_open(DEMO_SENSOR_NAME, 0);
        if (sensor == -1) {
            usleep(100000); /* wheel_sensor may still be starting */
        }
    }
    if (sensor == -1) {
        fputs("brake_ctl: wheel_sensor is not running\n", stderr);
        return 1;
    }

    /* Periodic tick: a timer pulse delivered to our own channel. */
    int self = ConnectAttach(0, 0, attach->chid, _NTO_SIDE_CHANNEL, 0);
    if (self == -1) {
        perror("brake_ctl: ConnectAttach");
        return 1;
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self, SIGEV_PULSE_PRIO_INHERIT, DEMO_PULSE_TICK, 0);
    timer_t timer;
    struct itimerspec period = {.it_value = {0, 100000000L},
                                .it_interval = {0, 100000000L}};
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1 ||
        timer_settime(timer, 0, &period, NULL) == -1) {
        perror("brake_ctl: timer");
        return 1;
    }

    for (;;) {
        demo_rx rx;
        rcvid_t rcvid = MsgReceive(attach->chid, &rx, sizeof(rx), NULL);
        if (rcvid == -1) {
            perror("brake_ctl: MsgReceive");
            return 1;
        }
        if (rcvid == 0) { /* pulse */
            if (rx.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(rx.pulse.scoid);
            } else if (rx.pulse.code == DEMO_PULSE_TICK) {
                demo_msg req = {.type = DEMO_MSG_READ_SPEED};
                demo_msg speed;
                /* Blocks until the sensor replies; nothing else is served. */
                if (MsgSend(sensor, &req, sizeof(req), &speed, sizeof(speed)) == -1) {
                    perror("brake_ctl: MsgSend");
                    return 1;
                }
            }
            continue;
        }
        if (rx.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);
        } else if (rx.type == DEMO_MSG_FAULT) {
            fputs("brake_ctl: sensor fault reported\n", stderr);
            MsgReply(rcvid, EOK, NULL, 0);
        } else {
            MsgError(rcvid, ENOSYS);
        }
    }
}
