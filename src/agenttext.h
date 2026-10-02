/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

class QTextDocument;

namespace KateAi
{

// Closes markdown fences that are still open, so a half-streamed answer does
// not swallow the rest of the transcript. Shared by the chat transcript and the
// sub-agent cards.
QString closeMarkdown(const QString &text);

/**
 * Renders markdown into `doc`, replacing fenced code blocks with syntax
 * highlighted HTML from Kate's highlighter.
 *
 * QTextDocument::setMarkdown() renders every fenced block as flat monospace
 * text in the prose colour, so code in an answer was much harder to read than
 * the same code in an editor. This walks the markdown instead, handing prose to
 * the normal markdown renderer and code to CodeHighlight.
 *
 * The streaming case matters: closeMarkdown() has already balanced the fences,
 * so a block that is still being received highlights as far as it has arrived
 * and picks up the rest on the next repaint.
 */
void renderMarkdown(QTextDocument *doc, const QString &markdown);

// Renders markdown into a document owned by the caller, ready for a QTextBrowser.
QTextDocument *makeMarkdownDocument(const QString &markdown);

} // namespace KateAi