/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "websearch.h"

#include "types.h"

#include <KLocalizedString>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

// Defined below, next to the other text helpers.
QString decodeHtmlEntities(const QString &input);

namespace
{
// The page is read into memory in one piece, so the download needs a ceiling.
// Five megabytes is far more than a readable web page and far less than a
// denial of service.
constexpr qint64 kMaxFetchBytes = 5 * 1024 * 1024;
}

namespace
{

// A plain browser User-Agent: several providers return a captcha page to an
// obviously automated client.
const char *kUserAgent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";

QNetworkRequest makeRequest(const QUrl &url, int transferTimeoutMs)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", QByteArray(kUserAgent));
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Honour the configured budget rather than a hardcoded constant, so raising
    // the timeout in Settings actually affects the transfer.
    request.setTransferTimeout(transferTimeoutMs);
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
    // Stop the watchdogs so none of them can fire against a reply that the
    // destructor is about to tear down.
    const auto timers = m_timers;
    m_timers.clear();
    for (const QPointer<QTimer> &timer : timers) {
        if (timer) {
            timer->stop();
        }
    }
}

void WebSearch::stopTimerFor(QNetworkReply *reply)
{
    const auto it = m_timers.constFind(reply);
    if (it == m_timers.constEnd()) {
        return;
    }
    const QPointer<QTimer> timer = it.value();
    m_timers.erase(it);
    if (timer) {
        timer->stop();
        timer->deleteLater();
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
    if (parsed.host().isEmpty()) {
        return false;
    }
    // The fetch tool is approved without a prompt, so it must not be able to
    // reach the host's own network: the loopback interface, the private
    // networks and the cloud metadata endpoint are all off limits.
    if (isBlockedHost(parsed.host())) {
        return false;
    }
    return true;
}

bool WebSearch::isBlockedHost(const QString &host)
{
    const QString name = host.trimmed().toLower();
    if (name.isEmpty()) {
        return true;
    }
    // A literal address is checked directly. A name is resolved by the network
    // stack, which is the only way to know what it points at; the resolution
    // itself is checked by the network layer's own policy below.
    const QHostAddress address(name);
    if (!address.isNull()) {
        // Loopback, the "any" address, the private ranges and the link-local
        // range are all off limits. The link-local range holds the cloud
        // metadata endpoint.
        if (address.isLoopback() || address.isLinkLocal() || address.isPrivateUse()) {
            return true;
        }
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 ip = address.toIPv4Address();
            if (ip == 0 || ip == 0xffffffffu) {
                return true;
            }
            // 169.254.0.0/16 is the link-local range, which contains the
            // cloud metadata endpoint (169.254.169.254).
            if ((ip & 0xffff0000u) == 0xa9fe0000u) {
                return true;
            }
        }
        return false;
    }
    // Named hosts that always point back into the machine or the metadata
    // service.
    static const QStringList blockedNames = {
        u"localhost"_s,
        u"localhost.localdomain"_s,
        u"ip6-localhost"_s,
        u"ip6-loopback"_s,
        u"metadata"_s,
        u"metadata.google.internal"_s,
        u"metadata.goog"_s,
    };
    if (blockedNames.contains(name)) {
        return true;
    }
    // A host that ends in ".localhost" resolves to the loopback interface.
    if (name.endsWith(u".localhost"_s)) {
        return true;
    }
    return false;
}

void WebSearch::abort()
{
    // Drop the watchdogs first: a timer that survives its reply would later
    // dereference freed memory.
    const auto timers = m_timers;
    m_timers.clear();
    for (const QPointer<QTimer> &timer : timers) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }
    const auto replies = m_replies;
    m_replies.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            // Detach before aborting: abort() emits finished() synchronously,
            // and an "aborted" reply must not be reported to the agent loop as
            // a finished search that produces a tool result after a stop.
            reply->disconnect(this);
            reply->abort();
        }
    }
    m_callMaxResults.clear();
}

