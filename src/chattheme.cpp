/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "chattheme.h"

#include <QColor>
#include <QHash>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace ChatTheme
{

namespace
{

// Surfaces: each step is a little lighter than the one behind it, which is
// what makes the composer read as "on top of" the transcript without needing a
// drop shadow.
QString kPanelBg = QStringLiteral("#1e1e1e");
QString kSurfaceBg = QStringLiteral("#242426");
QString kCardBg = QStringLiteral("#28282b");
// One step below the card, so a code block reads as inset rather than as
// another card on the panel.
QString kCodeBlockBg = QStringLiteral("#151517");
QString kInputBg = QStringLiteral("#2d2d31");
QString kHoverBg = QStringLiteral("#323236");

// Tool cards. The body sits clearly above the panel backdrop, and the head box
// one step above the body, so a tool call is legible as a box while you scroll
// past a wall of assistant text.
QString kToolBg = QStringLiteral("#26262b");
QString kToolHeaderBg = QStringLiteral("#33333a");

// Agent-task cards. One step above the tool-card surface and ringed on all four
// sides, so a delegated sub-agent reads as its own object rather than as
// another tool row in the log.
QString kAgentTaskBg = QStringLiteral("#2b2b32");
QString kIntentRowBg = QStringLiteral("#3a3020");

QString kBorder = QStringLiteral("#37373b");
QString kBorderStrong = QStringLiteral("#4a4a50");

QString kTextPrimary = QStringLiteral("#d4d4d6");
QString kTextMuted = QStringLiteral("#8b8b92");

QString kAccent = QStringLiteral("#3d7eff");
QString kAccentHover = QStringLiteral("#5590ff");
QString kSuccess = QStringLiteral("#4ec9a0");
QString kWarning = QStringLiteral("#e2b341");
QString kDanger = QStringLiteral("#f26d6d");

} // namespace

QString panelBg()
{
    return kPanelBg;
}
QString surfaceBg()
{
    return kSurfaceBg;
}
QString cardBg()
{
    return kCardBg;
}
// Inset surface for code blocks; exposed because the highlighter emits it as
// a table cell background, where QTextDocument ignores the stylesheet.
QString codeBlockBg()
{
    return kCodeBlockBg;
}
QString inputBg()
{
    return kInputBg;
}
QString hoverBg()
{
    return kHoverBg;
}

QString toolBg()
{
    return kToolBg;
}

QString toolHeaderBg()
{
    return kToolHeaderBg;
}

QString intentDock()
{
    // Chrome, not a card: no border and no fill of its own, so the rows inside
    // it are what read as boxes. Deliberately no descendant rule here -- a rule
    // matching the dock's children would flatten the borders on the very cards
    // the dock exists to frame.
    return QStringLiteral(
           "QWidget#intentDock {"
           "  background: transparent;"
           "  border: none;"
           "}");
}

QString agentTaskCard()
{
    return QStringLiteral(
           "QWidget#subtaskWidget {"
           "  background-color: %1;"
           "  border: 1px solid %2;"
           "  border-radius: 8px;"
           "}")
        .arg(kAgentTaskBg, kBorder);
}

QString intentApprovalRow()
{
    // Warm, high-contrast box: this is the one thing in the strip that stops the
    // agent, so it must not be mistakable for a status line.
    return QStringLiteral(
           "QWidget#approvalRow {"
           "  background-color: %1;"
           "  border: 1px solid %2;"
           "  border-radius: 8px;"
           "}")
        .arg(kIntentRowBg, kWarning);
}

QString border()
{
    return kBorder;
}
QString borderStrong()
{
    return kBorderStrong;
}

QString textPrimary()
{
    return kTextPrimary;
}
QString textMuted()
{
    return kTextMuted;
}

QString accent()
{
    return kAccent;
}
QString accentHover()
{
    return kAccentHover;
}
QString success()
{
    return kSuccess;
}
QString warning()
{
    return kWarning;
}
QString danger()
{
    return kDanger;
}

QString panel()
{
    return QStringLiteral("QWidget { background-color: %1; color: %2; }").arg(kPanelBg, kTextPrimary);
}

QString scrollArea()
{
    return QStringLiteral(
           "QScrollArea {"
           "  background-color: %1;"
           "  border: none;"
           "}"
           "%2")
        .arg(kPanelBg, scrollBar());
}

QString scrollBar()
{
    return QStringLiteral(
           "QScrollBar:vertical {"
           "  background: transparent;"
           "  width: 10px;"
           "  margin: 2px;"
           "}"
           "QScrollBar::handle:vertical {"
           "  background: %1;"
           "  border-radius: 4px;"
           "  min-height: 28px;"
           "}"
           "QScrollBar::handle:vertical:hover {"
           "  background: %2;"
           "}"
           "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
           "  height: 0;"
           "}"
           "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
           "  background: transparent;"
           "}")
        .arg(kBorderStrong, kTextMuted);
}

QString transcript()
{
    return QStringLiteral("QWidget#transcript { background-color: %1; }").arg(kPanelBg);
}

QString headerBar()
{
    return QStringLiteral(
           "QWidget#headerBar {"
           "  background-color: %1;"
           "  border-bottom: 1px solid %2;"
           "}")
        .arg(kPanelBg, kBorder);
}

QString chipButton()
{
    // Pill-shaped, quiet until hovered. Model and mode live next to the input,
    // so they should read as selectable chips rather than toolbar buttons.
    return QStringLiteral(
           "QPushButton {"
           "  background-color: %1;"
           "  color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 13px;"
           "  padding: 3px 11px;"
           "  font-size: 12px;"
           "  font-weight: 500;"
           "  text-align: left;"
           "}"
           "QPushButton:hover {"
           "  background-color: %4;"
           "  border-color: %5;"
           "  color: #ffffff;"
           "}"
           "QPushButton:pressed {"
           "  background-color: %5;"
           "}"
           "QPushButton:disabled {"
           "  color: #6a6a72;"
           "  border-color: %3;"
           "}")
        .arg(kCardBg, kTextPrimary, kBorder, kHoverBg, kBorderStrong);
}

QString iconButton()
{
    return QStringLiteral(
           "QPushButton {"
           "  background: transparent;"
           "  color: %1;"
           "  border: 1px solid transparent;"
           "  border-radius: 6px;"
           "}"
           "QPushButton:hover {"
           "  background-color: %2;"
           "  border-color: %3;"
           "}"
           "QPushButton:pressed {"
           "  background-color: %3;"
           "}"
           "QPushButton:disabled {"
           "  color: #55555c;"
           "}")
        .arg(kTextMuted, kHoverBg, kCardBg);
}

QString composerCard()
{
    // Sits on the panel with a one-pixel lift. The accent border on focus is
    // the panel's only "you are typing here" cue, so it stays quiet until then.
    return QStringLiteral(
           "QWidget#composerCard {"
           "  background-color: %1;"
           "  border: 1px solid %2;"
           "  border-radius: 12px;"
           "}"
           "QWidget#composerCard:focus {"
           "  border: 1px solid %3;"
           "}")
        .arg(kSurfaceBg, kBorder, kAccent);
}

QString promptInput()
{
    return QStringLiteral(
           "QPlainTextEdit {"
           "  background: transparent;"
           "  color: %1;"
           "  border: none;"
           "  padding: 4px 6px;"
           "  font-size: 13px;"
           "  selection-background-color: %2;"
           "  selection-color: #ffffff;"
           "}"
           "QPlainTextEdit:focus {"
           "  border: none;"
           "}")
        .arg(kTextPrimary, kAccent);
}

QString hintLabel()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(kTextMuted);
}

