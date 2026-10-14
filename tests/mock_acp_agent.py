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
CURRENT_MODEL = "grok-4.5"


def model_config(current, extra=False):
    options = [
        {
            "group": "grok",
            "name": "Grok",
            "options": [
                {"name": "Grok 4", "value": "grok-4"},
                {"name": "Grok 4.5", "value": "grok-4.5"},
            ],
        }
    ]
    if extra:
        options[0]["options"].append({"name": "Grok 4.7", "value": "grok-4.7"})
    return {
        "id": "model",
        "name": "Model",
        "category": "model",
        "type": "select",
        "currentValue": current,
        "options": options,
    }


def mode_config():
    return {
        "id": "mode",
        "name": "Mode",
        "category": "mode",
        "type": "select",
        "currentValue": "code",
        "options": [{"name": "Code", "value": "code"}],
    }


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


def request_no_wait(method, params):
    """Send a request without blocking for the reply, so the client can be put
    into the position of having two questions outstanding at once."""
    global NEXT_ID
    request_id = NEXT_ID
    NEXT_ID += 1
    send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
    return request_id


def ask_permission(tool_call_id):
    session_update(
        {
            "sessionUpdate": "tool_call",
            "toolCallId": tool_call_id,
            "name": "write_file",
            "title": "Write notes.txt",
            "kind": "edit",
            "status": "pending",
        }
    )
    return request_no_wait(
        "session/request_permission",
        {
            "sessionId": SESSION_ID,
            "toolCall": {"toolCallId": tool_call_id, "title": "Write notes.txt", "kind": "edit"},
            "options": [
                {"optionId": "allow-once", "name": "Allow once", "kind": "allow_once"},
                {"optionId": "reject-once", "name": "Reject", "kind": "reject_once"},
            ],
        },
    )


def outcome_summary(outcome):
    if not outcome:
        return "unanswered"
    if outcome.get("outcome") == "cancelled":
        return "cancelled"
    return outcome.get("optionId") or "unknown"


def handle_prompt(request_id, params):
    text = ""
    for block in params.get("prompt") or []:
        if isinstance(block, dict) and block.get("type") == "text":
            text += block.get("text") or ""

    if SCENARIO == "stringid":
        # JSON-RPC lets a reply quote the request id as a string. session/prompt
        # has no timeout, so a client that cannot match the id never finishes the
        # turn at all.
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": "string-id-ok"},
            }
        )
        result(str(request_id), {"stopReason": "end_turn"})
        return

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

    if SCENARIO == "permission-double":
        # Two questions outstanding at once. Only one can be answered at a time,
        # so the first must be closed by the client rather than left unanswered.
        first = ask_permission("call_a")
        second = ask_permission("call_b")
        outcomes = {}
        while len(outcomes) < 2:
            message = read_message()
            if message is None:
                break
            if message.get("id") in (first, second):
                outcomes[message["id"]] = ((message.get("result") or {}).get("outcome") or {})
            else:
                handle(message)
        summary = "a=%s;b=%s" % (
            outcome_summary(outcomes.get(first)),
            outcome_summary(outcomes.get(second)),
        )
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": "permissions:" + summary},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "permission-allowonly":
        # An agent that advertises an allow option but no reject option. Refusing
        # must never transmit that allow id back, because the agent would treat it
        # as approval and run the tool the user just denied.
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
                    {"optionId": "allow-always", "name": "Always allow", "kind": "allow_always"},
                ],
            },
        )
        outcome = ((response or {}).get("result") or {}).get("outcome") or {}
        # "cancelled" (no optionId at all) is the safe refusal; anything naming an
        # allow option means the agent would execute the denied tool.
        summary = outcome.get("optionId") or ("cancelled" if outcome.get("outcome") == "cancelled" else "unknown")
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": f"refusal:{summary}"},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "permission-alwaysonly":
        # An agent that advertises only the "always" kinds. A plain Allow has no
        # allow_once to send, so the client must still transmit an allow option
        # instead of cancelling a tool the user approved.
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
                    {"optionId": "allow-always", "name": "Always allow", "kind": "allow_always"},
                    {"optionId": "reject-always", "name": "Never allow", "kind": "reject_always"},
                ],
            },
        )
        outcome = ((response or {}).get("result") or {}).get("outcome") or {}
        summary = outcome.get("optionId") or ("cancelled" if outcome.get("outcome") == "cancelled" else "unknown")
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": f"approval:{summary}"},
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

    if SCENARIO == "plan":
        written = call_client(
            "write_plan_file",
            {"sessionId": SESSION_ID, "content": "# Plan\n\n1. Inspect the code\n2. Change it\n"},
        )
        path = ((written or {}).get("result") or {}).get("path", "")
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": "plan:" + path},
            }
        )
        result(request_id, {"stopReason": "end_turn"})
        return

    if SCENARIO == "suggest":
        call_client(
            "suggest",
            {
                "sessionId": SESSION_ID,
                "suggestion": "Use std::string_view here instead of a const reference",
                "path": "src/main.cpp",
                "line": 7,
            },
        )
        session_update(
            {
                "sessionUpdate": "agent_message_chunk",
                "content": {"type": "text", "text": "suggested"},
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
    global CURRENT_MODEL
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
        if SCENARIO == "models":
            result(
                request_id,
                {
                    "sessionId": SESSION_ID,
                    "configOptions": [model_config(CURRENT_MODEL), mode_config()],
                    # Older clients read this. Kate must prefer configOptions.
                    "models": {
                        "currentModelId": "stale-model",
                        "availableModels": [{"modelId": "stale-model", "name": "Stale"}],
                    },
                },
            )
        elif SCENARIO == "legacy-models":
            result(
                request_id,
                {
                    "sessionId": SESSION_ID,
                    "models": {
                        "currentModelId": "gemini-2.5-pro",
                        "availableModels": [
                            {"modelId": "gemini-2.5-pro", "name": "Gemini 2.5 Pro"},
                            {"modelId": "gemini-2.5-flash", "name": "Gemini 2.5 Flash"},
                        ],
                    },
                },
            )
        else:
            result(request_id, {"sessionId": SESSION_ID})
    elif method == "session/set_config_option":
        CURRENT_MODEL = params.get("value") or CURRENT_MODEL
        options = [model_config(CURRENT_MODEL, extra=True), mode_config()]
        session_update({"sessionUpdate": "config_option_update", "configOptions": options})
        result(request_id, {"configOptions": options})
    elif method == "session/set_model":
        result(request_id, {})
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
