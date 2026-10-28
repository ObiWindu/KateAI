/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "sessionstore.h"

#include "contextmanager.h"
#include "types.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGlobalStatic>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>

#include <atomic>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

// Helper functions (defined first so they can be used by static methods)
static QJsonArray messagesToJson(const QList<ChatMessage> &messages)
{
    QJsonArray arr;
    for (const auto &msg : messages) {
        QJsonObject obj;
        obj[u"role"_s] = static_cast<int>(msg.role);
        obj[u"content"_s] = msg.content;
        obj[u"thinking"_s] = msg.thinking;
        obj[u"plan"_s] = msg.plan;
        obj[u"toolCallId"_s] = msg.toolCallId;
        obj[u"name"_s] = msg.name;
        obj[u"toolCalls"_s] = msg.toolCalls;
        arr.append(obj);
    }
    return arr;
}

static QList<ChatMessage> messagesFromJson(const QJsonArray &arr)
{
    QList<ChatMessage> messages;
    for (const auto &val : arr) {
        const QJsonObject obj = val.toObject();
        ChatMessage msg;
        // The role comes out of the stored payload. A corrupted value would
        // be an enumerator no switch handles, and the message would be
        // silently dropped everywhere.
        const int roleValue = obj[u"role"_s].toInt(static_cast<int>(ChatMessage::Role::User));
        if (roleValue < static_cast<int>(ChatMessage::Role::System)
            || roleValue > static_cast<int>(ChatMessage::Role::Tool)) {
            qWarning() << "KateAI: a stored message has an unknown role" << roleValue << "- using User";
            msg.role = ChatMessage::Role::User;
        } else {
            msg.role = static_cast<ChatMessage::Role>(roleValue);
        }
        msg.content = obj[u"content"_s].toString();
        msg.thinking = obj[u"thinking"_s].toString();
        msg.plan = obj[u"plan"_s].toArray();
        msg.toolCallId = obj[u"toolCallId"_s].toString();
        msg.name = obj[u"name"_s].toString();
        msg.toolCalls = obj[u"toolCalls"_s].toArray();
        messages.append(msg);
    }
    return messages;
}

static QJsonArray stringListToJson(const QList<QString> &list)
{
    QJsonArray arr;
    for (const auto &s : list) {
        arr.append(s);
    }
    return arr;
}

static QList<QString> stringListFromJson(const QJsonArray &arr)
{
    QList<QString> list;
    for (const auto &val : arr) {
        list.append(val.toString());
    }
    return list;
}

static QJsonObject hashToJson(const QHash<QString, int> &hash)
{
    QJsonObject obj;
    for (auto it = hash.constBegin(); it != hash.constEnd(); ++it) {
        obj[it.key()] = it.value();
    }
    return obj;
}

static QHash<QString, int> hashFromJson(const QJsonObject &obj)
{
    QHash<QString, int> hash;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        hash[it.key()] = it.value().toInt();
    }
    return hash;
}

static QString generateConversationId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static bool isBadTitle(const QString &title)
{
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty()) {
        return true;
    }
    // Compaction summaries must never become a conversation title. Older
    // builds stored the first line of a compacted summary as the title, so a
    // title that starts with (or is a truncated prefix of) the summary header
    // is treated as missing and regenerated.
    if (trimmed.startsWith(ContextManager::summaryHeader().left(20))) {
        return true;
    }
    if (trimmed.contains(u"compacted to fit the context window"_s)) {
        return true;
    }
    if (trimmed.startsWith(u"[Earlier conversation was compacted"_s)) {
        return true;
    }
    return false;
}

static QString generateTitleFromMessages(const QList<ChatMessage> &messages)
{
    // First pass: a real user request that survived compaction.
    for (const auto &msg : messages) {
        if (msg.role != ChatMessage::Role::User || msg.content.trimmed().isEmpty()) {
            continue;
        }
        if (msg.content.trimmed().startsWith(ContextManager::summaryHeader())) {
            continue; // compaction summary, not a real request
        }
        if (msg.content.trimmed().startsWith(u"[KateAI agent controller]"_s)) {
            continue;
        }
        QString title = msg.content.trimmed().split(u'\n').first();
        if (title.length() > 50) {
            title = truncateSafe(title, 47) + u"...";
        }
        if (!title.isEmpty() && !isBadTitle(title)) {
            return title;
        }
    }

    // Second pass: if every user message was folded into a summary, recover the
    // first request the summary still records instead of showing the marker.
    for (const auto &msg : messages) {
        if (msg.role != ChatMessage::Role::User || msg.content.trimmed().isEmpty()) {
            continue;
        }
        if (!msg.content.trimmed().startsWith(ContextManager::summaryHeader())) {
            continue;
        }
        const QStringList lines = msg.content.split(u'\n');
        bool inAsked = false;
        for (const QString &line : lines) {
            const QString trimmedLine = line.trimmed();
            if (trimmedLine.startsWith(u"What was asked:"_s)) {
                inAsked = true;
                continue;
            }
            if (inAsked && trimmedLine.startsWith(u"1. "_s)) {
                QString title = trimmedLine.mid(3).trimmed();
                if (title.length() > 50) {
                    title = truncateSafe(title, 47) + u"...";
                }
                if (!title.isEmpty() && !isBadTitle(title)) {
                    return title;
                }
                break;
            }
            if (inAsked && trimmedLine.isEmpty()) {
                break;
            }
        }
    }

    return QDateTime::currentDateTime().toString(u"yyyy-MM-dd hh:mm"_s);
}

