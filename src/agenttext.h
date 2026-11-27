/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>
#include <QUrl>

class QObject;
class QTextDocument;
class QTextEdit;

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

// Renders markdown into a new QTextDocument owned by `parent` (or the caller
// when `parent` is null). Prefer setMarkdownContent() when updating a
// QTextEdit/QTextBrowser: QTextEdit::setDocument() does not take ownership of
// external documents.
QTextDocument *makeMarkdownDocument(const QString &markdown, const QString &styleSheet = QString(),
                                    QObject *parent = nullptr);

// Renders markdown into `view`'s existing QTextDocument in place, applying
// `styleSheet` before parsing. Reuses the document owned by `view` so
// streaming ticks and evicted cards do not leak QTextDocument instances.
void setMarkdownContent(QTextEdit *view, const QString &markdown, const QString &styleSheet = QString());

// Opens an external link only when it uses an explicit http/https scheme,
// preventing model-generated markdown from launching file:// or custom URI handlers.
bool openSafeExternalUrl(const QUrl &url);

} // namespace KateAi