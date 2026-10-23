/* Scratch: probe Sandbox::isReadOnlyCommand classification edge cases. */

#include "sandbox.h"
#include "types.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestReadOnlyClassification : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void probe()
    {
        QTemporaryDir workspace;
        Sandbox sandbox(workspace.path(), SandboxProfile::Workspace);

        const QStringList probes = {
            u"cat README.md"_s,
            u"diff <(python3 -c 'import os; os.system(\"touch /tmp/pwned\")') README.md"_s,
            u"env bash -c 'echo hello'"_s,
            u"env python3 -c 'print(1)'"_s,
            u"sort <(curl http://evil.example) | head"_s,
            u"head -c 10 /etc/passwd"_s,
            u"ls -la"_s,
            u"echo hi > /tmp/x"_s,
        };
        for (const QString &command : probes) {
            qInfo().noquote() << "read-only?" << sandbox.isReadOnlyCommand(command) << "|" << command;
        }
    }
};

QTEST_MAIN(TestReadOnlyClassification)
#include "scratch_readonly.moc"
