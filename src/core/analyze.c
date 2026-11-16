#include "kw/analyze.h"

#include <stdlib.h>
#include <string.h>

#include "kw/util.h"

/* ---------------------------------------------------------------- findings */

void kw_findings_init(kw_findings *f)
{
    memset(f, 0, sizeof(*f));
}

void kw_findings_free(kw_findings *f)
{
    for (size_t i = 0; i < f->n; i++) {
        kw_finding *x = &f->items[i];
        free(x->key);
        free(x->summary);
        free(x->details);
        for (size_t j = 0; j < x->n_procs; j++) {
            free(x->procs[j]);
        }
        free(x->procs);
    }
    free(f->items);
    memset(f, 0, sizeof(*f));
}

static kw_finding *add_finding(kw_findings *f, kw_finding_kind kind)
{
    void *items = f->items;
    kw_grow(&items, &f->cap, f->n + 1, sizeof(kw_finding));
    f->items = items;
    kw_finding *x = &f->items[f->n++];
    memset(x, 0, sizeof(*x));
    x->kind = kind;
    return x;
}

static void finding_add_proc(kw_finding *x, const char *name)
{
    for (size_t i = 0; i < x->n_procs; i++) {
        if (strcmp(x->procs[i], name) == 0) {
            return;
        }
    }
    x->procs = kw_xrealloc(x->procs, (x->n_procs + 1) * sizeof(char *));
    x->procs[x->n_procs++] = kw_xstrdup(name);
}

kw_severity kw_finding_severity(kw_finding_kind kind)
{
    switch (kind) {
    case KW_F_DEADLOCK:
    case KW_F_BLOCKING_CYCLE:
        return KW_SEV_CRITICAL;
    case KW_F_MUTUAL_SEND:
    case KW_F_CONTRACT:
        return KW_SEV_WARNING;
    case KW_F_POTENTIAL_CYCLE:
        return KW_SEV_NOTICE;
    }
    return KW_SEV_NOTICE;
}

const char *kw_finding_kind_name(kw_finding_kind kind)
{
    switch (kind) {
    case KW_F_DEADLOCK:
        return "deadlock";
    case KW_F_BLOCKING_CYCLE:
        return "blocking-cycle";
    case KW_F_MUTUAL_SEND:
        return "mutual-send";
    case KW_F_CONTRACT:
        return "contract";
    case KW_F_POTENTIAL_CYCLE:
        return "potential-cycle";
    }
    return "unknown";
}

const char *kw_severity_name(kw_severity sev)
{
    switch (sev) {
    case KW_SEV_CRITICAL:
        return "critical";
    case KW_SEV_WARNING:
        return "warning";
    case KW_SEV_NOTICE:
        return "notice";
    }
    return "notice";
}

int kw_findings_worst(const kw_findings *f)
{
    int worst = -1;
    for (size_t i = 0; i < f->n; i++) {
        int sev = (int)kw_finding_severity(f->items[i].kind);
        if (sev > worst) {
            worst = sev;
        }
    }
    return worst;
}

/* ------------------------------------------------------------------- graph */

void kw_graph_init(kw_graph *g)
{
    memset(g, 0, sizeof(*g));
}

void kw_graph_free(kw_graph *g)
{
    for (size_t i = 0; i < g->n_nodes; i++) {
        free(g->nodes[i].name);
    }
    free(g->nodes);
    free(g->edges);
    memset(g, 0, sizeof(*g));
}

static int graph_node(kw_graph *g, const char *name)
{
    for (size_t i = 0; i < g->n_nodes; i++) {
        if (strcmp(g->nodes[i].name, name) == 0) {
            return (int)i;
        }
    }
    void *items = g->nodes;
    kw_grow(&items, &g->cap_nodes, g->n_nodes + 1, sizeof(kw_gnode));
    g->nodes = items;
    g->nodes[g->n_nodes].name = kw_xstrdup(name);
    return (int)g->n_nodes++;
}

