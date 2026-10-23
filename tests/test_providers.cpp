#include "types.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestProviders : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void compressTextStaysInsideItsBudget()
    {
        // The indicator used to be added after the cut, so the result was
        // longer than the caller asked for.
        const QString text = QString(500, QLatin1Char('x'));
        const QString compressed = compressText(text, 100, true);
        QVERIFY2(compressed.size() <= 100,
                 qPrintable(QStringLiteral("compressText returned %1 characters").arg(compressed.size())));
        QVERIFY(compressed.contains(u"truncated"_s));
    }

    void truncateSafeDoesNotSplitASurrogatePair()
    {
        // A cut in the middle of an astral character leaves half of it behind,
        // and a lone surrogate is serialised as U+FFFD.
        // An emoji (a surrogate pair in UTF-16) followed by three letters.
        const QString text = QString::fromUtf8("\xF0\x9F\x98\x80" "abc");
        const QString cut = truncateSafe(text, 1);
        QVERIFY2(cut.isEmpty() || !cut.at(cut.size() - 1).isHighSurrogate(),
                 "the cut left a lone high surrogate behind");
        const QString full = truncateSafe(text, 100);
        QCOMPARE(full, text);
    }

    void smartCompressContextStaysInsideItsBudget()
    {
        // The marker used to be added on top of the kept head and tail.
        const QString text = QString(2000, QLatin1Char('x'));
        const QString compressed = smartCompressContext(text, 500, true);
        QVERIFY2(compressed.size() <= 500,
                 qPrintable(QStringLiteral("smartCompressContext returned %1 characters").arg(compressed.size())));
    }

    void smartCompressContextDoesNotSplitASurrogatePair()
    {
        const QString text = QString::fromUtf8("\xF0\x9F\x98\x80") + QString(2000, QLatin1Char('x'));
        const QString compressed = smartCompressContext(text, 500, true);
        for (int i = 0; i < compressed.size(); ++i) {
            const QChar c = compressed.at(i);
            if (c.isHighSurrogate()) {
                QVERIFY2(i + 1 < compressed.size() && compressed.at(i + 1).isLowSurrogate(),
                         "the compression left a lone high surrogate behind");
                ++i;
            } else {
                QVERIFY2(!c.isLowSurrogate(), "the compression left a lone low surrogate behind");
            }
        }
    }

    void aTelegramProviderHasAnId()
    {
        // The enumerator fell through to "grok", so the provider could not be
        // selected at all.
        QCOMPARE(providerId(Provider::Telegram), u"telegram"_s);
        QCOMPARE(providerFromId(u"telegram"_s), Provider::Telegram);
    }

    void anEmptyBaseUrlFallsBackToTheProviderDefault()
    {
        Settings settings;
        settings.provider = Provider::OpenAICompatible;
        settings.openaiCompatibleUrl.clear();
        QVERIFY(!providerBaseUrl(settings).isEmpty());
        settings.provider = Provider::ClaudeCompatible;
        settings.claudeCompatibleUrl.clear();
        QVERIFY(!providerBaseUrl(settings).isEmpty());
    }

    void noProviderShipsHardCodedModelNames()
    {
        // Model names used to be baked into defaultModels() and into the
        // Settings defaults, which went stale on every release and offered
        // models a given key could not reach. Everything must come from the
        // provider's own catalogue now.
        const QList<Provider> all = {Provider::Grok,
                                     Provider::OpenAI,
                                     Provider::OpenRouter,
                                     Provider::DeepSeek,
                                     Provider::OpenAICompatible,
                                     Provider::ClaudeCompatible,
                                     Provider::Kilo,
                                     Provider::Acp,
                                     Provider::OpenCode};
        for (const Provider provider : all) {
            QVERIFY2(defaultModels(provider).isEmpty(),
                     "defaultModels() must not hard-code model names");
        }

        Settings fresh;
        QVERIFY2(fresh.grokModel.isEmpty(), "Settings must not default to a model name");
        QVERIFY2(fresh.openaiModel.isEmpty(), "Settings must not default to a model name");
        QVERIFY2(fresh.openrouterModel.isEmpty(), "Settings must not default to a model name");
        QVERIFY2(fresh.deepseekModel.isEmpty(), "Settings must not default to a model name");
        QVERIFY2(fresh.kiloModel.isEmpty(), "Settings must not default to a model name");
        QVERIFY2(fresh.acpModel.isEmpty(), "Settings must not default to a model name");
        QVERIFY2(fresh.opencodeModel.isEmpty(), "Settings must not default to a model name");
    }

    void openCodeIsAProvider()
    {
        QCOMPARE(providerId(Provider::OpenCode), u"opencode"_s);
        QCOMPARE(providerFromId(u"opencode"_s), Provider::OpenCode);
        QVERIFY(!providerLabel(Provider::OpenCode).isEmpty());

        // Documented endpoint: opencode.ai serves its curated catalogue from
        // /zen/v1, which is also where the model list is fetched from.
        QCOMPARE(providerBaseUrl(Provider::OpenCode), u"https://opencode.ai/zen/v1"_s);

        Settings settings;
        settings.provider = Provider::OpenCode;
        settings.opencodeUrl.clear();
        QCOMPARE(providerBaseUrl(settings), u"https://opencode.ai/zen/v1"_s);
        settings.opencodeUrl = u"http://127.0.0.1:1234/v1"_s;
        QCOMPARE(providerBaseUrl(settings), u"http://127.0.0.1:1234/v1"_s);

        settings.opencodeApiKey = u"test-key"_s;
        QCOMPARE(apiKeyFor(settings), u"test-key"_s);
        settings.opencodeModel = u"gpt-5.5"_s;
        QCOMPARE(modelFor(settings), u"gpt-5.5"_s);

        // It is OpenAI-compatible, so its catalogue can be listed.
        QVERIFY(providerSupportsModelListing(Provider::OpenCode));
    }

    void everyProviderRoundTripsThroughItsId()
    {
        const QList<Provider> all = {Provider::Grok,
                                     Provider::OpenAI,
                                     Provider::OpenRouter,
                                     Provider::DeepSeek,
                                     Provider::OpenAICompatible,
                                     Provider::ClaudeCompatible,
                                     Provider::Kilo,
                                     Provider::Acp,
                                     Provider::OpenCode};
        for (const Provider provider : all) {
            const QString id = providerId(provider);
            QVERIFY(!id.isEmpty());
            QCOMPARE(providerFromId(id), provider);
            QVERIFY(!providerBaseUrl(provider).isEmpty());
            QVERIFY(!providerLabel(provider).isEmpty());
        }
    }

    void eachProviderHasItsOwnKeyAndModelSlot()
    {
        // A provider sharing another provider's slot is a silent bug: you set
        // the key and the model never reaches the wire.
        Settings settings;
        settings.openaiApiKey = u"a"_s;
        settings.openrouterApiKey = u"b"_s;
        settings.opencodeApiKey = u"c"_s;
        settings.openaiModel = u"m1"_s;
        settings.openrouterModel = u"m2"_s;
        settings.opencodeModel = u"m3"_s;

        settings.provider = Provider::OpenAI;
        QCOMPARE(apiKeyFor(settings), u"a"_s);
        QCOMPARE(modelFor(settings), u"m1"_s);
        settings.provider = Provider::OpenRouter;
        QCOMPARE(apiKeyFor(settings), u"b"_s);
        QCOMPARE(modelFor(settings), u"m2"_s);
        settings.provider = Provider::OpenCode;
        QCOMPARE(apiKeyFor(settings), u"c"_s);
        QCOMPARE(modelFor(settings), u"m3"_s);
    }

    void acpHasNoCatalogueEndpoint()
    {
        // ACP agents advertise themselves, so there is nothing to fetch and
        // the agent must not try.
        QVERIFY(!providerSupportsModelListing(Provider::Acp));
        QVERIFY(providerSupportsModelListing(Provider::OpenAI));
        QVERIFY(providerSupportsModelListing(Provider::OpenRouter));
    }

    void pickerSurfacesConfiguredAcpModelWithoutCatalogue()
    {
        Settings settings;
        settings.provider = Provider::Acp;
        settings.acpApiKey = u"acp-key"_s;
        settings.acpModel = u"workspace-agent"_s;

        QCOMPARE(pickerModelsFor(settings, Provider::Acp, {}),
                 QStringList{u"workspace-agent"_s});

        const QStringList catalog{u"other"_s, u"workspace-agent"_s};
        QCOMPARE(pickerModelsFor(settings, Provider::Acp, catalog), catalog);

        const QStringList withoutConfigured{u"alpha"_s, u"beta"_s};
        QCOMPARE(pickerModelsFor(settings, Provider::Acp, withoutConfigured),
                 (QStringList{u"workspace-agent"_s, u"alpha"_s, u"beta"_s}));

        settings.acpModel.clear();
        QVERIFY(pickerModelsFor(settings, Provider::Acp, {}).isEmpty());
    }

    void planParsingTerminatesOnATrailingLine()
    {
        // parsePlanFromText() used to spin forever once the plan section ended:
        // the final line was processed, `i` was never advanced past the end of
        // the text, and the loop re-entered on the same empty segment for as
        // long as fewer than 20 steps had been parsed.
        const QJsonArray noNewline = parsePlanFromText(u"Plan:\n1. Read the file\n2. Fix the bug"_s);
        QCOMPARE(noNewline.size(), 2);
        QCOMPARE(noNewline.at(0).toObject().value(u"description"_s).toString(), u"Read the file"_s);
        QCOMPARE(noNewline.at(1).toObject().value(u"description"_s).toString(), u"Fix the bug"_s);

        const QJsonArray withNewline = parsePlanFromText(u"Plan:\n1. Read the file\n2. Fix the bug\n"_s);
        QCOMPARE(withNewline.size(), 2);

        // A single step is enough to trigger the hang.
        const QJsonArray single = parsePlanFromText(u"Steps:\n- Do it"_s);
        QCOMPARE(single.size(), 1);

        // No plan heading means no plan, and no loop.
        QVERIFY(parsePlanFromText(u"Here is the answer."_s).isEmpty());

        // Steps stay capped so a runaway reply cannot grow the checklist.
        QString many = u"Plan:\n"_s;
        for (int i = 0; i < 40; ++i) {
            many += u"%1. step %2\n"_s.arg(i + 1).arg(i);
        }
        QCOMPARE(parsePlanFromText(many).size(), 20);
    }

    void planMergeKeepsIdsStable()
    {
        const QJsonArray plan = parsePlanFromText(u"Plan:\n1. one\n2. two"_s);
        const QJsonArray extended = mergePlanIntoAssistantMessage(plan, u"Plan:\n1. one\n2. two\n3. three"_s);
        QCOMPARE(extended.size(), 3);
        QCOMPARE(extended.at(0).toObject().value(u"id"_s).toString(), u"step1"_s);
        QCOMPARE(extended.at(2).toObject().value(u"description"_s).toString(), u"three"_s);
    }

    void acpNativeIsStdioTransport()
    {
        Settings settings;
        settings.provider = Provider::Acp;
        QCOMPARE(settings.apiFormat, ApiFormat::AcpNative);
        QVERIFY(usesAcpNative(settings));
        QVERIFY(!providerRequiresApiKey(settings, Provider::Acp));
        QVERIFY(providerIsSelectable(settings, Provider::Acp));
        QCOMPARE(settings.acpAgentId, u"grok-build"_s);
        QCOMPARE(settings.acpCommand, u"grok"_s);
        QCOMPARE(settings.acpArgs, u"agent stdio"_s);
        QCOMPARE(providerLabel(Provider::Acp), u"ACP"_s);
        QCOMPARE(acpAgentDisplayName(settings), u"Grok Build"_s);
        QCOMPARE(acpAgentPreset(u"gemini"_s).args, u"--acp"_s);
        QCOMPARE(acpAgentIdMatching(u"goose"_s, u"acp"_s), u"goose"_s);
        QCOMPARE(acpAgentIdMatching(u"python3"_s, u"agent.py"_s), u"custom"_s);

        settings.apiFormat = ApiFormat::OpenAICompatible;
        QVERIFY(!usesAcpNative(settings));
        QVERIFY(providerRequiresApiKey(settings, Provider::Acp));
        QCOMPARE(acpAgentDisplayName(settings), u"ACP"_s);
    }
};

QTEST_GUILESS_MAIN(TestProviders)
#include "test_providers.moc"