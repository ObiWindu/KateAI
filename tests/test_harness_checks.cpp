/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/checks.h"
#include "harness/turn.h"
#include "test_python.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

namespace
{

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

QString python(const QString &code)
{
    return u"%1 -c \"%2\""_s.arg(python3Command(), code);
}

} // namespace

class TestHarnessChecks : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void explicitConfigWins()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonObject config;
        config.insert(u"verify"_s, QJsonArray{
                                      python(u"import sys; sys.exit(0)"_s),
                                      QJsonObject{{u"command"_s, python(u"import sys; sys.exit(0)"_s)},
                                                  {u"paths"_s, QJsonArray{u"src/**"_s}}}});
        QVERIFY(writeFile(dir.filePath(u".kateai/config.json"_s),
                          QJsonDocument(config).toJson()));

        QVERIFY(writeFile(dir.filePath(u"build/CTestTestfile.cmake"_s), ""));
        const CheckPlan plan = CheckDiscovery::discover(dir.path(), {u"src/a.cpp"_s});
        QCOMPARE(plan.commands.size(), 2);
        QCOMPARE(plan.commands.at(0).command, python(u"import sys; sys.exit(0)"_s));
        QVERIFY(plan.sources.first().endsWith(u"config.json"_s));
    }

    void instructionCommandsAreParsed()
    {
        const QString text = u"# Rules\n\nSome text.\n\n## Commands\n\n"
                             u"- `ctest --output-on-failure` — run the suite\n"
                             u"- `cmake --build build` build it\n\n"
                             u"## Other\n- not a command\n"_s;
        const QStringList commands = CheckDiscovery::parseInstructionCommands(text);
        QCOMPARE(commands, QStringList({u"ctest --output-on-failure"_s,
                                        u"cmake --build build"_s}));
    }

    void detectsBuildSystems()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath(u"build/CTestTestfile.cmake"_s), ""));
        CheckPlan plan = CheckDiscovery::discover(dir.path());
        QCOMPARE(plan.commands.size(), 1);
        QCOMPARE(plan.commands.first().command, u"ctest --test-dir build --output-on-failure"_s);

        QTemporaryDir cargo;
        QVERIFY(cargo.isValid());
        QVERIFY(writeFile(cargo.filePath(u"Cargo.toml"_s), "[package]\n"));
        plan = CheckDiscovery::discover(cargo.path());
        QCOMPARE(plan.commands.first().command, u"cargo test"_s);
    }

    void pathFiltering()
    {
        CheckCommand command;
        command.command = u"true"_s;
        command.paths = {u"src/**"_s};
        QVERIFY(CheckDiscovery::pathRelevant(command, {u"src/main.cpp"_s}));
        QVERIFY(!CheckDiscovery::pathRelevant(command, {u"docs/readme.md"_s}));
        QVERIFY(CheckDiscovery::pathRelevant(command, {}));
    }

    void runnerPassesAndFails()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CheckPlan plan;
        plan.commands.append({python(u"import sys; sys.exit(0)"_s), {}, 10000});
        plan.commands.append({python(u"import sys; sys.exit(3)"_s), {}, 10000});

        const QList<CheckResult> results = CheckRunner::run(dir.path(), plan, nullptr);
        QCOMPARE(results.size(), 2);
        QVERIFY(results.at(0).ok);
        QCOMPARE(results.at(0).exitCode, 0);
        QVERIFY(!results.at(1).ok);
        QCOMPARE(results.at(1).exitCode, 3);
        QVERIFY(CheckRunner::evidence(results).contains(u"failed"_s));
    }

    void runnerKillsTimeouts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CheckPlan plan;
        plan.commands.append({python(u"import time; time.sleep(5)"_s), {}, 150});
        const QList<CheckResult> results = CheckRunner::run(dir.path(), plan, nullptr, 5000);
        QCOMPARE(results.size(), 1);
        QVERIFY(results.first().timedOut);
        QVERIFY(!results.first().ok);
        QVERIFY(results.first().durationMs < 4000);
    }

    void verificationMiddlewarePassesAndRetries()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TurnContext context;
        context.begin(u"turn"_s, u"conv"_s, dir.path(), u"fix"_s, u"code"_s);
        context.noteMutation(u"src/a.cpp"_s);

        CheckPlan failing;
        failing.commands.append({python(u"import sys; sys.exit(1)"_s), {}, 10000});
        VerificationMiddleware middleware(dir.path(), nullptr, 2);
        middleware.setPlan(failing);
        MiddlewareResult result = middleware.onStop(context);
        QCOMPARE(result.flow, MiddlewareFlow::Retry);
        QVERIFY(result.reason.contains(u"Verification failed"_s));
        QCOMPARE(context.verificationAttempts, 1);

        // A second failure consumes the retry budget; the next stop is denied.
        result = middleware.onStop(context);
        QCOMPARE(result.flow, MiddlewareFlow::Retry);
        QCOMPARE(context.verificationAttempts, 2);
        result = middleware.onStop(context);
        QCOMPARE(result.flow, MiddlewareFlow::Stop);

        CheckPlan passing;
        passing.commands.append({python(u"import sys; sys.exit(0)"_s), {}, 10000});
        TurnContext fresh;
        fresh.begin(u"turn"_s, u"conv"_s, dir.path(), u"fix"_s, u"code"_s);
        fresh.noteMutation(u"src/a.cpp"_s);
        VerificationMiddleware passingMiddleware(dir.path(), nullptr, 2);
        passingMiddleware.setPlan(passing);
        QCOMPARE(passingMiddleware.onStop(fresh).flow, MiddlewareFlow::Continue);
        QVERIFY(fresh.verificationPassed);
    }
};

QTEST_GUILESS_MAIN(TestHarnessChecks)
#include "test_harness_checks.moc"
