#include "harness/skills.h"
#include "sandbox.h"
#include "tools.h"
#include "types.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestSkillTool : public QObject
{
    Q_OBJECT

private:
    static void writeFile(const QString &path, const QString &contents)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        file.write(contents.toUtf8());
    }

    static QString document(const QString &name, const QString &description,
                            const QString &body, const QString &extra = QString())
    {
        return u"---\nname: %1\ndescription: %2\n%3---\n%4"_s.arg(name, description, extra, body);
    }

    static ToolCall skillCall(const QString &name, const QString &task = QString())
    {
        ToolCall call;
        call.id = u"call-1"_s;
        call.name = u"skill"_s;
        call.arguments.insert(u"name"_s, name);
        if (!task.isEmpty()) {
            call.arguments.insert(u"task"_s, task);
        }
        return call;
    }

private Q_SLOTS:
    void init()
    {
        m_workspace = std::make_unique<QTemporaryDir>();
        QVERIFY(m_workspace->isValid());
        m_state = std::make_unique<QTemporaryDir>();
        QVERIFY(m_state->isValid());
        SkillRegistry::setHomeForTests(m_state->path());
        SkillRegistry::setTrustStorePath(m_state->path() + u"/trust.json"_s);
    }

    void cleanup()
    {
        SkillRegistry::setHomeForTests(QString());
        SkillRegistry::setTrustStorePath(QString());
        m_workspace.reset();
        m_state.reset();
    }

    void loadsATrustedSkillBodyAndSupportingFiles()
    {
        writeFile(m_workspace->path() + u"/.kateai/skills/review/SKILL.md"_s,
                  document(u"review"_s, u"Review a diff."_s, u"Read the diff and report findings.\n"_s));
        writeFile(m_workspace->path() + u"/.kateai/skills/review/template.md"_s, u"template\n"_s);

        SkillRegistry registry(m_workspace->path());
        QString error;
        QVERIFY2(registry.trust(u"review"_s, &error), qPrintable(error));

        Sandbox sandbox(m_workspace->path(), SandboxProfile::Workspace);
        ToolRunner runner(sandbox, nullptr);
        runner.setSkillRegistry(&registry);
        const ToolResult result = runner.run(skillCall(u"review"_s));
        QVERIFY2(result.ok, qPrintable(result.output));
        QVERIFY(result.output.contains(u"Read the diff and report findings."_s));
        QVERIFY(result.output.contains(u"template.md"_s));
    }

    void untrustedProjectSkillIsRefusedWithATrustHint()
    {
        writeFile(m_workspace->path() + u"/.kateai/skills/unsafe/SKILL.md"_s,
                  document(u"unsafe"_s, u"Unsafe."_s, u"Run a command.\n"_s));

        SkillRegistry registry(m_workspace->path());
        Sandbox sandbox(m_workspace->path(), SandboxProfile::Workspace);
        ToolRunner runner(sandbox, nullptr);
        runner.setSkillRegistry(&registry);
        const ToolResult result = runner.run(skillCall(u"unsafe"_s));
        QVERIFY(!result.ok);
        QVERIFY(result.output.contains(u"trust-skill"_s));
    }

    void userOnlyAndPathScopedSkillsAreEnforced()
    {
        writeFile(m_workspace->path() + u"/.kateai/skills/user-only/SKILL.md"_s,
                  document(u"user-only"_s, u"User only."_s, u"Body.\n"_s,
                           u"disable-model-invocation: true\n"_s));
        writeFile(m_workspace->path() + u"/.kateai/skills/api/SKILL.md"_s,
                  document(u"api"_s, u"API only."_s, u"Body.\n"_s,
                           u"paths: [\"src/api/**\"]\n"_s));

        SkillRegistry registry(m_workspace->path());
        QString error;
        QVERIFY(registry.trust(u"api"_s, &error));
        Sandbox sandbox(m_workspace->path(), SandboxProfile::Workspace);
        ToolRunner runner(sandbox, nullptr);
        runner.setSkillRegistry(&registry);

        const ToolResult userOnly = runner.run(skillCall(u"user-only"_s));
        QVERIFY(!userOnly.ok);
        QVERIFY(userOnly.output.contains(u"only be invoked by the user"_s));

        const ToolResult inactive = runner.run(skillCall(u"api"_s));
        QVERIFY(!inactive.ok);
        QVERIFY(inactive.output.contains(u"path-scoped"_s));

        runner.setTouchedPaths({u"src/api/client.cpp"_s});
        const ToolResult active = runner.run(skillCall(u"api"_s));
        QVERIFY2(active.ok, qPrintable(active.output));
    }

    void allowedSkillListIsEnforced()
    {
        writeFile(m_workspace->path() + u"/.kateai/skills/one/SKILL.md"_s,
                  document(u"one"_s, u"One."_s, u"Body one.\n"_s));
        SkillRegistry registry(m_workspace->path());
        QString error;
        QVERIFY(registry.trust(u"one"_s, &error));
        Sandbox sandbox(m_workspace->path(), SandboxProfile::Workspace);
        ToolRunner runner(sandbox, nullptr);
        runner.setSkillRegistry(&registry);
        runner.setAllowedSkills({u"other"_s});
        const ToolResult result = runner.run(skillCall(u"one"_s));
        QVERIFY(!result.ok);
        QVERIFY(result.output.contains(u"allowed skill list"_s));

        // AgentSpec skill lists accept a trailing '*' the same way tool lists
        // do (the fixture in test_multiagent uses "api-*").
        runner.setAllowedSkills({u"on*"_s});
        const ToolResult wildcard = runner.run(skillCall(u"one"_s));
        QVERIFY2(wildcard.ok, qPrintable(wildcard.output));
    }

private:
    std::unique_ptr<QTemporaryDir> m_workspace;
    std::unique_ptr<QTemporaryDir> m_state;
};

QTEST_GUILESS_MAIN(TestSkillTool)
#include "test_skilltool.moc"
