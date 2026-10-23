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

def wait_response(rid, timeout=20):
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
        print("[OTHER]", json.dumps(msg)[:300], file=sys.stderr)
    return None

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "protocolVersion": 1,
    "clientCapabilities": {"fs": {"readTextFile": True, "writeTextFile": True},
                           "terminal": True,
                           "session": {"configOptions": {"boolean": {}}}},
    "clientInfo": {"name": "kateai", "title": "Kate AI", "version": "0.1.0"}}})
init = wait_response(1)
print("init authMethods:", json.dumps(init["result"].get("authMethods")))

# Kate sends authenticate only if an API key is set; test what opencode does
send({"jsonrpc": "2.0", "id": 2, "method": "authenticate", "params": {"methodId": "opencode-login"}})
auth = wait_response(2, timeout=15)
print("authenticate:", json.dumps(auth)[:800] if auth else "NO RESPONSE (timeout)")

send({"jsonrpc": "2.0", "id": 3, "method": "session/new", "params": {
    "cwd": "/home/macewindu/ai-dev/KateAI", "mcpServers": []}})
sn = wait_response(3, timeout=60)
print("session/new keys:", list(sn["result"].keys()) if sn and "result" in sn else json.dumps(sn)[:300])
sid = sn["result"]["sessionId"]

send({"jsonrpc": "2.0", "id": 4, "method": "session/set_config_option", "params": {
    "sessionId": sid, "configId": "model", "value": "mistral/codestral-latest"}})
r = wait_response(4, timeout=20)
print("set_config_option:", json.dumps(r)[:500] if r else "NO RESPONSE (timeout)")

send({"jsonrpc": "2.0", "id": 5, "method": "session/set_model", "params": {
    "sessionId": sid, "modelId": "mistral/codestral-latest"}})
r = wait_response(5, timeout=20)
print("set_model:", json.dumps(r)[:500] if r else "NO RESPONSE (timeout)")

proc.terminate()
