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
        QCOMPARE(policy.evaluate(u"query_project_graph"_s, QJsonObject{{u"query_type"_s, u"summary"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Allow);
    }

    void writesAskByDefault()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"edit_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"multi_edit_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"make"_s}}, box, &reason), PermissionPolicy::Verdict::Ask);
    }

    void acceptEditsAllowsWritesButAsksShell()
    {
        PermissionPolicy policy(PermissionMode::AcceptEdits);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"edit_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"multi_edit_file"_s, QJsonObject{{u"path"_s, u"a.cpp"_s}}, box, &reason), PermissionPolicy::Verdict::Allow);
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

    void autoApproveListSkipsThePrompt()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        policy.setAutoApproveTools({u"grep"_s, u"write_file"_s});
        QString reason;
        QCOMPARE(policy.evaluate(u"write_file"_s, {}, box, &reason), PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"edit_file"_s, {}, box, &reason), PermissionPolicy::Verdict::Ask);
    }

    void autoApproveCannotOverrideTheDenyList()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        policy.setAutoApproveTools({u"bash"_s});
        QString reason;
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"rm -rf /"_s}}, box, &reason), PermissionPolicy::Verdict::Deny);
    }

    void mcpToolsAreExecuteRiskAndAsk()
    {
        PermissionPolicy policy(PermissionMode::AcceptEdits);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        // An MCP tool can do anything its server offers, so Accept edits mode
        // must not wave it through the way it waves through write_file.
        QCOMPARE(policy.riskFor(u"mcp__files__read"_s), ToolRisk::Execute);
        QCOMPARE(policy.evaluate(u"mcp__files__write"_s, {}, box, &reason), PermissionPolicy::Verdict::Ask);
    }

    void mcpReadOnlyHintIsTreatedAsAReadTool()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        policy.setReadOnlyTools({u"mcp__git__status"_s});
        QString reason;
        QVERIFY(policy.isReadTool(u"mcp__git__status"_s));
        QCOMPARE(policy.riskFor(u"mcp__git__status"_s), ToolRisk::Read);
        QCOMPARE(policy.evaluate(u"mcp__git__status"_s, {}, box, &reason), PermissionPolicy::Verdict::Allow);
    }

    void subtasksAreExecuteRisk()
    {
        PermissionPolicy policy(PermissionMode::AcceptEdits);
        QCOMPARE(policy.riskFor(u"new_task"_s), ToolRisk::Execute);
    }

    void readOnlyShellCommandsCannotReachDeniedPaths()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        // "cat" is read-only, so these would otherwise be auto-allowed with no
        // prompt at all — the deny globs have to win over that shortcut.
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"cat .env"_s}}, box, &reason), PermissionPolicy::Verdict::Deny);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"cat ~/.ssh/id_rsa"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Deny);
        // Ordinary read-only shell work is still silent.
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"ls -la"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Allow);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"git status"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Allow);
    }

    void redirectionTurnsAReadOnlyCommandIntoAPrompt()
    {
        PermissionPolicy policy(PermissionMode::Ask);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        QString reason;
        // Read-only executable + redirect is a write, so it must ask.
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"cat notes.txt > out.txt"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Ask);
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"git status > report.md"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Ask);
        // Duplicating a descriptor is not a write.
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"ls 2>&1"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Allow);
    }

    void deniedPathsAreBlockedEvenWhenEverythingElseIsApproved()
    {
        PermissionPolicy policy(PermissionMode::AlwaysApprove);
        Sandbox box(QDir::tempPath(), SandboxProfile::Workspace);
        policy.setAutoApproveTools({u"bash"_s});
        policy.grantSession(u"bash"_s);
        QString reason;
        // Auto-approval, session grants and Always approve all sit below the
        // hard-deny rules, so none of them may unlock a secret.
        QCOMPARE(policy.evaluate(u"bash"_s, QJsonObject{{u"command"_s, u"cat .env"_s}}, box, &reason),
                 PermissionPolicy::Verdict::Deny);
    }
};

QTEST_GUILESS_MAIN(TestPermissions)
#include "test_permissions.moc"
