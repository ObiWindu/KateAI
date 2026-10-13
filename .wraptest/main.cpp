#include <QApplication>
#include <QTextDocument>
#include <cstdio>

static const char *css =
    "body { color: #e4e4e4; font-family: sans-serif; font-size: 13px; margin: 0; padding: 0; }"
    "p { margin-bottom: 8px; line-height: 1.5; }"
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
    const QString longLine = QStringLiteral("const int someExtremelyLongVariableName = anotherVeryLongFunctionCall(withArguments, andMoreArguments, yetAnotherArgumentThatRunsOn, andOn, andOn);");
    const QString docHtml = QStringLiteral("<html><body>") + codeBlock(longLine.toHtmlEscaped()) + QStringLiteral("</body></html>");

    QTextDocument d;
    d.setDefaultStyleSheet(QString::fromUtf8(css));
    d.setHtml(docHtml);
    d.setTextWidth(600);
    fprintf(stderr, "idealWidth=%f docsize=%fx%f\n", d.idealWidth(), d.size().width(), d.size().height());
    return 0;
}
