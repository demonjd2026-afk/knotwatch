#!/bin/sh
# Build a bootable QNX OS 8.0 QEMU image that contains knotwatch and the
# demo, and runs the demo scenario at the end of boot.
#
#   scripts/qnx-image.sh          # from macOS: builds, then makes build/vm
#   scripts/qnx-image.sh verify   # same, plus extra on-target checks at boot
#   scripts/qnx-image.sh verify2  # a second set of on-target checks only
#   scripts/qnx-image.sh plain    # nothing runs at boot; drive it yourself
#                                 # with scripts/qemu-run.sh console
#   ARCH=aarch64 scripts/qnx-image.sh [mode]   # 64-bit Arm instead of x86_64
#   SSH_KEY=path/to/key.pub scripts/qnx-image.sh [mode]   # allow root ssh with this key
#                                 # (the file must be inside this repository)
#
# Output: build/vm/output/ifs.bin and build/vm/output/disk-qemu.vmdk
# (build/vm-aarch64 for ARCH=aarch64). Boot it with scripts/qemu-run.sh,
# giving the same ARCH.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCH="${ARCH:-x86_64}"
case "$ARCH" in
    x86_64)  QNX_ARCH=x86_64;    VM="$REPO/build/vm" ;;
    aarch64) QNX_ARCH=aarch64le; VM="$REPO/build/vm-aarch64" ;;
    *) echo "error: ARCH must be x86_64 or aarch64" >&2; exit 1 ;;
esac

if [ "${1:-}" != "--inside" ]; then
    ARCH="$ARCH" sh "$REPO/scripts/qnx-docker-build.sh"
    exec sh "$REPO/scripts/qnx-docker.sh" env ARCH="$ARCH" SSH_KEY="${SSH_KEY:-}" \
        sh "$REPO/scripts/qnx-image.sh" --inside "${1:-demo}"
fi
MODE="${2:-demo}"

# From here on we are inside the container with the SDP environment set.
BIN="$REPO/build/$ARCH"
mkdir -p "$VM/local/snippets"

# The codelab's two programs, built the way the codelab tells its reader to,
# and the on-target test program.
mkdir -p "$BIN/codelab" "$BIN/target"
for prog in standoff whoblocks; do
    "nto$ARCH-gcc" -Wall -Wextra -O2 -o "$BIN/codelab/$prog" "$REPO/codelab/$prog.c"
done
"nto$ARCH-gcc" -Wall -Wextra -O2 -o "$BIN/target/kwt_locks" "$REPO/tests/target/kwt_locks.c"

# Files added to the image, under /system.
cat > "$VM/local/snippets/system_files.custom" <<FILES
[perms=555] bin/knotwatch=$BIN/knotwatch
[perms=555] bin/brake_ctl=$BIN/brake_ctl
[perms=555] bin/wheel_sensor=$BIN/wheel_sensor
[perms=555] bin/motor_ctl=$BIN/motor_ctl
[perms=555] bin/logger=$BIN/logger
[perms=555] bin/standoff=$BIN/codelab/standoff
[perms=555] bin/whoblocks=$BIN/codelab/whoblocks
[perms=555] bin/kwt_locks=$BIN/target/kwt_locks
[perms=444] etc/knotwatch/demo.contract=$REPO/demo/demo.contract
FILES

# Commands run at the end of boot: the demo scenario. Lines that start with
# "kwdemo:" are what scripts/qemu-run.sh looks for.
cat > "$VM/local/snippets/post_start.custom" <<'BOOT'
echo "kwdemo: booted $(uname -srm)"
/system/bin/knotwatch watch --contract /system/etc/knotwatch/demo.contract &
waitfor /dev/knotwatch/report 30
/system/bin/wheel_sensor --fault-after 100 &
sleep 1
/system/bin/brake_ctl &
sleep 6
echo "kwdemo: ---- report while the system still works ----"
cat /dev/knotwatch/report
echo "kwdemo: waiting for the sensor fault"
sleep 25
echo "kwdemo: ---- report after the fault ----"
cat /dev/knotwatch/report
/system/bin/knotwatch scan > /dev/null
echo "kwdemo: scan exit status $?"
pidin -p brake_ctl
pidin -p wheel_sensor
BOOT

# Extra checks of the pieces the demo itself does not exercise.
if [ "$MODE" = "verify" ]; then
    cat >> "$VM/local/snippets/post_start.custom" <<'BOOT'
echo "kwdemo: ---- verify: a lock held across a send ----"
/system/bin/logger --fault-after 30 &
sleep 1
/system/bin/motor_ctl &
sleep 15
cat /dev/knotwatch/report
pidin -p motor_ctl
pidin -p logger
KNOTWATCH_DEBUG_LOCKS=1 /system/bin/knotwatch capture 2>&1 > /dev/null | grep lock-debug | head -8
echo "kwdemo: ---- verify: codelab ----"
/system/bin/standoff &
sleep 4
pidin -p standoff
/system/bin/whoblocks
echo "kwdemo: whoblocks exit status $?"
echo "kwdemo: ---- verify: published files ----"
ls -l /dev/knotwatch
for f in report json dot snapshot; do
    echo "kwdemo: $f is $(cat /dev/knotwatch/$f | wc -c) bytes"
