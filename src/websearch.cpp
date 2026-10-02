/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "websearch.h"

#include <KLocalizedString>

#include <KLocalizedString>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// A plain browser User-Agent: several providers return a captcha page to an
// obviously automated client.
const char *kUserAgent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", QByteArray(kUserAgent));
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    return request;
}

QString collapseWhitespace(const QString &text)
{
    QString out = text;
    out.replace(QRegularExpression(u"[ \\t\\f\\v]+"_s), u" "_s);
    out.replace(QRegularExpression(u"\\n{3,}"_s), u"\n\n"_s);
    return out.trimmed();
}

} // namespace

QString WebSearchResult::toMarkdown(int index) const
{
    QString out = QStringLiteral("%1. [%2](%3)").arg(index).arg(title.isEmpty() ? url : title, url);
    if (!snippet.isEmpty()) {
        out += u"\n   "_s + snippet;
    }
    return out;
}

WebSearch::WebSearch(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

WebSearch::~WebSearch()
{
    // Abort without letting the completion handlers run. reply->abort() emits
    // finished() synchronously, which would call back into the owning
    // AgentLoop while it is already being destroyed.
    const auto replies = m_replies;
    m_replies.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            reply->disconnect(this);
            reply->abort();
        }
    }
    const auto timers = m_timers;
    m_timers.clear();
    for (const QPointer<QTimer> &timer : timers) {
        if (timer) {
            timer->stop();
        }
    }
}

QString WebSearch::providerId(Provider provider)
{
    switch (provider) {
    case Provider::DuckDuckGo:
        return QStringLiteral("duckduckgo");
    case Provider::Tavily:
        return QStringLiteral("tavily");
    case Provider::Brave:
        return QStringLiteral("brave");
    case Provider::SearXNG:
        return QStringLiteral("searxng");
    case Provider::Disabled:
        break;
    }
    return QStringLiteral("disabled");
}

WebSearch::Provider WebSearch::providerFromId(const QString &id)
{
    const QString key = id.trimmed().toLower();
    if (key == u"duckduckgo"_s) {
        return Provider::DuckDuckGo;
    }
    if (key == u"tavily"_s) {
        return Provider::Tavily;
    }
    if (key == u"brave"_s) {
        return Provider::Brave;
    }
    if (key == u"searxng"_s) {
        return Provider::SearXNG;
    }
    return Provider::Disabled;
}

void WebSearch::setProvider(Provider provider)
{
    m_provider = provider;
}

bool WebSearch::isConfigured() const
{
    switch (m_provider) {
    case Provider::DuckDuckGo:
        return true;
    case Provider::Tavily:
    case Provider::Brave:
        return !m_apiKey.isEmpty();
    case Provider::SearXNG:
        return !m_endpoint.isEmpty();
    case Provider::Disabled:
        break;
    }
    return false;
}

QString WebSearch::configurationError() const
{
    if (isConfigured()) {
        return QString();
    }
    switch (m_provider) {
    case Provider::Tavily:
        return i18n("Web search is set to Tavily but no API key is configured. Add one in Settings, or switch the provider.");
    case Provider::Brave:
        return i18n("Web search is set to Brave but no API key is configured. Add one in Settings, or switch the provider.");
    case Provider::SearXNG:
        return i18n("Web search is set to SearXNG but no server URL is configured. Add one in Settings, or switch the provider.");
    case Provider::DuckDuckGo:
        break;
    case Provider::Disabled:
        break;
    }
    return i18n("Web search is disabled in Settings.");
}

QString WebSearch::providerDescription() const
{
    switch (m_provider) {
    case Provider::DuckDuckGo:
        return i18n("DuckDuckGo (no key required)");
    case Provider::Tavily:
        return i18n("Tavily");
    case Provider::Brave:
        return i18n("Brave Search");
    case Provider::SearXNG:
        return i18n("SearXNG at %1", m_endpoint);
    case Provider::Disabled:
        break;
    }
    return i18n("disabled");
}

bool WebSearch::isFetchableUrl(const QString &url)
{
    const QUrl parsed(url.trimmed());
    if (!parsed.isValid()) {
        return false;
    }
    const QString scheme = parsed.scheme().toLower();
    if (scheme != u"http"_s && scheme != u"https"_s) {
        return false;
    }
    return !parsed.host().isEmpty();
}

void WebSearch::abort()
{
    const auto replies = m_replies;
    m_replies.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            reply->abort();
        }
    }
    const auto timers = m_timers;
    m_timers.clear();
    for (const QPointer<QTimer> &timer : timers) {
        if (timer) {
            timer->stop();
        }
    }
}

