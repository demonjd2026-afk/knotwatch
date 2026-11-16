/* standoff: two processes that send to each other and so block forever.
 * Used by the codelab in this directory. QNX OS 8.0. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/neutrino.h>
#include <unistd.h>

int main(void)
{
    int to_parent[2];
    if (pipe(to_parent) == -1) {
        perror("pipe");
        return 1;
    }
    int parent_chid = ChannelCreate(0);
    pid_t parent = getpid();
    pid_t child = fork();
    if (parent_chid == -1 || child == -1) {
        perror("standoff");
        return 1;
    }

    if (child == 0) {
        /* The child makes its own channel and tells the parent its id. */
        int chid = ChannelCreate(0);
        if (write(to_parent[1], &chid, sizeof(chid)) != sizeof(chid)) {
            return 1;
        }
        int coid = ConnectAttach(0, parent, parent_chid, _NTO_SIDE_CHANNEL, 0);
        sleep(1);
        MsgSend(coid, "hello", 6, NULL, 0); /* the parent never receives this */
        return 0;
    }

    int child_chid;
    if (read(to_parent[0], &child_chid, sizeof(child_chid)) != sizeof(child_chid)) {
        return 1;
    }
    int coid = ConnectAttach(0, child, child_chid, _NTO_SIDE_CHANNEL, 0);
    printf("standoff: %d and %d are about to send to each other\n", (int)parent,
           (int)child);
    fflush(stdout);
    MsgSend(coid, "hello", 6, NULL, 0); /* the child never receives this */
    return 0;
}
