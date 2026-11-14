/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "modes.h"
#include "types.h"

#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestModes : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPerModeTemperatureOverride()
    {
        const QString document = u"---\n"
                                 u"id: brainstorm\n"
                                 u"name: Brainstorm\n"
                                 u"groups: [read]\n"
                                 u"temperature: 0.8\n"
                                 u"top_p: 0.9\n"
                                 u"roleDefinition: Think widely.\n"
                                 u"---\n"
                                 u"Extra instructions.\n"_s;

        bool ok = false;
        const ModeDefinition mode = ModeRegistry::parseModeDocument(document, u"fallback"_s, &ok);
        QVERIFY(ok);
        QCOMPARE(mode.id, u"brainstorm"_s);
        QVERIFY(mode.hasTemperature());
        QCOMPARE(mode.temperature, 0.8);
        QVERIFY(mode.hasTopP());
        QCOMPARE(mode.topP, 0.9);

        Settings settings;
        settings.temperature = 0.2;
        settings.topP = 0.95;

        const Settings effective = ModeRegistry::settingsForMode(settings, mode);
        // The mode wins for the knobs it names...
        QCOMPARE(effective.temperature, 0.8);
        QCOMPARE(effective.topP, 0.9);
        // ...and everything else stays exactly as configured.
        QCOMPARE(effective.provider, settings.provider);
        QCOMPARE(effective.agentMode, settings.agentMode);
        QCOMPARE(effective.maxToolCalls, settings.maxToolCalls);
    }

    void testTopPCamelCaseAliasAndClamping()
    {
        bool ok = false;
        const ModeDefinition mode = ModeRegistry::parseModeDocument(
            u"---\nid: x\nname: X\ngroups: [read]\ntopP: 0.5\ntemperature: 7.0\n---\n"_s,
            u"x"_s,
            &ok);
        QVERIFY(ok);
        // camelCase is accepted for the same knob...
        QVERIFY(mode.hasTopP());
        QCOMPARE(mode.topP, 0.5);
        // ...and an out-of-range temperature is clamped, not dropped.
        QVERIFY(mode.hasTemperature());
        QCOMPARE(mode.temperature, 2.0);

        Settings settings;
        settings.temperature = 0.2;
        settings.topP = 0.95;
        const Settings effective = ModeRegistry::settingsForMode(settings, mode);
        QCOMPARE(effective.temperature, 2.0);
        QCOMPARE(effective.topP, 0.5);
    }

    void testInvalidSamplingValuesIgnored()
    {
        bool ok = false;
        const ModeDefinition mode = ModeRegistry::parseModeDocument(
            u"---\nid: y\nname: Y\ngroups: [read]\ntemperature: hot\ntop_p: nope\n---\n"_s,
            u"y"_s,
            &ok);
        QVERIFY(ok);
        QVERIFY(!mode.hasTemperature());
        QVERIFY(!mode.hasTopP());

        Settings settings;
        settings.temperature = 0.2;
        settings.topP = 0.95;
        const Settings effective = ModeRegistry::settingsForMode(settings, mode);
        QCOMPARE(effective.temperature, 0.2);
        QCOMPARE(effective.topP, 0.95);
    }

    void testBuiltInModesCarryNoSamplingOverride()
    {
        // Every built-in mode leaves the global settings in force, so the
        // config page's temperature keeps meaning what it says.
        for (const ModeDefinition &mode : ModeRegistry::builtInModes()) {
            QVERIFY2(!mode.hasTemperature(), qPrintable(mode.id));
            QVERIFY2(!mode.hasTopP(), qPrintable(mode.id));

            Settings settings;
            settings.temperature = 0.2;
            settings.topP = 0.95;
            const Settings effective = ModeRegistry::settingsForMode(settings, mode);
            QCOMPARE(effective.temperature, settings.temperature);
            QCOMPARE(effective.topP, settings.topP);
        }
    }

    void testModeFileTemplateMentionsSamplingKeys()
    {
        const QString tmpl = ModeRegistry::modeFileTemplate(u"mine"_s);
        QVERIFY(tmpl.contains(u"temperature"_s));
        QVERIFY(tmpl.contains(u"top_p"_s));

        // The template must parse as a mode: the keys are commented out, so a
        // file straight out of the template carries no overrides.
        bool ok = false;
        const ModeDefinition mode = ModeRegistry::parseModeDocument(tmpl, u"mine"_s, &ok);
        QVERIFY(ok);
        QCOMPARE(mode.id, u"mine"_s);
        QVERIFY(!mode.hasTemperature());
        QVERIFY(!mode.hasTopP());
    }

    void testCustomModeFileRoundTrip()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString path = tempDir.path() + u"/.kateai/modes/deterministic.md"_s;
        QVERIFY(QDir().mkpath(tempDir.path() + u"/.kateai/modes"_s));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(u"---\nid: deterministic\nname: Deterministic\ngroups: [read, edit]\ntemperature: 0.0\ntop_p: 0.1\n---\n"_s.toUtf8());
        }

        ModeRegistry registry;
        registry.setWorkspace(tempDir.path());
        const ModeDefinition *mode = registry.modeById(u"deterministic"_s);
        QVERIFY(mode);
        QCOMPARE(mode->temperature, 0.0);
        QCOMPARE(mode->topP, 0.1);
    }

    void testToolGroupsCoverTheNewTools()
    {
        // The read-only group must carry the planning/asking tools so plan
        // mode and ask mode keep working with them; the edit group carries
        // apply_patch.
        const QStringList read = builtInToolsForGroup(ToolGroup::read());
        for (const QString &name : {u"todo_write"_s, u"todo_read"_s, u"think"_s, u"exit_plan_mode"_s, u"question"_s, u"suggest"_s}) {
            QVERIFY2(read.contains(name), qPrintable(name));
        }
        const QStringList edit = builtInToolsForGroup(ToolGroup::edit());
        QVERIFY(edit.contains(u"apply_patch"_s));
        // Every built-in name is reachable through some group or is the
        // subtask tool (orchestrate group).
        const QStringList all = allBuiltInToolNames();
        const QStringList orchestrate = builtInToolsForGroup(ToolGroup::orchestrate());
        for (const QString &name : all) {
            const bool covered = read.contains(name) || edit.contains(name)
                || builtInToolsForGroup(ToolGroup::command()).contains(name)
                || builtInToolsForGroup(ToolGroup::web()).contains(name)
                || orchestrate.contains(name);
            QVERIFY2(covered, qPrintable(name));
        }
        // Plan mode therefore allows asking and todos but not patching.
        ModeDefinition planLike;
        planLike.groups = {ToolGroup::read()};
        const ToolAccess planAccess = ModeRegistry::toolAccessFor(planLike, true);
        QVERIFY(planAccess.allows(u"question"_s));
        QVERIFY(planAccess.allows(u"todo_write"_s));
        QVERIFY(!planAccess.allows(u"apply_patch"_s));
    }
};

QTEST_GUILESS_MAIN(TestModes)
#include "test_modes.moc"