void WebSearch::search(const QString &query, const QString &callId, int maxResults)
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

    // Register the per-call limit only once the request is actually going out:
    // an early return above (unconfigured provider, empty query) never reaches
    // onReplyFinished(), which is the only place the entry is removed.
    if (maxResults > 0) {
        m_callMaxResults.insert(callId, qBound(1, maxResults, 20));
    }

    // The provider request and the reply parser must agree on the count, so
    // both read this call's limit rather than the shared setting.
    const int limit = maxResults > 0 ? qBound(1, maxResults, 20) : 0;
    const int effectiveLimit = limit > 0 ? limit : m_maxResults;

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
        reply = m_nam->get(makeRequest(url, m_timeoutMs));
        break;
    }
    case Provider::Tavily: {
        QNetworkRequest request = makeRequest(QUrl(QStringLiteral("https://api.tavily.com/search")), m_timeoutMs);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QJsonObject payload{{QStringLiteral("api_key"), m_apiKey},
                                  {QStringLiteral("query"), trimmed},
                                  {QStringLiteral("max_results"), effectiveLimit},
                                  {QStringLiteral("search_depth"), QStringLiteral("basic")}};
        reply = m_nam->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        break;
    }
    case Provider::Brave: {
        QUrl url(QStringLiteral("https://api.search.brave.com/res/v1/web/search"));
        QUrlQuery braveItems;
        braveItems.addQueryItem(QStringLiteral("q"), trimmed);
        braveItems.addQueryItem(QStringLiteral("count"), QString::number(effectiveLimit));
        url.setQuery(braveItems);
        QNetworkRequest request = makeRequest(url, m_timeoutMs);
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
        reply = m_nam->get(makeRequest(url, m_timeoutMs));
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
    m_timers.insert(reply, timer);
    connect(timer, &QTimer::timeout, this, [this, reply, callId, trimmed] {
        if (!reply || !reply->isRunning()) {
            return;
        }
        stopTimerFor(reply);
        // Detach before aborting: abort() emits finished() synchronously, which
        // would run onReplyFinished() and report this call a second time. The
        // agent loop matches replies by callId, so a duplicate would pass its
        // guard and double-count the completion.
        reply->disconnect(this);
        m_replies.removeAll(reply);
        reply->abort();
        reply->deleteLater();
        // The call is over, so its per-call limit is released here as well.
        // Only onReplyFinished() used to clear it, and this path never
        // reaches it.
        m_callMaxResults.remove(callId);
        finishSearch(callId, {}, i18n("The search for '%1' timed out after %2 seconds.", trimmed, m_timeoutMs / 1000));
    });
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, callId, trimmed] {
        onReplyFinished(reply, callId, false, trimmed);
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

    QNetworkReply *reply = m_nam->get(makeRequest(QUrl(url.trimmed()), m_timeoutMs));
    m_replies.append(reply);

    // Stream the fetch as it lands. Only the byte count is read, never the
    // body itself: the reply is still consumed in one piece when it finishes,
    // and draining it here would empty the buffer the extraction depends on.
    auto lastEmit = QSharedPointer<QElapsedTimer>::create();
    lastEmit->start();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, callId, lastEmit] {
        if (!reply) {
            return;
        }
        // The body is read in one piece when the reply finishes, so nothing
        // bounds the memory a page (or a URL the model chose) can take. The
        // download is stopped as soon as the cap is reached.
        if (reply->bytesAvailable() + reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > kMaxFetchBytes) {
            stopTimerFor(reply);
            reply->disconnect(this);
            m_replies.removeAll(reply);
            reply->abort();
            reply->deleteLater();
            finishFetch(callId, QString(), QString(),
                        i18n("The page is larger than %1 KB and was not downloaded.").arg(kMaxFetchBytes / 1024));
            return;
        }
        if (lastEmit->elapsed() < 200) {
            return;
        }
        lastEmit->restart();
        const qint64 available = reply->bytesAvailable();
        if (available <= 0) {
            return;
        }
        Q_EMIT requestProgress(callId,
                               i18n("Downloading %1 — %2 KB",
                                    reply->request().url().host(),
                                    QString::number(available / 1024)));
    });

    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(m_timeoutMs);
    m_timers.insert(reply, timer);
    connect(timer, &QTimer::timeout, this, [this, reply, callId] {
        if (!reply || !reply->isRunning()) {
            return;
        }
        stopTimerFor(reply);
        // Detach first, so the abort below cannot reach onReplyFinished() and
        // report this fetch twice.
        reply->disconnect(this);
        m_replies.removeAll(reply);
        reply->abort();
        reply->deleteLater();
        finishFetch(callId, QString(), QString(), i18n("The page did not load within %1 seconds.", m_timeoutMs / 1000));
    });
    timer->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply, callId] {
        onReplyFinished(reply, callId, true, QString());
    });
}

