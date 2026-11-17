#!/bin/sh
# Boot the image made by scripts/qnx-image.sh in QEMU on this machine and show
# what the demo printed on the serial console.
#
#   scripts/qemu-run.sh            # run the demo, print its output, exit
#   scripts/qemu-run.sh console    # interactive serial console (Ctrl-A X quits)
#
#   ARCH=aarch64 scripts/qemu-run.sh [console]   # the 64-bit Arm image
#
# Needs QEMU (macOS: brew install qemu). The guest is emulated in software,
# so booting takes a minute or two.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCH="${ARCH:-x86_64}"
case "$ARCH" in
    x86_64)  VM="$REPO/build/vm" ;;
    aarch64) VM="$REPO/build/vm-aarch64" ;;
    *) echo "error: ARCH must be x86_64 or aarch64" >&2; exit 1 ;;
esac
LOG="$VM/output/console.log"
TIMEOUT="${TIMEOUT:-600}"

if [ ! -f "$VM/output/ifs.bin" ]; then
    echo "error: no image in $VM/output; run scripts/qnx-image.sh first" \
         "(with the same ARCH)" >&2
    exit 1
fi
cd "$VM"

# -snapshot keeps the disk image untouched: every boot starts from the same
# state, and two boots can run side by side. exec makes the background job
# QEMU itself, so the cleanup below stops QEMU and not just a wrapper shell.
# The machine options are the ones QNX's own mkqnximage uses for each
# architecture, without networking. For aarch64 it has three presets
# (ARMv8-A, ARMv8.2-A, ARMv9.0-A); recent QEMU releases no longer have the
# machine type of the first, so take the first preset this QEMU offers.
aarch64_preset() {
    machines=$(qemu-system-aarch64 -machine help)
    for preset in "virt-4.2 cortex-a57" "virt-7.2 cortex-a76" "virt-8.2 cortex-a710"; do
        if echo "$machines" | grep -q "^${preset%% *} "; then
            echo "$preset"
            return
        fi
    done
    echo "error: this QEMU has none of the machine types virt-4.2, virt-7.2," \
         "virt-8.2 that QNX's image tool supports" >&2
    exit 1
}

run_qemu() {
    if [ "$ARCH" = "aarch64" ]; then
        exec qemu-system-aarch64 -machine "$A64_MACHINE" -cpu "$A64_CPU" -smp 2 \
            -m 1G -no-reboot -snapshot \
            -drive file=output/disk-qemu.vmdk,if=none,id=drv0 \
            -device virtio-blk-device,drive=drv0 \
            -kernel output/ifs.bin \
            -object rng-random,filename=/dev/urandom,id=rng0 \
            -device virtio-rng-device,rng=rng0 "$@"
    fi
    exec qemu-system-x86_64 -smp 2 -cpu max -m 1G -no-reboot -snapshot \
        -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -kernel output/ifs.bin \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 "$@"
}

if [ "$ARCH" = "aarch64" ]; then
    preset=$(aarch64_preset)
    A64_MACHINE=${preset%% *}
    A64_CPU=${preset##* }
    echo "QEMU machine $A64_MACHINE, cpu $A64_CPU"
fi

if [ "${1:-}" = "console" ]; then
    run_qemu -nographic
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
