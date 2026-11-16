/* Tests for the portable core, driven by snapshot fixtures. Runs on any host:
 *   make check */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kw/analyze.h"
#include "kw/contract.h"
#include "kw/render.h"
#include "kw/snapshot_io.h"

static int failures;
static const char *fixture_dir;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

typedef struct {
    kw_snapshot snap;
    kw_graph graph;
    kw_findings findings;
} run;

static void load(const char *name, kw_snapshot *snap)
{
    char path[512], err[160];
    snprintf(path, sizeof(path), "%s/%s", fixture_dir, name);
    FILE *in = fopen(path, "r");
    if (in == NULL) {
        fprintf(stderr, "cannot open fixture %s\n", path);
        exit(2);
    }
    kw_snapshot_init(snap);
    if (kw_snapshot_read(in, snap, err, sizeof(err)) != 0) {
        fprintf(stderr, "fixture %s: %s\n", path, err);
        exit(2);
    }
    fclose(in);
}

static void analyze(const char *fixture, const kw_contract *contract, run *r)
{
    kw_options opts = {.min_block_ns = KW_DEFAULT_MIN_BLOCK_NS};
    load(fixture, &r->snap);
    kw_graph_init(&r->graph);
    kw_findings_init(&r->findings);
    kw_graph_absorb(&r->graph, &r->snap);
    kw_analyze(&r->snap, &r->graph, contract, &opts, &r->findings);
}

static void done(run *r)
{
    kw_findings_free(&r->findings);
    kw_graph_free(&r->graph);
    kw_snapshot_free(&r->snap);
}

static size_t count(const run *r, kw_finding_kind kind)
{
    size_t n = 0;
    for (size_t i = 0; i < r->findings.n; i++) {
        n += r->findings.items[i].kind == kind ? 1 : 0;
    }
    return n;
}

static const kw_finding *first(const run *r, kw_finding_kind kind)
{
    for (size_t i = 0; i < r->findings.n; i++) {
        if (r->findings.items[i].kind == kind) {
            return &r->findings.items[i];
        }
    }
    return NULL;
}

static void parse_contract(const char *text, kw_contract *c)
{
    char err[160];
    FILE *in = fmemopen((void *)text, strlen(text), "r");
    kw_contract_init(c);
    CHECK(in != NULL && kw_contract_read(in, c, err, sizeof(err)) == 0);
    if (in != NULL) {
        fclose(in);
    }
}

/* A shell blocked for hours in read() is REPLY-blocked on its console driver,
 * and the process manager and a driver hold connections to each other. Both
 * are normal and must not be reported. */
static void test_healthy(void)
{
    run r;
    analyze("healthy.kws", NULL, &r);
    CHECK(r.findings.n == 0);
    CHECK(kw_findings_worst(&r.findings) == -1);
    done(&r);
}

/* Two single-threaded processes waiting on each other, plus a third whose
 * request is stuck behind them: one deadlock containing all three threads. */
