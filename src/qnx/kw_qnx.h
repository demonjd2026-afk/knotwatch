/* The QNX-only half of knotwatch: collecting a snapshot from the process
 * manager, and serving results as a resource manager. Nothing outside
 * src/qnx/ includes a QNX header. */
#ifndef KW_QNX_H
#define KW_QNX_H

#include <stdint.h>

#include "kw/analyze.h"
#include "kw/contract.h"
#include "kw/model.h"

typedef struct {
    unsigned procs_skipped; /* /proc/<pid>/ctl could not be opened */
    unsigned conns_skipped; /* connection list of a process was unreadable */
    unsigned locks_unknown; /* mutex waiters whose lock owner was not found */
} kw_collect_stats;

/* Scan every process on this node into `s` (initialised by the caller) and
 * index it. Returns 0, or -1 with errno set if /proc itself is unreadable. */
int kw_collect(kw_snapshot *s, kw_collect_stats *stats);

typedef struct {
    const char *mount;         /* directory to appear under, e.g. /dev/knotwatch */
    unsigned interval_ms;      /* time between scans */
    int priority;              /* scheduling priority, 0 = leave unchanged */
    const kw_contract *contract; /* may be NULL */
    kw_options options;
    int drop;                  /* give up root once the files are published */
    unsigned uid, gid;         /* who to run as when `drop` is set */
} kw_watch_config;

/* Keep only the abilities a scan needs (xprocess_query, xprocess_mem_read),
 * then switch to uid:gid for good. Returns 0, or -1 with errno set. */
int kw_drop_privileges(unsigned uid, unsigned gid);

/* Run the resource manager. Only returns on a fatal error (-1). */
int kw_watch_run(const kw_watch_config *cfg);

#endif
