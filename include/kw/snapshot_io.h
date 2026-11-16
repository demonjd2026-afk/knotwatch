/* Snapshot file format: a line-oriented text capture of one scan.
 *
 *   knotwatch-snapshot 1
 *   meta time_ns=<n> scan_ns=<n>
 *   proc pid=<n> name=<token>
 *   thr  pid=<n> tid=<n> state=<NAME> prio=<n> rprio=<n> blocked_ns=<n>
 *        timeout=<0|1> [srv_pid=<n> srv_chid=<n>] [chid=<n>] [join_tid=<n>]
 *        [owner_pid=<n> owner_tid=<n>]      (MUTEX, when the owner is known)
 *   conn pid=<n> coid=<n> srv_pid=<n> srv_chid=<n>
 *
 * Blank lines and lines starting with '#' are ignored. A capture taken on a
 * QNX target can be analysed on any host with `knotwatch analyze`. */
#ifndef KW_SNAPSHOT_IO_H
#define KW_SNAPSHOT_IO_H

#include <stdio.h>

#include "kw/model.h"
#include "kw/util.h"

/* Parse a snapshot from `in` into `s` (initialised by the caller) and index
 * it. Returns 0 on success; on failure returns -1 and writes a message that
 * names the offending line into `err`. */
int kw_snapshot_read(FILE *in, kw_snapshot *s, char *err, size_t err_size);

/* Serialise an indexed snapshot. */
void kw_snapshot_write(const kw_snapshot *s, kw_strbuf *out);

#endif
