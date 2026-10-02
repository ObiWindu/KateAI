#include "agenttext.h"
#include "chattheme.h"
#include "codehighlight.h"

#include <QTest>
#include <QTextDocument>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestCodeHighlight : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void syntaxDefinitionsAreAvailable()
    {
        // Kate ships the definitions; without them every block degrades to
        // plain text and the feature is silently useless.
        QVERIFY2(CodeHighlight::isAvailable(), "no KSyntaxHighlighting definitions were found");
        QVERIFY(CodeHighlight::availableDefinitionCount() > 50);
    }

    void languageAliasesMapToRealDefinitions()
    {
        QCOMPARE(CodeHighlight::normaliseLanguage(QStringLiteral("cpp")), QStringLiteral("C++"));
        QCOMPARE(CodeHighlight::normaliseLanguage(QStringLiteral("js")), QStringLiteral("JavaScript"));
        QCOMPARE(CodeHighlight::normaliseLanguage(QStringLiteral("py")), QStringLiteral("Python"));
        QCOMPARE(CodeHighlight::normaliseLanguage(QStringLiteral("sh")), QStringLiteral("Bash"));
        QCOMPARE(CodeHighlight::normaliseLanguage(QString()), QString());
        // An unknown label is passed through untouched.
        QCOMPARE(CodeHighlight::normaliseLanguage(QStringLiteral("Brainfuck")), QStringLiteral("Brainfuck"));
    }

    void knownLanguageProducesColouredSpans()
    {
        const QString html = CodeHighlight::htmlForCode(QStringLiteral("int main() { return 0; }"),
                                                        QStringLiteral("cpp"));
        QVERIFY(html.contains(QStringLiteral("class=\"codeblock\"")));
        QVERIFY(html.contains(QStringLiteral("<span style=\"color:")));
        // The actual code must survive.
        QVERIFY(html.contains(QStringLiteral("int")));
        QVERIFY(html.contains(QStringLiteral("main")));
    }

    void unknownLanguageStillRendersAsABlock()
    {
        // A language Kate has no definition for must not vanish, and must keep
        // the same block treatment so it does not look like prose.
        const QString html = CodeHighlight::htmlForCode(QStringLiteral("some raw content"),
                                                        QStringLiteral("definitelynotalanguage"));
        QVERIFY(html.contains(QStringLiteral("class=\"codeblock\"")));
        QVERIFY(html.contains(QStringLiteral("some raw content")));
        QVERIFY(html.contains(QStringLiteral("definitelynotalanguage")));
    }

    void codeIsHtmlEscaped()
    {
        const QString html = CodeHighlight::htmlForCode(QStringLiteral("if (a < b && c) { }"),
                                                        QStringLiteral("cpp"));
        QVERIFY(!html.contains(QStringLiteral("<b &&")));
        QVERIFY(html.contains(QStringLiteral("&lt;")));
    }

    void markdownKeepsProseAroundCodeBlocks()
    {
        const QString markdown = QStringLiteral("before\n\n```cpp\nint x = 1;\n```\n\nafter\n");
        QTextDocument doc;
        renderMarkdown(&doc, markdown);
        const QString plain = doc.toPlainText();
        // Ordering is the thing most likely to break when a renderer starts
        // assembling HTML by hand.
        QVERIFY(plain.contains(QStringLiteral("before")));
        QVERIFY(plain.contains(QStringLiteral("int x = 1;")));
        QVERIFY(plain.contains(QStringLiteral("after")));
        QVERIFY(plain.indexOf(QStringLiteral("before")) < plain.indexOf(QStringLiteral("int x = 1;")));
        QVERIFY(plain.indexOf(QStringLiteral("int x = 1;")) < plain.indexOf(QStringLiteral("after")));
    }

    void multipleCodeBlocksAndProseAllSurvive()
    {
        // Regression: the first implementation called setMarkdown() per prose
        // run, and each call replaced the whole document.
        const QString markdown = QStringLiteral(
            "FIRSTWORD\n\n```cpp\nint alpha;\n```\n\nSECONDWORD\n\n```python\nbeta = 2\n```\n\nTHIRDWORD\n");
        QTextDocument doc;
        renderMarkdown(&doc, markdown);
        const QString plain = doc.toPlainText();
        QVERIFY(plain.contains(QStringLiteral("int alpha;")));
        QVERIFY(plain.contains(QStringLiteral("beta = 2")));
        QVERIFY(plain.contains(QStringLiteral("FIRSTWORD")));
        QVERIFY(plain.contains(QStringLiteral("SECONDWORD")));
        QVERIFY(plain.contains(QStringLiteral("THIRDWORD")));
        // And the blocks stay in document order.
        QVERIFY(plain.indexOf(QStringLiteral("FIRSTWORD")) < plain.indexOf(QStringLiteral("int alpha;")));
        QVERIFY(plain.indexOf(QStringLiteral("int alpha;")) < plain.indexOf(QStringLiteral("SECONDWORD")));
        QVERIFY(plain.indexOf(QStringLiteral("SECONDWORD")) < plain.indexOf(QStringLiteral("beta = 2")));
        QVERIFY(plain.indexOf(QStringLiteral("beta = 2")) < plain.indexOf(QStringLiteral("THIRDWORD")));
    }

    void unclosedFenceStillRendersWhileStreaming()
    {
        // What arrives mid-stream: an opening fence with no closing one yet.
        const QString streaming = QStringLiteral("Here:\n\n```cpp\nint partial = ");
        QTextDocument doc;
        renderMarkdown(&doc, streaming);
        QVERIFY(doc.toPlainText().contains(QStringLiteral("Here:")));
        QVERIFY(doc.toPlainText().contains(QStringLiteral("int partial =")));
    }

    void inlineCodeAndBoldSurvive()
    {
        QTextDocument doc;
        renderMarkdown(&doc, QStringLiteral("Use `grep` and **be careful**."));
        const QString plain = doc.toPlainText();
        QVERIFY(plain.contains(QStringLiteral("grep")));
        QVERIFY(plain.contains(QStringLiteral("be careful")));
    }

    void emptyInputIsSafe()
    {
        QTextDocument doc;
        renderMarkdown(&doc, QString());
        QVERIFY(doc.toPlainText().trimmed().isEmpty());
        renderMarkdown(nullptr, QStringLiteral("x")); // must not crash
    }
};

// QTextDocument needs a QGuiApplication (font database), so this cannot use
// QTEST_GUILESS_MAIN the way the other non-GUI tests do.
QTEST_MAIN(TestCodeHighlight)
#include "test_codehighlight.moc"