// The whole session state lives in a single JSON blob: it is always read and
// written as one unit, so there is nothing to gain from splitting it up.
static QByteArray sessionDataToJson(const SessionStore::SessionData &data)
{
    QJsonObject obj;
    obj[u"version"_s] = data.version;
    obj[u"messages"_s] = messagesToJson(data.messages);
    obj[u"currentThinking"_s] = data.currentThinking;
    obj[u"currentPlan"_s] = data.currentPlan;
    obj[u"planShown"_s] = data.planShown;
    obj[u"currentAssistant"_s] = data.currentAssistant;
    obj[u"stateEpoch"_s] = static_cast<qint64>(data.stateEpoch);
    obj[u"actionSignatures"_s] = stringListToJson(data.actionSignatures);
    obj[u"actionRepeatCounts"_s] = hashToJson(data.actionRepeatCounts);
    obj[u"changedPaths"_s] = stringListToJson(data.changedPaths);
    obj[u"changesNeedVerification"_s] = data.changesNeedVerification;
    obj[u"verificationAttempted"_s] = data.verificationAttempted;
    obj[u"verificationPromptCount"_s] = data.verificationPromptCount;
    obj[u"modelRequests"_s] = data.modelRequests;
    obj[u"toolCalls"_s] = data.toolCalls;
    obj[u"acpSessionId"_s] = data.acpSessionId;
    obj[u"workspace"_s] = data.workspace;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

static SessionStore::SessionData sessionDataFromJson(const QByteArray &payload)
{
    SessionStore::SessionData data;
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (doc.isNull() || !doc.isObject()) {
        return data;
    }

    const QJsonObject obj = doc.object();
    data.version = obj[u"version"_s].toInt(SessionStore::CURRENT_VERSION);
    data.messages = messagesFromJson(obj[u"messages"_s].toArray());
    data.currentThinking = obj[u"currentThinking"_s].toString();
    data.currentPlan = obj[u"currentPlan"_s].toArray();
    data.planShown = obj[u"planShown"_s].toBool();
    data.currentAssistant = obj[u"currentAssistant"_s].toString();
    // The epoch counts up, so a negative value is nonsense. Casting it to an
    // unsigned type would turn it into a huge number.
    data.stateEpoch = static_cast<quint64>(qMax<qint64>(0, obj[u"stateEpoch"_s].toInteger()));
    data.actionSignatures = stringListFromJson(obj[u"actionSignatures"_s].toArray());
    data.actionRepeatCounts = hashFromJson(obj[u"actionRepeatCounts"_s].toObject());
    data.changedPaths = stringListFromJson(obj[u"changedPaths"_s].toArray());
    data.changesNeedVerification = obj[u"changesNeedVerification"_s].toBool();
    data.verificationAttempted = obj[u"verificationAttempted"_s].toBool();
    data.verificationPromptCount = obj[u"verificationPromptCount"_s].toInt();
    data.modelRequests = obj[u"modelRequests"_s].toInt();
    data.toolCalls = obj[u"toolCalls"_s].toInt();
    data.acpSessionId = obj[u"acpSessionId"_s].toString();
    data.workspace = obj[u"workspace"_s].toString();

    // Version 2 adds the version field - no data migration needed, just ensure version is set
    if (data.version < SessionStore::CURRENT_VERSION) {
        data.version = SessionStore::CURRENT_VERSION;
    }

    return data;
}

static void repairBadConversationTitles(const QSqlDatabase &db)
{
    QSqlQuery query(db);
    if (!query.exec(u"SELECT id, title, payload FROM conversations"_s)) {
        return;
    }

    struct Row {
        QString id;
        QString title;
        QByteArray payload;
    };
    QList<Row> rows;
    while (query.next()) {
        Row row;
        row.id = query.value(0).toString();
        row.title = query.value(1).toString();
        row.payload = query.value(2).toByteArray();
        rows.append(row);
    }

    for (const Row &row : rows) {
        if (!isBadTitle(row.title)) {
            continue;
        }
        const SessionStore::SessionData data = sessionDataFromJson(row.payload);
        const QString newTitle = generateTitleFromMessages(data.messages);
        if (newTitle.isEmpty() || isBadTitle(newTitle) || newTitle == row.title) {
            continue;
        }
        QSqlQuery update(db);
        update.prepare(u"UPDATE conversations SET title = ? WHERE id = ?"_s);
        update.addBindValue(newTitle);
        update.addBindValue(row.id);
        if (!update.exec()) {
            qWarning() << "KateAI: cannot repair conversation title" << row.id << update.lastError().text();
        }
    }
}

// --- session database ---------------------------------------------------

static const QString ACTIVE_CONVERSATION_KEY = u"active_conversation"_s;
static const QString LEGACY_IMPORT_KEY = u"legacy_kconfig_imported"_s;

// Guards the database handle and every read-modify-write sequence below.
// Recursive because the public helpers call each other (save() -> saveConversation(),
// clear() -> deleteConversation(), ...).
static QRecursiveMutex &storeMutex()
{
    static QRecursiveMutex mutex;
    return mutex;
}

static QString databaseFilePath()
{
    const QByteArray overridePath = qgetenv("KATEAI_SESSION_DB");
    if (!overridePath.isEmpty()) {
        return QString::fromLocal8Bit(overridePath);
    }

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dataDir.isEmpty()) {
        dataDir = QDir::home().filePath(u".local/share"_s);
    }
    return dataDir + u"/kateai/sessions.sqlite"_s;
}

