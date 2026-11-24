#include "promptedit.h"

#include <QFrame>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QToolTip>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

class TestPromptEdit : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSubmitSignal()
    {
        PromptEdit edit;
        QSignalSpy spy(&edit, &PromptEdit::submitRequested);

        edit.setPlainText(u"Test prompt"_s);
        QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&edit, &enterEvent);

        QCOMPARE(spy.count(), 1);

        // Shift+Enter should NOT emit submitRequested
        QKeyEvent shiftEnterEvent(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
        QApplication::sendEvent(&edit, &shiftEnterEvent);

        QCOMPARE(spy.count(), 1);
    }

    void testEscapeSignal()
    {
        PromptEdit edit;
        QSignalSpy spy(&edit, &PromptEdit::escapePressed);

        QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&edit, &escEvent);

        QCOMPARE(spy.count(), 1);
    }

    void testHistoryNavigation()
    {
        PromptEdit edit;
        edit.addHistory(u"first prompt"_s);
        edit.addHistory(u"second prompt"_s);

        edit.setPlainText(u"current draft"_s);

        // Press Up: should recall "second prompt"
        QKeyEvent upEvent(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(&edit, &upEvent);
        QCOMPARE(edit.toPlainText(), u"second prompt"_s);

        // Press Up again: should recall "first prompt"
        QApplication::sendEvent(&edit, &upEvent);
        QCOMPARE(edit.toPlainText(), u"first prompt"_s);

        // Press Down: should recall "second prompt"
        QKeyEvent downEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(&edit, &downEvent);
        QCOMPARE(edit.toPlainText(), u"second prompt"_s);

        // Press Down again: should restore "current draft"
        QApplication::sendEvent(&edit, &downEvent);
        QCOMPARE(edit.toPlainText(), u"current draft"_s);
    }

    void testCompletionWords()
    {
        PromptEdit edit;
        const QStringList files = {u"chatwidget.cpp"_s, u"agentloop.cpp"_s, u"active"_s};
        edit.setCompletionWords(files);

        // Type "@chat" and trigger completion
        edit.setPlainText(u"check @chat"_s);
        edit.moveCursor(QTextCursor::End);

        QKeyEvent key(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, u"c"_s);
        QApplication::sendEvent(&edit, &key);

        // Test inserting completion directly
        QMetaObject::invokeMethod(&edit, "insertCompletion", Q_ARG(QString, u"chatwidget.cpp"_s));
        QVERIFY(edit.toPlainText().contains(u"@chatwidget.cpp "_s));
    }

    void testSlashCommandsFilterAndInsert()
    {
        PromptEdit edit;
        edit.show();
        QApplication::processEvents();

        QList<PromptCommand> commands;
        commands.append({u"/review"_s, u"Review the current diff"_s});
        commands.append({u"/release"_s, u"Prepare a release"_s});
        edit.setSlashCommands(commands);
        QVERIFY(edit.placeholderText().contains(u"/ for commands"_s));

        edit.setPlainText(u"/re"_s);
        edit.moveCursor(QTextCursor::End);
        QApplication::processEvents();

        auto *popup = edit.findChild<QFrame *>(u"promptCommandPopup"_s);
        QVERIFY(popup);
        QCOMPARE(edit.findChildren<QLabel *>(u"promptCommandRow"_s).size(), 2);

        // The typed text filters names and descriptions alike.
        edit.setPlainText(u"/diff"_s);
        edit.moveCursor(QTextCursor::End);
        QApplication::processEvents();
        QCOMPARE(edit.findChildren<QLabel *>(u"promptCommandRow"_s).size(), 1);

        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&edit, &enter);
        QCOMPARE(edit.toPlainText(), u"/review "_s);
    }

    void testSlashCommandsOnlyTriggerOnTheFirstCharacter()
    {
        PromptEdit edit;
        edit.show();
        QApplication::processEvents();
        edit.setSlashCommands({{u"/review"_s, u"Review the current diff"_s}});

        edit.setPlainText(u"please /rev"_s);
        edit.moveCursor(QTextCursor::End);
        QApplication::processEvents();
        auto *popup = edit.findChild<QFrame *>(u"promptCommandPopup"_s);
        QVERIFY(popup);
        QCOMPARE(edit.findChildren<QLabel *>(u"promptCommandRow"_s).size(), 0);

        edit.setSlashCommands({});
        QVERIFY(!edit.placeholderText().contains(u"/ for commands"_s));
    }

    void testSlashCommandHintLeadsTheLiveList()
    {
        // A restored ACP conversation has no live session at startup, so no
        // commands have been advertised yet. The composer must still teach the
        // "/" gesture; the list fills in when the agent connects.
        PromptEdit edit;
        QVERIFY(!edit.placeholderText().contains(u"/ for commands"_s));

        edit.setSlashCommandHintEnabled(true);
        QVERIFY(edit.placeholderText().contains(u"/ for commands"_s));

        // An advertised list keeps the hint, and clearing it does not take the
        // hint away while the ACP agent is still selected.
        edit.setSlashCommands({{u"/review"_s, u"Review the current diff"_s}});
        QVERIFY(edit.placeholderText().contains(u"/ for commands"_s));
        edit.setSlashCommands({});
        QVERIFY(edit.placeholderText().contains(u"/ for commands"_s));

        // Switching away from ACP restores the plain hint.
        edit.setSlashCommandHintEnabled(false);
        QVERIFY(!edit.placeholderText().contains(u"/ for commands"_s));
    }

    void testSlashHintExplainsAnEmptyCommandList()
    {
        // With an ACP agent selected but no session yet, "/" has nothing to
        // list. The popup still opens and says why instead of staying hidden
        // and making the gesture look broken.
        PromptEdit edit;
        edit.show();
        edit.setSlashCommandHintEnabled(true);
        edit.setPlainText(u"/"_s);
        edit.moveCursor(QTextCursor::End);
        QApplication::processEvents();

        auto *popup = edit.findChild<QFrame *>(u"promptCommandPopup"_s);
        QVERIFY(popup);
        QVERIFY2(popup->isVisible(), "the hint popup stayed hidden");
        QCOMPARE(edit.findChildren<QLabel *>(u"promptCommandRow"_s).size(), 0);
        auto *hint = edit.findChild<QLabel *>(u"promptCommandHintRow"_s);
        QVERIFY2(hint, "the empty-list hint row was not created");
        QVERIFY(hint->isVisible());

        // The first advertised list replaces the hint with the real rows.
        edit.setSlashCommands({{u"/review"_s, u"Review the current diff"_s}});
        QApplication::processEvents();
        QCOMPARE(edit.findChildren<QLabel *>(u"promptCommandRow"_s).size(), 1);
        QVERIFY(!edit.findChild<QLabel *>(u"promptCommandHintRow"_s));

        // ...and clearing it again brings the hint back rather than a blank
        // frame or a list of stale labels.
        edit.setSlashCommands({});
        QApplication::processEvents();
        QCOMPARE(edit.findChildren<QLabel *>(u"promptCommandRow"_s).size(), 0);
        QVERIFY(edit.findChild<QLabel *>(u"promptCommandHintRow"_s));
    }

    void testSlashPopupIsAWindowChildAboveTheCaret()
    {
        // A real neighbour (transcript) below the composer, so the popup has
        // somewhere to go above the caret.
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        layout->addStretch();
        auto *edit = new PromptEdit(&host);
        layout->addWidget(edit);
        host.resize(640, 480);
        host.show();
        QApplication::processEvents();

        edit->setSlashCommands({{u"/review"_s, u"Review the current diff"_s},
                                {u"/release"_s, u"Prepare a release"_s}});
        edit->setFocus();
        edit->setPlainText(u"/"_s);
        edit->moveCursor(QTextCursor::End);
        QApplication::processEvents();

        auto *popup = host.findChild<QFrame *>(u"promptCommandPopup"_s);
        QVERIFY(popup);
        // The popup must follow the editor, so it has to live in the hosting
        // window rather than float as a native window.
        QCOMPARE(popup->parentWidget(), &host);
        QVERIFY(!popup->isWindow());
        QVERIFY(popup->isVisible());

        // Typing more characters keeps the hint on screen and repositions it.
        edit->setPlainText(u"/re"_s);
        edit->moveCursor(QTextCursor::End);
        QApplication::processEvents();
        QVERIFY2(popup->isVisible(), "the hint vanished while the user was typing");

        const QRect caret = edit->cursorRect();
        const QPoint caretTop = edit->viewport()->mapTo(&host, QPoint(caret.left(), caret.top()));
        QVERIFY2(popup->geometry().bottom() <= caretTop.y(),
                 qPrintable(QStringLiteral("popup bottom %1 is below the caret top %2")
                                .arg(popup->geometry().bottom())
                                .arg(caretTop.y())));
        QVERIFY2(qAbs(popup->x() - caretTop.x()) <= 1,
                 qPrintable(QStringLiteral("popup x %1 is not at the caret x %2")
                                .arg(popup->x())
                                .arg(caretTop.x())));
    }

    void testFilteredSlashPopupKeepsItsRowsLaidOut()
    {
        // Regression: rows created while the popup is already visible used to
        // stay hidden, and a layout ignores hidden widgets when it measures
        // itself. The popup then collapsed to its border and margins -- a thin
        // strip at the very bottom of the panel -- as soon as typing narrowed
        // the list. The initial (unfiltered) list was laid out by show() and
        // looked right, which is why only the filtered one broke.
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        layout->addStretch();
        auto *edit = new PromptEdit(&host);
        layout->addWidget(edit);
        host.resize(640, 480);
        host.show();
        QApplication::processEvents();

        edit->setSlashCommands({{u"/review"_s, u"Review the current diff"_s},
                                {u"/release"_s, u"Prepare a release"_s},
                                {u"/revise"_s, u"Revise the last turn"_s}});
        edit->setFocus();
        // Typed, not pasted: the keystroke path rebuilds the list while the
        // popup is mapped, which is what triggered the collapse.
        QTest::keyClicks(edit, u"/re"_s);
        QApplication::processEvents();

        auto *popup = host.findChild<QFrame *>(u"promptCommandPopup"_s);
        QVERIFY(popup);
        QVERIFY2(popup->isVisible(), "the filtered popup disappeared");

        const QList<QLabel *> rows = host.findChildren<QLabel *>(u"promptCommandRow"_s);
        QCOMPARE(rows.size(), 3);
        for (QLabel *row : rows) {
            QVERIFY2(row->isVisible(), "a filtered command row stayed hidden");
            QVERIFY2(row->height() > 0, "a filtered command row was squeezed to no height");
        }
        // The frame has to hold the rows, not shrink to its 2px border plus
        // margins (12px) and land under the chat panel.
        QVERIFY2(popup->height() > rows.first()->height(),
                 qPrintable(QStringLiteral("filtered popup is %1px, a row is %2px")
                                .arg(popup->height())
                                .arg(rows.first()->height())));
        QVERIFY2(popup->height() > 12, "the filtered popup collapsed to an empty frame");

        // The list still sits above the caret where the user is typing.
        const QRect caret = edit->cursorRect();
        const QPoint caretTop = edit->viewport()->mapTo(&host, QPoint(caret.left(), caret.top()));
        QVERIFY2(popup->geometry().bottom() <= caretTop.y(),
                 qPrintable(QStringLiteral("popup bottom %1 is below the caret top %2")
                                .arg(popup->geometry().bottom())
                                .arg(caretTop.y())));
    }

    void testSlashPopupCoversTheConfiguredOverlayHost()
    {
        // The chat panel hands the composer a host bigger than the composer so
        // the list can be drawn over the transcript. The popup must be a child
        // of that host (in front of the panel's own children once raised), not
        // of the top-level window behind it.
        QWidget window;
        auto *outer = new QVBoxLayout(&window);
        auto *panel = new QWidget(&window);
        panel->setObjectName(u"panelStub"_s);
        outer->addWidget(panel);
        auto *panelLayout = new QVBoxLayout(panel);
        auto *transcript = new QWidget(panel);
        transcript->setObjectName(u"transcriptStub"_s);
        transcript->setMinimumHeight(320);
        panelLayout->addWidget(transcript, 1);
        auto *edit = new PromptEdit(panel);
        panelLayout->addWidget(edit);
        window.resize(640, 480);
        window.show();
        QApplication::processEvents();

        edit->setCommandPopupHost(panel);
        edit->setSlashCommands({{u"/review"_s, u"Review the current diff"_s},
                                {u"/release"_s, u"Prepare a release"_s}});
        edit->setFocus();
        edit->setPlainText(u"/"_s);
        edit->moveCursor(QTextCursor::End);
        QApplication::processEvents();

        auto *popup = panel->findChild<QFrame *>(u"promptCommandPopup"_s);
        QVERIFY2(popup, "the overlay host did not receive the command popup");
        QCOMPARE(popup->parentWidget(), panel);
        QVERIFY(popup->isVisible());

        // It has to reach up out of the composer and over the transcript.
        const int composerTop = edit->mapTo(panel, QPoint(0, 0)).y();
        QVERIFY2(popup->geometry().top() < composerTop,
                 qPrintable(QStringLiteral("popup top %1 is not above the composer top %2")
                                .arg(popup->geometry().top())
                                .arg(composerTop)));
        const QRect caret = edit->cursorRect();
        const QPoint caretTop = edit->viewport()->mapTo(panel, QPoint(caret.left(), caret.top()));
        QVERIFY2(popup->geometry().bottom() <= caretTop.y(),
                 qPrintable(QStringLiteral("popup bottom %1 is below the caret top %2")
                                .arg(popup->geometry().bottom())
                                .arg(caretTop.y())));
    }

    void testComposerIsAHalfTallerAgainAndATenthShorter()
    {
        PromptEdit edit;
        const int lineH = edit.fontMetrics().lineSpacing();

        // Four and a half lines of head-room when empty, a tenth less than it
        // used to carry, so the transcript gets the room the box used to take.
        const int emptyHeight = edit.height();
        QVERIFY2(emptyHeight >= lineH * 4,
                 qPrintable(QStringLiteral("empty composer is only %1px").arg(emptyHeight)));
        QVERIFY2(edit.sizeHint().height() >= lineH * 4,
                 qPrintable(QStringLiteral("size hint is only %1px").arg(edit.sizeHint().height())));
        // ...but not the full old height, which was 24px of padding on top of
        // those lines.
        QVERIFY2(emptyHeight < lineH * 4 + 24,
                 qPrintable(QStringLiteral("empty composer kept its old %1px height").arg(emptyHeight)));
    }

    void testShortTextNeverShrinksTheComposer()
    {
        PromptEdit edit;
        // The head-room is the floor of the box, not only its empty state: a
        // single short line must not collapse the composer back down.
        const int floorHeight = edit.height();
        edit.setPlainText(QStringLiteral("a single line is enough to see"));
        QApplication::processEvents();

        QVERIFY2(edit.height() >= floorHeight,
                 qPrintable(QStringLiteral("composer shrank to %1px, below the %2px floor")
                            .arg(edit.height())
                            .arg(floorHeight)));
    }

    void testVoiceButtonLivesInsidePromptAndShowsTooltipWhenDisabled()
    {
        PromptEdit edit;
        edit.resize(360, 100);
        edit.show();

        QPushButton *btn = edit.voiceButton();
        QVERIFY(btn != nullptr);
        QCOMPARE(btn->parentWidget(), &edit);
        QCOMPARE(btn->objectName(), u"voicePromptButton"_s);

        // Disabled by default until a usable mic is confirmed.
        QVERIFY(!btn->isEnabled());
        QVERIFY(btn->toolTip().contains(u"No microphone"_s, Qt::CaseInsensitive));

        // Hovering over the disabled mic button delivers QEvent::ToolTip to
        // PromptEdit, which must display the informational tooltip.
        const QPoint centerInEdit = btn->geometry().center();
        QHelpEvent helpEvent(QEvent::ToolTip, centerInEdit, edit.mapToGlobal(centerInEdit));
        QToolTip::hideText();
        QVERIFY(QApplication::sendEvent(&edit, &helpEvent));
        QCOMPARE(QToolTip::text(), btn->toolTip());

        // Enabling when a mic is usable allows clicking and emits voiceRequested.
        QSignalSpy voiceSpy(&edit, &PromptEdit::voiceRequested);
        edit.setVoiceAvailability(true, u"USB Condenser Mic"_s, u"Ctrl+Alt+V"_s);
        QVERIFY(btn->isEnabled());
        QVERIFY(btn->toolTip().contains(u"USB Condenser Mic"_s));
        btn->click();
        QCOMPARE(voiceSpy.count(), 1);
    }
};

} // namespace KateAi

QTEST_MAIN(KateAi::TestPromptEdit)
#include "test_promptedit.moc"
