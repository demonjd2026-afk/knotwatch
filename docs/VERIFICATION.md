# What is verified, and what still has to be

## Verified on Linux (`make check`)

- Snapshot parsing and writing, including rejection of malformed input.
- Deadlock, blocking-cycle, mutual-send, potential-cycle and contract
  findings against the fixtures in `tests/fixtures/`.
- Accumulation of the send graph across scans.
- Text, JSON (parsed by a real JSON parser) and DOT output.
- CLI exit codes and error handling.
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
- Noise on a stock image, two cases found:
  - The process manager and the disk driver hold connections to each other.
    Edges starting at `procnto` are now excluded; confirmed gone on re-run.
  - `devb-eide` and `fsevmgr` hold connections to each other. This is a true
    observation about two stock services, reported as a `potential-cycle`
    notice. It is left detectable and silenced with an `ignore` line in
    `demo/demo.contract`.

## Still to verify on QNX

1. A thread blocked in `MsgSend()` after `TimerTimeout()`: confirm it is
   treated as alive.
2. Running as a non-root user that has been granted `xprocess_query`.
3. Two readers of `/dev/knotwatch` files at the same time.
4. Mutex owners other than thread 1, a process-shared mutex held by another
   process, and the owner encoding on aarch64le.
5. aarch64le, and real hardware.

## Planned next

- Run as a non-root user with only the abilities it needs, with a security
  policy.
- A host-side HTML viewer for the JSON report.
- A CI recipe that boots QEMU, runs a workload and gates on `knotwatch scan`.
