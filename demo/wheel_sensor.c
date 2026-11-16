/* Demo server: answers wheel-speed requests.
 *
 *   wheel_sensor [--fault-after N]
 *
 * With --fault-after N, the Nth request triggers the design flaw: instead of
 * just replying, the sensor first reports a fault by sending a message to
 * brake_ctl. brake_ctl's only thread is at that moment waiting for our reply,
 * so nobody receives the fault report and both processes block forever. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "demo_proto.h"

int main(int argc, char **argv)
{
    long fault_after = 0;
    if (argc == 3 && strcmp(argv[1], "--fault-after") == 0) {
        fault_after = strtol(argv[2], NULL, 10);
    }
    if (argc != 1 && fault_after <= 0) {
        fputs("usage: wheel_sensor [--fault-after N]\n", stderr);
        return 64;
    }

    name_attach_t *attach = name_attach(NULL, DEMO_SENSOR_NAME, 0);
    if (attach == NULL) {
        perror("wheel_sensor: name_attach");
        return 1;
    }

    int brake = -1; /* connection used to report faults, opened on first use */
    long requests = 0;
    for (;;) {
        demo_rx rx;
        rcvid_t rcvid = MsgReceive(attach->chid, &rx, sizeof(rx), NULL);
        if (rcvid == -1) {
            perror("wheel_sensor: MsgReceive");
            return 1;
        }
        if (rcvid == 0) { /* pulse */
            if (rx.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(rx.pulse.scoid);
            }
            continue;
        }
        if (rx.type == _IO_CONNECT) { /* a client's name_open() */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (rx.type != DEMO_MSG_READ_SPEED) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        requests++;
        if (brake == -1) {
            /* Holding this connection is already a hierarchy violation that
             * knotwatch reports, long before any message crosses it. */
            brake = name_open(DEMO_BRAKE_NAME, 0);
        }
        if (fault_after > 0 && requests == fault_after && brake != -1) {
            demo_msg fault = {.type = DEMO_MSG_FAULT, .value = 1};
            fprintf(stderr, "wheel_sensor: fault on request %ld, notifying brake_ctl\n",
                    requests);
            MsgSend(brake, &fault, sizeof(fault), NULL, 0); /* never returns */
        }

        demo_msg reply = {.type = DEMO_MSG_READ_SPEED, .value = 88};
        MsgReply(rcvid, EOK, &reply, sizeof(reply));
    }
}
