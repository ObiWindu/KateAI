/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/hooks.h"
#include "test_python.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

QJsonObject hookFor(const QString &event, const QString &matcher, const QString &pythonCode,
                    const QJsonObject &extra = {})
{
    QJsonObject hook;
    hook.insert(u"matcher"_s, matcher);
    hook.insert(u"command"_s, QJsonArray{python3Command(), u"-c"_s, pythonCode});
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
        hook.insert(it.key(), it.value());
    }
    QJsonObject root;
    QJsonObject hooks;
    hooks.insert(event, QJsonArray{hook});
    root.insert(u"hooks"_s, hooks);
    return root;
}

QJsonObject toolPayload(const QString &name)
{
    return QJsonObject{{u"tool"_s, QJsonObject{{u"name"_s, name}}}};
}

} // namespace

class TestHarnessHooks : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
    }

    void projectHooksNeedTrust()
    {
        const QJsonObject root = hookFor(
            u"PreToolUse"_s, u"edit_file"_s,
            u"import sys; sys.stdin.read(); print('{\"decision\":\"deny\",\"reason\":\"no edits\"}')"_s);
        QVERIFY(writeFile(m_dir.filePath(u".kateai/hooks.json"_s), QJsonDocument(root).toJson()));

        HookRegistry registry;
        QVERIFY(registry.load(m_dir.path()));
        QVERIFY(!registry.projectTrusted());
        QVERIFY(!registry.hasHooks(u"PreToolUse"_s));
        HookOutcome outcome = registry.run(u"PreToolUse"_s, toolPayload(u"edit_file"_s));
        QVERIFY(!outcome.blocked);

        registry.setProjectTrusted(true);
        QVERIFY(registry.hasHooks(u"PreToolUse"_s));
        outcome = registry.run(u"PreToolUse"_s, toolPayload(u"edit_file"_s));
        QVERIFY(outcome.blocked);
        QCOMPARE(outcome.reason, u"no edits"_s);

        // The matcher is scoped: another tool is not blocked.
        outcome = registry.run(u"PreToolUse"_s, toolPayload(u"read_file"_s));
        QVERIFY(!outcome.blocked);
    }

    void updateInputIsReturned()
    {
        const QJsonObject root = hookFor(
            u"PreToolUse"_s, u"edit_file"_s,
            u"import sys; sys.stdin.read(); print('{\"updatedInput\":{\"path\":\"safe.txt\"}}')"_s);
        QVERIFY(writeFile(m_dir.filePath(u".kateai/hooks.json"_s), QJsonDocument(root).toJson()));
        HookRegistry registry;
        QVERIFY(registry.load(m_dir.path()));
        registry.setProjectTrusted(true);
        const HookOutcome outcome = registry.run(u"PreToolUse"_s, toolPayload(u"edit_file"_s));
        QVERIFY(!outcome.blocked);
        QCOMPARE(outcome.updatedInput.value(u"path"_s).toString(), u"safe.txt"_s);
    }

    void exitTwoBlocks()
    {
        const QJsonObject root = hookFor(
            u"PreToolUse"_s, u"bash"_s,
            u"import sys; sys.stdin.read(); sys.exit(2)"_s);
        QVERIFY(writeFile(m_dir.filePath(u".kateai/hooks.json"_s), QJsonDocument(root).toJson()));
        HookRegistry registry;
        QVERIFY(registry.load(m_dir.path()));
        registry.setProjectTrusted(true);
        const HookOutcome outcome = registry.run(u"PreToolUse"_s, toolPayload(u"bash"_s));
        QVERIFY(outcome.blocked);
        QVERIFY(!outcome.reason.isEmpty());
    }

    void timeoutAndFailureFollowPolicy()
    {
        const QJsonObject root = hookFor(
            u"PostToolUse"_s, u"edit_file"_s, u"import time; time.sleep(5)"_s,
            QJsonObject{{u"timeoutMs"_s, 100}, {u"onFailure"_s, u"block"_s}});
        QVERIFY(writeFile(m_dir.filePath(u".kateai/hooks.json"_s), QJsonDocument(root).toJson()));
        HookRegistry registry;
        QVERIFY(registry.load(m_dir.path()));
        registry.setProjectTrusted(true);
        HookOutcome outcome = registry.run(u"PostToolUse"_s, toolPayload(u"edit_file"_s));
        QVERIFY(outcome.timedOut);
        QVERIFY(outcome.blocked);

        const QJsonObject failureRoot = hookFor(
            u"Stop"_s, u""_s, u"import sys; sys.stdin.read(); sys.exit(1)"_s,
            QJsonObject{{u"onFailure"_s, u"continue"_s}});
        QVERIFY(writeFile(m_dir.filePath(u".kateai/hooks.json"_s),
                          QJsonDocument(failureRoot).toJson()));
        HookRegistry failing;
        QVERIFY(failing.load(m_dir.path()));
        failing.setProjectTrusted(true);
        outcome = failing.run(u"Stop"_s, QJsonObject{{u"reason"_s, u"done"_s}});
        QVERIFY(!outcome.blocked);
        QVERIFY(outcome.failed);
    }

    void trustKeyIsStable()
    {
        QCOMPARE(HookRegistry::trustKeyFor(u"/tmp/example"_s),
                 HookRegistry::trustKeyFor(u"/tmp/example"_s));
        QVERIFY(HookRegistry::trustKeyFor(u"/tmp/example"_s)
                != HookRegistry::trustKeyFor(u"/tmp/other"_s));
    }
};

QTEST_GUILESS_MAIN(TestHarnessHooks)
#include "test_harness_hooks.moc"
