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
 * Top is a document: the first message is flush against the top and the slack
 * falls away below, so the thread reads oldest-to-newest downward. Bottom is a
 * composer-pinned chat: the newest message sits against the input and the slack
 * collects above.
 *
 * The insert index for a new message has to land on the growing side of the
 * stretch: before it under Top, after it under Bottom. Getting that backwards
 * puts every new message on the far side of the slack, so user turns pile up
 * at the bottom of the viewport instead of in chronological order.
 *
 * Naming the two states keeps the choice explicit at the call sites that depend
 * on it.
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
 * before the indicator row, which stays last so live status sits under the
 * newest turn.
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
