/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "transcriptlayout.h"

#include <algorithm>

namespace KateAi
{

namespace
{
// How close to the end of the content still counts as "following". Small enough
// that momentum scrolling does not count as a deliberate scroll-away, large
// enough that a mouse wheel notch does not immediately break the follow.
constexpr int FollowThresholdPx = 40;
} // namespace

int transcriptInsertIndexFor(TranscriptAnchor anchor, int spacerIndex, int indicatorsIndex, int count)
{
    int index = count;
    if (spacerIndex >= 0) {
        // Under Bottom the stretch sits above the content and the message goes
        // just past it; under Top the stretch sits below the content and the
        // message goes just before it. Either way the message lands on the side
        // of the slack where the conversation actually grows.
        index = anchor == TranscriptAnchor::Bottom ? spacerIndex + 1 : spacerIndex;
    }
    if (indicatorsIndex >= 0) {
        // The status row is pinned to the bottom of the transcript, so no message
        // may ever be appended after it.
        index = std::min(index, indicatorsIndex);
    }
    return std::clamp(index, 0, count);
}

bool transcriptShouldFollowTail(int scrollbarValue, int scrollbarMaximum)
{
    return scrollbarMaximum - scrollbarValue <= FollowThresholdPx;
}

} // namespace KateAi
