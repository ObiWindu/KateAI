/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "chattheme.h"

#include "codehighlight.h"

#include <KLocalizedString>
#include <KTextEditor/Editor>
#include <KSyntaxHighlighting/Theme>

#include <QColor>
#include <QHash>
#include <QJsonDocument>
#include <QList>
#include <QPair>
#include <QPalette>

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

// Tool card surfaces. Fixed and independent of the theme palette: a tool card
// is identified by its hue (rail, icon, header tint), and that identity must
// survive a theme switch. The body sits clearly above the panel backdrop, and
// the head box one step above the body, so a tool call is legible as a box
// while you scroll past a wall of assistant text.
QString kToolBg = QStringLiteral("#26262b");
QString kToolHeaderBg = QStringLiteral("#33333a");

// Agent-task cards. One step above the surrounding surface and ringed on all
// four sides, so a delegated sub-agent reads as its own object rather than as
// another tool row in the log.
QString kAgentTaskBg = QStringLiteral("#463a10");
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

// Window-chrome colours: the tool view, the header bar, the composer strip and
// the intent dock. Initialised to the panel equivalents. In inherit mode they
// mirror the editor-derived panel colours (both follow the Kate editor window
// theme); under a built-in preset they mirror the preset's panel colours.
QString kWindowBg = kPanelBg;
QString kWindowText = kTextPrimary;
QString kWindowTextMuted = kTextMuted;
QString kWindowBorder = kBorder;
QString kWindowHover = kHoverBg;

// Fixed snapshots of the defaults above, captured at static initialisation --
// before loadTheme() can ever rewrite the globals. The tool card never
// follows the theme palette: its rail, icon, status marks, text, result
// surface and approval chrome keep the default look in every theme, so a
// transcript can be scanned by colour whether the panel is dark or light.
const QString kFixedCardBg = kCardBg;
const QString kFixedHoverBg = kHoverBg;
const QString kFixedCodeBlockBg = kCodeBlockBg;
const QString kFixedToolBg = kToolBg;
const QString kFixedToolHeaderBg = kToolHeaderBg;
const QString kFixedIntentRowBg = kIntentRowBg;
const QString kFixedBorder = kBorder;
const QString kFixedTextPrimary = kTextPrimary;
const QString kFixedTextMuted = kTextMuted;
const QString kFixedAccent = kAccent;
const QString kFixedAccentHover = kAccentHover;
const QString kFixedSuccess = kSuccess;
const QString kFixedWarning = kWarning;
const QString kFixedDanger = kDanger;

// True when the colour reads as a dark surface, which decides whether
// surfaces step towards white (dark themes) or towards black (light themes)
// and whether text on a surface is white or the theme's foreground.
bool isDarkColor(const QString &hex)
{
    const QColor c(hex);
    if (!c.isValid()) {
        return true;
    }
    return c.lightnessF() < 0.5;
}

QString lighten(const QString &hex, int percent)
{
    return mixColors(QStringLiteral("#ffffff"), hex, percent);
}

QString darken(const QString &hex, int percent)
{
    return mixColors(QStringLiteral("#000000"), hex, percent);
}

// Installs a whole palette into the globals the accessors read.
void setPalette(const ChatPalette &p)
{
    kPanelBg = p.panelBg;
    kSurfaceBg = p.surfaceBg;
    kCardBg = p.cardBg;
    kCodeBlockBg = p.codeBlockBg;
    kInputBg = p.inputBg;
    kHoverBg = p.hoverBg;
    kAgentTaskBg = p.agentTaskBg;
    kIntentRowBg = p.intentRowBg;
    kBorder = p.border;
    kBorderStrong = p.borderStrong;
    kTextPrimary = p.textPrimary;
    kTextMuted = p.textMuted;
    kAccent = p.accent;
    kAccentHover = p.accentHover;
    kSuccess = p.success;
    kWarning = p.warning;
    kDanger = p.danger;
    kWindowBg = p.windowBg;
    kWindowText = p.windowText;
    kWindowTextMuted = p.windowTextMuted;
    kWindowBorder = p.windowBorder;
    kWindowHover = p.windowHover;
}

// The Kate editor's current colour scheme, used when the panel inherits its
// colours from the editor. Invalid when there is no editor instance.
KSyntaxHighlighting::Theme editorTheme()
{
    auto *editor = KTextEditor::Editor::instance();
    if (!editor) {
        return KSyntaxHighlighting::Theme();
    }
    return editor->theme();
}

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
    // Chrome, not a card: no border, and the fill is the window chrome colour
    // so the strip reads as an extension of the composer below it rather than
    // as a slice of the transcript. The rows inside it are what read as boxes.
    // Deliberately no descendant rule here -- a rule matching the dock's
    // children would flatten the borders on the very cards the dock exists to
    // frame.
    return QStringLiteral(
           "QWidget#intentDock {"
           "  background-color: %1;"
           "  border: none;"
           "}")
        .arg(kWindowBg);
}

QString agentTaskCard(const QString &objectName)
{
    return QStringLiteral(
           "QWidget#%1 {"
           "  background-color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 8px;"
           "}")
        .arg(objectName, kAgentTaskBg, kBorder);
}