// WAL keeps readers from blocking on a write, which is what makes restoring the
// last session fast even while a request is streaming in.
static bool applySchema(QSqlDatabase &db)
{
    QSqlQuery pragmas(db);
    pragmas.exec(u"PRAGMA journal_mode=WAL"_s);
    pragmas.exec(u"PRAGMA synchronous=NORMAL"_s);

    static const char *const statements[] = {
        "CREATE TABLE IF NOT EXISTS meta ("
        "key TEXT PRIMARY KEY,"
        "value TEXT NOT NULL)",

        // The payload holds the full SessionData; the remaining columns mirror the
        // metadata the history menu needs so listing conversations never reads it.
        "CREATE TABLE IF NOT EXISTS conversations ("
        "id TEXT PRIMARY KEY,"
        "title TEXT NOT NULL DEFAULT '',"
        "created_at INTEGER NOT NULL DEFAULT 0,"
        "updated_at INTEGER NOT NULL DEFAULT 0,"
        "message_count INTEGER NOT NULL DEFAULT 0,"
        "version INTEGER NOT NULL DEFAULT 0,"
        "payload BLOB NOT NULL)",

        "CREATE INDEX IF NOT EXISTS conversations_updated_at ON conversations (updated_at DESC)",
    };

    for (const char *statement : statements) {
        QSqlQuery query(db);
        if (!query.exec(QString::fromLatin1(statement))) {
            qWarning() << "KateAI: cannot create the session database schema:" << query.lastError().text();
            return false;
        }
    }

    return true;
}

// Reads the pre-database (KConfig) history into the given connection.
static void importLegacyConversations(QSqlDatabase db);

// Opens (once per thread) the database backing the session store. Returns an
// invalid handle and sets *ok to false when the store is unavailable; callers
// then degrade to "no history" instead of losing the session.

// Cache of the per-thread connection names, guarded by storeMutex(). It is a
// Q_GLOBAL_STATIC rather than a function-local static: a QThread::finished
// handler erases the entry of a dying thread, and that handler can also run
// for the main thread during static teardown, after plain function-local
// statics have been destroyed. The null return of a destroyed global static
// keeps that path safe instead of touching freed memory.
using ThreadConnectionNames = QHash<QThread *, QString>;
Q_GLOBAL_STATIC(ThreadConnectionNames, connectionNames)

