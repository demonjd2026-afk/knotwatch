#!/bin/sh
# Cross-build knotwatch for QNX OS 8.0 from macOS (see qnx-docker.sh).
#
#   scripts/qnx-docker-build.sh              # x86_64  -> build/x86_64
#   ARCH=aarch64 scripts/qnx-docker-build.sh # aarch64 -> build/aarch64
#   scripts/qnx-docker-build.sh clean        # extra arguments go to make
set -eu
ARCH="${ARCH:-x86_64}"
exec sh "$(dirname "$0")/qnx-docker.sh" \
    make QNX=1 CC="nto$ARCH-gcc" BUILD="build/$ARCH" "$@"