static void graph_edge(kw_graph *g, int from, int to, bool observed)
{
    if (from == to) {
        return; /* a process messaging its own channel is not a hierarchy edge */
    }
    kw_gedge *e = NULL;
    for (size_t i = 0; i < g->n_edges; i++) {
        if (g->edges[i].from == from && g->edges[i].to == to) {
            e = &g->edges[i];
            break;
        }
    }
    if (e == NULL) {
        void *items = g->edges;
        kw_grow(&items, &g->cap_edges, g->n_edges + 1, sizeof(kw_gedge));
        g->edges = items;
        e = &g->edges[g->n_edges++];
        memset(e, 0, sizeof(*e));
        e->from = from;
        e->to = to;
    }
    if (observed) {
        e->observed = true;
    } else {
        e->connected = true;
    }
}

void kw_graph_absorb(kw_graph *g, const kw_snapshot *s)
{
    for (size_t i = 0; i < s->n_procs; i++) {
        graph_node(g, s->procs[i].name);
    }
    for (size_t i = 0; i < s->n_threads; i++) {
        const kw_thread *t = &s->threads[i];
        if (t->wait != KW_WAIT_SEND && t->wait != KW_WAIT_REPLY) {
            continue;
        }
        const kw_proc *client = kw_snapshot_proc(s, t->pid);
        const kw_proc *server = kw_snapshot_proc(s, t->target_pid);
        if (client != NULL && server != NULL) {
            graph_edge(g, graph_node(g, client->name), graph_node(g, server->name),
                       true);
        }
    }
    for (size_t i = 0; i < s->n_conns; i++) {
        const kw_proc *client = kw_snapshot_proc(s, s->conns[i].pid);
        const kw_proc *server = kw_snapshot_proc(s, s->conns[i].srv_pid);
        if (client != NULL && server != NULL) {
            graph_edge(g, graph_node(g, client->name), graph_node(g, server->name),
                       false);
        }
    }
}

/* -------------------------------------------- strongly connected components */

typedef struct {
    int n;
    int *start; /* CSR offsets, n + 1 entries */
    int *adj;   /* edge targets */
    int *comp;  /* component id per node */
    int *size;  /* node count per component */
    int n_comp;
} digraph;

static void digraph_build(digraph *d, int n, const int *from, const int *to, int m)
{
    d->n = n;
    d->start = kw_xcalloc((size_t)n + 1, sizeof(int));
    d->adj = kw_xcalloc((size_t)m, sizeof(int));
    d->comp = kw_xcalloc((size_t)n, sizeof(int));
    d->size = kw_xcalloc((size_t)n, sizeof(int));
    d->n_comp = 0;
    for (int i = 0; i < m; i++) {
        d->start[from[i] + 1]++;
    }
    for (int i = 0; i < n; i++) {
        d->start[i + 1] += d->start[i];
    }
    int *fill = kw_xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < m; i++) {
        d->adj[d->start[from[i]] + fill[from[i]]++] = to[i];
    }
    free(fill);
}

static void digraph_free(digraph *d)
{
    free(d->start);
    free(d->adj);
    free(d->comp);
    free(d->size);
    memset(d, 0, sizeof(*d));
}

/* Tarjan's algorithm with an explicit stack, so depth is not bounded by the
 * C stack on targets with small thread stacks. */
static void digraph_scc(digraph *d)
{
    int n = d->n;
    int *index = kw_xcalloc((size_t)n, sizeof(int));
    int *low = kw_xcalloc((size_t)n, sizeof(int));
    int *next_edge = kw_xcalloc((size_t)n, sizeof(int));
    int *stack = kw_xcalloc((size_t)n, sizeof(int));
    int *call = kw_xcalloc((size_t)n, sizeof(int));
    bool *on_stack = kw_xcalloc((size_t)n, sizeof(bool));
    int counter = 0, sp = 0;

    for (int root = 0; root < n; root++) {
        if (index[root] != 0) {
            continue;
        }
        int depth = 0;
        call[depth++] = root;
        index[root] = low[root] = ++counter;
        next_edge[root] = d->start[root];
        stack[sp++] = root;
        on_stack[root] = true;

        while (depth > 0) {
            int v = call[depth - 1];
            if (next_edge[v] < d->start[v + 1]) {
                int w = d->adj[next_edge[v]++];
                if (index[w] == 0) {
                    index[w] = low[w] = ++counter;
                    next_edge[w] = d->start[w];
                    stack[sp++] = w;
                    on_stack[w] = true;
                    call[depth++] = w;
                } else if (on_stack[w] && index[w] < low[v]) {
                    low[v] = index[w];
                }
                continue;
            }
            if (low[v] == index[v]) {
                int w;
                do {
                    w = stack[--sp];
                    on_stack[w] = false;
                    d->comp[w] = d->n_comp;
                    d->size[d->n_comp]++;
                } while (w != v);
                d->n_comp++;
            }
            depth--;
            if (depth > 0) {
                int parent = call[depth - 1];
                if (low[v] < low[parent]) {
                    low[parent] = low[v];
                }
            }
        }
    }
    free(index);
    free(low);
    free(next_edge);
    free(stack);
    free(call);
    free(on_stack);
}