static QSqlDatabase openStore(bool *ok)
{
    *ok = false;

    if (!QSqlDatabase::isDriverAvailable(u"QSQLITE"_s)) {
        qWarning() << "KateAI: the QSQLITE driver is missing, session history is unavailable.";
        return {};
    }

    QThread *thread = QThread::currentThread();

    auto *names = connectionNames();
    if (names) {
        const auto cached = names->constFind(thread);
        if (cached != names->constEnd()) {
            QSqlDatabase db = QSqlDatabase::database(cached.value(), false);
            if (db.isValid() && db.isOpen()) {
                *ok = true;
                return db;
            }
            names->erase(cached);
        }
    }

    const QString path = databaseFilePath();
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        qWarning() << "KateAI: cannot create the session database directory:" << directory;
    }

    // A thread that ends frees its address, and a new thread can get the same
    // one. A counter keeps the connection names unique, and the connection is
    // closed when the thread ends.
    static quint64 connectionCounter = 0;
    const QString connectionName = u"kateai_sessions_%1_%2"_s
                                       .arg(reinterpret_cast<quintptr>(thread))
                                       .arg(++connectionCounter);
    QString error;

    {
        // The handle must be destroyed before removeDatabase() runs.
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, connectionName);
        db.setDatabaseName(path);
        if (!db.open()) {
            error = db.lastError().text();
        } else if (!applySchema(db)) {
            error = u"cannot create the schema"_s;
            db.close();
        }
    }

    if (!error.isEmpty()) {
        qWarning() << "KateAI: cannot open the session database" << path << error;
        QSqlDatabase::removeDatabase(connectionName);
        return {};
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName, true);
    importLegacyConversations(db);
    repairBadConversationTitles(db);

    // The connection belongs to the thread that opened it. When the thread
    // ends, the connection has to go with it, or the next thread with the same
    // address would find the connection of a dead thread.
    auto *cleanup = new QObject(thread);
    QObject::connect(thread, &QThread::finished, cleanup, [thread, connectionName]() {
        {
            QSqlDatabase connection = QSqlDatabase::database(connectionName, false);
            if (connection.isValid()) {
                connection.close();
            }
        }
        QSqlDatabase::removeDatabase(connectionName);
        // The thread's address can be reused by a later thread: drop the
        // stale cache entry so nobody inherits a removed connection, and so
        // the cache cannot grow without bound. The guard also covers a
        // finished() that fires for the main thread during static teardown,
        // where the global static has already been destroyed.
        if (auto *names = connectionNames()) {
            QMutexLocker locker(&storeMutex());
            names->remove(thread);
        }
    });

    if (names) {
        names->insert(thread, connectionName);
    }
    *ok = true;
    return db;
}

static QString readMeta(const QSqlDatabase &db, const QString &key)
{
    QSqlQuery query(db);
    query.prepare(u"SELECT value FROM meta WHERE key = ?"_s);
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return QString();
    }
    return query.value(0).toString();
}

static void clearMeta(QSqlDatabase db, const QString &key);

static void writeMeta(QSqlDatabase db, const QString &key, const QString &value)
{
    // Empty means clear: callers previously had to remember clearMeta, and
    // saveConversation worked around with a double readMeta query.
    if (value.isEmpty()) {
        clearMeta(db, key);
        return;
    }
    QSqlQuery query(db);
    query.prepare(u"INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"_s);
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        qWarning() << "KateAI: cannot store" << key << "in the session database:" << query.lastError().text();
    }
}

static void clearMeta(QSqlDatabase db, const QString &key)
{
    QSqlQuery query(db);
    query.prepare(u"DELETE FROM meta WHERE key = ?"_s);
    query.addBindValue(key);
    if (!query.exec()) {
        qWarning() << "KateAI: cannot clear" << key << "in the session database:" << query.lastError().text();
    }
}

// A session is saved several times per turn, often within the same millisecond.
// Strictly increasing timestamps keep "most recent first" ordering stable.
static qint64 nextTimestamp()
{
    static qint64 lastIssued = 0; // guarded by storeMutex()
    lastIssued = qMax(QDateTime::currentMSecsSinceEpoch(), lastIssued + 1);
    return lastIssued;
}

static bool conversationExists(const QSqlDatabase &db, const QString &conversationId)
{
    QSqlQuery query(db);
    query.prepare(u"SELECT 1 FROM conversations WHERE id = ?"_s);
    query.addBindValue(conversationId);
    return query.exec() && query.next();
}

// --- one-time import of the legacy KConfig history -----------------------

static const QString LEGACY_LIST_GROUP = u"KateAIConversations"_s;
static const QString LEGACY_CONVERSATION_GROUP_PREFIX = u"KateAIConversation_"_s;
static const QString LEGACY_LIST_ENTRY_PREFIX = u"conv_"_s;
static const QString LEGACY_ACTIVE_ENTRY = u"ActiveConversation"_s;

