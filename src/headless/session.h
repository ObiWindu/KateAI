/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "args.h"
#include "types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>

namespace KateAi
{

class AgentLoop;
class DiskDocumentBridge;

/**
 * One headless conversation.
 *
 * The session owns the same AgentLoop the plugin uses; only the editor bridge
 * is replaced with disk-only I/O and no LSP/debug provider is installed. This
 * is the "one core, many clients" seam: CLI, ACP server and tests all drive the
 * same loop, tools, policy, verification gate and event log.
 */
class HeadlessSession : public QObject
{
    Q_OBJECT

public:
    HeadlessSession(const HeadlessOptions &options, bool externalPermissions,
                    QObject *parent = nullptr);
    ~HeadlessSession() override;

    bool initialize(QString *error = nullptr);
    bool isReady() const
    {
        return m_ready;
    }
    void start(const QString &prompt);
    void abort();
    void answerQuestion(const QString &answer);
    void resolvePermission(PermissionDecision decision, const QString &toolCallId = QString());

    AgentLoop *loop() const
    {
        return m_loop.get();
    }
    QString conversationId() const;
    bool saveConversation(QString *error = nullptr);
    QJsonObject summary() const;

Q_SIGNALS:
    void assistantDelta(const QString &delta);
    void thinkingDelta(const QString &delta);
    void planUpdated(const QJsonArray &plan);
    void toolStarted(const PermissionRequest &request);
    void toolFinished(const ToolResult &result);
    void notice(const QString &text);
    void harnessEvent(const QString &type, const QJsonObject &payload);
    // Only emitted in external-permission mode (the ACP server forwards them
    // to its client).
    void permissionRequested(const PermissionRequest &request);
    void questionRequested(const PermissionRequest &request);
    void finished(bool ok, const QString &stopReason, const QString &error);

private:
    void finishOnce(bool ok, const QString &stopReason, const QString &error);
    void applyProviderOverrides(Settings *settings) const;

    HeadlessOptions m_options;
    bool m_externalPermissions = false;
    bool m_ready = false;
    bool m_finishedEmitted = false;
    bool m_questionAnswered = false;
    QString m_error;
    QString m_stopReason = QStringLiteral("end_turn");
    std::unique_ptr<DiskDocumentBridge> m_bridge;
    std::unique_ptr<AgentLoop> m_loop;
};

} // namespace KateAi
