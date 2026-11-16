/* knotwatch command line. `analyze` is portable; `scan`, `capture` and
 * `watch` exist only in the QNX build (KW_QNX). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kw/analyze.h"
#include "kw/contract.h"
#include "kw/model.h"
#include "kw/render.h"
#include "kw/snapshot_io.h"
#include "kw/util.h"

#ifdef KW_QNX
#include "../qnx/kw_qnx.h"
#endif

enum { EXIT_USAGE = 64, EXIT_INPUT = 66 };

typedef enum { FMT_TEXT, FMT_JSON, FMT_DOT } format;

typedef struct {
    const char *command;
    const char *input;
    const char *contract_path;
    const char *mount;
    format fmt;
    unsigned long min_block_ms;
    unsigned long interval_ms;
    unsigned long priority;
} cli;

static void usage(FILE *out)
{
    fputs("usage: knotwatch <command> [options]\n"
          "\n"
          "commands:\n"
          "  analyze FILE   analyse a captured snapshot ('-' reads stdin); any host\n"
#ifdef KW_QNX
          "  scan           scan this system once and print a report\n"
          "  capture        scan this system once and print the raw snapshot\n"
          "  watch          keep scanning; publish results under --mount\n"
#endif
          "\n"
          "options:\n"
          "  --contract FILE    declared send hierarchy to check against\n"
          "  --format FMT       text (default), json or dot\n"
          "  --min-block-ms N   ignore threads blocked for less than N ms (500)\n"
#ifdef KW_QNX
          "  --interval-ms N    watch: time between scans (1000)\n"
          "  --mount DIR        watch: where to publish (/dev/knotwatch)\n"
          "  --priority N       watch: run the scanner at this priority\n"
#endif
          "\n"
          "exit status: 0 clean or notices only, 1 warnings, 2 deadlock or\n"
          "blocking cycle, 64 usage error, 66 unreadable input\n",
          out);
}

static int parse_ulong(const char *text, unsigned long max, unsigned long *out)
{
    if (*text == '\0' || *text == '-' || *text == '+') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, cli *c)
{
    c->command = argv[1];
    c->mount = "/dev/knotwatch";
    c->fmt = FMT_TEXT;
    c->min_block_ms = KW_DEFAULT_MIN_BLOCK_NS / 1000000ULL;
    c->interval_ms = 1000;

    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-' || strcmp(arg, "-") == 0) {
            if (c->input != NULL) {
                fprintf(stderr, "knotwatch: unexpected argument '%s'\n", arg);
                return -1;
            }
            c->input = arg;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "knotwatch: option '%s' needs a value\n", arg);
            return -1;
        }
        const char *val = argv[++i];
        int bad = 0;
        if (strcmp(arg, "--contract") == 0) {
            c->contract_path = val;
        } else if (strcmp(arg, "--mount") == 0) {
            c->mount = val;
        } else if (strcmp(arg, "--format") == 0) {
            if (strcmp(val, "text") == 0) {
                c->fmt = FMT_TEXT;
            } else if (strcmp(val, "json") == 0) {
                c->fmt = FMT_JSON;
            } else if (strcmp(val, "dot") == 0) {
                c->fmt = FMT_DOT;
            } else {
                bad = 1;
            }
        } else if (strcmp(arg, "--min-block-ms") == 0) {
            bad = parse_ulong(val, 86400000UL, &c->min_block_ms);
        } else if (strcmp(arg, "--interval-ms") == 0) {
            bad = parse_ulong(val, 3600000UL, &c->interval_ms) || c->interval_ms < 10;
        } else if (strcmp(arg, "--priority") == 0) {
            bad = parse_ulong(val, 255, &c->priority) || c->priority == 0;
        } else {
            fprintf(stderr, "knotwatch: unknown option '%s'\n", arg);
            return -1;
        }
        if (bad) {
            fprintf(stderr, "knotwatch: invalid value '%s' for %s\n", val, arg);
            return -1;
        }
    }
    return 0;
}

static int load_contract(const char *path, kw_contract *contract)
{
    FILE *in = fopen(path, "r");
    if (in == NULL) {
        fprintf(stderr, "knotwatch: cannot open contract %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    char err[160];
    int rc = kw_contract_read(in, contract, err, sizeof(err));
    fclose(in);
    if (rc != 0) {
        fprintf(stderr, "knotwatch: %s: %s\n", path, err);
    }
    return rc;
}

/* Analyse one snapshot, print the report, and map findings to an exit code. */
static int report(const kw_snapshot *snap, const kw_contract *contract, const cli *c)
{
    kw_graph graph;
    kw_findings findings;
    kw_strbuf out;
    kw_graph_init(&graph);
    kw_findings_init(&findings);
    kw_sb_init(&out);

    kw_options opts = {.min_block_ns = (uint64_t)c->min_block_ms * 1000000ULL};
    kw_graph_absorb(&graph, snap);
    kw_analyze(snap, &graph, contract, &opts, &findings);

    kw_report r = {.snap = snap, .graph = &graph, .findings = &findings};
    switch (c->fmt) {
    case FMT_TEXT:
        kw_render_text(&r, &out);
        break;
    case FMT_JSON:
        kw_render_json(&r, &out);
        break;
    case FMT_DOT:
        kw_render_dot(&r, &out);
        break;
    }
    fputs(out.data ? out.data : "", stdout);

    int worst = kw_findings_worst(&findings);
    kw_sb_free(&out);
    kw_findings_free(&findings);
    kw_graph_free(&graph);
    return worst == KW_SEV_CRITICAL ? 2 : worst == KW_SEV_WARNING ? 1 : 0;
}