done
echo "kwdemo: ---- verify: json (first 300 bytes) ----"
cat /dev/knotwatch/json | cut -c1-300
echo "kwdemo: ---- verify: dot (first 8 lines) ----"
cat /dev/knotwatch/dot | head -8
echo "kwdemo: ---- verify: snapshot replays through analyze ----"
cat /dev/knotwatch/snapshot > /dev/shmem/kw.kws
/system/bin/knotwatch analyze /dev/shmem/kw.kws > /dev/null
echo "kwdemo: analyze of published snapshot exit status $?"
echo "kwdemo: ---- verify: capture against pidin ----"
/system/bin/knotwatch capture > /dev/shmem/kw_capture.kws
echo "kwdemo: capture threads $(grep -c '^thr ' /dev/shmem/kw_capture.kws), pidin lines $(pidin | wc -l) (one header line)"
echo "kwdemo: capture threads by state:"
sed -n 's/^thr .* state=\([A-Z0-9]*\) .*/\1/p' /dev/shmem/kw_capture.kws | sort | uniq -c
echo "kwdemo: threads with the timeout field set: $(grep -c 'timeout=1' /dev/shmem/kw_capture.kws)"
grep 'timeout=1' /dev/shmem/kw_capture.kws | head -5
echo "kwdemo: ---- verify: a reader that stays open across scans ----"
(sleep 4; cat) < /dev/knotwatch/report | wc -c
echo "kwdemo: ---- verify: slog2 ----"
slog2info -b knotwatch | tail -6
echo "kwdemo: ---- verify: scan as an unprivileged user ----"
on -u 1000:1000 /system/bin/knotwatch scan 2>&1 | head -6
echo "kwdemo: unprivileged scan finished"
BOOT
fi

# A second boot's worth of checks, on a fresh system: cases that the demo and
# `verify` do not reach.
if [ "$MODE" = "verify2" ]; then
    cat > "$VM/local/snippets/post_start.custom" <<'BOOT'
echo "kwdemo: booted $(uname -srm)"
/system/bin/knotwatch watch --contract /system/etc/knotwatch/demo.contract &
waitfor /dev/knotwatch/report 30
echo "kwdemo: ---- verify2: a send under a kernel timeout ----"
/system/bin/wheel_sensor --fault-after 50 --send-timeout-ms 15000 &
sleep 1
/system/bin/brake_ctl &
sleep 10
echo "kwdemo: during the timeout (expected: no deadlock):"
cat /dev/knotwatch/report
/system/bin/knotwatch capture > /dev/shmem/t.kws
echo "kwdemo: SEND- and REPLY-blocked threads in a capture:"
grep -E 'state=(SEND|REPLY) ' /dev/shmem/t.kws
pidin -p wheel_sensor
sleep 12
echo "kwdemo: after the timeout (expected: the sensor carried on, no deadlock):"
cat /dev/knotwatch/report
pidin -p wheel_sensor
echo "kwdemo: ---- verify2: eight readers at once ----"
for i in 1 2 3 4 5 6 7 8; do
    cat /dev/knotwatch/report > /dev/shmem/r$i &
    cat /dev/knotwatch/json > /dev/shmem/j$i &
done
sleep 4
for i in 1 2 3 4 5 6 7 8; do
    echo "kwdemo: reader $i: report $(wc -c < /dev/shmem/r$i) bytes, starts '$(head -1 /dev/shmem/r$i)', json $(wc -c < /dev/shmem/j$i) bytes, ends '$(tail -c 4 /dev/shmem/j$i)'"
done
echo "kwdemo: ---- verify2: locks held by threads other than thread 1 ----"
/system/bin/kwt_locks threads &
sleep 5
cat /dev/knotwatch/report | grep -A5 'deadlock'
pidin -p kwt_locks
echo "kwdemo: ---- verify2: process-shared locks held by another process ----"
/system/bin/kwt_locks shared &
sleep 5
cat /dev/knotwatch/report | grep -A5 'deadlock'
pidin -p kwt_locks
KNOTWATCH_DEBUG_LOCKS=1 /system/bin/knotwatch capture 2>&1 > /dev/null | grep lock-debug | head -12
echo "kwdemo: ---- verify2: scan after giving up root ----"
/system/bin/knotwatch scan > /dev/shmem/root.txt 2> /dev/shmem/root.err
echo "kwdemo: as root: exit status $?"
head -3 /dev/shmem/root.txt
/system/bin/knotwatch scan --user 1000:1000 > /dev/shmem/user.txt 2> /dev/shmem/user.err
echo "kwdemo: with --user 1000:1000: exit status $?"
head -3 /dev/shmem/user.txt
cat /dev/shmem/user.err
echo "kwdemo: deadlocks seen as root: $(grep -c '^\[CRITICAL\]' /dev/shmem/root.txt), as user 1000: $(grep -c '^\[CRITICAL\]' /dev/shmem/user.txt)"
echo "kwdemo: ---- verify2: watch after giving up root ----"
/system/bin/knotwatch watch --mount /dev/knotwatch2 --user 1000:1000 &
waitfor /dev/knotwatch2/report 30
sleep 3
ls -l /dev/knotwatch2
head -3 /dev/knotwatch2/report
echo "kwdemo: deadlocks in the unprivileged watcher's report: $(grep -c '^\[CRITICAL\]' /dev/knotwatch2/report)"
BOOT
fi
echo 'echo "kwdemo: done"' >> "$VM/local/snippets/post_start.custom"
if [ "$MODE" = "plain" ]; then
    : > "$VM/local/snippets/post_start.custom"
fi

cd "$VM"
mkqnximage --type=qemu --arch="$QNX_ARCH" --noprompt --ssh-ident="${SSH_KEY:-none}" \
    --hostname=knotwatch --build
echo "image ready: $VM/output"