void WebSearch::search(const QString &query, const QString &callId)
{
    if (!isConfigured()) {
        finishSearch(callId, {}, configurationError());
        return;
    }
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        finishSearch(callId, {}, i18n("The search query was empty."));
        return;
    }

    QNetworkReply *reply = nullptr;
    switch (m_provider) {
    case Provider::DuckDuckGo: {
        // The HTML endpoint is the only keyless one that does not require a
        // JS runtime to interpret the response.
        QUrl url(QStringLiteral("https://html.duckduckgo.com/html/"));
        QUrlQuery queryItems;
        queryItems.addQueryItem(QStringLiteral("q"), trimmed);
        queryItems.addQueryItem(QStringLiteral("kl"), QStringLiteral("wt-wt"));
        url.setQuery(queryItems);
        reply = m_nam->get(makeRequest(url));
        break;
    }
    case Provider::Tavily: {
        QNetworkRequest request = makeRequest(QUrl(QStringLiteral("https://api.tavily.com/search")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QJsonObject payload{{QStringLiteral("api_key"), m_apiKey},
                                  {QStringLiteral("query"), trimmed},
                                  {QStringLiteral("max_results"), m_maxResults},
                                  {QStringLiteral("search_depth"), QStringLiteral("basic")}};
        reply = m_nam->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        break;
    }
    case Provider::Brave: {
        QUrl url(QStringLiteral("https://api.search.brave.com/res/v1/web/search"));
        QUrlQuery braveItems;
        braveItems.addQueryItem(QStringLiteral("q"), trimmed);
        braveItems.addQueryItem(QStringLiteral("count"), QString::number(m_maxResults));
        url.setQuery(braveItems);
        QNetworkRequest request = makeRequest(url);
        request.setRawHeader("X-Subscription-Token", m_apiKey.toUtf8());
        request.setRawHeader("Accept", "application/json");
        reply = m_nam->get(request);
        break;
    }
    case Provider::SearXNG: {
        QUrl url(m_endpoint);
        QUrlQuery searxItems;
        searxItems.addQueryItem(QStringLiteral("q"), trimmed);
        searxItems.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
        url.setQuery(searxItems);
        reply = m_nam->get(makeRequest(url));
        break;
    }
    case Provider::Disabled:
        finishSearch(callId, {}, configurationError());
        return;
    }

    m_replies.append(reply);
    // A hard ceiling: the agent has already moved on by the time a slow
    // provider answers, and a hung socket would outlive the turn.
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);
    connect(timer, &QTimer::timeout, this, [this, reply, callId] {
        if (reply->isRunning()) {
            reply->abort();
            finishSearch(callId, {}, i18n("The search request timed out after %1 seconds.", m_timeoutMs / 1000));
        }
    });
    m_timers.append(timer);
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, callId] {
        onReplyFinished(reply, callId, false);
    });
}

void WebSearch::fetchPage(const QString &url, const QString &callId)
{
    if (!isConfigured()) {
        // Fetching does not strictly need a search provider, but tying the
        // two together keeps one switch in Settings instead of two.
        finishFetch(callId, QString(), QString(), configurationError());
        return;
    }
    if (!isFetchableUrl(url)) {
        finishFetch(callId, QString(), QString(), i18n("Only http and https URLs can be fetched, not '%1'.").arg(url));
        return;
    }

    QNetworkReply *reply = m_nam->get(makeRequest(QUrl(url.trimmed())));
    m_replies.append(reply);

    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);
    connect(timer, &QTimer::timeout, this, [this, reply, callId] {
        if (reply->isRunning()) {
            reply->abort();
            finishFetch(callId, QString(), QString(), i18n("The page did not load within %1 seconds.", m_timeoutMs / 1000));
        }
    });
    m_timers.append(timer);
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, callId] {
        onReplyFinished(reply, callId, true);
    });
}

