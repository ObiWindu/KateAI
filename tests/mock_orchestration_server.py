#!/usr/bin/env python3
"""Minimal OpenAI-compatible mock used by test_orchestrationflow.

Two instances are started: one serves the planner tools, the other the executor
tools. The role is selected by the model name, so the same script can also be
used with a single provider when debugging.
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def sse(payload):
    return f"data: {json.dumps(payload)}\n\n"


def tool_call(call_id, name, arguments):
    return sse(
        {
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": call_id,
                                "type": "function",
                                "function": {
                                    "name": name,
                                    "arguments": json.dumps(arguments),
                                },
                            }
                        ]
                    },
                    "finish_reason": None,
                }
            ]
        }
    )


class Handler(BaseHTTPRequestHandler):
    role = "planner"

    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self.send_error(400)
            return

        tools = {
            tool.get("function", {}).get("name", "")
            for tool in body.get("tools", [])
        }
        messages = body.get("messages", [])
        is_review = "submit_review" in tools
        is_plan = "submit_plan" in tools

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def emit(text):
            self.wfile.write(text.encode())
            self.wfile.flush()

        if is_review:
            emit(
                tool_call(
                    "call_review",
                    "submit_review",
                    {
                        "approved": True,
                        "summary": "The executor wrote note.txt and reported success.",
                    },
                )
            )
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}))
        elif is_plan:
            emit(
                tool_call(
                    "call_plan",
                    "submit_plan",
                    {
                        "summary": "Create note.txt with a short marker.",
                        "needs_changes": True,
                        "steps": [{"description": "Write note.txt", "files": ["note.txt"]}],
                        "acceptance": ["note.txt contains orchestrated"],
                        "replan_if": ["The write fails twice"],
                    },
                )
            )
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}))
        elif self.role == "executor":
            already_wrote = any(
                call.get("function", {}).get("name") == "write_file"
                for message in messages
                if message.get("role") == "assistant"
                for call in message.get("tool_calls", [])
            )
            if already_wrote:
                emit(sse({"choices": [{"index": 0, "delta": {"content": "Wrote note.txt."}, "finish_reason": None}]}))
                emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}))
            else:
                emit(
                    tool_call(
                        "call_write",
                        "write_file",
                        {"path": "note.txt", "content": "orchestrated\n"},
                    )
                )
                emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}))
        else:
            emit(sse({"choices": [{"index": 0, "delta": {"content": "No plan needed."}, "finish_reason": None}]}))
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}))

        emit("data: [DONE]\n\n")


def main():
    port_file = sys.argv[1]
    Handler.role = sys.argv[2] if len(sys.argv) > 2 else "planner"
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(port_file, "w", encoding="utf-8") as handle:
        handle.write(str(server.server_address[1]))
    server.serve_forever()


if __name__ == "__main__":
    main()