static SessionStore::SessionData legacyConversationData(const QString &id)
{
    SessionStore::SessionData data;
    const KConfigGroup g(KSharedConfig::openConfig(), LEGACY_CONVERSATION_GROUP_PREFIX + id);

    data.version = g.readEntry(u"Version"_s, 1);

    const QByteArray messagesData = g.readEntry(u"Messages"_s, QByteArray());
    if (!messagesData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(messagesData);
        if (!doc.isNull() && doc.isArray()) {
            data.messages = messagesFromJson(doc.array());
        }
    }

    data.currentThinking = g.readEntry(u"CurrentThinking"_s, QString());
    const QByteArray planData = g.readEntry(u"CurrentPlan"_s, QByteArray());
    if (!planData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(planData);
        if (!doc.isNull() && doc.isArray()) {
            data.currentPlan = doc.array();
        }
    }
    data.planShown = g.readEntry(u"PlanShown"_s, false);
    data.currentAssistant = g.readEntry(u"CurrentAssistant"_s, QString());
    data.stateEpoch = g.readEntry(u"StateEpoch"_s, quint64(0));

    const QByteArray actionSigsData = g.readEntry(u"ActionSignatures"_s, QByteArray());
    if (!actionSigsData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(actionSigsData);
        if (!doc.isNull() && doc.isArray()) {
            data.actionSignatures = stringListFromJson(doc.array());
        }
    }

    const QByteArray actionCountsData = g.readEntry(u"ActionRepeatCounts"_s, QByteArray());
    if (!actionCountsData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(actionCountsData);
        if (!doc.isNull() && doc.isObject()) {
            data.actionRepeatCounts = hashFromJson(doc.object());
        }
    }

    const QByteArray changedPathsData = g.readEntry(u"ChangedPaths"_s, QByteArray());
    if (!changedPathsData.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(changedPathsData);
        if (!doc.isNull() && doc.isArray()) {
            data.changedPaths = stringListFromJson(doc.array());
        }
    }

    data.changesNeedVerification = g.readEntry(u"ChangesNeedVerification"_s, false);
    data.verificationAttempted = g.readEntry(u"VerificationAttempted"_s, false);
    data.verificationPromptCount = g.readEntry(u"VerificationPromptCount"_s, 0);
    data.modelRequests = g.readEntry(u"ModelRequests"_s, 0);
    data.toolCalls = g.readEntry(u"ToolCalls"_s, 0);

    if (data.version < SessionStore::CURRENT_VERSION) {
        data.version = SessionStore::CURRENT_VERSION;
    }

    return data;
}

// Releases before this version stored the history as KConfig groups. Importing
// them once keeps existing conversations reachable after the upgrade; the legacy
// entries are only dropped once every conversation is readable from the database.
static void importLegacyConversations(QSqlDatabase db)
{
    // The import runs once per process. The flag in the database only stops
    // a second import in a later session; two threads that open a connection
    // at the same time would both pass that check and import at the same
    // time.
    static std::atomic_bool importStarted{false};
    bool expected = false;
    if (!importStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!readMeta(db, LEGACY_IMPORT_KEY).isEmpty()) {
        return;
    }

    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    KConfigGroup listGroup(config, LEGACY_LIST_GROUP);

    QStringList conversationIds;
    for (const QString &key : listGroup.keyList()) {
        if (key.startsWith(LEGACY_LIST_ENTRY_PREFIX)) {
            conversationIds.append(key.mid(LEGACY_LIST_ENTRY_PREFIX.size()));
        }
    }
    const QString activeId = listGroup.readEntry(LEGACY_ACTIVE_ENTRY, QString());

    if (conversationIds.isEmpty()) {
        writeMeta(db, LEGACY_IMPORT_KEY, u"1"_s);
        return;
    }

    if (!db.transaction()) {
        qWarning() << "KateAI: cannot start the session history import:" << db.lastError().text();
        return;
    }

    const qint64 now = nextTimestamp();
    bool complete = true;

    for (const QString &id : conversationIds) {
        const SessionStore::SessionData data = legacyConversationData(id);
        const KConfigGroup convGroup(config, LEGACY_CONVERSATION_GROUP_PREFIX + id);
        const QDateTime createdAt = QDateTime::fromString(convGroup.readEntry(u"CreatedAt"_s, QString()), Qt::ISODate);
        const QDateTime updatedAt = QDateTime::fromString(convGroup.readEntry(u"UpdatedAt"_s, QString()), Qt::ISODate);

        QSqlQuery insert(db);
        insert.prepare(u"INSERT OR IGNORE INTO conversations (id, title, created_at, updated_at, message_count, version, payload) VALUES (?, ?, ?, ?, ?, ?, ?)"_s);
        insert.addBindValue(id);
        insert.addBindValue(convGroup.readEntry(u"Title"_s, generateTitleFromMessages(data.messages)));
        insert.addBindValue(createdAt.isValid() ? createdAt.toMSecsSinceEpoch() : now);
        insert.addBindValue(updatedAt.isValid() ? updatedAt.toMSecsSinceEpoch() : now);
        insert.addBindValue(data.messages.size());
        insert.addBindValue(SessionStore::CURRENT_VERSION);
        insert.addBindValue(sessionDataToJson(data));

        if (!insert.exec()) {
            qWarning() << "KateAI: cannot import conversation" << id << insert.lastError().text();
            complete = false;
            break;
        }

        if (id == activeId && conversationExists(db, id)) {
            writeMeta(db, ACTIVE_CONVERSATION_KEY, id);
        }
    }

    if (complete) {
        for (const QString &id : conversationIds) {
            if (!conversationExists(db, id)) {
                complete = false;
                break;
            }
        }
    }

    if (!complete) {
        db.rollback();
        return; // Keep the legacy entries, the next start retries the import.
    }

    if (!db.commit()) {
        qWarning() << "KateAI: cannot commit the session history import:" << db.lastError().text();
        return;
    }

    for (const QString &id : conversationIds) {
        KConfigGroup(config, LEGACY_CONVERSATION_GROUP_PREFIX + id).deleteGroup();
    }
    listGroup.deleteGroup();
    config->sync();

    writeMeta(db, LEGACY_IMPORT_KEY, u"1"_s);
}

