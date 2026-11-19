#!/bin/sh
# End-to-end check of the MCP server against a live target.
#
# Builds the demo image with root ssh enabled for a throwaway key, boots it in
# QEMU with the guest's ssh port forwarded to this machine, waits for the
# demo's deadlock, and then asks knotwatch about it through
# agent/knotwatch_mcp.py over ssh, the way an AI assistant's MCP client does.
#
#   scripts/agent-check.sh
#
# Needs what scripts/qnx-image.sh and scripts/qemu-run.sh need, plus python3
# and ssh on this machine. The key lives in build/ssh and is used for nothing
# else.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
KEY="$REPO/build/ssh/id_ed25519"
PORT="${PORT:-2222}"

mkdir -p build/ssh
if [ ! -f "$KEY" ]; then
    ssh-keygen -q -t ed25519 -N '' -C knotwatch-agent-check -f "$KEY"
fi
SSH_KEY="$KEY.pub" sh scripts/qnx-image.sh demo
NET_PORT="$PORT" AFTER="python3 '$REPO/tests/agent_live.py' '$KEY' $PORT" sh scripts/qemu-run.sh
