/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace KateAi
{

// One hit from a search provider.
struct WebSearchResult {
    QString title;
    QString url;
    QString snippet;
    // Rendered for the model: a numbered list it can quote and cite.
    QString toMarkdown(int index) const;
};

/**
 * Web search and page fetching for the agent.
 *
 * Four providers are supported because there is no single right answer:
 *
 *  - DuckDuckGo  no API key, works out of the box, unofficial endpoint
 *  - Tavily      built for LLM agents, returns cleaned snippets
 *  - Brave       independent index, conventional search API
 *  - SearXNG     self-hosted, no key, useful on an isolated network
 *
 * Everything is asynchronous: a search routinely takes several seconds, and
 * blocking the agent loop on it would stall the transcript. The parsing
 * routines are static and pure so they can be tested without a network.
 */
class WebSearch : public QObject
{
    Q_OBJECT

public:
    enum class Provider {
        Disabled,
        DuckDuckGo,
        Tavily,
        Brave,
        SearXNG,
    };

    explicit WebSearch(QObject *parent = nullptr);
    ~WebSearch() override;

    void setProvider(Provider provider);
    Provider provider() const
    {
        return m_provider;
    }
    void setApiKey(const QString &key)
    {
        m_apiKey = key;
    }
    // Base URL for SearXNG, e.g. http://localhost:8888.
    void setEndpoint(const QString &endpoint)
    {
        m_endpoint = endpoint.trimmed();
    }
    void setMaxResults(int count)
    {
        m_maxResults = qBound(1, count, 20);
    }
    void setTimeoutMs(int ms)
    {
        m_timeoutMs = qMax(1000, ms);
    }

    // False when the provider needs a key or endpoint that is not configured,
    // so the agent can be told why the tool is unavailable instead of failing.
    bool isConfigured() const;
    // Human-readable reason when isConfigured() is false.
    QString configurationError() const;
    // A short description of the active provider for the system prompt.
    QString providerDescription() const;

    void search(const QString &query, const QString &callId);
    void fetchPage(const QString &url, const QString &callId);
    void abort();

    static QString providerId(Provider provider);
    static Provider providerFromId(const QString &id);

    // --- Pure parsing helpers, used by the network paths and by tests. ------
    static QList<WebSearchResult> parseDuckDuckGo(const QByteArray &html, int maxResults);
    static QList<WebSearchResult> parseTavily(const QJsonObject &root, int maxResults);
    static QList<WebSearchResult> parseBrave(const QJsonObject &root, int maxResults);
    static QList<WebSearchResult> parseSearx(const QJsonObject &root, int maxResults);
    // Strips markup and collapses whitespace so the model gets prose, not HTML.
    static QString extractReadableText(const QByteArray &html, int maxChars);
    static QString htmlTitle(const QByteArray &html);
    // Rejects anything that is not plain http(s); guards against file:// and
    // other schemes reaching the network stack.
    static bool isFetchableUrl(const QString &url);

Q_SIGNALS:
    void searchFinished(const QString &callId, const QList<WebSearchResult> &results, const QString &error);
    void fetchFinished(const QString &callId, const QString &title, const QString &text, const QString &error);

private:
    void finishSearch(const QString &callId, const QList<WebSearchResult> &results, const QString &error);
    void finishFetch(const QString &callId, const QString &title, const QString &text, const QString &error);
    void onReplyFinished(QNetworkReply *reply, const QString &callId, bool isFetch, const QString &query);
    // Stops and removes the watchdog timer belonging to `reply`. Must run
    // before the reply is deleted: the timer lambda holds a pointer to it.
    void stopTimerFor(QNetworkReply *reply);

    Provider m_provider = Provider::DuckDuckGo;
    QString m_apiKey;
    QString m_endpoint;
    int m_maxResults = 5;
    int m_timeoutMs = 20000;
    QNetworkAccessManager *m_nam = nullptr;
    // In-flight replies, so abort() can cancel them and the destructor does
    // not leave sockets running against a destroyed object.
    QList<QPointer<QNetworkReply>> m_replies;
    // Watchdog timers keyed by the reply they guard. Keyed rather than listed
    // so a completed request can drop its timer, which is what stops a timer
    // from dereferencing a reply that has already been deleted.
    QHash<QNetworkReply *, QPointer<QTimer>> m_timers;
};

} // namespace KateAi