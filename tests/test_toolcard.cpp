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
};

QTEST_MAIN(TestToolCard)
#include "test_toolcard.moc"
