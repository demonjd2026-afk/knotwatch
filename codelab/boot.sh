#!/bin/sh
# Boot the image made by make-image.sh in QEMU, with the QNX console in this
# terminal. Quit with Ctrl-A, then X.
#
#   sh codelab/boot.sh
#
# -snapshot leaves the image untouched, so every boot starts clean.
set -eu
cd "$(dirname "$0")/vm"
exec qemu-system-x86_64 -smp 2 -cpu max -m 1G -no-reboot -snapshot -nographic \
    -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
    -kernel output/ifs.bin \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-pci,rng=rng0
