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

// --- Tool surfaces. ----------------------------------------------------------
// Tool cards sit in the transcript, so they must not reuse the transcript
// backdrop: a read_file / grep / bash card has to read as a distinct object at
// a glance rather than dissolve into the prose around it. The body and the
// head box are two steps apart so the header reads as a header.
QString toolBg();
QString toolHeaderBg();

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
QString activityPill();   // the live "Working" / "Thinking" line
QString userCard();
QString assistantCard();
QString sectionLabel();
QString scrollBar();
QString toolHeader();    // solid head box on every tool card

// Rich-text bodies. These go into QTextDocument::setDefaultStyleSheet(), so
// they are CSS rather than Qt widget selectors.
QString messageCss();     // assistant markdown
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

} // namespace ChatTheme

} // namespace KateAi