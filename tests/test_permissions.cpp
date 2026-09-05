#include "permissions.h"
#include "sandbox.h"

#include <QDir>
#include <QJsonObject>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestPermissions : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readToolsAutoAllow()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        QCOMPARE(policy.evaluate(u"read_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"list_dir"_s, {}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"grep"_s, QJsonObject{{u"pattern"_s, u"foo"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
    }

    void writesAskByDefault()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"edit_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"make"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
    }

    void acceptEditsAllowsWritesButAsksShell()
    {
        PermissionPolicy policy(PermissionMode::AcceptEdits);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"make"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"ls"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
    }

    void alwaysApprove()
    {
        PermissionPolicy policy(PermissionMode::AlwaysApprove);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, {}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"make"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"rm -rf /"_s}}, box, &reason), PermissionPolicy::Verdict::Deny);
    }

    void sessionGrant()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        policy.grantSession(u"write_file"_s);
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, {}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"make"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
    }
};

QTEST_GUILESS_MAIN(TestPermissions)
#include "test_permissions.moc"