void WebSearch::onReplyFinished(QNetworkReply *reply, const QString &callId, bool isFetch, const QString &query)
{
    // Stop the watchdog before the reply goes away: its lambda holds a pointer
    // to this reply and would dereference freed memory when it fired.
    stopTimerFor(reply);
    m_replies.removeAll(reply);
    // Drop this call's limit whatever happens next, so a failed search cannot
    // leave it behind. Keep the value: it belongs to this reply, which is about
    // to be consumed below.
    const int pendingLimit = m_callMaxResults.take(callId);
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
        // The private-address check ran on the URL the model named, but a
        // redirect can send the request anywhere. The reply's url() is the
        // final target once the redirects have been followed, so a fetch that
        // landed on a blocked host is refused here: the request may have
        // happened, but its content never reaches the conversation.
        if (isBlockedHost(reply->url().host())) {
            finishFetch(callId, QString(), QString(),
                        i18n("The page redirected to a blocked host (%1) and was not read.", reply->url().host()));
            return;
        }
        // A server that answers 200 to an error page is still an error page;
        // the body check below catches the usual "soft 404".
        if (status >= 400) {
            finishFetch(callId, QString(), QString(), i18n("The server returned HTTP %1.").arg(status));
            return;
        }
        // The cap is enforced while the page downloads; this is the case of a
        // response without a content length, whose size is only known here.
        if (body.size() > kMaxFetchBytes) {
            finishFetch(callId, QString(), QString(),
                        i18n("The page is larger than %1 KB and was not read.").arg(kMaxFetchBytes / 1024));
            return;
        }
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        if (!contentType.isEmpty()
            && !contentType.contains(u"text/html"_s) && !contentType.contains(u"text/plain"_s)
            && !contentType.contains(u"application/xhtml+xml"_s) && !contentType.contains(u"text/xml"_s)
            && !contentType.contains(u"application/xml"_s) && !contentType.contains(u"application/json"_s)) {
            finishFetch(callId, QString(), QString(),
                        i18n("The server returned '%1', which is not a text document.").arg(contentType));
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
    // The reply can arrive after a sibling call changed the shared setting, so
    // the limit is the one this call registered for itself.
    const int maxResults = pendingLimit > 0 ? pendingLimit : m_maxResults;
    switch (m_provider) {
    case Provider::Tavily:
        results = parseTavily(QJsonDocument::fromJson(body).object(), maxResults);
        break;
    case Provider::Brave:
        results = parseBrave(QJsonDocument::fromJson(body).object(), maxResults);
        break;
    case Provider::SearXNG:
        results = parseSearx(QJsonDocument::fromJson(body).object(), maxResults);
        break;
    case Provider::DuckDuckGo:
    case Provider::Disabled:
        results = parseDuckDuckGo(body, maxResults);
        break;
    }

    if (results.isEmpty()) {
        finishSearch(callId, {}, i18n("The search returned no results for '%1'.").arg(query));
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
            // The redirect carries the target URL percent-encoded, followed by
            // the parameters of the redirect itself. The split has to happen
            // before the decoding: a target with a query string of its own
            // ("…%3Fx%3D1%26y%3D2") would be cut at the decoded ampersand and
            // lose everything after it.
            QString encoded = href.mid(marker + 5);
            const int amp = encoded.indexOf(QLatin1Char('&'));
            if (amp > 0) {
                encoded.truncate(amp);
            }
            href = QUrl::fromPercentEncoding(encoded.toUtf8());
        }
        if (href.isEmpty() || !isFetchableUrl(href)) {
            continue;
        }
        WebSearchResult result;
        result.url = href;
        QString title = match.captured(2);
        title.replace(tagPattern, QString());
        result.title = collapseWhitespace(decodeHtmlEntities(title));
        if (snippetIt.hasNext()) {
            QString raw = snippetIt.next().captured(1);
            raw.replace(tagPattern, QString());
            result.snippet = collapseWhitespace(decodeHtmlEntities(raw));
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

// Decodes the HTML entities that appear in prose: the named ones are
// replaced, and the numeric ones are turned into the character they stand
// for. A code point outside the basic plane becomes a surrogate pair, not two
// lone surrogates.
QString decodeHtmlEntities(const QString &input)
{
    static const QHash<QString, QString> kEntities = {
        {QStringLiteral("&nbsp;"), QStringLiteral(" ")},  {QStringLiteral("&amp;"), QStringLiteral("&")},
        {QStringLiteral("&lt;"), QStringLiteral("<")},    {QStringLiteral("&gt;"), QStringLiteral(">")},
        {QStringLiteral("&quot;"), QStringLiteral("\"")},{QStringLiteral("&#39;"), QStringLiteral("'")},
        {QStringLiteral("&apos;"), QStringLiteral("'")}, {QStringLiteral("&mdash;"), QStringLiteral("\u2014")},
        {QStringLiteral("&ndash;"), QStringLiteral("\u2013")},{QStringLiteral("&hellip;"), QStringLiteral("\u2026")},
    };
    QString text = input;
    for (auto it = kEntities.constBegin(); it != kEntities.constEnd(); ++it) {
        text.replace(it.key(), it.value());
    }

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
    return decoded;
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
    text = decodeHtmlEntities(text);

    text = collapseWhitespace(text);
    if (maxChars > 0 && text.size() > maxChars) {
        text = truncateSafe(text, maxChars);
        text += QStringLiteral("\n\n… (truncated)");
    }
    return text;
}

} // namespace KateAi