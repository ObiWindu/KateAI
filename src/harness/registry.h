/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "../types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace KateAi
{

/**
 * Declarative metadata for one tool.
 *
 * `schema` is the exact OpenAI-shaped function object the transport already
 * advertises. Keeping it byte-identical avoids a model regression while the
 * metadata makes deferral, search, budgets, and the inspector possible.
 */
struct ToolSpec {
    QString name;
    QString title;
    QString description;
    QString group;
    ToolRisk risk = ToolRisk::Read;
    bool concurrencySafe = true;
    bool idempotent = true;
    bool requiresPriorRead = false;
    bool alwaysLoaded = false;
    bool deferByDefault = false;
    int maxResultChars = 20000;
    QJsonObject schema;
    QStringList aliases;
    bool mcp = false;
};

struct AdvertisedTools {
    QJsonArray tools;
    QStringList deferred;
    bool deferralActive = false;
    int schemaTokens = 0;
};

class ToolRegistry
{
public:
    ToolRegistry();

    // Adds the built-in specs. Called by the constructor; public so tests can
    // build an empty registry when needed.
    void loadBuiltins();
    void addSpec(const ToolSpec &spec);
    void removeSpec(const QString &name);
    void clear();

    // MCP tools are wrapped from their existing definitions. They are deferred
    // by default because a large server catalog is the case search exists for.
    void addMcpDefinition(const QJsonObject &definition);

    QList<ToolSpec> specs() const
    {
        return m_specs;
    }
    ToolSpec spec(const QString &name) const;
    bool contains(const QString &name) const;

    // The JSON for a model request. Deferred specs are omitted unless
    // `includeDeferred` or the spec was discovered through tool_search.
    QJsonArray definitions(const ToolAccess &access, bool includeDeferred = false) const;
    int schemaTokens(const QJsonArray &definitions) const;

    AdvertisedTools advertised(const ToolAccess &access, int contextWindow, bool searchEnabled,
                               int toolCountThreshold = 15) const;

    // Marks a deferred tool as discovered, so its schema is advertised on the
    // next request as well.
    void markDiscovered(const QString &name);
    bool isDiscovered(const QString &name) const
    {
        return m_discovered.contains(name);
    }
    QSet<QString> discovered() const
    {
        return m_discovered;
    }
    void clearDiscovered()
    {
        m_discovered.clear();
    }

    // BM25 over name, title, description, and aliases. Returns at most `limit`
    // names, highest score first, restricted to tools the access allows.
    QStringList search(const QString &query, const ToolAccess &access, int limit = 8) const;

    int resultBudgetFor(const QString &name) const;

    // The bridge tools that make deferral usable.
    static QJsonObject searchDefinition();
    static QJsonObject describeDefinition();
    static QJsonObject callDefinition();

    static QString builtinTitle(const QString &name);
    static QString builtinGroup(const QString &name);
    static ToolRisk builtinRisk(const QString &name);

private:
    QList<ToolSpec> m_specs;
    QSet<QString> m_discovered;
};

// Splits tool text into lowercase search terms. Exposed for tests.
QStringList tokenizeToolText(const QString &text);

} // namespace KateAi
