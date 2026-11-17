/* Demo server: answers wheel-speed requests.
 *
 *   wheel_sensor [--fault-after N] [--send-timeout-ms M]
 *
 * With --fault-after N, the Nth request triggers the design flaw: instead of
 * just replying, the sensor first reports a fault by sending a message to
 * brake_ctl. brake_ctl's only thread is at that moment waiting for our reply,
 * so nobody receives the fault report and both processes block forever.
 *
 * With --send-timeout-ms M the fault report is sent under a kernel timeout.
 * The design flaw is still there, but the send fails after M ms instead of
 * hanging, and the sensor carries on. knotwatch must not call that a
 * deadlock while the timeout is running. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <time.h>

#include "demo_proto.h"

int main(int argc, char **argv)
{
    long fault_after = 0, timeout_ms = 0;
    int bad = (argc % 2) == 0;
    for (int i = 1; !bad && i + 1 < argc; i += 2) {
        long v = strtol(argv[i + 1], NULL, 10);
        if (strcmp(argv[i], "--fault-after") == 0 && v > 0) {
            fault_after = v;
        } else if (strcmp(argv[i], "--send-timeout-ms") == 0 && v > 0 && v <= 3600000) {
            timeout_ms = v;
        } else {
            bad = 1;
        }
    }
    if (bad) {
        fputs("usage: wheel_sensor [--fault-after N] [--send-timeout-ms M]\n", stderr);
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
            if (timeout_ms > 0) {
                uint64_t ns = (uint64_t)timeout_ms * 1000000ULL;
                TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL,
                             &ns, NULL);
            }
            /* Without a timeout this never returns. */
            if (MsgSend(brake, &fault, sizeof(fault), NULL, 0) == -1) {
                fprintf(stderr, "wheel_sensor: fault report failed (%s), carrying on\n",
                        strerror(errno));
            }
        }

        demo_msg reply = {.type = DEMO_MSG_READ_SPEED, .value = 88};
        MsgReply(rcvid, EOK, &reply, sizeof(reply));
    }
}