/* Shortest cycle through `start` that stays inside its component. Writes the
 * node sequence (start first, not repeated at the end) into `path` and
 * returns its length; 0 if there is none. */
static int digraph_cycle(const digraph *d, int start, int *path)
{
    int n = d->n;
    int *prev = kw_xcalloc((size_t)n, sizeof(int));
    int *queue = kw_xcalloc((size_t)n, sizeof(int));
    bool *seen = kw_xcalloc((size_t)n, sizeof(bool));
    int head = 0, tail = 0, last = -1;

    queue[tail++] = start;
    seen[start] = true;
    prev[start] = -1;
    while (head < tail && last < 0) {
        int v = queue[head++];
        for (int e = d->start[v]; e < d->start[v + 1]; e++) {
            int w = d->adj[e];
            if (w == start) {
                last = v;
                break;
            }
            if (!seen[w] && d->comp[w] == d->comp[start]) {
                seen[w] = true;
                prev[w] = v;
                queue[tail++] = w;
            }
        }
    }
    int len = 0;
    if (last >= 0) {
        for (int v = last; v != -1; v = prev[v]) {
            len++;
        }
        int i = len;
        for (int v = last; v != -1; v = prev[v]) {
            path[--i] = v;
        }
    }
    free(prev);
    free(queue);
    free(seen);
    return len;
}

/* --------------------------------------------------------- snapshot checks */

/* Waits whose resolution depends on one specific other thread: the thread
 * being joined, or the thread that holds the mutex. */
static bool waits_on_thread(const kw_thread *t)
{
    return t->wait == KW_WAIT_JOIN ||
           (t->wait == KW_WAIT_MUTEX && t->target_tid != 0);
}

/* Index of that thread, or -1 if it is not in the snapshot. */
static long awaited_thread(const kw_snapshot *s, const kw_thread *t)
{
    int32_t pid = t->wait == KW_WAIT_JOIN ? t->pid : t->target_pid;
    return kw_snapshot_thread_index(s, pid, t->target_tid);
}

static bool follows_wait(const kw_thread *t)
{
    return t->wait == KW_WAIT_SEND || t->wait == KW_WAIT_REPLY ||
           waits_on_thread(t);
}

/* A thread is a candidate for "stuck" only if it waits on another thread, has
 * no kernel timeout that would free it, and has been blocked long enough that
 * every candidate in this snapshot was blocked at one common instant. */
static bool is_candidate(const kw_snapshot *s, const kw_thread *t, const kw_options *o)
{
    uint64_t need = o->min_block_ns;
    if (s->scan_ns > UINT64_MAX - need) {
        return false;
    }
    need += s->scan_ns;
    return follows_wait(t) && !t->timeout_armed && t->blocked_ns >= need;
}

static void describe_thread(const kw_snapshot *s, const kw_thread *t, kw_strbuf *out)
{
    const kw_proc *p = kw_snapshot_proc(s, t->pid);
    char dur[32];
    kw_format_duration(t->blocked_ns, dur, sizeof(dur));
    kw_sb_printf(out, "  %s[%d] thread %d ", p ? p->name : "?", (int)t->pid,
                 (int)t->tid);
    if (t->wait == KW_WAIT_JOIN) {
        kw_sb_printf(out, "JOIN-blocked on thread %d", (int)t->target_tid);
    } else if (t->wait == KW_WAIT_MUTEX) {
        const kw_proc *owner = kw_snapshot_proc(s, t->target_pid);
        kw_sb_printf(out, "MUTEX-blocked on a lock held by %s[%d] thread %d",
                     owner ? owner->name : "?", (int)t->target_pid,
                     (int)t->target_tid);
    } else {
        const kw_proc *srv = kw_snapshot_proc(s, t->target_pid);
        kw_sb_printf(out, "%s-blocked on %s[%d] channel %d", t->state,
                     srv ? srv->name : "?", (int)t->target_pid, (int)t->target_chid);
    }
    kw_sb_printf(out, " for %s (priority %u)\n", dur, (unsigned)t->prio);
}