void WebSearch::onReplyFinished(QNetworkReply *reply, const QString &callId, bool isFetch)
{
    m_replies.removeAll(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString error = reply->errorString();
        if (isFetch) {
            finishFetch(callId, QString(), QString(), error);
        } else {
            finishSearch(callId, {}, error);
        }
        return;
    }

    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (isFetch) {
        // A server that answers 200 to an error page is still an error page;
        // the body check below catches the usual "soft 404".
        if (status >= 400) {
            finishFetch(callId, QString(), QString(), i18n("The server returned HTTP %1.").arg(status));
            return;
        }
        const QString title = htmlTitle(body);
        const QString text = extractReadableText(body, 12000);
        if (text.isEmpty()) {
            finishFetch(callId, title, QString(), i18n("The page loaded but contained no readable text (it may be JavaScript-rendered)."));
            return;
        }
        finishFetch(callId, title, text, QString());
        return;
    }

    if (status >= 400) {
        finishSearch(callId, {}, i18n("The search provider returned HTTP %1.").arg(status));
        return;
    }

    QList<WebSearchResult> results;
    switch (m_provider) {
    case Provider::Tavily:
        results = parseTavily(QJsonDocument::fromJson(body).object(), m_maxResults);
        break;
    case Provider::Brave:
        results = parseBrave(QJsonDocument::fromJson(body).object(), m_maxResults);
        break;
    case Provider::SearXNG:
        results = parseSearx(QJsonDocument::fromJson(body).object(), m_maxResults);
        break;
    case Provider::DuckDuckGo:
    case Provider::Disabled:
        results = parseDuckDuckGo(body, m_maxResults);
        break;
    }

    if (results.isEmpty()) {
        finishSearch(callId, {}, i18n("The search returned no results for '%1'.").arg(QString()));
        return;
    }
    finishSearch(callId, results, QString());
}

void WebSearch::finishSearch(const QString &callId, const QList<WebSearchResult> &results, const QString &error)
{
    Q_EMIT searchFinished(callId, results, error);
}

void WebSearch::finishFetch(const QString &callId, const QString &title, const QString &text, const QString &error)
{
    Q_EMIT fetchFinished(callId, title, text, error);
}

// --- Parsing -----------------------------------------------------------------

