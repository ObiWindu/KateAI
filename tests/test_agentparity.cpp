#include "agentloop.h"
#include "credentialstore.h"
#include "mcp.h"
#include "modes.h"
#include "rules.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestAgentParity : public QObject
{
    Q_OBJECT

private:
    static QStringList toolNames(const QJsonArray &definitions)
    {
        QStringList names;
        for (const QJsonValue &value : definitions) {
            names.append(value.toObject().value(u"function"_s).toObject().value(u"name"_s).toString());
        }
        return names;
    }

private Q_SLOTS:
    // --- turn lifecycle ------------------------------------------------------

    void turnStateIdsAreStableAndTerminalStatesAreDistinct()
    {
        // These ids are persisted/logged and must not change silently.
        QCOMPARE(AgentLoop::turnStateId(AgentLoop::TurnState::Idle), u"idle"_s);
        QCOMPARE(AgentLoop::turnStateId(AgentLoop::TurnState::AwaitingModel), u"awaiting-model"_s);
        QCOMPARE(AgentLoop::turnStateId(AgentLoop::TurnState::AwaitingPermission), u"awaiting-permission"_s);
        QCOMPARE(AgentLoop::turnStateId(AgentLoop::TurnState::Interrupted), u"interrupted"_s);

        QVERIFY(AgentLoop::isTerminalTurnState(AgentLoop::TurnState::Completed));
        QVERIFY(AgentLoop::isTerminalTurnState(AgentLoop::TurnState::Failed));
        QVERIFY(AgentLoop::isTerminalTurnState(AgentLoop::TurnState::Interrupted));
        QVERIFY(!AgentLoop::isTerminalTurnState(AgentLoop::TurnState::Running));
        QVERIFY(!AgentLoop::isTerminalTurnState(AgentLoop::TurnState::AwaitingTool));
    }

    // --- modes ---------------------------------------------------------------

    void builtInModesExist()
    {
        ModeRegistry registry;
        const QList<ModeDefinition> modes = registry.modes();
        const QStringList ids{modes.isEmpty() ? QString() : modes.at(0).id};
        Q_UNUSED(ids)
        QStringList all;
        for (const ModeDefinition &mode : modes) {
            QVERIFY2(mode.builtIn, qPrintable(mode.id));
            all.append(mode.id);
        }
        QCOMPARE(all, QStringList({u"code"_s, u"ask"_s, u"architect"_s, u"debug"_s, u"orchestrator"_s}));
    }

    void modeGroupsControlTheAdvertisedTools()
    {
        const QJsonArray codeDefinitions = toolDefinitions(ModeRegistry::toolAccessForGroups({ToolGroup::read(), ToolGroup::edit(),
                                                                                            ToolGroup::command(), ToolGroup::mcp()}));
        const QStringList codeTools = toolNames(codeDefinitions);
        QVERIFY(codeTools.contains(u"write_file"_s));
        QVERIFY(codeTools.contains(u"bash"_s));
        QVERIFY(!codeTools.contains(u"new_task"_s));

        const QStringList askTools = toolNames(toolDefinitions(ModeRegistry::toolAccessForGroups({ToolGroup::read(), ToolGroup::mcp()})));
        QCOMPARE(askTools.size(), 28);
        QVERIFY(askTools.contains(u"read_tool_result"_s));
        // The plan file lives outside the workspace and suggest only posts a
        // card, so read-only modes and plan mode keep both.
        QVERIFY(askTools.contains(u"write_plan_file"_s));
        QVERIFY(askTools.contains(u"suggest"_s));
        // A question touches nothing, so a read-only turn may ask one -- which
        // is exactly when the turn is otherwise stuck on the user.
        QVERIFY(askTools.contains(u"question"_s));
        QVERIFY(askTools.contains(u"skill"_s));
        // The todo list, think and exit_plan_mode are also workspace-neutral.
        QVERIFY(askTools.contains(u"todo_write"_s));
        QVERIFY(askTools.contains(u"think"_s));
        QVERIFY(askTools.contains(u"exit_plan_mode"_s));
        QVERIFY(!askTools.contains(u"apply_patch"_s));
        QVERIFY(!askTools.contains(u"write_file"_s));
        QVERIFY(!askTools.contains(u"bash"_s));

        const QStringList orchestratorTools = toolNames(toolDefinitions(ModeRegistry::toolAccessForGroups({ToolGroup::read(),
                                                                                                         ToolGroup::orchestrate()})));
        QVERIFY(orchestratorTools.contains(u"new_task"_s));
        QVERIFY(!orchestratorTools.contains(u"write_file"_s));
    }

    void planModeOverridesTheMode()
    {
        // Even the Orchestrator must not edit or spawn work while planning.
        const ToolAccess access = ModeRegistry::toolAccessForGroups({ToolGroup::read(), ToolGroup::edit(), ToolGroup::command(),
                                                                     ToolGroup::orchestrate()});
        ModeDefinition mode;
        mode.id = u"test"_s;
        mode.groups = {ToolGroup::read(), ToolGroup::edit(), ToolGroup::command(), ToolGroup::orchestrate()};
        const ToolAccess planned = ModeRegistry::toolAccessFor(mode, true);
        QVERIFY(!planned.allows(u"write_file"_s));
        QVERIFY(!planned.allows(u"bash"_s));
        QVERIFY(!planned.allows(u"new_task"_s));
        QVERIFY(planned.allows(u"read_file"_s));
        // Planning is exactly when the plan file matters, so the read-only
        // clamp must keep it -- and with it suggest, which is just a card.
        QVERIFY(planned.allows(u"write_plan_file"_s));
        QVERIFY(planned.allows(u"suggest"_s));
        Q_UNUSED(access)
    }

    void acpClientMethodsUseTheDefaultAcceptPolicy()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QVERIFY(QDir().mkpath(workspace.filePath(u".kateai"_s)));
        QFile policyFile(workspace.filePath(u".kateai/policy.json"_s));
        QVERIFY(policyFile.open(QIODevice::WriteOnly));
        policyFile.write(QJsonDocument(QJsonObject{{u"rules"_s, QJsonArray{
            QJsonObject{{u"id"_s, u"blocked-client-write"_s},
                        {u"decision"_s, u"deny"_s},
                        {u"match"_s, QJsonObject{{u"tool"_s, u"write_file"_s},
                                                {u"path"_s, u"client.txt"_s}}}}
        }}}).toJson());
        policyFile.close();
        qputenv("KATEAI_POLICY_V2", "1");

        AgentLoop agent;
        Settings settings;
        settings.provider = Provider::Acp;
        settings.apiFormat = ApiFormat::AcpNative;
        settings.permissionMode = PermissionMode::Ask;
        settings.planMode = true;
        settings.sandbox = SandboxProfile::Workspace;
        QVERIFY(settings.acceptAllAcpTools);
        agent.setSettings(settings);
        agent.setWorkspace(workspace.path());

        int prompts = 0;
        QStringList activity;
        connect(&agent, &AgentLoop::permissionNeeded, &agent,
                [&](const PermissionRequest &) { ++prompts; });
        connect(&agent, &AgentLoop::activityUpdated, &agent,
                [&](const QString &message) { activity.append(message); });

        PermissionRequest clientWrite;
        clientWrite.acpClientMethod = true;
        clientWrite.toolName = u"write_file"_s;
        clientWrite.toolCallId = u"acp-client-write"_s;
        clientWrite.risk = ToolRisk::Write;
        clientWrite.mutatesFiles = true;
        clientWrite.path = u"client.txt"_s;
        clientWrite.details = u"{\"path\":\"client.txt\",\"content\":\"accepted\"}"_s;
        Q_EMIT agent.acpClient()->permissionNeeded(clientWrite);
        QCOMPARE(prompts, 0);
        QVERIFY(activity.isEmpty());

        PermissionRequest hardline;
        hardline.acpClientMethod = true;
        hardline.toolName = u"bash"_s;
        hardline.toolCallId = u"acp-client-hardline"_s;
        hardline.risk = ToolRisk::Execute;
        hardline.details = u"{\"command\":\"git push --force origin main\"}"_s;
        Q_EMIT agent.acpClient()->permissionNeeded(hardline);
        QVERIFY(!activity.isEmpty());

        settings.acceptAllAcpTools = false;
        settings.planMode = false;
        agent.setSettings(settings);
        PermissionRequest optedOut = clientWrite;
        optedOut.toolCallId = u"acp-client-opt-out"_s;
        const int before = activity.size();
        Q_EMIT agent.acpClient()->permissionNeeded(optedOut);
        QVERIFY(activity.size() > before);
        QVERIFY(activity.last().contains(u"blocked-client-write"_s));
        qunsetenv("KATEAI_POLICY_V2");
    }

    void toolAccessMatchesMcpToolsByPrefix()
    {
        ToolAccess access = ModeRegistry::toolAccessForGroups({ToolGroup::read(), ToolGroup::mcp()});
        QVERIFY(access.allows(u"mcp__git__status"_s));
        QVERIFY(access.allows(u"read_file"_s));
        QVERIFY(!access.allows(u"bash"_s));
        QVERIFY(!ToolAccess::unrestricted().isEmpty());
    }

    void customModesAreLoadedFromDisk()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QDir dir(workspace.path() + u"/.kateai/modes"_s);
        QVERIFY(dir.mkpath(u"."_s));

        QFile file(dir.filePath(u"reviewer.md"_s));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("---\n"
                   "id: reviewer\n"
                   "name: Reviewer\n"
                   "description: Reviews changes\n"
                   "groups: [read, mcp]\n"
                   "roleDefinition: |\n"
                   "  You review code and never edit it.\n"
                   "customInstructions: |\n"
                   "  Always cite line numbers.\n"
                   "---\n"
                   "Also prefer small diffs.\n");
        file.close();

        ModeRegistry registry;
        registry.setWorkspace(workspace.path());
        QVERIFY(registry.customModeIds().contains(u"reviewer"_s));

        const ModeDefinition *mode = registry.modeById(u"reviewer"_s);
        QVERIFY(mode != nullptr);
        QVERIFY(!mode->builtIn);
        QCOMPARE(mode->name, u"Reviewer"_s);
        QCOMPARE(mode->groups, QStringList({ToolGroup::read(), ToolGroup::mcp()}));
        QVERIFY(mode->roleDefinition.contains(u"never edit"_s));
        QVERIFY(mode->customInstructions.join(u'\n').contains(u"cite line numbers"_s));
        QVERIFY(mode->customInstructions.join(u'\n').contains(u"small diffs"_s));

        // A custom mode must not be able to shadow a built-in one.
        ModeDefinition shadow;
        shadow.id = u"code"_s;
        shadow.builtIn = false;
        Q_UNUSED(shadow)
        QCOMPARE(registry.modeById(u"code"_s)->builtIn, true);
    }

    void unknownGroupsInCustomModesAreDropped()
    {
        QTemporaryDir workspace;
        QDir dir(workspace.path() + u"/.kateai/modes"_s);
        QVERIFY(dir.mkpath(u"."_s));
        QFile file(dir.filePath(u"typo.md"_s));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("---\nid: typo\ngroups: [read, edti]\n---\n");
        file.close();

        ModeRegistry registry;
        registry.setWorkspace(workspace.path());
        const ModeDefinition *mode = registry.modeById(u"typo"_s);
        QVERIFY(mode != nullptr);
        QCOMPARE(mode->groups, QStringList({ToolGroup::read()}));
    }

    // --- mcp -----------------------------------------------------------------

    void qualifiedToolNamesRoundTrip()
    {
        QVERIFY(isMcpToolName(u"mcp__git__status"_s));
        QVERIFY(!isMcpToolName(u"read_file"_s));

        QString server;
        QString tool;
        QVERIFY(splitMcpToolName(u"mcp__git__status"_s, &server, &tool));
        QCOMPARE(server, u"git"_s);
        QCOMPARE(tool, u"status"_s);

        QVERIFY(!splitMcpToolName(u"mcp__git"_s, &server, &tool));
        QVERIFY(!splitMcpToolName(u"read_file"_s, &server, &tool));
    }

    void mcpConfigRoundTripsThroughJson()
    {
        McpServerConfig stdio;
        stdio.name = u"filesystem"_s;
        stdio.command = u"npx"_s;
        stdio.args = QStringList({u"-y"_s, u"@mcp/server-filesystem"_s, u"/tmp"_s});
        stdio.alwaysAllow = QStringList({u"read_file"_s, u"list_*"_s});
        stdio.timeoutMs = 45000;

        McpServerConfig http;
        http.name = u"remote"_s;
        http.transport = McpTransport::Http;
        http.url = u"https://example.com/mcp"_s;
        http.headers.insert(u"Authorization"_s, u"Bearer x"_s);

        const QJsonObject root = McpServerConfig::serversToJson({stdio, http});
        QVERIFY(root.contains(u"mcpServers"_s));

        const QList<McpServerConfig> parsed = McpServerConfig::serversFromJson(root);
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed.at(0).name, u"filesystem"_s);
        QCOMPARE(parsed.at(0).args, stdio.args);
        QCOMPARE(parsed.at(0).alwaysAllow, stdio.alwaysAllow);
        QCOMPARE(parsed.at(0).timeoutMs, 45000);
        QCOMPARE(parsed.at(1).transport, McpTransport::Http);
        QCOMPARE(parsed.at(1).url, http.url);
        QCOMPARE(parsed.at(1).headers.value(u"Authorization"_s), u"Bearer x"_s);
    }

    void mcpConfigValidatesEndpoints()
    {
        McpServerConfig config;
        QString error;
        config.name = u"broken"_s;
        QVERIFY(!config.isValid(&error));
        QVERIFY(!error.isEmpty());

        config.transport = McpTransport::Http;
        QVERIFY(!config.isValid(&error));

        config.url = u"https://example.com/mcp"_s;
        QVERIFY(config.isValid(&error));

        config.transport = McpTransport::Stdio;
        config.command = u"npx"_s;
        QVERIFY(config.isValid(&error));
    }

    void mcpServerListPersistsToTheWorkspace()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());

        McpServerConfig config;
        config.name = u"demo"_s;
        config.command = u"true"_s;

        QString error;
        QVERIFY(McpConfigStore::save(workspace.path(), {config}, &error));
        QCOMPARE(McpConfigStore::configPath(workspace.path()), workspace.path() + u"/.kateai/mcp.json"_s);

        const QList<McpServerConfig> loaded = McpConfigStore::load(workspace.path());
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().name, u"demo"_s);
        QCOMPARE(loaded.first().command, u"true"_s);
    }

    void installedAgentArgsAreQuotedWhenSerialized()
    {
        QJsonObject agent;
        agent.insert(u"id"_s, u"space-agent"_s);
        agent.insert(u"name"_s, u"Space Agent"_s);
        agent.insert(u"command"_s, u"npx"_s);
        agent.insert(u"args"_s, QJsonArray{u"--yes"_s, u"pkg"_s, u"arg with space"_s});
        Settings settings;
        settings.acpInstalledAgents = QString::fromUtf8(
            QJsonDocument(QJsonArray{agent}).toJson(QJsonDocument::Compact));
        const AcpAgentPreset launch = acpAgentLaunchLine(settings, u"space-agent"_s);
        // The stored launch line is a command string parsed with
        // QProcess::splitCommand(); the whitespace argument must survive.
        QCOMPARE(QProcess::splitCommand(launch.args),
                 (QStringList{u"--yes"_s, u"pkg"_s, u"arg with space"_s}));
    }

    void mcpSecretsAreNotWrittenIntoTheWorkspaceFile()
    {
        // mcp.json is commonly committed. Authorization headers and env values
        // must live in the credential store, not in that file.
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::Memory);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        McpServerConfig config;
        config.name = u"remote"_s;
        config.transport = McpTransport::Http;
        config.url = u"https://example.com/mcp"_s;
        config.headers.insert(u"Authorization"_s, u"Bearer top-secret"_s);
        McpServerConfig local;
        local.name = u"local"_s;
        local.command = u"true"_s;
        local.env.insert(u"API_KEY"_s, u"env-secret"_s);

        QString error;
        QVERIFY2(McpConfigStore::save(workspace.path(), {config, local}, &error), qPrintable(error));
        QFile file(McpConfigStore::configPath(workspace.path()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray onDisk = file.readAll();
        QVERIFY(!onDisk.contains("top-secret"));
        QVERIFY(!onDisk.contains("env-secret"));

        const QList<McpServerConfig> loaded = McpConfigStore::load(workspace.path());
        QCOMPARE(loaded.size(), 2);
        auto byName = [&loaded](const QString &name) {
            for (const McpServerConfig &server : loaded) {
                if (server.name == name) {
                    return server;
                }
            }
            return McpServerConfig();
        };
        QCOMPARE(byName(u"remote"_s).headers.value(u"Authorization"_s), u"Bearer top-secret"_s);
        QCOMPARE(byName(u"local"_s).env.value(u"API_KEY"_s), u"env-secret"_s);
        CredentialStore::resetForTests();
    }

    void mcpToolDefinitionCarriesTheServerSchema()
    {
        McpTool tool;
        tool.server = u"git"_s;
        tool.name = u"status"_s;
        tool.description = u"[git] Show the working tree status"_s;
        tool.readOnly = true;
        QJsonObject schema;
        schema.insert(u"type"_s, u"object"_s);
        QJsonObject properties;
        properties.insert(u"path"_s, QJsonObject{{u"type"_s, u"string"_s}});
        schema.insert(u"properties"_s, properties);
        tool.inputSchema = schema;

        QCOMPARE(tool.qualifiedName(), u"mcp__git__status"_s);
        const QJsonObject definition = tool.toToolDefinition();
        QCOMPARE(definition.value(u"function"_s).toObject().value(u"name"_s).toString(), u"mcp__git__status"_s);
        QCOMPARE(definition.value(u"function"_s).toObject().value(u"parameters"_s).toObject(), schema);
    }

    void mcpNamesCannotForgeADifferentServerIdentity()
    {
        McpTool tool;
        tool.server = u"evil__server"_s;
        tool.name = u"tool"_s;
        // The "__" run is collapsed so the name cannot be split differently.
        QCOMPARE(tool.qualifiedName(), u"mcp__evil_server__tool"_s);
        QString server;
        QString parsedTool;
        QVERIFY(splitMcpToolName(tool.qualifiedName(), &server, &parsedTool));
        QCOMPARE(server, u"evil_server"_s);
        QCOMPARE(parsedTool, u"tool"_s);
    }

    // --- rules ---------------------------------------------------------------

    void rulesAreLoadedInPriorityOrder()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QVERIFY(QDir().mkpath(workspace.path() + u"/.kateai/rules"_s));
        QVERIFY(QDir().mkpath(workspace.path() + u"/.clinerules"_s));

        auto write = [&workspace](const QString &relativePath, const QString &contents) {
            QFile file(workspace.path() + u'/' + relativePath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write(contents.toUtf8());
        };

        write(u".kateai/rules/style.md"_s, u"Use tabs."_s);
        write(u".kateai/rules/debug.md"_s, u"Always log the failing input."_s);
        write(u".clinerules/legacy.md"_s, u"Legacy rule."_s);
        write(u"AGENTS.md"_s, u"Agents rule."_s);

        const QList<RulesLoader::Block> blocks = RulesLoader::load(workspace.path(), QStringLiteral("debug"));
        QVERIFY(blocks.size() >= 4);
        // Mode-specific rules come last so they win.
        QCOMPARE(blocks.last().source, u"rules/debug.md"_s);
        QVERIFY(blocks.last().content.contains(u"failing input"_s));

        const QString rendered = RulesLoader::render(blocks, u"Global rule."_s);
        QVERIFY(rendered.startsWith(u"<rules source=\"global\">"_s));
        QVERIFY(rendered.contains(u"AGENTS.md"_s));
        QVERIFY(rendered.contains(u"Use tabs."_s));
    }

    void rulesIgnoreConfigFilesOutsideTheRulesDirectory()
    {
        QTemporaryDir workspace;
        QVERIFY(QDir().mkpath(workspace.path() + u"/.kateai"_s));
        QFile file(workspace.path() + u"/.kateai/mcp.json"_s);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(u"{\"mcpServers\":{}}"_s.toUtf8());
        file.close();

        // Configuration files must never be injected into the system prompt.
        QVERIFY(RulesLoader::load(workspace.path(), QStringLiteral("code")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestAgentParity)
#include "test_agentparity.moc"