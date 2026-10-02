#!/usr/bin/env python3
"""Minimal MCP server over stdio used by the Kate AI integration test.

Speaks just enough of the protocol for the test: initialize, tools/list and
tools/call, with one read-only tool and one read-write tool.
"""

import json
import sys


def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def result(request_id, payload):
    send({"jsonrpc": "2.0", "id": request_id, "result": payload})


def error(request_id, message, code=-32000):
    send({"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}})


TOOLS = [
    {
        "name": "echo",
        "description": "Echo the given text back to the caller.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "boom",
        "description": "Always fails, for error-path testing.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue

        method = message.get("method")
        request_id = message.get("id")
        params = message.get("params") or {}

        if method == "initialize":
            result(request_id, {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}}, "serverInfo": {"name": "mock", "version": "1"}})
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            result(request_id, {"tools": TOOLS})
        elif method == "tools/call":
            name = params.get("name")
            if name == "echo":
                result(request_id, {"content": [{"type": "text", "text": params.get("arguments", {}).get("text", "")}], "isError": False})
            elif name == "boom":
                result(request_id, {"content": [{"type": "text", "text": "the tool exploded"}], "isError": True})
            else:
                error(request_id, f"Unknown tool: {name}", -32602)
        else:
            if request_id is not None:
                error(request_id, f"Unknown method: {method}", -32601)

    return 0


if __name__ == "__main__":
    sys.exit(main())