QString tokenLabel()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(kTextMuted);
}

QString infoBar()
{
    return QStringLiteral(
           "QLabel {"
           "  color: %1;"
           "  font-size: 12px;"
           "  font-weight: 500;"
           "  padding: 7px 11px;"
           "  background-color: rgba(242, 109, 109, 0.10);"
           "  border: 1px solid rgba(242, 109, 109, 0.35);"
           "  border-radius: 8px;"
           "}")
        .arg(kDanger);
}

QString jumpToLatest()
{
    return QStringLiteral(
           "QPushButton {"
           "  background-color: %1;"
           "  color: #ffffff;"
           "  border: 1px solid %2;"
           "  border-radius: 13px;"
           "  padding: 5px 13px;"
           "  font-size: 11px;"
           "  font-weight: 600;"
           "}"
           "QPushButton:hover {"
           "  background-color: %2;"
           "  border-color: %3;"
           "}")
        .arg(kAccent, kAccentHover, kBorderStrong);
}

QString activityPill()
{
    return QStringLiteral(
           "QLabel {"
           "  color: %1;"
           "  font-size: 11px;"
           "  padding: 3px 10px;"
           "  background-color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 11px;"
           "}")
        .arg(kTextMuted, kSurfaceBg, kBorder);
}

QString thinkingPill()
{
    return QStringLiteral(
           "QLabel {"
           "  color: #b4d0ff;"
           "  font-size: 12px;"
           "  font-weight: 600;"
           "  padding: 5px 12px;"
           "  background-color: rgba(61, 126, 255, 0.22);"
           "  border: 1px solid %1;"
           "  border-radius: 13px;"
           "}")
        .arg(kAccent);
}

