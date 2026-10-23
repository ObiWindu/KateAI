/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the three-line cap on tool headers and command previews.
 *
 * A `bash` card carries the whole command line and a script can run to hundreds
 * of characters. Left unbounded, one such card grew tall enough to bury the
 * conversation around it, which is most painful exactly when the context window
 * is full and the transcript is already long.
 */

#include "chattheme.h"
#include "toolcallwidget.h"

#include <QApplication>
#include <QLabel>
#include <QTest>
#include <QVBoxLayout>
#include <QTextBrowser>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{
constexpr int kMaxLines = 3;

QString longCommand()
{
    return QStringLiteral(
        "cmake --build build -j$(nproc) --target kateai && ctest --test-dir build "
        "--output-on-failure && ctest --test-dir build --repeat until-pass:3 && "
        "echo 'all green, this keeps going for a while' && git push origin main");
}

QString longScript()
{
    return QStringLiteral("#!/usr/bin/env bash\n"
                          "set -euo pipefail\n"
                          "for f in src/*.cpp; do\n"
                          "  clang-format -i \"$f\"\n"
                          "  git add \"$f\"\n"
                          "done\n"
                          "cmake --build build -j8\n"
                          "ctest --test-dir build\n"
                          "echo done\n"
                          "exit 0\n");
}

QLabel *titleLabel(const ToolCallWidget &w)
{
    return w.findChild<QLabel *>(QStringLiteral("toolTitle"));
}
} // namespace

