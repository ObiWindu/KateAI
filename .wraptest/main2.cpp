#include <QApplication>
#include <QTextDocument>
#include <QDebug>
#include <cstdio>

static QString css =
    "body { color: #e4e4e4; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
    "table.codeblock { border: 1px solid #3c3c40; border-radius: 6px; margin: 8px 0; background-color: #151517; }"
    "td.codeblock-lang { color: #8b8b92; font-family: sans-serif; font-size: 10px; font-weight: 600; background-color: #151517; padding: 4px 10px; }"
    "pre.codeblock { color: #e4e4e4; font-family: monospace; font-size: 12px; white-space: pre-wrap; background-color: #151517; margin: 6px 10px; }";

QString codeBlock(const QString &body)
{
    return QStringLiteral(
        "<table class=\"codeblock\" width=\"100%\" cellpadding=\"0\" cellspacing=\"0\" style=\"margin-top:6px; margin-bottom:6px;\">"
        "<tr><td class=\"codeblock-lang\" bgcolor=\"#151517\" style=\"color:#8b8b92; font-size:10px; font-weight:600; padding:5px 10px;\">cpp</td></tr>"
        "<tr><td bgcolor=\"#151517\" style=\"padding:2px 10px 8px 10px;\">"
        "<pre class=\"codeblock\" style=\"margin:0; color:#e4e4e4;\">%1</pre></td></tr>"
        "</table>").arg(body);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // 1) long line with spaces, pre-wrap
    {
        QTextDocument d;
        d.setDefaultStyleSheet(css);
        d.setHtml(QStringLiteral("<html><body>") + codeBlock(QStringLiteral("foo bar baz qux quux corge grault garply waldo fred plugh xyzzy thud").toHtmlEscaped()) + QStringLiteral("</body></html>"));
        d.setTextWidth(400);
        fprintf(stderr, "spaces: idealWidth=%f size=%fx%f\n", d.idealWidth(), d.size().width(), d.size().height());
    }
    // 2) long single word (no spaces)
    {
        QTextDocument d;
        d.setDefaultStyleSheet(css);
        const QString word = QStringLiteral("averylongsinglewordwithnowhitespacewhatsoeverthatgoesonandonandonandonandonandonandonandonandonandon");
        d.setHtml(QStringLiteral("<html><body>") + codeBlock(word.toHtmlEscaped()) + QStringLiteral("</body></html>"));
        d.setTextWidth(400);
        fprintf(stderr, "noword: idealWidth=%f size=%fx%f\n", d.idealWidth(), d.size().width(), d.size().height());
    }
    // 3) long single word, no white-space rule at all (default pre behaviour)
    {
        QTextDocument d;
        d.setDefaultStyleSheet(QString());
        d.setHtml(QStringLiteral("<html><body>") + codeBlock(QStringLiteral("averylongsinglewordwithnowhitespacewhatsoeverthatgoesonandonandonandonandonandonandonandonandonandon").toHtmlEscaped()) + QStringLiteral("</body></html>"));
        d.setTextWidth(400);
        fprintf(stderr, "nostylesheet: idealWidth=%f size=%fx%f\n", d.idealWidth(), d.size().width(), d.size().height());
    }
    // 4) table width 100%: short content, does the table span the width?
    {
        QTextDocument d;
        d.setDefaultStyleSheet(css);
        d.setHtml(QStringLiteral("<html><body>") + codeBlock(QStringLiteral("int x = 1;").toHtmlEscaped()) + QStringLiteral("</body></html>"));
        d.setTextWidth(400);
        fprintf(stderr, "short: idealWidth=%f size=%fx%f\n", d.idealWidth(), d.size().width(), d.size().height());
    }
    return 0;
}
