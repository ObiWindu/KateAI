/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Pins the tool card's icons, widgets and colours to the main-branch design.
 *
 * The chat UI rework made the panel themeable, and the tool card deliberately
 * opted out: its look is the one thing that must stay identical to main, so a
 * transcript can be scanned by colour no matter which palette is installed.
 * This test holds the card to that promise. Every expected value below is a
 * literal from main's palette or main's icon table -- not a value read back
 * from the code under test -- so any drift away from main's design fails here
 * instead of shipping silently.
 */

#include "chattheme.h"
#include "toolcallwidget.h"
#include "types.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

// Main branch's palette, as literal constants. The card's pinned ToolCard
// namespace must resolve to exactly these values, because these are the
// colours main's cards are painted with.
constexpr auto kMainSuccess = "#4ec9a0";
constexpr auto kMainDanger = "#f26d6d";
constexpr auto kMainWarning = "#e2b341";
constexpr auto kMainTextPrimary = "#d4d4d6";
constexpr auto kMainTextMuted = "#8b8b92";
constexpr auto kMainAccent = "#3d7eff";
constexpr auto kMainAccentHover = "#5590ff";
constexpr auto kMainCardBg = "#28282b";
constexpr auto kMainHoverBg = "#323236";
constexpr auto kMainCodeBlockBg = "#151517";
constexpr auto kMainIntentRowBg = "#3a3020";
// The terminal surface a command's output sits on: one step darker than the
// inset used everywhere else, so running and reading are told apart.
constexpr auto kMainTerminalBg = "#101014";

// Main branch's icon table: one glyph per tool family, with ACP and MCP names
// mapped onto the same icons and a gear for everything unknown.
struct IconEntry {
    const char *tool;
    QStringView glyph;
};
const IconEntry kMainIcons[] = {
    {"read_file", u"\u{1F4C4}"},
    {"fs/read_text_file", u"\u{1F4C4}"},
    {"write_file", u"\u{1F4DD}"},
    {"fs/write_text_file", u"\u{1F4DD}"},
    {"edit_file", u"\u{270F}"},
    {"multi_edit_file", u"\u{270F}"},
    {"multi_replace_file_content", u"\u{270F}"},
    {"list_dir", u"\u{1F4C1}"},
    {"grep", u"\u{1F50D}"},
    {"glob", u"\u{1F50E}"},
    {"bash", u"\u{26A1}"},
    {"web_search", u"\u{1F310}"},
    {"web_fetch", u"\u{1F4D6}"},
    {"query_project_graph", u"\u{1F578}"},
    {"write_plan_file", u"\u{1F4CB}"},
    {"suggest", u"\u{1F4A1}"},
    {"question", u"\u{2753}"},
    {"goal_report", u"\u{1F3AF}"},
    {"new_task", u"\u{1F9E9}"},
    {"mcp__weather", u"\u{1F50C}"},
    {"terminal/exec", u"\u{1F5B3}"},
    {"terminal", u"\u{1F5B3}"},
    {"totally_unknown_tool", u"\u{2699}"},
};

Settings settingsWithPreset(const QString &preset)
{
    Settings settings;
    settings.themeSource = u"preset"_s;
    settings.themePreset = preset;
    return settings;
}

QString sampleDiff()
{
    return QStringLiteral("--- a/src/main.cpp\n"
                           "+++ b/src/main.cpp\n"
                           "@@ -1,2 +1,2 @@\n"
                           "-int old = 1;\n"
                           "+int old = 2;\n"
                           " context line\n");
}

} // namespace

