/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/prompt.h"

#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

PromptRequest makeRequest()
{
    PromptRequest request;
    request.mode = ModeRegistry::builtInModes().first();
    request.workspace = u"/tmp/project"_s;
    request.provider = Provider::OpenAI;
    request.modelName = u"gpt-4.1"_s;
    request.settings.contextWindow = 128000;
    request.toolNames = {u"read_file"_s, u"edit_file"_s};
    return request;
}

} // namespace

class TestHarnessPrompt : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void renderingIsDeterministic()
    {
        const PromptRequest request = makeRequest();
        const PromptResult first = PromptAssembler::assemble(request);
        const PromptResult second = PromptAssembler::assemble(request);
        QCOMPARE(first.text, second.text);
        QCOMPARE(first.staticHash, second.staticHash);
        QVERIFY(!first.text.isEmpty());
        QVERIFY(first.text.contains(u"Kate AI"_s));
        QVERIFY(first.text.contains(u"gpt-4.1"_s) == false); // the prompt is model-agnostic
    }

    void staticPrefixSurvivesSessionChanges()
    {
        PromptRequest first = makeRequest();
        first.rules = u"Rule one: use tabs."_s;
        PromptRequest second = makeRequest();
        second.rules = u"Rule two: use spaces."_s;
        second.plan = u"- [ ] change the parser"_s;

        const PromptResult a = PromptAssembler::assemble(first);
        const PromptResult b = PromptAssembler::assemble(second);
        QCOMPARE(a.staticHash, b.staticHash);
        QVERIFY(a.text != b.text);
        QVERIFY(b.text.contains(u"Rule two"_s));
        QVERIFY(b.text.contains(u"change the parser"_s));
    }

    void staticPrefixDoesNotContainEditorState()
    {
        PromptRequest request = makeRequest();
        request.toolSchemaTokens = 400;
        const PromptResult result = PromptAssembler::assemble(request);
        QVERIFY(!result.text.contains(u"<git_status>"_s));
        QVERIFY(!result.text.contains(u"<editor_context>"_s));
        QCOMPARE(result.manifest.value(u"toolSchemaTokens"_s).toInt(), 400);
    }

    void budgetTruncatesWithAMarker()
    {
        PromptRequest request = makeRequest();
        request.settings.contextWindow = 4096; // 1024-token prompt budget
        request.rules = QString(20000, u'x');
        const PromptResult result = PromptAssembler::assemble(request);
        QCOMPARE(result.budgetTokens, 4096);
        QVERIFY(result.truncated);
        QVERIFY2(result.totalTokens <= result.budgetTokens + 500,
                 qPrintable(QStringLiteral("total %1 budget %2")
                                .arg(result.totalTokens)
                                .arg(result.budgetTokens)));
        QVERIFY(result.text.contains(u"section truncated"_s));
        QVERIFY(result.manifest.value(u"truncated"_s).toBool());
    }

    void nonAsciiContentStaysWithinTheTokenBudget()
    {
        PromptRequest request = makeRequest();
        request.settings.contextWindow = 4096;
        // CJK characters cost roughly three bytes each, so the old
        // four-characters-per-token truncation kept about three times the
        // requested budget.
        request.rules = QString(4000, QChar(0x4E2D));
        const PromptResult result = PromptAssembler::assemble(request);
        QVERIFY(result.truncated);
        QVERIFY2(result.totalTokens <= result.budgetTokens + 100,
                 qPrintable(QStringLiteral("total %1 budget %2")
                                .arg(result.totalTokens)
                                .arg(result.budgetTokens)));
    }

    void toolSchemasAreReservedFromTheBudget()
    {
        PromptRequest request = makeRequest();
        request.settings.contextWindow = 4096;
        request.toolSchemaTokens = 3000;
        const PromptResult result = PromptAssembler::assemble(request);
        QVERIFY2(result.totalTokens <= result.budgetTokens + 100,
                 qPrintable(QStringLiteral("total %1 budget %2")
                                .arg(result.totalTokens)
                                .arg(result.budgetTokens)));
    }

    void tinyWindowBudgetNeverExceedsTheWindow()
    {
        PromptRequest request = makeRequest();
        request.settings.contextWindow = 2048;
        const PromptResult result = PromptAssembler::assemble(request);
        QVERIFY(result.budgetTokens <= 2048);
    }

    void profileChangesGuidanceOnly()
    {
        PromptRequest openai = makeRequest();
        PromptRequest anthropic = makeRequest();
        anthropic.provider = Provider::ClaudeCompatible;
        anthropic.modelName = u"claude-sonnet-4"_s;
        const PromptResult a = PromptAssembler::assemble(openai);
        const PromptResult b = PromptAssembler::assemble(anthropic);
        QCOMPARE(a.variant, PromptVariant::OpenAI);
        QCOMPARE(b.variant, PromptVariant::Anthropic);
        QVERIFY(a.text.contains(u"unified patches"_s, Qt::CaseInsensitive));
        QVERIFY(b.text.contains(u"visible preamble"_s, Qt::CaseInsensitive));
    }

    void verificationGateReplacesTheLegacyNudge()
    {
        PromptRequest request = makeRequest();
        request.verificationGate = true;
        const PromptResult result = PromptAssembler::assemble(request);
        QVERIFY(result.text.contains(u"harness runs the project's discovered checks"_s));
        QVERIFY(!result.text.contains(u"VERIFICATION REQUIREMENT"_s));
    }
};

QTEST_GUILESS_MAIN(TestHarnessPrompt)
#include "test_harness_prompt.moc"