static size_t uf_find(size_t *parent, size_t i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static void uf_union(size_t *parent, size_t a, size_t b)
{
    a = uf_find(parent, a);
    b = uf_find(parent, b);
    if (a != b) {
        parent[a > b ? a : b] = a < b ? a : b; /* smallest index is the root */
    }
}

/* Deadlock = knot. Start by assuming every thread that is not a candidate is
 * alive. A candidate becomes alive if anything that could unblock it is alive:
 * for SEND/REPLY any thread of the server process (any of them may receive or
 * reply), for JOIN the joined thread, for MUTEX the thread holding the lock.
 * Iterate to a fixed point; whatever is
 * left can only be unblocked by threads that are themselves left, i.e. never.
 * `dead[i]` is set for those threads. */
static void find_deadlocks(const kw_snapshot *s, const kw_options *o, bool *dead,
                           kw_findings *out)
{
    size_t n = s->n_threads;
    for (size_t i = 0; i < n; i++) {
        const kw_thread *t = &s->threads[i];
        dead[i] = is_candidate(s, t, o);
        if (!dead[i]) {
            continue;
        }
        if (waits_on_thread(t)) {
            if (awaited_thread(s, t) < 0) {
                dead[i] = false; /* that thread is gone: the wait will end */
            }
        } else {
            const kw_proc *srv = kw_snapshot_proc(s, t->target_pid);
            if (srv == NULL || srv->n_threads == 0) {
                dead[i] = false; /* server not in this scan: cannot judge */
            }
        }
    }

    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (!dead[i]) {
                continue;
            }
            const kw_thread *t = &s->threads[i];
            bool rescuer = false;
            if (waits_on_thread(t)) {
                rescuer = !dead[awaited_thread(s, t)];
            } else {
                const kw_proc *srv = kw_snapshot_proc(s, t->target_pid);
                for (size_t j = 0; j < srv->n_threads && !rescuer; j++) {
                    rescuer = !dead[srv->first_thread + j];
                }
            }
            if (rescuer) {
                dead[i] = false;
                changed = true;
            }
        }
    }

    /* Group dead threads that wait on each other into separate findings. */
    size_t *parent = kw_xcalloc(n, sizeof(size_t));
    for (size_t i = 0; i < n; i++) {
        parent[i] = i;
    }
    for (size_t i = 0; i < n; i++) {
        if (!dead[i]) {
            continue;
        }
        const kw_thread *t = &s->threads[i];
        if (waits_on_thread(t)) {
            uf_union(parent, i, (size_t)awaited_thread(s, t));
        } else {
            const kw_proc *srv = kw_snapshot_proc(s, t->target_pid);
            for (size_t j = 0; j < srv->n_threads; j++) {
                uf_union(parent, i, srv->first_thread + j);
            }
        }
    }

    for (size_t root = 0; root < n; root++) {
        if (!dead[root] || uf_find(parent, root) != root) {
            continue;
        }
        kw_finding *x = add_finding(out, KW_F_DEADLOCK);
        kw_strbuf key, details, names;
        kw_sb_init(&key);
        kw_sb_init(&details);
        kw_sb_init(&names);
        kw_sb_puts(&key, "deadlock:");
        size_t members = 0, pids = 0;
        int32_t last_pid = -1;
        for (size_t i = root; i < n; i++) {
            if (!dead[i] || uf_find(parent, i) != root) {
                continue;
            }
            const kw_thread *t = &s->threads[i];
            const kw_proc *p = kw_snapshot_proc(s, t->pid);
            if (t->pid != last_pid) { /* threads are sorted by pid */
                last_pid = t->pid;
                pids++; /* two instances of one program are two processes */
            }
            size_t before = x->n_procs;
            finding_add_proc(x, p->name);
            if (x->n_procs != before) {
                kw_sb_printf(&names, "%s%s", before ? ", " : "", p->name);
            }
            kw_sb_printf(&key, "%s%s/%d", members ? "," : "", p->name, (int)t->tid);
            describe_thread(s, t, &details);
            members++;
        }
        kw_sb_puts(&details, "  No thread that could unblock them is runnable or "
                             "waiting for an outside event.\n");
        kw_strbuf summary;
        kw_sb_init(&summary);
        kw_sb_printf(&summary, "%zu thread%s permanently blocked across %zu process%s: %s",
                     members, members == 1 ? "" : "s", pids, pids == 1 ? "" : "es",
                     names.data);
        x->key = kw_sb_steal(&key);
        x->summary = kw_sb_steal(&summary);
        x->details = kw_sb_steal(&details);
        kw_sb_free(&names);
    }
    free(parent);
}

