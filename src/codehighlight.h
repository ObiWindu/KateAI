/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

namespace KateAi
{

/**
 * Syntax highlighting for code blocks in the chat transcript, using Kate's own
 * engine (KSyntaxHighlighting).
 *
 * The transcript used to render every fenced block as flat monospace text in the
 * same colour as prose, because QTextDocument's markdown renderer has no notion
 * of syntax. That made a 40-line diff or config sample much harder to read than
 * the same code in an editor. Since Kate already ships the syntax definitions
 * and themes the user has configured, the chat can reuse them instead of
 * carrying a second, divergent highlighter.
 *
 * The definitions and theme are resolved once and cached: Repository is
 * expensive to build, and the transcript re-renders on every streamed delta.
 */
namespace CodeHighlight
{

// True when Kate's syntax definitions are available. Always true on a normal
// install; the callers degrade to plain text when it is not.
bool isAvailable();

// HTML for one code block, including its own <pre> wrapper and the language
// label. The body is wrapped in per-token <span>s carrying the theme colours.
// Returns an empty string when the language is unknown, so the caller can fall
// back to its own rendering.
QString htmlForCode(const QString &code, const QString &language);

// Maps the many spellings models emit ("js", "c++", "sh") onto definition
// names KSyntaxHighlighting knows. Returns the input unchanged when there is no
// better match.
QString normaliseLanguage(const QString &language);

// Overrides the theme used for highlighting; empty restores Kate's default
// dark theme. Intended for tests and for a future "follow editor theme".
void setThemeName(const QString &themeName);
QString themeName();

// Number of syntax definitions the repository knows about. Used by the tests to
// confirm highlighting really resolved something.
int availableDefinitionCount();

} // namespace CodeHighlight

} // namespace KateAi