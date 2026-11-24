/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Guards the properties the question card is built around:
 *
 *  1. It shows the question, and offers the suggested answers as one-tap chips.
 *  2. An answer reports the card's id and the text, whichever way it was given.
 *  3. allow_freeform == false leaves nothing to type, only the offered answers.
 *  4. An answered card is a record: nothing left to press, answer on show.
 */

#include "questionwidget.h"

#include "chattheme.h"
#include "settings.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestQuestionWidget : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCardShowsTheQuestionAndTheOfferedAnswers()
    {
        QuestionWidget widget(u"q-1"_s);
        widget.setQuestion(u"Rewrite the whole file, or patch it in place?"_s, {u"Rewrite"_s, u"Patch"_s}, true);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        const QList<QTextBrowser *> bodies = widget.findChildren<QTextBrowser *>();
        QCOMPARE(bodies.size(), 1);
        QVERIFY2(bodies.first()->toPlainText().contains(u"patch it in place"_s), qPrintable(bodies.first()->toPlainText()));
        // The body must actually occupy space, not just hold the text in a
        // collapsed document that is never painted.
        QVERIFY2(bodies.first()->height() > 20,
                 qPrintable(QStringLiteral("the question body collapsed to %1px").arg(bodies.first()->height())));
        // The text has to be painted, not merely stored in the document: a
        // body colour equal to the card background leaves a blank card even
        // though toPlainText() still returns the question.
        const QImage painted = bodies.first()->viewport()->grab().toImage();
        QSet<QRgb> colours;
        for (int y = 0; y < painted.height(); ++y) {
            for (int x = 0; x < painted.width(); ++x) {
                colours.insert(painted.pixel(x, y));
            }
        }
        QVERIFY2(colours.size() > 2, "the question body painted no text pixels");

        // The offered answers are one-tap chips, not a menu to go hunting for.
        QVERIFY2(widget.findChild<QPushButton *>(), qPrintable(QStringLiteral("no option chips at all")));
        const QList<QPushButton *> buttons = widget.findChildren<QPushButton *>();
        QStringList labels;
        for (QPushButton *button : buttons) {
            labels << button->text();
        }
        QVERIFY(labels.contains(u"Rewrite"_s));
        QVERIFY(labels.contains(u"Patch"_s));
        QVERIFY(labels.contains(u"Answer"_s));

        // Free-form answering is on by default.
        QVERIFY(widget.findChild<QLineEdit *>());
        QVERIFY(widget.findChild<QLineEdit *>()->isVisible());
    }

    void testQuestionBodyPaintsInBothThemes()
    {
        for (const QString &preset : {u"kateai-dark"_s, u"kateai-light"_s}) {
            Settings settings;
            settings.themePreset = preset;
            ChatTheme::loadTheme(settings);

            QuestionWidget widget(u"q-theme"_s);
            widget.setQuestion(u"Rewrite the file, or patch it in place?"_s, {u"Rewrite"_s, u"Patch"_s}, true);
            widget.show();
            QVERIFY(QTest::qWaitForWindowExposed(&widget));

            QTextBrowser *body = widget.findChild<QTextBrowser *>();
            QVERIFY(body);
            const QImage painted = body->viewport()->grab().toImage();
            QSet<QRgb> colours;
            for (int y = 0; y < painted.height(); ++y) {
                for (int x = 0; x < painted.width(); ++x) {
                    colours.insert(painted.pixel(x, y));
                }
            }
            QVERIFY2(colours.size() > 2, qPrintable(u"the %1 question body painted no text"_s.arg(preset)));
        }
        ChatTheme::loadTheme(Settings());
    }

    void testCardShowsTheQuestionInsideAHostDock()
    {
        // The chat panel creates the card while its host is still hidden, fills
        // it, and only then adds it to the dock layout. The body has to come
        // up with the question once the dock is on screen.
        QWidget dock;
        auto *dockLayout = new QVBoxLayout(&dock);
        dockLayout->setContentsMargins(10, 6, 10, 0);

        auto *card = new QuestionWidget(u"q-dock"_s, &dock);
        card->setQuestion(u"Rewrite the whole file, or patch it in place?"_s, {u"Rewrite"_s}, true);
        dockLayout->addWidget(card);
        dock.resize(600, 400);
        dock.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dock));
        QTest::qWait(50);

        QTextBrowser *body = card->findChild<QTextBrowser *>();
        QVERIFY(body);
        QVERIFY2(body->toPlainText().contains(u"patch it in place"_s), qPrintable(body->toPlainText()));
        QVERIFY2(body->height() > 10,
                 qPrintable(QStringLiteral("the question body collapsed to %1px").arg(body->height())));
    }

    void testChipAnswersWithItsOwnText()
    {
        QuestionWidget widget(u"q-2"_s);
        widget.setQuestion(u"Which approach?"_s, {u"First approach"_s, u"Second approach"_s}, true);

        QSignalSpy answered(&widget, &QuestionWidget::answered);
        QVERIFY(answered.isValid());

        const QList<QPushButton *> buttons = widget.findChildren<QPushButton *>();
        QPushButton *chip = nullptr;
        for (QPushButton *button : buttons) {
            if (button->text() == u"Second approach"_s) {
                chip = button;
                break;
            }
        }
        QVERIFY2(chip, qPrintable(QStringLiteral("no chip for the second answer")));
        QTest::mouseClick(chip, Qt::LeftButton);

        QCOMPARE(answered.size(), 1);
        const QList<QVariant> args = answered.takeFirst();
        QCOMPARE(args.at(0).toString(), u"q-2"_s);
        QCOMPARE(args.at(1).toString(), u"Second approach"_s);
    }

    void testTypedAnswerIsReported()
    {
        QuestionWidget widget(u"q-3"_s);
        widget.setQuestion(u"What should the flag be called?"_s, QStringList(), true);

        QSignalSpy answered(&widget, &QuestionWidget::answered);
        widget.findChild<QLineEdit *>()->setText(u"  --dry-run  "_s);
        QTest::mouseClick(widget.findChild<QPushButton *>(), Qt::LeftButton);

        QCOMPARE(answered.size(), 1);
        QCOMPARE(answered.takeFirst().at(1).toString(), u"--dry-run"_s);
    }

    void testNoFreeformLeavesOnlyTheOfferedAnswers()
    {
        QuestionWidget widget(u"q-4"_s);
        widget.setQuestion(u"Pick one."_s, {u"Only choice"_s}, false);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QVERIFY(widget.findChild<QLineEdit *>()->isHidden());
        const QList<QPushButton *> buttons = widget.findChildren<QPushButton *>();
        bool sawChip = false;
        for (QPushButton *button : buttons) {
            if (button->text() == u"Only choice"_s) {
                sawChip = true;
            }
            // The only buttons left are the offered answers.
            QVERIFY(button->text() == u"Only choice"_s || button->text() == u"Answer"_s);
        }
        QVERIFY(sawChip);
    }

    void testAnsweredCardIsARecord()
    {
        QuestionWidget widget(u"q-5"_s);
        widget.setQuestion(u"Which one?"_s, {u"This one"_s}, true);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        widget.markAnswered(u"This one"_s);

        for (QPushButton *button : widget.findChildren<QPushButton *>()) {
            QVERIFY2(!button->isVisible(), qPrintable(QStringLiteral("an answered card still offers %1").arg(button->text())));
        }
        QVERIFY(widget.findChild<QLineEdit *>()->isHidden());
        const QList<QLabel *> labels = widget.findChildren<QLabel *>();
        bool sawAnswer = false;
        for (QLabel *label : labels) {
            if (label->text().contains(u"This one"_s)) {
                sawAnswer = true;
            }
        }
        QVERIFY2(sawAnswer, "the record has to say what was answered");

        // A second answer cannot be filed for the same question.
        QSignalSpy answered(&widget, &QuestionWidget::answered);
        widget.markAnswered(u"Something else"_s);
        QCOMPARE(answered.size(), 0);
    }
};

QTEST_MAIN(TestQuestionWidget)
#include "test_questionwidget.moc"