SessionStore::SessionData SessionStore::load()
{
    // Load the active conversation
    const QString activeId = getActiveConversationId();
    if (!activeId.isEmpty()) {
        const SessionData activeData = loadConversation(activeId);
        if (!activeData.messages.isEmpty()) {
            return activeData;
        }
    }

    // The active pointer can refer to a new, not-yet-saved chat (or a row that
    // was lost before its transaction completed). Resume the newest persisted
    // conversation instead of presenting an empty transcript while history is
    // still available.
    const QList<ConversationInfo> latest = listConversations(1);
    if (!latest.isEmpty()) {
        setActiveConversation(latest.first().id);
        return loadConversation(latest.first().id);
    }

    return SessionData();
}

void SessionStore::save(const SessionData &data, int maxConversations)
{
    const QString activeId = getActiveConversationId();
    if (!activeId.isEmpty()) {
        saveConversation(activeId, data, QString(), maxConversations);
    } else {
        // Create a new conversation if none active
        const QString newId = createNewConversation();
        saveConversation(newId, data, QString(), maxConversations);
    }
}

void SessionStore::clear()
{
    const QString activeId = getActiveConversationId();
    if (!activeId.isEmpty()) {
        deleteConversation(activeId);
    }
}

QList<SessionStore::ConversationInfo> SessionStore::listConversations(int maxConversations)
{
    QList<ConversationInfo> conversations;

    QMutexLocker locker(&storeMutex());
    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return conversations;
    }

    const QString activeId = readMeta(db, ACTIVE_CONVERSATION_KEY);

    // A LIMIT placeholder cannot be left unbound, so build the query for the
    // requested bound (0 means "no limit").
    QSqlQuery query(db);
    if (maxConversations > 0) {
        query.prepare(u"SELECT id, title, created_at, updated_at, message_count, payload FROM conversations ORDER BY updated_at DESC LIMIT ?"_s);
        query.addBindValue(maxConversations);
    } else {
        query.prepare(u"SELECT id, title, created_at, updated_at, message_count, payload FROM conversations ORDER BY updated_at DESC"_s);
    }
    if (!query.exec()) {
        qWarning() << "KateAI: cannot list conversations:" << query.lastError().text();
        return conversations;
    }

    while (query.next()) {
        ConversationInfo info;
        info.id = query.value(0).toString();
        info.title = query.value(1).toString();
        info.createdAt = QDateTime::fromMSecsSinceEpoch(query.value(2).toLongLong());
        info.updatedAt = QDateTime::fromMSecsSinceEpoch(query.value(3).toLongLong());
        info.messageCount = query.value(4).toInt();
        info.isActive = (info.id == activeId);

        // Repair any title that still carries a compaction marker so the history
        // list never shows "earlier conversation was compacted...".
        if (isBadTitle(info.title)) {
            const SessionStore::SessionData data = sessionDataFromJson(query.value(5).toByteArray());
            const QString repaired = generateTitleFromMessages(data.messages);
            if (!repaired.isEmpty() && !isBadTitle(repaired)) {
                info.title = repaired;
                QSqlQuery upd(db);
                upd.prepare(u"UPDATE conversations SET title = ? WHERE id = ?"_s);
                upd.addBindValue(repaired);
                upd.addBindValue(info.id);
                upd.exec();
            }
        }

        conversations.append(info);
    }

    return conversations;
}

