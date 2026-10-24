#!/usr/bin/env python3
"""Mock OpenAI-compatible LLM used by the sub-agent parallel test.

The first request (the orchestrator) gets two new_task tool calls in a single
response. Requests whose last user message carries the CHILD marker are treated
as sub-agents and answered with plain text after a deliberate delay, so the test
can observe both sub-agents in flight at the same time. A request whose last
user message carries the QUESTION marker is answered with a single question tool
call, which the test must answer before the turn can finish.

Usage: mock_llm_server.py <port-file>
"""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHILD_MARKER = "CHILD-TASK"
# What a question-flow test sends to make the mock ask something.
QUESTION_MARKER = "Ask the user something."
# Sent to make the mock ask a question with no text in it, which the loop must
# refuse rather than park the turn waiting for an answer to nothing.
EMPTY_QUESTION_MARKER = "Ask nothing at all."
# Sent to make the model post a suggestion with the suggest tool.
SUGGEST_MARKER = "Suggest an improvement."
# Same call, but the arguments arrive as a JSON object the way Ollama's
# OpenAI-compatible endpoint delivers them.
SUGGEST_OBJECT_ARGS_MARKER = "Suggest with object arguments."
# Sent to make the model run a long shell command, so a stop can be pressed
# while the command is still in flight.
STALL_MARKER = "Run a long command."
# The orchestrator's own prompt; only the first request looks like this.
PARENT_MARKER = "Split this across two agents."
# Sent to make the model write a file, so the pre-mutation checkpoint and the
# queued continuation can be observed.
WRITE_MARKER = "Write the note file."
# Sent to make the model stream text slowly, so a stop can be pressed while
# the answer is still being typed into the chat.
STREAM_MARKER = "Type slowly."
# Same, but the chunks are hidden reasoning: the stop has to freeze the
# paced reveal of thinking text, not only the visible answer.
THINK_MARKER = "Think slowly."
# Long enough that two sequential dispatches would visibly not overlap.
CHILD_DELAY_SECONDS = 1.5
# The orchestrator delegates exactly once; every later turn gets prose. Without
# this the parent would spawn sub-agents forever.
DELEGATED = False
# The question is asked once per server, for the same reason.
ASKED = False
ASKED_EMPTY = False
# The suggestion is posted once per server, for the same reason.
SUGGESTED = False
# The long command is issued once per server, for the same reason.
STALLED = False
# The file write is issued once per server, for the same reason.
WROTE = False
STREAMED = False
THOUGHT = False


def sse(payload):
    return f"data: {json.dumps(payload)}\n\n"


def chunk_text(text):
    return sse({"choices": [{"index": 0, "delta": {"content": text}, "finish_reason": None}]})


def chunk_thinking(text):
    return sse({"choices": [{"index": 0, "delta": {"reasoning_content": text}, "finish_reason": None}]})


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


def tool_call_object_args(index, call_id, name, arguments):
    # Ollama and other local servers deliver the arguments as a JSON object
    # rather than a string. The client must cope with both shapes.
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
                                "function": {"name": name, "arguments": arguments},
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
        global DELEGATED, ASKED, ASKED_EMPTY, SUGGESTED, STALLED, WROTE, STREAMED, THOUGHT
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
                content = message.get("content", "")
                if isinstance(content, list):
                    content = " ".join(
                        block.get("text", "") for block in content if isinstance(block, dict)
                    )
                last_user = content
        is_child = CHILD_MARKER in last_user
        is_orchestrator_first_turn = PARENT_MARKER in last_user and not DELEGATED
        is_question = QUESTION_MARKER in last_user and not ASKED
        is_empty_question = EMPTY_QUESTION_MARKER in last_user and not ASKED_EMPTY
        is_suggest = SUGGEST_MARKER in last_user and not SUGGESTED
        is_suggest_object_args = SUGGEST_OBJECT_ARGS_MARKER in last_user and not SUGGESTED
        is_write = WRITE_MARKER in last_user and not WROTE
        is_stream = STREAM_MARKER in last_user and not STREAMED
        is_think = THINK_MARKER in last_user and not THOUGHT
        # Anthropic puts the system prompt at the top level, so its requests are
        # the ones without a system-role message in the array.
        is_anthropic = "anthropic-version" in self.headers

        def respond(tool_name, call_id, arguments, object_args=False):
            if is_anthropic:
                # Messages API: tool arguments stream as JSON fragments after a
                # content_block_start that carries an empty input.
                emit(
                    "event: content_block_start\n"
                    + sse({"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": call_id, "name": tool_name, "input": {}}})
                )
                emit(
                    "event: content_block_delta\n"
                    + sse(
                        {
                            "type": "content_block_delta",
                            "index": 0,
                            "delta": {"type": "input_json_delta", "partial_json": json.dumps(arguments)},
                        }
                    )
                )
                emit("event: message_delta\n" + sse({"type": "message_delta", "delta": {"stop_reason": "tool_use"}}))
                emit("event: message_stop\n" + sse({"type": "message_stop"}))
            else:
                emit(
                    tool_call_object_args(0, call_id, tool_name, arguments)
                    if object_args
                    else tool_call(0, call_id, tool_name, arguments)
                )
                emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}))

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
        elif STALL_MARKER in last_user and not STALLED:
            STALLED = True
            # A command long enough that the stop button can be pressed while
            # it is still running.
            respond("bash", "call_sleep", {"command": "sleep 30"})
        elif is_stream:
            STREAMED = True
            # Chunks spaced out over several seconds: a stop can be pressed
            # while the answer is visibly still streaming in.
            for i in range(30):
                emit(chunk_text(f"Streaming line {i} of the slow answer.\n\n"))
                time.sleep(0.2)
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}))
        elif is_think:
            THOUGHT = True
            # Hidden reasoning, spaced out the same way: the stop has to
            # freeze the paced thinking reveal, not just the visible text.
            for i in range(20):
                emit(chunk_thinking(f"Thought {i} of the slow reasoning.\n\n"))
                time.sleep(0.25)
            emit(chunk_text("The reasoned answer."))
            emit(sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}))
        elif is_write:
            WROTE = True
            respond(
                "write_file",
                "call_write",
                {"path": "notes.txt", "content": "written by the mock model"},
            )
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
        elif is_question:
            ASKED = True
            # One question with two offered answers: the turn cannot finish
            # until the user picks or types one.
            respond(
                "question",
                "call_q",
                {
                    "question": "Rewrite the file, or patch it in place?",
                    "options": ["Rewrite", "Patch"],
                },
            )
        elif is_empty_question:
            ASKED_EMPTY = True
            respond("question", "call_qe", {"question": ""})
        elif is_suggest_object_args:
            SUGGESTED = True
            respond(
                "suggest",
                "call_s",
                {
                    "suggestion": "Use std::string_view here instead of a const reference.",
                    "path": "src/main.cpp",
                    "line": 42,
                },
                object_args=True,
            )
        elif is_suggest:
            SUGGESTED = True
            # Advice the user never asked for: the suggestion tool is how the
            # agent raises it instead of editing the file itself.
            respond(
                "suggest",
                "call_s",
                {
                    "suggestion": "Use std::string_view here instead of a const reference.",
                    "path": "src/main.cpp",
                    "line": 42,
                },
            )
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