/* Blocking cycle: processes blocked on each other right now. Weaker than a
 * deadlock (some other thread in one of them is still alive, e.g. an idle
 * pool thread, so in theory it could reply) but in practice the same bug. */
static void find_blocking_cycles(const kw_snapshot *s, const kw_options *o,
                                 const bool *dead, kw_findings *out)
{
    size_t n = s->n_threads;
    int *from = kw_xcalloc(n, sizeof(int));
    int *to = kw_xcalloc(n, sizeof(int));
    size_t *thread_of = kw_xcalloc(n, sizeof(size_t));
    int m = 0;

    for (size_t i = 0; i < n; i++) {
        const kw_thread *t = &s->threads[i];
        if (waits_on_thread(t) || !is_candidate(s, t, o)) {
            continue;
        }
        const kw_proc *client = kw_snapshot_proc(s, t->pid);
        const kw_proc *server = kw_snapshot_proc(s, t->target_pid);
        if (server == NULL || server == client) {
            continue;
        }
        from[m] = (int)(client - s->procs);
        to[m] = (int)(server - s->procs);
        thread_of[m] = i;
        m++;
    }

    digraph d;
    digraph_build(&d, (int)s->n_procs, from, to, m);
    digraph_scc(&d);

    for (int c = 0; c < d.n_comp; c++) {
        if (d.size[c] < 2) {
            continue;
        }
        bool all_dead = true;
        for (int e = 0; e < m; e++) {
            if (d.comp[from[e]] == c && d.comp[to[e]] == c && !dead[thread_of[e]]) {
                all_dead = false;
            }
        }
        if (all_dead) {
            continue; /* already reported as a deadlock */
        }
        kw_finding *x = add_finding(out, KW_F_BLOCKING_CYCLE);
        kw_strbuf key, details, names;
        kw_sb_init(&key);
        kw_sb_init(&details);
        kw_sb_init(&names);
        kw_sb_puts(&key, "blocking-cycle:");
        for (size_t p = 0; p < s->n_procs; p++) {
            if (d.comp[p] != c) {
                continue;
            }
            size_t before = x->n_procs;
            finding_add_proc(x, s->procs[p].name);
            if (x->n_procs != before) {
                kw_sb_printf(&names, "%s%s", before ? ", " : "", s->procs[p].name);
                kw_sb_printf(&key, "%s%s", before ? "," : "", s->procs[p].name);
            }
        }
        for (int e = 0; e < m; e++) {
            if (d.comp[from[e]] == c && d.comp[to[e]] == c) {
                describe_thread(s, &s->threads[thread_of[e]], &details);
            }
        }
        kw_sb_puts(&details, "  These requests wait on each other. Another thread in "
                             "one of the processes is still alive, so this is "
                             "reported as a cycle rather than a proven deadlock.\n");
        kw_strbuf summary;
        kw_sb_init(&summary);
        kw_sb_printf(&summary, "processes are blocked on each other: %s", names.data);
        x->key = kw_sb_steal(&key);
        x->summary = kw_sb_steal(&summary);
        x->details = kw_sb_steal(&details);
        kw_sb_free(&names);
    }
    digraph_free(&d);
    free(from);
    free(to);
    free(thread_of);
}

