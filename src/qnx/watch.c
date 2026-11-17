/* knotwatch as a resource manager.
 *
 * One thread, one dispatch loop. A timer delivers a pulse to our own channel
 * every interval; the pulse handler scans the system, analyses it and swaps
 * in new contents for four read-only files:
 *
 *   <mount>/report     human-readable findings
 *   <mount>/json       the same, machine-readable
 *   <mount>/dot        the accumulated send graph for Graphviz
 *   <mount>/snapshot   the raw scan, replayable with `knotwatch analyze`
 *
 * Every open() pins the contents current at that moment (a reference-counted
 * blob held by the OCB), so a reader never sees half of one scan and half of
 * the next. New and cleared findings are logged to slog2 and stderr. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct kw_node;
struct kw_ocb;
#define IOFUNC_ATTR_T struct kw_node
#define IOFUNC_OCB_T struct kw_ocb
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>

#include "kw/render.h"
#include "kw/snapshot_io.h"
#include "kw/util.h"
#include "kw_qnx.h"

typedef struct kw_blob {
    unsigned refs;
    size_t len;
    char *data;
} kw_blob;

struct kw_node {
    iofunc_attr_t attr; /* must be first: the iofunc layer sees only this */
    kw_blob *blob;
};

struct kw_ocb {
    iofunc_ocb_t hdr; /* must be first */
    kw_blob *blob;
};

enum { NODE_REPORT, NODE_JSON, NODE_DOT, NODE_SNAPSHOT, NODE_COUNT };
static const char *const node_names[NODE_COUNT] = {"report", "json", "dot", "snapshot"};

typedef struct {
    const kw_watch_config *cfg;
    struct kw_node nodes[NODE_COUNT];
    kw_graph graph;    /* accumulated across scans */
    char **active;     /* keys of the findings present after the last scan */
    size_t n_active;
    slog2_buffer_t log;
} watch_state;

static watch_state state;

/* ------------------------------------------------------------------- blobs */

static kw_blob *blob_new(kw_strbuf *text)
{
    kw_blob *b = kw_xcalloc(1, sizeof(*b));
    b->refs = 1;
    b->len = text->len;
    b->data = kw_sb_steal(text);
    return b;
}

static void blob_unref(kw_blob *b)
{
    if (b != NULL && --b->refs == 0) {
        free(b->data);
        free(b);
    }
}

static void node_publish(struct kw_node *node, kw_strbuf *text)
{
    blob_unref(node->blob);
    node->blob = blob_new(text);
    node->attr.nbytes = (off_t)node->blob->len;
    node->attr.flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_DIRTY_TIME;
}

/* --------------------------------------------------------- resource manager */

static IOFUNC_OCB_T *ocb_calloc(resmgr_context_t *ctp, IOFUNC_ATTR_T *node)
{
    (void)ctp;
    struct kw_ocb *ocb = calloc(1, sizeof(*ocb));
    if (ocb != NULL) {
        ocb->blob = node->blob;
        ocb->blob->refs++;
    }
    return ocb;
}

