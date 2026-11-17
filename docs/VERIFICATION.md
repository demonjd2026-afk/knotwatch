# What is verified, and what still has to be

## Verified on Linux (`make check`)

- Snapshot parsing and writing, including rejection of malformed input.
- Deadlock, blocking-cycle, mutual-send, potential-cycle and contract
  findings against the fixtures in `tests/fixtures/`.
- Accumulation of the send graph across scans.
- Text, JSON (parsed by a real JSON parser) and DOT output.
- CLI exit codes and error handling.
- The MCP server in `agent/`: handshake, tool listing, all three tools, every
  error path, cleaning of text from the target, and that the source command
  is not run through a shell (`tests/test_agent.py`, part of `make check`).
  Also driven with the official MCP Python client library (version 2.3.0),
  using the real report in `viewer/example.json`.
- No warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; no
  findings under AddressSanitizer and UndefinedBehaviorSanitizer.

## Verified on QNX OS 8.0.0, x86_64, QEMU

Built with `ntox86_64-gcc` from QNX SDP 8.0 (build 141, Linux host tools), run
with `scripts/qnx-image.sh` and `scripts/qemu-run.sh`.

- Everything in `src/qnx/` and `demo/` compiles without warnings and links.
- The resource manager starts, `/dev/knotwatch/report` appears and can be read.
- A full scan of the stock image (22 processes, 101 threads, 225 connections)
  takes about 30 ms under emulation.
- Before the planted fault the demo is reported as a `potential-cycle` and a
  contract violation; after it, as a `deadlock` naming both threads.
- The blocked states and peers knotwatch reports for the two demo threads
  match `pidin` (REPLY on the sensor, SEND on the controller).
- `nsec_since_block` behaves as assumed: 21 s reported for threads that had
  been blocked for about 21 s.
- `knotwatch scan` exits with status 2 while the deadlock is present.
- New and cleared findings are printed as they change.
- `/dev/knotwatch` lists four read-only files (`report`, `json`, `dot`,
  `snapshot`) with sizes that match what a read returns.
- The published `snapshot` replays through `knotwatch analyze` on the target
  and gives the same verdict (exit status 2).
- `knotwatch capture` sees every thread: 101, against 102 in a `pidin` taken
  a moment later that also counts `pidin` itself.
- A reader that keeps `report` open across several scans gets one complete,
  consistent report.
- slog2 receives each new and cleared finding (`slog2info -b knotwatch`).
- `debug_thread_t.timeout`: `<sys/states.h>` documents it as a bit per state
  with a kernel timeout armed. In a live capture it was set only on a
  sleeping thread and clear on every SEND, REPLY and JOIN-blocked thread.
- Without root, no process is readable. knotwatch now reports that as an
  error instead of an empty, clean-looking report.
- The `ignore` rule in `demo/demo.contract` silences the stock-services
  notice.
- Mutex waits: in the second demo (`motor_ctl`, `logger`) the owner of the
  lock is found and the three-thread deadlock is reported. The owner agrees
  with `pidin` (`413719-01`). Trace with `KNOTWATCH_DEBUG_LOCKS=1`.
- The codelab in `codelab/`: `standoff` and `whoblocks` build with
  `ntox86_64-gcc -Wall -Wextra` without warnings, the image built by
  `codelab/make-image.sh` boots to a root shell, and the output blocks in the
  tutorial are from that run. knotwatch reports the codelab's deadlock too.
- The interactive console (`scripts/qemu-run.sh console`) gives a root shell.
- Noise on a stock image, two cases found:
  - The process manager and the disk driver hold connections to each other.
    Edges starting at `procnto` are now excluded; confirmed gone on re-run.
  - `devb-eide` and `fsevmgr` hold connections to each other. This is a true
    observation about two stock services, reported as a `potential-cycle`
    notice. It is left detectable and silenced with an `ignore` line in
    `demo/demo.contract`.

- A send under a kernel timeout (`scripts/qnx-image.sh verify2`): with
  `wheel_sensor --send-timeout-ms 15000` the sensor's thread shows
  `state=SEND ... timeout=1` in a capture, no deadlock is reported while the
  timeout runs, the send then fails with "Connection timed out" and the
  sensor goes back to RECEIVE. The hierarchy warnings stay.
- Eight readers of `report` and eight of `json` at the same time each get a
  complete, identical file.
- Mutex owners that are not thread 1: `tests/target/kwt_locks.c threads`
  is reported as a three-thread deadlock with thread 2 waiting on thread 3
  and thread 3 on thread 2, as `pidin` shows (`811030-03`, `811030-02`).
- Process-shared mutexes held by another process: `kwt_locks shared` is
  reported as a two-process deadlock, each main thread waiting on a lock held
  by the other process, as `pidin` shows.
- Without root: `knotwatch scan --user 1000:1000` and
  `knotwatch watch --user 1000:1000` print "running as uid 1000, gid 1000"
  and report the same two deadlocks as a scan run as root. The process keeps
  the `xprocess_query` and `xprocess_mem_read` abilities and nothing else
  beyond an ordinary user's.

## Verified on QNX OS 8.0.0, aarch64le, QEMU

Built with `ntoaarch64-gcc`, image made with `ARCH=aarch64
scripts/qnx-image.sh verify`, booted with `ARCH=aarch64 scripts/qemu-run.sh`
on QEMU's `virt-7.2` machine with a `cortex-a76` CPU (`uname`: QNX 8.0.0
QEMU_virt aarch64le).

- Everything compiles without warnings, as on x86_64.
- Both demo scenarios: the latent cycle is reported before the fault, the
  two-thread deadlock after it, and the three-thread deadlock with the lock
  held across a send. `knotwatch scan` exits with status 2.
- The mutex owner is decoded the same way as on x86_64 (raw value
  `0x90120001` for pid 397330 thread 1) and agrees with `pidin`
  (`397330-01`).
- Blocked states and peers agree with `pidin` for all demo threads.
- The codelab's `standoff` and `whoblocks` build and behave as in the
  tutorial; knotwatch reports that deadlock too.
- The four published files, the snapshot replay, the long-lived reader,
  slog2 logging and the refusal of an unprivileged scan all behave as on
  x86_64. A capture has 61 threads against 62 in `pidin`, which counts
  itself.
- The `verify2` checks pass as on x86_64: no deadlock is reported for a send
  under a kernel timeout (`state=SEND ... timeout=1`) and the sensor
  recovers; eight concurrent readers get identical complete files; locks
  held by threads 2 and 3 and process-shared locks held by another process
  are attributed correctly and agree with `pidin`; `scan` and `watch` with
  `--user 1000:1000` run as uid 1000 and report the same two deadlocks as
  root.

## Still to verify on QNX

1. Real hardware.
2. The MCP server reading from a live target over `ssh`, and its use from
   inside a specific AI assistant. The QEMU image used so far has no network.

## Planned next

- A security policy (secpol) type for knotwatch, as an alternative to
  `--user`.
- A CI recipe that boots QEMU, runs a workload and gates on `knotwatch scan`.