QList<WebSearchResult> WebSearch::parseDuckDuckGo(const QByteArray &html, int maxResults)
{
    QList<WebSearchResult> results;
    // Each hit is an <a class="result__a" href="...">title</a> followed by a
    // <a class="result__snippet">. The href is a redirect through
    // //duckduckgo.com/l/?uddg=<encoded>, which has to be unwrapped.
    static const QRegularExpression anchorPattern(
        QStringLiteral("<a[^>]*class=\"[^\"]*result__a[^\"]*\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression snippetPattern(
        QStringLiteral("<a[^>]*class=\"[^\"]*result__snippet[^\"]*\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tagPattern(QStringLiteral("<[^>]+>"));

    auto it = anchorPattern.globalMatch(QString::fromUtf8(html));
    auto snippetIt = snippetPattern.globalMatch(QString::fromUtf8(html));
    while (it.hasNext() && results.size() < maxResults) {
        const QRegularExpressionMatch match = it.next();
        QString href = match.captured(1);
        // Unwrap the redirect wrapper.
        const int marker = href.indexOf(QLatin1String("uddg="));
        if (marker >= 0) {
            href = QUrl::fromPercentEncoding(href.mid(marker + 5).toUtf8());
            const int amp = href.indexOf(QLatin1Char('&'));
            if (amp > 0) {
                href.truncate(amp);
            }
        }
        if (href.isEmpty() || !isFetchableUrl(href)) {
            continue;
        }
        WebSearchResult result;
        result.url = href;
        QString title = match.captured(2);
        title.replace(tagPattern, QString());
        result.title = collapseWhitespace(title);
        if (snippetIt.hasNext()) {
            QString raw = snippetIt.next().captured(1);
            raw.replace(tagPattern, QString());
            result.snippet = collapseWhitespace(raw);
        }
        results.append(result);
    }
    return results;
}

QList<WebSearchResult> WebSearch::parseTavily(const QJsonObject &root, int maxResults)
{
    QList<WebSearchResult> results;
    const QJsonArray items = root.value(u"results"_s).toArray();
    for (const QJsonValue &value : items) {
        if (results.size() >= maxResults) {
            break;
        }
        const QJsonObject item = value.toObject();
        WebSearchResult result;
        result.title = item.value(u"title"_s).toString();
        result.url = item.value(u"url"_s).toString();
        result.snippet = item.value(u"content"_s).toString().simplified();
        if (!result.url.isEmpty()) {
            results.append(result);
        }
    }
    return results;
}

QList<WebSearchResult> WebSearch::parseBrave(const QJsonObject &root, int maxResults)
{
    QList<WebSearchResult> results;
    const QJsonObject web = root.value(u"web"_s).toObject();
    const QJsonArray items = web.value(u"results"_s).toArray();
    for (const QJsonValue &value : items) {
        if (results.size() >= maxResults) {
            break;
        }
        const QJsonObject item = value.toObject();
        WebSearchResult result;
        result.title = item.value(u"title"_s).toString();
        result.url = item.value(u"url"_s).toString();
        result.snippet = item.value(u"description"_s).toString().simplified();
        if (!result.url.isEmpty()) {
            results.append(result);
        }
    }
    return results;
}

QList<WebSearchResult> WebSearch::parseSearx(const QJsonObject &root, int maxResults)
{
    QList<WebSearchResult> results;
    const QJsonArray items = root.value(u"results"_s).toArray();
    for (const QJsonValue &value : items) {
        if (results.size() >= maxResults) {
            break;
        }
        const QJsonObject item = value.toObject();
        WebSearchResult result;
        result.title = item.value(u"title"_s).toString();
        result.url = item.value(u"url"_s).toString();
        result.snippet = item.value(u"content"_s).toString().simplified();
        if (!result.url.isEmpty()) {
            results.append(result);
        }
    }
    return results;
}

QString WebSearch::htmlTitle(const QByteArray &html)
{
    static const QRegularExpression pattern(QStringLiteral("<title[^>]*>(.*?)</title>"),
                                            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(html));
    if (!match.hasMatch()) {
        return QString();
    }
    return collapseWhitespace(match.captured(1));
}

QString WebSearch::extractReadableText(const QByteArray &html, int maxChars)
{
    QString text = QString::fromUtf8(html);

    // Drop the parts that are never prose.
    static const QStringList kDrop = {QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("noscript"),
                                      QStringLiteral("svg"), QStringLiteral("head"), QStringLiteral("nav"),
                                      QStringLiteral("footer"), QStringLiteral("form"), QStringLiteral("iframe"),
                                      QStringLiteral("template")};
    for (const QString &tag : kDrop) {
        // Deliberately not static: a static local inside a loop body is
        // initialised once, on the first iteration, so every tag would be
        // matched with the pattern built for the first one.
        const QRegularExpression dropPattern(
            QStringLiteral("<%1\\b[^>]*>.*?</%1>").arg(tag),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
        text.replace(dropPattern, QStringLiteral(" "));
    }
    // Turn block boundaries into newlines so paragraphs survive.
    static const QRegularExpression blockPattern(QStringLiteral("</(p|div|section|article|h[1-6]|li|tr|pre)>"),
                                                 QRegularExpression::CaseInsensitiveOption);
    text.replace(blockPattern, QStringLiteral("\n"));
    static const QRegularExpression breakPattern(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    text.replace(breakPattern, QStringLiteral("\n"));

    // Everything else is markup.
    static const QRegularExpression tagPattern(QStringLiteral("<[^>]+>"));
    text.replace(tagPattern, QStringLiteral(" "));

    // Entity decoding for the handful that matter in prose.
    static const QHash<QString, QString> kEntities = {
        {QStringLiteral("&nbsp;"), QStringLiteral(" ")},  {QStringLiteral("&amp;"), QStringLiteral("&")},
        {QStringLiteral("&lt;"), QStringLiteral("<")},    {QStringLiteral("&gt;"), QStringLiteral(">")},
        {QStringLiteral("&quot;"), QStringLiteral("\"")},{QStringLiteral("&#39;"), QStringLiteral("'")},
        {QStringLiteral("&apos;"), QStringLiteral("'")}, {QStringLiteral("&mdash;"), QStringLiteral("—")},
        {QStringLiteral("&ndash;"), QStringLiteral("–")},{QStringLiteral("&hellip;"), QStringLiteral("…")},
    };
    for (auto it = kEntities.constBegin(); it != kEntities.constEnd(); ++it) {
        text.replace(it.key(), it.value());
    }

    // Numeric and hex entities. Documentation sites lean on these heavily
    // (&#116; for "t"), and leaving them raw makes the model read noise.
    static const QRegularExpression numericEntity(QStringLiteral("&#(x?)([0-9a-fA-F]+);"));
    QString decoded;
    decoded.reserve(text.size());
    int copied = 0;
    auto entityIt = numericEntity.globalMatch(text);
    while (entityIt.hasNext()) {
        const QRegularExpressionMatch match = entityIt.next();
        decoded += QString(text.constBegin() + copied, match.capturedStart() - copied);
        bool converted = false;
        const uint code = match.captured(2).toUInt(&converted, match.captured(1).isEmpty() ? 10 : 16);
        if (converted && code > 0 && code <= 0x10FFFF) {
            decoded += QString::fromUcs4(&code, 1);
        } else {
            decoded += match.captured(0);
        }
        copied = match.capturedEnd();
    }
    decoded += QString(text.constBegin() + copied, text.size() - copied);
    text = decoded;

    text = collapseWhitespace(text);
    if (maxChars > 0 && text.size() > maxChars) {
        text.truncate(maxChars);
        text += QStringLiteral("\n\n… (truncated)");
    }
    return text;
}

} // namespace KateAi