/* Snapshot collector for QNX OS 8.0.
 *
 * Sources, all documented process-manager interfaces:
 *   /proc/<pid>/ctl + DCMD_PROC_TIDSTATUS   per-thread state and what it is
 *                                           blocked on (debug_thread_t)
 *   /proc/<pid>/exefile                     executable path, for the name
 *   ConnectServerInfo()                     every connection a process holds
 *
 * ctl is opened read-only: the process manager allows only one writer per
 * process, and a read-only descriptor never competes with a debugger. */
#include <ctype.h>
#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/states.h>
#include <time.h>
#include <unistd.h>

#include "kw_qnx.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void state_name(unsigned state, char *out, size_t out_size)
{
    const char *name = NULL;
    switch (state) {
    case STATE_DEAD:
        name = "DEAD";
        break;
    case STATE_RUNNING:
        name = "RUNNING";
        break;
    case STATE_READY:
        name = "READY";
        break;
    case STATE_STOPPED:
        name = "STOPPED";
        break;
    case STATE_SEND:
        name = "SEND";
        break;
    case STATE_RECEIVE:
        name = "RECEIVE";
        break;
    case STATE_REPLY:
        name = "REPLY";
        break;
    case STATE_SIGWAITINFO:
        name = "SIGWAITINFO";
        break;
    case STATE_NANOSLEEP:
        name = "NANOSLEEP";
        break;
    case STATE_MUTEX:
        name = "MUTEX";
        break;
    case STATE_CONDVAR:
        name = "CONDVAR";
        break;
    case STATE_JOIN:
        name = "JOIN";
        break;
    case STATE_SEM:
        name = "SEM";
        break;
    default:
        break;
    }
    if (name != NULL) {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "STATE%u", state);
    }
}

/* Process name = basename of /proc/<pid>/exefile, or "pid<N>" if unreadable. */
static void process_name(pid_t pid, char *out, size_t out_size)
{
    char path[64];
    char exe[PATH_MAX];
    snprintf(out, out_size, "pid%d", (int)pid);
    snprintf(path, sizeof(path), "/proc/%d/exefile", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return;
    }
    ssize_t n = read(fd, exe, sizeof(exe) - 1);
    close(fd);
    if (n <= 0) {
        return;
    }
    exe[n] = '\0';
    exe[strcspn(exe, "\r\n")] = '\0';
    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    if (*base != '\0') {
        snprintf(out, out_size, "%.*s", (int)(out_size - 1), base);
    }
}

static void collect_threads(int ctl, pid_t pid, kw_snapshot *s)
{
    procfs_status st;
    memset(&st, 0, sizeof(st));
    /* TIDSTATUS returns the first thread whose id is >= st.tid and stores
     * that id back, so stepping by one visits every thread. */
    for (st.tid = 1; devctl(ctl, DCMD_PROC_TIDSTATUS, &st, sizeof(st), NULL) == EOK;
         st.tid++) {
        kw_thread t;
        memset(&t, 0, sizeof(t));
        t.pid = (int32_t)pid;
        t.tid = (int32_t)st.tid;
        state_name(st.state, t.state, sizeof(t.state));
        t.wait = kw_wait_from_state(t.state);
        t.prio = st.priority;
        t.real_prio = st.real_priority;
        t.timeout_armed = st.timeout != 0;
        t.blocked_ns = st.nsec_since_block;
        switch (t.wait) {
        case KW_WAIT_SEND:
        case KW_WAIT_REPLY:
            t.target_pid = (int32_t)st.blocked.connect.pid;
            t.target_chid = st.blocked.connect.chid;
            break;
        case KW_WAIT_RECEIVE:
            t.target_chid = st.blocked.channel.chid;
            break;
        case KW_WAIT_JOIN:
            t.target_tid = (int32_t)st.blocked.join.tid;
            break;
        case KW_WAIT_NONE:
            break;
        }
        kw_snapshot_add_thread(s, &t);
    }
}

/* Returns 0, or -1 if the connection list could not be read at all. */
static int collect_connections(pid_t pid, kw_snapshot *s)
{
    int coid = 0;
    for (;;) {
        struct _server_info info;
        memset(&info, 0, sizeof(info));
        /* Returns the first connection id >= coid, or -1 when there is none
         * (EINVAL) or the process has exited (ESRCH). */
        int found = ConnectServerInfo(pid, coid, &info);
        if (found == -1) {
            return (coid == 0 && errno != EINVAL && errno != ESRCH) ? -1 : 0;
        }
        if ((info.flags & _NTO_COF_DEAD) == 0) {
            kw_conn c = {.pid = (int32_t)pid,
                         .coid = found,
                         .srv_pid = (int32_t)info.pid,
                         .srv_chid = info.chid};
            kw_snapshot_add_conn(s, &c);
        }
        if (found == INT_MAX) {
            return 0;
        }
        coid = found + 1;
    }
}

static int is_pid_name(const char *name)
{
    if (*name == '\0') {
        return 0;
    }
    for (; *name; name++) {
        if (!isdigit((unsigned char)*name)) {
            return 0;
        }
    }
    return 1;
}

int kw_collect(kw_snapshot *s, kw_collect_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        return -1;
    }
    s->time_ns = now_ns();

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_pid_name(ent->d_name)) {
            continue;
        }
        pid_t pid = (pid_t)strtol(ent->d_name, NULL, 10);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/ctl", (int)pid);
        int ctl = open(path, O_RDONLY);
        if (ctl == -1) {
            if (errno != ENOENT && errno != ESRCH) {
                stats->procs_skipped++; /* exists but we may not look at it */
            }
            continue;
        }
        char name[KW_NAME_MAX];
        process_name(pid, name, sizeof(name));
        kw_snapshot_add_proc(s, (int32_t)pid, name);
        collect_threads(ctl, pid, s);
        close(ctl);
        if (collect_connections(pid, s) != 0) {
            stats->conns_skipped++;
        }
    }
    closedir(dir);

    s->scan_ns = now_ns() - s->time_ns;
    kw_snapshot_index(s);
    return 0;
}
