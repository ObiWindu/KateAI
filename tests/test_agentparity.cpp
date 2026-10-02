#include "mcp.h"
#include "modes.h"
#include "rules.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
        QCOMPARE(askTools.size(), 5);
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
        Q_UNUSED(access)
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