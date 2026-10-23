/* Scratch: does ToolRunner::runBashAsync lose the final output chunk? */

#include "tools.h"
#include "types.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestBashDrain : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void asyncBashKeepsFinalOutput()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Sandbox sandbox(workspace.path(), SandboxProfile::Off);
        int failures = 0;
        for (int i = 0; i < 60; ++i) {
            ToolRunner runner(sandbox, nullptr);

            ToolCall call;
            call.id = QStringLiteral("call-%1").arg(i);
            call.name = u"bash"_s;
            call.arguments = QJsonObject{{u"command"_s, u"echo FINAL-OUTPUT-MARKER"_s}};

            bool finished = false;
            ToolResult result;
            connect(&runner, &ToolRunner::bashFinished, &runner, [&finished, &result](const ToolResult &r) {
                result = r;
                finished = true;
            });

            runner.runBashAsync(call);

            QElapsedTimer timer;
            timer.start();
            while (!finished && timer.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QTest::qWait(1);
            }
            QVERIFY2(finished, "bash never finished");
            if (!result.output.contains(u"FINAL-OUTPUT-MARKER"_s, Qt::CaseInsensitive)) {
                ++failures;
                qInfo() << "iteration" << i << "lost output, got:" << result.output;
            }
        }
        qInfo() << "lost-output iterations:" << failures << "of 60";
        QVERIFY2(failures == 0, "the final chunk of output was lost before the result was reported");
    }
};

QTEST_MAIN(TestBashDrain)
#include "scratch_bashdrain.moc"
