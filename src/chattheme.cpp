/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "chattheme.h"

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
    // Deliberately not a coloured badge: this line changes constantly while a
    // turn runs, so it stays low-contrast and lets the tool cards below it
    // carry the actual signal.
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

QString userCard()
{
    // A left accent rail instead of a full border: it marks the turn boundary
    // without boxing the text in.
    return QStringLiteral(
           "QWidget#userCard {"
           "  background-color: %1;"
           "  border: none;"
           "  border-left: 2px solid %2;"
           "  border-radius: 4px;"
           "}")
        .arg(kCardBg, kAccent);
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

QString toolHeader()
{
    // The head box is a solid slab, not a tint: it is the only part of a tool
    // card visible while collapsed, so it has to carry the tool's identity on
    // its own. The top radii match the card's so the two read as one object.
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
        .arg(kToolHeaderBg, kBorder, kHoverBg);
}

QString messageCss()
{
    return QStringLiteral(
           "body { color: %1; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
           "p { margin-bottom: 8px; line-height: 1.5; }"
           "ul, ol { margin-bottom: 8px; padding-left: 20px; }"
           "li { margin-bottom: 4px; }"
           // Code blocks get their own surface: a step darker than the card and
           // distinct from the panel behind it, so a long sample reads as a
           // separate object rather than as more prose.
           "pre.codeblock {"
           "  background-color: %2;"
           "  color: %1;"
           "  padding: 10px 12px;"
           "  margin: 10px 0;"
           "  border: 1px solid %3;"
           "  border-left: 3px solid %4;"
           "  border-radius: 8px;"
           "  font-family: monospace;"
           "  font-size: 12px;"
           "  white-space: pre-wrap;"
           "}"
           "span.codeblock-lang {"
           "  display: block;"
           "  color: %5;"
           "  font-family: sans-serif;"
           "  font-size: 10px;"
           "  font-weight: 600;"
           "  letter-spacing: 0.5px;"
           "  margin-bottom: 6px;"
           "}"
           "code { font-family: monospace; font-size: 12px; background-color: %6; color: %1;"
           "       padding: 2px 5px; border-radius: 3px; }"
           "blockquote { border-left: 3px solid %4; padding-left: 10px; color: %5; margin: 8px 0; }"
           "a { color: %4; text-decoration: none; }")
        .arg(kTextPrimary, kCodeBlockBg, kBorder, kAccent, kTextMuted, kHoverBg);
}

QString thinkingCss()
{
    return QStringLiteral(
           "body { color: %1; font-style: italic; font-size: 12px; margin: 0; padding: 0; background: transparent; }"
           "p { color: %1; margin-bottom: 4px; }")
        .arg(kTextMuted);
}

QString toggleLink()
{
    return QStringLiteral(
           "QPushButton {"
           "  color: %1;"
           "  font-size: 11px;"
           "  font-weight: 600;"
           "  border: none;"
           "  text-align: left;"
           "  padding: 2px 0;"
           "}"
           "QPushButton:hover { color: #ffffff; }")
        .arg(kTextMuted);
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

} // namespace ChatTheme
} // namespace KateAi