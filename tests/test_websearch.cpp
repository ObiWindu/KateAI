#include "websearch.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

// The network paths are exercised separately by hand; these tests pin the
// parsing, which is where a provider format change would silently corrupt
// results.
class TestWebSearch : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void duckDuckGoHtmlIsParsed()
    {
        // DuckDuckGo wraps every hit in a redirect whose target is percent
        // encoded, so the parser has to unwrap it or the model receives
        // //duckduckgo.com/l/?uddg=... links.
        const QByteArray html =
            "<html><body>"
            "<a class=\"result__a\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Fqnetworkaccessmanager.html&amp;rut=abc\">QNetworkAccessManager <b>Class</b></a>"
            "<a class=\"result__snippet\">Member Function Documentation for the class.</a>"
            "<a class=\"result__a\" href=\"https://example.org/plain\">Plain link</a>"
            "<a class=\"result__snippet\">Already a direct URL.</a>"
            "<a class=\"result__a\" href=\"https://excluded.test\">Third result</a>"
            "</body></html>";

        const auto results = WebSearch::parseDuckDuckGo(html, 5);
        QCOMPARE(results.size(), 3);
        QCOMPARE(results.at(0).url, QStringLiteral("https://doc.qt.io/qt-6/qnetworkaccessmanager.html"));
        QCOMPARE(results.at(0).title, QStringLiteral("QNetworkAccessManager Class"));
        QCOMPARE(results.at(0).snippet, QStringLiteral("Member Function Documentation for the class."));
        // A direct href must survive untouched.
        QCOMPARE(results.at(1).url, QStringLiteral("https://example.org/plain"));
    }

    void duckDuckGoRespectsMaxResults()
    {
        const QByteArray html =
            "<a class=\"result__a\" href=\"https://a.test\">A</a>"
            "<a class=\"result__a\" href=\"https://b.test\">B</a>"
            "<a class=\"result__a\" href=\"https://c.test\">C</a>";
        QCOMPARE(WebSearch::parseDuckDuckGo(html, 2).size(), 2);
    }

    void tavilyJsonIsParsed()
    {
        const QByteArray json =
            "{\"results\":["
            "{\"title\":\"First\",\"url\":\"https://one.test\",\"content\":\"  snippet one  \"},"
            "{\"title\":\"Second\",\"url\":\"https://two.test\",\"content\":\"snippet two\"},"
            "{\"title\":\"No url\",\"content\":\"dropped\"}"
            "]}";
        const auto results = WebSearch::parseTavily(QJsonDocument::fromJson(json).object(), 5);
        // An entry with no URL cannot be cited, so it is dropped.
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(0).title, QStringLiteral("First"));
        QCOMPARE(results.at(0).snippet, QStringLiteral("snippet one"));
        QCOMPARE(results.at(1).url, QStringLiteral("https://two.test"));
    }

    void braveJsonIsParsed()
    {
        const QByteArray json = "{\"web\":{\"results\":["
                                 "{\"title\":\"Brave one\",\"url\":\"https://b1.test\",\"description\":\"desc one\"}"
                                 "]}}";
        const auto results = WebSearch::parseBrave(QJsonDocument::fromJson(json).object(), 5);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results.at(0).title, QStringLiteral("Brave one"));
        QCOMPARE(results.at(0).snippet, QStringLiteral("desc one"));
    }

    void searxJsonIsParsed()
    {
        const QByteArray json = "{\"results\":["
                                 "{\"title\":\"Searx\",\"url\":\"https://s.test\",\"content\":\"searx snippet\"}"
                                 "]}";
        const auto results = WebSearch::parseSearx(QJsonDocument::fromJson(json).object(), 5);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results.at(0).url, QStringLiteral("https://s.test"));
        QCOMPARE(results.at(0).snippet, QStringLiteral("searx snippet"));
    }

    void markdownRendersAnIndexAndCitation()
    {
        WebSearchResult result;
        result.title = QStringLiteral("Title");
        result.url = QStringLiteral("https://x.test");
        result.snippet = QStringLiteral("Snippet");
        const QString md = result.toMarkdown(3);
        QVERIFY(md.startsWith(QStringLiteral("3. [Title](https://x.test)")));
        QVERIFY(md.contains(QStringLiteral("Snippet")));
    }

    void readableTextDropsMarkupAndDecodesEntities()
    {
        const QByteArray html =
            "<html><head><title>Page Title</title>"
            "<style>.x{color:red}</style></head><body>"
            "<script>var a = 1;</script>"
            "<p>First &amp; second</p>"
            "<p>Numeric &#116;est &mdash; done</p>"
            "<nav>Skip me</nav>"
            "</body></html>";

        const QString text = WebSearch::extractReadableText(html, 0);
        // Every dropped tag must actually go: a static local in the drop loop
        // once left only <script> being stripped.
        QVERIFY(!text.contains(QStringLiteral("Skip me")));
        QVERIFY(!text.contains(QStringLiteral("var a = 1")));
        QVERIFY(!text.contains(QStringLiteral("color:red")));
        QVERIFY(!text.contains(QStringLiteral("Page Title")));
        QVERIFY(text.contains(QStringLiteral("First & second")));
        // Numeric and hex entities must not reach the model as raw &#…;
        QVERIFY(!text.contains(QStringLiteral("&#")));
        QVERIFY(text.contains(QStringLiteral("First")));
        QVERIFY(text.contains(QStringLiteral("—")));
        QCOMPARE(WebSearch::htmlTitle(html), QStringLiteral("Page Title"));
    }

    void readableTextTruncatesWithAMarker()
    {
        const QByteArray html = "<p>" + QByteArray(500, 'x') + "</p>";
        const QString text = WebSearch::extractReadableText(html, 100);
        QVERIFY(text.length() < 200);
        QVERIFY(text.contains(QStringLiteral("truncated")));
    }

    void onlyHttpSchemesAreFetchable()
    {
        // Guards against file:// and friends reaching the network stack.
        QVERIFY(WebSearch::isFetchableUrl(QStringLiteral("https://example.com")));
        QVERIFY(WebSearch::isFetchableUrl(QStringLiteral("http://example.com/page")));
        QVERIFY(!WebSearch::isFetchableUrl(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!WebSearch::isFetchableUrl(QStringLiteral("ftp://example.com")));
        QVERIFY(!WebSearch::isFetchableUrl(QStringLiteral("example.com")));
        QVERIFY(!WebSearch::isFetchableUrl(QString()));
    }

    void providerIdRoundTrips()
    {
        for (const auto provider : {WebSearch::Provider::DuckDuckGo,
                                     WebSearch::Provider::Tavily,
                                     WebSearch::Provider::Brave,
                                     WebSearch::Provider::SearXNG,
                                     WebSearch::Provider::Disabled}) {
            QCOMPARE(WebSearch::providerFromId(WebSearch::providerId(provider)), provider);
        }
        QCOMPARE(WebSearch::providerFromId(QStringLiteral("nonsense")), WebSearch::Provider::Disabled);
    }

    void configurationDependsOnProvider()
    {
        WebSearch ws;
        // DuckDuckGo must work with no setup at all, or the feature is dead
        // on arrival for anyone who has not signed up for an API key.
        ws.setProvider(WebSearch::Provider::DuckDuckGo);
        QVERIFY(ws.isConfigured());
        QVERIFY(ws.configurationError().isEmpty());

        ws.setProvider(WebSearch::Provider::Tavily);
        QVERIFY(!ws.isConfigured());
        QVERIFY(!ws.configurationError().isEmpty());
        ws.setApiKey(QStringLiteral("key"));
        QVERIFY(ws.isConfigured());

        ws.setProvider(WebSearch::Provider::SearXNG);
        ws.setApiKey(QString());
        QVERIFY(!ws.isConfigured());
        ws.setEndpoint(QStringLiteral("http://localhost:8888"));
        QVERIFY(ws.isConfigured());

        ws.setProvider(WebSearch::Provider::Disabled);
        QVERIFY(!ws.isConfigured());
    }
};

QTEST_GUILESS_MAIN(TestWebSearch)
#include "test_websearch.moc"