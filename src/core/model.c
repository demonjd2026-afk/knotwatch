#include "kw/model.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kw/util.h"

void kw_snapshot_init(kw_snapshot *s)
{
    memset(s, 0, sizeof(*s));
}

void kw_snapshot_free(kw_snapshot *s)
{
    free(s->procs);
    free(s->threads);
    free(s->conns);
    memset(s, 0, sizeof(*s));
}

void kw_copy_name(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_size; i++) {
        dst[i] = isspace((unsigned char)src[i]) ? '_' : src[i];
    }
    dst[i] = '\0';
    if (i == 0 && dst_size > 1) {
        dst[0] = '?';
        dst[1] = '\0';
    }
}

void kw_snapshot_add_proc(kw_snapshot *s, int32_t pid, const char *name)
{
    void *items = s->procs;
    kw_grow(&items, &s->cap_procs, s->n_procs + 1, sizeof(kw_proc));
    s->procs = items;
    kw_proc *p = &s->procs[s->n_procs++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    kw_copy_name(p->name, sizeof(p->name), name);
}

void kw_snapshot_add_thread(kw_snapshot *s, const kw_thread *t)
{
    void *items = s->threads;
    kw_grow(&items, &s->cap_threads, s->n_threads + 1, sizeof(kw_thread));
    s->threads = items;
    s->threads[s->n_threads++] = *t;
}

void kw_snapshot_add_conn(kw_snapshot *s, const kw_conn *c)
{
    void *items = s->conns;
    kw_grow(&items, &s->cap_conns, s->n_conns + 1, sizeof(kw_conn));
    s->conns = items;
    s->conns[s->n_conns++] = *c;
}

static int cmp_proc(const void *a, const void *b)
{
    const kw_proc *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static int cmp_thread(const void *a, const void *b)
{
    const kw_thread *x = a, *y = b;
    if (x->pid != y->pid) {
        return (x->pid > y->pid) - (x->pid < y->pid);
    }
    return (x->tid > y->tid) - (x->tid < y->tid);
}

static kw_proc *find_proc(const kw_snapshot *s, int32_t pid)
{
    kw_proc key = {.pid = pid};
    if (s->n_procs == 0) {
        return NULL;
    }
    return bsearch(&key, s->procs, s->n_procs, sizeof(kw_proc), cmp_proc);
}

void kw_snapshot_index(kw_snapshot *s)
{
    if (s->n_threads > 1) {
        qsort(s->threads, s->n_threads, sizeof(kw_thread), cmp_thread);
    }
    if (s->n_procs > 1) {
        qsort(s->procs, s->n_procs, sizeof(kw_proc), cmp_proc);
    }

    /* Add placeholders for threads whose process record is missing. Threads
     * are sorted, so each unknown pid is seen as one run. */
    size_t known = s->n_procs;
    for (size_t i = 0; i < s->n_threads; i++) {
        int32_t pid = s->threads[i].pid;
        if (i > 0 && s->threads[i - 1].pid == pid) {
            continue;
        }
        kw_proc key = {.pid = pid};
        if (known == 0 ||
            bsearch(&key, s->procs, known, sizeof(kw_proc), cmp_proc) == NULL) {
            char name[KW_NAME_MAX];
            snprintf(name, sizeof(name), "pid%d", (int)pid);
            kw_snapshot_add_proc(s, pid, name);
        }
    }
    if (s->n_procs != known && s->n_procs > 1) {
        qsort(s->procs, s->n_procs, sizeof(kw_proc), cmp_proc);
    }

    for (size_t i = 0; i < s->n_procs; i++) {
        s->procs[i].first_thread = 0;
        s->procs[i].n_threads = 0;
    }
    for (size_t i = 0; i < s->n_threads; i++) {
        kw_proc *p = find_proc(s, s->threads[i].pid);
        if (p->n_threads == 0) {
            p->first_thread = i;
        }
        p->n_threads++;
    }
}

const kw_proc *kw_snapshot_proc(const kw_snapshot *s, int32_t pid)
{
    return find_proc(s, pid);
}

long kw_snapshot_thread_index(const kw_snapshot *s, int32_t pid, int32_t tid)
{
    kw_thread key = {.pid = pid, .tid = tid};
    if (s->n_threads == 0) {
        return -1;
    }
    const kw_thread *t =
        bsearch(&key, s->threads, s->n_threads, sizeof(kw_thread), cmp_thread);
    return t ? (long)(t - s->threads) : -1;
}

kw_wait_kind kw_wait_from_state(const char *state)
{
    if (strcmp(state, "SEND") == 0) {
        return KW_WAIT_SEND;
    }
    if (strcmp(state, "REPLY") == 0) {
        return KW_WAIT_REPLY;
    }
    if (strcmp(state, "RECEIVE") == 0) {
        return KW_WAIT_RECEIVE;
    }
    if (strcmp(state, "JOIN") == 0) {
        return KW_WAIT_JOIN;
    }
    if (strcmp(state, "MUTEX") == 0) {
        return KW_WAIT_MUTEX;
    }
    return KW_WAIT_NONE;
}
