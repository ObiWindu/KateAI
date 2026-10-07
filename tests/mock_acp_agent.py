#!/usr/bin/env python3
"""Minimal ACP agent over stdio used by the Kate AI integration test.

Speaks initialize, session/new, session/prompt, session/cancel, and can issue
session/request_permission, fs/*, and terminal/* back to the client.
"""

import json
import sys

SCENARIO = sys.argv[1] if len(sys.argv) > 1 else "echo"
NEXT_ID = 1000
SESSION_ID = "sess_kateai_test"


def send(message):
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def result(request_id, payload):
    send({"jsonrpc": "2.0", "id": request_id, "result": payload})


def error(request_id, message, code=-32000):
    send({"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}})


def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})


def read_message():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            continue
    return None


def call_client(method, params):
    global NEXT_ID
    request_id = NEXT_ID
    NEXT_ID += 1
    send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
    while True:
        message = read_message()
        if message is None:
            return None
        if message.get("id") == request_id:
            return message
        handle(message)


def session_update(update):
    notify("session/update", {"sessionId": SESSION_ID, "update": update})


def handle_prompt(request_id, params):
    text = ""
    for block in params.get("prompt") or []:
        if isinstance(block, dict) and block.get("type") == "text":
            text += block.get("text") or ""

    if SCENARIO == "permission":
        session_update(
            {
                "sessionUpdate": "tool_call",
                "toolCallId": "call_perm",
                "name": "write_file",
                "title": "Write notes.txt",
                "kind": "edit",
                "status": "pending",
            }
        )
        response = call_client(
            "session/request_permission",
            {
                "sessionId": SESSION_ID,
                "toolCall": {"toolCallId": "call_perm", "title": "Write notes.txt", "kind": "edit"},
                "options": [
                    {"optionId": "allow-once", "name": "Allow once", "kind": "allow_once"},
                    {"optionId": "reject-once", "name": "Reject", "kind": "reject_once"},
                ],
            },
        )
        option = (((response or {}).get("result") or {}).get("outcome") or {}).get("optionId")
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": f"permission:{option}"},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "fs":
        read_path = ""
        write_path = ""
        for token in text.split():
            if token.startswith("READPATH:"):
                read_path = token.split(":", 1)[1]
            if token.startswith("WRITEPATH:"):
                write_path = token.split(":", 1)[1]
        read = call_client(
            "fs/read_text_file",
            {"sessionId": SESSION_ID, "path": read_path},
        )
        content = ((read or {}).get("result") or {}).get("content", "")
        call_client(
            "fs/write_text_file",
            {"sessionId": SESSION_ID, "path": write_path, "content": "from-agent:" + content},
        )
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": "wrote"},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "quoted":
        created = call_client(
            "terminal/create",
            {
                "sessionId": SESSION_ID,
                "command": "/bin/bash -c 'printf %s quoted-ok'",
            },
        )
        terminal_id = ((created or {}).get("result") or {}).get("terminalId")
        call_client("terminal/wait_for_exit", {"sessionId": SESSION_ID, "terminalId": terminal_id})
        output = call_client("terminal/output", {"sessionId": SESSION_ID, "terminalId": terminal_id})
        call_client("terminal/release", {"sessionId": SESSION_ID, "terminalId": terminal_id})
        body = ((output or {}).get("result") or {}).get("output", "")
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": body.strip()},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "terminal":
        created = call_client(
            "terminal/create",
            {
                "sessionId": SESSION_ID,
                "command": "python3",
                "args": ["-c", "print('hello-term')"],
            },
        )
        terminal_id = ((created or {}).get("result") or {}).get("terminalId")
        call_client("terminal/wait_for_exit", {"sessionId": SESSION_ID, "terminalId": terminal_id})
        output = call_client("terminal/output", {"sessionId": SESSION_ID, "terminalId": terminal_id})
        call_client("terminal/release", {"sessionId": SESSION_ID, "terminalId": terminal_id})
        body = ((output or {}).get("result") or {}).get("output", "")
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": body.strip()},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "cancel":
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": "before-cancel"},
            }
        )
        while True:
            message = read_message()
            if message is None:
                return
            if message.get("method") == "session/cancel":
                result(request_id, {"stopReason": "cancelled"})
                return
            handle(message)

    session_update(
        {
            "sessionUpdate": "agent_thought_chunk",
            "content": {"type": "text", "text": "thinking..."},
        }
    )
    session_update(
        {
            "sessionUpdate": "plan",
            "entries": [{"content": "Look around", "priority": "high", "status": "completed"}],
        }
    )
    session_update(
        {
            "sessionUpdate": "tool_call",
            "toolCallId": "call_ls",
            "name": "list_dir",
            "title": "List workspace",
            "kind": "read",
            "status": "pending",
        }
    )
    session_update(
        {
            "sessionUpdate": "tool_call_update",
            "toolCallId": "call_ls",
            "status": "completed",
            "content": [{"type": "content", "content": {"type": "text", "text": "ok"}}],
        }
    )
    session_update(
        {
            "sessionUpdate": "agent_message_chunk",
            "content": {"type": "text", "text": "hello "},
        }
    )
    session_update(
        {
            "sessionUpdate": "agent_message_chunk",
            "content": {"type": "text", "text": text.strip()},
        }
    )
    result(request_id, {"stopReason": "end_turn"})


def handle(message):
    method = message.get("method")
    request_id = message.get("id")
    params = message.get("params") or {}

    if method == "initialize":
        result(
            request_id,
            {
                "protocolVersion": 1,
                "agentCapabilities": {
                    "loadSession": True,
                    "promptCapabilities": {"embeddedContext": True},
                    "sessionCapabilities": {"resume": {}, "close": {}},
                },
                "agentInfo": {"name": "mock-acp", "title": "Mock ACP", "version": "1"},
                "authMethods": [],
            },
        )
    elif method == "authenticate":
        result(request_id, {})
    elif method == "session/new":
        result(request_id, {"sessionId": SESSION_ID})
    elif method == "session/load" or method == "session/resume":
        result(request_id, {})
    elif method == "session/close":
        result(request_id, {})
    elif method == "session/prompt":
        handle_prompt(request_id, params)
    elif method == "session/cancel":
        return
    else:
        if request_id is not None:
            error(request_id, f"Unknown method: {method}", -32601)


def main():
    while True:
        message = read_message()
        if message is None:
            return 0
        handle(message)


if __name__ == "__main__":
    sys.exit(main() or 0)
