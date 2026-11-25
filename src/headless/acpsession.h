/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "args.h"
#include "harness/acpserver.h"
#include "session.h"

namespace KateAi
{

// Bridges one HeadlessSession into the ACP server's session interface.
class HeadlessAcpSession : public AcpServerSession
{
    Q_OBJECT

public:
    explicit HeadlessAcpSession(const HeadlessOptions &options, QObject *parent = nullptr);

    bool initialize(QString *error = nullptr);

    void startPrompt(const QString &text) override;
    void cancel() override;
    void resolvePermission(PermissionDecision decision,
                           const QString &toolCallId = QString()) override;
    void answerQuestion(const QString &answer) override;

private:
    HeadlessSession m_session;
};

} // namespace KateAi
