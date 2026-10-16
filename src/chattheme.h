/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

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

// Agent-task cards (sub-agents). A full border plus a raised yellow
// background, one step above a tool card: a delegated task is a different
// *kind* of thing than a tool the orchestrator ran itself, and the warm surface
// is what separates someone else's work from the agent's own tool rows at a
// glance. The border colour still carries the outcome.
QString agentTaskCard();

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
QString chipButton();     // model / mode selectors: text plus a caret affordance
QString iconButton();     // square icon-only button
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
QString planChecklist();  // structured plan steps

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
// Stylesheet for the header row of a tool card, tinted by tool.
QString toolHeader(const QString &toolName);
// Blends a hex colour onto another at the given percentage (0-100).
QString mixColors(const QString &hex, const QString &onto, int percent);

} // namespace ChatTheme

} // namespace KateAi