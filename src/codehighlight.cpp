/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "codehighlight.h"

#include "chattheme.h"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace CodeHighlight
{

namespace
{

// Repository construction walks the installed definition files, which is far too
// expensive to repeat on every streamed token.
QPointer<KSyntaxHighlighting::Repository> g_repository;
QHash<QString, KSyntaxHighlighting::Definition> g_definitionCache;
KSyntaxHighlighting::Theme g_theme;
QString g_themeName;
bool g_resolvedTheme = false;

// Models rarely emit the canonical definition name. These are the spellings
// worth accepting; anything else falls through to the repository's own matching.
const QHash<QString, QString> &languageAliases()
{
    static const QHash<QString, QString> aliases = {
        {QStringLiteral("c++"), QStringLiteral("C++")},
        {QStringLiteral("cpp"), QStringLiteral("C++")},
        {QStringLiteral("cc"), QStringLiteral("C++")},
        {QStringLiteral("hpp"), QStringLiteral("C++")},
        {QStringLiteral("c"), QStringLiteral("C")},
        {QStringLiteral("h"), QStringLiteral("C")},
        {QStringLiteral("js"), QStringLiteral("JavaScript")},
        {QStringLiteral("jsx"), QStringLiteral("JavaScript")},
        {QStringLiteral("mjs"), QStringLiteral("JavaScript")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript")},
        {QStringLiteral("tsx"), QStringLiteral("TypeScript")},
        {QStringLiteral("py"), QStringLiteral("Python")},
        {QStringLiteral("python3"), QStringLiteral("Python")},
        {QStringLiteral("rb"), QStringLiteral("Ruby")},
        {QStringLiteral("sh"), QStringLiteral("Bash")},
        {QStringLiteral("shell"), QStringLiteral("Bash")},
        {QStringLiteral("zsh"), QStringLiteral("Bash")},
        {QStringLiteral("console"), QStringLiteral("Bash")},
        {QStringLiteral("yml"), QStringLiteral("YAML")},
        {QStringLiteral("md"), QStringLiteral("Markdown")},
        {QStringLiteral("htm"), QStringLiteral("HTML")},
        {QStringLiteral("xml"), QStringLiteral("XML")},
        {QStringLiteral("kt"), QStringLiteral("Kotlin")},
        {QStringLiteral("rs"), QStringLiteral("Rust")},
        {QStringLiteral("golang"), QStringLiteral("Go")},
        {QStringLiteral("dockerfile"), QStringLiteral("Dockerfile")},
        {QStringLiteral("makefile"), QStringLiteral("Makefile")},
        {QStringLiteral("cmake"), QStringLiteral("CMake")},
        {QStringLiteral("sql"), QStringLiteral("SQL")},
        {QStringLiteral("diff"), QStringLiteral("Diff")},
        {QStringLiteral("patch"), QStringLiteral("Diff")},
        {QStringLiteral("text"), QStringLiteral("")},
        {QStringLiteral("plain"), QStringLiteral("")},
        {QStringLiteral("txt"), QStringLiteral("")},
    };
    return aliases;
}

KSyntaxHighlighting::Repository *repository()
{
    if (!g_repository) {
        g_repository = new KSyntaxHighlighting::Repository();
        g_definitionCache.clear();
    }
    return g_repository.data();
}

// Resolves a definition, tolerating the many case and separator spellings.
KSyntaxHighlighting::Definition definitionFor(const QString &language)
{
    const QString key = language.trimmed();
    if (key.isEmpty()) {
        return {};
    }
    const auto cached = g_definitionCache.constFind(key);
    if (cached != g_definitionCache.constEnd()) {
        return cached.value();
    }

    KSyntaxHighlighting::Repository *repo = repository();
    KSyntaxHighlighting::Definition definition;

    const QString canonical = normaliseLanguage(key);
    if (!canonical.isEmpty()) {
        definition = repo->definitionForName(canonical);
    }
    if (!definition.isValid() && canonical != key) {
        definition = repo->definitionForName(key);
    }
    if (!definition.isValid()) {
        // Last resort: the hint may be a file name or a suffix ("main.cpp").
        definition = repo->definitionForFileName(key);
    }
    g_definitionCache.insert(key, definition);
    return definition;
}

KSyntaxHighlighting::Theme theme()
{
    if (g_resolvedTheme) {
        return g_theme;
    }
    KSyntaxHighlighting::Repository *repo = repository();
    KSyntaxHighlighting::Theme selected;
    if (!g_themeName.isEmpty()) {
        // An explicitly named theme wins.
        for (const KSyntaxHighlighting::Theme &candidate : repo->themes()) {
            if (candidate.name().compare(g_themeName, Qt::CaseInsensitive) == 0) {
                selected = candidate;
                break;
            }
        }
    }
    if (!selected.isValid()) {
        // Dark, because the chat panel is dark; the light default would be
        // unreadable on it.
        selected = repo->defaultTheme(KSyntaxHighlighting::Repository::DarkTheme);
    }
    if (!selected.isValid()) {
        selected = repo->defaultTheme(KSyntaxHighlighting::Repository::LightTheme);
    }
    g_theme = selected;
    g_resolvedTheme = true;
    return g_theme;
}

QString styleFor(const KSyntaxHighlighting::Theme &t, const KSyntaxHighlighting::Format &format)
{
    const auto style = format.textStyle();
    if (!style) {
        return QString();
    }
    QString css = QStringLiteral("color:%1").arg(QColor::fromRgb(t.textColor(style)).name());
    if (t.isBold(style)) {
        css += u";font-weight:bold"_s;
    }
    if (t.isItalic(style)) {
        css += u";font-style:italic"_s;
    }
    if (t.isUnderline(style)) {
        css += u";text-decoration:underline"_s;
    }
    return css;
}

// Collects the formats AbstractHighlighter emits and turns them into HTML
// spans. Consecutive runs of the same format are coalesced, otherwise a block
// of uniform code emits one <span> per character.
class HtmlHighlighter : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    void highlightToHtml(const QString &code, KSyntaxHighlighting::Theme t)
    {
        setTheme(t);
        m_parts.clear();
        // AbstractHighlighter reports offsets into the text it is given, so the
        // buffer has to stay alive for the whole run.
        m_buffer = code;
        KSyntaxHighlighting::State state;
        int pos = 0;
        while (pos <= m_buffer.size()) {
            const int nl = m_buffer.indexOf(QLatin1Char('\n'), pos);
            const int end = nl < 0 ? m_buffer.size() : nl;
            m_lineBase = pos;
            if (end > pos || nl >= 0) {
                state = highlightLine(QStringView(m_buffer).mid(pos, end - pos), state);
            }
            if (nl < 0) {
                break;
            }
            // The newline itself needs no part: takeHtml() fills the gaps
            // between reported formats straight from the buffer.
            pos = nl + 1;
            if (pos == m_buffer.size()) {
                break;
            }
        }
    }

    // Runs the highlighter and returns the HTML body in one step.
    QString highlightToHtmlBody(const QString &code, KSyntaxHighlighting::Theme t)
    {
        highlightToHtml(code, t);
        return takeHtml(t);
    }

    QString takeHtml(const KSyntaxHighlighting::Theme &t) const
    {
        QString out;
        out.reserve(m_buffer.size() * 2);
        // applyFormat() is only called where there IS a format; everything
        // else (plain identifiers, whitespace, the line breaks between the
        // reported ranges) has to be filled in from the buffer, or the block
        // renders with most of its text missing.
        int cursor = 0;
        for (const Part &part : m_parts) {
            if (part.offset > cursor) {
                out += m_buffer.mid(cursor, part.offset - cursor).toHtmlEscaped();
            }
            const QString text = m_buffer.mid(part.offset, part.length).toHtmlEscaped();
            const QString style = styleFor(t, part.format);
            out += style.isEmpty() ? text : QStringLiteral("<span style=\"%1\">%2</span>").arg(style, text);
            cursor = qMax(cursor, part.offset + part.length);
        }
        if (cursor < m_buffer.size()) {
            out += m_buffer.mid(cursor).toHtmlEscaped();
        }
        return out;
    }

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        if (length <= 0) {
            return;
        }
        // applyFormat() reports offsets relative to the line handed to
        // highlightLine(), so they have to be rebased onto the whole buffer
        // before they can index into it.
        const int absolute = m_lineBase + offset;
        if (!m_parts.isEmpty()) {
            Part &last = m_parts.last();
            // Merge adjacent runs of the same format; otherwise a block of
            // uniform code emits one <span> per character.
            if (last.format.id() == format.id() && last.offset + last.length == absolute) {
                last.length += length;
                return;
            }
        }
        m_parts << Part{absolute, length, format};
    }

private:
    struct Part {
        int offset = 0;
        int length = 0;
        KSyntaxHighlighting::Format format;
    };

    QString m_buffer;
    int m_lineBase = 0;
    QList<Part> m_parts;
};

QString styleFor(KSyntaxHighlighting::Theme t, const KSyntaxHighlighting::Format &format)
{
    const auto style = format.textStyle();
    if (!style) {
        return QString();
    }
    QString css = QStringLiteral("color:%1").arg(QColor::fromRgb(t.textColor(style)).name());
    if (t.isBold(style)) {
        css += u";font-weight:bold"_s;
    }
    if (t.isItalic(style)) {
        css += u";font-style:italic"_s;
    }
    if (t.isUnderline(style)) {
        css += u";text-decoration:underline"_s;
    }
    return css;
}

} // namespace