QString intentApprovalRow()
{
    // Warm, high-contrast box: this is the one thing in the strip that stops the
    // agent, so it must not be mistakable for a status line. Pinned to the
    // defaults: the strip is part of a tool card, which never follows the
    // theme palette.
    return QStringLiteral(
           "QWidget#approvalRow {"
           "  background-color: %1;"
           "  border: 1px solid %2;"
           "  border-radius: 8px;"
           "}")
        .arg(kFixedIntentRowBg, kFixedWarning);
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

QString windowBg()
{
    return kWindowBg;
}
QString windowText()
{
    return kWindowText;
}
QString windowTextMuted()
{
    return kWindowTextMuted;
}
QString windowBorder()
{
    return kWindowBorder;
}
QString windowHover()
{
    return kWindowHover;
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
    // The header bar is window chrome: it carries the window-chrome colours,
    // which in inherit mode follow the editor theme together with the panel.
    return QStringLiteral(
           "QWidget#headerBar {"
           "  background-color: %1;"
           "  border-bottom: 1px solid %2;"
           "}")
        .arg(kWindowBg, kWindowBorder);
}

QString headerTitle()
{
    // The thread title sits on the header bar, i.e. on window chrome, so its
    // colour comes from the window-chrome muted text.
    return QStringLiteral(
           "QLabel {"
           "  color: %1;"
           "  font-size: 11px;"
           "  font-weight: 600;"
           "  letter-spacing: 0.4px;"
           "}")
        .arg(kWindowTextMuted);
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
           "  color: %2;"
           "}"
           "QPushButton:pressed {"
           "  background-color: %5;"
           "}"
           "QPushButton:disabled {"
           "  color: %6;"
           "  border-color: %3;"
           "}")
        .arg(kCardBg, kTextPrimary, kBorder, kHoverBg, kBorderStrong, kTextMuted);
}

QString iconButton()
{
    // Disabled: the muted text blended halfway towards the panel, so the
    // dimmed state reads in both dark and light themes instead of carrying
    // a fixed dark gray that vanishes on light panels.
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
           "  color: %4;"
           "}")
        .arg(kTextMuted, kHoverBg, kCardBg, mixColors(kTextMuted, kPanelBg, 50));
}

QString headerIconButton()
{
    // Icon buttons on the header bar sit on window chrome, so their text and
    // hover use the window-chrome colours. Same shape as iconButton(): only
    // the colour source differs.
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
           "  color: %4;"
           "}")
        .arg(kWindowTextMuted, kWindowHover, kWindowBorder, mixColors(kWindowTextMuted, kWindowBg, 50));
}

QString windowHintLabel()
{
    // Hint text that sits on window chrome (the status strip under the input).
    return QStringLiteral("QLabel { color: %1; font-size: 11px; }").arg(kWindowTextMuted);
}

QString windowChrome()
{
    // The tool view that hosts the panel: the KateAI window itself. Painted
    // from the window-chrome colours, which follow the editor theme in inherit
    // mode; the chat widget paints its own content on top of it.
    return QStringLiteral("QWidget#kateaiToolView { background-color: %1; color: %2; }")
        .arg(kWindowBg, kWindowText);
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
           "  selection-color: %3;"
           "}"
           "QPlainTextEdit:focus {"
           "  border: none;"
           "}")
        .arg(kTextPrimary, kAccent, textOn(kAccent));
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
           "  background-color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 8px;"
           "}")
        .arg(kDanger, tinted(kDanger, 0.10), tinted(kDanger, 0.35));
}

