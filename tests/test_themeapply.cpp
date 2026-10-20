/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the property that a theme change reaches the chat UI, not just the
 * window around it:
 *
 *  1. The panel root, the header bar and the welcome screen follow the
 *     installed palette, so switching presets repaints the chat panel.
 *  2. Colour overrides land on the panel without a restart.
 *  3. A user card rendered before the switch is repainted after it, because
 *     the transcript rebuild re-creates the cards from the message list.
 *  4. A tool card is theme-independent: applyStyle() re-runs on a theme
 *     change, but the card keeps the same stylesheet in every theme.
 */

#include "chatwidget.h"
#include "chattheme.h"
#include "sessionstoreisolation.h"
#include "toolcallwidget.h"
#include "types.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestThemeApply : public QObject
{
    Q_OBJECT

private:
    static Settings settingsWithPreset(const QString &preset)
    {
        Settings settings;
        settings.themeSource = u"preset"_s;
        settings.themePreset = preset;
        return settings;
    }

    // Mirrors KateAiView: the palette is installed first, then the widget is
    // told about the new settings.
    static void applySettings(ChatWidget &widget, const Settings &settings)
    {
        ChatTheme::loadTheme(settings);
        widget.setSettings(settings);
    }

    static QWidget *headerBar(ChatWidget &widget)
    {
        return widget.findChild<QWidget *>(u"headerBar"_s);
    }

    static QWidget *welcomeWidget(ChatWidget &widget)
    {
        return widget.findChild<QWidget *>(u"welcomeWidget"_s);
    }

private Q_SLOTS:
    void testPanelFollowsTheInstalledPalette()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        const Settings dark = settingsWithPreset(u"kateai-dark"_s);
        applySettings(widget, dark);

        // The panel root paints the theme's backdrop: the chat UI itself
        // carries the palette, not just the window around it.
        QVERIFY2(widget.styleSheet().contains(ChatTheme::panelBg()), qPrintable(widget.styleSheet()));
        QVERIFY2(widget.styleSheet().contains(ChatTheme::textPrimary()), qPrintable(widget.styleSheet()));
        // The panel rule must select the root by ID: a class selector like
        // `ChatWidget { ... }` never matches the qualified class name
        // (KateAi::ChatWidget), and the backdrop would silently not paint.
        QVERIFY2(widget.styleSheet().contains(u"#chatWidget {"_s), qPrintable(widget.styleSheet()));

        QWidget *header = headerBar(widget);
        QVERIFY2(header, "header bar not found");
        QVERIFY2(header->styleSheet().contains(ChatTheme::panelBg()), qPrintable(header->styleSheet()));

        // The empty-state hero is part of the chat UI and follows the theme too.
        QWidget *welcome = welcomeWidget(widget);
        QVERIFY2(welcome, "welcome widget not found");
        const auto chips = welcome->findChildren<QPushButton *>(u"welcomeChip"_s);
        QVERIFY2(!chips.isEmpty(), "welcome chips not found");
        for (const auto *chip : chips) {
            QVERIFY2(chip->styleSheet().contains(ChatTheme::surfaceBg()), qPrintable(chip->styleSheet()));
        }
    }

    void testPresetSwitchRepaintsTheChatUI()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        applySettings(widget, settingsWithPreset(u"kateai-dark"_s));
        const QString darkPanel = ChatTheme::panelBg();

        applySettings(widget, settingsWithPreset(u"kateai-light"_s));

        // The whole panel re-reads the palette: root, header and welcome
        // chips all moved to the light surfaces.
        QVERIFY2(ChatTheme::panelBg() != darkPanel, "palette did not change between presets");
        QVERIFY2(widget.styleSheet().contains(ChatTheme::panelBg()), qPrintable(widget.styleSheet()));
        QVERIFY2(!widget.styleSheet().contains(darkPanel), qPrintable(widget.styleSheet()));

        QWidget *header = headerBar(widget);
        QVERIFY2(header, "header bar not found");
        QVERIFY2(header->styleSheet().contains(ChatTheme::panelBg()), qPrintable(header->styleSheet()));
        QVERIFY2(!header->styleSheet().contains(darkPanel), qPrintable(header->styleSheet()));

        QWidget *welcome = welcomeWidget(widget);
        QVERIFY2(welcome, "welcome widget not found");
        const auto chips = welcome->findChildren<QPushButton *>(u"welcomeChip"_s);
        QVERIFY2(!chips.isEmpty(), "welcome chips not found");
        for (const auto *chip : chips) {
            QVERIFY2(chip->styleSheet().contains(ChatTheme::surfaceBg()), qPrintable(chip->styleSheet()));
        }
    }

    void testColourOverrideReachesThePanel()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Settings settings = settingsWithPreset(u"kateai-dark"_s);
        settings.themeOverrides = QStringLiteral(R"({"panelBg": "#123456"})");
        applySettings(widget, settings);

        QVERIFY2(widget.styleSheet().contains(u"#123456"_s), qPrintable(widget.styleSheet()));
        QWidget *header = headerBar(widget);
        QVERIFY2(header, "header bar not found");
        QVERIFY2(header->styleSheet().contains(u"#123456"_s), qPrintable(header->styleSheet()));
    }

    void testUserCardIsRepaintedAfterAThemeSwitch()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        widget.agent()->setWorkspace(workspace.path());

        applySettings(widget, settingsWithPreset(u"kateai-dark"_s));
        widget.ask(u"hello theme"_s);

        // The turn fails without an API key, but the user card is already in
        // the transcript. Wait for the turn to end so the theme switch takes
        // the rebuild path rather than the mid-turn one.
        QElapsedTimer timer;
        timer.start();
        while (widget.agent()->isBusy() && timer.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QTest::qWait(10);
        }
        QVERIFY2(!widget.agent()->isBusy(), "turn never ended");

        QWidget *userCard = widget.findChild<QWidget *>(u"userCard"_s);
        QVERIFY2(userCard, "user card not found after asking");
        QVERIFY2(userCard->styleSheet().contains(ChatTheme::userCardBg()), qPrintable(userCard->styleSheet()));
        bool themedLabel = false;
        for (const auto *label : userCard->findChildren<QLabel *>()) {
            if (label->styleSheet().contains(ChatTheme::userCardText())) {
                themedLabel = true;
            }
        }
        QVERIFY2(themedLabel, "no user-card label carries the themed text colour");

        // Switch to the light preset: the rebuild re-creates the card with the
        // new palette, and the card text follows the card surface.
        applySettings(widget, settingsWithPreset(u"kateai-light"_s));

        userCard = widget.findChild<QWidget *>(u"userCard"_s);
        QVERIFY2(userCard, "user card not rebuilt after the theme switch");
        QVERIFY2(userCard->styleSheet().contains(ChatTheme::userCardBg()), qPrintable(userCard->styleSheet()));
        const auto labels = userCard->findChildren<QLabel *>();
        bool foundThemedLabel = false;
        for (const auto *label : labels) {
            if (label->styleSheet().contains(ChatTheme::userCardText())) {
                foundThemedLabel = true;
            }
        }
        QVERIFY2(foundThemedLabel, "no user-card label carries the themed text colour");
    }

    void testToolCardRestylesInPlace()
    {
        ToolCallWidget card(u"call-1"_s);
        card.setToolInfo(u"read_file"_s, u"src/main.cpp"_s, ToolRisk::Read);
        ToolResult result;
        result.toolCallId = u"call-1"_s;
        result.name = u"read_file"_s;
        result.output = u"int main() {}"_s;
        result.ok = true;
        card.setFinished(result);

        QTextBrowser *details = card.findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY(details);

        // A theme switch re-runs the card's restyle, but the card is
        // theme-independent by design: its surfaces, text and result view are
        // pinned to the fixed default palette, so a read_file card is the
        // same cool surface with the same text colours in dark and light
        // themes alike -- a transcript can be scanned by colour in any theme.
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
        card.applyStyle();
        QVERIFY2(card.styleSheet().contains(ChatTheme::toolCardBg(u"read_file"_s)),
                 qPrintable(card.styleSheet()));
        const QString darkCardSheet = card.styleSheet();
        QVERIFY2(details->styleSheet().contains(ChatTheme::ToolCard::codeBlockBg()), qPrintable(details->styleSheet()));
        const QString darkDetails = details->styleSheet();

        ChatTheme::loadTheme(settingsWithPreset(u"kateai-light"_s));
        card.applyStyle();
        // The whole card is theme-independent: neither its surfaces nor its
        // text colours may change with the theme.
        QVERIFY2(card.styleSheet() == darkCardSheet,
                 qPrintable(QStringLiteral("card surface changed with theme:\n%1\nvs\n%2")
                                .arg(darkCardSheet, card.styleSheet())));
        QVERIFY2(details->styleSheet() == darkDetails,
                 qPrintable(QStringLiteral("details surface changed with theme:\n%1\nvs\n%2")
                                .arg(darkDetails, details->styleSheet())));
        QVERIFY2(details->styleSheet().contains(ChatTheme::ToolCard::codeBlockBg()), qPrintable(details->styleSheet()));
    }
};

// Explicit main rather than QTEST_MAIN: the widget built here autosaves its
// transcript into SessionStore while it is destroyed, and the store override
// has to be in place before the first widget exists.
int main(int argc, char **argv)
{
    isolateSessionStore();

    QApplication app(argc, argv);
    TestThemeApply tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_themeapply.moc"
