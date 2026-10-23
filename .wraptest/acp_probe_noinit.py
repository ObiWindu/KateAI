#!/usr/bin/env python3
import json, subprocess, sys, threading, queue, time

cmd = ["opencode", "acp"]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, bufsize=1)

def reader(stream, q, name):
    for line in stream:
        q.put((name, line.rstrip("\n")))

q = queue.Queue()
threading.Thread(target=reader, args=(proc.stdout, q, "stdout"), daemon=True).start()
threading.Thread(target=reader, args=(proc.stderr, q, "stderr"), daemon=True).start()

def send(obj):
    proc.stdin.write(json.dumps(obj) + "\n")
    proc.stdin.flush()

def wait_response(rid, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        try:
            src, line = q.get(timeout=1)
        except queue.Empty:
            continue
        if src == "stderr":
            print("[STDERR]", line[:200], file=sys.stderr)
            continue
        try:
            msg = json.loads(line)
        except Exception:
            print("[RAW]", line[:300], file=sys.stderr)
            continue
        if msg.get("id") == rid:
            return msg
        else:
            print("[OTHER]", json.dumps(msg)[:400], file=sys.stderr)
    return None

# Mirror Kate AI's initialize params
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": 1,
    "clientCapabilities": {
        "fs": {"readTextFile": True, "writeTextFile": True},
        "terminal": True,
        "kateai": {"queryProjectGraph": True, "writePlanFile": True, "suggest": True},
        "session": {"configOptions": {"boolean": {}}},
    },
    "clientInfo": {"name": "kateai", "title": "Kate AI", "version": "0.1.0"},
}})
init = wait_response(1)
print("=== INITIALIZE RESULT ===")
print(json.dumps(init, indent=2)[:6000])

pass
send({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {
    "cwd": "/home/macewindu/ai-dev/KateAI", "mcpServers": []}})
sn = wait_response(2, timeout=60)
print("=== SESSION/NEW RESULT ===")
print(json.dumps(sn, indent=2)[:12000])

proc.terminate()