static void test_deadlock(void)
{
    run r;
    analyze("deadlock.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 1);
    CHECK(count(&r, KW_F_BLOCKING_CYCLE) == 0); /* not reported twice */
    const kw_finding *f = first(&r, KW_F_DEADLOCK);
    CHECK(f != NULL && f->n_procs == 3);
    CHECK(f != NULL && strstr(f->summary, "3 threads") != NULL);
    CHECK(f != NULL && strstr(f->details, "brake_ctl[11] thread 1 REPLY-blocked on "
                                          "wheel_sensor[10] channel 1") != NULL);
    CHECK(f != NULL && strstr(f->details, "hmi[12] thread 1") != NULL);
    CHECK(f != NULL && strstr(f->details, "hmi[12] thread 2") == NULL);
    CHECK(r.findings.items[0].kind == KW_F_DEADLOCK); /* most severe first */
    CHECK(kw_findings_worst(&r.findings) == KW_SEV_CRITICAL);
    done(&r);
}

/* Same cycle, but the server has an idle pool thread that could in theory
 * reply. Not provably dead, so it is a blocking cycle instead. */
static void test_pool_cycle(void)
{
    run r;
    analyze("pool.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 0);
    CHECK(count(&r, KW_F_BLOCKING_CYCLE) == 1);
    const kw_finding *f = first(&r, KW_F_BLOCKING_CYCLE);
    CHECK(f != NULL && f->n_procs == 2);
    done(&r);
}

/* Blocked for 1 ms: an ordinary exchange, nothing is stuck. The sends in both
 * directions are still evidence of a hierarchy violation. */
static void test_transient(void)
{
    run r;
    analyze("transient.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 0);
    CHECK(count(&r, KW_F_BLOCKING_CYCLE) == 0);
    CHECK(count(&r, KW_F_MUTUAL_SEND) == 1);
    CHECK(count(&r, KW_F_POTENTIAL_CYCLE) == 0); /* same cycle, already reported */
    CHECK(kw_findings_worst(&r.findings) == KW_SEV_WARNING);
    done(&r);
}

/* A thread with a kernel timeout armed will unblock by itself. */
static void test_timeout(void)
{
    run r;
    analyze("timeout.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 0);
    CHECK(count(&r, KW_F_BLOCKING_CYCLE) == 0);
    done(&r);
}

/* Nothing blocked, but each process holds a connection to the other. */
static void test_latent(void)
{
    run r;
    analyze("latent.kws", NULL, &r);
    CHECK(r.findings.n == 1);
    CHECK(count(&r, KW_F_POTENTIAL_CYCLE) == 1);
    const kw_finding *f = first(&r, KW_F_POTENTIAL_CYCLE);
    CHECK(f != NULL && strstr(f->details, "connection only") != NULL);
    CHECK(kw_findings_worst(&r.findings) == KW_SEV_NOTICE);
    done(&r);
}

static void test_join(void)
{
    run r;
    analyze("join.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 1);
    const kw_finding *f = first(&r, KW_F_DEADLOCK);
    CHECK(f != NULL && f->n_procs == 1);
    CHECK(f != NULL && strstr(f->summary, "2 threads") != NULL);
    done(&r);
}

/* A lock held across a send. With the owner known, all three threads are
 * provably stuck; without it, the lock waiter must be assumed alive and only
 * the cycle between the processes can be reported. */
static void test_mutex(void)
{
    run r;
    analyze("mutex.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 1);
    CHECK(count(&r, KW_F_BLOCKING_CYCLE) == 0);
    const kw_finding *f = first(&r, KW_F_DEADLOCK);
    CHECK(f != NULL && strstr(f->summary, "3 threads") != NULL && f->n_procs == 2);
    CHECK(f != NULL && strstr(f->details, "motor_ctl[30] thread 2 MUTEX-blocked on a "
                                          "lock held by motor_ctl[30] thread 1") != NULL);
    done(&r);

    analyze("mutex_unknown.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 0);
    CHECK(count(&r, KW_F_BLOCKING_CYCLE) == 1);
    done(&r);

    /* Two threads of one process, each holding the lock the other wants. */
    analyze("abba.kws", NULL, &r);
    CHECK(count(&r, KW_F_DEADLOCK) == 1);
    f = first(&r, KW_F_DEADLOCK);
    CHECK(f != NULL && strstr(f->summary, "2 threads") != NULL && f->n_procs == 1);
    done(&r);
}

static void test_contract(void)
{
    kw_contract c;
    parse_contract("# demo\n"
                   "layer 0 procnto*\n"
                   "layer 1 wheel_sensor   # server\n"
                   "layer 2 brake_ctl\n",
                   &c);
    CHECK(kw_contract_layer(&c, "procnto-smp-instr") == 0);
    CHECK(kw_contract_layer(&c, "brake_ctl") == 2);
    CHECK(kw_contract_layer(&c, "sh") == -1);

    run r;
    analyze("latent.kws", &c, &r);
    CHECK(count(&r, KW_F_CONTRACT) == 1);
    const kw_finding *f = first(&r, KW_F_CONTRACT);
    CHECK(f != NULL && strstr(f->summary, "wheel_sensor (layer 1) holds a connection "
                                          "to brake_ctl (layer 2)") != NULL);
    done(&r);
    kw_contract_free(&c);

    /* `ignore` removes the edge from both the contract and the cycle check. */
    parse_contract("layer 1 wheel_sensor\nlayer 2 brake_ctl\n"
                   "ignore wheel_* brake_ctl\n",
                   &c);
    analyze("latent.kws", &c, &r);
    CHECK(r.findings.n == 0);
    done(&r);
    kw_contract_free(&c);

    /* `strict` flags every process without a layer. */
    parse_contract("strict\nlayer 0 *\n", &c);
    analyze("healthy.kws", &c, &r);
    CHECK(count(&r, KW_F_CONTRACT) == 3); /* same-layer edges, none unlisted */
    done(&r);
    kw_contract_free(&c);

    parse_contract("strict\nlayer 0 procnto*\nlayer 1 devc-*\n", &c);
    analyze("healthy.kws", &c, &r);
    CHECK(count(&r, KW_F_CONTRACT) == 1);
    f = first(&r, KW_F_CONTRACT);
    CHECK(f != NULL && strstr(f->summary, "sh is not assigned") != NULL);
    done(&r);
    kw_contract_free(&c);
}

static void test_contract_errors(void)
{
    static const char *const bad[] = {"layer\n", "layer x foo\n", "layer -1 foo\n",
                                      "layer 1\n", "ignore a\n", "ignore a b c\n",
                                      "strict yes\n", "bogus\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        kw_contract c;
        char err[160];
        FILE *in = fmemopen((void *)bad[i], strlen(bad[i]), "r");
        kw_contract_init(&c);
        CHECK(kw_contract_read(in, &c, err, sizeof(err)) == -1);
        CHECK(strstr(err, "line 1") != NULL);
        fclose(in);
        kw_contract_free(&c);
    }
}

static void test_snapshot_errors(void)
{
    static const char *const bad[] = {
        "",
        "not-a-snapshot\n",
        "knotwatch-snapshot 1\nproc pid=1\n",
        "knotwatch-snapshot 1\nproc pid=-1 name=x\n",
        "knotwatch-snapshot 1\nproc pid=99999999999 name=x\n",
        "knotwatch-snapshot 1\nthr pid=1 tid=1 state=REPLY prio=1 rprio=1 "
        "blocked_ns=0 timeout=0\n", /* REPLY without a server */
        "knotwatch-snapshot 1\nthr pid=1 tid=1 state=READY prio=999 rprio=1 "
        "blocked_ns=0 timeout=0\n",
        "knotwatch-snapshot 1\nthr pid=1 tid=1 state=AVERYLONGSTATENAMEXX prio=1 "
        "rprio=1 blocked_ns=0 timeout=0\n",
        "knotwatch-snapshot 1\nwhat pid=1\n",
        "knotwatch-snapshot 1\nthr pid=1 tid=1 state=MUTEX prio=1 rprio=1 "
        "blocked_ns=0 timeout=0 owner_pid=1\n", /* owner needs both parts */
        "knotwatch-snapshot 1\nproc pid=1 name\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        kw_snapshot s;
        char err[160] = "";
        FILE *in = fmemopen((void *)bad[i], strlen(bad[i]) ? strlen(bad[i]) : 1, "r");
        if (strlen(bad[i]) == 0) {
            fclose(in);
            in = fopen("/dev/null", "r");
        }
        kw_snapshot_init(&s);
        CHECK(kw_snapshot_read(in, &s, err, sizeof(err)) == -1);
        CHECK(err[0] != '\0');
        fclose(in);
        kw_snapshot_free(&s);
    }
}

/* write -> read -> write must be a fixed point. */
static void test_roundtrip(void)
{
    kw_snapshot a, b;
    kw_strbuf one, two;
    char err[160];
    load("mutex.kws", &a);
    kw_sb_init(&one);
    kw_sb_init(&two);
    kw_snapshot_write(&a, &one);
    FILE *in = fmemopen(one.data, one.len, "r");
    kw_snapshot_init(&b);
    CHECK(kw_snapshot_read(in, &b, err, sizeof(err)) == 0);
    fclose(in);
    kw_snapshot_write(&b, &two);
    CHECK(one.len == two.len && strcmp(one.data, two.data) == 0);
    CHECK(b.n_threads == a.n_threads && b.n_conns == a.n_conns);
    kw_sb_free(&one);
    kw_sb_free(&two);
    kw_snapshot_free(&a);
    kw_snapshot_free(&b);
}

/* A thread without a process record gets a placeholder process. */
static void test_orphan_thread(void)
{
    const char *text = "knotwatch-snapshot 1\n"
                       "thr pid=7 tid=1 state=READY prio=10 rprio=10 blocked_ns=0 "
                       "timeout=0\n";
    kw_snapshot s;
    char err[160];
    FILE *in = fmemopen((void *)text, strlen(text), "r");
    kw_snapshot_init(&s);
    CHECK(kw_snapshot_read(in, &s, err, sizeof(err)) == 0);
    fclose(in);
    const kw_proc *p = kw_snapshot_proc(&s, 7);
    CHECK(p != NULL && strcmp(p->name, "pid7") == 0 && p->n_threads == 1);
    kw_snapshot_free(&s);
}

/* The send graph accumulates: a cycle whose two halves were seen in different
 * scans is still a cycle. */
static void test_accumulation(void)
{
    const char *scans[2] = {
        "knotwatch-snapshot 1\nproc pid=1 name=a\nproc pid=2 name=b\n"
        "thr pid=1 tid=1 state=REPLY prio=1 rprio=1 blocked_ns=10 timeout=0 "
        "srv_pid=2 srv_chid=1\n"
        "thr pid=2 tid=1 state=READY prio=1 rprio=1 blocked_ns=0 timeout=0\n",
        "knotwatch-snapshot 1\nproc pid=1 name=a\nproc pid=2 name=b\n"
        "thr pid=2 tid=1 state=REPLY prio=1 rprio=1 blocked_ns=10 timeout=0 "
        "srv_pid=1 srv_chid=1\n"
        "thr pid=1 tid=1 state=READY prio=1 rprio=1 blocked_ns=0 timeout=0\n"};
    kw_graph g;
    kw_graph_init(&g);
    kw_options opts = {.min_block_ns = KW_DEFAULT_MIN_BLOCK_NS};
    for (int i = 0; i < 2; i++) {
        kw_snapshot s;
        kw_findings f;
        char err[160];
        FILE *in = fmemopen((void *)scans[i], strlen(scans[i]), "r");
        kw_snapshot_init(&s);
        kw_findings_init(&f);
        CHECK(kw_snapshot_read(in, &s, err, sizeof(err)) == 0);
        fclose(in);
        kw_graph_absorb(&g, &s);
        kw_analyze(&s, &g, NULL, &opts, &f);
        CHECK(f.n == (size_t)i); /* nothing after scan 1, mutual send after 2 */
        CHECK(i == 0 || f.items[0].kind == KW_F_MUTUAL_SEND);
        kw_findings_free(&f);
        kw_snapshot_free(&s);
    }
    kw_graph_free(&g);
}

static void test_render(void)
{
    run r;
    analyze("deadlock.kws", NULL, &r);
    kw_report rep = {.snap = &r.snap, .graph = &r.graph, .findings = &r.findings};
    kw_strbuf out;
    kw_sb_init(&out);
    kw_render_text(&rep, &out);
    CHECK(strstr(out.data, "[CRITICAL] deadlock:") != NULL);
    kw_sb_free(&out);
    kw_render_json(&rep, &out);
    CHECK(strstr(out.data, "\"worst\":\"critical\"") != NULL);
    CHECK(strstr(out.data, "\"kind\":\"deadlock\"") != NULL);
    kw_sb_free(&out);
    kw_render_dot(&rep, &out);
    CHECK(strstr(out.data, "\"brake_ctl\" -> \"wheel_sensor\";") != NULL);
    CHECK(strstr(out.data, "\"brake_ctl\" [fillcolor=\"#ffa8a8\"]") != NULL);
    kw_sb_free(&out);
    done(&r);
}

int main(int argc, char **argv)
{
    fixture_dir = argc > 1 ? argv[1] : "tests/fixtures";
    test_healthy();
    test_deadlock();
    test_pool_cycle();
    test_transient();
    test_timeout();
    test_latent();
    test_join();
    test_mutex();
    test_contract();
    test_contract_errors();
    test_snapshot_errors();
    test_roundtrip();
    test_orphan_thread();
    test_accumulation();
    test_render();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all core tests passed");
    return 0;
}
