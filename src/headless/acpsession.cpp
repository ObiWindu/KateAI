/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpsession.h"

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

HeadlessAcpSession::HeadlessAcpSession(const HeadlessOptions &options, QObject *parent)
    : AcpServerSession(parent)
    , m_session(options, true, this)
{
    connect(&m_session, &HeadlessSession::assistantDelta, this,
            &AcpServerSession::textDelta);
    connect(&m_session, &HeadlessSession::thinkingDelta, this,
            &AcpServerSession::thinkingDelta);
    connect(&m_session, &HeadlessSession::planUpdated, this,
            &AcpServerSession::planUpdated);
    connect(&m_session, &HeadlessSession::toolStarted, this,
            &AcpServerSession::toolStarted);
    connect(&m_session, &HeadlessSession::toolFinished, this,
            &AcpServerSession::toolFinished);
    connect(&m_session, &HeadlessSession::permissionRequested, this,
            &AcpServerSession::permissionNeeded);
    connect(&m_session, &HeadlessSession::questionRequested, this,
            &AcpServerSession::questionAsked);
    connect(&m_session, &HeadlessSession::notice, this, &AcpServerSession::notice);
    connect(&m_session, &HeadlessSession::finished, this, &AcpServerSession::finished);
}

bool HeadlessAcpSession::initialize(QString *error)
{
    return m_session.initialize(error);
}

void HeadlessAcpSession::startPrompt(const QString &text)
{
    m_session.start(text);
}

void HeadlessAcpSession::cancel()
{
    m_session.abort();
}

void HeadlessAcpSession::resolvePermission(PermissionDecision decision,
                                           const QString &toolCallId)
{
    m_session.resolvePermission(decision, toolCallId);
}

void HeadlessAcpSession::answerQuestion(const QString &answer)
{
    m_session.answerQuestion(answer);
}

} // namespace KateAi