QString workingPill()
{
    return QStringLiteral(
           "QLabel {"
           "  color: #f3d48a;"
           "  font-size: 12px;"
           "  font-weight: 600;"
           "  padding: 5px 12px;"
           "  background-color: rgba(226, 179, 65, 0.20);"
           "  border: 1px solid %1;"
           "  border-radius: 13px;"
           "}")
        .arg(kWarning);
}

QString userCard()
{
    // User turns are deliberately neutral. Blue is reserved for actions and
    // focus elsewhere in the panel, so it should not make every prompt look
    // like an actionable control. Keep the surface dark enough that its white
    // text has the same contrast as the rest of the transcript.
    return QStringLiteral(
           "QWidget#userCard {"
           "  background-color: #3a3a3d;"
           "  border: 1px solid #505055;"
           "  border-left: 3px solid #686870;"
           "  border-radius: 8px;"
           "}");
}

QString assistantCard()
{
    return QStringLiteral(
           "QWidget#assistantCard {"
           "  background: transparent;"
           "  border: none;"
           "}")
        .arg(kPanelBg);
}

QString sectionLabel()
{
    return QStringLiteral(
           "QLabel {"
           "  color: %1;"
           "  font-size: 11px;"
           "  font-weight: 600;"
           "  letter-spacing: 0.4px;"
           "}")
        .arg(kTextMuted);
}

QString mixColors(const QString &hex, const QString &onto, int percent)
{
    const QColor top(hex);
    QColor base(onto);
    if (!top.isValid() || !base.isValid()) {
        return onto;
    }
    const qreal t = qBound(0, percent, 100) / 100.0;
    const QColor mixed(
        qRound(base.red() + (top.red() - base.red()) * t),
        qRound(base.green() + (top.green() - base.green()) * t),
        qRound(base.blue() + (top.blue() - base.blue()) * t));
    return mixed.name();
}

QString toolHeader(const QString &toolName)
{
    // The head box carries the tool's identity, because it is the only part of
    // the card visible while collapsed: the hue is blended into the surface
    // rather than painted on it, so the label on top stays readable. The top
    // radii match the card's so the two read as one object.
    const QString headBg = mixColors(toolColor(toolName), kToolHeaderBg, 16);
    const QString hoverBg = mixColors(toolColor(toolName), kToolHeaderBg, 30);
    return QStringLiteral(
           "QWidget#toolHeader {"
           "  background-color: %1;"
           "  border: none;"
           "  border-bottom: 1px solid %2;"
           "  border-top-left-radius: 6px;"
           "  border-top-right-radius: 6px;"
           "}"
           "QWidget#toolHeader:hover {"
           "  background-color: %3;"
           "}")
        .arg(headBg, kBorder, hoverBg);
}

