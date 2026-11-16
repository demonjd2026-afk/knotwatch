/* Snapshot collector for QNX OS 8.0.
 *
 * Sources, all documented process-manager interfaces:
 *   /proc/<pid>/ctl + DCMD_PROC_TIDSTATUS   per-thread state and what it is
 *                                           blocked on (debug_thread_t)
 *   /proc/<pid>/exefile                     executable path, for the name
 *   ConnectServerInfo()                     every connection a process holds
 *   /proc/<pid>/as                          read-only, and only when a thread
 *                                           is blocked on a mutex: the mutex
 *                                           itself is read to see who holds it
 *
 * ctl is opened read-only: the process manager allows only one writer per
 * process, and a read-only descriptor never competes with a debugger. */
#include <ctype.h>
#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/states.h>
#include <time.h>
#include <unistd.h>

#include "kw/util.h"
#include "kw_qnx.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Names for the kernel thread states of QNX OS 8.0 (<sys/states.h>). */
static void state_name(unsigned state, char *out, size_t out_size)
{
    static const struct {
        unsigned state;
        const char *name;
    } names[] = {
        {STATE_DEAD, "DEAD"},
        {STATE_RUNNING, "RUNNING"},
        {STATE_READY, "READY"},
        {STATE_STOPPED, "STOPPED"},
        {STATE_SEND, "SEND"},
        {STATE_RECEIVE, "RECEIVE"},
        {STATE_REPLY, "REPLY"},
        {STATE_MQ_SEND, "MQ_SEND"},
        {STATE_MQ_RECEIVE, "MQ_RECEIVE"},
        {STATE_WAITPAGE, "WAITPAGE"},
        {STATE_SIGSUSPEND, "SIGSUSPEND"},
        {STATE_SIGWAITINFO, "SIGWAITINFO"},
        {STATE_NANOSLEEP, "NANOSLEEP"},
        {STATE_MUTEX, "MUTEX"},
        {STATE_CONDVAR, "CONDVAR"},
        {STATE_JOIN, "JOIN"},
        {STATE_INTR, "INTR"},
        {STATE_SEM, "SEM"},
        {STATE_WAITCTX, "WAITCTX"},
        {STATE_RWLOCK_READ, "RWLOCK_READ"},
        {STATE_RWLOCK_WRITE, "RWLOCK_WRITE"},
        {STATE_BARRIER, "BARRIER"},
        {STATE_PIPE, "PIPE"},
        {STATE_CREATE, "CREATE"},
        {STATE_DESTROY, "DESTROY"},
        {STATE_MUON_MUTEX, "MUON_MUTEX"},
        {STATE_TRACEBUFFER, "TRACEBUFFER"},
        {STATE_INTR_ATTACH_EV, "INTR_ATTACH_EV"},
        {STATE_TIMER_DELEGATE, "TIMER_DELEGATE"},
        {STATE_SEND_NOTIFY, "SEND_NOTIFY"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (names[i].state == state) {
            snprintf(out, out_size, "%s", names[i].name);
            return;
        }
    }
    snprintf(out, out_size, "STATE%u", state);
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

/* Facts about a thread that are only needed while collecting. One entry per
 * snapshot thread, in the same order. */
typedef struct {
    uint64_t mutex; /* address of the mutex it is blocked on, or 0 */
} thread_aux;

typedef struct {
    thread_aux *items;
    size_t n, cap;
} aux_list;

static void collect_threads(int ctl, pid_t pid, kw_snapshot *s, aux_list *aux)
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
        case KW_WAIT_MUTEX: /* owner filled in by resolve_lock_owners() */
        case KW_WAIT_NONE:
            break;
        }
        kw_snapshot_add_thread(s, &t);

        void *items = aux->items;
        kw_grow(&items, &aux->cap, aux->n + 1, sizeof(thread_aux));
        aux->items = items;
        thread_aux *a = &aux->items[aux->n++];
        memset(a, 0, sizeof(*a));
        a->mutex = t.wait == KW_WAIT_MUTEX ? st.blocked.sync.sync : 0;
    }
}

/* Set KNOTWATCH_DEBUG_LOCKS=1 to trace how mutex owners are looked up. */
static bool debug_locks(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("KNOTWATCH_DEBUG_LOCKS") != NULL;
    }
    return enabled == 1;
}

