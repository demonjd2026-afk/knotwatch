#!/bin/sh
# Build a bootable QNX OS 8.0 x86_64 QEMU image that contains knotwatch and
# the demo, and runs the demo scenario at the end of boot.
#
#   scripts/qnx-image.sh          # from macOS: builds, then makes build/vm
#
# Output: build/vm/output/ifs.bin and build/vm/output/disk-qemu.vmdk.
# Boot it with scripts/qemu-run.sh.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)

if [ "${1:-}" != "--inside" ]; then
    sh "$REPO/scripts/qnx-docker-build.sh"
    exec sh "$REPO/scripts/qnx-docker.sh" sh "$REPO/scripts/qnx-image.sh" --inside
fi

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
echo "kwdemo: done"
BOOT

cd "$VM"
mkqnximage --type=qemu --arch=x86_64 --noprompt --ssh-ident=none \
    --hostname=knotwatch --build
echo "image ready: $VM/output"