static int cmd_analyze(const cli *c, const kw_contract *contract)
{
    if (c->input == NULL) {
        fputs("knotwatch: analyze needs a snapshot file (or '-')\n", stderr);
        return EXIT_USAGE;
    }
    FILE *in = strcmp(c->input, "-") == 0 ? stdin : fopen(c->input, "r");
    if (in == NULL) {
        fprintf(stderr, "knotwatch: cannot open %s: %s\n", c->input, strerror(errno));
        return EXIT_INPUT;
    }
    kw_snapshot snap;
    kw_snapshot_init(&snap);
    char err[160];
    int rc = kw_snapshot_read(in, &snap, err, sizeof(err));
    if (in != stdin) {
        fclose(in);
    }
    if (rc != 0) {
        fprintf(stderr, "knotwatch: %s: %s\n", c->input, err);
        kw_snapshot_free(&snap);
        return EXIT_INPUT;
    }
    rc = report(&snap, contract, c);
    kw_snapshot_free(&snap);
    return rc;
}

#ifdef KW_QNX
static int collect_or_complain(kw_snapshot *snap)
{
    kw_collect_stats stats;
    kw_snapshot_init(snap);
    if (kw_collect(snap, &stats) != 0) {
        fprintf(stderr, "knotwatch: cannot scan /proc: %s\n", strerror(errno));
        kw_snapshot_free(snap);
        return -1;
    }
    if (stats.procs_skipped != 0 || stats.conns_skipped != 0) {
        fprintf(stderr,
                "knotwatch: partial scan: %u processes and %u connection lists "
                "were not readable (run as root, or grant xprocess_query)\n",
                stats.procs_skipped, stats.conns_skipped);
    }
    return 0;
}

static int cmd_scan(const cli *c, const kw_contract *contract)
{
    kw_snapshot snap;
    if (collect_or_complain(&snap) != 0) {
        return EXIT_INPUT;
    }
    int rc = report(&snap, contract, c);
    kw_snapshot_free(&snap);
    return rc;
}

static int cmd_capture(void)
{
    kw_snapshot snap;
    if (collect_or_complain(&snap) != 0) {
        return EXIT_INPUT;
    }
    kw_strbuf out;
    kw_sb_init(&out);
    kw_snapshot_write(&snap, &out);
    fputs(out.data, stdout);
    kw_sb_free(&out);
    kw_snapshot_free(&snap);
    return 0;
}

static int cmd_watch(const cli *c, const kw_contract *contract)
{
    kw_watch_config cfg = {
        .mount = c->mount,
        .interval_ms = (unsigned)c->interval_ms,
        .priority = (int)c->priority,
        .contract = contract,
        .options = {.min_block_ns = (uint64_t)c->min_block_ms * 1000000ULL},
    };
    return kw_watch_run(&cfg) == 0 ? 0 : 70;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? EXIT_USAGE : 0;
    }
    cli c;
    memset(&c, 0, sizeof(c));
    if (parse_args(argc, argv, &c) != 0) {
        return EXIT_USAGE;
    }

    kw_contract contract;
    kw_contract_init(&contract);
    const kw_contract *active = NULL;
    if (c.contract_path != NULL) {
        if (load_contract(c.contract_path, &contract) != 0) {
            kw_contract_free(&contract);
            return EXIT_INPUT;
        }
        active = &contract;
    }

    int rc;
    if (strcmp(c.command, "analyze") == 0) {
        rc = cmd_analyze(&c, active);
#ifdef KW_QNX
    } else if (strcmp(c.command, "scan") == 0) {
        rc = cmd_scan(&c, active);
    } else if (strcmp(c.command, "capture") == 0) {
        rc = cmd_capture();
    } else if (strcmp(c.command, "watch") == 0) {
        rc = cmd_watch(&c, active);
#endif
    } else {
        fprintf(stderr, "knotwatch: unknown command '%s'\n", c.command);
        usage(stderr);
        rc = EXIT_USAGE;
    }
    kw_contract_free(&contract);
    return rc;
}