/* Read `len` bytes at virtual address `addr` of another process. */
static int read_memory(pid_t pid, uint64_t addr, void *buf, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return -1;
    }
    ssize_t n = -1;
    if (lseek(fd, (off_t)addr, SEEK_SET) != (off_t)-1) {
        n = read(fd, buf, len);
    }
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

/* The value a thread leaves in a mutex it has locked. On QNX OS 8.0 this is
 * the low bits of the process id above the thread id: a mutex held by thread
 * 1 of process 0x65017 reads 0x50170001 once the "waiters" bit is masked off,
 * which is also how pidin names the owner. Established by observation on
 * QNX OS 8.0.0 (x86_64), not from a documented interface. */
static uint32_t lock_owner_id(const kw_thread *t)
{
    return (((uint32_t)t->pid << 16) | (uint32_t)t->tid) & _NTO_SYNC_OWNER_MASK;
}

/* For every thread blocked on a mutex, find the thread that holds it, by
 * reading the mutex from the waiter's memory. The stored id only has room for
 * part of the process id, so a thread of the waiter's own process is
 * preferred; a thread elsewhere (a process-shared mutex) is accepted only if
 * it is the single match. Anything less certain leaves the owner unknown, and
 * the analysis then treats the waiter as not stuck. */
static void resolve_lock_owners(kw_snapshot *s, aux_list *aux, kw_collect_stats *stats)
{
    for (size_t i = 0; i < s->n_threads; i++) {
        kw_thread *t = &s->threads[i];
        if (aux->items[i].mutex == 0) {
            continue;
        }
        sync_t lock;
        uint32_t owner = 0;
        memset(&lock, 0, sizeof(lock));
        int rc = read_memory(t->pid, aux->items[i].mutex, &lock, sizeof(lock));
        if (rc == 0) {
            switch (lock.__owner) {
            case _NTO_SYNC_INITIALIZER:
            case _NTO_SYNC_DESTROYED:
            case _NTO_SYNC_NAMED_SEM:
            case _NTO_SYNC_SEM:
            case _NTO_SYNC_COND:
            case _NTO_SYNC_SPIN:
            case _NTO_SYNC_JOB:
            case _NTO_SYNC_OWNERDEAD:
                break; /* not a held mutex */
            default:
                owner = lock.__owner & _NTO_SYNC_OWNER_MASK;
                break;
            }
        }

        long found = -1;
        size_t elsewhere = 0;
        for (size_t j = 0; owner != 0 && j < s->n_threads; j++) {
            const kw_thread *c = &s->threads[j];
            if (j == i || c->tid > 0xffff || lock_owner_id(c) != owner) {
                continue;
            }
            if (c->pid == t->pid) {
                found = (long)j;
                elsewhere = 0;
                break;
            }
            found = (long)j;
            elsewhere++;
        }
        if (elsewhere > 1) {
            found = -1; /* ambiguous */
        }
        if (debug_locks()) {
            fprintf(stderr,
                    "knotwatch: lock-debug: waiter %d/%d mutex=0x%llx read=%d "
                    "raw owner=0x%x -> %s",
                    (int)t->pid, (int)t->tid,
                    (unsigned long long)aux->items[i].mutex, rc, lock.__owner,
                    found < 0 ? "unknown\n" : "");
            if (found >= 0) {
                fprintf(stderr, "%d/%d\n", (int)s->threads[found].pid,
                        (int)s->threads[found].tid);
            }
        }
        if (found < 0) {
            stats->locks_unknown++;
            continue;
        }
        t->target_pid = s->threads[found].pid;
        t->target_tid = s->threads[found].tid;
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
    aux_list aux = {NULL, 0, 0};

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
        collect_threads(ctl, pid, s, &aux);
        close(ctl);
        if (collect_connections(pid, s) != 0) {
            stats->conns_skipped++;
        }
    }
    closedir(dir);
    resolve_lock_owners(s, &aux, stats); /* before indexing reorders threads */
    free(aux.items);

    s->scan_ns = now_ns() - s->time_ns;
    kw_snapshot_index(s);
    return 0;
}
