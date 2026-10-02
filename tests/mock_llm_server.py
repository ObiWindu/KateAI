#!/usr/bin/env python3
"""Mock OpenAI-compatible LLM used by the sub-agent parallel test.

The first request (the orchestrator) gets two new_task tool calls in a single
response. Requests whose last user message carries the CHILD marker are treated
as sub-agents and answered with plain text after a deliberate delay, so the test
can observe both sub-agents in flight at the same time.

Usage: mock_llm_server.py <port-file>
"""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHILD_MARKER = "CHILD-TASK"
# The orchestrator's own prompt; only the first request looks like this.
PARENT_MARKER = "Split this across two agents."
# Long enough that two sequential dispatches would visibly not overlap.
CHILD_DELAY_SECONDS = 1.5
# The orchestrator delegates exactly once; every later turn gets prose. Without
# this the parent would spawn sub-agents forever.
DELEGATED = False


def sse(payload):
    return f"data: {json.dumps(payload)}\n\n"


def chunk_text(text):
    return sse({"choices": [{"index": 0, "delta": {"content": text}, "finish_reason": None}]})


def tool_call(index, call_id, name, arguments):
    return sse(
        {
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "tool_calls": [
                            {
                                "index": index,
                                "id": call_id,
                                "type": "function",
                                "function": {"name": name, "arguments": json.dumps(arguments)},
                            }
                        ]
                    },
                    "finish_reason": None,
                }
            ]
        }
    )


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # keep the test output clean
        pass

    def do_POST(self):
        global DELEGATED
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self.send_error(400)
            return

        messages = body.get("messages", [])
        last_user = ""
        for message in messages:
            if message.get("role") == "user":
                last_user = message.get("content", "")
        is_child = CHILD_MARKER in last_user
        is_orchestrator_first_turn = PARENT_MARKER in last_user and not DELEGATED

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.flush()

        def emit(text):
            self.wfile.write(text.encode())
            self.wfile.flush()

        if is_child:
            time.sleep(CHILD_DELAY_SECONDS)
            who = "A" if "agent-a" in last_user else "B"
            emit(chunk_text(f"[sub-agent {who}] finished its share of the work."))
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}))
        elif is_orchestrator_first_turn:
            DELEGATED = True
            # Two independent subtasks in one response: these must overlap.
            emit(
                tool_call(
                    0,
                    "call_a",
                    "new_task",
                    {"description": f"{CHILD_MARKER} agent-a: handle the parser", "agent": "coder"},
                )
            )
            emit(
                tool_call(
                    1,
                    "call_b",
                    "new_task",
                    {"description": f"{CHILD_MARKER} agent-b: handle the writer", "agent": "coder"},
                )
            )
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}))
        else:
            # The orchestrator's follow-up turn: summarise in prose.
            emit(chunk_text("Both sub-agents reported back."))
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}))

        emit("data: [DONE]\n\n")


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as handle:
            handle.write(str(server.server_address[1]))
    server.serve_forever()


if __name__ == "__main__":
    main()