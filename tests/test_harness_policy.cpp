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
#include <QStandardPaths>
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

    void commandPrefixAllowRejectsShellChaining()
    {
        const QString policyPath = m_dir.filePath(u"prefix-policy.json"_s);
        const QJsonObject allowGit{{u"id"_s, u"allow-git-status"_s},
                                   {u"match"_s, QJsonObject{{u"tool"_s, u"bash"_s},
                                                            {u"commandPrefix"_s, QJsonArray{u"git status"_s}}}},
                                   {u"decision"_s, u"allow"_s}};
        const QJsonObject denyForcePush{{u"id"_s, u"deny-force"_s},
                                        {u"match"_s, QJsonObject{{u"tool"_s, u"bash"_s},
                                                                 {u"commandPrefix"_s, QJsonArray{u"git push"_s}},
                                                                 {u"commandRegex"_s, u"--force"_s}}},
                                        {u"decision"_s, u"deny"_s}};
        QJsonObject root;
        root.insert(u"rules"_s, QJsonArray{allowGit, denyForcePush});
        QVERIFY(writeFile(policyPath, QJsonDocument(root).toJson()));

        PolicyEngine engine;
        QVERIFY(engine.loadFiles({policyPath}));

        QCOMPARE(engine.evaluate(u"bash"_s, argsForCommand(u"git status"_s), *m_sandbox, m_dir.path()).verdict,
                 PolicyVerdict::Allow);
        QCOMPARE(engine.evaluate(u"bash"_s, argsForCommand(u"git status --short"_s), *m_sandbox, m_dir.path()).verdict,
                 PolicyVerdict::Allow);
        // Chained, piped, redirected, subshell, and multi-line commands must not auto-allow.
        for (const QString &chained : {u"git status && rm -rf src"_s,
                                       u"git status; id"_s,
                                       u"git status | cat"_s,
                                       u"git status > /tmp/out"_s,
                                       u"git status $(id)"_s,
                                       u"git status\nid"_s}) {
            QCOMPARE(engine.evaluate(u"bash"_s, argsForCommand(chained), *m_sandbox, m_dir.path()).verdict,
                     PolicyVerdict::NoMatch);
        }
        // Deny rules must still match chained commands.
        QCOMPARE(engine.evaluate(u"bash"_s, argsForCommand(u"git push --force origin main && echo ok"_s),
                                 *m_sandbox, m_dir.path()).verdict,
                 PolicyVerdict::Deny);
    }

    void applyPatchPathsAndNonExistentSymlinkAncestorsMatch()
    {
        const QString patchSecret = u"--- a/src/secret/key.txt\n+++ b/src/secret/key.txt\n@@ -1 +1 @@\n-a\n+b\n"_s;
        const QString patchEnv = u"--- a/.env\n+++ b/.env\n@@ -1 +1 @@\n-A=1\n+A=2\n"_s;
        QVERIFY(PolicyEngine::hardline(u"apply_patch"_s, QJsonObject{{u"patch"_s, patchEnv}}, *m_sandbox));

        const QString policyPath = m_dir.filePath(u"patch-policy.json"_s);
        const QJsonObject denySecret{{u"id"_s, u"deny-secret"_s},
                                     {u"match"_s, QJsonObject{{u"path"_s, u"src/secret/**"_s}}},
                                     {u"decision"_s, u"deny"_s}};
        const QJsonObject denyExec{{u"id"_s, u"deny-exec"_s},
                                   {u"match"_s, QJsonObject{{u"risk"_s, u"execute"_s}}},
                                   {u"decision"_s, u"deny"_s}};
        QJsonObject root;
        root.insert(u"rules"_s, QJsonArray{denySecret, denyExec});
        QVERIFY(writeFile(policyPath, QJsonDocument(root).toJson()));

        PolicyEngine engine;
        QVERIFY(engine.loadFiles({policyPath}));
        QCOMPARE(engine.evaluate(u"apply_patch"_s, QJsonObject{{u"patch"_s, patchSecret}},
                                 *m_sandbox, m_dir.path()).verdict,
                 PolicyVerdict::Deny);
        QCOMPARE(engine.evaluate(u"build_run"_s, QJsonObject{}, *m_sandbox, m_dir.path()).verdict,
                 PolicyVerdict::Deny);

#ifndef Q_OS_WIN
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(u"src/secret"_s)));
        QVERIFY(QFile::link(dir.filePath(u"src/secret"_s), dir.filePath(u"src/alias"_s)));
        Sandbox sandbox(dir.path(), SandboxProfile::Off);
        // Non-existent target file inside a symlinked ancestor directory must still match.
        QCOMPARE(engine.evaluate(u"write_file"_s, argsForPath(u"src/alias/new-file.txt"_s),
                                 sandbox, dir.path()).verdict,
                 PolicyVerdict::Deny);
#endif
    }

    void projectPolicyCannotAutoAllowByDefault()
    {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString policyPath = dir.filePath(u".kateai/policy.json"_s);
        const QJsonObject allowAll{{u"id"_s, u"allow-all"_s},
                                   {u"match"_s, QJsonObject{{u"tool"_s, u"bash"_s}}},
                                   {u"decision"_s, u"allow"_s}};
        const QJsonObject denySecret{{u"id"_s, u"deny-secret"_s},
                                     {u"match"_s, QJsonObject{{u"path"_s, u"secret/**"_s}}},
                                     {u"decision"_s, u"deny"_s}};
        QJsonObject root;
        root.insert(u"rules"_s, QJsonArray{allowAll, denySecret});
        QVERIFY(writeFile(policyPath, QJsonDocument(root).toJson()));

        PolicyEngine engine;
        QVERIFY(engine.load(dir.path()));
        QVERIFY(!engine.warnings().isEmpty());
        Sandbox sandbox(dir.path(), SandboxProfile::Off);
        QCOMPARE(engine.evaluate(u"bash"_s, argsForCommand(u"echo hi"_s), sandbox, dir.path()).verdict,
                 PolicyVerdict::NoMatch);
        QCOMPARE(engine.evaluate(u"read_file"_s, argsForPath(u"secret/a.txt"_s), sandbox, dir.path()).verdict,
                 PolicyVerdict::Deny);
    }
};

QTEST_GUILESS_MAIN(TestHarnessPolicy)
#include "test_harness_policy.moc"
