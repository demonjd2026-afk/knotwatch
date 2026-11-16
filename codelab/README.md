# Codelab: find a message-passing deadlock on QNX OS 8.0 by reading /proc

Two processes on a QNX system have stopped responding. Nothing crashed and
nothing is using CPU. In this codelab you create that situation on purpose,
look at it with `pidin`, and then write a small tool, `whoblocks`, that asks
the process manager what every thread is waiting for and tells you when
processes are waiting on each other.

**You will learn**

- what the SEND, RECEIVE and REPLY thread states mean,
- how to read a thread's state and what it is blocked on from `/proc`,
- how to turn "who waits for whom" into a cycle check,
- how to get your own programs into a QNX image and run them in QEMU.

**You will need**

- QNX SDP 8.0, free for non-commercial use through
  [QNX Everywhere](https://qnx.software/en/developers/get-started/qnx-everywhere),
- QEMU (`qemu-system-x86_64`),
- on macOS only: Docker, because the SDP's build tools are Linux programs.

Time: about 45 minutes, most of it reading.

Everything here was run on QNX OS 8.0.0 (SDP 8.0 build 141), x86_64, in QEMU,
from an Apple Silicon Mac. The output blocks are copied from that run; your
process ids will differ. The Linux commands are the same ones without the
Docker wrapper, and I have not run them on a Linux host myself.

## 1. Three states to know (5 min)

QNX processes talk by sending messages, and a send is synchronous: the sender
stops until it gets a reply. A thread involved in that is in one of three
states.

| State | The thread has | It is waiting for |
| --- | --- | --- |
| RECEIVE | called `MsgReceive()` | a message to arrive |
| SEND | called `MsgSend()` | the server to receive the message |
| REPLY | called `MsgSend()`, and the server has received it | the server to reply |

Being blocked is normal. A shell waiting for you to type is REPLY-blocked on
the console driver. What is not normal is two processes that are each waiting
for the other. QNX's own advice for avoiding that is to arrange processes in
a hierarchy and only ever send one way along it.

## 2. Set up (10 min, plus download time)

1. Get a QNX SDP 8.0 licence and install the SDP with QNX Software Center.
   On macOS, install the package "QNX SDP Host Tools (Linux 64-bit)" as well.
   The SDP is expected in `~/qnx800`.
2. Install QEMU. On macOS: `brew install qemu`.
3. Get this repository and go to its top directory.

The SDP provides two things this codelab uses: the cross compiler
`ntox86_64-gcc`, and `mkqnximage`, which builds a bootable QNX image.

On Linux, load the SDP environment into your shell before using them:

```sh
. ~/qnx800/qnxsdp-env.sh
```

On macOS they cannot run directly, so this repository has a wrapper,
`scripts/qnx-docker.sh`, that runs a command inside a small Linux container
with the SDP environment already loaded.

## 3. Write a deadlock (5 min)

[standoff.c](standoff.c) is about 45 lines. It splits into two processes with
`fork()`. Each one creates a channel, connects to the other's channel, and
sends:

```c
int coid = ConnectAttach(0, child, child_chid, _NTO_SIDE_CHANNEL, 0);
...
MsgSend(coid, "hello", 6, NULL, 0); /* the child never receives this */
```

Neither process ever calls `MsgReceive()`, so neither message is ever
received. Both threads stay SEND-blocked for as long as the system runs.

Real deadlocks are rarely this obvious. Usually one direction is the normal
request path and the other is a rare callback. The state the system ends up
in is the same.

## 4. Build an image and boot it (10 min)

[make-image.sh](make-image.sh) does three things.

It cross-compiles the two programs:

```sh
ntox86_64-gcc -Wall -Wextra -O2 -o "$VM/bin/standoff" "$HERE/standoff.c"
ntox86_64-gcc -Wall -Wextra -O2 -o "$VM/bin/whoblocks" "$HERE/whoblocks.c"
```

It tells `mkqnximage` to copy them into the image. Each line of
`local/snippets/system_files.custom` names a file to add under `/system`:

```
[perms=555] bin/standoff=/path/to/codelab/vm/bin/standoff
[perms=555] bin/whoblocks=/path/to/codelab/vm/bin/whoblocks
```

And it builds the image:

```sh
mkqnximage --type=qemu --arch=x86_64 --noprompt --ssh-ident=none \
    --hostname=codelab --build
```

Run it.

```sh
# Linux
sh codelab/make-image.sh

# macOS
sh scripts/qnx-docker.sh sh codelab/make-image.sh
```

It ends with `image ready:` and leaves `ifs.bin` (the boot image) and
`disk-qemu.vmdk` (the disk) in `codelab/vm/output`.

Now boot it. [boot.sh](boot.sh) starts QEMU with the QNX console in your
terminal:

```sh
sh codelab/boot.sh
```

On an Apple Silicon Mac the x86_64 guest is fully emulated, so allow a minute.
The boot ends like this, and you are at a root shell with no login:

```
Process count:22
Startup complete
QNX codelab 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64

#
```

If the prompt does not appear, press Enter. To leave QEMU later, press Ctrl-A
and then X.

## 5. Look at a healthy system (2 min)

Run the tool you are about to study, before anything is wrong:

```
# whoblocks
whoblocks[339970] thread 1 is REPLY-blocked on procnto-smp-instr[1]
```

One thread is blocked, and it is `whoblocks` itself: at the instant it asks
the process manager (`procnto`) about its own thread, it is waiting for the
answer. No cycle is reported.

## 6. Cause the deadlock (3 min)

Start `standoff` in the background and ask `pidin`, QNX's process viewer,
about it:

```
# standoff &
[1] 348162
# standoff: 348162 and 352274 are about to send to each other

# pidin -p standoff
     pid tid name                         prio STATE          Blocked
  348162   1 system/bin/standoff           10r SEND           352274
  352274   1 system/bin/standoff           10r SEND           348162
```

Read the last two columns: process 348162 is SEND-blocked on 352274, and
352274 is SEND-blocked on 348162. With two processes you can see the cycle by
eye. With forty you want a program to find it.

## 7. How whoblocks finds it (10 min)

[whoblocks.c](whoblocks.c) is about 135 lines. It has four parts.

**Every process is a directory under `/proc`.** The tool walks it and keeps
the entries whose names are numbers:

```c
DIR *proc = opendir("/proc");
...
while ((entry = readdir(proc)) != NULL) {
    if (isdigit((unsigned char)entry->d_name[0])) {
        scan_process((pid_t)atoi(entry->d_name));
    }
}
```

**`/proc/<pid>/ctl` answers questions about a process.** You open it and send
it `devctl()` commands. `DCMD_PROC_TIDSTATUS` fills in a `procfs_status` for
the first thread whose id is at least the one you ask for, so counting up
from 1 visits every thread:

```c
for (status.tid = 1;
     devctl(fd, DCMD_PROC_TIDSTATUS, &status, sizeof(status), NULL) == EOK;
     status.tid++) {
```

The file is opened read-only. That is enough for asking, and it does not get
in the way of a debugger, which needs the one writable handle.

**The status says what the thread is blocked on.** For a SEND or REPLY-blocked
thread, `status.blocked.connect.pid` is the process it is waiting for:

```c
if (status.state != STATE_SEND && status.state != STATE_REPLY) {
    continue;
}
if (status.blocked.connect.pid == pid) {
    continue; /* waiting on a thread of its own process */
}
```

The second test matters. On this image a thread inside `procnto` waits on
another thread of `procnto`. Without the test the tool reports a cycle of one
process with itself, which is how the first version of this program was
wrong.

**A cycle is a walk that comes back.** For each waiting process the tool
follows "is waiting for" from process to process. If it arrives back where it
started, those processes are waiting on each other:

```c
pid_t at = waits_for(start);
for (int steps = 0; at != 0 && at != start && steps < n_waits; steps++) {
    lowest = lowest && at > start;
    at = waits_for(at);
}
```

`lowest` makes sure each cycle is printed once, starting from its smallest
process id, instead of once per member.

Process names come from `/proc/<pid>/exefile`, which holds the path of the
program the process is running.

## 8. Run it on the deadlock (1 min)

```
# whoblocks
standoff[348162] thread 1 is SEND-blocked on standoff[352274]
standoff[352274] thread 1 is SEND-blocked on standoff[348162]
whoblocks[360468] thread 1 is REPLY-blocked on procnto-smp-instr[1]
cycle: standoff[348162] -> standoff[352274] -> standoff[348162]
```

The two `standoff` lines match what `pidin` showed, and the last line states
the conclusion. `whoblocks` exits with status 2 when it finds a cycle and 0
when it does not, so a script can act on the answer.

## What whoblocks does not do

It is a teaching tool, and it takes shortcuts that a real system breaks.

- It follows only the first blocked thread of each process. A server with a
  pool of threads may still have one free to reply, so a cycle between two
  such processes is not necessarily a deadlock.
- It ignores locks. A thread that holds a mutex while it waits for a reply
  can deadlock a system without any cycle of messages.
- It reports only what is stuck right now. A process that holds a connection
  back to its own client is a deadlock waiting for the wrong moment, and
  nothing is blocked yet.

[knotwatch](../README.md), the project this codelab ships with, handles those
cases. It starts from the same two `/proc` calls you just read.

To take the small tool further yourself:

1. Print how long each thread has been blocked. `procfs_status` has a field
   for it, `nsec_since_block`.
2. Report threads in `STATE_MUTEX` as well. `status.blocked.sync.sync` is the
   address of the mutex.
3. List what each process is connected to, blocked or not, with
   `ConnectServerInfo()`.

## If something goes wrong

- **`'EOK' undeclared`** when compiling: include `<errno.h>`. That is where
  QNX defines it.
- **`Failed to get "write" lock`** from QEMU: another QEMU is still running
  with the same disk image. Quit it, or run `pkill -f qemu-system-x86_64`.
- **`whoblocks` prints nothing useful as an ordinary user:** it has to run as
  root. Without privileges a process may not open another process's `ctl`
  file.
- **`error: no QNX SDP at ...`** from the macOS wrapper: the SDP is not in
  `~/qnx800`. Set `QNX_SDP` to where it is installed.
