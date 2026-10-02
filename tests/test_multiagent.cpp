#include "agentlocks.h"
#include "agentteam.h"
#include "modes.h"
#include "types.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestMultiAgent : public QObject
{
    Q_OBJECT

private:
    // Writes a file under the temp workspace, creating parent directories.
    static void writeFile(const QString &path, const QString &contents)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        file.write(contents.toUtf8());
    }

private Q_SLOTS:
    // --- roster ---------------------------------------------------------------

    void builtinRosterCoversTheStandardRoles()
    {
        const QList<AgentProfile> builtin = AgentTeam::builtinAgents();
        QStringList ids;
        for (const AgentProfile &profile : builtin) {
            ids.append(profile.id);
            QVERIFY(profile.builtIn);
            QVERIFY(!profile.description.isEmpty());
            QVERIFY(ModeRegistry::isBuiltInModeId(profile.modeId));
        }
        QCOMPARE(ids, QStringList({u"scout"_s, u"architect"_s, u"coder"_s, u"debugger"_s, u"reviewer"_s}));
    }

    void readonlyAgentsDoNotRunTheCodeMode()
    {
        const QList<AgentProfile> builtin = AgentTeam::builtinAgents();
        for (const AgentProfile &profile : builtin) {
            if (profile.id == u"coder"_s) {
                continue;
            }
            // Only the implementer may edit; everyone else is advisory.
            QVERIFY2(profile.modeId != u"code"_s, qPrintable(profile.id));
        }
    }

    void teamMergesBuiltinAndCustomAgents()
    {
        AgentTeam team;
        AgentProfile custom;
        custom.id = u"doc-writer"_s;
        custom.name = u"Doc Writer"_s;
        custom.modeId = u"ask"_s;
        custom.description = u"Writes and polishes documentation."_s;
        team.setCustomAgents({custom});

        const std::optional<AgentProfile> found = team.byId(u"doc-writer"_s);
        QVERIFY(found.has_value());
        QCOMPARE(found->name, u"Doc Writer"_s);
        QVERIFY(!found->builtIn);

        // Built-ins are still there.
        QVERIFY(team.byId(u"scout"_s).has_value());
        QVERIFY(team.describeRoster().contains(u"Doc Writer"_s));
    }

    void disabledAgentsAreNotOffered()
    {
        AgentTeam team;
        AgentProfile custom;
        custom.id = u"off"_s;
        custom.enabled = false;
        team.setCustomAgents({custom});
        QVERIFY(!team.byId(u"off"_s).has_value());
        QVERIFY(!team.describeRoster().contains(u"off"_s));
    }

    void customAgentsLoadFromTheWorkspace()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QVERIFY(QDir().mkpath(workspace.path() + u"/.kateai/agents"_s));
        writeFile(workspace.path() + u"/.kateai/agents/api-auditor.md"_s,
                  u"---\n"
                  u"id: api-auditor\n"
                  u"name: API Auditor\n"
                  u"description: Checks public API compatibility\n"
                  u"mode: ask\n"
                  u"---\n"
                  u"Extra guidance for the auditor.\n"_s);

        AgentTeam team;
        team.setWorkspace(workspace.path());
        const std::optional<AgentProfile> profile = team.byId(u"api-auditor"_s);
        QVERIFY(profile.has_value());
        QCOMPARE(profile->name, u"API Auditor"_s);
        QCOMPARE(profile->modeId, u"ask"_s);
        QCOMPARE(profile->description, u"Checks public API compatibility"_s);
    }

    void agentFileWithoutAModeKeyIsAccepted()
    {
        QTemporaryDir workspace;
        QVERIFY(QDir().mkpath(workspace.path() + u"/.kateai/agents"_s));
        writeFile(workspace.path() + u"/.kateai/agents/fetcher.md"_s,
                          u"---\nid: fetcher\nname: Fetcher\n---\nPulls dependencies.\n"_s);

        AgentTeam team;
        team.setWorkspace(workspace.path());
        const std::optional<AgentProfile> profile = team.byId(u"fetcher"_s);
        QVERIFY(profile.has_value());
        // No "mode" key, and the id is not a built-in mode: defaults to code.
        QCOMPARE(profile->modeId, u"code"_s);
        // The description falls back to the first body line.
        QCOMPARE(profile->description, u"Pulls dependencies."_s);
    }

    void unknownAgentIdsResolveToAUsableFallback()
    {
        AgentTeam team;
        const AgentProfile fallback = team.resolve(u"does-not-exist"_s);
        QCOMPARE(fallback.id, u"does-not-exist"_s);
        QCOMPARE(fallback.modeId, u"code"_s);

        // A real agent is returned unchanged.
        const AgentProfile scout = team.resolve(u"scout"_s);
        QCOMPARE(scout.modeId, u"ask"_s);
    }

    void agentProfileRoundTripsThroughJson()
    {
        AgentProfile profile;
        profile.id = u"perf"_s;
        profile.name = u"Performance"_s;
        profile.modeId = u"debug"_s;
        profile.description = u"Looks for hot paths."_s;
        profile.enabled = true;

        const AgentProfile parsed = AgentProfile::fromJson(profile.toJson());
        QCOMPARE(parsed.id, profile.id);
        QCOMPARE(parsed.name, profile.name);
        QCOMPARE(parsed.modeId, profile.modeId);
        QCOMPARE(parsed.description, profile.description);
    }

    // --- edit locks ------------------------------------------------------------

    void locksBlockTwoAgentsOnOneFile()
    {
        WorkspaceLocks locks;
        QVERIFY(locks.tryAcquire(u"src/a.cpp"_s, u"agent-1"_s));
        // Re-acquiring by the same owner is fine; one agent can touch a file
        // across several calls.
        QVERIFY(locks.tryAcquire(u"src/a.cpp"_s, u"agent-1"_s));
        // A second agent is refused.
        QVERIFY(!locks.tryAcquire(u"src/a.cpp"_s, u"agent-2"_s));
        QCOMPARE(locks.holder(u"src/a.cpp"_s), u"agent-1"_s);

        // A different file is unaffected.
        QVERIFY(locks.tryAcquire(u"src/b.cpp"_s, u"agent-2"_s));
    }

    void locksAreReleasedPerAgentAndPerPath()
    {
        WorkspaceLocks locks;
        QVERIFY(locks.tryAcquire(u"a.txt"_s, u"agent-1"_s));
        QVERIFY(locks.tryAcquire(u"b.txt"_s, u"agent-1"_s));

        locks.release(u"a.txt"_s, u"agent-1"_s);
        QVERIFY(!locks.isLocked(u"a.txt"_s));
        QVERIFY(locks.isLocked(u"b.txt"_s));

        locks.releaseAll(u"agent-1"_s);
        QCOMPARE(locks.count(), 0);
    }

    void releaseByAnotherAgentDoesNotStealTheLock()
    {
        WorkspaceLocks locks;
        QVERIFY(locks.tryAcquire(u"a.txt"_s, u"agent-1"_s));
        locks.release(u"a.txt"_s, u"agent-2"_s);
        QVERIFY(locks.isLocked(u"a.txt"_s));
    }

    void equivalentPathsCollide()
    {
        WorkspaceLocks locks;
        QVERIFY(locks.tryAcquire(u"a.txt"_s, u"agent-1"_s));
        // "./a.txt" is the same file, so a differently spelled path must not
        // slip past the guard.
        QVERIFY(!locks.tryAcquire(u"./a.txt"_s, u"agent-2"_s));
        QVERIFY(!locks.tryAcquire(QDir::cleanPath(u"./a.txt"_s), u"agent-3"_s));
    }

    void emptyPathsAreAlwaysAcquirable()
    {
        WorkspaceLocks locks;
        // A tool without a path must not be blocked by an unrelated lock.
        QVERIFY(locks.tryAcquire(QString(), u"agent-1"_s));
        QCOMPARE(locks.count(), 0);
    }

    void lockDeniedIsReported()
    {
        WorkspaceLocks locks;
        QString reportedPath;
        QString reportedHolder;
        QObject::connect(&locks, &WorkspaceLocks::lockDenied, &locks, [&](const QString &path, const QString &, const QString &holder) {
            reportedPath = path;
            reportedHolder = holder;
        });

        QVERIFY(locks.tryAcquire(u"a.txt"_s, u"agent-1"_s));
        QVERIFY(!locks.tryAcquire(u"a.txt"_s, u"agent-2"_s));
        QVERIFY(!reportedPath.isEmpty());
        QCOMPARE(reportedHolder, u"agent-1"_s);
    }

    // --- new_task schema ---------------------------------------------------------

    void newTaskSchemaAdvertisesTheRosterOptions()
    {
        const QJsonArray tools = toolDefinitions(ModeRegistry::toolAccessForGroups({ToolGroup::read(), ToolGroup::orchestrate()}));
        QJsonObject schema;
        for (const QJsonValue &value : tools) {
            const QJsonObject function = value.toObject().value(u"function"_s).toObject();
            if (function.value(u"name"_s).toString() == u"new_task"_s) {
                schema = function.value(u"parameters"_s).toObject();
            }
        }
        QVERIFY(!schema.isEmpty());
        const QJsonObject properties = schema.value(u"properties"_s).toObject();
        QVERIFY(properties.contains(u"agent"_s));
        QVERIFY(properties.contains(u"mode"_s));
        QVERIFY(properties.contains(u"include_transcript"_s));
        QVERIFY(properties.contains(u"description"_s));

        // Only the description is required.
        QStringList required;
        for (const QJsonValue &value : schema.value(u"required"_s).toArray()) {
            required.append(value.toString());
        }
        QCOMPARE(required, QStringList({u"description"_s}));
    }

    // --- reachability ---------------------------------------------------------

    // The roster is useless if the model cannot call new_task. Code is the
    // default mode, so delegation has to be available there.
    void workingModesCanDelegate()
    {
        ModeRegistry registry;
        for (const QString &modeId : {u"code"_s, u"debug"_s, u"orchestrator"_s}) {
            const ModeDefinition mode = registry.modeOrDefault(modeId);
            QVERIFY2(ModeRegistry::toolAccessFor(mode, false).allows(subtaskToolName()), qPrintable(modeId));
        }
    }

    // Ask and Architect promise in their own role text that they cannot change
    // anything. Handing them new_task would let them spawn a coder that edits,
    // so the read-only guarantee has to hold at the tool level too.
    void readonlyModesCannotDelegate()
    {
        ModeRegistry registry;
        for (const QString &modeId : {u"ask"_s, u"architect"_s}) {
            const ModeDefinition mode = registry.modeOrDefault(modeId);
            QVERIFY2(mode.readOnly, qPrintable(modeId));
            QVERIFY2(!ModeRegistry::toolAccessFor(mode, false).allows(subtaskToolName()), qPrintable(modeId));
        }
    }

    void newTaskIsAbsentWithoutTheOrchestrateGroup()
    {
        ModeRegistry registry;
        const ModeDefinition mode = registry.modeOrDefault(u"ask"_s);
        const ToolAccess access = ModeRegistry::toolAccessFor(mode, false);
        QVERIFY(!access.allows(subtaskToolName()));
        // The schema must not be advertised either, or the model will only ever
        // see a tool it is refused when it calls it.
        for (const QJsonValue &value : toolDefinitions(access)) {
            QVERIFY(value.toObject().value(u"function"_s).toObject().value(u"name"_s).toString() != u"new_task"_s);
        }
    }

    // --- agent / mode resolution ---------------------------------------------

    // Mirrors AgentLoop::dispatchSubtask so the precedence rule is pinned here
    // rather than only inside the loop: an explicit mode always wins, an agent
    // supplies the default, and an unknown id still yields a usable subtask.
    static QString effectiveMode(const AgentTeam &team, const ModeRegistry &registry, const QString &agent, const QString &mode)
    {
        const QString lookupId = agent.isEmpty() ? mode : agent;
        const AgentProfile profile = team.resolve(lookupId);
        QString modeId = profile.modeId;
        if (registry.modeById(mode)) {
            modeId = mode;
        }
        return modeId;
    }

    void anAgentSuppliesTheDefaultMode()
    {
        AgentTeam team;
        ModeRegistry registry;
        QCOMPARE(effectiveMode(team, registry, u"coder"_s, QString()), u"code"_s);
        QCOMPARE(effectiveMode(team, registry, u"debugger"_s, QString()), u"debug"_s);
        QCOMPARE(effectiveMode(team, registry, u"reviewer"_s, QString()), u"ask"_s);
    }

    void anExplicitModeOverridesTheAgentDefault()
    {
        AgentTeam team;
        ModeRegistry registry;
        // The schema tells the model `mode` overrides the agent default, so
        // passing both has to honour the mode.
        QCOMPARE(effectiveMode(team, registry, u"coder"_s, u"ask"_s), u"ask"_s);
        QCOMPARE(effectiveMode(team, registry, u"reviewer"_s, u"code"_s), u"code"_s);
        // An agent given on its own still runs in its own mode.
        QCOMPARE(effectiveMode(team, registry, u"scout"_s, QString()), u"ask"_s);
    }

    void anUnknownAgentStillResolvesToAUsableMode()
    {
        AgentTeam team;
        ModeRegistry registry;
        // resolve() returns the generic code fallback rather than failing the
        // subtask, so an unrecognised id must not leave modeId unset.
        QCOMPARE(effectiveMode(team, registry, u"not-a-real-agent"_s, QString()), u"code"_s);
    }
};

QTEST_GUILESS_MAIN(TestMultiAgent)
#include "test_multiagent.moc"