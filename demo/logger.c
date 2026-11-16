/* Demo server for the second scenario: records log messages.
 *
 *   logger [--fault-after N]
 *
 * With --fault-after N, while handling the Nth log message the logger asks
 * motor_ctl for its configuration before replying. The thread that sent the
 * log message is waiting for that reply while holding motor_ctl's state lock,
 * and the thread that answers configuration requests needs the same lock. */
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
        fputs("usage: logger [--fault-after N]\n", stderr);
        return 64;
    }

    name_attach_t *attach = name_attach(NULL, DEMO_LOGGER_NAME, 0);
    if (attach == NULL) {
        perror("logger: name_attach");
        return 1;
    }

    int motor = -1; /* opened on first use */
    long messages = 0;
    for (;;) {
        demo_rx rx;
        rcvid_t rcvid = MsgReceive(attach->chid, &rx, sizeof(rx), NULL);
        if (rcvid == -1) {
            perror("logger: MsgReceive");
            return 1;
        }
        if (rcvid == 0) { /* pulse */
            if (rx.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(rx.pulse.scoid);
            }
            continue;
        }
        if (rx.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (rx.type != DEMO_MSG_LOG) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        messages++;
        if (motor == -1) {
            motor = name_open(DEMO_MOTOR_NAME, 0);
        }
        if (fault_after > 0 && messages == fault_after && motor != -1) {
            demo_msg query = {.type = DEMO_MSG_CONFIG};
            demo_msg config;
            fprintf(stderr, "logger: fault on message %ld, asking motor_ctl for its "
                            "configuration\n",
                    messages);
            MsgSend(motor, &query, sizeof(query), &config, sizeof(config));
        }
        MsgReply(rcvid, EOK, NULL, 0);
    }
}
