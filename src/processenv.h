/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace KateAi
{

// True when an environment variable name conventionally carries a secret.
// The check is deliberately name-based: KateAI cannot inspect the value of
// every variable, but it can avoid handing credentials to processes that do
// not need them.
bool isSecretEnvironmentName(const QString &name);

// The current process environment with secret-shaped variables removed.
// Callers that legitimately need a specific variable add it back explicitly
// (for example an MCP server's configured `env` block, or an ACP terminal
// request's `env` array), which keeps the exposure deliberate and auditable.
QProcessEnvironment sanitizedProcessEnvironment();

// Same, for a caller-supplied base (tests and sub-agent environments).
QProcessEnvironment sanitizedProcessEnvironment(const QProcessEnvironment &base);

} // namespace KateAi