QString jumpToLatest()
{
    return QStringLiteral(
           "QPushButton {"
           "  background-color: %1;"
           "  color: %4;"
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
        .arg(kAccent, kAccentHover, kBorderStrong, textOn(kAccent));
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

// One pill sheet. The pulse dims the pill between beats instead of animating
// its opacity through a QGraphicsOpacityEffect: Qt renders a widget that
// carries a graphics effect into an offscreen surface, and colour-emoji glyphs
// -- the brain on the thinking pill is one -- do not survive that trip, they
// come out blank or hollow. Swapping between the bright and dim sheets keeps
// the heartbeat and paints the glyph straight onto the screen.
//
// The pill is bare text on purpose: no background, no border, half the old
// footprint. It floats at the very bottom right of the transcript, where a
// boxed pill would read as a card rather than as a corner tick, and the
// colour alone carries the state.
QString pillSheet(const QString &textColor)
{
    return QStringLiteral(
           "QLabel {"
           "  color: %1;"
           "  font-size: 6px;"
           "  font-weight: 600;"
           "  padding: 2px 6px;"
           "  border: none;"
           "  background: transparent;"
           "}")
        .arg(textColor);
}

// Text colour for a pill built on the given hue. With no background wash the
// colour has to read straight off the panel in both directions: a white tint
// of a dark hue on dark panels (the default blue, the default amber), and the
// hue itself when the hue is light -- an accent inherited from a bright
// editor keyword (Tokyo Night's #bb9af7) tinted towards white would be light
// text on a light panel. The dim phase dims towards the panel instead of
// towards white.
QString pillTextColor(const QString &hueHex, bool dim)
{
    if (!isDarkColor(kPanelBg)) {
        return dim ? mixColors(hueHex, kPanelBg, 20) : hueHex;
    }
    const QColor hue(hueHex);
    if (hue.isValid() && hue.lightnessF() > 0.65) {
        return dim ? mixColors(hueHex, kPanelBg, 25) : hueHex;
    }
    return mixColors(QStringLiteral("#ffffff"), hueHex, dim ? 15 : 30);
}

QString thinkingPill()
{
    // The theme's accent hue, not a hardcoded colour -- the pill must follow
    // the palette when the theme changes.
    return pillSheet(pillTextColor(kAccent, false));
}

QString thinkingPillDim()
{
    return pillSheet(pillTextColor(kAccent, true));
}

QString workingPill()
{
    return pillSheet(pillTextColor(kWarning, false));
}

QString workingPillDim()
{
    return pillSheet(pillTextColor(kWarning, true));
}

QString userCardBg()
{
    // A neutral step up from the panel, derived so a light theme gets a light
    // card instead of the dark bubble the dark palette hard-coded.
    return mixColors(kTextPrimary, kPanelBg, 12);
}

QString userCardText()
{
    return textOn(userCardBg());
}

QString userCard()
{
    // User turns are deliberately neutral. Blue is reserved for actions and
    // focus elsewhere in the panel, so it should not make every prompt look
    // like an actionable control. The surface is derived from the panel and
    // foreground, and the text colour follows the surface, so the card keeps
    // its contrast in both dark and light themes.
    return QStringLiteral(
           "QWidget#userCard {"
           "  background-color: %1;"
           "  border: 1px solid %2;"
           "  border-left: 3px solid %3;"
           "  border-radius: 8px;"
           "}")
        .arg(userCardBg(), mixColors(kTextPrimary, kPanelBg, 22), mixColors(kTextPrimary, kPanelBg, 32));
}

QString assistantCard()
{
    return QStringLiteral(
        "QWidget#assistantCard {"
        "  background: transparent;"
        "  border: none;"
        "}");
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

QString tinted(const QString &hex, double alpha)
{
    QColor c(hex);
    if (!c.isValid()) {
        return hex;
    }
    c.setAlphaF(qBound(0.0, alpha, 1.0));
    return c.name(QColor::HexArgb);
}

QString textOn(const QString &surfaceBg)
{
    if (isDarkColor(surfaceBg)) {
        return QStringLiteral("#ffffff");
    }
    // Light surface. When the primary text is itself dark (light panels) it
    // is the right colour. On a dark panel the primary text is light, so a
    // light surface here is a light accent chip (an editor theme with a
    // bright keyword colour, like Tokyo Night's #bb9af7): light-on-light
    // would be unreadable, and the panel backdrop is the dark counterpart.
    return isDarkColor(kTextPrimary) ? kTextPrimary : kPanelBg;
}

QString menu()
{
    return QStringLiteral(
           "QMenu {"
           "  background-color: %1;"
           "  color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 6px;"
           "  padding: 4px;"
           "}"
           "QMenu::item {"
           "  padding: 6px 18px 6px 12px;"
           "  border-radius: 4px;"
           "}"
           "QMenu::item:selected {"
           "  background-color: %4;"
           "  color: %5;"
           "}"
           "QMenu::separator {"
           "  height: 1px;"
           "  background-color: %3;"
           "  margin: 4px 0;"
           "}")
        .arg(kSurfaceBg, kTextPrimary, kBorder, kAccent, textOn(kAccent));
}

QString popupMenu()
{
    return QStringLiteral(
           "QFrame#modelPopup {"
           "  background-color: %1;"
           "  color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 8px;"
           "}"
           "QListWidget {"
           "  background-color: %1;"
           "  color: %2;"
           "  border: none;"
           "  outline: none;"
           "  padding: 2px;"
           "}"
           "QListWidget::item {"
           "  padding: 5px 10px;"
           "  border-radius: 4px;"
           "}"
           "QListWidget::item:selected {"
           "  background-color: %4;"
           "  color: %7;"
           "}"
           "QListWidget::item:hover:!selected {"
           "  background-color: %5;"
           "}"
           "QPushButton#modelPopupConfig {"
           "  background: transparent;"
           "  color: %6;"
           "  border: none;"
           "  text-align: left;"
           "  padding: 6px 10px;"
           "}"
           "QPushButton#modelPopupConfig:hover {"
           "  color: %2;"
           "  background-color: %5;"
           "  border-radius: 4px;"
           "}")
        .arg(kSurfaceBg, kTextPrimary, kBorder, kAccent, kHoverBg, kTextMuted, textOn(kAccent));
}

QString lineEdit()
{
    return QStringLiteral(
           "QLineEdit {"
           "  background-color: %1;"
           "  color: %2;"
           "  border: 1px solid %3;"
           "  border-radius: 4px;"
           "  padding: 6px 10px;"
           "  font-size: 12px;"
           "}"
           "QLineEdit:focus {"
           "  border-color: %4;"
           "}")
        .arg(kInputBg, kTextPrimary, kBorder, kAccent);
}

QString toolHeader(const QString &toolName)
{
    // The head box carries the tool's identity, because it is the only part of
    // the card visible while collapsed: the hue is blended into the surface
    // rather than painted on it, so the label on top stays readable. The top
    // radii match the card's so the two read as one object.
    const QString toolHue = toolColor(toolName);
    const QString headBg = mixColors(toolHue, kFixedToolHeaderBg, 16);
    const QString hoverBg = mixColors(toolHue, kFixedToolHeaderBg, 30);
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
        .arg(headBg, kFixedBorder, hoverBg);
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

QString userMessageCss()
{
    // messageCss() with the body colour taken from userCardText(): the user
    // card's surface is derived from the palette (dark in dark themes, light
    // in light ones), and a document stylesheet cannot inherit what the widget
    // stylesheet painted, so the text colour must be derived the same way.
    // The code-block surfaces stay the shared inset colour so a pasted sample
    // reads the same on either side of the conversation.
    return QStringLiteral(
               "body { color: %6; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
               "p { margin-bottom: 8px; line-height: 1.5; }"
               "ul, ol { margin-bottom: 8px; padding-left: 20px; }"
               "li { margin-bottom: 4px; }"
               "table.codeblock { border: 1px solid %1; border-radius: 6px; margin: 8px 0; background-color: %2; }"
               "td.codeblock-lang { color: %3; font-family: sans-serif; font-size: 10px; font-weight: 600;"
               "                    background-color: %2; padding: 4px 10px; }"
               "pre.codeblock { color: %6; font-family: monospace; font-size: 12px; white-space: pre-wrap;"
               "                 background-color: %2; margin: 6px 10px; }"
               "code { font-family: monospace; font-size: 12px; background-color: %4; color: %6;"
               "       padding: 2px 5px; border-radius: 3px; }"
               "blockquote { border-left: 3px solid %5; padding-left: 10px; color: %3; margin: 8px 0; }"
               "a { color: %5; text-decoration: none; }")
        .arg(kBorder, kCodeBlockBg, kTextMuted, kHoverBg, kAccent, userCardText());
}

QString thinkingCss()
{
    // The reasoning text is markdown like any message, so the code rules have
    // to match messageCss(): without them a fenced block or inline code in the
    // reasoning renders as prose, and the thinking block reads as if the model
    // sent literal backticks.
    return QStringLiteral(
           "body { color: %1; font-weight: 600; font-size: 12px; margin: 0; padding: 0; background: transparent; }"
           "p { color: %1; font-weight: 600; margin-bottom: 4px; }"
           "table.codeblock { border: 1px solid %2; border-radius: 6px; margin: 8px 0; background-color: %3; }"
           "td.codeblock-lang { color: %4; font-family: sans-serif; font-size: 10px; font-weight: 600;"
           "                    background-color: %3; padding: 4px 10px; }"
           "pre.codeblock { color: %1; font-family: monospace; font-size: 12px; white-space: pre-wrap;"
           "                 background-color: %3; margin: 6px 10px; }"
           "code { font-family: monospace; font-size: 12px; background-color: %5; color: %1;"
           "       padding: 2px 5px; border-radius: 3px; }"
           "blockquote { border-left: 3px solid %6; padding-left: 10px; color: %4; margin: 8px 0; }")
        .arg(kTextPrimary, kBorder, kCodeBlockBg, kTextMuted, kHoverBg, kAccent);
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

QString todoList()
{
    return QStringLiteral("QWidget { background: transparent; border: none; }");
}

QString todoProgress()
{
    return QStringLiteral("QLabel { color: %1; font-size: 10px; border: none; background: transparent; }")
        .arg(kTextMuted);
}

QString todoItemText()
{
    return QStringLiteral("QLabel { color: %1; font-size: 12px; border: none; background: transparent; }")
        .arg(kTextPrimary);
}

QString todoItemTextActive()
{
    return QStringLiteral(
           "QLabel { color: %1; font-size: 12px; font-weight: 500; border: none; background: transparent; }")
        .arg(kTextPrimary);
}

QString todoItemTextDone()
{
    // A finished step stays in the list as the record of what the run did, but
    // it must not compete with the steps still open.
    return QStringLiteral(
           "QLabel { color: %1; font-size: 12px; text-decoration: line-through;"
           " border: none; background: transparent; }")
        .arg(kTextMuted);
}

QString todoGlyphPending()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; border: none; background: transparent; }")
        .arg(kTextMuted);
}

QString todoGlyphActive()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; border: none; background: transparent; }")
        .arg(kAccent);
}

QString todoGlyphActiveDim()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; border: none; background: transparent; }")
        .arg(tinted(kAccent, 0.35));
}