static void ocb_free(IOFUNC_OCB_T *ocb)
{
    blob_unref(ocb->blob);
    free(ocb);
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, &ocb->hdr, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    const kw_blob *blob = ocb->blob;
    size_t offset = (size_t)ocb->hdr.offset;
    size_t left = offset < blob->len ? blob->len - offset : 0;
    size_t nbytes = _IO_READ_GET_NBYTES(msg);
    if (nbytes > left) {
        nbytes = left;
    }
    if (nbytes == 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    /* The blob outlives the reply: this OCB holds a reference to it. */
    SETIOV(ctp->iov, blob->data + offset, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    ocb->hdr.offset += (off_t)nbytes;
    return _RESMGR_NPARTS(1);
}

/* ------------------------------------------------------------------ scans */

static int key_active(const char *key)
{
    for (size_t i = 0; i < state.n_active; i++) {
        if (strcmp(state.active[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

static void log_event(uint8_t severity, const char *what, const char *text)
{
    slog2f(state.log, 0, severity, "%s: %s", what, text);
    fprintf(stderr, "knotwatch: %s: %s\n", what, text);
}

/* Log findings that appeared or disappeared since the previous scan. */
static void log_changes(const kw_findings *f)
{
    for (size_t i = 0; i < f->n; i++) {
        const kw_finding *x = &f->items[i];
        if (key_active(x->key)) {
            continue;
        }
        kw_severity sev = kw_finding_severity(x->kind);
        log_event(sev == KW_SEV_CRITICAL  ? SLOG2_CRITICAL
                  : sev == KW_SEV_WARNING ? SLOG2_WARNING
                                          : SLOG2_NOTICE,
                  kw_finding_kind_name(x->kind), x->summary);
    }
    for (size_t i = 0; i < state.n_active; i++) {
        int still = 0;
        for (size_t j = 0; j < f->n && !still; j++) {
            still = strcmp(f->items[j].key, state.active[i]) == 0;
        }
        if (!still) {
            log_event(SLOG2_INFO, "cleared", state.active[i]);
        }
        free(state.active[i]);
    }
    free(state.active);
    state.active = kw_xcalloc(f->n, sizeof(char *));
    state.n_active = f->n;
    for (size_t i = 0; i < f->n; i++) {
        state.active[i] = kw_xstrdup(f->items[i].key);
    }
}

static void scan_once(void)
{
    kw_snapshot snap;
    kw_collect_stats stats;
    kw_snapshot_init(&snap);
    if (kw_collect(&snap, &stats) != 0) {
        log_event(SLOG2_ERROR, "scan failed", strerror(errno));
        kw_snapshot_free(&snap);
        return;
    }
    if (snap.n_procs == 0) {
        /* Keep the last good results rather than publish a false all-clear. */
        log_event(SLOG2_ERROR, "scan failed",
                  "no process was readable; run as root or with xprocess_query");
        kw_snapshot_free(&snap);
        return;
    }

    kw_findings findings;
    kw_findings_init(&findings);
    kw_graph_absorb(&state.graph, &snap);
    kw_analyze(&snap, &state.graph, state.cfg->contract, &state.cfg->options, &findings);

    kw_report r = {.snap = &snap, .graph = &state.graph, .findings = &findings};
    kw_strbuf text;
    kw_sb_init(&text);
    kw_render_text(&r, &text);
    if (stats.procs_skipped != 0 || stats.conns_skipped != 0) {
        kw_sb_printf(&text,
                     "\nPartial scan: %u processes and %u connection lists were "
                     "not readable.\n",
                     stats.procs_skipped, stats.conns_skipped);
    }
    if (stats.locks_unknown != 0) {
        kw_sb_printf(&text,
                     "\n%u threads wait for a mutex whose owner could not be "
                     "determined; they are treated as not stuck.\n",
                     stats.locks_unknown);
    }
    node_publish(&state.nodes[NODE_REPORT], &text);
    kw_render_json(&r, &text);
    node_publish(&state.nodes[NODE_JSON], &text);
    kw_render_dot(&r, &text);
    node_publish(&state.nodes[NODE_DOT], &text);
    kw_snapshot_write(&snap, &text);
    node_publish(&state.nodes[NODE_SNAPSHOT], &text);

    log_changes(&findings);
    kw_findings_free(&findings);
    kw_snapshot_free(&snap);
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;
    scan_once();
    return 0;
}

/* ------------------------------------------------------------------ set-up */

static int fail(const char *what)
{
    fprintf(stderr, "knotwatch: %s: %s\n", what, strerror(errno));
    return -1;
}

int kw_watch_run(const kw_watch_config *cfg)
{
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t io_funcs;
    static iofunc_funcs_t ocb_funcs = {
        .nfuncs = _IOFUNC_NFUNCS, .ocb_calloc = ocb_calloc, .ocb_free = ocb_free};
    static iofunc_mount_t mount;

    state.cfg = cfg;
    kw_graph_init(&state.graph);

    slog2_buffer_set_config_t log_cfg;
    memset(&log_cfg, 0, sizeof(log_cfg));
    log_cfg.buffer_set_name = "knotwatch";
    log_cfg.num_buffers = 1;
    log_cfg.verbosity_level = SLOG2_INFO;
    log_cfg.buffer_config[0].buffer_name = "events";
    log_cfg.buffer_config[0].num_pages = 4;
    if (slog2_register(&log_cfg, &state.log, 0) == -1) {
        return fail("slog2_register");
    }

    if (cfg->priority > 0) {
        int err = pthread_setschedprio(pthread_self(), cfg->priority);
        if (err != EOK) {
            errno = err;
            return fail("cannot set priority");
        }
    }

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) {
        return fail("dispatch_create");
    }

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS,
                     &io_funcs);
    io_funcs.read = io_read;
    io_funcs.read64 = io_read;
    mount.funcs = &ocb_funcs;

    for (int i = 0; i < NODE_COUNT; i++) {
        struct kw_node *node = &state.nodes[i];
        kw_strbuf empty;
        kw_sb_init(&empty);
        iofunc_attr_init(&node->attr, S_IFREG | 0444, NULL, NULL);
        node->attr.mount = &mount;
        node_publish(node, &empty);

        char path[256];
        int n = snprintf(path, sizeof(path), "%s/%s", cfg->mount, node_names[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            errno = ENAMETOOLONG;
            return fail(cfg->mount);
        }
        if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0, &connect_funcs, &io_funcs,
                          node) == -1) {
            return fail(path);
        }
    }

    /* Periodic scan: a timer pulse on our own dispatch channel. */
    int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
    if (code == -1) {
        return fail("pulse_attach");
    }
    int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (coid == -1) {
        return fail("message_connect");
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, (short)code, 0);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        return fail("timer_create");
    }
    struct itimerspec period;
    period.it_value.tv_sec = cfg->interval_ms / 1000;
    period.it_value.tv_nsec = (long)(cfg->interval_ms % 1000) * 1000000L;
    period.it_interval = period.it_value;
    if (timer_settime(timer, 0, &period, NULL) == -1) {
        return fail("timer_settime");
    }

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        return fail("dispatch_context_alloc");
    }
    /* Everything that needs root (the path names, the priority) is done. */
    if (cfg->drop && kw_drop_privileges(cfg->uid, cfg->gid) != 0) {
        return fail("cannot drop privileges");
    }
    scan_once(); /* have content before the first reader arrives */
    fprintf(stderr, "knotwatch: watching, results under %s (every %u ms)\n", cfg->mount,
            cfg->interval_ms);

    for (;;) {
        dispatch_context_t *next = dispatch_block(ctp);
        if (next == NULL) {
            return fail("dispatch_block");
        }
        ctp = next;
        dispatch_handler(ctp);
    }
}