bool isAvailable()
{
    return repository()->definitions().size() > 0;
}

int availableDefinitionCount()
{
    return repository()->definitions().size();
}

QString normaliseLanguage(const QString &language)
{
    const QString key = language.trimmed().toLower();
    if (key.isEmpty()) {
        return QString();
    }
    const auto it = languageAliases().constFind(key);
    if (it != languageAliases().constEnd()) {
        return it.value();
    }
    return language.trimmed();
}

void setThemeName(const QString &themeName)
{
    g_themeName = themeName.trimmed();
    g_resolvedTheme = false;
    g_theme = KSyntaxHighlighting::Theme();
}

QString themeName()
{
    return g_themeName;
}

QString htmlForCode(const QString &code, const QString &language)
{
    const KSyntaxHighlighting::Definition def = definitionFor(language);
    const KSyntaxHighlighting::Theme t = theme();

    // An unknown (or absent) language still gets the block treatment, just
    // without colours, so a sample in an exotic language is not left as loose
    // monospace text on the panel.
    QString body;
    if (def.isValid() && t.isValid()) {
        HtmlHighlighter highlighter;
        highlighter.setTheme(t);
        highlighter.setDefinition(def);
        body = highlighter.highlightToHtmlBody(code, t);
    }
    if (body.isEmpty()) {
        body = code.toHtmlEscaped();
    }

    const QString shown = language.trimmed().isEmpty() ? def.name() : language.trimmed();
    // A table is used as the container because QTextDocument honours cell
    // backgrounds reliably, while <pre> backgrounds and borders are largely
    // ignored by its CSS subset. Two stacked rows give one continuous block
    // with the language label as a header strip.
    return QStringLiteral(
               "<table class=\"codeblock\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" "
               "style=\"margin-top:6px; margin-bottom:6px;\">"
               "<tr><td class=\"codeblock-lang\" bgcolor=\"%1\" "
               "style=\"color:%2; font-size:10px; font-weight:600; padding:5px 10px;\">%3</td></tr>"
               "<tr><td bgcolor=\"%1\" style=\"padding:2px 10px 8px 10px;\">"
               "<pre style=\"margin:0;\">%4</pre></td></tr>"
               "</table>")
        .arg(ChatTheme::codeBlockBg(), ChatTheme::textMuted(), shown.toHtmlEscaped(), body);
}

QString htmlBodyForCode(const QString &code, const QString &language)
{
    const KSyntaxHighlighting::Definition def = definitionFor(language);
    const KSyntaxHighlighting::Theme t = theme();

    QString body;
    if (def.isValid() && t.isValid()) {
        HtmlHighlighter highlighter;
        highlighter.setTheme(t);
        highlighter.setDefinition(def);
        body = highlighter.highlightToHtmlBody(code, t);
    }
    if (body.isEmpty()) {
        body = code.toHtmlEscaped();
    }
    return body;
}

} // namespace CodeHighlight
} // namespace KateAi