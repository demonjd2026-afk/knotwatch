#include "kw/snapshot_io.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 16

typedef struct {
    const char *key;
    const char *value;
} field;

/* Split "k=v k=v ..." in place. Returns the field count or -1 if a token has
 * no '=' or there are too many tokens. */
static int split_fields(char *text, field *fields)
{
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(text, " \t\r\n", &save); tok != NULL;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL || n == MAX_FIELDS) {
            return -1;
        }
        *eq = '\0';
        fields[n].key = tok;
        fields[n].value = eq + 1;
        n++;
    }
    return n;
}

static const char *get(const field *fields, int n, const char *key)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(fields[i].key, key) == 0) {
            return fields[i].value;
        }
    }
    return NULL;
}

/* Fetch an unsigned integer field no larger than `max`. Missing fields are an
 * error unless `optional`, in which case *out keeps its value. */
static int get_u64(const field *fields, int n, const char *key, uint64_t max,
                   int optional, uint64_t *out)
{
    const char *v = get(fields, n, key);
    if (v == NULL) {
        return optional ? 0 : -1;
    }
    if (*v == '\0' || *v == '-' || *v == '+') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long x = strtoull(v, &end, 10);
    if (errno != 0 || *end != '\0' || x > max) {
        return -1;
    }
    *out = x;
    return 0;
}

static int get_i32(const field *fields, int n, const char *key, int optional,
                   int32_t *out)
{
    uint64_t x = (uint64_t)*out;
    if (get_u64(fields, n, key, INT32_MAX, optional, &x) != 0) {
        return -1;
    }
    *out = (int32_t)x;
    return 0;
}

static int parse_thread(const field *f, int n, kw_snapshot *s)
{
    kw_thread t;
    memset(&t, 0, sizeof(t));
    uint64_t prio = 0, rprio = 0, timeout = 0;
    const char *state = get(f, n, "state");
    if (state == NULL || *state == '\0' || strlen(state) >= sizeof(t.state)) {
        return -1;
    }
    if (get_i32(f, n, "pid", 0, &t.pid) || get_i32(f, n, "tid", 0, &t.tid) ||
        get_u64(f, n, "prio", 255, 0, &prio) ||
        get_u64(f, n, "rprio", 255, 0, &rprio) ||
        get_u64(f, n, "blocked_ns", UINT64_MAX, 0, &t.blocked_ns) ||
        get_u64(f, n, "timeout", 1, 0, &timeout)) {
        return -1;
    }
    strcpy(t.state, state);
    t.prio = (uint8_t)prio;
    t.real_prio = (uint8_t)rprio;
    t.timeout_armed = timeout != 0;
    t.wait = kw_wait_from_state(state);

    int bad = 0;
    switch (t.wait) {
    case KW_WAIT_SEND:
    case KW_WAIT_REPLY:
        bad = get_i32(f, n, "srv_pid", 0, &t.target_pid) ||
              get_i32(f, n, "srv_chid", 0, &t.target_chid);
        break;
    case KW_WAIT_RECEIVE:
        bad = get_i32(f, n, "chid", 0, &t.target_chid);
        break;
    case KW_WAIT_JOIN:
        bad = get_i32(f, n, "join_tid", 0, &t.target_tid);
        break;
    case KW_WAIT_NONE:
        break;
    }
    if (bad) {
        return -1;
    }
    kw_snapshot_add_thread(s, &t);
    return 0;
}

