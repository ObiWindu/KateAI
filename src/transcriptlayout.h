/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QList>
#include <QString>

namespace KateAi
{

/**
 * How a transcript is anchored within its viewport when the conversation is
 * shorter than the panel.
 *
 * Bottom is what a chat wants: the newest message sits against the composer and
 * the slack collects above the conversation, so the thread reads as a
 * conversation that continues upward. Top is what a document wants: the first
 * message is flush against the top and the slack falls away below.
 *
 * This was a free choice in the transcript layout and flipping it broke two
 * things at once, because everything else assumed Bottom:
 *
 *  - the "jump to latest" affordance, which only means anything when the newest
 *    content is the content you want to reach;
 *  - the insert index for new messages, which has to land after the stretch
 *    under Bottom and before it under Top. Getting that backwards put every new
 *    message below the slack, at the far end of the viewport.
 *
 * Naming the two states keeps the choice explicit at the call sites that depend
 * on it, instead of leaving it to be re-derived from the layout.
 */
enum class TranscriptAnchor {
    Bottom,
    Top,
};

/**
 * Where in the transcript layout a newly appended message must be inserted.
 *
 * `spacerIndex` is the layout index of the stretch item (-1 when there is none)
 * and `indicatorsIndex` the index of the pinned thinking/working row (-1 when it
 * is absent). The result is always in [0, count].
 *
 * The rule: land on the far side of the stretch from the anchor, and always
 * before the indicator row, which has to stay pinned at the bottom.
 */
int transcriptInsertIndexFor(TranscriptAnchor anchor, int spacerIndex, int indicatorsIndex, int count);

/**
 * Whether the transcript should follow new content, given where the scrollbar
 * currently sits.
 *
 * A short tail (within `FollowThresholdPx` of the end) counts as following; past
 * that the user has scrolled away and the panel should stop yanking the viewport
 * and offer "jump to latest" instead.
 */
bool transcriptShouldFollowTail(int scrollbarValue, int scrollbarMaximum);

} // namespace KateAi
