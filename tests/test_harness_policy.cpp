/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/audit.h"
#include "harness/policy.h"
#include "sandbox.h"

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

QJsonObject argsForPath(const QString &path)
{
    return QJsonObject{{u"path"_s, path}};
}

QJsonObject argsForCommand(const QString &command)
{
    return QJsonObject{{u"command"_s, command}};
}

} // namespace

class TestHarnessPolicy : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    Sandbox *m_sandbox = nullptr;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_sandbox = new Sandbox(m_dir.path(), SandboxProfile::Off);
    }

    void cleanupTestCase()
    {
        delete m_sandbox;
    }

    void denyWinsOverBroadAllow()
    {
        const QString policyPath = m_dir.filePath(u".kateai/policy.json"_s);
        const QJsonObject ruleDeny{{u"id"_s, u"deny-secret"_s},
                                   {u"match"_s, QJsonObject{{u"tool"_s, u"edit_file"_s},
                                                            {u"path"_s, u"src/secret/**"_s}}},
                                   {u"decision"_s, u"deny"_s},
                                   {u"reason"_s, u"secrets are off limits"_s}};
        const QJsonObject ruleAllow{{u"id"_s, u"allow-src"_s},
                                    {u"match"_s, QJsonObject{{u"tool"_s, u"edit_file"_s},
                                                             {u"path"_s, u"src/**"_s}}},
                                    {u"decision"_s, u"allow"_s}};
        const QJsonObject ruleAsk{{u"id"_s, u"ask-rm"_s},
                                  {u"match"_s, QJsonObject{{u"tool"_s, u"bash"_s},
                                                           {u"commandPrefix"_s,
                                                            QJsonArray{u"rm"_s}}}},
                                  {u"decision"_s, u"ask"_s}};
        const QJsonObject forcePush{{u"id"_s, u"deny-force-push"_s},
                                    {u"match"_s, QJsonObject{{u"tool"_s, u"bash"_s},
                                                             {u"commandPrefix"_s,
                                                              QJsonArray{u"git push"_s}},
                                                             {u"commandRegex"_s, u"--force"_s}}},
                                    {u"decision"_s, u"deny"_s}};
        QJsonObject root;
        root.insert(u"rules"_s, QJsonArray{ruleDeny, ruleAllow, ruleAsk, forcePush});
        QVERIFY(writeFile(policyPath, QJsonDocument(root).toJson()));

        PolicyEngine engine;
        QVERIFY(engine.loadFiles({policyPath}));
        QCOMPARE(engine.loadedFiles().size(), 1);

        PolicyDecision decision = engine.evaluate(u"edit_file"_s, argsForPath(u"src/secret/a.cpp"_s),
                                                  *m_sandbox, m_dir.path());
        QCOMPARE(decision.verdict, PolicyVerdict::Deny);
        QCOMPARE(decision.ruleId, u"deny-secret"_s);

        decision = engine.evaluate(u"edit_file"_s, argsForPath(u"src/a.cpp"_s), *m_sandbox,
                                   m_dir.path());
        QCOMPARE(decision.verdict, PolicyVerdict::Allow);
        QCOMPARE(decision.ruleId, u"allow-src"_s);

        decision = engine.evaluate(u"bash"_s, argsForCommand(u"git push --force origin main"_s),
                                   *m_sandbox, m_dir.path());
        QCOMPARE(decision.verdict, PolicyVerdict::Deny);

        decision = engine.evaluate(u"bash"_s, argsForCommand(u"rm -r build"_s), *m_sandbox,
                                   m_dir.path());
        QCOMPARE(decision.verdict, PolicyVerdict::Ask);

        decision = engine.evaluate(u"bash"_s, argsForCommand(u"echo hello"_s), *m_sandbox,
                                   m_dir.path());
        QCOMPARE(decision.verdict, PolicyVerdict::NoMatch);
    }

    void hardlineSurvivesEverything()
    {
        Sandbox sandbox(m_dir.path(), SandboxProfile::Off);
        QString reason;
        QVERIFY(PolicyEngine::hardline(u"bash"_s, argsForCommand(u"rm -rf /"_s), sandbox, &reason));
        QVERIFY(!reason.isEmpty());
        QVERIFY(PolicyEngine::hardline(u"edit_file"_s, argsForPath(u".env"_s), sandbox));
        QVERIFY(PolicyEngine::hardline(u"read_file"_s, argsForPath(u"/etc/shadow"_s), sandbox));
        QVERIFY(PolicyEngine::hardline(u"bash"_s, argsForCommand(u"cat .env"_s), sandbox));
        QVERIFY(!PolicyEngine::hardline(u"read_file"_s, argsForPath(u"src/main.cpp"_s), sandbox));
    }

    void symlinksAreResolvedBeforeMatching()
    {
#ifndef Q_OS_WIN
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(u"src/real"_s)));
        QVERIFY(writeFile(dir.filePath(u"src/real/secret.txt"_s), "no\n"));
        QVERIFY(QFile::link(dir.filePath(u"src/real"_s), dir.filePath(u"src/link"_s)));

        const QString policyPath = dir.filePath(u"policy.json"_s);
        const QJsonObject rule{{u"id"_s, u"deny-real"_s},
                               {u"match"_s, QJsonObject{{u"path"_s, u"src/real/**"_s}}},
                               {u"decision"_s, u"deny"_s}};
        QJsonObject root;
        root.insert(u"rules"_s, QJsonArray{rule});
        QVERIFY(writeFile(policyPath, QJsonDocument(root).toJson()));

        PolicyEngine engine;
        QVERIFY(engine.loadFiles({policyPath}));
        Sandbox sandbox(dir.path(), SandboxProfile::Off);
        const PolicyDecision decision = engine.evaluate(
            u"read_file"_s, argsForPath(u"src/link/secret.txt"_s), sandbox, dir.path());
        QCOMPARE(decision.verdict, PolicyVerdict::Deny);
