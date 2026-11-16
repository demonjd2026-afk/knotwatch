#!/bin/sh
# Build a bootable QNX OS 8.0 x86_64 QEMU image that contains knotwatch and
# the demo, and runs the demo scenario at the end of boot.
#
#   scripts/qnx-image.sh          # from macOS: builds, then makes build/vm
#   scripts/qnx-image.sh verify   # same, plus extra on-target checks at boot
#
# Output: build/vm/output/ifs.bin and build/vm/output/disk-qemu.vmdk.
# Boot it with scripts/qemu-run.sh.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)

if [ "${1:-}" != "--inside" ]; then
    sh "$REPO/scripts/qnx-docker-build.sh"
    exec sh "$REPO/scripts/qnx-docker.sh" sh "$REPO/scripts/qnx-image.sh" \
        --inside "${1:-demo}"
fi
MODE="${2:-demo}"

# From here on we are inside the container with the SDP environment set.
BIN="$REPO/build/x86_64"
VM="$REPO/build/vm"
mkdir -p "$VM/local/snippets"

# Files added to the image, under /system.
cat > "$VM/local/snippets/system_files.custom" <<FILES
[perms=555] bin/knotwatch=$BIN/knotwatch
[perms=555] bin/brake_ctl=$BIN/brake_ctl
[perms=555] bin/wheel_sensor=$BIN/wheel_sensor
[perms=444] etc/knotwatch/demo.contract=$REPO/demo/demo.contract
FILES

# Commands run at the end of boot: the demo scenario. Lines that start with
# "kwdemo:" are what scripts/qemu-run.sh looks for.
cat > "$VM/local/snippets/post_start.custom" <<'BOOT'
echo "kwdemo: booted $(uname -sr)"
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
echo 'echo "kwdemo: done"' >> "$VM/local/snippets/post_start.custom"

cd "$VM"
mkqnximage --type=qemu --arch=x86_64 --noprompt --ssh-ident=none \
    --hostname=knotwatch --build
echo "image ready: $VM/output"