QString todoGlyphDone()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; border: none; background: transparent; }")
        .arg(kSuccess);
}

bool isEditToolName(const QString &toolName)
{
    const QString key = toolName.trimmed().toLower();
    if (key.isEmpty() || key == u"write_plan_file"_s || key == u"read_file"_s || key == u"read"_s
        || key == u"fs/read_text_file"_s || key == u"grep"_s || key == u"search"_s
        || key == u"glob"_s || key == u"bash"_s || key == u"execute"_s || key == u"list_dir"_s
        || key.startsWith(u"mcp__"_s) || key.startsWith(u"terminal"_s)) {
        return false;
    }
    if (key == u"write_file"_s || key == u"fs/write_text_file"_s || key == u"write"_s
        || key == u"edit_file"_s || key == u"edit"_s || key == u"str_replace"_s
        || key == u"multi_edit_file"_s || key == u"multi_replace_file_content"_s
        || key == u"delete"_s || key == u"apply_patch"_s || key == u"search_replace"_s
        || key == u"replace"_s) {
        return true;
    }
    // Agents name the same work however they like. The family is what the card
    // colour and the diff highlight key off, not the exact identifier.
    return key.contains(u"edit"_s) || key.contains(u"write"_s) || key.contains(u"replace"_s)
        || key.contains(u"patch"_s) || key.contains(u"delete"_s) || key.contains(u"remove"_s);
}

