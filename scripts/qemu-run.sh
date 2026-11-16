#!/bin/sh
# Boot the image made by scripts/qnx-image.sh in QEMU on this machine and show
# what the demo printed on the serial console.
#
#   scripts/qemu-run.sh            # run the demo, print its output, exit
#   scripts/qemu-run.sh console    # interactive serial console (Ctrl-A X quits)
#
# Needs qemu-system-x86_64 (macOS: brew install qemu). On Apple Silicon the
# guest is emulated, so booting takes a minute or two.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
VM="$REPO/build/vm"
LOG="$VM/output/console.log"
TIMEOUT="${TIMEOUT:-600}"

if [ ! -f "$VM/output/ifs.bin" ]; then
    echo "error: no image in $VM/output; run scripts/qnx-image.sh first" >&2
    exit 1
fi
cd "$VM"

run_qemu() {
    qemu-system-x86_64 -smp 2 -cpu max -m 1G -no-reboot \
        -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -kernel output/ifs.bin \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 "$@"
}

if [ "${1:-}" = "console" ]; then
    run_qemu -nographic
    exit
fi

: > "$LOG"
run_qemu -display none -monitor none -serial "file:$LOG" </dev/null &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT

echo "booting QNX in QEMU (up to ${TIMEOUT}s); full console in $LOG"
waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
    if grep -q "kwdemo: done" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "error: QEMU exited early" >&2
        break
    fi
    sleep 1
    waited=$((waited + 1))
done
sleep 2

# Everything from the first demo line onwards.
sed -n '/kwdemo: booted/,$p' "$LOG"
if grep -q "kwdemo: done" "$LOG"; then
    exit 0
fi
echo "error: the demo did not finish; see $LOG" >&2
exit 1
