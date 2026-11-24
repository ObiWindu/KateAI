/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "events.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace KateAi
{

/**
 * Append-only storage for conversation events.
 *
 * The table lives in the session database, next to the legacy conversation
 * payload. Every append is one transaction that advances the conversation
 * head and updates the message projection, so a crash either commits an event
 * and its projection together or commits neither. The projection can always
 * be rebuilt from the log.
 */
class EventStore
{
public:
    explicit EventStore(QString databasePath = QString());
    ~EventStore();

    EventStore(const EventStore &) = delete;
    EventStore &operator=(const EventStore &) = delete;

    static QString defaultDatabasePath();

    bool isOpen() const;
    bool open(QString *error = nullptr);
    void close();

    // Appends events in order. A zero seq is assigned from the conversation
    // head; a caller-supplied seq must be greater than the current head.
    bool append(const QString &conversationId, const QList<HarnessEvent> &events,
                QString *error = nullptr);
    // Copies events (by value) up to `throughSeq` into a new conversation.
    // Used by conversation fork so the child starts with a replayable prefix.
    bool copyConversation(const QString &sourceId, const QString &targetId,
                          qint64 throughSeq = -1, QString *error = nullptr);

    QList<HarnessEvent> read(const QString &conversationId, qint64 fromSeq = 0, int limit = -1,
                             QString *error = nullptr) const;
    qint64 headSeq(const QString &conversationId) const;

    // Rebuilds event_message_projection from the log. Safe to call at any time.
    bool rebuildProjection(const QString &conversationId, QString *error = nullptr);

    // The message rows derived from the log, as the transcript expects them.
    QList<ChatMessage> projectMessages(const QString &conversationId, QString *error = nullptr) const;

    // Imports a conversation's legacy payload as events once. Existing event
    // logs are left untouched. Returns true when there was nothing to do.
    bool backfillConversation(const QString &conversationId, QString *error = nullptr);
    bool backfillAll(QString *error = nullptr);

    bool removeConversation(const QString &conversationId, QString *error = nullptr);
    bool clearAll(QString *error = nullptr);
    // Deletes events whose conversation row no longer exists. Called after
    // history pruning, never on open: a brand-new conversation has no row
    // until its first save, and its events must survive until then.
    void pruneOrphans();

    QString lastError() const
    {
        return m_lastError;
    }

private:
    bool ensureSchema(QString *error);
    bool applyProjection(const HarnessEvent &event, QSqlDatabase &db, QString *error);
    QSqlDatabase database() const;

    QString m_path;
    QString m_connectionName;
    QString m_lastError;
};

} // namespace KateAi
