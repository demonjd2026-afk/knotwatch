# What is verified, and what still has to be

## Verified (on Linux, `make check`)

- Snapshot parsing and writing, including rejection of malformed input.
- Deadlock, blocking-cycle, mutual-send, potential-cycle and contract
  findings against the fixtures in `tests/fixtures/`.
- Accumulation of the send graph across scans.
- Text, JSON (parsed by a real JSON parser) and DOT output.
- CLI exit codes and error handling.
- No warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; no
  findings under AddressSanitizer and UndefinedBehaviorSanitizer.

## To verify on QNX OS 8.0 (not done yet)

Everything in `src/qnx/` and `demo/` was written from the QNX 8.0 documentation
and has never been compiled against the real headers. In order:

1. **It compiles.** `make QNX=1 CC=ntox86_64-gcc`. Likely friction: header
   names, `rcvid_t`, the `iofunc_mount_t` / `iofunc_funcs_t` layout, the
   `read64` member of `resmgr_io_funcs_t`, state names in `<sys/states.h>`.
2. **`knotwatch capture` output is plausible.** Compare a few processes with
   `pidin`: same thread count, same states, same blocked-on pid for SEND and
   REPLY.
3. **Read-only `ctl` is enough.** `collect.c` opens `/proc/<pid>/ctl` with
   `O_RDONLY`. Confirm `DCMD_PROC_TIDSTATUS` returns unsanitised data for
   processes of other users when run as root.
4. **`ConnectServerInfo()` across processes.** Confirm it lists another
   process's connections as root, and what it returns without privileges
   (expected: the scan reports the lists as skipped).
5. **`debug_thread_t.timeout`.** `collect.c` treats a non-zero value as "a
   kernel timeout is armed". Confirm with a thread blocked in `MsgSend()` after
   `TimerTimeout()` versus one without. If the field means something else,
   deadlocks could be missed.
6. **`nsec_since_block`.** Confirm it grows while a thread stays REPLY-blocked
   and is small right after it blocks.
7. **Resource manager.** `ls -l /dev/knotwatch`, `cat` each file, two readers
   at once, a reader that stays open across several scans.
8. **Demo.** Before the fault: `potential-cycle` plus the contract warning.
   After: `deadlock` naming both processes. Check `slog2info -b knotwatch`.
9. **Noise on a stock image.** Run `knotwatch scan` on the unmodified QEMU
   image and review every `potential-cycle`. System services that hold
   connections to each other may need default `ignore` rules.

## Planned next

- Follow mutex ownership so lock-plus-message deadlocks are covered.
- Run as a non-root user with only the abilities it needs, with a security
  policy.
- A host-side HTML viewer for the JSON report.
- A CI recipe that boots QEMU, runs a workload and gates on `knotwatch scan`.
