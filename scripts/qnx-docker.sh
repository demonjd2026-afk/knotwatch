#!/bin/sh
# Run a command with the QNX SDP 8.0 environment, inside a small x86_64 Linux
# container. For hosts that cannot run the SDP tools natively (macOS, Apple
# Silicon). Only the SDP, the licence folder and this repository are visible
# to the container, and it runs as the calling user.
#
#   scripts/qnx-docker.sh <command> [args...]
#
# Needs: Docker, QNX SDP 8.0 in ~/qnx800 (or $QNX_SDP) with the package
# "QNX SDP Host Tools (Linux 64-bit)" installed, and an activated licence.
set -eu

SDP="${QNX_SDP:-$HOME/qnx800}"
IMAGE=knotwatch-qnx-build:2
REPO=$(cd "$(dirname "$0")/.." && pwd)

if [ ! -f "$SDP/qnxsdp-env.sh" ]; then
    echo "error: no QNX SDP at $SDP (set QNX_SDP to its location)" >&2
    exit 1
fi
if [ ! -d "$SDP/host/linux/x86_64" ]; then
    echo "error: $SDP has no Linux host tools; install 'QNX SDP Host Tools" \
         "(Linux 64-bit)' with QNX Software Center" >&2
    exit 1
fi
if [ ! -d "$HOME/.qnx" ]; then
    echo "error: $HOME/.qnx not found; activate your licence in QNX Software" \
         "Center first" >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$IMAGE" - <<'DOCKERFILE'
FROM ubuntu:22.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends make file \
 && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi

exec docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    -e HOME="$HOME" -e KW_SDP="$SDP" \
    -v "$SDP:$SDP:ro" \
    -v "$HOME/.qnx:$HOME/.qnx" \
    -v "$REPO:$REPO" -w "$REPO" \
    "$IMAGE" bash -c '. "$KW_SDP/qnxsdp-env.sh" >/dev/null && exec "$@"' _ "$@"
