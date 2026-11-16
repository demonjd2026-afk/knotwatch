# knotwatch

Finds message-passing deadlocks on QNX OS 8.0, including the ones that have
not happened yet.

QNX systems are built from processes that talk by synchronous message passing.
The classic rule for keeping that deadlock-free is the *send hierarchy*: never
have two threads send to each other, and keep all sends going one way up a
tree. Nothing enforces the rule. A violation usually sits unnoticed until a
rare code path sends a message "the wrong way", and then two processes wait on
each other forever.

knotwatch watches a running system and reports three things:

| Finding | Meaning | Severity |
| --- | --- | --- |
| `deadlock` | A set of threads that no runnable thread can ever unblock | critical |
| `blocking-cycle` | Processes blocked on each other right now, not yet provably permanent | critical |
| `mutual-send` | Sends have been observed around a cycle: the hierarchy is violated | warning |
| `contract` | A send or connection breaks the layering you declared | warning |
| `potential-cycle` | Connections form a cycle, though no message has crossed it yet | notice |

The last one is the point: the demo system is flagged while it is still
running normally, minutes before it deadlocks.

```
[CRITICAL] deadlock: 3 threads permanently blocked across 3 processes: wheel_sensor, brake_ctl, hmi
  wheel_sensor[10] thread 1 SEND-blocked on brake_ctl[11] channel 1 for 4.0 s (priority 10)
  brake_ctl[11] thread 1 REPLY-blocked on wheel_sensor[10] channel 1 for 4.0 s (priority 10)
  hmi[12] thread 1 REPLY-blocked on brake_ctl[11] channel 1 for 2.0 s (priority 12)
  No thread that could unblock them is runnable or waiting for an outside event.
```

## Status

- The portable core (snapshot format, analysis, contract, renderers, CLI) is
  complete and tested on Linux: `make check`, also clean under ASan and UBSan.
- The QNX half (`src/qnx/`, `demo/`) is written against the QNX OS 8.0
  documentation and has **not yet been compiled or run on QNX**.
  [docs/VERIFICATION.md](docs/VERIFICATION.md) lists exactly what has to be
  confirmed on a target.

## How it works

![architecture](docs/architecture.svg)

A scan reads, for every process, each thread's kernel state and what it is
blocked on (`DCMD_PROC_TIDSTATUS` on `/proc/<pid>/ctl`) and every connection
the process holds (`ConnectServerInfo()`). That becomes a *snapshot*.

**Deadlock detection.** A client blocked on a server can be released by *any*
thread of that server, so a plain cycle search is not enough. knotwatch
computes which blocked threads still have a possible rescuer: a thread is
alive if it is runnable, waiting for an outside event, or has a kernel timeout
armed; a blocked thread is alive if anything it waits on is alive. Whatever is
left after that reaches a fixed point is a knot, and is reported as a
deadlock. Only threads blocked for longer than the scan took are considered,
so everything reported was blocked at one common instant.

**Send graph.** Across scans knotwatch accumulates who has been seen sending
to whom, and who holds a connection to whom. Cycles in that graph are
hierarchy violations, split by evidence into `mutual-send` and
`potential-cycle`.

**Contract.** Optionally declare the hierarchy you intend and have it checked:

```
layer 0 procnto*
layer 1 wheel_sensor
layer 2 brake_ctl
ignore  hmi_* logger     # a link you have reviewed and accept
strict                   # every process must have a layer
```

A process may only send to a strictly lower layer.

## Build and run

Requirements: QNX SDP 8.0 (free through QNX Everywhere), GNU make. Targets:
QNX OS 8.0 on x86_64 or aarch64le, in QEMU or on hardware.

```sh
# On the target (self-hosted QNX Developer Desktop):
make

# Cross-compiling, with the QNX SDP 8.0 environment sourced:
make QNX=1 CC=ntox86_64-gcc  BUILD=build/x86_64      # QEMU x86_64
make QNX=1 CC=ntoaarch64-gcc BUILD=build/aarch64le   # Raspberry Pi, QEMU virt
```

On the target, as root:

```sh
knotwatch scan                       # one scan, report, exit code for scripts
knotwatch scan --contract my.contract
knotwatch watch &                    # resource manager
cat /dev/knotwatch/report            # live findings
cat /dev/knotwatch/dot | dot -Tsvg > sendgraph.svg
knotwatch capture > system.kws       # take a snapshot home
```

The demo:

```sh
knotwatch watch --contract demo/demo.contract &
wheel_sensor --fault-after 300 &     # faults after about 30 s
brake_ctl &
cat /dev/knotwatch/report            # now: potential-cycle and contract warning
sleep 40; cat /dev/knotwatch/report  # after the fault: deadlock
```

On any host (Linux, macOS), no QNX tools needed:

```sh
make && make check
build/knotwatch analyze system.kws --format json
```

Exit status: 0 clean or notices only, 1 warnings, 2 deadlock or blocking
cycle. That makes `knotwatch scan` usable as a gate in a QEMU-based CI job.

## Where QNX is used

| What | Where |
| --- | --- |
| Thread states and block reasons from the process manager | `src/qnx/collect.c`, `collect_threads()` |
| Connection enumeration with `ConnectServerInfo()` | `src/qnx/collect.c`, `collect_connections()` |
| Resource manager with per-open pinned contents (extended OCB) | `src/qnx/watch.c`, `io_read()`, `ocb_calloc()` |
| Timer pulse on the dispatch channel | `src/qnx/watch.c`, `kw_watch_run()`, `on_tick()` |
| slog2 event logging | `src/qnx/watch.c`, `log_changes()` |
| Native message passing, `name_attach()`/`name_open()` | `demo/brake_ctl.c`, `demo/wheel_sensor.c` |

## Limits

- knotwatch is a development, integration and CI tool. QNX documents `/proc`
  as a debugging interface and advises against using it to monitor production
  systems; knotwatch follows that advice.
- Mutex and condition-variable waits are not followed yet. A thread blocked on
  a mutex is treated as alive, so such deadlocks are missed, never
  misreported.
- A `potential-cycle` is a hint. Two processes may hold connections to each
  other and only ever exchange pulses, which cannot deadlock. Review it, then
  `ignore` it in the contract.
- Processes are identified by executable name in the send graph, so several
  instances of one program are one node.

## License

MIT, see [LICENSE](LICENSE).
