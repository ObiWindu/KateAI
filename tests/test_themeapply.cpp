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

#include <KTextEditor/Editor>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPalette>
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
    // The "inherit from Kate editor" path derives its palette from the editor's
    // colour scheme. A light editor scheme must produce a light palette, and a
    // dark one a dark palette: this is what makes the panel follow KDE when the
    // system theme switches between dark and light.
    void testEditorThemeDerivesMatchingPalette()
    {
        KSyntaxHighlighting::Repository repo;

        const auto lightTheme = repo.defaultTheme(KSyntaxHighlighting::Repository::LightTheme);
        QVERIFY2(lightTheme.isValid(), "no light default theme available");
        const auto lightPal = ChatTheme::paletteForEditorTheme(lightTheme);
        const QColor lightBg(lightPal.panelBg);
        QVERIFY2(lightBg.isValid(), qPrintable(lightPal.panelBg));
        QVERIFY2(lightBg.lightnessF() > 0.5,
                 qPrintable(QStringLiteral("light editor theme produced a dark panel: %1").arg(lightPal.panelBg)));
        // Text must contrast with the panel.
        const QColor lightFg(lightPal.textPrimary);
        QVERIFY2(lightFg.isValid(), qPrintable(lightPal.textPrimary));
        QVERIFY2(lightFg.lightnessF() < 0.5,
                 qPrintable(QStringLiteral("light panel got light text: %1 on %2")
                                .arg(lightPal.textPrimary, lightPal.panelBg)));

        const auto darkTheme = repo.defaultTheme(KSyntaxHighlighting::Repository::DarkTheme);
        QVERIFY2(darkTheme.isValid(), "no dark default theme available");
        const auto darkPal = ChatTheme::paletteForEditorTheme(darkTheme);
        const QColor darkBg(darkPal.panelBg);
        QVERIFY2(darkBg.isValid(), qPrintable(darkPal.panelBg));
        QVERIFY2(darkBg.lightnessF() < 0.5,
                 qPrintable(QStringLiteral("dark editor theme produced a light panel: %1").arg(darkPal.panelBg)));
        // Text must contrast with the panel.
        const QColor darkFg(darkPal.textPrimary);
        QVERIFY2(darkFg.isValid(), qPrintable(darkPal.textPrimary));
        QVERIFY2(darkFg.lightnessF() > 0.5,
                 qPrintable(QStringLiteral("dark panel got dark text: %1 on %2")
                                .arg(darkPal.textPrimary, darkPal.panelBg)));

        // The two palettes must differ: switching the editor scheme must move
        // every surface, not just the panel background.
        QVERIFY2(lightPal.panelBg != darkPal.panelBg, "light and dark editor themes gave the same panel");
        QVERIFY2(lightPal.textPrimary != darkPal.textPrimary, "light and dark editor themes gave the same text");
    }

    // A light editor theme must produce status colours that contrast with a
    // light panel: the dark defaults (light teal, yellow, pink) are washed out
    // or invisible on light surfaces, so the derivation darkens them.
    void testEditorThemeLightStatusColorsContrast()
    {
        KSyntaxHighlighting::Repository repo;
        const auto lightTheme = repo.defaultTheme(KSyntaxHighlighting::Repository::LightTheme);
        QVERIFY2(lightTheme.isValid(), "no light default theme available");
        const auto pal = ChatTheme::paletteForEditorTheme(lightTheme);

        // Each status colour must be darker than the panel background so it
        // stays readable on a light surface.
        const QColor panel(pal.panelBg);
        const QColor success(pal.success);
        const QColor warning(pal.warning);
        const QColor danger(pal.danger);
        QVERIFY2(success.isValid(), qPrintable(pal.success));
        QVERIFY2(warning.isValid(), qPrintable(pal.warning));
        QVERIFY2(danger.isValid(), qPrintable(pal.danger));
        QVERIFY2(success.lightnessF() < panel.lightnessF(),
                 qPrintable(QStringLiteral("light theme success %1 is not darker than panel %2")
                                .arg(pal.success, pal.panelBg)));
        QVERIFY2(warning.lightnessF() < panel.lightnessF(),
                 qPrintable(QStringLiteral("light theme warning %1 is not darker than panel %2")
                                .arg(pal.warning, pal.panelBg)));
        QVERIFY2(danger.lightnessF() < panel.lightnessF(),
                 qPrintable(QStringLiteral("light theme danger %1 is not darker than panel %2")
                                .arg(pal.danger, pal.panelBg)));
    }

    // The accent drives buttons, focus rings, selected items and pills, so an
    // accent inherited from an editor theme must stay visible against the
    // panel it is painted on. The dark selection washes many editor themes
    // ship (ayu Dark's #273747 on #0a0e14) must not reach the palette
    // unvalidated, and no accent may end up a twin of the danger colour.
    void testEditorThemeAccentStaysVisible()
    {
        KSyntaxHighlighting::Repository repo;
        QVERIFY2(repo.themes().size() > 0, "no syntax themes available");
        for (const auto &theme : repo.themes()) {
            const auto pal = ChatTheme::paletteForEditorTheme(theme);
            const QColor accent(pal.accent);
            const QColor panel(pal.panelBg);
            const QColor danger(pal.danger);
            QVERIFY2(accent.isValid(),
                     qPrintable(QStringLiteral("%1: invalid accent").arg(theme.name())));
            // A clear lightness step from the panel: an accent within a small
            // step of the panel is invisible no matter how saturated it is.
            QVERIFY2(qAbs(accent.lightnessF() - panel.lightnessF()) >= 0.2,
                    qPrintable(QStringLiteral("%1: accent %2 vanishes on panel %3")
                                   .arg(theme.name(), pal.accent, pal.panelBg)));
            // A visible hue, or one of the tuned blues the derivation falls
            // back to when the theme offers no usable colour.
            const bool defaultBlue = accent == QColor(u"#3d7eff"_s) || accent == QColor(u"#2563eb"_s);
            QVERIFY2(accent.saturationF() >= 0.25 || defaultBlue,
                     qPrintable(QStringLiteral("%1: unsaturated accent %2").arg(theme.name(), pal.accent)));
            // Not the danger colour in disguise: approval buttons and error
            // states must not share a look.
            int hueDelta = qAbs(accent.hue() - danger.hue());
            if (hueDelta > 180) {
                hueDelta = 360 - hueDelta;
            }
            QVERIFY2(accent.hue() < 0 || danger.hue() < 0 || hueDelta >= 15
                         || qAbs(accent.lightnessF() - danger.lightnessF()) >= 0.2,
                     qPrintable(QStringLiteral("%1: accent %2 collides with danger %3")
                                   .arg(theme.name(), pal.accent, pal.danger)));
        }
    }

    // A light accent chip on a dark panel (an accent inherited from a bright
    // editor keyword colour, like Tokyo Night's #bb9af7) must carry dark
    // text: the light primary text of a dark panel would vanish on it.
    void testTextOnLightAccent()
    {
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
        const QColor onLightAccent(ChatTheme::textOn(u"#bb9af7"_s));
        QVERIFY2(onLightAccent.lightnessF() < 0.3,
                 qPrintable(QStringLiteral("text on a light accent is not dark: %1")
                                .arg(onLightAccent.name())));
        const QColor onPanel(ChatTheme::textOn(ChatTheme::panelBg()));
        QCOMPARE(onPanel, QColor(u"#ffffff"_s));

        // On a light panel the primary text is itself dark, so light
        // surfaces keep the primary text colour.
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-light"_s));
        const QColor onCard(ChatTheme::textOn(ChatTheme::cardBg()));
        QVERIFY2(onCard.lightnessF() < 0.5,
                 qPrintable(QStringLiteral("light panel text on a card is not dark: %1").arg(onCard.name())));

        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
    }

    // Pills built on a light accent (inherited from a bright editor keyword
    // colour) must use the accent itself as text on its own wash, and dim
    // towards the panel in the pulse phase -- tinting a light accent towards
    // white would leave light text on a light wash.
    void testPillsWithLightAccent()
    {
        Settings settings = settingsWithPreset(u"kateai-dark"_s);
        settings.themeOverrides = uR"({"accent": "#bb9af7"})"_s;
        ChatTheme::loadTheme(settings);

        const QString bright = ChatTheme::thinkingPill();
        QVERIFY2(bright.contains(u"color: #bb9af7"_s),
                 qPrintable(QStringLiteral("bright pill does not carry the light accent as text:\n%1").arg(bright)));

        const QString dim = ChatTheme::thinkingPillDim();
        const QString expectedDimText = ChatTheme::mixColors(ChatTheme::accent(), ChatTheme::panelBg(), 25);
        QVERIFY2(dim.contains(expectedDimText),
                 qPrintable(QStringLiteral("dim pill text is not the accent dimmed towards the panel:\n%1\nexpected %2")
                                .arg(dim, expectedDimText)));

        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
    }

    // The thinking and working pills must derive their text colour from the
    // palette, not carry a hardcoded light tint that is invisible on a light
    // panel. In a light theme the text colour is the accent/warning itself
    // (dark enough to read); in a dark theme it is a light tint of it.
    void testPillsAdaptToLightTheme()
    {
        // Dark theme: the pill text is a light tint of the accent.
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
        const QString darkThinking = ChatTheme::thinkingPill();
        const QString darkWorking = ChatTheme::workingPill();
        // The hardcoded light tints must not appear: the text colour is derived.
        QVERIFY2(!darkThinking.contains(u"#b4d0ff"_s),
                 qPrintable(QStringLiteral("dark thinking pill carries a hardcoded light tint:\n%1").arg(darkThinking)));
        QVERIFY2(!darkWorking.contains(u"#f3d48a"_s),
                 qPrintable(QStringLiteral("dark working pill carries a hardcoded light tint:\n%1").arg(darkWorking)));

        // Light theme: the pill text must not be a light tint either; it is
        // the accent/warning colour itself, which is dark on a light panel.
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-light"_s));
        const QString lightThinking = ChatTheme::thinkingPill();
        const QString lightWorking = ChatTheme::workingPill();
        QVERIFY2(!lightThinking.contains(u"#b4d0ff"_s),
                 qPrintable(QStringLiteral("light thinking pill carries a hardcoded light tint:\n%1").arg(lightThinking)));
        QVERIFY2(!lightWorking.contains(u"#f3d48a"_s),
                 qPrintable(QStringLiteral("light working pill carries a hardcoded light tint:\n%1").arg(lightWorking)));
        // The light pill must carry the light theme's accent colour as its text.
        QVERIFY2(lightThinking.contains(ChatTheme::accent()),
                 qPrintable(QStringLiteral("light thinking pill does not use the theme accent:\n%1").arg(lightThinking)));
        QVERIFY2(lightWorking.contains(ChatTheme::warning()),
                 qPrintable(QStringLiteral("light working pill does not use the theme warning:\n%1").arg(lightWorking)));

        // Restore the default dark theme so later tests start from a known state.
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
    }

    // The KateAI window (tool view, header bar, composer strip) follows the
    // window colour theme: the chrome colours are derived from the application
    // palette's window roles, with borders and hover stepping away from the
    // window background the way the chat surfaces step away from the panel.
    void testWindowChromeFollowsWindowPalette()
    {
        QPalette windowPalette;
        windowPalette.setColor(QPalette::Window, QColor(u"#202124"_s));
        windowPalette.setColor(QPalette::WindowText, QColor(u"#e8eaed"_s));

        const auto base = ChatTheme::paletteForPreset(u"kateai-dark"_s);
        const auto darkPal = ChatTheme::applyWindowChrome(base, windowPalette);
        QCOMPARE(darkPal.windowBg, u"#202124"_s);
        QCOMPARE(darkPal.windowText, u"#e8eaed"_s);
        QCOMPARE(darkPal.windowTextMuted, ChatTheme::mixColors(u"#e8eaed"_s, u"#202124"_s, 45));
        // A dark window theme steps towards white for its chrome lines.
        QCOMPARE(darkPal.windowBorder, ChatTheme::mixColors(u"#ffffff"_s, u"#202124"_s, 18));
        QCOMPARE(darkPal.windowHover, ChatTheme::mixColors(u"#ffffff"_s, u"#202124"_s, 16));
        // The chat content colours are untouched by the window chrome.
        QCOMPARE(darkPal.panelBg, base.panelBg);
        QCOMPARE(darkPal.textPrimary, base.textPrimary);
        QCOMPARE(darkPal.accent, base.accent);

        // A light window theme steps towards black instead.
        windowPalette.setColor(QPalette::Window, QColor(u"#f1f3f4"_s));
        windowPalette.setColor(QPalette::WindowText, QColor(u"#1f1f1f"_s));
        const auto lightPal = ChatTheme::applyWindowChrome(base, windowPalette);
        QCOMPARE(lightPal.windowBg, u"#f1f3f4"_s);
        QCOMPARE(lightPal.windowText, u"#1f1f1f"_s);
        QCOMPARE(lightPal.windowBorder, ChatTheme::mixColors(u"#000000"_s, u"#f1f3f4"_s, 16));
        QCOMPARE(lightPal.windowHover, ChatTheme::mixColors(u"#000000"_s, u"#f1f3f4"_s, 10));
    }

    // A built-in preset is self-contained: the window chrome stays on the
    // preset's own colours, so the panel reads as one surface and the preset
    // look is unchanged.
    void testPresetKeepsWindowChromeOnPanelColors()
    {
        const auto pal = ChatTheme::paletteForSettings(settingsWithPreset(u"kateai-dark"_s));
        QCOMPARE(pal.windowBg, pal.panelBg);
        QCOMPARE(pal.windowText, pal.textPrimary);
        QCOMPARE(pal.windowTextMuted, pal.textMuted);
        QCOMPARE(pal.windowBorder, pal.border);
        QCOMPARE(pal.windowHover, pal.hoverBg);

        const auto light = ChatTheme::paletteForSettings(settingsWithPreset(u"kateai-light"_s));
        QCOMPARE(light.windowBg, light.panelBg);
        QCOMPARE(light.windowText, light.textPrimary);
        QCOMPARE(light.windowBorder, light.border);
        QCOMPARE(light.windowHover, light.hoverBg);
    }

    // In inherit mode both the window chrome and the chat content follow the
    // Kate editor window theme (the editor colour scheme): the chrome mirrors
    // the panel colours, so the whole panel reads as one surface in the
    // editor's colours.
    void testEditorSourceInheritsEditorThemeForWindowAndChat()
    {
        Settings settings = settingsWithPreset(u"kateai-dark"_s);
        settings.themeSource = u"editor"_s;
        const auto pal = ChatTheme::paletteForSettings(settings);
        // The chat content follows the editor colour scheme.
        auto *editor = KTextEditor::Editor::instance();
        QVERIFY2(editor, "no editor instance available");
        const auto expectedContent = ChatTheme::paletteForEditorTheme(editor->theme());
        QCOMPARE(pal.panelBg, expectedContent.panelBg);
        QCOMPARE(pal.textPrimary, expectedContent.textPrimary);
        QCOMPARE(pal.accent, expectedContent.accent);
        // The window chrome mirrors the panel colours, so the KateAI window
        // and the chat inside it both inherit from the editor theme.
        QCOMPARE(pal.windowBg, pal.panelBg);
        QCOMPARE(pal.windowText, pal.textPrimary);
        QCOMPARE(pal.windowTextMuted, pal.textMuted);
        QCOMPARE(pal.windowBorder, pal.border);
        QCOMPARE(pal.windowHover, pal.hoverBg);
    }

    // The chrome stylesheets read the window chrome colours, so installing a
    // palette moves the header bar, the header title, the header icon buttons,
    // the status strip hint, the tool view and the intent dock with it.
    void testWindowChromeStylesFollowPalette()
    {
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
        QVERIFY2(ChatTheme::headerBar().contains(ChatTheme::windowBg()), qPrintable(ChatTheme::headerBar()));
        QVERIFY2(ChatTheme::headerBar().contains(ChatTheme::windowBorder()), qPrintable(ChatTheme::headerBar()));
        QVERIFY2(ChatTheme::headerTitle().contains(ChatTheme::windowTextMuted()), qPrintable(ChatTheme::headerTitle()));
        QVERIFY2(ChatTheme::headerIconButton().contains(ChatTheme::windowTextMuted()),
                 qPrintable(ChatTheme::headerIconButton()));
        QVERIFY2(ChatTheme::headerIconButton().contains(ChatTheme::windowHover()),
                 qPrintable(ChatTheme::headerIconButton()));
        QVERIFY2(ChatTheme::headerIconButton().contains(ChatTheme::windowBorder()),
                 qPrintable(ChatTheme::headerIconButton()));
        QVERIFY2(ChatTheme::windowHintLabel().contains(ChatTheme::windowTextMuted()),
                 qPrintable(ChatTheme::windowHintLabel()));
        QVERIFY2(ChatTheme::windowChrome().contains(ChatTheme::windowBg()), qPrintable(ChatTheme::windowChrome()));
        QVERIFY2(ChatTheme::windowChrome().contains(ChatTheme::windowText()), qPrintable(ChatTheme::windowChrome()));
        QVERIFY2(ChatTheme::intentDock().contains(ChatTheme::windowBg()), qPrintable(ChatTheme::intentDock()));
    }

    // A window-chrome colour override reaches the window: the header bar and
    // the composer strip repaint with it, while the chat content keeps the
    // preset panel colour.
    void testWindowChromeOverrideReachesTheWindow()
    {
        ChatWidget widget;
        widget.resize(900, 700);
        widget.show();

        Settings settings = settingsWithPreset(u"kateai-dark"_s);
        settings.themeOverrides = QStringLiteral(R"({"windowBg": "#123456", "windowBorder": "#654321"})");
        applySettings(widget, settings);

        QCOMPARE(ChatTheme::windowBg(), u"#123456"_s);
        QWidget *header = headerBar(widget);
        QVERIFY2(header, "header bar not found");
        QVERIFY2(header->styleSheet().contains(u"#123456"_s), qPrintable(header->styleSheet()));
        QVERIFY2(header->styleSheet().contains(u"#654321"_s), qPrintable(header->styleSheet()));

        QWidget *composer = widget.findChild<QWidget *>(u"composerContainer"_s);
        QVERIFY2(composer, "composer container not found");
        QVERIFY2(composer->styleSheet().contains(u"#123456"_s), qPrintable(composer->styleSheet()));
        QVERIFY2(composer->styleSheet().contains(u"#654321"_s), qPrintable(composer->styleSheet()));

        // The chat content keeps the preset panel colour.
        QVERIFY2(widget.styleSheet().contains(ChatTheme::panelBg()), qPrintable(widget.styleSheet()));
        QVERIFY2(!widget.styleSheet().contains(u"#123456"_s), qPrintable(widget.styleSheet()));

        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
    }

    // An invalid editor theme falls back to the default dark palette rather
    // than producing a broken (all-black or all-white) palette.
    void testInvalidEditorThemeFallsBackToDefault()
    {
        const KSyntaxHighlighting::Theme invalid;
        const auto pal = ChatTheme::paletteForEditorTheme(invalid);
        const auto def = ChatTheme::paletteForPreset(u"kateai-dark"_s);
        QCOMPARE(pal.panelBg, def.panelBg);
        QCOMPARE(pal.textPrimary, def.textPrimary);
    }

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
