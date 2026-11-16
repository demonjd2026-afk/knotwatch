#!/bin/sh
# Build the codelab's two programs and put them in a bootable QNX OS 8.0
# image for QEMU (x86_64). Run it with the QNX SDP 8.0 environment set:
#
#   Linux:  . ~/qnx800/qnxsdp-env.sh && sh codelab/make-image.sh
#   macOS:  sh scripts/qnx-docker.sh sh codelab/make-image.sh
#
# Output: codelab/vm/output/ifs.bin and codelab/vm/output/disk-qemu.vmdk.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VM="$HERE/vm"
mkdir -p "$VM/bin" "$VM/local/snippets"

ntox86_64-gcc -Wall -Wextra -O2 -o "$VM/bin/standoff" "$HERE/standoff.c"
ntox86_64-gcc -Wall -Wextra -O2 -o "$VM/bin/whoblocks" "$HERE/whoblocks.c"

# mkqnximage copies every file listed here into the image, under /system.
cat > "$VM/local/snippets/system_files.custom" <<FILES
[perms=555] bin/standoff=$VM/bin/standoff
[perms=555] bin/whoblocks=$VM/bin/whoblocks
FILES

cd "$VM"
mkqnximage --type=qemu --arch=x86_64 --noprompt --ssh-ident=none \
    --hostname=codelab --build
echo "image ready: $VM/output"
