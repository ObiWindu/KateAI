# ACP server

`kateai-headless acp --stdio` exposes Kate AI's harness to an ACP v1 client.
The server owns JSON-RPC framing and session ids; each session owns a
`HeadlessSession`/`AgentLoop`, its conversation and its event log. The plugin
never starts the server.

## Transport

One JSON-RPC 2.0 object per line on stdin/stdout. A worker thread reads stdin
and posts lines to the main thread; writes are serialised on the main thread.
The server exits when stdin closes or the host sends EOF.

## Handshake

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{},"clientInfo":{"name":"host","version":"1"}}}
```

Response:

```json
{"jsonrpc":"2.0","id":1,"result":{
  "protocolVersion":1,
  "agentCapabilities":{
    "loadSession":false,
    "promptCapabilities":{"image":false,"audio":false,"embeddedContext":true},
    "sessionCapabilities":{"close":{}}
  },
  "agentInfo":{"name":"kateai","title":"Kate AI","version":"1.0.0"},
  "authMethods":[]}}
```

## Sessions

`session/new` takes `{cwd, mcpServers}` and returns `{sessionId}`. The session
runs with that `cwd` as its workspace. `session/close` removes it and cancels
pending work.

`session/prompt` takes `{sessionId, prompt:[{type:"text",text:"…"}]}`. The
response is deferred until the turn finishes and carries
`{stopReason:"end_turn"|"cancelled"|"max_tokens"}`. A session can run further
prompts once the previous response is sent. A second prompt while one is in
flight returns `-32000`.

`session/cancel` is a notification and aborts the active turn.

## Updates

The server sends `session/update`:

| `sessionUpdate` | Payload |
| --- | --- |
| `agent_message_chunk` | `content:{type:"text",text}` |
| `agent_thought_chunk` | `content:{type:"text",text}` |
| `plan` | `entries:[{content,status,priority}]` |
| `tool_call` | `toolCallId`, `title`, `kind`, `status`, `content`, optional `rawInput` |
| `tool_call_update` | `toolCallId`, `status` (`completed`/`failed`/`cancelled`), `content` |

`kind` maps the tool risk class: `read`, `edit`, `execute`.

## Permissions

When the policy asks, the server sends a `session/request_permission` request:

```json
{"jsonrpc":"2.0","id":1000,"method":"session/request_permission","params":{
  "sessionId":"sess-…",
  "toolCall":{"toolCallId":"call-1","title":"Write note.txt","kind":"edit","status":"pending"},
  "options":[
    {"optionId":"allow_once","name":"Allow once","kind":"allow_once"},
    {"optionId":"allow_session","name":"Allow for this session","kind":"allow_always"},
    {"optionId":"reject_once","name":"Reject","kind":"reject_once"}
  ]}}
```

The host answers:

```json
{"jsonrpc":"2.0","id":1000,"result":{"outcome":{"outcome":"selected","optionId":"allow_session"}}}
```

A missing/unknown option, an error response or a cancellation resolves to
deny. The hardline floor and policy denies always win; a permission request is
only sent for an action the policy already classified as `ask`.

## Questions

`_kateai/ask_user` mirrors the native question tool:

```json
{"jsonrpc":"2.0","id":1001,"method":"_kateai/ask_user","params":{
  "sessionId":"sess-…","question":"Which file?","options":["a","b"],"allowFreeform":true}}
```

Answer with `{"answer":"a"}` (or `{"text":"a"}`). An empty answer resumes the
tool as cancelled.

## Kate methods

| Method | Result |
| --- | --- |
| `_kateai/ping` | `{ok:true}` |
| `_kateai/capabilities` | event log, verification, policy, hooks, permissions, questions and sessions booleans |
| `authenticate` | `{}` (the harness uses configured provider credentials) |

`session/load` is not implemented; the server advertises `loadSession:false`.

## Security

- The same sandbox, hardline, policy, hooks and verification gate as the plugin.
- The client sees redacted event/tool text, never credential values.
- Permissions are scoped to the session; `allow_session` does not persist.
- The server rejects requests for an unknown session and caps pending prompts
  per session at one.
