#include "promptedit.h"

#include <QTest>
#include <QSignalSpy>
#include <QKeyEvent>

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

    void testComposerIsHalfTallerAgain()
    {
        PromptEdit edit;
        const int lineH = edit.fontMetrics().lineSpacing();

        // Four and a half lines of head-room when empty, half again the three
        // it used to carry, so a pasted stack trace stays readable.
        const int emptyHeight = edit.height();
        QVERIFY2(emptyHeight >= lineH * 4 + 24,
                 qPrintable(QStringLiteral("empty composer is only %1px").arg(emptyHeight)));
        QVERIFY2(edit.sizeHint().height() >= lineH * 4 + 24,
                 qPrintable(QStringLiteral("size hint is only %1px").arg(edit.sizeHint().height())));
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
};

} // namespace KateAi

QTEST_MAIN(KateAi::TestPromptEdit)
#include "test_promptedit.moc"