QString toolLabel(const QString &toolName)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("read_file"), QStringLiteral("Read file")},
        {QStringLiteral("read"), QStringLiteral("Read file")},
        {QStringLiteral("write_file"), QStringLiteral("Write file")},
        {QStringLiteral("write"), QStringLiteral("Write file")},
        {QStringLiteral("edit_file"), QStringLiteral("Edit file")},
        {QStringLiteral("edit"), QStringLiteral("Edit file")},
        {QStringLiteral("str_replace"), QStringLiteral("Edit file")},
        {QStringLiteral("multi_edit_file"), QStringLiteral("Edit file")},
        {QStringLiteral("multi_replace_file_content"), QStringLiteral("Edit file")},
        {QStringLiteral("delete"), QStringLiteral("Delete file")},
        {QStringLiteral("list_dir"), QStringLiteral("List directory")},
        {QStringLiteral("grep"), QStringLiteral("Search")},
        {QStringLiteral("search"), QStringLiteral("Search")},
        {QStringLiteral("execute"), QStringLiteral("Run command")},
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

    const auto it = labels.constFind(toolName.trimmed().toLower());
    if (it != labels.constEnd()) {
        return it.value();
    }
    if (isEditToolName(toolName)) {
        const QString key = toolName.trimmed().toLower();
        if (key.contains(u"delete"_s) || key.contains(u"remove"_s)) {
            return QStringLiteral("Delete file");
        }
        if (key.contains(u"write"_s) && !key.contains(u"edit"_s) && !key.contains(u"replace"_s)
            && !key.contains(u"patch"_s)) {
            return QStringLiteral("Write file");
        }
        return QStringLiteral("Edit file");
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
    if (isEditToolName(toolName) || toolName == QLatin1String("bash") || toolName == QLatin1String("execute")) {
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
        {QStringLiteral("read"), QStringLiteral("#56b6c2")},
        {QStringLiteral("write_file"), QStringLiteral("#98c379")},
        {QStringLiteral("fs/write_text_file"), QStringLiteral("#98c379")},
        {QStringLiteral("write"), QStringLiteral("#98c379")},
        {QStringLiteral("edit_file"), QStringLiteral("#c678dd")},
        {QStringLiteral("edit"), QStringLiteral("#c678dd")},
        {QStringLiteral("str_replace"), QStringLiteral("#c678dd")},
        {QStringLiteral("multi_edit_file"), QStringLiteral("#c678dd")},
        {QStringLiteral("multi_replace_file_content"), QStringLiteral("#c678dd")},
        {QStringLiteral("delete"), QStringLiteral("#be5046")},
        {QStringLiteral("list_dir"), QStringLiteral("#d19a66")},
        {QStringLiteral("grep"), QStringLiteral("#e5c07b")},
        {QStringLiteral("search"), QStringLiteral("#e5c07b")},
        {QStringLiteral("glob"), QStringLiteral("#61afef")},
        {QStringLiteral("bash"), QStringLiteral("#e06c75")},
        {QStringLiteral("execute"), QStringLiteral("#e06c75")},
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

    const QString key = toolName.trimmed().toLower();
    const auto it = colors.constFind(key);
    if (it != colors.constEnd()) {
        return it.value();
    }
    if (isEditToolName(toolName)) {
        // Same hues as the built-in edit and write tools, so an ACP card is
        // scannable the same way instead of falling through to the muted rail.
        if (key.contains(u"delete"_s) || key.contains(u"remove"_s)) {
            return QStringLiteral("#be5046");
        }
        if (key.contains(u"write"_s) && !key.contains(u"edit"_s) && !key.contains(u"replace"_s)
            && !key.contains(u"patch"_s)) {
            return QStringLiteral("#98c379");
        }
        return QStringLiteral("#c678dd");
    }
    if (key.startsWith(QLatin1String("mcp__"))) {
        // MCP tools are a different kind of thing: a server the user configured,
        // not a built-in. One shared colour marks them as such.
        return QStringLiteral("#8fbcbb");
    }
    if (key.startsWith(QLatin1String("terminal"))) {
        return QStringLiteral("#e06c75");
    }
    // The fixed muted, not the theme's: this hue is the unknown-tool rail and
    // icon, which must not change with the theme.
    return kFixedTextMuted;
}

QString toolCardBg(const QString &toolName)
{
    return mixColors(toolColor(toolName), kFixedToolBg, 10);
}

namespace ToolCard
{

QString textPrimary()
{
    return kFixedTextPrimary;
}

QString textMuted()
{
    return kFixedTextMuted;
}

QString success()
{
    return kFixedSuccess;
}

QString danger()
{
    return kFixedDanger;
}

QString warning()
{
    return kFixedWarning;
}

QString accent()
{
    return kFixedAccent;
}

QString accentHover()
{
    return kFixedAccentHover;
}

QString cardBg()
{
    return kFixedCardBg;
}

QString hoverBg()
{
    return kFixedHoverBg;
}

QString codeBlockBg()
{
    return kFixedCodeBlockBg;
}

} // namespace ToolCard


// --- Themes. -----------------------------------------------------------------

namespace
{

// The default palette: the values the globals are initialised with at the top
// of this file. Used as the fallback for every preset and as the base for
// editor-derived palettes.
ChatPalette defaultPalette()
{
    ChatPalette p;
    p.panelBg = QStringLiteral("#1e1e1e");
    p.surfaceBg = QStringLiteral("#242426");
    p.cardBg = QStringLiteral("#28282b");
    p.codeBlockBg = QStringLiteral("#151517");
    p.inputBg = QStringLiteral("#2d2d31");
    p.hoverBg = QStringLiteral("#323236");
    p.agentTaskBg = QStringLiteral("#463a10");
    p.intentRowBg = QStringLiteral("#3a3020");
    p.border = QStringLiteral("#37373b");
    p.borderStrong = QStringLiteral("#4a4a50");
    p.textPrimary = QStringLiteral("#d4d4d6");
    p.textMuted = QStringLiteral("#8b8b92");
    p.accent = QStringLiteral("#3d7eff");
    p.accentHover = QStringLiteral("#5590ff");
    p.success = QStringLiteral("#4ec9a0");
    p.warning = QStringLiteral("#e2b341");
    p.danger = QStringLiteral("#f26d6d");
    // A preset is self-contained: the window chrome stays on the preset's own
    // colours so the panel reads as one surface.
    p.windowBg = p.panelBg;
    p.windowText = p.textPrimary;
    p.windowTextMuted = p.textMuted;
    p.windowBorder = p.border;
    p.windowHover = p.hoverBg;
    return p;
}

// A light palette for users who prefer a bright panel.
ChatPalette lightPalette()
{
    ChatPalette p;
    p.panelBg = QStringLiteral("#f5f5f5");
    p.surfaceBg = QStringLiteral("#ececee");
    p.cardBg = QStringLiteral("#ffffff");
    p.codeBlockBg = QStringLiteral("#e8e8ea");
    p.inputBg = QStringLiteral("#ffffff");
    p.hoverBg = QStringLiteral("#e0e0e3");
    p.agentTaskBg = QStringLiteral("#ececf0");
    p.intentRowBg = QStringLiteral("#fdf3d0");
    p.border = QStringLiteral("#d0d0d4");
    p.borderStrong = QStringLiteral("#b8b8bc");
    p.textPrimary = QStringLiteral("#1a1a1c");
    p.textMuted = QStringLiteral("#6e6e74");
    p.accent = QStringLiteral("#2563eb");
    p.accentHover = QStringLiteral("#1d4ed8");
    p.success = QStringLiteral("#16a34a");
    p.warning = QStringLiteral("#d97706");
    p.danger = QStringLiteral("#dc2626");
    p.windowBg = p.panelBg;
    p.windowText = p.textPrimary;
    p.windowTextMuted = p.textMuted;
    p.windowBorder = p.border;
    p.windowHover = p.hoverBg;
    return p;
}

// Ordered list of every overridable colour key, matching the ChatPalette fields.
const QStringList &colorKeys()
{
    static const QStringList keys = {
        QStringLiteral("panelBg"),
        QStringLiteral("surfaceBg"),
        QStringLiteral("cardBg"),
        QStringLiteral("codeBlockBg"),
        QStringLiteral("inputBg"),
        QStringLiteral("hoverBg"),
        QStringLiteral("agentTaskBg"),
        QStringLiteral("intentRowBg"),
        QStringLiteral("border"),
        QStringLiteral("borderStrong"),
        QStringLiteral("textPrimary"),
        QStringLiteral("textMuted"),
        QStringLiteral("accent"),
        QStringLiteral("accentHover"),
        QStringLiteral("success"),
        QStringLiteral("warning"),
        QStringLiteral("danger"),
        QStringLiteral("windowBg"),
        QStringLiteral("windowText"),
        QStringLiteral("windowTextMuted"),
        QStringLiteral("windowBorder"),
        QStringLiteral("windowHover"),
    };
    return keys;
}

// Human-readable labels for each colour key, shown in the config page.
QString colorKeyLabel(const QString &key)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("panelBg"), i18nc("theme colour", "Panel background")},
        {QStringLiteral("surfaceBg"), i18nc("theme colour", "Surface background")},
        {QStringLiteral("cardBg"), i18nc("theme colour", "Card background")},
        {QStringLiteral("codeBlockBg"), i18nc("theme colour", "Code block background")},
        {QStringLiteral("inputBg"), i18nc("theme colour", "Input background")},
        {QStringLiteral("hoverBg"), i18nc("theme colour", "Hover background")},
        {QStringLiteral("agentTaskBg"), i18nc("theme colour", "Agent task background")},
        {QStringLiteral("intentRowBg"), i18nc("theme colour", "Approval row background")},
        {QStringLiteral("border"), i18nc("theme colour", "Border")},
        {QStringLiteral("borderStrong"), i18nc("theme colour", "Border (strong)")},
        {QStringLiteral("textPrimary"), i18nc("theme colour", "Primary text")},
        {QStringLiteral("textMuted"), i18nc("theme colour", "Muted text")},
        {QStringLiteral("accent"), i18nc("theme colour", "Accent")},
        {QStringLiteral("accentHover"), i18nc("theme colour", "Accent (hover)")},
        {QStringLiteral("success"), i18nc("theme colour", "Success")},
        {QStringLiteral("warning"), i18nc("theme colour", "Warning")},
        {QStringLiteral("danger"), i18nc("theme colour", "Danger")},
        {QStringLiteral("windowBg"), i18nc("theme colour", "Window background")},
        {QStringLiteral("windowText"), i18nc("theme colour", "Window text")},
        {QStringLiteral("windowTextMuted"), i18nc("theme colour", "Window text (muted)")},
        {QStringLiteral("windowBorder"), i18nc("theme colour", "Window border")},
        {QStringLiteral("windowHover"), i18nc("theme colour", "Window hover")},
    };
    return labels.value(key, key);
}