QString messageCss()
{
    return QStringLiteral(
           "body { color: %1; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
           "p { margin-bottom: 8px; line-height: 1.5; }"
           "ul, ol { margin-bottom: 8px; padding-left: 20px; }"
           "li { margin-bottom: 4px; }"
           // Fenced code is emitted by CodeHighlight as a two-row table -- a
           // language strip over a <pre class="codeblock">. Qt's rich text
           // engine honours cell backgrounds and table borders but ignores most
           // block-level CSS, so these selectors match that markup. Rules that
           // name only `pre.codeblock`/`span.codeblock-lang` never match and
           // leave the block unboxed and the code uncoloured.
           "table.codeblock { border: 1px solid %3; border-radius: 6px; margin: 8px 0; background-color: %2; }"
           "td.codeblock-lang { color: %5; font-family: sans-serif; font-size: 10px; font-weight: 600;"
           "                    background-color: %2; padding: 4px 10px; }"
           "pre.codeblock { color: %1; font-family: monospace; font-size: 12px; white-space: pre-wrap;"
           "                 background-color: %2; margin: 6px 10px; }"
           "code { font-family: monospace; font-size: 12px; background-color: %6; color: %1;"
           "       padding: 2px 5px; border-radius: 3px; }"
           "blockquote { border-left: 3px solid %4; padding-left: 10px; color: %5; margin: 8px 0; }"
           "a { color: %4; text-decoration: none; }")
        .arg(kTextPrimary, kCodeBlockBg, kBorder, kAccent, kTextMuted, kHoverBg);
}

QString thinkingCss()
{
    return QStringLiteral(
           "body { color: %1; font-weight: 600; font-size: 12px; margin: 0; padding: 0; background: transparent; }"
           "p { color: %1; font-weight: 600; margin-bottom: 4px; }")
        .arg(kTextPrimary);
}

QString toggleLink()
{
    return QStringLiteral(
           "QPushButton {"
           "  color: %1;"
           "  font-size: 11px;"
           "  font-weight: 700;"
           "  border: none;"
           "  text-align: left;"
           "  padding: 2px 0;"
           "}"
           "QPushButton:hover { color: %1; }")
        .arg(kTextPrimary);
}

QString roleHeader()
{
    return QStringLiteral("QLabel { color: %1; font-size: 10px; font-weight: 600; letter-spacing: 0.6px;"
                          " border: none; background: transparent; }")
        .arg(kTextMuted);
}

QString messageText()
{
    return QStringLiteral("QLabel { color: %1; font-size: 13px; border: none; background: transparent; }")
        .arg(kTextPrimary);
}

QString planChecklist()
{
    return QStringLiteral(
           "QCheckBox { color: %1; font-size: 12px; spacing: 8px; }"
           "QCheckBox::indicator { width: 14px; height: 14px; }")
        .arg(kTextPrimary);
}

QString toolLabel(const QString &toolName)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("read_file"), QStringLiteral("Read file")},
        {QStringLiteral("write_file"), QStringLiteral("Write file")},
        {QStringLiteral("edit_file"), QStringLiteral("Edit file")},
        {QStringLiteral("multi_edit_file"), QStringLiteral("Edit file")},
        {QStringLiteral("multi_replace_file_content"), QStringLiteral("Edit file")},
        {QStringLiteral("list_dir"), QStringLiteral("List directory")},
        {QStringLiteral("grep"), QStringLiteral("Search")},
        {QStringLiteral("glob"), QStringLiteral("Find files")},
        {QStringLiteral("bash"), QStringLiteral("Run command")},
        {QStringLiteral("new_task"), QStringLiteral("Delegate")},
        {QStringLiteral("write_plan_file"), QStringLiteral("Write plan file")},
        {QStringLiteral("suggest"), QStringLiteral("Suggestion")},
        {QStringLiteral("question"), QStringLiteral("Question")},
        {QStringLiteral("attempt_completion"), QStringLiteral("Finish")},
        {QStringLiteral("ask_followup_question"), QStringLiteral("Ask")},
        {QStringLiteral("fetch_rules"), QStringLiteral("Load rules")},
        {QStringLiteral("update_project_graph"), QStringLiteral("Index project")},
        {QStringLiteral("project_graph"), QStringLiteral("Read project map")},
    };

    const auto it = labels.constFind(toolName);
    if (it != labels.constEnd()) {
        return it.value();
    }
    // MCP tools arrive as mcp__server__tool.
    if (toolName.startsWith(QLatin1String("mcp__"))) {
        return QStringLiteral("MCP tool");
    }
    if (toolName.isEmpty()) {
        return QStringLiteral("Tool");
    }
    return toolName;
}