class TestToolCardParity : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The card's pinned palette is main's palette, colour for colour.
    void cardPaletteIsMainBranchPalette()
    {
        QCOMPARE(ChatTheme::ToolCard::success(), QString::fromLatin1(kMainSuccess));
        QCOMPARE(ChatTheme::ToolCard::danger(), QString::fromLatin1(kMainDanger));
        QCOMPARE(ChatTheme::ToolCard::warning(), QString::fromLatin1(kMainWarning));
        QCOMPARE(ChatTheme::ToolCard::textPrimary(), QString::fromLatin1(kMainTextPrimary));
        QCOMPARE(ChatTheme::ToolCard::textMuted(), QString::fromLatin1(kMainTextMuted));
        QCOMPARE(ChatTheme::ToolCard::accent(), QString::fromLatin1(kMainAccent));
        QCOMPARE(ChatTheme::ToolCard::accentHover(), QString::fromLatin1(kMainAccentHover));
        QCOMPARE(ChatTheme::ToolCard::cardBg(), QString::fromLatin1(kMainCardBg));
        QCOMPARE(ChatTheme::ToolCard::hoverBg(), QString::fromLatin1(kMainHoverBg));
        QCOMPARE(ChatTheme::ToolCard::codeBlockBg(), QString::fromLatin1(kMainCodeBlockBg));
    }

    // --- Widgets ---------------------------------------------------------------

    void widgetTreeMatchesMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget card(u"call-widgets"_s);
        card.setToolInfo(u"bash"_s, u"echo hi"_s, ToolRisk::Execute);
        layout->addWidget(&card);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // Header box, with the head surface painting its own background.
        QWidget *header = card.findChild<QWidget *>(u"toolHeader"_s);
        QVERIFY2(header, "toolHeader missing");
        QVERIFY2(header->testAttribute(Qt::WA_StyledBackground),
                 "toolHeader must paint its own stylesheet background");

        // Header row, main's order: icon, title, status, diff stat, duration,
        // expand button.
        QLabel *icon = card.findChild<QLabel *>(u"toolIcon"_s);
        QVERIFY2(icon, "toolIcon missing");
        QCOMPARE(icon->width(), 16);
        QCOMPARE(icon->height(), 16);
        QLabel *title = card.findChild<QLabel *>(u"toolTitle"_s);
        QVERIFY2(title, "toolTitle missing");
        QVERIFY2(title->wordWrap(), "toolTitle must wrap like main's");
        const auto headerLabels = header->findChildren<QLabel *>();
        QVERIFY2(headerLabels.contains(icon) && headerLabels.contains(title),
                 "icon and title must sit in the header row");
        QLabel *status = nullptr;
        for (auto *label : headerLabels) {
            if (label != icon && label != title && label->objectName().isEmpty()
                && label->maximumWidth() == 20 && label->minimumWidth() == 20) {
                status = label;
            }
        }
        QVERIFY2(status, "status glyph label missing from the header row");

        // Diff stat and duration start hidden: a read-only, instant call does
        // not carry a meaningless stat or timer.
        QLabel *diffStat = card.findChild<QLabel *>(u"toolDiffStat"_s);
        QVERIFY2(diffStat, "toolDiffStat missing");
        QVERIFY2(diffStat->isHidden(), "diff stat must start hidden");
        QVERIFY2(diffStat->text().isEmpty(), "diff stat must start empty");

        // Expand chevron, 20x20 flat button.
        QPushButton *expand = nullptr;
        const auto buttons = card.findChildren<QPushButton *>();
        for (auto *button : buttons) {
            if (button->width() == 20 && button->height() == 20 && button->isFlat()) {
                expand = button;
            }
        }
        QVERIFY2(expand, "expand button missing");

        // Body: caption, diff surface, result surface -- in that order.
        QWidget *container = nullptr;
        for (auto *child : card.findChildren<QWidget *>()) {
            if (child->objectName().isEmpty() && child->maximumHeight() == 0) {
                container = child;
            }
        }
        QVERIFY2(container, "details container missing");
        QLabel *caption = card.findChild<QLabel *>(u"toolViewCaption"_s);
        QVERIFY2(caption, "toolViewCaption missing: the body must name what it holds");
        QVERIFY2(container->isAncestorOf(caption), "caption must sit in the details container");
        // Main names the body as soon as the tool is known, even before
        // anything has arrived -- the empty container keeps it off the screen
        // -- so a bash card's caption reads "Output" from setToolInfo on.
        QVERIFY2(!caption->isHidden(), "caption must be shown once the tool is known");
        QVERIFY2(caption->text() == u"Output"_s,
                 qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));
        QTextBrowser *diffSurface = card.findChild<QTextBrowser *>(u"toolDiff"_s);
        QVERIFY2(diffSurface, "toolDiff surface missing: edits need their own colour-coded surface");
        QVERIFY2(container->isAncestorOf(diffSurface), "diff surface must sit in the details container");
        QVERIFY2(diffSurface->isHidden(), "diff surface must start hidden");
        QTextBrowser *details = card.findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY2(details, "toolDetails missing");
        QVERIFY2(container->isAncestorOf(details), "result surface must sit in the details container");

        // Approval strip: hint, subject, then Allow / Always / Deny.
        QWidget *approval = card.findChild<QWidget *>(u"approvalRow"_s);
        QVERIFY2(approval, "approvalRow missing");
        QVERIFY2(approval->isHidden(), "approval strip must start hidden");
        QLabel *hint = nullptr;
        const auto approvalLabels = approval->findChildren<QLabel *>();
        for (auto *label : approvalLabels) {
            if (label->text() == u"Needs approval"_s) {
                hint = label;
            }
        }
        QVERIFY2(hint, "approval hint missing");
        QString buttonText;
        for (auto *button : approval->findChildren<QPushButton *>()) {
            buttonText += button->text();
        }
        QVERIFY2(buttonText.contains(u"Allow"_s), "Allow button missing");
        QVERIFY2(buttonText.contains(u"Always"_s), "Always button missing");
        QVERIFY2(buttonText.contains(u"Deny"_s), "Deny button missing");
    }

    // --- Icons -------------------------------------------------------------------

    void iconsMatchMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);

        for (const IconEntry &entry : kMainIcons) {
            ToolCallWidget card(u"call-icon"_s, &host);
            card.setToolInfo(QString::fromLatin1(entry.tool), u"args"_s, ToolRisk::Read);
            layout->addWidget(&card);

            QLabel *icon = card.findChild<QLabel *>(u"toolIcon"_s);
            QVERIFY2(icon, "toolIcon missing");
            QVERIFY2(icon->text() == entry.glyph.toString(),
                     qPrintable(QStringLiteral("icon for %1 is %2, main's is %3")
                                    .arg(QString::fromLatin1(entry.tool), icon->text().toHtmlEscaped(),
                                         entry.glyph.toString().toHtmlEscaped())));
            layout->removeWidget(&card);
        }
    }

    // --- Colours -----------------------------------------------------------------

    void railAndHeaderMatchMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        const char *tools[] = {"read_file", "write_file", "edit_file", "bash", "grep", "question"};
        for (const char *tool : tools) {
            ToolCallWidget card(u"call-colour"_s, &host);
            card.setToolInfo(QString::fromLatin1(tool), u"args"_s, ToolRisk::Read);
            layout->addWidget(&card);

            // Card body: the tool's hue tinted into main's tool surface, with
            // the tool's own colour on the rail.
            const QString expectedBg = ChatTheme::toolCardBg(QString::fromLatin1(tool));
            const QString expectedRail = ChatTheme::toolColor(QString::fromLatin1(tool));
            QVERIFY2(card.styleSheet().contains(expectedBg),
                     qPrintable(card.styleSheet()));
            QVERIFY2(card.styleSheet().contains(expectedRail),
                     qPrintable(card.styleSheet()));
            QVERIFY2(card.styleSheet().contains(u"border-left: 2px solid"_s),
                     "main's card carries a 2px accent rail on the left");

            // Head box: main's per-tool header sheet, unmodified.
            QWidget *header = card.findChild<QWidget *>(u"toolHeader"_s);
            QVERIFY2(header->styleSheet() == ChatTheme::toolHeader(QString::fromLatin1(tool)),
                     qPrintable(header->styleSheet()));

            // Title text carries main's primary colour.
            QLabel *title = card.findChild<QLabel *>(u"toolTitle"_s);
            QVERIFY2(title->styleSheet().contains(QString::fromLatin1(kMainTextPrimary)),
                     qPrintable(title->styleSheet()));

            layout->removeWidget(&card);
        }
    }

    void statusAndChromeColoursMatchMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        // A finished call: green check on success, red cross on failure --
        // main's exact status colours.
        {
            ToolCallWidget card(u"call-ok"_s, &host);
            card.setToolInfo(u"read_file"_s, u"src/main.cpp"_s, ToolRisk::Read);
            layout->addWidget(&card);
            ToolResult ok;
            ok.name = u"read_file"_s;
            ok.ok = true;
            ok.output = u"contents"_s;
            card.setFinished(ok);
            QTest::qWait(20);
            QLabel *status = nullptr;
            for (auto *label : card.findChildren<QLabel *>()) {
                if (label->styleSheet().contains(QString::fromLatin1(kMainSuccess))
                    && label->text() == u"\u2713"_s) {
                    status = label;
                }
            }
            QVERIFY2(status, "a succeeded call must show main's green check");
            QVERIFY2(status->styleSheet().contains(QString::fromLatin1(kMainSuccess)),
                     qPrintable(status->styleSheet()));
            layout->removeWidget(&card);
        }
        {
            ToolCallWidget card(u"call-fail"_s, &host);
            card.setToolInfo(u"read_file"_s, u"src/main.cpp"_s, ToolRisk::Read);
            layout->addWidget(&card);
            ToolResult failed;
            failed.name = u"read_file"_s;
            failed.ok = false;
            failed.output = u"no such file"_s;
            card.setFinished(failed);
            QTest::qWait(20);
            // Failure overrides everything: the rail and the glyph both go
            // main's red.
            QVERIFY2(card.styleSheet().contains(QString::fromLatin1(kMainDanger)),
                     qPrintable(card.styleSheet()));
            QLabel *status = nullptr;
            for (auto *label : card.findChildren<QLabel *>()) {
                if (label->styleSheet().contains(QString::fromLatin1(kMainDanger))
                    && label->text() == u"\u2717"_s) {
                    status = label;
                }
            }
            QVERIFY2(status, "a failed call must show main's red cross");
            layout->removeWidget(&card);
        }

        // Header chrome: diff stat in main's green, duration and chevron in
        // main's muted colour.
        {
            ToolCallWidget card(u"call-chrome"_s, &host);
            card.setToolInfo(u"edit_file"_s, u"src/main.cpp"_s, ToolRisk::Write);
            card.setDescribeDiff(sampleDiff());
            layout->addWidget(&card);
            host.show();
            QVERIFY(QTest::qWaitForWindowExposed(&host));
            QTest::qWait(20);

            QLabel *diffStat = card.findChild<QLabel *>(u"toolDiffStat"_s);
            QVERIFY2(diffStat->isVisible(), "diff stat must appear once a diff is attached");
            // The stat uses rich text: additions in green, deletions in red.
            const QString diffText = diffStat->text();
            QVERIFY2(diffText.contains(u"+1"_s),
                     qPrintable(QStringLiteral("diff stat reads %1").arg(diffText)));
            QVERIFY2(diffText.contains(QStringLiteral("\u22121")),
                     qPrintable(QStringLiteral("diff stat reads %1").arg(diffText)));
            QVERIFY2(diffText.contains(QString::fromLatin1(kMainSuccess)),
                     qPrintable(QStringLiteral("added count must be green: %1").arg(diffText)));
            QVERIFY2(diffText.contains(QString::fromLatin1(kMainDanger)),
                     qPrintable(QStringLiteral("removed count must be red: %1").arg(diffText)));
            QVERIFY2(diffStat->styleSheet().contains(u"monospace"_s),
                     "main's diff stat is monospace");

            QLabel *caption = card.findChild<QLabel *>(u"toolViewCaption"_s);
            QVERIFY2(caption->text() == u"Changes"_s,
                     qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));
            QVERIFY2(caption->styleSheet().contains(QString::fromLatin1(kMainTextMuted)),
                     qPrintable(caption->styleSheet()));

            QPushButton *expand = nullptr;
            for (auto *button : card.findChildren<QPushButton *>()) {
                if (button->styleSheet().contains(QString::fromLatin1(kMainTextMuted))) {
                    expand = button;
                }
            }
            QVERIFY2(expand, "expand chevron missing or unstyled");
        }
    }

    void resultSurfacesMatchMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        // A file read sits on main's code inset, white on near-black.
        {
            ToolCallWidget card(u"call-read"_s, &host);
            card.setToolInfo(u"read_file"_s, u"src/main.cpp"_s, ToolRisk::Read);
            layout->addWidget(&card);
            ToolResult ok;
            ok.name = u"read_file"_s;
            ok.ok = true;
            ok.output = u"int main() {}"_s;
            card.setFinished(ok);
            host.show();
            QVERIFY(QTest::qWaitForWindowExposed(&host));
            QTest::qWait(20);

            QTextBrowser *details = card.findChild<QTextBrowser *>(u"toolDetails"_s);
            QVERIFY2(details->styleSheet().contains(QString::fromLatin1(kMainCodeBlockBg)),
                     qPrintable(details->styleSheet()));
            QVERIFY2(details->styleSheet().contains(u"color: #ffffff"_s),
                     qPrintable(details->styleSheet()));
            QLabel *caption = card.findChild<QLabel *>(u"toolViewCaption"_s);
            QVERIFY2(caption->text() == u"File contents"_s,
                     qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));
            layout->removeWidget(&card);
        }

        // A command's output sits on main's terminal surface, one step darker.
        {
            ToolCallWidget card(u"call-bash"_s, &host);
            card.setToolInfo(u"bash"_s, u"echo hi"_s, ToolRisk::Execute);
            layout->addWidget(&card);
            ToolResult ok;
            ok.name = u"bash"_s;
            ok.ok = true;
            ok.output = u"hi"_s;
            card.setFinished(ok);
            QTest::qWait(20);

            QTextBrowser *details = card.findChild<QTextBrowser *>(u"toolDetails"_s);
            QVERIFY2(details->styleSheet().contains(QString::fromLatin1(kMainTerminalBg)),
                     qPrintable(details->styleSheet()));
            QVERIFY2(details->styleSheet().contains(u"color: #ffffff"_s),
                     qPrintable(details->styleSheet()));
            QLabel *caption = card.findChild<QLabel *>(u"toolViewCaption"_s);
            QVERIFY2(caption->text() == u"Output"_s,
                     qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));
        }
    }

    void diffSurfaceMatchesMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget card(u"call-diff"_s, &host);
        card.setToolInfo(u"edit_file"_s, u"src/main.cpp"_s, ToolRisk::Write);
        layout->addWidget(&card);
        card.setDescribeDiff(sampleDiff());
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(20);

        QVERIFY2(card.isExpanded(), "an attached diff must open the card");
        QTextBrowser *diffSurface = card.findChild<QTextBrowser *>(u"toolDiff"_s);
        QVERIFY2(diffSurface->isVisible(), "the diff surface must be shown once a diff is attached");
        QVERIFY2(diffSurface->toPlainText().contains(u"-int old = 1;"_s),
                 qPrintable(diffSurface->toPlainText()));
        QVERIFY2(diffSurface->toPlainText().contains(u"+int old = 2;"_s),
                 qPrintable(diffSurface->toPlainText()));

        // Each line carries its own background, main's tint of green and red
        // over the code inset.
        const QString addBg = ChatTheme::mixColors(QString::fromLatin1(kMainSuccess),
                                                   QString::fromLatin1(kMainCodeBlockBg), 24);
        const QString delBg = ChatTheme::mixColors(QString::fromLatin1(kMainDanger),
                                                   QString::fromLatin1(kMainCodeBlockBg), 24);
        const QString html = diffSurface->toHtml();
        QVERIFY2(html.contains(addBg, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("diff is missing main's added-line background %1:\n%2")
                                .arg(addBg, html.left(2000))));
        QVERIFY2(html.contains(delBg, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("diff is missing main's removed-line background %1:\n%2")
                                .arg(delBg, html.left(2000))));
        QVERIFY2(diffSurface->styleSheet().contains(QString::fromLatin1(kMainCodeBlockBg)),
                 qPrintable(diffSurface->styleSheet()));
    }

    void runningInputMatchesMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget card(u"call-input"_s, &host);
        card.setToolInfo(u"bash"_s, u"longjob.sh"_s, ToolRisk::Execute);
        layout->addWidget(&card);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // A card in flight says what it is working on: the arguments are the
        // body, the card opens itself, and the caption says "Input".
        card.setRunningInput(u"--flag one\n--flag two"_s);
        QTest::qWait(20);
        QVERIFY2(card.isExpanded(), "running input must open the card");
        QTextBrowser *details = card.findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY2(details->toPlainText().contains(u"--flag one"_s),
                 qPrintable(details->toPlainText()));
        QLabel *caption = card.findChild<QLabel *>(u"toolViewCaption"_s);
        QVERIFY2(caption->text() == u"Input"_s,
                 qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));

        // A live stream replaces the arguments. Main does not re-run the
        // caption when a chunk arrives -- it is re-derived when the call
        // finishes -- so mid-stream the caption still names the body "Input",
        // exactly like main.
        card.setPartialOutput(u"building\n"_s);
        QTest::qWait(20);
        QVERIFY2(details->toPlainText().contains(u"building"_s),
                 qPrintable(details->toPlainText()));
        QVERIFY2(caption->text() == u"Input"_s,
                 qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));

        // Once the call has finished, the caption follows the body.
        ToolResult done;
        done.name = u"bash"_s;
        done.ok = true;
        done.output = u"building"_s;
        card.setFinished(done);
        QTest::qWait(20);
        QVERIFY2(caption->text() == u"Output"_s,
                 qPrintable(QStringLiteral("caption reads %1").arg(caption->text())));

        // The arguments are capped like main caps them: twelve lines, with the
        // tail marked rather than dropped silently.
        ToolCallWidget longCard(u"call-longinput"_s, &host);
        longCard.setToolInfo(u"bash"_s, u"longjob.sh"_s, ToolRisk::Execute);
        layout->addWidget(&longCard);
        QString manyLines;
        for (int i = 0; i < 40; ++i) {
            manyLines += u"line %1\n"_s.arg(i);
        }
        longCard.setRunningInput(manyLines);
        QTest::qWait(20);
        QTextBrowser *longDetails = longCard.findChild<QTextBrowser *>(u"toolDetails"_s);
        QVERIFY2(longDetails->toPlainText().contains(u"line 11"_s),
                 qPrintable(longDetails->toPlainText()));
        QVERIFY2(!longDetails->toPlainText().contains(u"line 13"_s),
                 qPrintable(longDetails->toPlainText()));
        QVERIFY2(longDetails->toPlainText().contains(u"more lines"_s),
                 qPrintable(longDetails->toPlainText()));
    }

    void approvalStripColoursMatchMainBranch()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget card(u"call-approval"_s, &host);
        card.setToolInfo(u"edit_file"_s, u"src/main.cpp"_s, ToolRisk::Write);
        layout->addWidget(&card);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        card.showApproval();
        QWidget *approval = card.findChild<QWidget *>(u"approvalRow"_s);
        QVERIFY2(approval->isVisible(), "showApproval must reveal the strip");

        // Main's warm, high-contrast strip.
        QVERIFY2(approval->styleSheet().contains(QString::fromLatin1(kMainIntentRowBg)),
                 qPrintable(approval->styleSheet()));
        QVERIFY2(approval->styleSheet().contains(QString::fromLatin1(kMainWarning)),
                 qPrintable(approval->styleSheet()));

        // Buttons: accent-filled Allow, outlined Always, red Deny -- main's
        // exact fills and borders.
        bool allow = false;
        bool always = false;
        bool deny = false;
        for (auto *button : approval->findChildren<QPushButton *>()) {
            const QString sheet = button->styleSheet();
            if (button->text() == u"Allow"_s) {
                allow = sheet.contains(QString::fromLatin1(kMainAccent))
                    && sheet.contains(QString::fromLatin1(kMainAccentHover));
            } else if (button->text() == u"Always"_s) {
                always = sheet.contains(QString::fromLatin1(kMainCardBg))
                    && sheet.contains(QString::fromLatin1(kMainTextPrimary))
                    && sheet.contains(QString::fromLatin1(kMainHoverBg));
            } else if (button->text() == u"Deny"_s) {
                deny = sheet.contains(QString::fromLatin1(kMainDanger));
            }
        }
        QVERIFY2(allow, "Allow button is not main's accent-filled button");
        QVERIFY2(always, "Always button is not main's outlined button");
        QVERIFY2(deny, "Deny button is not main's red button");

        // The hint carries main's warning colour, the subject main's text.
        bool hint = false;
        bool subject = false;
        for (auto *label : approval->findChildren<QLabel *>()) {
            if (label->text() == u"Needs approval"_s) {
                hint = label->styleSheet().contains(QString::fromLatin1(kMainWarning));
            } else {
                subject = label->styleSheet().contains(QString::fromLatin1(kMainTextPrimary));
            }
        }
        QVERIFY2(hint, "approval hint does not carry main's warning colour");
        QVERIFY2(subject, "approval subject does not carry main's text colour");
    }

    // The card is main's card in every theme: install the light preset and
    // nothing about the card may move. This is the "still not the ones from
    // main branch" guard -- a retheme that leaks the palette into the card
    // fails here.
    void cardStaysMainBranchColouredInEveryTheme()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget card(u"call-theme"_s, &host);
        card.setToolInfo(u"read_file"_s, u"src/main.cpp"_s, ToolRisk::Read);
        ToolResult ok;
        ok.name = u"read_file"_s;
        ok.ok = true;
        ok.output = u"int main() {}"_s;
        card.setFinished(ok);
        layout->addWidget(&card);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(20);

        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
        card.applyStyle();
        const QString cardSheet = card.styleSheet();
        const QString headerSheet = card.findChild<QWidget *>(u"toolHeader"_s)->styleSheet();
        const QString detailsSheet = card.findChild<QTextBrowser *>(u"toolDetails"_s)->styleSheet();
        QVERIFY2(detailsSheet.contains(QString::fromLatin1(kMainCodeBlockBg)),
                 qPrintable(detailsSheet));

        // Sanity: the presets really do differ, so the checks below are not
        // vacuously comparing a palette against itself.
        const QString darkPanelBg = ChatTheme::panelBg();
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-light"_s));
        QVERIFY2(ChatTheme::panelBg() != darkPanelBg,
                 "dark and light presets share a backdrop; the theme check is vacuous");

        card.applyStyle();
        QVERIFY2(card.styleSheet() == cardSheet,
                 qPrintable(QStringLiteral("card sheet moved with the theme:\n%1\nvs\n%2")
                                .arg(cardSheet, card.styleSheet())));
        QVERIFY2(card.findChild<QWidget *>(u"toolHeader"_s)->styleSheet() == headerSheet,
                 "header sheet moved with the theme");
        QVERIFY2(card.findChild<QTextBrowser *>(u"toolDetails"_s)->styleSheet() == detailsSheet,
                 "result surface moved with the theme");
        QVERIFY2(card.findChild<QTextBrowser *>(u"toolDetails"_s)->styleSheet()
                     .contains(QString::fromLatin1(kMainCodeBlockBg)),
                 "result surface is not main's code inset under the light theme");
        QVERIFY2(card.styleSheet().contains(ChatTheme::toolColor(u"read_file"_s)),
                 "the rail must keep the tool's fixed hue under the light theme");

        // Leave the process on the dark preset for any test that runs after
        // this one.
        ChatTheme::loadTheme(settingsWithPreset(u"kateai-dark"_s));
    }
};

QTEST_MAIN(TestToolCardParity)
#include "test_toolcardparity.moc"
