/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

class QPalette;

namespace KSyntaxHighlighting
{
class Theme;
}

namespace KateAi
{

/**
 * One home for every colour, radius and padding the chat panel paints with.
 *
 * The panel used to carry its look in roughly thirty copy-pasted
 * setStyleSheet() calls spread over chatwidget.cpp, toolcallwidget.cpp and
 * subtaskwidget.cpp. The same button style was duplicated seven times with
 * small differences, so surfaces drifted apart and a palette change meant
 * hunting through every file. Widgets now ask this module for their style,
 * which keeps the panel visually consistent and makes a retheme a one-file job.
 *
 * The palette follows the dark surfaces of an editor side panel: a near-black
 * backdrop, one elevation step for cards, a second for inputs, and a single
 * blue accent reserved for actions that change something.
 */
namespace ChatTheme
{

// --- Surfaces, from the window backdrop up to the composer. ------------------
QString panelBg();
QString surfaceBg();
QString cardBg();
QString codeBlockBg();
QString inputBg();
QString hoverBg();

// Tool surfaces. ----------------------------------------------------------
// Tool cards sit in the transcript, so they must not reuse the transcript
// backdrop: a read_file / grep / bash card has to read as a distinct object at
// a glance rather than dissolve into the prose around it. The body and the
// head box are two steps apart so the header reads as a header.
QString toolBg();
QString toolHeaderBg();

// --- The intent dock. ------------------------------------------------------
// Everything that needs the user to press something lives in one strip pinned
// directly above the input, so the call to action is never scrolled away in the
// transcript. The dock itself is chrome and stays flat; only its cards are
// boxed.
QString intentDock();

// Agent-task cards (sub-agents, suggestions, questions). A full border plus a
// raised background, one step above a tool card: a delegated task is a
// different *kind* of thing than a tool the orchestrator ran itself. The
// selector targets the card's objectName, which every caller passes in —
// hard-coding one card's name here left the other cards without a surface.
// The default is the sub-agent card, so callers that only know the kind of
// card (not which one) can ask for the style directly.
QString agentTaskCard(const QString &objectName = QStringLiteral("subtaskWidget"));

// --- The window around the chat. ---------------------------------------------
// The KateAI window -- the tool view, the header bar, the composer strip and
// the intent dock -- is chrome. In inherit mode it follows the editor colour
// theme together with the chat content: both the KateAI plugin window and the
// chatUI inherit from the Kate editor window theme, so the whole panel reads
// as one surface in the editor's colours. With a built-in preset the window
// colours equal the panel colours as well, so a preset panel also reads as
// one surface.
QString windowBg();
QString windowText();
QString windowTextMuted();
QString windowBorder();
QString windowHover();

// --- Lines. ------------------------------------------------------------------
QString border();
QString borderStrong();

// --- Type. -------------------------------------------------------------------
QString textPrimary();
QString textMuted();

// --- Accents. ----------------------------------------------------------------
QString accent();
QString accentHover();
QString success();
QString warning();
QString danger();

// --- Per-widget stylesheets. ------------------------------------------------
// Each returns a complete stylesheet for one widget, so a call site reads as
// `button->setStyleSheet(ChatTheme::chipButton())`.
QString panel();
QString scrollArea();
QString transcript();
QString headerBar();
// The thread title in the header bar: chrome text, so it follows the window
// colour theme rather than the chat palette's muted text.
QString headerTitle();
QString chipButton();     // model / mode selectors: text plus a caret affordance
QString iconButton();     // square icon-only button
// Icon buttons that sit on the window chrome (the header bar): their text and
// hover use the window-chrome colours. iconButton() stays for buttons on chat
// surfaces, where the chat palette's colours are the right ones.
QString headerIconButton();
// Small hint text on the window chrome (the status strip under the input).
QString windowHintLabel();
// The tool view that hosts the whole panel: the KateAI window itself.
QString windowChrome();
QString composerCard();
QString promptInput();
QString hintLabel();
QString tokenLabel();
QString infoBar();
QString jumpToLatest();
QString activityPill();   // generic live status pill
QString thinkingPill();   // blue brain pill for hidden reasoning
QString thinkingPillDim(); // the pulse phase of the thinking pill
QString workingPill();    // amber live-tool pill
QString workingPillDim(); // the pulse phase of the working pill
QString userCard();
// The user card's surface and the text colour that stays readable on it.
QString userCardBg();
QString userCardText();
QString assistantCard();
QString sectionLabel();
QString scrollBar();
// Solid head box on every tool card, tinted with the tool's hue.
QString toolHeader(const QString &toolName);
QString intentApprovalRow();  // docked "needs approval" strip

// Rich-text bodies. These go into QTextDocument::setDefaultStyleSheet(), so
// they are CSS rather than Qt widget selectors.
QString messageCss();     // assistant markdown
// The same markup for the user's own turns. The user card is a white-on-dark
// surface rather than the muted prose of an assistant answer, so it repeats
// the body rules with white instead of textPrimary(): a document stylesheet
// cannot inherit the colour a widget stylesheet paints with.
QString userMessageCss(); // user markdown
QString thinkingCss();    // the collapsed reasoning block
QString toggleLink();     // the "Reasoning" disclosure button
QString roleHeader();     // the YOU / KATE AI captions above a message
QString messageText();    // plain user / assistant body text
// --- The todo list. -----------------------------------------------------------
// The plan steps with their advancement, drawn the way an agent panel draws a
// todo list: a header row with the "done/total" counter, and one row per step
// led by a status glyph (pending / pulsing active / done). One stylesheet per
// element, the way every other widget asks for its look.
QString todoList();           // the container
QString todoProgress();       // the "n/m" counter in the header
QString todoItemText();       // a pending step's text
QString todoItemTextActive(); // the step being worked on
QString todoItemTextDone();   // a finished step: muted and struck through
QString todoGlyphPending();   // ○, muted
QString todoGlyphActive();    // ●, accent
QString todoGlyphActiveDim(); // ●, accent at low alpha (the pulse's dim phase)
QString todoGlyphDone();      // ✓, success

// --- Copy --------------------------------------------------------------------
// Tool names reach the model in snake_case ("multi_replace_file_content").
// A card in the transcript should read "Edit file" instead.
QString toolLabel(const QString &toolName);
// Colour for a tool's status dot, given how the call ended.
QString toolAccent(const QString &toolName, bool running, bool ok);
// File edits and writes, including the names ACP agents invent for the same
// work ("Edit", "Write", "apply_patch", "StrReplace"). Comparison is
// case-insensitive: the protocol's kind is lowercase, the tool's own name
// often is not, and a miss here is what left those cards uncoloured.
bool isEditToolName(const QString &toolName);

// --- Per-tool colour identity -------------------------------------------------
// Every tool family carries its own hue, so a transcript full of cards can be
// scanned by colour as well as by label: reads are cool, writes are green,
// edits violet, commands warm. railColor is the saturated hue (card rail, icon,
// tool-name prefix); the two surfaces are that hue blended down onto the tool
// card surfaces so the tint never fights the text on top of it.
QString toolColor(const QString &toolName);
// The card body surface, tinted with the tool's hue.
QString toolCardBg(const QString &toolName);

// --- Tool card chrome (theme-independent). -----------------------------------
// The tool card never follows the theme palette: its text, status marks,
// approval buttons and result surface are pinned to the default palette, so
// a tool card keeps the same look in every theme and a transcript can be
// scanned by colour whether the panel is dark or light.
namespace ToolCard {
QString textPrimary();
QString textMuted();
QString success();
QString danger();
QString warning();
QString accent();
QString accentHover();
QString cardBg();
QString hoverBg();
QString codeBlockBg();
}

// Blends a hex colour onto another at the given percentage (0-100).
QString mixColors(const QString &hex, const QString &onto, int percent);
// A colour at the given opacity as #AARRGGBB, which Qt stylesheets accept.
QString tinted(const QString &hex, double alpha);
// The colour a label on the given surface should use: white on dark surfaces,
// the theme's primary text colour on light ones when that text is itself dark,
// and the panel backdrop when the surface is a light accent chip on a dark
// panel (light text would vanish on it).
QString textOn(const QString &surfaceBg);

// --- Popup chrome. -----------------------------------------------------------
// Shared stylesheets for the panel's menus and the model-picker popup, so a
// retheme does not leave a light popup floating over a dark panel.
QString menu();
QString popupMenu();
QString lineEdit();

// --- Themes. -----------------------------------------------------------------
// The whole palette as one value. The accessors above read the process-wide
// palette that loadTheme() installs; these functions compute palettes without
// touching global state, which is what the config page previews with.
struct ChatPalette {
    QString panelBg;
    QString surfaceBg;
    QString cardBg;
    QString codeBlockBg;
    QString inputBg;
    QString hoverBg;
    QString agentTaskBg;
    QString intentRowBg;
    QString border;
    QString borderStrong;
    QString textPrimary;
    QString textMuted;
    QString accent;
    QString accentHover;
    QString success;
    QString warning;
    QString danger;
    // Window-chrome colours (tool view, header bar, composer strip). In
    // inherit mode these mirror the panel colours, so the KateAI window and
    // the chat inside it both follow the editor theme; under a built-in
    // preset they mirror the preset's panel colours as well.
    QString windowBg;
    QString windowText;
    QString windowTextMuted;
    QString windowBorder;
    QString windowHover;
};

// Built-in palette ids for the config page ("kateai-dark", "kateai-light").
QStringList themePresetIds();
QString themePresetLabel(const QString &id);
// Every colour a user can override, in display order, with a label for the UI.
QStringList themeColorKeys();
QString themeColorLabel(const QString &key);
// The colour stored under one override key in a palette ("" when unknown).
QString paletteColor(const ChatPalette &palette, const QString &key);
// The palette for a built-in preset id; unknown ids give the default palette.
ChatPalette paletteForPreset(const QString &id);
// Derives a palette from a Kate editor colour scheme: the editor background
// and text become the panel, and every surface is a step away from them so
// contrast survives both dark and light schemes. The accent comes from the
// scheme's selection or keyword colour, but only when the candidate is
// actually visible against the panel. The window-chrome colours mirror the
// panel colours, so the KateAI window and the chat inside it both inherit
// from the editor theme. Invalid themes fall back to the default palette.
ChatPalette paletteForEditorTheme(const KSyntaxHighlighting::Theme &theme);
// Applies {key: hex} overrides on top of a palette; invalid colours and
// unknown keys are ignored.
ChatPalette applyThemeOverrides(ChatPalette palette, const QJsonObject &overrides);
// Fills a palette's window-chrome colours from a window colour theme (the
// application palette's window roles): the window background, its text, and
// the borders and hover derived from them. When the palette carries no usable
// window colours, the chrome falls back to the palette's own panel colours.
// Not used by paletteForSettings() in inherit mode -- there the window chrome
// follows the editor theme together with the chat content.
ChatPalette applyWindowChrome(ChatPalette palette, const QPalette &windowPalette);
// The effective palette for a settings struct: the selected source plus the
// user's overrides.
ChatPalette paletteForSettings(const Settings &settings);
// Installs the settings' palette into the process-wide palette the accessors
// above read, and points code highlighting at the matching scheme. Call
// whenever settings change or the editor theme may have changed.
void loadTheme(const Settings &settings);

} // namespace ChatTheme

} // namespace KateAi