/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "agenttext.h"

#include "codehighlight.h"

#include <QRegularExpression>
#include <QTextDocument>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

struct Fence
{
    QChar marker;
    int length = 0;
    QString language;
};

// Markdown fences may use three or more backticks or tildes and allow up to
// three leading spaces. Parse the marker separately from the info string so
// language tags remain correct when a model adds tabs or attributes.
bool openingFence(const QString &line, Fence *fence)
{
    int indent = 0;
    while (indent < line.size() && line.at(indent) == u' ' && indent < 4) {
        ++indent;
    }
    if (indent > 3 || indent >= line.size()) {
        return false;
    }

    const QChar marker = line.at(indent);
    if (marker != u'`' && marker != u'~') {
        return false;
    }
    int end = indent;
    while (end < line.size() && line.at(end) == marker) {
        ++end;
    }
    const int length = end - indent;
    if (length < 3) {
        return false;
    }

    const QString info = line.mid(end).trimmed();
    // CommonMark forbids backticks in the info string of a backtick fence.
    if (marker == u'`' && info.contains(u'`')) {
        return false;
    }
    if (fence) {
        fence->marker = marker;
        fence->length = length;
        fence->language = info.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
    }
    return true;
}

bool closingFence(const QString &line, const Fence &fence)
{
    int indent = 0;
    while (indent < line.size() && line.at(indent) == u' ' && indent < 4) {
        ++indent;
    }
    if (indent > 3 || indent >= line.size() || line.at(indent) != fence.marker) {
        return false;
    }
    int end = indent;
    while (end < line.size() && line.at(end) == fence.marker) {
        ++end;
    }
    return end - indent >= fence.length && line.mid(end).trimmed().isEmpty();
}

} // namespace

QString closeMarkdown(const QString &text)
{
    // Streaming chunks often end halfway through a fenced block. Close only
    // that block for rendering: counting backticks, asterisks or underscores
    // across the raw source also counts punctuation inside code and ordinary
    // identifiers, which can make Qt reinterpret subsequent words as markup.
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(u'\r', u'\n');
    const QStringList lines = normalized.split(u'\n');
    Fence open;
    bool inFence = false;
    for (const QString &line : lines) {
        if (!inFence) {
            if (openingFence(line, &open)) {
                inFence = true;
            }
        } else if (closingFence(line, open)) {
            inFence = false;
        }
    }
    if (!inFence) {
        return text;
    }
    return normalized + (normalized.endsWith(u'\n') ? QString() : QStringLiteral("\n"))
        + QString(open.length, open.marker);
}

void renderMarkdown(QTextDocument *doc, const QString &markdown)
{
    if (!doc) {
        return;
    }

    // QTextDocument::setMarkdown() replaces the whole document, so the parts
    // cannot be rendered one at a time. The body is assembled by hand instead:
    // prose goes through a scratch document, code through the highlighter.
    QString html = QStringLiteral("<html><body>");
    // Providers can stream either LF or CRLF. A stray carriage return in a
    // code line can otherwise be handed to the HTML parser as content and
    // change where text appears in the transcript.
    QString normalized = markdown;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(u'\r', u'\n');
    const QStringList lines = normalized.split(u'\n');
    QStringList prose;
    int i = 0;

    auto flushProse = [&html, &prose] {
        if (prose.isEmpty()) {
            return;
        }
        // Models and pasted text often use a typographic bullet instead of a
        // Markdown list marker. Qt treats those as ordinary paragraphs, which
        // makes list entries inconsistent with the surrounding Markdown.
        for (QString &line : prose) {
            qsizetype marker = 0;
            while (marker < line.size() && line.at(marker).isSpace()) {
                ++marker;
            }
            if (marker + 1 < line.size()
                && (line.at(marker) == u'•' || line.at(marker) == u'·' || line.at(marker) == u'‣'
                    || line.at(marker) == u'◦')
                && line.at(marker + 1).isSpace()) {
                line.replace(marker, 1, u"-"_s);
            }
        }
        QTextDocument scratch;
        scratch.setMarkdown(prose.join(u'\n'));
        const QString converted = scratch.toHtml();
        // Qt emits the opening tag with attributes -- <body style="..."> -- so
        // the body content is bounded by the tag's *start* and the '>' that
        // closes it, not by a literal "<body>". Matching the literal string
        // never hits: every prose run then fell through to the escaped-text
        // fallback below, which is how a whole chat of **bold**, `code` and
        // bullet lists rendered as raw markdown source.
        const int bodyTagStart = converted.indexOf(u"<body"_s);
        const int bodyTagEnd = bodyTagStart >= 0 ? converted.indexOf(u'>', bodyTagStart) : -1;
        const int bodyEnd = converted.lastIndexOf(u"</body>"_s);
        if (bodyTagEnd >= 0 && bodyEnd > bodyTagEnd) {
            html += converted.mid(bodyTagEnd + 1, bodyEnd - bodyTagEnd - 1);
        } else {
            html += QStringLiteral("<p>%1</p>")
                        .arg(prose.join(QStringLiteral("\n")).toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>")));
        }
        prose.clear();
    };

    while (i < lines.size()) {
        Fence fence;
        if (!openingFence(lines.at(i), &fence)) {
            prose << lines.at(i);
            ++i;
            continue;
        }

        flushProse();

        QStringList code;
        ++i;
        while (i < lines.size() && !closingFence(lines.at(i), fence)) {
            code << lines.at(i);
            ++i;
        }
        if (i < lines.size()) {
            ++i; // consume the closing fence
        }

        const QString body = code.join(u'\n');
        // htmlForCode falls back to an uncoloured block for languages it does not
        // know, so its result is always usable.
        html += CodeHighlight::htmlForCode(body, fence.language);
    }

    flushProse();
    html += QStringLiteral("</body></html>");
    doc->setHtml(html);
}

QTextDocument *makeMarkdownDocument(const QString &markdown, const QString &styleSheet)
{
    auto *doc = new QTextDocument;
    // The stylesheet must be set BEFORE the markdown is parsed:
    // QTextDocument::setDefaultStyleSheet() only affects content that is parsed
    // afterwards, and nothing re-applies it. Setting it here (rather than on the
    // QTextBrowser's own document) is what makes it survive setDocument(), which
    // swaps in this document and discards the browser's.
    if (!styleSheet.isEmpty()) {
        doc->setDefaultStyleSheet(styleSheet);
    }
    renderMarkdown(doc, markdown);
    return doc;
}

} // namespace KateAi