QString SessionStore::conversationTitle(const QString &conversationId)
{
    if (conversationId.isEmpty()) {
        return QString();
    }
    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return QString();
    }

    QSqlQuery query(db);
    query.prepare(u"SELECT title, payload FROM conversations WHERE id = ?"_s);
    query.addBindValue(conversationId);
    if (!query.exec() || !query.next()) {
        return QString();
    }
    QString title = query.value(0).toString();
    if (isBadTitle(title)) {
        const SessionStore::SessionData data = sessionDataFromJson(query.value(1).toByteArray());
        const QString repaired = generateTitleFromMessages(data.messages);
        if (!repaired.isEmpty() && !isBadTitle(repaired)) {
            title = repaired;
            QSqlQuery upd(db);
            upd.prepare(u"UPDATE conversations SET title = ? WHERE id = ?"_s);
            upd.addBindValue(repaired);
            upd.addBindValue(conversationId);
            upd.exec();
        }
    }
    return title;
}

SessionStore::SessionData SessionStore::loadConversation(const QString &conversationId)
{
    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return SessionData();
    }

    QSqlQuery query(db);
    query.prepare(u"SELECT payload FROM conversations WHERE id = ?"_s);
    query.addBindValue(conversationId);
    if (!query.exec() || !query.next()) {
        return SessionData();
    }

    return sessionDataFromJson(query.value(0).toByteArray());
}

bool SessionStore::saveConversation(const QString &conversationId, const SessionData &data, const QString &title, int maxConversations)
{
    if (conversationId.isEmpty()) {
        qWarning() << "KateAI: refusing to save a conversation with an empty ID";
        return false;
    }

    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return false;
    }

    // Generate title if not provided
    const QString convTitle = title.isEmpty() ? generateTitleFromMessages(data.messages) : title;
    const qint64 now = nextTimestamp();

    if (!db.transaction()) {
        qWarning() << "KateAI: cannot save conversation" << conversationId << db.lastError().text();
        return false;
    }

    bool stored = false;
    // Whether the conversation is new. The caller uses it to know if the
    // number of stored conversations grew.
    const bool isNew = !conversationExists(db, conversationId);

    QString convTitleToUse = convTitle;
    if (!isNew && title.isEmpty()) {
        // Preserve existing title when updating (e.g. after context compaction)
        // so titles don't get lost or regenerated from compacted messages.
        // A previously stored bad/compacted title is treated as missing and
        // regenerated from the (possibly compacted) messages instead of being
        // kept forever.
        QSqlQuery getTitle(db);
        getTitle.prepare(u"SELECT title FROM conversations WHERE id = ?"_s);
        getTitle.addBindValue(conversationId);
        if (getTitle.exec() && getTitle.next()) {
            const QString existingTitle = getTitle.value(0).toString();
            if (!existingTitle.isEmpty() && !isBadTitle(existingTitle)) {
                convTitleToUse = existingTitle;
            } else {
                convTitleToUse = generateTitleFromMessages(data.messages);
            }
        } else {
            convTitleToUse = generateTitleFromMessages(data.messages);
        }
    } else if (isNew && title.isEmpty()) {
        convTitleToUse = generateTitleFromMessages(data.messages);
    } else if (!title.isEmpty()) {
        convTitleToUse = title;
    }

    if (!isNew) {
        QSqlQuery update(db);
        update.prepare(u"UPDATE conversations SET title = ?, updated_at = ?, message_count = ?, version = ?, payload = ? WHERE id = ?"_s);
        update.addBindValue(convTitleToUse);
        update.addBindValue(now);
        update.addBindValue(data.messages.size());
        update.addBindValue(CURRENT_VERSION);
        update.addBindValue(sessionDataToJson(data));
        update.addBindValue(conversationId);
        stored = update.exec();
        if (!stored) {
            qWarning() << "KateAI: cannot update conversation" << conversationId << update.lastError().text();
        }
    } else {
        QSqlQuery insert(db);
        insert.prepare(u"INSERT INTO conversations (id, title, created_at, updated_at, message_count, version, payload) VALUES (?, ?, ?, ?, ?, ?, ?)"_s);
        insert.addBindValue(conversationId);
        insert.addBindValue(convTitle);
        insert.addBindValue(now);
        insert.addBindValue(now);
        insert.addBindValue(data.messages.size());
        insert.addBindValue(CURRENT_VERSION);
        insert.addBindValue(sessionDataToJson(data));
        stored = insert.exec();
        if (!stored) {
            qWarning() << "KateAI: cannot store conversation" << conversationId << insert.lastError().text();
        }
    }

    if (stored) {
        // Keep the restore pointer in the same transaction as the transcript.
        // A crash must not leave SQLite pointing at an unpersisted blank chat.
        // Only the conversation the user is actually in may become the active
        // one: saving any conversation would otherwise switch the pointer
        // away from the chat the user is reading.
        if (readMeta(db, ACTIVE_CONVERSATION_KEY).isEmpty() || readMeta(db, ACTIVE_CONVERSATION_KEY) == conversationId) {
            writeMeta(db, ACTIVE_CONVERSATION_KEY, conversationId);
        }
    }

    if (!stored || !db.commit()) {
        db.rollback();
        qWarning() << "KateAI: cannot commit conversation" << conversationId << db.lastError().text();
        return false;
    }

    // Prune old conversations if needed
    pruneOldConversations(maxConversations);
    return isNew;
}

