#!/usr/bin/env python3
"""Drives agent/knotwatch_mcp.py against a running QNX target over ssh.
Run by scripts/agent-check.sh once the demo in the guest has deadlocked.

    python3 tests/agent_live.py <private-key> <port>
"""
import os
import shlex
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_agent import Client          # the same client the unit test uses


def main(key, port):
    ssh = ["ssh", "-i", key, "-p", port, "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
           "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=10",
           "root@127.0.0.1"]
    print("agent-check: waiting for ssh on port %s" % port)
    for attempt in range(30):
        done = subprocess.run(ssh + ["uname", "-srm"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if done.returncode == 0:
            print("agent-check: target is", done.stdout.strip())
            break
        time.sleep(4)
    else:
        print("agent-check: FAIL: no ssh connection:", done.stderr.strip())
        return 1

    command = " ".join(shlex.quote(a) for a in ssh + ["cat", "/dev/knotwatch/json"])
    c = Client("--command", command)
    init = c.ask("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "agent-check", "version": "0"}})["result"]
    print("agent-check: server", init["serverInfo"]["name"], init["serverInfo"]["version"])
    status, err = c.tool("knotwatch_status")
    print("---- knotwatch_status ----")
    print(status)
    ok = not err and "[critical] deadlock" in status and "wheel_sensor" in status
    keys = [l.split("key: ")[1] for l in status.splitlines() if "key: deadlock" in l]
    if keys:
        finding, err2 = c.tool("knotwatch_finding", {"key": keys[0]})
        print("---- knotwatch_finding ----")
        print(finding)
        ok = ok and not err2 and "SEND-blocked" in finding and "Usual fixes on QNX" in finding
    process, err3 = c.tool("knotwatch_process", {"name": "brake_ctl"})
    print("---- knotwatch_process brake_ctl ----")
    print(process)
    ok = ok and not err3 and "wheel_sensor -> brake_ctl" in process
    c.close()
    print("agent-check: PASS" if ok else "agent-check: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
