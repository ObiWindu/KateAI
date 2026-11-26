/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QtGlobal>

namespace KateAi
{

/**
 * Converts retained text into an estimate of the memory it really costs.
 *
 * The transcript budgets were written in characters, but a widget does not
 * retain a plain string: a `QTextDocument` keeps block, fragment and format
 * structures beside the text, so one retained character costs several bytes
 * and not one. A character budget therefore under-counts the real footprint.
 *
 * The factors below are deliberately conservative upper estimates, not
 * measurements: a browser that has not laid out yet costs far less, and one
 * that is on screen can cost considerably more per character while its layout
 * and visible blocks are warm. `docs/RAM_SAVINGS_PLAN.md` records the change
 * to byte budgets; `docs/MEMORY_BASELINE.md` does not cover these factors.
 */
namespace RetainedCost
{

// Upper estimate for one character of plain text in a QTextDocument, including
// its block structure. A document that has been laid out measured closer to
// 4-6 bytes per character on Qt 6.
constexpr double PlainBytesPerChar = 8.0;
// Upper estimate for one character of rich (HTML) text once block, fragment and
// format structures are counted. Format structs are partly pooled, so a large
// document trends lower; budgeting high trims a little early rather than
// letting the real footprint exceed the budget the cap promises.
constexpr double RichBytesPerChar = 176.0;

// Bytes retained by a document holding `chars` characters of rich text.
constexpr double richDocumentBytes(qsizetype chars)
{
    return chars > 0 ? chars * RichBytesPerChar : 0.0;
}

// Bytes retained by a plain-text buffer of `chars` characters.
constexpr double plainBytes(qsizetype chars)
{
    return chars > 0 ? chars * PlainBytesPerChar : 0.0;
}

// Converts a byte budget into the character budget to use for rich text.
constexpr qsizetype richCharsForBudget(double budgetBytes)
{
    return static_cast<qsizetype>(budgetBytes / RichBytesPerChar);
}

} // namespace RetainedCost

} // namespace KateAi
