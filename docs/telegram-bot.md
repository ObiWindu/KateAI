# Telegram remote prompting

Outbound-only bridge: Kate polls `https://api.telegram.org` with
`getUpdates` long-polling. No inbound port, no webhook, no NAT/firewall work.

## Setup

1. Talk to `@BotFather` → `/newbot` → copy the token.
2. Find your numeric chat id via `@userinfobot`.
3. Kate → Settings → Kate AI → **Telegram**:
   - Enabled: on
   - Bot token: `<token>` (or env `KATEAI_TELEGRAM_BOT_TOKEN`, which wins and is never saved)
   - Allowed chat ids: `123456789` (comma/space separated; empty = bridge refuses to start)
   - Remote writes: off = read-only remote (plan mode + Strict sandbox).
4. Send `/help` to the bot.

## Security model

- **Fail-closed**: malformed token or empty allowlist → bridge never polls.
- **Allowlist**: every `message`/`callback_query` is checked; others are dropped silently (no oracle).
- **Hardened loop** (`telegrambridge.cpp: hardenedSettingsForRemote`): `Ask` permissions,
  `Strict`/`ReadOnly` sandbox, no outside reads, auto-approvals cleared, remote budgets capped
  (`maxToolCalls ≤ 50`, `maxModelRequests ≤ 25`). Read-only remote additionally forces plan mode.
- **Approvals**: every mutating tool posts `Allow once / Deny` inline buttons (`callback_data`
  `tg:allow`/`tg:deny`, no internal ids). 2 min default-deny timeout. `Allow session` is never
  offered remotely.
- **Questions**: the `question` tool forwards text + options; the next message from the same chat
  is the answer; timeout cancels.
- **Single-flight**: one remote turn at a time; `/stop` aborts, `/new` resets.
- **Redaction**: `sk-`, `xai-`, GitHub, AWS, `Bearer`, `api_key`, private-key blocks and the bot
  token are replaced before `sendMessage`; messages truncated to ~3500 chars.
- **Audit**: remote turns are prefixed `[telegram <chatId>]` and checkpointed
  (`Before telegram prompt`) so `/undo` from the panel can roll back.
- Token lives with the other API keys (owner-only file, see `settings.cpp`), plus env override.

## Commands

`/ask <prompt>`, `/new`, `/stop`, `/status`, `/approve`, `/deny`, `/help`. Anything else
starting with `/` is rejected; plain text is a prompt.
