/* Analysis: turn snapshots into findings.
 *
 * Two inputs with different lifetimes:
 *   - a snapshot answers "who is blocked on whom right now";
 *   - the send graph accumulates, across snapshots, which process has ever
 *     been seen sending to (or holding a connection to) which other process.
 *
 * Findings, most to least severe:
 *   DEADLOCK         a set of threads that no runnable thread can ever
 *                    unblock (a knot in the wait-for graph)
 *   BLOCKING_CYCLE   processes mutually blocked on each other right now,
 *                    though some unrelated thread in them is still alive
 *   MUTUAL_SEND      a send cycle has been observed over time: the send
 *                    hierarchy is violated even if nothing is stuck yet
 *   CONTRACT         an edge breaks the declared layering
 *   POTENTIAL_CYCLE  connections form a cycle; no send across it seen yet
 */
#ifndef KW_ANALYZE_H
#define KW_ANALYZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kw/contract.h"
#include "kw/model.h"

typedef enum {
    KW_F_DEADLOCK = 0,
    KW_F_BLOCKING_CYCLE,
    KW_F_MUTUAL_SEND,
    KW_F_CONTRACT,
    KW_F_POTENTIAL_CYCLE
} kw_finding_kind;

typedef enum { KW_SEV_NOTICE = 0, KW_SEV_WARNING, KW_SEV_CRITICAL } kw_severity;

typedef struct {
    kw_finding_kind kind;
    char *key;     /* stable identity, used to report a finding only once */
    char *summary; /* one line */
    char *details; /* zero or more '\n'-terminated lines */
    char **procs;  /* names of the processes involved */
    size_t n_procs;
} kw_finding;

typedef struct {
    kw_finding *items;
    size_t n, cap;
} kw_findings;

typedef struct {
    char *name;
} kw_gnode;

typedef struct {
    int from, to;   /* node indices: client -> server */
    bool observed;  /* a thread was seen SEND/REPLY-blocked across it */
    bool connected; /* the client holds a connection to the server */
} kw_gedge;

typedef struct {
    kw_gnode *nodes;
    size_t n_nodes, cap_nodes;
    kw_gedge *edges;
    size_t n_edges, cap_edges;
} kw_graph;

typedef struct {
    /* A blocked thread only counts once it has been blocked this long (plus
     * the scan duration, so that everything reported was blocked at the same
     * instant). Filters out ordinary short request/reply exchanges. */
    uint64_t min_block_ns;
} kw_options;

#define KW_DEFAULT_MIN_BLOCK_NS 500000000ULL

void kw_graph_init(kw_graph *g);
void kw_graph_free(kw_graph *g);
/* Merge the edges visible in an indexed snapshot into the graph. Processes
 * are identified by name, so the graph survives restarts and pid reuse. */
void kw_graph_absorb(kw_graph *g, const kw_snapshot *s);

void kw_findings_init(kw_findings *f);
void kw_findings_free(kw_findings *f);

/* Run every check. `contract` may be NULL. Findings are appended to `out`
 * ordered by severity. */
void kw_analyze(const kw_snapshot *s, const kw_graph *g, const kw_contract *contract,
                const kw_options *opts, kw_findings *out);

kw_severity kw_finding_severity(kw_finding_kind kind);
const char *kw_finding_kind_name(kw_finding_kind kind);
const char *kw_severity_name(kw_severity sev);
/* Highest severity present, or -1 when there are no findings. */
int kw_findings_worst(const kw_findings *f);

#endif