void SessionStore::deleteConversation(const QString &conversationId)
{
    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return;
    }

    if (!db.transaction()) {
        qWarning() << "KateAI: cannot delete conversation" << conversationId << db.lastError().text();
        return;
    }

    QSqlQuery query(db);
    query.prepare(u"DELETE FROM conversations WHERE id = ?"_s);
    query.addBindValue(conversationId);
    if (!query.exec()) {
        db.rollback();
        qWarning() << "KateAI: cannot delete conversation" << conversationId << query.lastError().text();
        return;
    }

    // If this was the active conversation, clear active
    if (readMeta(db, ACTIVE_CONVERSATION_KEY) == conversationId) {
        clearMeta(db, ACTIVE_CONVERSATION_KEY);
    }

    if (!db.commit()) {
        qWarning() << "KateAI: cannot commit the deletion of conversation" << conversationId << db.lastError().text();
    }
}

QString SessionStore::createNewConversation()
{
    const QString newId = generateConversationId();

    // Only set this ID as active — do NOT save an empty conversation record yet.
    // The conversation will be registered in the list the first time
    // saveConversation() is called with real messages, preventing ghost entries
    // from appearing in the history menu.
    setActiveConversation(newId);

    return newId;
}

void SessionStore::setActiveConversation(const QString &conversationId)
{
    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return;
    }

    if (conversationId.isEmpty()) {
        clearMeta(db, ACTIVE_CONVERSATION_KEY);
        return;
    }

    writeMeta(db, ACTIVE_CONVERSATION_KEY, conversationId);
}

QString SessionStore::getActiveConversationId()
{
    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return QString();
    }

    return readMeta(db, ACTIVE_CONVERSATION_KEY);
}

void SessionStore::pruneOldConversations(int maxConversations)
{
    if (maxConversations <= 0) {
        return; // Unlimited
    }

    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return;
    }

    // Delete the oldest conversations beyond the limit. The active
    // conversation is kept: pruning it would leave the restore pointer
    // naming a row that no longer exists, and the app would silently fall
    // back to another conversation on the next start.
    QSqlQuery query(db);
    query.prepare(u"DELETE FROM conversations WHERE id NOT IN (SELECT id FROM conversations ORDER BY updated_at DESC LIMIT ?) "
                  u"AND id IS NOT (SELECT value FROM meta WHERE key = ?)"_s);
    query.addBindValue(maxConversations);
    query.addBindValue(ACTIVE_CONVERSATION_KEY);
    if (!query.exec()) {
        qWarning() << "KateAI: cannot prune old conversations:" << query.lastError().text();
    }
}

void SessionStore::clearAllConversations()
{
    QMutexLocker locker(&storeMutex());

    bool ok = false;
    QSqlDatabase db = openStore(&ok);
    if (!ok) {
        return;
    }

    if (!db.transaction()) {
        qWarning() << "KateAI: cannot clear the conversation history:" << db.lastError().text();
        return;
    }

    QSqlQuery query(db);
    if (!query.exec(u"DELETE FROM conversations"_s)) {
        db.rollback();
        qWarning() << "KateAI: cannot clear the conversation history:" << query.lastError().text();
        return;
    }

    clearMeta(db, ACTIVE_CONVERSATION_KEY);
    if (!db.commit()) {
        qWarning() << "KateAI: cannot commit the conversation history clear:" << db.lastError().text();
    }
}

} // namespace KateAi
