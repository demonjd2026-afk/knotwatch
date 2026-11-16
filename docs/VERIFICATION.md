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
- Noise on a stock image, two cases found:
  - The process manager and the disk driver hold connections to each other.
    Edges starting at `procnto` are now excluded; confirmed gone on re-run.
  - `devb-eide` and `fsevmgr` hold connections to each other. This is a true
    observation about two stock services, reported as a `potential-cycle`
    notice. It is left detectable and silenced with an `ignore` line in
    `demo/demo.contract`.

## Still to verify on QNX

1. `knotwatch capture` output against `pidin` for a whole system, not only
   the two demo threads.
2. Behaviour without root: which processes and connection lists become
   unreadable, and that the scan says so.
3. `debug_thread_t.timeout`: `collect.c` treats a non-zero value as "a kernel
   timeout is armed". It is zero for the deadlocked demo threads, as
   expected, but a thread blocked after `TimerTimeout()` has not been checked.
   If the field means something else, deadlocks could be missed.
4. The other files under `/dev/knotwatch` (`json`, `dot`, `snapshot`), two
   readers at once, and a reader that stays open across several scans.
5. slog2 output: `slog2info -b knotwatch`.
6. aarch64le, and real hardware.

## Planned next

- Follow mutex ownership so lock-plus-message deadlocks are covered.
- Run as a non-root user with only the abilities it needs, with a security
  policy.
- A host-side HTML viewer for the JSON report.
- A CI recipe that boots QEMU, runs a workload and gates on `knotwatch scan`.