// Writes one colour from the palette into the key, if the key is known.
void setPaletteColor(ChatPalette &palette, const QString &key, const QString &value)
{
    if (!QColor(value).isValid()) {
        return;
    }
    if (key == u"panelBg"_s) palette.panelBg = value;
    else if (key == u"surfaceBg"_s) palette.surfaceBg = value;
    else if (key == u"cardBg"_s) palette.cardBg = value;
    else if (key == u"codeBlockBg"_s) palette.codeBlockBg = value;
    else if (key == u"inputBg"_s) palette.inputBg = value;
    else if (key == u"hoverBg"_s) palette.hoverBg = value;
    else if (key == u"agentTaskBg"_s) palette.agentTaskBg = value;
    else if (key == u"intentRowBg"_s) palette.intentRowBg = value;
    else if (key == u"border"_s) palette.border = value;
    else if (key == u"borderStrong"_s) palette.borderStrong = value;
    else if (key == u"textPrimary"_s) palette.textPrimary = value;
    else if (key == u"textMuted"_s) palette.textMuted = value;
    else if (key == u"accent"_s) palette.accent = value;
    else if (key == u"accentHover"_s) palette.accentHover = value;
    else if (key == u"success"_s) palette.success = value;
    else if (key == u"warning"_s) palette.warning = value;
    else if (key == u"danger"_s) palette.danger = value;
    else if (key == u"windowBg"_s) palette.windowBg = value;
    else if (key == u"windowText"_s) palette.windowText = value;
    else if (key == u"windowTextMuted"_s) palette.windowTextMuted = value;
    else if (key == u"windowBorder"_s) palette.windowBorder = value;
    else if (key == u"windowHover"_s) palette.windowHover = value;
}

} // namespace

QStringList themePresetIds()
{
    return {QStringLiteral("kateai-dark"), QStringLiteral("kateai-light")};
}

QString themePresetLabel(const QString &id)
{
    if (id == u"kateai-dark"_s) {
        return i18nc("theme preset", "Kate AI Dark");
    }
    if (id == u"kateai-light"_s) {
        return i18nc("theme preset", "Kate AI Light");
    }
    return id;
}

QStringList themeColorKeys()
{
    return colorKeys();
}

QString themeColorLabel(const QString &key)
{
    return colorKeyLabel(key);
}