/* ------------------------------------------------------------ graph checks */

static const kw_gedge *graph_find_edge(const kw_graph *g, int from, int to)
{
    for (size_t i = 0; i < g->n_edges; i++) {
        if (g->edges[i].from == from && g->edges[i].to == to) {
            return &g->edges[i];
        }
    }
    return NULL;
}

/* The process manager (procnto) is the microkernel itself. It holds
 * connections to drivers and filesystems and they hold connections to it, by
 * design, so edges that start at it say nothing about an application's send
 * hierarchy. Edges that end at it are kept: everything may send to it. */
static bool is_process_manager(const char *name)
{
    return strncmp(name, "procnto", 7) == 0;
}

/* Build a digraph over the send graph's nodes from the edges that are not
 * ignored by the contract and, if `observed_only`, were actually seen used. */
static void hierarchy_digraph(const kw_graph *g, const kw_contract *contract,
                              bool observed_only, digraph *d)
{
    int *from = kw_xcalloc(g->n_edges, sizeof(int));
    int *to = kw_xcalloc(g->n_edges, sizeof(int));
    int m = 0;
    for (size_t i = 0; i < g->n_edges; i++) {
        const kw_gedge *e = &g->edges[i];
        if (observed_only && !e->observed) {
            continue;
        }
        if (is_process_manager(g->nodes[e->from].name) ||
            kw_contract_ignored(contract, g->nodes[e->from].name,
                                g->nodes[e->to].name)) {
            continue;
        }
        from[m] = e->from;
        to[m] = e->to;
        m++;
    }
    digraph_build(d, (int)g->n_nodes, from, to, m);
    digraph_scc(d);
    free(from);
    free(to);
}

static void report_cycle(const kw_graph *g, const digraph *d, int comp,
                         kw_finding_kind kind, kw_findings *out)
{
    int start = -1;
    for (int v = 0; v < d->n && start < 0; v++) {
        if (d->comp[v] == comp) {
            start = v;
        }
    }
    int *path = kw_xcalloc((size_t)d->n, sizeof(int));
    int len = digraph_cycle(d, start, path);

    kw_finding *x = add_finding(out, kind);
    kw_strbuf key, summary, details;
    kw_sb_init(&key);
    kw_sb_init(&summary);
    kw_sb_init(&details);
    kw_sb_printf(&key, "%s:", kw_finding_kind_name(kind));
    for (int v = 0; v < d->n; v++) {
        if (d->comp[v] == comp) {
            kw_sb_printf(&key, "%s%s", x->n_procs ? "," : "", g->nodes[v].name);
            finding_add_proc(x, g->nodes[v].name);
        }
    }
    kw_sb_puts(&summary, kind == KW_F_MUTUAL_SEND
                             ? "send hierarchy violated, sends observed around a cycle: "
                             : "connections form a cycle, a latent deadlock if "
                               "messages are sent around it: ");
    for (int i = 0; i < len; i++) {
        int a = path[i], b = path[(i + 1) % len];
        const kw_gedge *e = graph_find_edge(g, a, b);
        kw_sb_printf(&summary, "%s -> ", g->nodes[a].name);
        kw_sb_printf(&details, "  %s -> %s  [%s]\n", g->nodes[a].name, g->nodes[b].name,
                     e != NULL && e->observed ? "send observed" : "connection only");
    }
    kw_sb_puts(&summary, len > 0 ? g->nodes[path[0]].name : "?");
    if (d->size[comp] > len) {
        kw_sb_printf(&details, "  (%d processes are entangled in total; one "
                               "shortest cycle is shown)\n",
                     d->size[comp]);
    }
    x->key = kw_sb_steal(&key);
    x->summary = kw_sb_steal(&summary);
    x->details = kw_sb_steal(&details);
    free(path);
}