static int parse_line(char *line, kw_snapshot *s)
{
    char *rest = line + strcspn(line, " \t");
    if (*rest != '\0') {
        *rest++ = '\0';
    }
    field f[MAX_FIELDS];
    int n = split_fields(rest, f);
    if (n < 0) {
        return -1;
    }

    if (strcmp(line, "meta") == 0) {
        return get_u64(f, n, "time_ns", UINT64_MAX, 0, &s->time_ns) ||
               get_u64(f, n, "scan_ns", UINT64_MAX, 0, &s->scan_ns)
                   ? -1
                   : 0;
    }
    if (strcmp(line, "proc") == 0) {
        int32_t pid = 0;
        const char *name = get(f, n, "name");
        if (name == NULL || *name == '\0' || get_i32(f, n, "pid", 0, &pid)) {
            return -1;
        }
        kw_snapshot_add_proc(s, pid, name);
        return 0;
    }
    if (strcmp(line, "thr") == 0) {
        return parse_thread(f, n, s);
    }
    if (strcmp(line, "conn") == 0) {
        kw_conn c;
        memset(&c, 0, sizeof(c));
        if (get_i32(f, n, "pid", 0, &c.pid) || get_i32(f, n, "coid", 0, &c.coid) ||
            get_i32(f, n, "srv_pid", 0, &c.srv_pid) ||
            get_i32(f, n, "srv_chid", 0, &c.srv_chid)) {
            return -1;
        }
        kw_snapshot_add_conn(s, &c);
        return 0;
    }
    return -1;
}

int kw_snapshot_read(FILE *in, kw_snapshot *s, char *err, size_t err_size)
{
    char *line = NULL;
    size_t cap = 0;
    unsigned long lineno = 0;
    int seen_header = 0;
    int rc = 0;

    while (getline(&line, &cap, in) != -1) {
        lineno++;
        char *p = line + strspn(line, " \t");
        p[strcspn(p, "\r\n")] = '\0';
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (!seen_header) {
            if (strcmp(p, "knotwatch-snapshot 1") != 0) {
                snprintf(err, err_size,
                         "line %lu: expected header 'knotwatch-snapshot 1'", lineno);
                rc = -1;
                break;
            }
            seen_header = 1;
            continue;
        }
        if (parse_line(p, s) != 0) {
            snprintf(err, err_size, "line %lu: malformed record", lineno);
            rc = -1;
            break;
        }
    }
    free(line);
    if (rc == 0 && ferror(in)) {
        snprintf(err, err_size, "read error");
        rc = -1;
    }
    if (rc == 0 && !seen_header) {
        snprintf(err, err_size, "empty input: no snapshot header");
        rc = -1;
    }
    if (rc == 0) {
        kw_snapshot_index(s);
    }
    return rc;
}

void kw_snapshot_write(const kw_snapshot *s, kw_strbuf *out)
{
    kw_sb_puts(out, "knotwatch-snapshot 1\n");
    kw_sb_printf(out, "meta time_ns=%" PRIu64 " scan_ns=%" PRIu64 "\n", s->time_ns,
                 s->scan_ns);
    for (size_t i = 0; i < s->n_procs; i++) {
        kw_sb_printf(out, "proc pid=%d name=%s\n", (int)s->procs[i].pid,
                     s->procs[i].name);
    }
    for (size_t i = 0; i < s->n_threads; i++) {
        const kw_thread *t = &s->threads[i];
        kw_sb_printf(out,
                     "thr pid=%d tid=%d state=%s prio=%u rprio=%u blocked_ns=%" PRIu64
                     " timeout=%d",
                     (int)t->pid, (int)t->tid, t->state, (unsigned)t->prio,
                     (unsigned)t->real_prio, t->blocked_ns, t->timeout_armed ? 1 : 0);
        switch (t->wait) {
        case KW_WAIT_SEND:
        case KW_WAIT_REPLY:
            kw_sb_printf(out, " srv_pid=%d srv_chid=%d", (int)t->target_pid,
                         (int)t->target_chid);
            break;
        case KW_WAIT_RECEIVE:
            kw_sb_printf(out, " chid=%d", (int)t->target_chid);
            break;
        case KW_WAIT_JOIN:
            kw_sb_printf(out, " join_tid=%d", (int)t->target_tid);
            break;
        case KW_WAIT_NONE:
            break;
        }
        kw_sb_puts(out, "\n");
    }
    for (size_t i = 0; i < s->n_conns; i++) {
        const kw_conn *c = &s->conns[i];
        kw_sb_printf(out, "conn pid=%d coid=%d srv_pid=%d srv_chid=%d\n", (int)c->pid,
                     (int)c->coid, (int)c->srv_pid, (int)c->srv_chid);
    }
}
