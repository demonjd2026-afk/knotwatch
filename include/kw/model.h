/* The snapshot: one observation of every process, thread and connection.
 *
 * This is the seam of the whole tool. The QNX collector produces a snapshot
 * from the process manager; everything after that (analysis, rendering) is
 * portable C that never touches a QNX header, so it can be tested and replayed
 * on any host from a captured snapshot file. */
#ifndef KW_MODEL_H
#define KW_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KW_NAME_MAX 64
#define KW_STATE_MAX 16

typedef enum {
    KW_WAIT_NONE = 0, /* running, ready, or blocked on something we don't follow */
    KW_WAIT_SEND,     /* sent a message nobody has received yet */
    KW_WAIT_REPLY,    /* message received, waiting for the reply */
    KW_WAIT_RECEIVE,  /* server thread waiting for a message */
    KW_WAIT_JOIN,     /* waiting for another thread in the same process */
    KW_WAIT_MUTEX     /* waiting for a mutex another thread holds */
} kw_wait_kind;

typedef struct {
    int32_t pid;
    int32_t tid;
    char state[KW_STATE_MAX]; /* kernel state name, e.g. "REPLY" */
    kw_wait_kind wait;
    int32_t target_pid;  /* SEND/REPLY: server process; MUTEX: owner's process */
    int32_t target_chid; /* SEND/REPLY: server channel; RECEIVE: own channel */
    int32_t target_tid;  /* JOIN: thread being joined; MUTEX: owning thread,
                            0 when the owner could not be determined */
    uint8_t prio;        /* effective priority (after inheritance) */
    uint8_t real_prio;   /* base priority */
    bool timeout_armed;  /* the kernel will unblock this thread by itself */
    uint64_t blocked_ns; /* time spent in the current blocked state */
} kw_thread;

typedef struct {
    int32_t pid;
    char name[KW_NAME_MAX];
    size_t first_thread; /* valid after kw_snapshot_index() */
    size_t n_threads;
} kw_proc;

typedef struct {
    int32_t pid;  /* client process */
    int32_t coid; /* its connection id */
    int32_t srv_pid;
    int32_t srv_chid;
} kw_conn;

typedef struct {
    uint64_t time_ns; /* monotonic time the scan started */
    uint64_t scan_ns; /* how long the scan took: bounds sampling skew */
    kw_proc *procs;
    size_t n_procs, cap_procs;
    kw_thread *threads;
    size_t n_threads, cap_threads;
    kw_conn *conns;
    size_t n_conns, cap_conns;
} kw_snapshot;

void kw_snapshot_init(kw_snapshot *s);
void kw_snapshot_free(kw_snapshot *s);
void kw_snapshot_add_proc(kw_snapshot *s, int32_t pid, const char *name);
void kw_snapshot_add_thread(kw_snapshot *s, const kw_thread *t);
void kw_snapshot_add_conn(kw_snapshot *s, const kw_conn *c);

/* Sort records and link threads to their processes. Call once after the last
 * add and before any lookup or analysis. Threads whose process record is
 * missing get a placeholder process named "pid<N>". */
void kw_snapshot_index(kw_snapshot *s);

/* Lookups; valid after kw_snapshot_index(). Return NULL / -1 when absent. */
const kw_proc *kw_snapshot_proc(const kw_snapshot *s, int32_t pid);
long kw_snapshot_thread_index(const kw_snapshot *s, int32_t pid, int32_t tid);

/* Map a kernel state name to the wait kind the analysis follows. */
kw_wait_kind kw_wait_from_state(const char *state);

/* Copy `src` into a fixed-size name field, replacing whitespace so the value
 * stays a single token in snapshot files. */
void kw_copy_name(char *dst, size_t dst_size, const char *src);

#endif
