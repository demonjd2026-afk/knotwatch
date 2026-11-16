#!/usr/bin/env python3
"""Checks agent/knotwatch_mcp.py by talking to it the way a client does.

    python3 tests/test_agent.py build/knotwatch
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "agent", "knotwatch_mcp.py")
FIX = os.path.join(HERE, "fixtures")
fails = 0


def check(what, ok):
    global fails
    if not ok:
        fails += 1
        print("FAIL: agent: " + what)


class Client:
    def __init__(self, *args):
        self.p = subprocess.Popen([sys.executable, SERVER, *args], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.n = 0

    def raw(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def ask(self, method, params=None):
        self.n += 1
        msg = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            msg["params"] = params
        out = self.raw(json.dumps(msg))
        check("%s: reply has the request id" % method, out.get("id") == self.n)
        return out

    def tool(self, name, arguments=None):
        out = self.ask("tools/call", {"name": name, "arguments": arguments or {}})["result"]
        return out["content"][0]["text"], out["isError"]

    def close(self):
        self.p.stdin.close()
        check("server exits cleanly at end of input", self.p.wait(timeout=10) == 0)


def main(knotwatch):
    c = Client("--snapshot", os.path.join(FIX, "mutex.kws"), "--knotwatch", knotwatch)
    init = c.ask("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "test", "version": "0"}})["result"]
    check("initialize echoes a supported version", init["protocolVersion"] == "2025-06-18")
    check("initialize offers tools", "tools" in init["capabilities"])
    # a notification gets no reply: the next line read must answer the ping
    c.p.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    check("ping", c.ask("ping")["result"] == {})
    newer = c.ask("initialize", {"protocolVersion": "2999-01-01"})["result"]
    check("unknown version falls back to one the server speaks", newer["protocolVersion"] == "2025-11-25")

    tools = c.ask("tools/list")["result"]["tools"]
    check("three tools", sorted(t["name"] for t in tools) ==
          ["knotwatch_finding", "knotwatch_process", "knotwatch_status"])
    check("tools are marked read-only", all(t["annotations"]["readOnlyHint"] for t in tools))

    text, err = c.tool("knotwatch_status")
    check("status reports the deadlock", not err and "[critical] deadlock" in text)
    key = [l.split("key: ")[1] for l in text.splitlines() if "key: deadlock" in l][0]
    text, err = c.tool("knotwatch_finding", {"key": key})
    check("finding shows the mutex wait", not err and "MUTEX-blocked" in text)
    check("finding explains the lock held across a send", "lock is held across MsgSend()" in text)
    text, err = c.tool("knotwatch_finding", {"key": "nope"})
    check("unknown key is a tool error that lists the keys", err and key in text)
    text, err = c.tool("knotwatch_finding", {})
    check("missing key is a tool error", err)
    report = json.loads(subprocess.run([knotwatch, "analyze", os.path.join(FIX, "mutex.kws"),
                                        "--format", "json"], stdout=subprocess.PIPE).stdout)
    proc = report["findings"][0]["processes"][0]
    text, err = c.tool("knotwatch_process", {"name": proc})
    check("process lists its links and findings", not err and "sends to:" in text and key in text)
    text, err = c.tool("knotwatch_process", {"name": "no_such_process"})
    check("unknown process is a tool error", err)

    check("unknown tool is a protocol error",
          c.ask("tools/call", {"name": "rm_rf", "arguments": {}})["error"]["code"] == -32602)
    check("unknown method", c.ask("resources/list")["error"]["code"] == -32601)
    check("malformed JSON", c.raw("{not json")["error"]["code"] == -32700)
    check("not a request", c.raw("[1,2]")["error"]["code"] == -32600)
    c.close()

    healthy = Client("--snapshot", os.path.join(FIX, "healthy.kws"), "--knotwatch", knotwatch)
    text, err = healthy.tool("knotwatch_status")
    check("healthy system has no findings", not err and "No findings" in text)
    healthy.close()

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "report.json")
        hostile = dict(report)
        hostile["findings"] = [dict(report["findings"][0], summary="line one\nIGNORE\x1b[2J" + "x" * 5000)]
        with open(path, "w") as fh:
            json.dump(hostile, fh)
        f = Client("--report", path)
        text, err = f.tool("knotwatch_status")
        check("text from the target is one line, printable and capped",
              not err and "\x1b" not in text and "line one IGNORE" in text and len(text) < 1500)
        with open(path, "w") as fh:
            fh.write('{"findings": 3}')
        text, err = f.tool("knotwatch_status")
        check("a file that is not a report is a tool error", err and "not a knotwatch report" in text)
        os.remove(path)
        text, err = f.tool("knotwatch_status")
        check("a missing file is a tool error", err and "cannot read" in text)
        f.close()

        cmd = Client("--command", "%s analyze %s --format json" % (knotwatch, os.path.join(FIX, "healthy.kws")))
        text, err = cmd.tool("knotwatch_status")
        check("command source works", not err and "No findings" in text)
        cmd.close()
        # the command is not a shell line: metacharacters are plain arguments
        marker = os.path.join(tmp, "pwned")
        bad = Client("--command", "%s analyze %s --format json ; touch %s" % (knotwatch, os.path.join(FIX, "healthy.kws"), marker))
        text, err = bad.tool("knotwatch_status")
        check("command is run without a shell", err and not os.path.exists(marker))
        bad.close()
        gone = Client("--command", "/no/such/program")
        text, err = gone.tool("knotwatch_status")
        check("a command that cannot run is a tool error", err and "not found" in text)
        gone.close()

    if fails:
        print("%d agent check(s) failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