static void find_hierarchy_cycles(const kw_graph *g, const kw_contract *contract,
                                  kw_findings *mutual, kw_findings *potential)
{
    digraph seen, all;
    hierarchy_digraph(g, contract, true, &seen);
    hierarchy_digraph(g, contract, false, &all);

    for (int c = 0; c < seen.n_comp; c++) {
        if (seen.size[c] >= 2) {
            report_cycle(g, &seen, c, KW_F_MUTUAL_SEND, mutual);
        }
    }
    for (int c = 0; c < all.n_comp; c++) {
        if (all.size[c] < 2) {
            continue;
        }
        /* Skip if this is exactly a cycle already reported as observed. */
        int first = -1;
        bool same = true;
        for (int v = 0; v < all.n; v++) {
            if (all.comp[v] != c) {
                continue;
            }
            if (first < 0) {
                first = v;
            }
            same = same && seen.comp[v] == seen.comp[first];
        }
        if (same && seen.size[seen.comp[first]] == all.size[c]) {
            continue;
        }
        report_cycle(g, &all, c, KW_F_POTENTIAL_CYCLE, potential);
    }
    digraph_free(&seen);
    digraph_free(&all);
}

static void check_contract(const kw_graph *g, const kw_contract *contract,
                           kw_findings *out)
{
    for (size_t i = 0; i < g->n_edges; i++) {
        const kw_gedge *e = &g->edges[i];
        const char *client = g->nodes[e->from].name;
        const char *server = g->nodes[e->to].name;
        if (is_process_manager(client) ||
            kw_contract_ignored(contract, client, server)) {
            continue;
        }
        int lc = kw_contract_layer(contract, client);
        int ls = kw_contract_layer(contract, server);
        if (lc < 0 || ls < 0 || ls < lc) {
            continue;
        }
        kw_finding *x = add_finding(out, KW_F_CONTRACT);
        kw_strbuf key, summary, details;
        kw_sb_init(&key);
        kw_sb_init(&summary);
        kw_sb_init(&details);
        kw_sb_printf(&key, "contract:%s->%s", client, server);
        kw_sb_printf(&summary, "%s (layer %d) %s %s (layer %d)", client, lc,
                     e->observed ? "sends to" : "holds a connection to", server, ls);
        kw_sb_printf(&details, "  The contract only allows sends to a lower layer. "
                               "Evidence: %s.\n",
                     e->observed ? "send observed" : "connection only");
        finding_add_proc(x, client);
        finding_add_proc(x, server);
        x->key = kw_sb_steal(&key);
        x->summary = kw_sb_steal(&summary);
        x->details = kw_sb_steal(&details);
    }
    if (!contract->strict) {
        return;
    }
    for (size_t i = 0; i < g->n_nodes; i++) {
        const char *name = g->nodes[i].name;
        if (kw_contract_layer(contract, name) >= 0) {
            continue;
        }
        kw_finding *x = add_finding(out, KW_F_CONTRACT);
        kw_strbuf key, summary;
        kw_sb_init(&key);
        kw_sb_init(&summary);
        kw_sb_printf(&key, "contract:unlisted:%s", name);
        kw_sb_printf(&summary, "%s is not assigned to any layer (strict contract)",
                     name);
        finding_add_proc(x, name);
        x->key = kw_sb_steal(&key);
        x->summary = kw_sb_steal(&summary);
        x->details = kw_xstrdup("");
    }
}

static void move_findings(kw_findings *dst, kw_findings *src)
{
    for (size_t i = 0; i < src->n; i++) {
        void *items = dst->items;
        kw_grow(&items, &dst->cap, dst->n + 1, sizeof(kw_finding));
        dst->items = items;
        dst->items[dst->n++] = src->items[i];
    }
    free(src->items);
    memset(src, 0, sizeof(*src));
}

void kw_analyze(const kw_snapshot *s, const kw_graph *g, const kw_contract *contract,
                const kw_options *opts, kw_findings *out)
{
    bool *dead = kw_xcalloc(s->n_threads, sizeof(bool));
    find_deadlocks(s, opts, dead, out);
    find_blocking_cycles(s, opts, dead, out);
    free(dead);

    kw_findings mutual, potential;
    kw_findings_init(&mutual);
    kw_findings_init(&potential);
    find_hierarchy_cycles(g, contract, &mutual, &potential);
    move_findings(out, &mutual);
    if (contract != NULL) {
        check_contract(g, contract, out);
    }
    move_findings(out, &potential);
}
