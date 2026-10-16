/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "agenttext.h"

#include "codehighlight.h"

#include <QTextDocument>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QString closeMarkdown(const QString &text)
{
    QString result = text;
    // Close unclosed triple backticks (code blocks)
    if (result.count(u"```"_s) % 2 == 1) {
        result += u"\n```"_s;
    }
    // Close unclosed single backticks (inline code)
    if (result.count(u"`"_s) % 2 == 1) {
        result += u"`"_s;
    }
    // Close unclosed bold markers (**)
    if (result.count(u"**"_s) % 2 == 1) {
        result += u"**"_s;
    }
    // Close unclosed italic markers (*) - but not part of **
    int singleAsterisk = 0;
    for (int i = 0; i < result.length(); ++i) {
        if (result[i] == u'*') {
            bool isDouble = (i + 1 < result.length() && result[i + 1] == u'*') || (i > 0 && result[i - 1] == u'*');
            if (!isDouble) {
                singleAsterisk++;
            }
        }
    }
    if (singleAsterisk % 2 == 1) {
        result += u"*"_s;
    }
    // Close unclosed underscore italic markers (_) - but not part of __
    int singleUnderscore = 0;
    for (int i = 0; i < result.length(); ++i) {
        if (result[i] == u'_') {
            bool isDouble = (i + 1 < result.length() && result[i + 1] == u'_') || (i > 0 && result[i - 1] == u'_');
            if (!isDouble) {
                singleUnderscore++;
            }
        }
    }
    if (singleUnderscore % 2 == 1) {
        result += u"_"_s;
    }
    return result;
}

namespace
{

// Matches an opening or closing ``` fence, capturing an optional language hint.
bool isFence(const QString &line, QString *language, bool *opening)
{
    QString trimmed = line;
    while (trimmed.startsWith(QLatin1Char(' '))) {
        trimmed.remove(0, 1);
    }
    if (!trimmed.startsWith(u"```"_s)) {
        return false;
    }
    trimmed = trimmed.mid(3).trimmed();
    // A fence may be indented for a list item; more than three spaces means the
    // line is ordinary text that happens to contain backticks.
    if (trimmed.startsWith(QLatin1Char('`'))) {
        return false;
    }
    if (opening) {
        *opening = true;
    }
    if (language) {
        *language = trimmed.section(QLatin1Char(' '), 0, 0);
    }
    return true;
}

// A closing fence carries no language hint.
bool isClosingFence(const QString &line)
{
    QString trimmed = line.trimmed();
    if (!trimmed.startsWith(u"```"_s)) {
        return false;
    }
    return trimmed.mid(3).trimmed().isEmpty();
}

} // namespace

void renderMarkdown(QTextDocument *doc, const QString &markdown)
{
    if (!doc) {
        return;
    }

    // QTextDocument::setMarkdown() replaces the whole document, so the parts
    // cannot be rendered one at a time. The body is assembled by hand instead:
    // prose goes through a scratch document, code through the highlighter.
    QString html = QStringLiteral("<html><body>");
    const QStringList lines = markdown.split(u'\n');
    QStringList prose;
    int i = 0;

    auto flushProse = [&html, &prose] {
        if (prose.isEmpty()) {
            return;
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
        QString language;
        bool opening = false;
        if (!isFence(lines.at(i), &language, &opening) || isClosingFence(lines.at(i))) {
            prose << lines.at(i);
            ++i;
            continue;
        }

        flushProse();

        QStringList code;
        ++i;
        while (i < lines.size() && !isClosingFence(lines.at(i))) {
            code << lines.at(i);
            ++i;
        }
        if (i < lines.size()) {
            ++i; // consume the closing fence
        }

        const QString body = code.join(u'\n');
        // htmlForCode falls back to an uncoloured block for languages it does not
        // know, so its result is always usable.
        html += CodeHighlight::htmlForCode(body, language);
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