/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "configpage.h"
#include "plugin.h"
#include "settings.h"
#include "types.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QTabWidget>
#include <QStandardPaths>
#include <QTest>
#include <QWidget>

#include <memory>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

// A settings page needs a plugin for its defaults and its shared network
// manager. The real plugin object is heavy, so the test only relies on what the
// page actually calls: settings() and networkManager().
class StubPlugin : public KateAiPlugin
{
public:
    StubPlugin()
        : KateAiPlugin(nullptr)
    {
        Settings s = SettingsStore::load();
        s.contextWindow = 200000;
        setSettings(s);
    }
};

QTabWidget *findTabs(QWidget *page)
{
    const auto tabs = page->findChildren<QTabWidget *>();
    return tabs.isEmpty() ? nullptr : tabs.first();
}

// The provider combo is the one that carries provider ids.
QComboBox *findComboWithProviderIds(QWidget *page)
{
    for (QComboBox *combo : page->findChildren<QComboBox *>()) {
        if (combo->findData(providerId(Provider::OpenCode)) >= 0) {
            return combo;
        }
    }
    return nullptr;
}

} // namespace

namespace KateAi
{

class TestConfigPage : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void everyTabIsPresent()
    {
        StubPlugin plugin;
        KateAiConfigPage page(nullptr, &plugin);
        auto *tabs = findTabs(&page);
        QVERIFY2(tabs, "the settings page has no tab widget");
        // The tab labels are part of the page's contract: a lazily built tab
        // must still be listed, or a setting becomes unreachable.
        QStringList labels;
        for (int i = 0; i < tabs->count(); ++i) {
            // A single "&" is a mnemonic marker, "&&" a literal ampersand.
            labels.append(tabs->tabText(i).remove(u'&'));
        }
        const QStringList expected = {
            u"AI Providers"_s,      u"ACP Registry"_s,        u"Security & Permissions"_s,
            u"Agent & Context"_s,   u"Modes & Tools"_s,       u"MCP Servers"_s,
            u"Web Search"_s,        u"Completion"_s,          u"Checkpoints & Subtasks"_s,
            u"Agent Team"_s,        u"Themes"_s,              u"Voice"_s,
        };
        for (const QString &raw : expected) {
            // Normalise the same way as the labels read back above.
            const QString label = QString(raw).remove(u'&');
            QVERIFY2(labels.contains(label),
                     qPrintable(u"missing settings tab: %1 (have: %2)"_s
                                    .arg(label, labels.join(u", "_s))));
        }
    }

    void everyTabCanBeVisited()
    {
        StubPlugin plugin;
        KateAiConfigPage page(nullptr, &plugin);
        auto *tabs = findTabs(&page);
        QVERIFY(tabs);
        // Visiting a tab is what forces a lazily built one to materialise; a
        // tab that throws or leaves its widget null would only show up here.
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QCoreApplication::processEvents();
            QWidget *current = tabs->currentWidget();
            QVERIFY2(current, qPrintable(u"tab %1 has no widget"_s.arg(i)));
        }
    }

    void tabsAreBuiltOnlyWhenVisited()
    {
        StubPlugin plugin;
        KateAiConfigPage page(nullptr, &plugin);
        auto *tabs = findTabs(&page);
        QVERIFY(tabs);

        // The provider selector lives on the first tab, which the page opens
        // on, so it exists. A combo on a tab nobody visited must not.
        QVERIFY2(findComboWithProviderIds(&page), "the first tab was not built");

        // Every other tab is still just a placeholder at this point: opening
        // Settings must not construct twelve pages.
        int builtTabs = 0;
        for (int i = 0; i < tabs->count(); ++i) {
            if (!tabs->widget(i)->property("kateaiConfigTab").isValid()) {
                continue;
            }
            // A placeholder carries the id but has no child widgets.
            if (!tabs->widget(i)->findChildren<QWidget *>().isEmpty()) {
                ++builtTabs;
            }
        }
        QCOMPARE(builtTabs, 1);

        // Visiting one materialises exactly that one.
        tabs->setCurrentIndex(tabs->count() - 1);
        QCoreApplication::processEvents();
        QVERIFY2(!tabs->currentWidget()->findChildren<QWidget *>().isEmpty(),
                 "visiting a tab did not build its widgets");

        int builtAfter = 0;
        for (int i = 0; i < tabs->count(); ++i) {
            if (!tabs->widget(i)->findChildren<QWidget *>().isEmpty()) {
                ++builtAfter;
            }
        }
        QCOMPARE(builtAfter, 2);
    }

    void unvisitedTabsKeepStoredValuesOnApply()
    {
        StubPlugin plugin;
        Settings stored = plugin.settings();
        stored.checkpointsEnabled = true;
        stored.checkpointRetention = 7;
        stored.voiceAutoSend = true;
        stored.webSearchProvider = u"tavily"_s;
        plugin.setSettings(stored);

        KateAiConfigPage page(nullptr, &plugin);
        auto *tabs = findTabs(&page);
        QVERIFY(tabs);
        // Only the first tab was built. Applying must not invent values for the
        // tabs that never loaded their widgets.
        page.apply();

        const Settings applied = plugin.settings();
        QCOMPARE(applied.checkpointsEnabled, stored.checkpointsEnabled);
        QCOMPARE(applied.checkpointRetention, stored.checkpointRetention);
        QCOMPARE(applied.voiceAutoSend, stored.voiceAutoSend);
        QCOMPARE(applied.webSearchProvider, stored.webSearchProvider);
    }

    void providerSelectionSurvivesReset()
    {
        StubPlugin plugin;
        KateAiConfigPage page(nullptr, &plugin);
        auto *tabs = findTabs(&page);
        QVERIFY(tabs);
        tabs->setCurrentIndex(0);
        QCoreApplication::processEvents();

        const auto combos = page.findChildren<QComboBox *>();
        QComboBox *providerCombo = nullptr;
        for (QComboBox *combo : combos) {
            // The provider combo is the one carrying provider ids.
            if (combo->findData(providerId(Provider::OpenCode)) >= 0) {
                providerCombo = combo;
                break;
            }
        }
        QVERIFY2(providerCombo, "the provider selector was not found");

        // The page must reflect the settings it was constructed with.
        const Settings s = plugin.settings();
        QCOMPARE(providerCombo->currentData().toString(), providerId(s.provider));

        // A reset restores the stored value after the user edits it.
        const int original = providerCombo->currentIndex();
        providerCombo->setCurrentIndex(original == 0 ? 1 : 0);
        page.reset();
        QCOMPARE(providerCombo->currentIndex(), original);
    }

    void applyIsSafeWithoutEdits()
    {
        StubPlugin plugin;
        const Settings before = plugin.settings();
        KateAiConfigPage page(nullptr, &plugin);
        // Applying an untouched page must not change anything: a lazily built
        // tab that never materialised must not write defaults over the stored
        // settings.
        page.apply();
        const Settings after = plugin.settings();
        QCOMPARE(after.provider, before.provider);
        QCOMPARE(after.opencodeModel, before.opencodeModel);
        QCOMPARE(after.acpAgentId, before.acpAgentId);
        QCOMPARE(after.voiceProvider, before.voiceProvider);
        QCOMPARE(after.maxToolCalls, before.maxToolCalls);
    }
};

} // namespace KateAi

QTEST_MAIN(KateAi::TestConfigPage)

#include "test_configpage.moc"
