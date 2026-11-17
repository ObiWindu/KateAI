/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "settings.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QStandardPaths>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestConfigMigration : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void versionIsWrittenOnSave()
    {
        SettingsStore::save(Settings());
        const KConfigGroup g = KConfigGroup(KSharedConfig::openConfig(), u"KateAI"_s);
        QCOMPARE(g.readEntry(u"ConfigVersion"_s, 0), SettingsStore::configVersion());
    }

    void legacyMaxIterationsMigrates()
    {
        // Simulate a pre-0.4.0 config that only has MaxIterations.
        KConfigGroup g = KConfigGroup(KSharedConfig::openConfig(), u"KateAI"_s);
        g.writeEntry(u"MaxIterations"_s, 7);
        g.deleteEntry(u"MaxToolCalls"_s);
        g.sync();

        const Settings loaded = SettingsStore::load();
        QCOMPARE(loaded.maxToolCalls, 7);
    }
};

QTEST_GUILESS_MAIN(TestConfigMigration)
#include "test_configmigration.moc"