class TestToolCard : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLongSingleLineCommandCapsAtThreeLines()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t1"), &host);
        w.setToolInfo(QStringLiteral("bash"), longCommand(), ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        // The header is re-measured by a 0 ms single-shot reflow once the real
        // width is known; let it land before asserting on the settled height.
        QTest::qWait(60);

        QLabel *title = titleLabel(w);
        QVERIFY(title);
        const QFontMetrics fm(title->font());
        // Three lines of text plus the two pixels the height carries.
        const int cap = fm.lineSpacing() * kMaxLines + 2;
        QVERIFY2(title->height() <= cap,
                 qPrintable(QStringLiteral("title %1px exceeds the %2px three-line cap")
                                .arg(title->height())
                                .arg(cap)));
        // Truncation must be visible, not silent.
        QVERIFY2(title->text().contains(QChar(0x2026)), qPrintable(title->text()));
    }

    void testMultiLineScriptCapsAtThreeLines()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t2"), &host);
        w.setToolInfo(QStringLiteral("bash"), longScript(), ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QLabel *title = titleLabel(w);
        QVERIFY(title);
        const QFontMetrics fm(title->font());
        QVERIFY2(title->height() <= fm.lineSpacing() * kMaxLines + 2,
                 qPrintable(QStringLiteral("title %1px").arg(title->height())));
        QVERIFY2(title->text().contains(QChar(0x2026)), qPrintable(title->text()));
    }

    void testShortTitleIsLeftAlone()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t3"), &host);
        w.setToolInfo(QStringLiteral("read_file"),
                      QStringLiteral("/home/user/project/src/agentloop.cpp"),
                      ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QLabel *title = titleLabel(w);
        QVERIFY(title);
        const QFontMetrics fm(title->font());
        // One line, so the cap must not pad it out to three.
        QVERIFY2(title->height() <= fm.lineSpacing() + 2,
                 qPrintable(QStringLiteral("short title padded to %1px").arg(title->height())));
        QVERIFY2(!title->text().contains(QChar(0x2026)), qPrintable(title->text()));
        // The path must still be readable, not elided away.
        QVERIFY2(title->text().contains(QStringLiteral("agentloop.cpp")), qPrintable(title->text()));
    }

    void testFullCommandSurvivesInTheTooltip()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t4"), &host);
        const QString cmd = longCommand();
        w.setToolInfo(QStringLiteral("bash"), cmd, ToolRisk::Read);
        layout->addWidget(&w);

        // Clamping the header must not throw the command away: it has to remain
        // reachable somewhere.
        QVERIFY2(w.toolName() == QStringLiteral("bash"), qPrintable(w.toolName()));
    }

    void testCardDoesNotGrowWithoutBound()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t5"), &host);
        w.setToolInfo(QStringLiteral("bash"), longCommand(), ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // A single card has no business filling a 400px panel.
        QVERIFY2(w.height() < 260, qPrintable(QStringLiteral("card grew to %1px").arg(w.height())));
    }

    // --- Live output ----------------------------------------------------------

    void testRunningToolShowsPartialOutput()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t6"), &host);
        w.setToolInfo(QStringLiteral("bash"), QStringLiteral("sleep 30 && echo done"), ToolRisk::Execute);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);
        QVERIFY(w.isRunning());

        // Nothing has streamed yet, so there is nothing to show.
        QVERIFY(w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText().trimmed().isEmpty());

        w.setPartialOutput(QStringLiteral("step 1\nstep 2"));
        QTest::qWait(20);
        const QTextBrowser *details = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"));
        QVERIFY2(details->toPlainText().contains(QStringLiteral("step 1")),
                 qPrintable(details->toPlainText()));
        QVERIFY2(details->toPlainText().contains(QStringLiteral("step 2")),
                 qPrintable(details->toPlainText()));
    }

    void testPartialOutputGrowsRatherThanBeingReplaced()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t7"), &host);
        w.setToolInfo(QStringLiteral("bash"), QStringLiteral("make -j8"), ToolRisk::Execute);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        w.setPartialOutput(QStringLiteral("building a\n"));
        w.setPartialOutput(QStringLiteral("building a\nbuilding b\n"));
        QTest::qWait(20);
        const QString text = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("building a")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("building b")), qPrintable(text));
    }

    void testFinishedResultReplacesTheStream()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t8"), &host);
        w.setToolInfo(QStringLiteral("bash"), QStringLiteral("echo hi"), ToolRisk::Execute);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        w.setPartialOutput(QStringLiteral("hi\n"));
        ToolResult result;
        result.name = u"bash"_s;
        result.ok = true;
        result.output = u"hi\nexit code 0"_s;
        w.setFinished(result);
        QTest::qWait(20);

        QVERIFY(!w.isRunning());
        const QString text = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("hi")), qPrintable(text));
        // The final result wins over whatever the stream showed.
        QVERIFY2(text.contains(QStringLiteral("exit code 0")), qPrintable(text));
    }

    void testPartialOutputIsIgnoredOnceFinished()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t9"), &host);
        w.setToolInfo(QStringLiteral("bash"), QStringLiteral("echo hi"), ToolRisk::Execute);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        ToolResult result;
        result.name = u"bash"_s;
        result.ok = true;
        result.output = u"final answer"_s;
        w.setFinished(result);
        QTest::qWait(20);

        // A late chunk arriving after the tool ended must not clobber it.
        w.setPartialOutput(QStringLiteral("stale chunk"));
        QTest::qWait(20);
        QVERIFY(w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText().contains(QStringLiteral("final answer")));
        QVERIFY(!w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText().contains(QStringLiteral("stale chunk")));
    }

    // --- Per-tool colour ------------------------------------------------------

    void testEveryToolCarriesItsOwnColour()
    {
        // A transcript of cards has to be scannable by colour, so no two tool
        // families share a rail and every one has one.
        const QStringList tools{QStringLiteral("read_file"), QStringLiteral("write_file"),
                                QStringLiteral("edit_file"), QStringLiteral("list_dir"),
                                QStringLiteral("grep"), QStringLiteral("glob"),
                                QStringLiteral("bash"), QStringLiteral("question")};
        QStringList seen;
        for (const QString &tool : tools) {
            const QString colour = ChatTheme::toolColor(tool);
            QVERIFY2(!colour.isEmpty(), qPrintable(tool));
            QVERIFY2(colour.startsWith(QLatin1Char('#')), qPrintable(colour));
            QVERIFY2(!seen.contains(colour), qPrintable(QStringLiteral("%1 shares its colour with another tool").arg(tool)));
            seen << colour;
        }
    }

    void testCardRailTakesTheToolsColour()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t10"), &host);
        w.setToolInfo(QStringLiteral("grep"), QStringLiteral("pattern in src/"), ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QVERIFY2(w.styleSheet().contains(ChatTheme::toolColor(QStringLiteral("grep"))),
                 qPrintable(w.styleSheet()));
    }

    void testCardBodyIsTintedButNotRepainted()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t11"), &host);
        w.setToolInfo(QStringLiteral("read_file"), QStringLiteral("src/main.cpp"), ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        const QString body = ChatTheme::toolCardBg(QStringLiteral("read_file"));
        QVERIFY2(body != ChatTheme::toolBg(), "a tool card must not be tinted by its tool at all");
        QVERIFY2(w.styleSheet().contains(body), qPrintable(w.styleSheet()));
    }

    void testFailedCardGoesRedWhateverTheTool()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("t12"), &host);
        w.setToolInfo(QStringLiteral("read_file"), QStringLiteral("src/main.cpp"), ToolRisk::Read);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        ToolResult failed;
        failed.name = u"read_file"_s;
        failed.ok = false;
        failed.output = u"Failed to read src/main.cpp"_s;
        w.setFinished(failed);
        QTest::qWait(20);

        QVERIFY2(w.styleSheet().contains(ChatTheme::danger()), qPrintable(w.styleSheet()));
    }

    // --- File edits ----------------------------------------------------------

    QString editDiff() const
    {
        return QStringLiteral("--- a/src/main.cpp\n"
                              "+++ b/src/main.cpp\n"
                              "@@ -1,3 +1,3 @@\n"
                              " keep this line\n"
                              "-const int old = 1;\n"
                              "+const int revised = 2;\n");
    }

    void testEditCardCarriesADiffSurface()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("d1"), &host);
        w.setToolInfo(QStringLiteral("edit_file"), QStringLiteral("src/main.cpp"), ToolRisk::Write);
        w.setDescribeDiff(editDiff());
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // The change is the point of an edit, so it must be on screen without
        // the user hunting for it.
        QVERIFY2(w.isExpanded(), qPrintable(w.toolName()));
        QTextBrowser *diffView = w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(diffView, qPrintable(QStringLiteral("no diff surface on an edit card")));
        QVERIFY2(diffView->isVisible(), qPrintable(QStringLiteral("diff surface is hidden")));
        QVERIFY2(diffView->height() > 0, qPrintable(QStringLiteral("diff surface has no height")));
        QVERIFY2(diffView->toPlainText().contains(QStringLiteral("const int revised = 2;")),
                 qPrintable(diffView->toPlainText()));
    }

    void testAddedLinesAreGreenAndRemovedRed()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("d2"), &host);
        w.setToolInfo(QStringLiteral("edit_file"), QStringLiteral("src/main.cpp"), ToolRisk::Write);
        w.setDescribeDiff(editDiff());
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QTextBrowser *diffView = w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY(diffView);
        const QString html = diffView->toHtml();
        // Both hues have to reach the rendered HTML, as the line's own colour
        // and as the background that marks the whole line as added or removed.
        QVERIFY2(html.contains(ChatTheme::success()),
                 qPrintable(QStringLiteral("no green in the diff:\n%1").arg(html)));
        QVERIFY2(html.contains(ChatTheme::danger()),
                 qPrintable(QStringLiteral("no red in the diff:\n%1").arg(html)));
        // Green belongs to the added line only, red to the removed one.
        const QString addBg = ChatTheme::mixColors(ChatTheme::success(), ChatTheme::codeBlockBg(), 24);
        const QString delBg = ChatTheme::mixColors(ChatTheme::danger(), ChatTheme::codeBlockBg(), 24);
        QVERIFY2(html.contains(addBg), qPrintable(QStringLiteral("no green highlight behind the added line:\n%1").arg(html)));
        QVERIFY2(html.contains(delBg), qPrintable(QStringLiteral("no red highlight behind the removed line:\n%1").arg(html)));
    }

    void testHugeDiffIsCapped()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("d5"), &host);
        w.setToolInfo(QStringLiteral("write_file"), QStringLiteral("src/generated.cpp"), ToolRisk::Write);
        // A write of a generated file carries a diff with a thousand lines.
        QString big = QStringLiteral("--- a/src/generated.cpp\n+++ b/src/generated.cpp\n");
        for (int i = 0; i < 1000; ++i) {
            big += QStringLiteral("+line %1\n").arg(i);
        }
        w.setDescribeDiff(big);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QTextBrowser *diffView = w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY(diffView);
        // The stat still counts every changed line, so the header is honest.
        QVERIFY2(w.hasDiffPreview(), qPrintable(QStringLiteral("huge diff not attached")));
        // But the rendered diff is bounded and says what was dropped.
        const QString shown = diffView->toPlainText();
        QVERIFY2(shown.contains(QStringLiteral("more lines")),
                 qPrintable(shown.left(200)));
        QVERIFY2(shown.contains(QStringLiteral("line 0")), qPrintable(shown.left(200)));
    }

    void testReadOnlyToolsCarryNoDiffSurface()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("d3"), &host);
        w.setToolInfo(QStringLiteral("read_file"), QStringLiteral("src/main.cpp"), ToolRisk::Read);
        w.setDescribeDiff(editDiff());
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QTextBrowser *diffView = w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(diffView && !diffView->isVisible(),
                 qPrintable(QStringLiteral("a read showed a diff surface")));
        // With nothing to change, no meaningless "+0 −0" either.
        QLabel *stat = nullptr;
        for (QLabel *label : w.findChildren<QLabel *>()) {
            if (label->text().startsWith(QLatin1Char('+'))) {
                stat = label;
                break;
            }
        }
        QVERIFY2(!stat, qPrintable(QStringLiteral("read tool carries a diff stat")));
    }

    void testDiffOpensEvenWhenTheToolReportsNothing()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("d4"), &host);
        w.setToolInfo(QStringLiteral("write_file"), QStringLiteral("src/new.cpp"), ToolRisk::Write);
        w.setDescribeDiff(editDiff());
        ToolResult result;
        result.name = u"write_file"_s;
        result.ok = true;
        result.output = QString();
        w.setFinished(result);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // A write that answers with an empty result still has a change to show,
        // so the card must not collapse itself around nothing.
        QVERIFY2(w.isExpanded(), qPrintable(QStringLiteral("collapsed")));
        QVERIFY2(w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"))->height() > 0,
                 qPrintable(QStringLiteral("diff collapsed with the result")));
    }

    // --- Per-tool surfaces ---------------------------------------------------

    void testDifferentToolsGetDifferentSurfaces()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget read(QStringLiteral("v1"), &host);
        read.setToolInfo(QStringLiteral("read_file"), QStringLiteral("src/main.cpp"), ToolRisk::Read);
        ToolCallWidget run(QStringLiteral("v2"), &host);
        run.setToolInfo(QStringLiteral("bash"), QStringLiteral("make -j8"), ToolRisk::Execute);
        ToolCallWidget search(QStringLiteral("v3"), &host);
        search.setToolInfo(QStringLiteral("grep"), QStringLiteral("ToolCallWidget"), ToolRisk::Read);
        layout->addWidget(&read);
        layout->addWidget(&run);
        layout->addWidget(&search);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QLabel *readCaption = read.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QLabel *runCaption = run.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QLabel *searchCaption = search.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QVERIFY(readCaption && runCaption && searchCaption);
        QVERIFY2(readCaption->isVisible() && runCaption->isVisible() && searchCaption->isVisible(),
                 qPrintable(QStringLiteral("a caption went missing")));
        // Each kind of call says what it holds, and the three are not the same
        // thing said three times.
        QVERIFY2(readCaption->text() != runCaption->text(), qPrintable(readCaption->text()));
        QVERIFY2(readCaption->text() != searchCaption->text(), qPrintable(readCaption->text()));
        QVERIFY2(runCaption->text() != searchCaption->text(), qPrintable(runCaption->text()));

        // A command runs on its own, darker surface; a read sits on the inset
        // every other card uses.
        const QString runSheet = run.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->styleSheet();
        QVERIFY2(runSheet.contains(QStringLiteral("#101014")), qPrintable(runSheet));
        const QString readSheet = read.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->styleSheet();
        QVERIFY2(readSheet.contains(ChatTheme::codeBlockBg()), qPrintable(readSheet));
    }

    // --- Live content --------------------------------------------------------
    //
    // A card in flight used to show its header and nothing else for every tool
    // that does not stream: the user watched a row appear, sit there, and only
    // learn what it had done once it was over. The card now carries the call's
    // own arguments until something better arrives.

    // The whole live sequence for an edit_file call, in the order ChatWidget
    // drives it: describe-diff at toolStarted, the arguments as the running
    // input, then the one-line result. The change is the card's content, so
    // the code block must still hold it after the tool reports back — the
    // reported regression was a card whose code block came back empty.
    void testEditCardKeepsItsDiffThroughTheWholeTurn()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("e2"), &host);
        w.setToolInfo(QStringLiteral("edit_file"), QStringLiteral("src/app.cpp"), ToolRisk::Write);
        w.setDescribeDiff(editDiff());
        w.setFilePath(QStringLiteral("src/app.cpp"));
        w.setRunning();
        w.setRunningInput(QStringLiteral("Replace:\nconst int old = 1;\n\nWith:\nconst int revised = 2;"));
        ToolResult result;
        result.name = u"edit_file"_s;
        result.ok = true;
        result.output = QStringLiteral("Updated /workspace/src/app.cpp");
        w.setFinished(result);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        auto *diff = w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(diff, qPrintable(QStringLiteral("no diff surface")));
        QVERIFY2(!diff->toPlainText().trimmed().isEmpty(),
                 qPrintable(QStringLiteral("the code block is empty")));
        QVERIFY2(diff->isVisible(), qPrintable(QStringLiteral("the code block is hidden")));
        QVERIFY2(diff->height() > 0, qPrintable(QStringLiteral("the code block has no height")));
        QVERIFY2(diff->toPlainText().contains(QStringLiteral("const int revised = 2;")),
                 qPrintable(diff->toPlainText()));
        QVERIFY2(w.isExpanded(), qPrintable(QStringLiteral("collapsed")));
    }

    // A tool that reports no output must not leave an empty code block behind:
    // the block frame (surface + language label) with no code inside is what
    // an edit card restored from an interrupted ACP turn used to show.
    void testFinishedWithNoOutputLeavesNoEmptyCodeBlock()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("e3"), &host);
        w.setToolInfo(QStringLiteral("edit"), QStringLiteral("src/app.cpp"), ToolRisk::Write);
        ToolResult result;
        result.name = u"edit"_s;
        result.ok = true;
        result.output = QString();
        w.setFinished(result);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QTextBrowser *body = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"));
        QVERIFY2(body, qPrintable(QStringLiteral("no body surface")));
        QVERIFY2(body->toPlainText().trimmed().isEmpty(),
                 qPrintable(QStringLiteral("an empty result painted a code block: ")
                                + body->toPlainText()));
        QVERIFY2(!w.isExpanded(), qPrintable(QStringLiteral("expanded around nothing")));
    }

    void testRunningToolShowsWhatItIsWorkingOn()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("l1"), &host);
        w.setToolInfo(QStringLiteral("read_file"), QStringLiteral("src/agentloop.cpp"), ToolRisk::Read);
        w.setRunningInput(QStringLiteral("{\n    \"path\": \"src/agentloop.cpp\",\n    \"start_line\": 100\n}"));
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // The arguments are the card's content while it runs, so the body is
        // open and holding them rather than sitting at zero height.
        QVERIFY2(w.isExpanded(), qPrintable(QStringLiteral("running card stayed collapsed")));
        const QTextBrowser *details = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"));
        QVERIFY(details);
        QVERIFY2(details->toPlainText().contains(QStringLiteral("src/agentloop.cpp")),
                 qPrintable(details->toPlainText()));
        QVERIFY2(details->height() > 0, qPrintable(QStringLiteral("body has no height")));
        QVERIFY2(w.height() > 60, qPrintable(QStringLiteral("card is header-only at %1px").arg(w.height())));
        // The caption names what the body holds right now.
        QLabel *caption = w.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QVERIFY2(caption && caption->isVisible(), qPrintable(QStringLiteral("caption hidden while running")));
        QVERIFY2(caption->text() == QStringLiteral("Input"), qPrintable(caption->text()));
    }
    void testRunningInputGivesWayToOutput()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("l2"), &host);
        w.setToolInfo(QStringLiteral("grep"), QStringLiteral("ToolCallWidget in src/"), ToolRisk::Read);
        w.setRunningInput(QStringLiteral("{\n    \"pattern\": \"ToolCallWidget\"\n}"));
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);
        QVERIFY2(w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText().contains(QStringLiteral("pattern")),
                 qPrintable(QStringLiteral("arguments never reached the body")));

        // A stream replaces them...
        w.setPartialOutput(QStringLiteral("src/toolcallwidget.cpp:118\n"));
        QTest::qWait(20);
        const QString streamed = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(streamed.contains(QStringLiteral("toolcallwidget.cpp")), qPrintable(streamed));
        QVERIFY2(!streamed.contains(QStringLiteral("pattern")), qPrintable(streamed));

        // ...and so does the result.
        ToolResult result;
        result.name = u"grep"_s;
        result.ok = true;
        result.output = u"src/toolcallwidget.cpp:118: class ToolCallWidget"_s;
        w.setFinished(result);
        QTest::qWait(20);
        const QString final = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(final.contains(QStringLiteral("class ToolCallWidget")), qPrintable(final));
        QVERIFY2(!final.contains(QStringLiteral("pattern")), qPrintable(final));
        QLabel *caption = w.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QVERIFY2(caption->text() == QStringLiteral("Matches"), qPrintable(caption->text()));
    }

    void testQuestionCaptionNamesTheBody()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("l7"), &host);
        w.setToolInfo(QStringLiteral("question"), QStringLiteral("Rewrite, or patch?"), ToolRisk::Read);
        w.setRunningInput(QStringLiteral("Rewrite the file, or patch it in place?"));
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // The body holds prose, not arguments, and the caption says so rather
        // than calling the question an "Input".
        QLabel *caption = w.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QVERIFY2(caption->text() == QStringLiteral("Question"), qPrintable(caption->text()));

        ToolResult result;
        result.name = u"question"_s;
        result.ok = true;
        result.output = u"Patch"_s;
        w.setFinished(result);
        QTest::qWait(20);
        QLabel *answered = w.findChild<QLabel *>(QStringLiteral("toolViewCaption"));
        QVERIFY2(answered->text() == QStringLiteral("Answer"), qPrintable(answered->text()));
    }

    void testLiveStreamAndDiffOutrankTheInput()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        // An edit already shows the change, which says more than the arguments
        // it was asked with; a command already shows its output.
        ToolCallWidget edit(QStringLiteral("l3"), &host);
        edit.setToolInfo(QStringLiteral("edit_file"), QStringLiteral("src/main.cpp"), ToolRisk::Write);
        edit.setDescribeDiff(editDiff());
        edit.setRunningInput(QStringLiteral("{\n    \"path\": \"src/main.cpp\",\n    \"old_string\": \"keep this line\"\n}"));
        ToolCallWidget run(QStringLiteral("l4"), &host);
        run.setToolInfo(QStringLiteral("bash"), QStringLiteral("make -j8"), ToolRisk::Execute);
        run.setRunningInput(QStringLiteral("{\n    \"command\": \"make -j8\"\n}"));
        run.setPartialOutput(QStringLiteral("[ 42%] Building\n"));
        layout->addWidget(&edit);
        layout->addWidget(&run);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        QTextBrowser *diffView = edit.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(diffView->toPlainText().contains(QStringLiteral("const int revised = 2;")),
                 qPrintable(diffView->toPlainText()));
        const QString editBody = edit.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(editBody.trimmed().isEmpty(), qPrintable(editBody));

        const QString runBody = run.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(runBody.contains(QStringLiteral("Building")), qPrintable(runBody));
        QVERIFY2(!runBody.contains(QStringLiteral("make -j8")), qPrintable(runBody));
    }

    void testRunningInputIsIgnoredOnceTheToolEnds()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("l5"), &host);
        w.setToolInfo(QStringLiteral("read_file"), QStringLiteral("src/main.cpp"), ToolRisk::Read);
        ToolResult result;
        result.name = u"read_file"_s;
        result.ok = true;
        result.output = u"1: int main() {}"_s;
        w.setFinished(result);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        // A late stream of arguments arriving after the fact would be a lie
        // about a run that is already over.
        w.setRunningInput(QStringLiteral("{\n    \"path\": \"src/main.cpp\"\n}"));
        QTest::qWait(20);
        const QString text = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("int main()")), qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("path")), qPrintable(text));
    }

    void testHugeInputIsCapped()
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("l6"), &host);
        w.setToolInfo(QStringLiteral("write_file"), QStringLiteral("src/generated.cpp"), ToolRisk::Write);
        QStringList args;
        for (int i = 0; i < 400; ++i) {
            args << QStringLiteral("    \"line %1\": \"value\",").arg(i);
        }
        w.setRunningInput(QStringLiteral("{\n") + args.join(u'\n') + u"\n}"_s);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTest::qWait(60);

        const QString shown = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(shown.contains(QStringLiteral("more lines")), qPrintable(shown.left(200)));
        // The card must not grow to the whole panel over a placeholder.
        QVERIFY2(w.height() < 400, qPrintable(QStringLiteral("card grew to %1px").arg(w.height())));
    }

    // ACP reports the edit by kind ("edit"), and the unified diff arrives in
    // the result rather than in the arguments. The card has to take the edit
    // hue and colour the change as soon as that text is known, not after the
    // tool has already finished and not as grey plain text.
    void testAcpEditKindIsColouredAndShowsItsDiff()
    {
        QCOMPARE(ChatTheme::toolColor(QStringLiteral("edit")), ChatTheme::toolColor(QStringLiteral("edit_file")));
        QCOMPARE(ChatTheme::toolColor(QStringLiteral("Edit")), ChatTheme::toolColor(QStringLiteral("edit_file")));
        QCOMPARE(ChatTheme::toolColor(QStringLiteral("str_replace")), ChatTheme::toolColor(QStringLiteral("edit_file")));
        QCOMPARE(ChatTheme::toolColor(QStringLiteral("StrReplace")), ChatTheme::toolColor(QStringLiteral("edit_file")));
        QCOMPARE(ChatTheme::toolColor(QStringLiteral("apply_patch")), ChatTheme::toolColor(QStringLiteral("edit_file")));
        QCOMPARE(ChatTheme::toolColor(QStringLiteral("Write")), ChatTheme::toolColor(QStringLiteral("write_file")));
        QVERIFY(ChatTheme::toolColor(QStringLiteral("edit")) != ChatTheme::textMuted());

        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        host.resize(440, 400);

        ToolCallWidget w(QStringLiteral("acp-edit"), &host);
        w.setToolInfo(QStringLiteral("edit"), QStringLiteral("src/main.cpp"), ToolRisk::Write);
        layout->addWidget(&w);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));

        const QString output = QStringLiteral("Edit applied successfully.\n"
                                              "--- a/src/main.cpp\n"
                                              "+++ b/src/main.cpp\n"
                                              "@@ -1 +1 @@\n"
                                              "-old\n"
                                              "+new\n");
        w.setPartialOutput(output);
        QTest::qWait(40);
        QVERIFY2(w.isRunning(), "the streamed diff waited for the tool to finish");
        QVERIFY2(w.styleSheet().contains(ChatTheme::toolColor(QStringLiteral("edit_file"))),
                 qPrintable(w.styleSheet()));
        QTextBrowser *diffView = w.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(diffView && diffView->isVisible(), "no coloured diff while the edit is still running");
        QVERIFY2(diffView->toPlainText().contains(QStringLiteral("+new")), qPrintable(diffView->toPlainText()));
        QVERIFY2(diffView->toHtml().contains(ChatTheme::success()),
                 qPrintable(diffView->toHtml()));
        const QString streamedDetails = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(streamedDetails.contains(QStringLiteral("Edit applied successfully")), qPrintable(streamedDetails));
        QVERIFY2(!streamedDetails.contains(QStringLiteral("+new")), qPrintable(streamedDetails));

        ToolResult result;
        result.name = u"edit"_s;
        result.ok = true;
        result.output = output;
        w.setFinished(result);
        QTest::qWait(40);

        QVERIFY2(w.styleSheet().contains(ChatTheme::toolColor(QStringLiteral("edit_file"))),
                 qPrintable(w.styleSheet()));
        QVERIFY2(diffView->isVisible(), "the diff disappeared once the edit finished");
        QVERIFY2(diffView->toPlainText().contains(QStringLiteral("+new")), qPrintable(diffView->toPlainText()));
        QVERIFY2(diffView->toHtml().contains(ChatTheme::success()), qPrintable(diffView->toHtml()));
        const QString details = w.findChild<QTextBrowser *>(QStringLiteral("toolDetails"))->toPlainText();
        QVERIFY2(details.contains(QStringLiteral("Edit applied successfully")), qPrintable(details));
        QVERIFY2(!details.contains(QStringLiteral("+++")), qPrintable(details));

        // Agents capitalise the write tool. Its card takes the write hue, and
        // the added and removed lines are highlighted rather than left as plain
        // text on the muted rail.
        ToolCallWidget write(QStringLiteral("acp-write"), &host);
        write.setToolInfo(QStringLiteral("Write"), QStringLiteral("src/main.cpp"), ToolRisk::Write);
        layout->addWidget(&write);
        const QString written = QStringLiteral("--- a/src/main.cpp\n"
                                               "+++ b/src/main.cpp\n"
                                               "@@ -1 +1 @@\n"
                                               "-old\n"
                                               "+new\n");
        write.setDescribeDiff(written);
        QTest::qWait(40);
        QVERIFY2(write.styleSheet().contains(ChatTheme::toolColor(QStringLiteral("write_file"))),
                 qPrintable(write.styleSheet()));
        QTextBrowser *writeDiff = write.findChild<QTextBrowser *>(QStringLiteral("toolDiff"));
        QVERIFY2(writeDiff && writeDiff->isVisible(), "the Write card did not open its diff");
        const QString writeHtml = writeDiff->toHtml();
        QVERIFY2(writeHtml.contains(ChatTheme::success()), qPrintable(writeHtml));
        QVERIFY2(writeHtml.contains(ChatTheme::danger()), qPrintable(writeHtml));
    }
};

QTEST_MAIN(TestToolCard)
#include "test_toolcard.moc"
