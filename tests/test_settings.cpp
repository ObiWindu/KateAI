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
        s.opencodeApiKey = u"oc_sk_test"_s;
        s.opencodeModel = u"claude-sonnet-5-5"_s;
        s.opencodePlan = OpenCodePlan::Go;
        s.opencodeUrl = u"https://opencode.example.invalid/inference/go/v1"_s;
        s.acpAgentModels = u"{\"grok\": [\"m1\"]}"_s;
        s.acpAgentCommands = u"{\"grok\": [{\"name\":\"review\",\"description\":\"Review the changes\"}]}"_s;
        s.acceptAllAcpTools = false;
        s.frequencyPenalty = 0.25;
        s.presencePenalty = 0.5;
        s.toolCallTimeoutMs = 45000;
        s.structuredThinking = false;
        s.structuredPlanning = false;
        s.showPlanAsChecklist = false;
        s.maxThinkingTokens = 2048;
        s.maxPlanSteps = 4;
        s.requireVerification = false;
        s.maxVerificationAttempts = 1;
        s.telemetryEnabled = true;
        s.telemetryEndpoint = u"http://127.0.0.1:4318"_s;
        s.telemetryContentCapture = false;
        s.costBudgetUsd = 2.5;
        s.enableAutoRetry = false;
        s.maxRetryAttempts = 1;
        s.baseRetryDelaySeconds = 1;
        s.maxRetryDelaySeconds = 30;
        s.retryStrategy = u"linear"_s;
        s.voiceProvider = u"openai_compatible"_s;
        s.voiceApiKey = u"sk-voice-test"_s;
        s.voiceEndpoint = u"http://127.0.0.1:8000/v1/audio/transcriptions"_s;
        s.voiceModel = u"whisper-large-v3-turbo"_s;
        s.voiceLanguage = u"en"_s;
        s.voiceLocalCommand = u"whisper-cli -m /tmp/model.bin -f {file}"_s;
        s.voiceInputDevice = u"alsa_input.usb-mic"_s;
        s.voiceNoiseRemoval = false;
        s.voiceAutoSend = false;
        s.orchestration.enabled = true;
        s.orchestration.planner = ModelRoute{Provider::OpenAI, u"planner-test"_s, u"high"_s};
        s.orchestration.executor = ModelRoute{Provider::DeepSeek, u"executor-test"_s, u"low"_s};
        s.orchestration.maxReplans = 5;
        s.orchestration.maxReviewRounds = 3;
        s.orchestration.reviewMode = u"always"_s;

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
        QCOMPARE(loaded.opencodeApiKey, s.opencodeApiKey);
        QCOMPARE(loaded.opencodeModel, s.opencodeModel);
        QCOMPARE(loaded.opencodePlan, s.opencodePlan);
        QCOMPARE(loaded.opencodeUrl, s.opencodeUrl);
        QCOMPARE(loaded.acpAgentModels, s.acpAgentModels);
        QCOMPARE(loaded.acpAgentCommands, s.acpAgentCommands);
        QCOMPARE(loaded.acceptAllAcpTools, false);
        QCOMPARE(loaded.frequencyPenalty, s.frequencyPenalty);
        QCOMPARE(loaded.presencePenalty, s.presencePenalty);
        QCOMPARE(loaded.toolCallTimeoutMs, s.toolCallTimeoutMs);
        QCOMPARE(loaded.structuredThinking, s.structuredThinking);
        QCOMPARE(loaded.structuredPlanning, s.structuredPlanning);
        QCOMPARE(loaded.showPlanAsChecklist, s.showPlanAsChecklist);
        QCOMPARE(loaded.maxThinkingTokens, s.maxThinkingTokens);
        QCOMPARE(loaded.maxPlanSteps, s.maxPlanSteps);
        QCOMPARE(loaded.requireVerification, s.requireVerification);
        QCOMPARE(loaded.maxVerificationAttempts, s.maxVerificationAttempts);
        QCOMPARE(loaded.telemetryEnabled, s.telemetryEnabled);
        QCOMPARE(loaded.telemetryEndpoint, s.telemetryEndpoint);
        QCOMPARE(loaded.telemetryContentCapture, s.telemetryContentCapture);
        QCOMPARE(loaded.costBudgetUsd, s.costBudgetUsd);
        QCOMPARE(loaded.enableAutoRetry, s.enableAutoRetry);
        QCOMPARE(loaded.maxRetryAttempts, s.maxRetryAttempts);
        QCOMPARE(loaded.baseRetryDelaySeconds, s.baseRetryDelaySeconds);
        QCOMPARE(loaded.maxRetryDelaySeconds, s.maxRetryDelaySeconds);
        QCOMPARE(loaded.retryStrategy, s.retryStrategy);
        QCOMPARE(loaded.voiceProvider, s.voiceProvider);
        QCOMPARE(loaded.voiceApiKey, s.voiceApiKey);
        QCOMPARE(loaded.voiceEndpoint, s.voiceEndpoint);
        QCOMPARE(loaded.voiceModel, s.voiceModel);
        QCOMPARE(loaded.voiceLanguage, s.voiceLanguage);
        QCOMPARE(loaded.voiceLocalCommand, s.voiceLocalCommand);
        QCOMPARE(loaded.voiceInputDevice, s.voiceInputDevice);
        QCOMPARE(loaded.voiceNoiseRemoval, s.voiceNoiseRemoval);
        QCOMPARE(loaded.voiceAutoSend, s.voiceAutoSend);
        QCOMPARE(loaded.orchestration.enabled, true);
        QCOMPARE(loaded.orchestration.planner.provider, Provider::OpenAI);
        QCOMPARE(loaded.orchestration.planner.model, u"planner-test"_s);
        QCOMPARE(loaded.orchestration.planner.effort, u"high"_s);
        QCOMPARE(loaded.orchestration.executor.provider, Provider::DeepSeek);
        QCOMPARE(loaded.orchestration.executor.model, u"executor-test"_s);
        QCOMPARE(loaded.orchestration.maxReplans, 5);
        QCOMPARE(loaded.orchestration.maxReviewRounds, 3);
        QCOMPARE(loaded.orchestration.reviewMode, u"always"_s);
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
        QCOMPARE(fresh.acceptAllAcpTools, true);
        QCOMPARE(fresh.opencodePlan, OpenCodePlan::PayAsYouGo);
        QCOMPARE(fresh.opencodeUrl, QString());
        QCOMPARE(fresh.maxPlanSteps, defaults.maxPlanSteps);
        QCOMPARE(fresh.requireVerification, defaults.requireVerification);
        QCOMPARE(fresh.enableAutoRetry, defaults.enableAutoRetry);
        QCOMPARE(fresh.retryStrategy, defaults.retryStrategy);
        QCOMPARE(fresh.verbosity, defaults.verbosity);
    }
    void unlimitedSavedConversationsIsPreserved()
    {
        // 0 is the config UI's "Unlimited". Clamping it to 1 made the next
        // save prune every other conversation.
        Settings s;
        s.maxSavedConversations = 0;
        SettingsStore::save(s);
        QCOMPARE(SettingsStore::load().maxSavedConversations, 0);
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "test_settings.moc"