QString toolAccent(const QString &toolName, bool running, bool ok)
{
    if (running) {
        return kWarning;
    }
    if (!ok) {
        return kDanger;
    }
    if (toolName == QLatin1String("write_file") || toolName == QLatin1String("edit_file")
        || toolName == QLatin1String("multi_edit_file")
        || toolName == QLatin1String("multi_replace_file_content")
        || toolName == QLatin1String("bash")) {
        // Anything that changes the workspace stays visually distinct even on
        // success, so a glance at the transcript shows what was touched.
        return kAccent;
    }
    return kSuccess;
}

QString toolColor(const QString &toolName)
{
    // One hue per tool family. Reads are cool and quiet, writes green, edits
    // violet, and anything that runs or delegates warm -- the card rail, icon
    // and tool-name prefix all take this, so a long transcript of cards can be
    // scanned by colour before a single word is read.
    static const QHash<QString, QString> colors = {
        {QStringLiteral("read_file"), QStringLiteral("#56b6c2")},
        {QStringLiteral("fs/read_text_file"), QStringLiteral("#56b6c2")},
        {QStringLiteral("write_file"), QStringLiteral("#98c379")},
        {QStringLiteral("fs/write_text_file"), QStringLiteral("#98c379")},
        {QStringLiteral("edit_file"), QStringLiteral("#c678dd")},
        {QStringLiteral("multi_edit_file"), QStringLiteral("#c678dd")},
        {QStringLiteral("multi_replace_file_content"), QStringLiteral("#c678dd")},
        {QStringLiteral("list_dir"), QStringLiteral("#d19a66")},
        {QStringLiteral("grep"), QStringLiteral("#e5c07b")},
        {QStringLiteral("glob"), QStringLiteral("#61afef")},
        {QStringLiteral("bash"), QStringLiteral("#e06c75")},
        {QStringLiteral("web_search"), QStringLiteral("#5ab0d8")},
        {QStringLiteral("web_fetch"), QStringLiteral("#5ab0d8")},
        {QStringLiteral("query_project_graph"), QStringLiteral("#a3be8c")},
        {QStringLiteral("write_plan_file"), QStringLiteral("#ebcb8b")},
        {QStringLiteral("suggest"), QStringLiteral("#b48ead")},
        {QStringLiteral("question"), QStringLiteral("#3d7eff")},
        {QStringLiteral("goal_report"), QStringLiteral("#98c379")},
        {QStringLiteral("new_task"), QStringLiteral("#88c0d0")},
        {QStringLiteral("fetch_rules"), QStringLiteral("#a3be8c")},
    };

    const auto it = colors.constFind(toolName);
    if (it != colors.constEnd()) {
        return it.value();
    }
    if (toolName.startsWith(QLatin1String("mcp__"))) {
        // MCP tools are a different kind of thing: a server the user configured,
        // not a built-in. One shared colour marks them as such.
        return QStringLiteral("#8fbcbb");
    }
    if (toolName.startsWith(QLatin1String("terminal"))) {
        return QStringLiteral("#e06c75");
    }
    return kTextMuted;
}

QString toolCardBg(const QString &toolName)
{
    return mixColors(toolColor(toolName), kToolBg, 10);
}

} // namespace ChatTheme
} // namespace KateAi
