/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QList>
#include <QJsonObject>

namespace KateAi
{

class SessionStore
{
public:
    struct SessionData {
        QList<ChatMessage> messages;
        QString currentThinking;
        QJsonArray currentPlan;
        bool planShown = false;
        QString currentAssistant;
        quint64 stateEpoch = 0;
        QList<QString> actionSignatures;
        QHash<QString, int> actionRepeatCounts;
        QList<QString> changedPaths;
        bool changesNeedVerification = false;
        bool verificationAttempted = false;
        int verificationPromptCount = 0;
        int modelRequests = 0;
        int toolCalls = 0;
    };

    static SessionData load();
    static void save(const SessionData &data);
    static void clear();
};

} // namespace KateAi