/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "eventstore.h"

#include "sessionstore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

const QString kSchemaVersionKey = QStringLiteral("event_schema_version");
constexpr int kEventSchemaVersion = 1;

QString connectionNameFor(EventStore *store)
{
    static quint64 counter = 0;
    return QStringLiteral("kateai-events-%1-%2")
        .arg(reinterpret_cast<quintptr>(store))
        .arg(++counter);
}

QString toolCallsJson(const QJsonArray &calls)
{
    return QString::fromUtf8(QJsonDocument(calls).toJson(QJsonDocument::Compact));
}

} // namespace

EventStore::EventStore(QString databasePath)
    : m_path(std::move(databasePath))
    , m_connectionName(connectionNameFor(this))
{
}

EventStore::~EventStore()
{
    close();
}

QString EventStore::defaultDatabasePath()
{
    return SessionStore::databasePath();
}

bool EventStore::isOpen() const
{
    const QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    return db.isValid() && db.isOpen();
}

QSqlDatabase EventStore::database() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

void EventStore::close()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
            if (db.isValid()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool EventStore::open(QString *error)
{
    if (isOpen()) {
        return true;
    }

    if (!QSqlDatabase::isDriverAvailable(u"QSQLITE"_s)) {
        m_lastError = u"The QSQLITE driver is missing; the event log is unavailable."_s;
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    const QString path = m_path.isEmpty() ? defaultDatabasePath() : m_path;
    if (!path.isEmpty()) {
        const QString directory = QFileInfo(path).absolutePath();
        if (!QDir().mkpath(directory)) {
            m_lastError = u"Cannot create the event-log directory: %1"_s.arg(directory);
            if (error) {
                *error = m_lastError;
            }
            return false;
        }
    }

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_connectionName);
        db.setDatabaseName(path);
        if (!db.open()) {
            m_lastError = db.lastError().text();
            if (error) {
                *error = m_lastError;
            }
            QSqlDatabase::removeDatabase(m_connectionName);
            return false;
        }

        QSqlQuery pragmas(db);
        pragmas.exec(u"PRAGMA journal_mode=WAL"_s);
        pragmas.exec(u"PRAGMA synchronous=FULL"_s);
        pragmas.exec(u"PRAGMA busy_timeout=5000"_s);
        pragmas.exec(u"PRAGMA foreign_keys=ON"_s);
    }

    if (!ensureSchema(error)) {
        return false;
    }
    return true;
}

void EventStore::pruneOrphans()
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen()) {
        return;
    }
    // A conversation deleted while this process was not running leaves its
    // events behind. The payload table is the authority on which conversations
    // still exist; if it is absent (an event-only database) leave everything.
    QSqlQuery check(db);
    if (!check.exec(u"SELECT 1 FROM conversations LIMIT 1"_s)
        && check.lastError().isValid()) {
        return;
    }
    for (const QString &table :
         {u"session_event"_s, u"event_message_projection"_s, u"event_conversation_head"_s}) {
        QSqlQuery query(db);
        query.exec(u"DELETE FROM %1 WHERE conversation_id NOT IN (SELECT id FROM conversations)"_s
                       .arg(table));
    }
}

