#!/usr/bin/env python3
"""Scripted OpenAI-compatible provider for the harness eval suite.

The script is a JSON file:

    {"responses": [
        {"text": "Looking at the file."},
        {"tool_calls": [{"name": "read_file", "arguments": {"path": "a.py"}}]},
        {"text": "Fixed.", "usage": {"input_tokens": 120, "output_tokens": 20}}
    ]}

Each HTTP request consumes one entry. When the script is exhausted the server
answers with a plain final message, so a runaway agent is visible in the report
instead of hanging the suite. It never calls a real model.
"""

from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Script:
    def __init__(self, path: str):
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        self.responses = list(data.get("responses") or [])
        self.index = 0
        self.requests = []

    def next(self, body: dict) -> dict:
        self.requests.append(body)
        if self.index < len(self.responses):
            response = self.responses[self.index]
            self.index += 1
            return response
        return {"text": "Script exhausted; stopping."}


def sse(payload: dict) -> bytes:
    return ("data: " + json.dumps(payload, separators=(",", ":")) + "\n\n").encode()


def stream(handler: BaseHTTPRequestHandler, script: Script, body: dict) -> None:
    response = script.next(body)
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.end_headers()

    text = response.get("text") or ""
    if text:
        for start in range(0, len(text), 400):
            handler.wfile.write(
                sse(
                    {
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"content": text[start : start + 400]},
                                "finish_reason": None,
                            }
                        ]
                    }
                )
            )
            handler.wfile.flush()

    calls = response.get("tool_calls") or []
    for index, call in enumerate(calls):
        arguments = call.get("arguments")
        if not isinstance(arguments, str):
            arguments = json.dumps(arguments or {}, separators=(",", ":"))
        handler.wfile.write(
            sse(
                {
                    "choices": [
                        {
                            "index": 0,
                            "delta": {
                                "tool_calls": [
                                    {
                                        "index": index,
                                        "id": call.get("id") or f"call-{script.index}-{index}",
                                        "type": "function",
                                        "function": {
                                            "name": call.get("name", ""),
                                            "arguments": arguments,
                                        },
                                    }
                                ]
                            },
                            "finish_reason": None,
                        }
                    ]
                }
            )
        )
        handler.wfile.flush()

    finish = "tool_calls" if calls else "stop"
    handler.wfile.write(
        sse(
            {
                "choices": [
                    {"index": 0, "delta": {}, "finish_reason": finish}
                ]
            }
        )
    )
    usage = response.get("usage")
    if usage:
        handler.wfile.write(
            sse(
                {
                    "choices": [],
                    "usage": {
                        "prompt_tokens": int(usage.get("input_tokens", 0)),
                        "completion_tokens": int(usage.get("output_tokens", 0)),
                        "total_tokens": int(usage.get("input_tokens", 0))
                        + int(usage.get("output_tokens", 0)),
                        "prompt_tokens_details": {
                            "cached_tokens": int(usage.get("cached_input_tokens", 0))
                        },
                    },
                }
            )
        )
    handler.wfile.write(b"data: [DONE]\n\n")
    handler.wfile.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    script: Script

    def log_message(self, *args):  # keep the report clean
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self.send_error(400)
            return
        stream(self, self.script, body)

    def do_GET(self):
        # A small health/catalogue endpoint makes failures easier to diagnose.
        payload = json.dumps({"data": [{"id": "scripted", "object": "model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True)
    parser.add_argument("--port-file", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()

    script = Script(args.script)
    Handler.script = script
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    with open(args.port_file, "w", encoding="utf-8") as handle:
        handle.write(str(server.server_port))
        handle.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
