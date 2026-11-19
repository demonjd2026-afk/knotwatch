# knotwatch

**Finds message-passing deadlocks on QNX OS 8.0, including the ones that have
not happened yet.**

![QNX OS 8.0](https://img.shields.io/badge/QNX%20OS-8.0-1c7ed6)
![targets](https://img.shields.io/badge/targets-x86__64%20%7C%20aarch64le-2f9e44)
![language](https://img.shields.io/badge/C-gnu11-495057)
![license](https://img.shields.io/badge/license-MIT-f08c00)

**[Watch the demo (under 3 minutes)](https://youtu.be/IeGtVh0GBWg)** ·
[Codelab](codelab/README.md) ·
[What is verified](docs/VERIFICATION.md)

My entry for the QNX Global Hackathon, category Developer Tooling & Host Tools.

## Contents

- [What it does](#what-it-does)
- [Why it matters](#why-it-matters)
- [How it works](#how-it-works)
- [Build and run](#build-and-run)
- [Report viewer](#report-viewer)
- [AI assistants](#ai-assistants)
- [Codelab](#codelab)
- [Where QNX is used](#where-qnx-is-used)
- [Verification status](#verification-status)
- [Repository layout](#repository-layout)
- [Limits](#limits)
- [Hackathon submission](#hackathon-submission)
- [License](#license)

## What it does

QNX systems are built from processes that talk by synchronous message passing.
The classic rule for keeping that deadlock-free is the *send hierarchy*: never
have two threads send to each other, and keep all sends going one way up a
tree. Nothing enforces the rule. A violation usually sits unnoticed until a
rare code path sends a message "the wrong way", and then two processes wait on
each other forever.

knotwatch watches a running system and reports five kinds of finding:

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
[CRITICAL] deadlock: 2 threads permanently blocked across 2 processes: wheel_sensor, brake_ctl
  wheel_sensor[327700] thread 1 SEND-blocked on brake_ctl[339989] channel 1 for 21.0 s (priority 10)
  brake_ctl[339989] thread 1 REPLY-blocked on wheel_sensor[327700] channel 1 for 21.0 s (priority 10)
  No thread that could unblock them is runnable or waiting for an outside event.
```

That is real output from the demo on QNX OS 8.0 in QEMU. Twenty-five seconds
earlier, while both processes were still exchanging messages normally, the
same run had already reported:

```
[WARNING] contract: wheel_sensor (layer 1) holds a connection to brake_ctl (layer 2)
[NOTICE] potential-cycle: connections form a cycle, a latent deadlock if messages are sent around it: wheel_sensor -> brake_ctl -> wheel_sensor
```

## Why it matters

A send-hierarchy deadlock does not crash and does not log. The system, or a
part of it, simply stops answering. Finding the cause by hand goes like this:

1. Run `pidin` and pick out the SEND- and REPLY-blocked threads among every
   thread on the system.
2. For each one, look up the process it is blocked on, and check *every*
   thread of that process, because any of them could still reply.
3. If one of those is MUTEX-blocked, find the thread that holds the lock and
   see what that thread is blocked on in turn.
4. Repeat until the chain closes into a cycle, then convince yourself that it
   is permanent and not just slow.

knotwatch does those steps on every scan. In the recorded demo run the fault
fired and the deadlock was reported one second later, with the threads, what
each waits on, and the statement that nothing can unblock them. The design
fault behind it (a connection that goes up the hierarchy) had been reported
while the system was still healthy, and `knotwatch scan` exits with status 2
so an automated test fails instead of hanging.

## How it works

![architecture](docs/architecture.svg)

A scan reads, for every process, each thread's kernel state and what it is
blocked on (`DCMD_PROC_TIDSTATUS` on `/proc/<pid>/ctl`) and every connection
the process holds (`ConnectServerInfo()`). For a thread blocked on a mutex it
also reads the mutex from that process's memory to learn which thread holds
it. That becomes a *snapshot*.

**Deadlock detection.** A client blocked on a server can be released by *any*
thread of that server, so a plain cycle search is not enough. knotwatch
computes which blocked threads still have a possible rescuer: a thread is
alive if it is runnable, waiting for an outside event, or has a kernel timeout
armed; a blocked thread is alive if anything it waits on is alive. Whatever is
left after that reaches a fixed point is a knot, and is reported as a
deadlock. Message waits, mutex waits and thread joins are all followed, so a
lock held across a send is caught too:

```
[CRITICAL] deadlock: 3 threads permanently blocked across 2 processes: logger, motor_ctl
  logger[401430] thread 1 REPLY-blocked on motor_ctl[413719] channel 1 for 11.9 s (priority 10)
  motor_ctl[413719] thread 1 REPLY-blocked on logger[401430] channel 1 for 11.9 s (priority 10)
  motor_ctl[413719] thread 2 MUTEX-blocked on a lock held by motor_ctl[413719] thread 1 for 11.9 s (priority 10)
```

Only threads blocked for longer than the scan took are considered, so
everything reported was blocked at one common instant.

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

Requirements: QNX SDP 8.0 (free through QNX Everywhere), GNU make. Tested
targets: QNX OS 8.0.0 on x86_64 and on aarch64le, both in QEMU, built with
`ntox86_64-gcc` and `ntoaarch64-gcc` from the SDP's Linux host tools.

```sh
# On the target (self-hosted QNX Developer Desktop):
make

# Cross-compiling, with the QNX SDP 8.0 environment sourced:
make QNX=1 CC=ntox86_64-gcc  BUILD=build/x86_64      # QEMU x86_64
make QNX=1 CC=ntoaarch64-gcc BUILD=build/aarch64     # QEMU virt, 64-bit Arm boards
```

From macOS, where the SDP tools do not run natively, the scripts below run
them in a small Linux container (Docker) and boot the result in QEMU
(`brew install qemu`). They expect the SDP in `~/qnx800` with the package
"QNX SDP Host Tools (Linux 64-bit)" installed.

```sh
sh scripts/qnx-docker-build.sh   # compile -> build/x86_64
sh scripts/qnx-image.sh          # bootable image with knotwatch and the demo
sh scripts/qemu-run.sh           # boot it, run the demo, print the reports
sh scripts/qemu-run.sh console   # or get an interactive console instead
```

Put `ARCH=aarch64` in front of any of them for 64-bit Arm. That image also
needs the SDP packages "QEMU Virtualization Drivers (Startup)" and
"Virtualization Drivers (Block)", installed with QNX Software Center.

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

`scan`, `capture` and `watch` have to start as root, but need not stay root:
with `--user UID[:GID]` knotwatch keeps the two process-manager abilities a
scan needs (`xprocess_query`, `xprocess_mem_read`) and switches to that user.
`watch` does so after it has attached its path names.

Exit status: 0 clean or notices only, 1 warnings, 2 deadlock or blocking
cycle. That makes `knotwatch scan` usable as a gate in a QEMU-based CI job.

## Report viewer

`viewer/index.html` is a single self-contained page that draws a JSON report:
the send graph with servers above their clients, and the findings beside it.
Selecting a finding highlights the processes involved. Open the file in a
browser, then drop a report on it or choose "Show the example".
The screenshot shows the example with "Only processes with findings" ticked.

![report viewer](docs/viewer.png)

```sh
knotwatch scan --format json > report.json        # on the target
build/knotwatch analyze system.kws --format json  # or from a snapshot, on any host
```

The page is generated from `viewer/template.html` by `scripts/make-viewer.sh`,
which embeds `viewer/example.json` as the example. That file is a real
report, read from `/dev/knotwatch/json` on QNX OS 8.0.0 with both demo
scenarios deadlocked.

## AI assistants

`agent/knotwatch_mcp.py` is a small [Model Context Protocol](https://modelcontextprotocol.io)
server. It gives an AI coding assistant three read-only tools, so the
assistant can check a QNX system while it works on the code:

| Tool | Answers |
| --- | --- |
| `knotwatch_status` | Is anything deadlocked? One line per finding. |
| `knotwatch_finding` | The evidence for one finding, what it means, and the usual fixes on QNX. |
| `knotwatch_process` | Who one process sends to, and who sends to it. |

It runs on the development host and needs only Python 3. Because knotwatch
publishes its results as files, reaching a target needs no agent on the
target, just a command that prints the file:

```sh
# a live target
python3 agent/knotwatch_mcp.py --command 'ssh root@target cat /dev/knotwatch/json'
# a saved report, or a snapshot analysed on the host
python3 agent/knotwatch_mcp.py --report report.json
python3 agent/knotwatch_mcp.py --snapshot system.kws --knotwatch build/knotwatch
```

Register one of those lines as an MCP server in your assistant. Add `--check`
to print the status once and exit, to test the source.

The server only reads. The source is fixed at start-up, tool arguments never
reach a command line, and the command runs without a shell. Text that comes
from the target is passed to the assistant as data, with control characters
removed and its length capped.

An answer from `knotwatch_finding` for the second demo, from the real report:

```
[critical] deadlock: 3 threads permanently blocked across 2 processes: logger, motor_ctl

Evidence from the target:
  logger[385045] thread 1 REPLY-blocked on motor_ctl[389142] channel 1 for 38.5 s (priority 10)
  motor_ctl[389142] thread 1 REPLY-blocked on logger[385045] channel 1 for 38.5 s (priority 10)
  motor_ctl[389142] thread 2 MUTEX-blocked on a lock held by motor_ctl[389142] thread 1 for 38.5 s (priority 10)
  ...
Usual fixes on QNX:
  2. Replace the upward MsgSend() with something that does not block: a pulse
     (MsgSendPulse()) or an event the client armed earlier (MsgDeliverEvent()).
  3. If a thread is MUTEX-blocked in the list, a lock is held across MsgSend().
     Copy what is needed, release the lock, then send.
```

## Codelab

[codelab/README.md](codelab/README.md) is a 45-minute tutorial: create a
message-passing deadlock on QNX OS 8.0, look at it with `pidin`, and write a
135-line tool that finds it by reading `/proc`. It is self-contained, and it
is the simplest way to understand what knotwatch builds on.

## Where QNX is used

| What | Where |
| --- | --- |
| Thread states and block reasons from the process manager | `src/qnx/collect.c`, `collect_threads()` |
| Connection enumeration with `ConnectServerInfo()` | `src/qnx/collect.c`, `collect_connections()` |
| Mutex owner read from the waiter's address space (`/proc/<pid>/as`) | `src/qnx/collect.c`, `resolve_lock_owners()` |
| Resource manager with per-open pinned contents (extended OCB) | `src/qnx/watch.c`, `io_read()`, `ocb_calloc()` |
| Timer pulse on the dispatch channel | `src/qnx/watch.c`, `kw_watch_run()`, `on_tick()` |
| slog2 event logging | `src/qnx/watch.c`, `log_changes()` |
| Dropping root while keeping two process-manager abilities (`procmgr_ability()`) | `src/qnx/privs.c`, `kw_drop_privileges()` |
| A send under a kernel timeout (`TimerTimeout()`) | `demo/wheel_sensor.c` |
| Native message passing, `name_attach()`/`name_open()` | `demo/brake_ctl.c`, `demo/wheel_sensor.c` |
| A mutex held across `MsgSend()` | `demo/motor_ctl.c`, `demo/logger.c` |

## Verification status

- Builds with QNX SDP 8.0 for x86_64 and for 64-bit Arm (`ntox86_64-gcc`,
  `ntoaarch64-gcc`) without warnings, and runs on QNX OS 8.0.0 on both, in
  QEMU. The demo scenarios and the on-target checks pass end to end on both:
  `scripts/qnx-image.sh` then `scripts/qemu-run.sh`.
- The portable core (snapshot format, analysis, contract, renderers, CLI) is
  tested on Linux with `make check`, also clean under ASan and UBSan.
- An MCP server (`agent/knotwatch_mcp.py`) lets AI coding assistants query
  the reports. It is tested on Linux, including against the official MCP
  client library.
- Not yet run on hardware.
  [docs/VERIFICATION.md](docs/VERIFICATION.md) lists what has been confirmed
  on a target and what has not.

## Repository layout

```
src/core/      portable core: snapshot format, analysis, contract, renderers
src/qnx/       QNX-only code: collector, resource manager, privilege drop
src/cli/       the knotwatch command line
include/kw/    headers of the portable core
demo/          four small processes with two planted design faults, and a contract
codelab/       the tutorial and its two programs
viewer/        the single-page report viewer and a real example report
agent/         the MCP server for AI assistants
tests/         unit tests, CLI tests, fixtures, and an on-target test program
scripts/       build, image and QEMU scripts (including the macOS container path)
docs/          architecture diagram, verification notes
```

## Limits

- knotwatch is a development, integration and CI tool. QNX documents `/proc`
  as a debugging interface and advises against using it to monitor production
  systems; knotwatch follows that advice.
- Condition-variable, semaphore and read-write-lock waits are not followed. A
  thread blocked on one is treated as alive, so such deadlocks are missed,
  never misreported.
- The owner of a mutex is decoded from the value QNX stores in it. That
  encoding was established by observation on QNX OS 8.0.0, is the same on
  x86_64 and aarch64le, and agrees with `pidin`; it is not a documented interface. When the owner cannot be
  determined with certainty the waiter is treated as alive, and the report
  says so.
- A `potential-cycle` is a hint. Two processes may hold connections to each
  other and only ever exchange pulses, which cannot deadlock. Review it, then
  `ignore` it in the contract.
- Processes are identified by executable name in the send graph, so several
  instances of one program are one node.
- Edges that start at the process manager (`procnto`) are left out of the
  hierarchy and contract checks. It is the microkernel, and it and the
  drivers hold connections to each other by design.

## Hackathon submission

| | |
| --- | --- |
| Category | Developer Tooling & Host Tools |
| QNX version | QNX OS 8.0.0, built with QNX SDP 8.0 (build 141) |
| Targets | x86_64 and aarch64le, both run in QEMU (`mkqnximage --type=qemu`) |
| Toolchain | `ntox86_64-gcc` and `ntoaarch64-gcc` from the SDP's Linux host tools, GNU make |
| Demo video | https://youtu.be/IeGtVh0GBWg |
| Codelab | [codelab/README.md](codelab/README.md) |
| Proof of QNX use | [Where QNX is used](#where-qnx-is-used), with files and functions |

## License

MIT, see [LICENSE](LICENSE).