#endif
    }

    void invalidRulesWarnButDoNotBrick()
    {
        const QString policyPath = m_dir.filePath(u"invalid-policy.json"_s);
        const QJsonObject badDecision{{u"id"_s, u"bad"_s},
                                      {u"match"_s, QJsonObject{{u"tool"_s, u"read_file"_s}}},
                                      {u"decision"_s, u"maybe"_s}};
        QJsonObject root;
        root.insert(u"rules"_s, QJsonArray{badDecision});
        QVERIFY(writeFile(policyPath, QJsonDocument(root).toJson()));

        PolicyEngine engine;
        QVERIFY(engine.loadFiles({policyPath}));
        QVERIFY(engine.rules().isEmpty());
        QCOMPARE(engine.warnings().size(), 1);
        QCOMPARE(engine.evaluate(u"read_file"_s, argsForPath(u"a.cpp"_s), *m_sandbox,
                                 m_dir.path()).verdict,
                 PolicyVerdict::NoMatch);
    }

    void auditRedactsAndAppends()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AuditLog::setPathForTests(dir.filePath(u"audit.jsonl"_s));
        QVERIFY(AuditLog::recordPermission(u"turn"_s, u"bash"_s, u"deny"_s, u"hardline"_s,
                                           u"key sk-abcdefghijklmnop"_s));
        QVERIFY(AuditLog::recordPermission(u"turn"_s, u"edit_file"_s, u"allow"_s, u"ok"_s, {}));

        const QStringList lines = AuditLog::recentLines(10);
        QCOMPARE(lines.size(), 2);
        QVERIFY(!lines.first().contains(u"sk-abcdefghijklmnop"_s));
        QVERIFY(lines.first().contains(u"[redacted]"_s));
        AuditLog::setPathForTests(QString());
    }
};

QTEST_GUILESS_MAIN(TestHarnessPolicy)
#include "test_harness_policy.moc"
