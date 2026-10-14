/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "settings.h"

#include <QStandardPaths>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestSettings : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Never touch the developer's real kateai config.
        QStandardPaths::setTestModeEnabled(true);
    }

    void everyPersistedFieldSurvivesARoundTrip()
    {
        // save() used to write only part of the Settings struct, so any value
        // the config page set for the remaining fields silently reverted to the
        // struct default on the next start (wrong provider URL, empty key,
        // disabled retry/verification, …).
        Settings s;
        s.provider = Provider::ClaudeCompatible;
        s.apiFormat = ApiFormat::AnthropicCompatible;
        s.claudeCompatibleApiKey = u"sk-ant-test"_s;
        s.claudeCompatibleModel = u"claude-sonnet-test"_s;
        s.claudeCompatibleUrl = u"https://claude.example.invalid/v1"_s;
        s.openaiCompatibleApiKey = u"sk-oai-test"_s;
        s.openaiCompatibleModel = u"llama-test"_s;
        s.openaiCompatibleUrl = u"http://127.0.0.1:11434/v1"_s;
        s.kiloApiKey = u"kilo-test"_s;
        s.kiloModel = u"kilo-model-test"_s;
        s.acpAgentModels = u"{\"grok\": [\"m1\"]}"_s;
        s.messageSpeed = 1;
        s.frequencyPenalty = 0.25;
        s.presencePenalty = 0.5;
        s.toolCallTimeoutMs = 45000;
        s.maxContextMessages = 40;
        s.compactOnFailure = false;
        s.structuredThinking = false;
        s.structuredPlanning = false;
        s.showPlanAsChecklist = false;
        s.maxThinkingTokens = 2048;
        s.maxPlanSteps = 4;
        s.requireVerification = false;
        s.maxVerificationAttempts = 1;
        s.adaptiveTemperature = false;
        s.explorationTemperature = 0.7;
        s.exploitationTemperature = 0.05;
        s.enablePlanUpdates = false;
        s.narrativeProgress = false;
        s.enableAutoRetry = false;
        s.maxRetryAttempts = 1;
        s.baseRetryDelaySeconds = 1;
        s.maxRetryDelaySeconds = 30;
        s.retryStrategy = u"linear"_s;

        SettingsStore::save(s);
        const Settings loaded = SettingsStore::load();

        QCOMPARE(loaded.provider, s.provider);
        QCOMPARE(loaded.apiFormat, s.apiFormat);
        QCOMPARE(loaded.claudeCompatibleApiKey, s.claudeCompatibleApiKey);
        QCOMPARE(loaded.claudeCompatibleModel, s.claudeCompatibleModel);
        QCOMPARE(loaded.claudeCompatibleUrl, s.claudeCompatibleUrl);
        QCOMPARE(loaded.openaiCompatibleApiKey, s.openaiCompatibleApiKey);
        QCOMPARE(loaded.openaiCompatibleModel, s.openaiCompatibleModel);
        QCOMPARE(loaded.openaiCompatibleUrl, s.openaiCompatibleUrl);
        QCOMPARE(loaded.kiloApiKey, s.kiloApiKey);
        QCOMPARE(loaded.kiloModel, s.kiloModel);
        QCOMPARE(loaded.acpAgentModels, s.acpAgentModels);
        QCOMPARE(loaded.messageSpeed, s.messageSpeed);
        QCOMPARE(loaded.frequencyPenalty, s.frequencyPenalty);
        QCOMPARE(loaded.presencePenalty, s.presencePenalty);
        QCOMPARE(loaded.toolCallTimeoutMs, s.toolCallTimeoutMs);
        QCOMPARE(loaded.maxContextMessages, s.maxContextMessages);
        QCOMPARE(loaded.compactOnFailure, s.compactOnFailure);
        QCOMPARE(loaded.structuredThinking, s.structuredThinking);
        QCOMPARE(loaded.structuredPlanning, s.structuredPlanning);
        QCOMPARE(loaded.showPlanAsChecklist, s.showPlanAsChecklist);
        QCOMPARE(loaded.maxThinkingTokens, s.maxThinkingTokens);
        QCOMPARE(loaded.maxPlanSteps, s.maxPlanSteps);
        QCOMPARE(loaded.requireVerification, s.requireVerification);
        QCOMPARE(loaded.maxVerificationAttempts, s.maxVerificationAttempts);
        QCOMPARE(loaded.adaptiveTemperature, s.adaptiveTemperature);
        QCOMPARE(loaded.explorationTemperature, s.explorationTemperature);
        QCOMPARE(loaded.exploitationTemperature, s.exploitationTemperature);
        QCOMPARE(loaded.enablePlanUpdates, s.enablePlanUpdates);
        QCOMPARE(loaded.narrativeProgress, s.narrativeProgress);
        QCOMPARE(loaded.enableAutoRetry, s.enableAutoRetry);
        QCOMPARE(loaded.maxRetryAttempts, s.maxRetryAttempts);
        QCOMPARE(loaded.baseRetryDelaySeconds, s.baseRetryDelaySeconds);
        QCOMPARE(loaded.maxRetryDelaySeconds, s.maxRetryDelaySeconds);
        QCOMPARE(loaded.retryStrategy, s.retryStrategy);
    }

    void loadingAnEmptyConfigKeepsTheDocumentedDefaults()
    {
        // A fresh install must still start from the struct defaults, so the new
        // keys cannot shift behaviour for existing users.
        SettingsStore::save(Settings());
        const Settings fresh = SettingsStore::load();

        Settings defaults;
        QCOMPARE(fresh.claudeCompatibleUrl, defaults.claudeCompatibleUrl);
        QCOMPARE(fresh.openaiCompatibleUrl, defaults.openaiCompatibleUrl);
        QCOMPARE(fresh.structuredPlanning, defaults.structuredPlanning);
        QCOMPARE(fresh.structuredThinking, defaults.structuredThinking);
        QCOMPARE(fresh.maxPlanSteps, defaults.maxPlanSteps);
        QCOMPARE(fresh.requireVerification, defaults.requireVerification);
        QCOMPARE(fresh.enableAutoRetry, defaults.enableAutoRetry);
        QCOMPARE(fresh.retryStrategy, defaults.retryStrategy);
        QCOMPARE(fresh.verbosity, defaults.verbosity);
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "test_settings.moc"