QString paletteColor(const ChatPalette &palette, const QString &key)
{
    if (key == u"panelBg"_s) return palette.panelBg;
    if (key == u"surfaceBg"_s) return palette.surfaceBg;
    if (key == u"cardBg"_s) return palette.cardBg;
    if (key == u"codeBlockBg"_s) return palette.codeBlockBg;
    if (key == u"inputBg"_s) return palette.inputBg;
    if (key == u"hoverBg"_s) return palette.hoverBg;
    if (key == u"agentTaskBg"_s) return palette.agentTaskBg;
    if (key == u"intentRowBg"_s) return palette.intentRowBg;
    if (key == u"border"_s) return palette.border;
    if (key == u"borderStrong"_s) return palette.borderStrong;
    if (key == u"textPrimary"_s) return palette.textPrimary;
    if (key == u"textMuted"_s) return palette.textMuted;
    if (key == u"accent"_s) return palette.accent;
    if (key == u"accentHover"_s) return palette.accentHover;
    if (key == u"success"_s) return palette.success;
    if (key == u"warning"_s) return palette.warning;
    if (key == u"danger"_s) return palette.danger;
    if (key == u"windowBg"_s) return palette.windowBg;
    if (key == u"windowText"_s) return palette.windowText;
    if (key == u"windowTextMuted"_s) return palette.windowTextMuted;
    if (key == u"windowBorder"_s) return palette.windowBorder;
    if (key == u"windowHover"_s) return palette.windowHover;
    return QString();
}

ChatPalette paletteForPreset(const QString &id)
{
    if (id == u"kateai-light"_s) {
        return lightPalette();
    }
    // Unknown ids and "kateai-dark" both give the default dark palette.
    return defaultPalette();
}

ChatPalette paletteForEditorTheme(const KSyntaxHighlighting::Theme &theme)
{
    ChatPalette p = defaultPalette();
    if (!theme.isValid()) {
        return p;
    }

    // The editor's background and text become the panel's, and every surface
    // is derived as a step away from them so contrast survives both dark and
    // light editor schemes.
    //
    // editorColor() and textColor() return QRgb where 0 means "not set".
    // Assigning 0 to QColor via the QRgb constructor yields a valid but
    // transparent black, which silently produces a dark palette for a light
    // theme like Breeze Light. Check for 0 explicitly and fall back through
    // the text-style background before giving up.
    const QRgb bgRgb = theme.editorColor(KSyntaxHighlighting::Theme::EditorColorRole::BackgroundColor);
    const QRgb styleBgRgb = theme.backgroundColor(KSyntaxHighlighting::Theme::TextStyle::Normal);
    const QColor editorBg = bgRgb != 0 ? QColor::fromRgb(bgRgb)
                          : styleBgRgb != 0 ? QColor::fromRgb(styleBgRgb)
                          : QColor();
    if (!editorBg.isValid()) {
        return p;
    }

    const bool dark = editorBg.lightnessF() < 0.5;
    // Text colour: fall back to a colour that contrasts with the background
    // when the theme does not specify one, instead of a fixed light gray
    // that is invisible on a light editor background.
    const QRgb fgRgb = theme.textColor(KSyntaxHighlighting::Theme::TextStyle::Normal);
    const QColor editorFg = fgRgb != 0 ? QColor::fromRgb(fgRgb)
                                      : QColor(dark ? u"#d4d4d6"_s : u"#1a1a1c"_s);

    const QString bg = editorBg.name();
    const QString fg = editorFg.name();

    p.panelBg = bg;
    p.textPrimary = fg;
    // Muted text: blend the foreground towards the background.
    p.textMuted = mixColors(fg, bg, 45);

    if (dark) {
        p.surfaceBg = lighten(bg, 4);
        p.cardBg = lighten(bg, 8);
        p.codeBlockBg = darken(bg, 8);
        p.inputBg = lighten(bg, 12);
        p.hoverBg = lighten(bg, 16);
        p.agentTaskBg = lighten(bg, 10);
        p.intentRowBg = mixColors(QStringLiteral("#e2b341"), bg, 18);
        p.border = lighten(bg, 18);
        p.borderStrong = lighten(bg, 26);
    } else {
        p.surfaceBg = darken(bg, 4);
        p.cardBg = QStringLiteral("#ffffff");
        p.codeBlockBg = darken(bg, 8);
        p.inputBg = QStringLiteral("#ffffff");
        p.hoverBg = darken(bg, 10);
        p.agentTaskBg = darken(bg, 7);
        p.intentRowBg = mixColors(QStringLiteral("#fdf3d0"), bg, 30);
        p.border = darken(bg, 16);
        p.borderStrong = darken(bg, 24);
        // Status colours: darker variants that keep their contrast on light
        // surfaces, instead of the light tints designed for dark backgrounds
        // (the dark defaults would be washed out or invisible on a light panel).
        p.success = QStringLiteral("#16a34a");
        p.warning = QStringLiteral("#d97706");
        p.danger = QStringLiteral("#dc2626");
    }

    // Accent: derive it from the editor theme's own colours, but only from a
    // candidate that can actually carry the accent's job. The accent paints
    // buttons, focus rings, selected items and pills, so it must be a visible
    // hue with a clear lightness step from the panel. The selection colour of
    // a dark editor theme is usually a translucent dark wash meant to sit
    // *behind* text -- ayu Dark #273747 on #0a0e14, Dracula #44475a, Tokyo
    // Night #27283e -- and inherited at face value it leaves every accent
    // affordance invisible against the panel. The keyword colour is the
    // fallback: it is designed to be read as text on this very background,
    // which is exactly the contrast property the accent needs. Same QRgb
    // 0-check as the background above: QColor(0) is valid transparent black.
    const auto themeColor = [](QRgb rgb) {
        return rgb != 0 ? QColor::fromRgb(rgb) : QColor();
    };
    // A gray is not an accent (Breeze Dark's keyword is the plain foreground),
    // and a colour within a small lightness step of the panel is invisible no
    // matter how saturated it is (Solarized Dark's selection #083d4a).
    const auto usableAccent = [&editorBg](const QColor &candidate) {
        if (!candidate.isValid() || candidate.saturationF() < 0.25) {
            return false;
        }
        return qAbs(candidate.lightnessF() - editorBg.lightnessF()) >= 0.25;
    };
    // A candidate in the red family would be indistinguishable from the
    // danger colour: approval buttons and error states must not share a look
    // (GitHub Dark's keyword #f97583 sits within a few degrees of #f26d6d).
    // Hues further than that are distinct to the eye even in the same warm
    // family -- ayu's orange #ff8f40 stays an accent, not an error colour.
    const auto clashesWithDanger = [&p](const QColor &candidate) {
        const QColor danger(p.danger);
        if (!danger.isValid() || candidate.hue() < 0) {
            return false;
        }
        int hueDelta = qAbs(candidate.hue() - danger.hue());
        if (hueDelta > 180) {
            hueDelta = 360 - hueDelta;
        }
        return hueDelta < 15 && qAbs(candidate.lightnessF() - danger.lightnessF()) < 0.2;
    };
    const QColor selection = themeColor(theme.editorColor(KSyntaxHighlighting::Theme::EditorColorRole::TextSelection));
    const QColor keyword = themeColor(theme.textColor(KSyntaxHighlighting::Theme::TextStyle::Keyword));

    QColor accent;
    if (usableAccent(selection) && !clashesWithDanger(selection)) {
        accent = selection;
    } else if (usableAccent(keyword) && !clashesWithDanger(keyword)) {
        accent = keyword;
    }
    if (accent.isValid()) {
        p.accent = accent.name();
        p.accentHover = mixColors(p.accent, fg, 20);
    } else if (!dark) {
        // A light editor theme starts from the dark palette's defaults, and
        // the default blue is tuned for dark surfaces; use the light preset's
        // darker blue, which stays readable on a light panel.
        p.accent = lightPalette().accent;
        p.accentHover = mixColors(p.accent, fg, 20);
    }

    // The window chrome (tool view, header bar, composer strip, intent dock)
    // follows the editor theme too: both the KateAI window and the chat inside
    // it inherit from the Kate editor window theme, so the whole panel reads
    // as one surface in the editor's colours. The chrome mirrors the panel
    // colours, the same way a built-in preset does.
    p.windowBg = p.panelBg;
    p.windowText = p.textPrimary;
    p.windowTextMuted = p.textMuted;
    p.windowBorder = p.border;
    p.windowHover = p.hoverBg;

    return p;
}

