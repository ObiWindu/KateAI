#include "agenttext.h"
#include "chattheme.h"
#include "codehighlight.h"

#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextTable>
#include <QFont>

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

    // Regression: the prose half of renderMarkdown() used to look for a literal
    // "<body>" in QTextDocument::toHtml() output. Qt writes the tag with its
    // attributes -- <body style="..."> -- so the match always failed and every
    // prose run fell through to the toHtmlEscaped() fallback, which renders
    // markdown *source*. A chat of "**bold**", "`code`" and bullet lists was
    // the visible result.
    void proseMarkupIsParsedNotShownAsSource()
    {
        const QString markdown = QStringLiteral("Use `grep` and **be careful**:\n\n- first\n- second\n\n## Section\n");
        QTextDocument doc;
        renderMarkdown(&doc, markdown);

        const QString plain = doc.toPlainText();
        // The markers are consumed, not quoted back.
        QVERIFY2(!plain.contains(QStringLiteral("**")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("`")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("- first")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("first")), qPrintable(plain));

        // And the spans carry the formatting the markup asked for.
        bool sawBold = false;
        bool sawCodeFont = false;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            for (QTextBlock::Iterator it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment frag = it.fragment();
                if (frag.text().startsWith(QStringLiteral("be careful")) && frag.charFormat().fontWeight() >= QFont::Bold) {
                    sawBold = true;
                }
                if (frag.text().startsWith(QStringLiteral("grep")) && frag.charFormat().fontFamily() == QStringLiteral("monospace")) {
                    sawCodeFont = true;
                }
            }
        }
        QVERIFY2(sawBold, "bold markers did not produce bold text");
        QVERIFY2(sawCodeFont, "inline code did not produce a monospace span");
    }

    void emptyInputIsSafe()
    {
        QTextDocument doc;
        renderMarkdown(&doc, QString());
        QVERIFY(doc.toPlainText().trimmed().isEmpty());
        renderMarkdown(nullptr, QStringLiteral("x")); // must not crash
    }

    // The dark code block used to lose its stylesheet entirely: the browser's
    // default stylesheet is installed on the browser's own document, and
    // setDocument() replaces that document with the freshly rendered one. Every
    // token the highlighter leaves plain then fell back to the document default
    // of black and vanished against the dark block background, so a fenced cpp
    // sample showed only its keywords. The block must stay legible whether or
    // not a stylesheet reaches the document.
    void codeBlockStaysLegibleWithoutAStylesheet()
    {
        // Plain -- un-highlighted -- tokens are what silently render invisible.
        const QStringList plainTokens = {QStringLiteral(" main"), QStringLiteral("{}"),
                                         QStringLiteral(" b"), QStringLiteral(" ")};

        const auto renderWith = [&plainTokens](bool withStyleSheet) {
            QTextDocument doc;
            if (withStyleSheet) {
                doc.setDefaultStyleSheet(ChatTheme::messageCss());
            }
            doc.setHtml(QStringLiteral("<html><body>")
                        + CodeHighlight::htmlForCode(QStringLiteral("int main() { return 0; }\nif (a < b) {}"),
                                                      QStringLiteral("cpp"))
                        + QStringLiteral("</body></html>"));
            int visible = 0;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                for (QTextBlock::Iterator it = b.begin(); !it.atEnd(); ++it) {
                    const QTextFragment frag = it.fragment();
                    if (plainTokens.contains(frag.text())
                        && frag.charFormat().foreground().color().value() > 100) {
                        ++visible;
                    }
                }
            }
            return visible;
        };

        QVERIFY2(renderWith(false) > 0,
                 "plain code renders black on the dark block when no stylesheet reaches the document");
        QVERIFY2(renderWith(true) > 0, "messageCss() leaves plain code invisible");
    }

    // The stylesheet has to survive the document swap: makeMarkdownDocument()
    // owns the document that setDocument() installs, so a stylesheet set on the
    // browser's own document is thrown away. This is what keeps prose coloured
    // and code blocks styled.
    void makeMarkdownDocumentCarriesTheStylesheet()
    {
        const QString markdown = QStringLiteral("Intro.\n\n```cpp\nint x = 1;\n```\n");

        QTextDocument styled;
        styled.setDefaultStyleSheet(ChatTheme::messageCss());
        renderMarkdown(&styled, markdown);
        QVERIFY2(styled.begin().charFormat().foreground().color().value() > 60,
                 "messageCss() must be applied before the markdown is parsed");

        // And the helper hands back the document the stylesheet was set on,
        // which is what a QTextBrowser receives via setDocument().
        QTextDocument *doc = makeMarkdownDocument(markdown, ChatTheme::messageCss());
        QVERIFY(!doc->defaultStyleSheet().isEmpty());
        QVERIFY2(doc->begin().charFormat().foreground().color().value() > 60,
                 "makeMarkdownDocument() ignored its stylesheet argument");
        delete doc;

        // Rendering into a full document also keeps the code visible.
        QTextDocument *assistant = makeMarkdownDocument(markdown, ChatTheme::messageCss());
        int lightCodeRuns = 0;
        for (QTextBlock b = assistant->begin(); b.isValid(); b = b.next()) {
            for (QTextBlock::Iterator it = b.begin(); !it.atEnd(); ++it) {
                if (it.fragment().charFormat().foreground().color().value() > 100) {
                    ++lightCodeRuns;
                }
            }
        }
        QVERIFY2(lightCodeRuns >= 2, "assistant markdown renders code in the default black");
        delete assistant;
    }

    // The code block must be a box: a table frame with a background carrying the
    // language label above the code.
    void codeBlockIsAFramedTableWithLanguageLabel()
    {
        QTextDocument doc;
        doc.setDefaultStyleSheet(ChatTheme::messageCss());
        doc.setHtml(QStringLiteral("<html><body>")
                    + CodeHighlight::htmlForCode(QStringLiteral("int x = 1;"), QStringLiteral("cpp"))
                    + QStringLiteral("</body></html>"));

        bool sawTable = false;
        for (QTextFrame::Iterator it = doc.rootFrame()->begin(); !it.atEnd(); ++it) {
            if (it.currentFrame()) {
                sawTable = true;
            }
        }
        QVERIFY2(sawTable, "code block is not rendered as a framed box");
        QVERIFY2(doc.toPlainText().contains(QStringLiteral("cpp")), "language label is missing");
        QVERIFY2(doc.toPlainText().contains(QStringLiteral("int x = 1;")), "code text is missing");
    }

    // CSS that names elements the markup never emits silently disables block
    // styling: the rules match nothing. Every selector messageCss() ships for
    // code must name the element CodeHighlight actually produces.
    void messageCssTargetsTheMarkupThatIsEmitted()
    {
        const QString html = CodeHighlight::htmlForCode(QStringLiteral("int x = 1;"), QStringLiteral("cpp"));
        const QString css = ChatTheme::messageCss();

        QVERIFY2(!css.contains(QStringLiteral("span.codeblock-lang")),
                 "span.codeblock-lang is a dead selector: the label is a table cell");

        for (const QLatin1String element : {QLatin1String("table"), QLatin1String("td"), QLatin1String("pre")}) {
            // The tag the selector names must exist in the emitted markup...
            QVERIFY2(html.contains(u"<"_s + QString(element)),
                     qPrintable(u"emitted markup has no <"_s + QString(element)));
        }
        // ...and the stylesheet must carry a rule for the code block itself.
        QVERIFY2(css.contains(QStringLiteral("table.codeblock")), "messageCss() no longer styles table.codeblock");
        QVERIFY2(css.contains(QStringLiteral("td.codeblock-lang")), "messageCss() no longer styles the language strip");
        QVERIFY2(css.contains(QStringLiteral("pre.codeblock")), "messageCss() no longer styles pre.codeblock");
    }
};

// QTextDocument needs a QGuiApplication (font database), so this cannot use
// QTEST_GUILESS_MAIN the way the other non-GUI tests do.
QTEST_MAIN(TestCodeHighlight)
#include "test_codehighlight.moc"