bool EventStore::ensureSchema(QString *error)
{
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen()) {
        m_lastError = u"The event database is not open."_s;
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    static const char *const statements[] = {
        "CREATE TABLE IF NOT EXISTS harness_meta ("
        "key TEXT PRIMARY KEY,"
        "value TEXT NOT NULL)",

        "CREATE TABLE IF NOT EXISTS session_event ("
        "conversation_id TEXT NOT NULL,"
        "seq INTEGER NOT NULL,"
        "ts INTEGER NOT NULL,"
        "turn_id TEXT,"
        "step_id TEXT,"
        "type TEXT NOT NULL,"
        "payload TEXT NOT NULL,"
        "imported INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY (conversation_id, seq))",

        "CREATE INDEX IF NOT EXISTS session_event_turn "
        "ON session_event (conversation_id, turn_id, seq)",

        "CREATE TABLE IF NOT EXISTS event_conversation_head ("
        "conversation_id TEXT PRIMARY KEY,"
        "head_seq INTEGER NOT NULL DEFAULT 0)",

        "CREATE TABLE IF NOT EXISTS event_message_projection ("
        "conversation_id TEXT NOT NULL,"
        "seq INTEGER NOT NULL,"
        "role INTEGER NOT NULL,"
        "content TEXT,"
        "thinking TEXT,"
        "tool_call_id TEXT,"
        "tool_name TEXT,"
        "tool_calls TEXT,"
        "PRIMARY KEY (conversation_id, seq))",
    };

    for (const char *statement : statements) {
        QSqlQuery query(db);
        if (!query.exec(QString::fromLatin1(statement))) {
            m_lastError = query.lastError().text();
            if (error) {
                *error = m_lastError;
            }
            return false;
        }
    }

    QSqlQuery version(db);
    version.prepare(u"INSERT INTO harness_meta (key, value) VALUES (?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value"_s);
    version.addBindValue(kSchemaVersionKey);
    version.addBindValue(QString::number(kEventSchemaVersion));
    if (!version.exec()) {
        m_lastError = version.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

qint64 EventStore::headSeq(const QString &conversationId) const
{
    if (conversationId.isEmpty()) {
        return 0;
    }
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen()) {
        return 0;
    }
    QSqlQuery query(db);
    query.prepare(u"SELECT head_seq FROM event_conversation_head WHERE conversation_id = ?"_s);
    query.addBindValue(conversationId);
    if (query.exec() && query.next()) {
        return query.value(0).toLongLong();
    }
    query.prepare(u"SELECT COALESCE(MAX(seq), 0) FROM session_event WHERE conversation_id = ?"_s);
    query.addBindValue(conversationId);
    if (query.exec() && query.next()) {
        return query.value(0).toLongLong();
    }
    return 0;
}

bool EventStore::append(const QString &conversationId, const QList<HarnessEvent> &events,
                        QString *error)
{
    if (conversationId.isEmpty()) {
        m_lastError = u"Cannot append events without a conversation id."_s;
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    if (events.isEmpty()) {
        return true;
    }
    QString localError;
    if (!open(error ? error : &localError)) {
        return false;
    }

    QSqlDatabase db = database();
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    qint64 head = headSeq(conversationId);
    QSqlQuery insert(db);
    insert.prepare(u"INSERT INTO session_event "
                   "(conversation_id, seq, ts, turn_id, step_id, type, payload, imported) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"_s);

    for (HarnessEvent event : events) {
        if (event.seq <= 0) {
            event.seq = ++head;
        } else if (event.seq <= head) {
            m_lastError = u"Event sequence %1 is not after the conversation head %2."_s
                              .arg(event.seq)
                              .arg(head);
            db.rollback();
            if (error) {
                *error = m_lastError;
            }
            return false;
        } else {
            head = event.seq;
        }
        event.conversationId = conversationId;
        if (event.timestampMs <= 0) {
            event.timestampMs = QDateTime::currentMSecsSinceEpoch();
        }

        insert.addBindValue(conversationId);
        insert.addBindValue(event.seq);
        insert.addBindValue(event.timestampMs);
        insert.addBindValue(event.turnId.isEmpty() ? QVariant() : QVariant(event.turnId));
        insert.addBindValue(event.stepId.isEmpty() ? QVariant() : QVariant(event.stepId));
        insert.addBindValue(event.type);
        insert.addBindValue(QString::fromUtf8(Events::encode(event.payload)));
        insert.addBindValue(event.imported ? 1 : 0);
        if (!insert.exec()) {
            m_lastError = insert.lastError().text();
            db.rollback();
            if (error) {
                *error = m_lastError;
            }
            return false;
        }

        if (!applyProjection(event, db, error)) {
            db.rollback();
            return false;
        }
    }

    QSqlQuery updateHead(db);
    updateHead.prepare(u"INSERT INTO event_conversation_head (conversation_id, head_seq) "
                       "VALUES (?, ?) ON CONFLICT(conversation_id) "
                       "DO UPDATE SET head_seq = excluded.head_seq"_s);
    updateHead.addBindValue(conversationId);
    updateHead.addBindValue(head);
    if (!updateHead.exec()) {
        m_lastError = updateHead.lastError().text();
        db.rollback();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    if (!db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

bool EventStore::applyProjection(const HarnessEvent &event, QSqlDatabase &db, QString *error)
{
    int role = -1;
    QString content;
    QString thinking;
    QString toolCallId;
    QString toolName;
    QJsonArray toolCalls;

    if (event.type == EventTypes::TurnStart) {
        const QString text = event.payload.value(u"userText"_s).toString();
        if (text.isEmpty()) {
            return true;
        }
        role = static_cast<int>(ChatMessage::Role::User);
        content = text;
    } else if (event.type == EventTypes::AssistantMessage) {
        role = static_cast<int>(ChatMessage::Role::Assistant);
        content = event.payload.value(u"text"_s).toString();
        thinking = event.payload.value(u"thinking"_s).toString();
        const QJsonArray calls = event.payload.value(u"toolCalls"_s).toArray();
        for (const QJsonValue &value : calls) {
            const QJsonObject call = value.toObject();
            const QString name = call.value(u"name"_s).toString();
            const QString id = call.value(u"id"_s).toString();
            if (name.isEmpty()) {
                continue;
            }
            QJsonObject function;
            function.insert(u"name"_s, name);
            function.insert(u"arguments"_s,
                            call.value(u"arguments"_s).isString()
                                ? call.value(u"arguments"_s).toString()
                                : QString::fromUtf8(QJsonDocument(call.value(u"arguments"_s).toObject())
                                                        .toJson(QJsonDocument::Compact)));
            QJsonObject encoded;
            encoded.insert(u"id"_s, id);
            encoded.insert(u"type"_s, u"function"_s);
            encoded.insert(u"function"_s, function);
            toolCalls.append(encoded);
        }
    } else if (event.type == EventTypes::ToolResult) {
        role = static_cast<int>(ChatMessage::Role::Tool);
        content = event.payload.value(u"output"_s).toString();
        toolCallId = event.payload.value(u"toolCallId"_s).toString();
        toolName = event.payload.value(u"name"_s).toString();
    } else if (event.type == EventTypes::Message) {
        const int storedRole = event.payload.value(u"role"_s).toInt(-1);
        if (storedRole < static_cast<int>(ChatMessage::Role::System)
            || storedRole > static_cast<int>(ChatMessage::Role::Tool)) {
            return true;
        }
        role = storedRole;
        content = event.payload.value(u"content"_s).toString();
        thinking = event.payload.value(u"thinking"_s).toString();
        toolCallId = event.payload.value(u"toolCallId"_s).toString();
        toolName = event.payload.value(u"name"_s).toString();
        if (event.payload.value(u"toolCalls"_s).isArray()) {
            toolCalls = event.payload.value(u"toolCalls"_s).toArray();
        }
    } else {
        return true;
    }

    QSqlQuery query(db);
    query.prepare(u"INSERT OR REPLACE INTO event_message_projection "
                  "(conversation_id, seq, role, content, thinking, tool_call_id, tool_name, tool_calls) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"_s);
    query.addBindValue(event.conversationId);
    query.addBindValue(event.seq);
    query.addBindValue(role);
    query.addBindValue(content);
    query.addBindValue(thinking);
    query.addBindValue(toolCallId);
    query.addBindValue(toolName);
    query.addBindValue(toolCalls.isEmpty() ? QString() : toolCallsJson(toolCalls));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

QList<HarnessEvent> EventStore::read(const QString &conversationId, qint64 fromSeq, int limit,
                                     QString *error) const
{
    QList<HarnessEvent> events;
    if (conversationId.isEmpty()) {
        return events;
    }
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen()) {
        if (error) {
            *error = u"The event database is not open."_s;
        }
        return events;
    }

    QSqlQuery query(db);
    if (limit > 0) {
        query.prepare(u"SELECT seq, ts, turn_id, step_id, type, payload, imported "
                      "FROM session_event WHERE conversation_id = ? AND seq > ? "
                      "ORDER BY seq ASC LIMIT ?"_s);
        query.addBindValue(conversationId);
        query.addBindValue(fromSeq);
        query.addBindValue(limit);
    } else {
        query.prepare(u"SELECT seq, ts, turn_id, step_id, type, payload, imported "
                      "FROM session_event WHERE conversation_id = ? AND seq > ? "
                      "ORDER BY seq ASC"_s);
        query.addBindValue(conversationId);
        query.addBindValue(fromSeq);
    }
    if (!query.exec()) {
        const_cast<EventStore *>(this)->m_lastError = query.lastError().text();
        if (error) {
            *error = query.lastError().text();
        }
        return events;
    }

    while (query.next()) {
        HarnessEvent event;
        event.conversationId = conversationId;
        event.seq = query.value(0).toLongLong();
        event.timestampMs = query.value(1).toLongLong();
        event.turnId = query.value(2).toString();
        event.stepId = query.value(3).toString();
        event.type = query.value(4).toString();
        event.payload = Events::decode(query.value(5).toString().toUtf8());
        event.imported = query.value(6).toInt() != 0;
        events.append(event);
    }
    return events;
}

bool EventStore::rebuildProjection(const QString &conversationId, QString *error)
{
    QString localError;
    if (!open(error ? error : &localError)) {
        return false;
    }
    QSqlDatabase db = database();
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    QSqlQuery clear(db);
    clear.prepare(u"DELETE FROM event_message_projection WHERE conversation_id = ?"_s);
    clear.addBindValue(conversationId);
    if (!clear.exec()) {
        m_lastError = clear.lastError().text();
        db.rollback();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    const QList<HarnessEvent> events = read(conversationId, 0, -1, error);
    for (const HarnessEvent &event : events) {
        if (!applyProjection(event, db, error)) {
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

QList<ChatMessage> EventStore::projectMessages(const QString &conversationId, QString *error) const
{
    QList<ChatMessage> messages;
    QSqlDatabase db = database();
    if (!db.isValid() || !db.isOpen()) {
        if (error) {
            *error = u"The event database is not open."_s;
        }
        return messages;
    }
    QSqlQuery query(db);
    query.prepare(u"SELECT role, content, thinking, tool_call_id, tool_name, tool_calls "
                  "FROM event_message_projection WHERE conversation_id = ? ORDER BY seq ASC"_s);
    query.addBindValue(conversationId);
    if (!query.exec()) {
        if (error) {
            *error = query.lastError().text();
        }
        return messages;
    }
    while (query.next()) {
        ChatMessage message;
        message.role = static_cast<ChatMessage::Role>(query.value(0).toInt());
        message.content = query.value(1).toString();
        message.thinking = query.value(2).toString();
        message.toolCallId = query.value(3).toString();
        message.name = query.value(4).toString();
        const QString calls = query.value(5).toString();
        if (!calls.isEmpty()) {
            const QJsonDocument document = QJsonDocument::fromJson(calls.toUtf8());
            if (document.isArray()) {
                message.toolCalls = document.array();
            }
        }
        messages.append(message);
    }
    return messages;
}

bool EventStore::backfillConversation(const QString &conversationId, QString *error)
{
    QString localError;
    if (!open(error ? error : &localError)) {
        return false;
    }
    if (headSeq(conversationId) > 0) {
        return true;
    }

    QSqlDatabase db = database();
    QSqlQuery payloadQuery(db);
    payloadQuery.prepare(u"SELECT payload FROM conversations WHERE id = ?"_s);
    payloadQuery.addBindValue(conversationId);
    if (!payloadQuery.exec()) {
        // The legacy table may not exist in a standalone event-only database.
        return true;
    }
    if (!payloadQuery.next()) {
        return true;
    }

    const QJsonDocument document = QJsonDocument::fromJson(payloadQuery.value(0).toByteArray());
    if (!document.isObject()) {
        return true;
    }
    const QJsonArray messages = document.object().value(u"messages"_s).toArray();
    if (messages.isEmpty()) {
        return true;
    }

    QList<HarnessEvent> events;
    QString turnId;
    int turnNumber = 0;
    for (const QJsonValue &value : messages) {
        const QJsonObject object = value.toObject();
        const int role = object.value(u"role"_s).toInt(static_cast<int>(ChatMessage::Role::User));
        const QString content = object.value(u"content"_s).toString();
        if (role == static_cast<int>(ChatMessage::Role::User)) {
            turnId = QStringLiteral("imported-%1").arg(++turnNumber);
            QJsonObject payload;
            payload.insert(u"userText"_s, content);
            events.append(Events::make(EventTypes::TurnStart, payload, turnId));
        } else if (role == static_cast<int>(ChatMessage::Role::Assistant)) {
            QJsonObject payload;
            payload.insert(u"text"_s, content);
            payload.insert(u"thinking"_s, object.value(u"thinking"_s).toString());
            QJsonArray calls;
            for (const QJsonValue &callValue : object.value(u"toolCalls"_s).toArray()) {
                const QJsonObject call = callValue.toObject();
                const QJsonObject function = call.value(u"function"_s).toObject();
                if (function.value(u"name"_s).toString().isEmpty()) {
                    continue;
                }
                QJsonObject encoded;
                encoded.insert(u"id"_s, call.value(u"id"_s).toString());
                encoded.insert(u"name"_s, function.value(u"name"_s).toString());
                encoded.insert(u"arguments"_s, function.value(u"arguments"_s).toString());
                calls.append(encoded);
            }
            if (!calls.isEmpty()) {
                payload.insert(u"toolCalls"_s, calls);
            }
            events.append(Events::make(EventTypes::AssistantMessage, payload, turnId));
        } else if (role == static_cast<int>(ChatMessage::Role::Tool)) {
            QJsonObject payload;
            payload.insert(u"toolCallId"_s, object.value(u"toolCallId"_s).toString());
            payload.insert(u"name"_s, object.value(u"name"_s).toString());
            payload.insert(u"output"_s, content);
            events.append(Events::make(EventTypes::ToolResult, payload, turnId));
        } else if (role == static_cast<int>(ChatMessage::Role::System)) {
            QJsonObject payload;
            payload.insert(u"role"_s, role);
            payload.insert(u"content"_s, content);
            events.append(Events::make(EventTypes::Message, payload, turnId));
        }
    }
    if (!turnId.isEmpty()) {
        QJsonObject payload;
        payload.insert(u"status"_s, u"imported"_s);
        events.append(Events::make(EventTypes::TurnEnd, payload, turnId));
    }
    for (HarnessEvent &event : events) {
        event.imported = true;
    }
    return append(conversationId, events, error);
}

bool EventStore::backfillAll(QString *error)
{
    QString localError;
    if (!open(error ? error : &localError)) {
        return false;
    }
    QSqlDatabase db = database();
    QSqlQuery query(db);
    if (!query.exec(u"SELECT id FROM conversations ORDER BY updated_at ASC"_s)) {
        return true;
    }
    QStringList ids;
    while (query.next()) {
        ids.append(query.value(0).toString());
    }
    for (const QString &id : ids) {
        if (!backfillConversation(id, error)) {
            return false;
        }
    }
    return true;
}

bool EventStore::clearAll(QString *error)
{
    QString localError;
    if (!open(error ? error : &localError)) {
        return false;
    }
    QSqlDatabase db = database();
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    for (const QString &table :
         {u"session_event"_s, u"event_message_projection"_s, u"event_conversation_head"_s}) {
        QSqlQuery query(db);
        if (!query.exec(u"DELETE FROM %1"_s.arg(table))) {
            m_lastError = query.lastError().text();
            db.rollback();
            if (error) {
                *error = m_lastError;
            }
            return false;
        }
    }
    if (!db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

bool EventStore::removeConversation(const QString &conversationId, QString *error)
{
    QString localError;
    if (!open(error ? error : &localError)) {
        return false;
    }
    QSqlDatabase db = database();
    if (!db.transaction()) {
        m_lastError = db.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    for (const QString &table :
         {u"session_event"_s, u"event_message_projection"_s, u"event_conversation_head"_s}) {
        QSqlQuery query(db);
        if (table == u"event_conversation_head"_s) {
            query.prepare(u"DELETE FROM event_conversation_head WHERE conversation_id = ?"_s);
        } else {
            query.prepare(u"DELETE FROM %1 WHERE conversation_id = ?"_s.arg(table));
        }
        query.addBindValue(conversationId);
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            db.rollback();
            if (error) {
                *error = m_lastError;
            }
            return false;
        }
    }
    if (!db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

} // namespace KateAi
