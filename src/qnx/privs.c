/* Running without root. knotwatch needs two things an ordinary user is not
 * allowed: to query other processes (thread states, connections) and to read
 * a mutex out of another process's memory. QNX grants those as separate
 * process-manager abilities, so the process keeps exactly those two and then
 * gives up root for good. */
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <sys/procmgr.h>
#include <unistd.h>

#include "kw_qnx.h"

int kw_drop_privileges(unsigned uid, unsigned gid)
{
    int err = procmgr_ability(
        0,
        PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_XPROCESS_QUERY,
        PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_XPROCESS_MEM_READ,
        PROCMGR_AID_EOL);
    if (err != EOK) {
        errno = err;
        return -1;
    }
    /* Group first: after setuid() we would no longer be allowed to. */
    if (setgroups(0, NULL) == -1 || setgid((gid_t)gid) == -1 ||
        setuid((uid_t)uid) == -1) {
        return -1;
    }
    if (uid != 0 && setuid(0) != -1) { /* root must not be recoverable */
        errno = EPERM;
        return -1;
    }
    fprintf(stderr, "knotwatch: running as uid %u, gid %u\n", (unsigned)geteuid(),
            (unsigned)getegid());
    return 0;
}
