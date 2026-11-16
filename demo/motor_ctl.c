/* Demo client for the second scenario: two threads and one lock.
 *
 *   motor_ctl
 *
 * Thread 1 is the control loop. Every 100 ms it locks the motor state,
 * updates it and logs it, and it keeps the lock while it waits for the
 * logger's reply. Thread 2 answers configuration requests and takes the same
 * lock to read the state. See demo_proto.h for how that deadlocks. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <unistd.h>

#include "demo_proto.h"

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int motor_speed;

/* Thread 2: serve configuration requests on the attached channel. */
static void *config_server(void *arg)
{
    name_attach_t *attach = arg;
    for (;;) {
        demo_rx rx;
        rcvid_t rcvid = MsgReceive(attach->chid, &rx, sizeof(rx), NULL);
        if (rcvid == -1) {
            perror("motor_ctl: MsgReceive");
            return NULL;
        }
        if (rcvid == 0) { /* pulse */
            if (rx.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(rx.pulse.scoid);
            }
            continue;
        }
        if (rx.type == _IO_CONNECT) {
            MsgReply(rcvid, EOK, NULL, 0);
        } else if (rx.type == DEMO_MSG_CONFIG) {
            demo_msg config = {.type = DEMO_MSG_CONFIG};
            pthread_mutex_lock(&state_lock); /* blocks while thread 1 logs */
            config.value = motor_speed;
            pthread_mutex_unlock(&state_lock);
            MsgReply(rcvid, EOK, &config, sizeof(config));
        } else {
            MsgError(rcvid, ENOSYS);
        }
    }
}

int main(void)
{
    name_attach_t *attach = name_attach(NULL, DEMO_MOTOR_NAME, 0);
    if (attach == NULL) {
        perror("motor_ctl: name_attach");
        return 1;
    }
    pthread_t server;
    int err = pthread_create(&server, NULL, config_server, attach);
    if (err != EOK) {
        fprintf(stderr, "motor_ctl: pthread_create: %s\n", strerror(err));
        return 1;
    }

    int logger = -1;
    for (int tries = 0; logger == -1 && tries < 100; tries++) {
        logger = name_open(DEMO_LOGGER_NAME, 0);
        if (logger == -1) {
            usleep(100000); /* the logger may still be starting */
        }
    }
    if (logger == -1) {
        fputs("motor_ctl: logger is not running\n", stderr);
        return 1;
    }

    /* Thread 1: the control loop. */
    for (;;) {
        demo_msg log = {.type = DEMO_MSG_LOG};
        pthread_mutex_lock(&state_lock);
        motor_speed++;
        log.value = motor_speed;
        /* The flaw: the lock is still held while waiting for the reply. */
        if (MsgSend(logger, &log, sizeof(log), NULL, 0) == -1) {
            perror("motor_ctl: MsgSend");
            return 1;
        }
        pthread_mutex_unlock(&state_lock);
        usleep(100000);
    }
}
