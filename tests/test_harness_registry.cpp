/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harness/registry.h"

#include "modes.h"

#include <QJsonDocument>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

QJsonObject mcpTool(int index)
{
    const QString name = u"mcp__server__tool%1"_s.arg(index);
    QJsonObject parameters{
        {u"type"_s, u"object"_s},
        {u"properties"_s, QJsonObject{
             {u"query"_s, QJsonObject{{u"type"_s, u"string"_s},
                                      {u"description"_s, u"Search the remote catalog."_s}}},
         }},
    };
    return QJsonObject{
        {u"type"_s, u"function"_s},
        {u"function"_s, QJsonObject{
             {u"name"_s, name},
             {u"description"_s,
              u"Remote server operation number %1: search issues, fetch records, and update tickets."_s.arg(index)},
             {u"parameters"_s, parameters},
         }},
    };
}

} // namespace

class TestHarnessRegistry : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void specsCoverEveryBuiltin()
    {
        ToolRegistry registry;
        const QStringList names = allBuiltInToolNames();
        for (const QString &name : names) {
            QVERIFY2(registry.contains(name), qPrintable(name));
        }
        QCOMPARE(registry.specs().size(), names.size());
    }

    void builtinSchemasAreByteIdentical()
    {
        ToolRegistry registry;
        const QJsonArray definitions = toolDefinitions(ToolAccess::unrestricted());
        for (const QJsonValue &value : definitions) {
            const QJsonObject definition = value.toObject();
            const QString name = definition.value(u"function"_s).toObject().value(u"name"_s).toString();
            const ToolSpec spec = registry.spec(name);
            QVERIFY2(!spec.name.isEmpty(), qPrintable(name));
            QCOMPARE(QJsonDocument(spec.schema).toJson(QJsonDocument::Compact),
                     QJsonDocument(definition).toJson(QJsonDocument::Compact));
        }
    }

    void searchFindsByIntent()
    {
        ToolRegistry registry;
        const ToolAccess access = ToolAccess::unrestricted();
        QCOMPARE(registry.search(u"debugger stack"_s, access).value(0), u"debug_stack"_s);
        QCOMPARE(registry.search(u"web search"_s, access).value(0), u"web_search"_s);
        QCOMPARE(registry.search(u"project graph"_s, access).value(0), u"query_project_graph"_s);
        QCOMPARE(registry.search(u"shell command"_s, access).value(0), u"bash"_s);
    }

    void largeMcpCatalogDefersAndDropsSchemaCost()
    {
        ToolRegistry registry;
        for (int i = 0; i < 100; ++i) {
            registry.addMcpDefinition(mcpTool(i));
        }
        const ToolAccess access = ToolAccess::unrestricted();
        const QJsonArray all = registry.definitions(access, true);
        const AdvertisedTools advertised = registry.advertised(access, 128000, true);
        QVERIFY(advertised.deferralActive);
        QVERIFY(advertised.deferred.size() >= 100);
        QVERIFY(advertised.tools.size() < all.size() / 3);

        const int allTokens = registry.schemaTokens(all);
        QVERIFY2(advertised.schemaTokens < allTokens * 3 / 4,
                 qPrintable(QStringLiteral("all %1, advertised %2")
                                .arg(allTokens)
                                .arg(advertised.schemaTokens)));

        QStringList advertisedNames;
        for (const QJsonValue &value : advertised.tools) {
            advertisedNames.append(value.toObject().value(u"function"_s).toObject()
                                       .value(u"name"_s).toString());
        }
        QVERIFY(advertisedNames.contains(u"tool_search"_s));
        QVERIFY(advertisedNames.contains(u"tool_describe"_s));
        QVERIFY(advertisedNames.contains(u"tool_call"_s));
        QVERIFY(advertisedNames.contains(u"read_file"_s));
        QVERIFY(!advertisedNames.contains(u"debug_stack"_s));
    }

    void memoryWriteIsAlwaysAdvertised()
    {
        ToolRegistry registry;
        const AdvertisedTools advertised = registry.advertised(
            ToolAccess::unrestricted(), 128000, true);
        bool found = false;
        for (const QJsonValue &value : advertised.tools) {
            const QString name = value.toObject().value(u"function"_s).toObject()
                                     .value(u"name"_s).toString();
            found = found || name == u"memory_write"_s;
        }
        QVERIFY(found);
    }

    void discoveredToolIsAdvertisedNextRequest()
    {
        ToolRegistry registry;
        for (int i = 0; i < 20; ++i) {
            registry.addMcpDefinition(mcpTool(i));
        }
        const ToolAccess access = ToolAccess::unrestricted();
        QVERIFY(!registry.isDiscovered(u"mcp__server__tool3"_s));
        registry.markDiscovered(u"mcp__server__tool3"_s);
        const QJsonArray definitions = registry.definitions(access);
        bool found = false;
        for (const QJsonValue &value : definitions) {
            if (value.toObject().value(u"function"_s).toObject().value(u"name"_s).toString()
                == u"mcp__server__tool3"_s) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    void resultBudgetsArePerTool()
    {
        ToolRegistry registry;
        QVERIFY(registry.resultBudgetFor(u"bash"_s) > registry.resultBudgetFor(u"list_dir"_s));
        QCOMPARE(registry.resultBudgetFor(u"read_tool_result"_s), 80000);
        QVERIFY(registry.resultBudgetFor(u"unknown_tool"_s) > 0);
    }

    void deniedToolsAreNotAdvertised()
    {
        ToolRegistry registry;
        ToolAccess access;
        access.allow(u"read_file"_s);
        const AdvertisedTools advertised = registry.advertised(access, 128000, true);
        QCOMPARE(advertised.tools.size(), 1);
        QCOMPARE(advertised.tools.first().toObject().value(u"function"_s).toObject()
                     .value(u"name"_s).toString(),
                 u"read_file"_s);
        QVERIFY(registry.search(u"bash"_s, access).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestHarnessRegistry)
#include "test_harness_registry.moc"