ChatPalette applyThemeOverrides(ChatPalette palette, const QJsonObject &overrides)
{
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
        setPaletteColor(palette, it.key(), it.value().toString());
    }
    return palette;
}

ChatPalette applyWindowChrome(ChatPalette palette, const QPalette &windowPalette)
{
    // Fills the window-chrome colours from a window colour theme (the
    // application palette's window roles): the window background, its text,
    // and the borders and hover derived from them. Not used by
    // paletteForSettings() in inherit mode -- there the window chrome follows
    // the editor theme together with the chat content -- but kept as a public
    // utility for callers that want to derive the chrome from the system
    // palette instead.
    const QColor window = windowPalette.color(QPalette::Window);
    const QColor windowText = windowPalette.color(QPalette::WindowText);
    if (!window.isValid() || !windowText.isValid()) {
        // No usable window palette: keep the chrome on the palette's own
        // panel colours rather than paint it with an invalid colour.
        palette.windowBg = palette.panelBg;
        palette.windowText = palette.textPrimary;
        palette.windowTextMuted = palette.textMuted;
        palette.windowBorder = palette.border;
        palette.windowHover = palette.hoverBg;
        return palette;
    }

    const bool dark = window.lightnessF() < 0.5;
    palette.windowBg = window.name();
    palette.windowText = windowText.name();
    palette.windowTextMuted = mixColors(windowText.name(), window.name(), 45);
    palette.windowBorder = dark ? lighten(window.name(), 18) : darken(window.name(), 16);
    palette.windowHover = dark ? lighten(window.name(), 16) : darken(window.name(), 10);
    return palette;
}

ChatPalette paletteForSettings(const Settings &settings)
{
    ChatPalette palette;
    const bool inherit = settings.themeSource == u"editor"_s;
    if (inherit) {
        // Inherit mode: both the chat content and the window chrome follow
        // the Kate editor window theme (the editor colour scheme). The whole
        // panel -- the KateAI window and the chat inside it -- reads as one
        // surface in the editor's colours.
        palette = paletteForEditorTheme(editorTheme());
    } else {
        palette = paletteForPreset(settings.themePreset);
    }

    // Apply user overrides on top of the selected source.
    QJsonObject overrides;
    if (!settings.themeOverrides.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(settings.themeOverrides.toUtf8());
        if (doc.isObject()) {
            overrides = doc.object();
        }
    }
    palette = applyThemeOverrides(palette, overrides);

    if (!inherit) {
        // A built-in preset is self-contained: the window chrome mirrors the
        // preset's panel colours -- including panelBg/textPrimary/border/
        // hoverBg overrides -- so the panel reads as one surface and a panel
        // override reaches the header bar and the composer strip. Re-applying
        // the overrides lets an explicit window colour win over the mirror.
        ChatPalette mirrored = palette;
        mirrored.windowBg = palette.panelBg;
        mirrored.windowText = palette.textPrimary;
        mirrored.windowTextMuted = palette.textMuted;
        mirrored.windowBorder = palette.border;
        mirrored.windowHover = palette.hoverBg;
        palette = applyThemeOverrides(mirrored, overrides);
    }
    return palette;
}

void loadTheme(const Settings &settings)
{
    const ChatPalette palette = paletteForSettings(settings);
    setPalette(palette);

    // Point code highlighting at the editor's scheme when inheriting, so code
    // blocks in the transcript match the editor's syntax colours.
    if (settings.themeSource == u"editor"_s) {
        auto *editor = KTextEditor::Editor::instance();
        if (editor) {
            CodeHighlight::setThemeName(editor->theme().name());
        }
    } else {
        // Built-in presets use Kate's default dark or light theme for code.
        if (settings.themePreset == u"kateai-light"_s) {
            CodeHighlight::setThemeName(QStringLiteral("Default"));
        } else {
            CodeHighlight::setThemeName(QString());
        }
    }
}

} // namespace ChatTheme
} // namespace KateAi
