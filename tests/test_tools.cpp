/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "documentbridge.h"
#include "sandbox.h"
#include "tools.h"
#include "types.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestTools : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testMultiEditFileBasic()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/sample.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("Line 1: alpha\nLine 2: beta\nLine 3: gamma\nLine 4: delta\n");
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        QJsonObject chunk1{
            {u"old_string"_s, u"alpha"_s},
            {u"new_string"_s, u"ALPHA"_s}
        };
        QJsonObject chunk2{
            {u"old_string"_s, u"delta"_s},
            {u"new_string"_s, u"DELTA"_s}
        };
        QJsonArray edits{chunk1, chunk2};

        ToolCall call;
        call.id = u"call-1"_s;
        call.name = u"multi_edit_file"_s;
        call.arguments = QJsonObject{
            {u"path"_s, u"sample.txt"_s},
            {u"edits"_s, edits}
        };

        ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));
        QVERIFY(res.output.contains(u"2 edit chunks applied"_s));

        QString updatedContent;
        QVERIFY(bridge.readDocument(filePath, &updatedContent));
        QCOMPARE(updatedContent, u"Line 1: ALPHA\nLine 2: beta\nLine 3: gamma\nLine 4: DELTA\n"_s);
    }

    void testMultiEditFileAntigravityAlias()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/code.py"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("def foo():\n    return 1\n\ndef bar():\n    return 2\n");
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        QJsonObject chunk1{
            {u"TargetContent"_s, u"return 1"_s},
            {u"ReplacementContent"_s, u"return 100"_s}
        };
        QJsonObject chunk2{
            {u"TargetContent"_s, u"return 2"_s},
            {u"ReplacementContent"_s, u"return 200"_s}
        };
        QJsonArray chunks{chunk1, chunk2};

        ToolCall call;
        call.id = u"call-2"_s;
        call.name = u"multi_replace_file_content"_s;
        call.arguments = QJsonObject{
            {u"TargetFile"_s, u"code.py"_s},
            {u"ReplacementChunks"_s, chunks}
        };

        ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));

        QString updatedContent;
        QVERIFY(bridge.readDocument(filePath, &updatedContent));
        QCOMPARE(updatedContent, u"def foo():\n    return 100\n\ndef bar():\n    return 200\n"_s);
    }

    void testMultiEditFileAtomicFailure()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString original = u"Line 1: first\nLine 2: second\nLine 3: third\n"_s;
        const QString filePath = tempDir.path() + u"/atomic.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(original.toUtf8());
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        QJsonObject chunk1{
            {u"old_string"_s, u"first"_s},
            {u"new_string"_s, u"FIRST"_s}
        };
        QJsonObject chunk2{
            {u"old_string"_s, u"non_existent_string"_s},
            {u"new_string"_s, u"SOMETHING"_s}
        };
        QJsonArray edits{chunk1, chunk2};

        ToolCall call;
        call.id = u"call-3"_s;
        call.name = u"multi_edit_file"_s;
        call.arguments = QJsonObject{
            {u"path"_s, u"atomic.txt"_s},
            {u"edits"_s, edits}
        };

        ToolResult res = runner.run(call);
        QVERIFY(!res.ok);
        QVERIFY(res.output.contains(u"Chunk 2 of 2"_s));
        QVERIFY(res.output.contains(u"No edits were applied"_s));

        // Verify file remains unmodified
        QString currentContent;
        QVERIFY(bridge.readDocument(filePath, &currentContent));
        QCOMPARE(currentContent, original);
    }

    void testMultiEditFileReplaceAll()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/replace_all.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("int count = 0;\ncount++;\ncount++;\n");
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        // Without replace_all it should fail on multiple occurrences
        QJsonObject chunkFail{
            {u"old_string"_s, u"count"_s},
            {u"new_string"_s, u"counter"_s},
            {u"replace_all"_s, false}
        };
        ToolCall callFail;
        callFail.id = u"call-4"_s;
        callFail.name = u"multi_edit_file"_s;
        callFail.arguments = QJsonObject{
            {u"path"_s, u"replace_all.txt"_s},
            {u"edits"_s, QJsonArray{chunkFail}}
        };
        ToolResult resFail = runner.run(callFail);
        QVERIFY(!resFail.ok);
        QVERIFY(resFail.output.contains(u"matched 3 times"_s));

        // With replace_all it should succeed
        QJsonObject chunkSuccess{
            {u"old_string"_s, u"count"_s},
            {u"new_string"_s, u"counter"_s},
            {u"replace_all"_s, true}
        };
        ToolCall callSuccess;
        callSuccess.id = u"call-5"_s;
        callSuccess.name = u"multi_edit_file"_s;
        callSuccess.arguments = QJsonObject{
            {u"path"_s, u"replace_all.txt"_s},
            {u"edits"_s, QJsonArray{chunkSuccess}}
        };
        ToolResult resSuccess = runner.run(callSuccess);
        QVERIFY2(resSuccess.ok, qPrintable(resSuccess.output));

        QString updatedContent;
        QVERIFY(bridge.readDocument(filePath, &updatedContent));
        QCOMPARE(updatedContent, u"int counter = 0;\ncounter++;\ncounter++;\n"_s);
    }

    void testDescribeDiffMultiEdit()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/diff_test.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("Line 1: old\nLine 2: keep\nLine 3: old\n");
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        QJsonObject chunk1{
            {u"old_string"_s, u"Line 1: old"_s},
            {u"new_string"_s, u"Line 1: new"_s}
        };
        QJsonObject chunk2{
            {u"old_string"_s, u"Line 3: old"_s},
            {u"new_string"_s, u"Line 3: new"_s}
        };
        ToolCall call;
        call.id = u"call-6"_s;
        call.name = u"multi_edit_file"_s;
        call.arguments = QJsonObject{
            {u"path"_s, u"diff_test.txt"_s},
            {u"edits"_s, QJsonArray{chunk1, chunk2}}
        };

        PermissionRequest req = runner.describe(call);
        QCOMPARE(req.risk, ToolRisk::Write);
        QVERIFY(req.describeDiff.contains(u"-Line 1: old"_s));
        QVERIFY(req.describeDiff.contains(u"+Line 1: new"_s));
        QVERIFY(req.describeDiff.contains(u"-Line 3: old"_s));
        QVERIFY(req.describeDiff.contains(u"+Line 3: new"_s));
    }

    void bashLargeOutputIsCaptured()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // "Off" keeps the command out of bubblewrap so the test needs no helper.
        Sandbox sandbox(tempDir.path(), SandboxProfile::Off);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);
        runner.setTimeoutMs(20000);

        ToolCall call;
        call.id = u"bash-large"_s;
        call.name = u"bash"_s;
        // 1 MiB of output is far more than a pipe buffer holds. If the runner
        // does not drain the child while it runs, the command blocks on write
        // and is reported as a timeout instead of completing.
        call.arguments = QJsonObject{{u"command"_s, u"(echo HEAD_MARKER; head -c 1048576 /dev/zero | tr '\\0' 'x'; echo TAIL_MARKER)"_s}};

        ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));
        // clip() keeps the first and second halves of the 80000-character
        // budget and reports what it dropped, so the markers at both ends of
        // the stream prove the whole output arrived.
        QVERIFY2(res.output.contains(u"truncated"_s), qPrintable(res.output.left(200)));
        QVERIFY2(res.output.contains(u"HEAD_MARKER"_s), qPrintable(res.output.left(200)));
        QVERIFY2(res.output.contains(u"TAIL_MARKER"_s), qPrintable(res.output.right(200)));
    }

    void testMultiEditFileOverlapRejected()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/overlap.txt"_s;
        const QString original = QStringLiteral("one two\n");
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(original.toUtf8());
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        // Both old_strings exist in the original file, so the pre-pass accepts
        // them. But the first edit produces the text the second one matches,
        // so the second edit would land inside text the first one just wrote.
        QJsonObject chunk1{
            {u"old_string"_s, u"two"_s},
            {u"new_string"_s, u"one two"_s}
        };
        QJsonObject chunk2{
            {u"old_string"_s, u"one two"_s},
            {u"new_string"_s, u"X"_s}
        };

        ToolCall call;
        call.id = u"call-overlap"_s;
        call.name = u"multi_edit_file"_s;
        call.arguments = QJsonObject{
            {u"path"_s, u"overlap.txt"_s},
            {u"edits"_s, QJsonArray{chunk1, chunk2}}
        };

        ToolResult res = runner.run(call);
        QVERIFY2(!res.ok, qPrintable(res.output));
        QVERIFY2(res.output.contains(u"overlaps text an earlier edit"_s), qPrintable(res.output));

        // Nothing was written: the batch is all-or-nothing.
        QString currentContent;
        QVERIFY(bridge.readDocument(filePath, &currentContent));
        QCOMPARE(currentContent, original);
    }

    void testMultiEditFileOverlapAcrossDeletion()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/overlap_del.txt"_s;
        const QString original = QStringLiteral("j;k\njk\n");
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(original.toUtf8());
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        // The first edit deletes the separator, so the second edit's
        // old_string would span the join the deletion created.
        QJsonObject chunk1{
            {u"old_string"_s, u";"_s},
            {u"new_string"_s, QString()}
        };
        QJsonObject chunk2{
            {u"old_string"_s, u"jk"_s},
            {u"new_string"_s, u"joined"_s},
            {u"replace_all"_s, true}
        };

        ToolCall call;
        call.id = u"call-overlap-del"_s;
        call.name = u"multi_edit_file"_s;
        call.arguments = QJsonObject{
            {u"path"_s, u"overlap_del.txt"_s},
            {u"edits"_s, QJsonArray{chunk1, chunk2}}
        };

        ToolResult res = runner.run(call);
        QVERIFY2(!res.ok, qPrintable(res.output));

        QString currentContent;
        QVERIFY(bridge.readDocument(filePath, &currentContent));
        QCOMPARE(currentContent, original);
    }

    void testMultiEditFileDisjointEditsStillApply()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/disjoint.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("keep one two remove three\n");
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        // Edits that change length in both directions and never touch each
        // other must still apply; the overlap check must not over-reject.
        QJsonObject chunk1{
            {u"old_string"_s, u"one"_s},
            {u"new_string"_s, u"1"_s}
        };
        QJsonObject chunk2{
            {u"old_string"_s, u" two "_s},
            {u"new_string"_s, QString()}
        };
        QJsonObject chunk3{
            {u"old_string"_s, u"three"_s},
            {u"new_string"_s, u"3 -> done"_s}
        };

        ToolCall call;
        call.id = u"call-disjoint"_s;
        call.name = u"multi_edit_file"_s;
        call.arguments = QJsonObject{
            {u"path"_s, u"disjoint.txt"_s},
            {u"edits"_s, QJsonArray{chunk1, chunk2, chunk3}}
        };

        ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));
        QVERIFY(res.output.contains(u"3 edit chunks applied"_s));

        QString updated;
        QVERIFY(bridge.readDocument(filePath, &updated));
        QCOMPARE(updated, u"keep 1remove 3 -> done\n"_s);
    }

    void bashReportsFailingExitCode()
    {
        QTemporaryDir tempDir;
        Sandbox sandbox(tempDir.path(), SandboxProfile::Off);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);
        runner.setTimeoutMs(10000);

        ToolCall call;
        call.id = u"bash-fail"_s;
        call.name = u"bash"_s;
        call.arguments = QJsonObject{{u"command"_s, u"echo boom >&2; exit 3"_s}};

        ToolResult res = runner.run(call);
        QVERIFY(!res.ok);
        QVERIFY(res.output.contains(u"boom"_s));
        QVERIFY(res.output.contains(u"exit code 3"_s));
    }

    void writePlanFileWritesTempFileTheUserCanFollow()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // Read-only on purpose: the plan file is Kate's own temp file, so even
        // a sandbox that blocks every workspace write must not stop the agent
        // from writing its plan down.
        Sandbox sandbox(tempDir.path(), SandboxProfile::ReadOnly);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"plan-1"_s;
        call.name = u"write_plan_file"_s;
        call.arguments = QJsonObject{{u"content"_s, u"# Plan\n\n1. Read the code\n2. Change it\n"_s}};

        const ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));

        const QString planPath = planFilePath(tempDir.path());
        QVERIFY2(planPath.startsWith(QDir::tempPath()), qPrintable(planPath));
        QString written;
        QVERIFY(bridge.readDocument(planPath, &written));
        QCOMPARE(written, u"# Plan\n\n1. Read the code\n2. Change it\n"_s);

        // The plan is the agent's notes, not a project file: nothing may land
        // inside the workspace.
        QCOMPARE(QDir(tempDir.path()).entryList(QDir::Files), QStringList());

        // A second call updates the same file the user already has open.
        call.arguments = QJsonObject{{u"content"_s, u"# Plan\n\n1. Read the code (done)\n"_s}};
        const ToolResult second = runner.run(call);
        QVERIFY2(second.ok, qPrintable(second.output));
        QVERIFY(bridge.readDocument(planPath, &written));
        QCOMPARE(written, u"# Plan\n\n1. Read the code (done)\n"_s);
    }

    void writePlanFileRejectsEmptyContentAndStaysReadRisk()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"plan-empty"_s;
        call.name = u"write_plan_file"_s;
        call.arguments = QJsonObject{{u"content"_s, u"   "_s}};

        const ToolResult res = runner.run(call);
        QVERIFY(!res.ok);

        // Read risk: it never prompts, and it must not be mistaken for a
        // workspace write the edit tracker should queue.
        call.arguments = QJsonObject{{u"content"_s, u"notes"_s}};
        const PermissionRequest req = runner.describe(call);
        QCOMPARE(req.risk, ToolRisk::Read);
        QVERIFY(!req.mutatesFiles);
    }

    void suggestPostsAdviceWithoutTouchingTheWorkspace()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"suggest-1"_s;
        call.name = u"suggest"_s;
        call.arguments = QJsonObject{
            {u"suggestion"_s, u"Use string_view here instead of a const reference"_s},
            {u"path"_s, u"main.cpp"_s},
            {u"line"_s, 12},
        };

        const ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));

        // Advice is not a change: nothing is written anywhere, and the anchored
        // file is not created -- a suggestion may point at a file the work has
        // not made yet.
        QCOMPARE(QDir(tempDir.path()).entryList(QDir::Files), QStringList());
        QCOMPARE(QDir(tempDir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList());
    }

    void suggestRejectsEmptyTextAndBadLines()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"suggest-empty"_s;
        call.name = u"suggest"_s;
        call.arguments = QJsonObject{{u"suggestion"_s, u"   "_s}};
        QVERIFY(!runner.run(call).ok);

        call.arguments = QJsonObject{{u"suggestion"_s, u"Do the thing"_s}, {u"line"_s, 0}};
        const ToolResult badLine = runner.run(call);
        QVERIFY2(!badLine.ok, qPrintable(badLine.output));
        QVERIFY2(badLine.output.contains(u"1-based"_s), qPrintable(badLine.output));
    }

#ifdef Q_OS_UNIX
    void bashTimeoutStopsBackgroundChildren()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        Sandbox sandbox(tempDir.path(), SandboxProfile::Off);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);
        runner.setTimeoutMs(1500);

        ToolCall call;
        call.id = u"bash-timeout"_s;
        call.name = u"bash"_s;
        // The shell backgrounds a writer and then blocks. Killing the shell on
        // timeout must not leave the writer behind to touch the workspace.
        call.arguments = QJsonObject{
            {u"command"_s, u"sh -c 'sleep 2; echo survived > marker.txt' & sleep 30"_s}
        };

        const ToolResult res = runner.run(call);
        QVERIFY(!res.ok);
        QVERIFY(res.output.contains(u"timed out"_s));

        const QString marker = tempDir.path() + u"/marker.txt"_s;
        QTest::qWait(3500);
        QVERIFY2(!QFile::exists(marker), "a background child survived the timeout and kept writing");
    }
#endif

    void questionIsReadRiskAndCarriesItsPrompt()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Sandbox sandbox(tempDir.path(), SandboxProfile::ReadOnly);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"question-1"_s;
        call.name = u"question"_s;
        call.arguments = QJsonObject{
            {u"question"_s, u"Rewrite the file, or patch it in place?"_s},
            {u"options"_s, QJsonArray{u"Rewrite"_s, u"Patch in place"_s}},
        };

        const PermissionRequest req = runner.describe(call);
        // Asking the user something touches the workspace in no way at all, so
        // it must never prompt and never look like a mutation.
        QCOMPARE(req.risk, ToolRisk::Read);
        QVERIFY(!req.mutatesFiles);
        QCOMPARE(req.questionText, u"Rewrite the file, or patch it in place?"_s);
        QCOMPARE(req.questionOptions, QStringList({u"Rewrite"_s, u"Patch in place"_s}));
        QVERIFY(req.allowFreeform);
        QVERIFY(req.summary.contains(u"Rewrite the file"_s));

        // The runner itself can never answer a question: only the user can, so
        // reaching it means the loop failed to intercept, which must say so
        // rather than fabricate an answer.
        const ToolResult res = runner.run(call);
        QVERIFY2(!res.ok, qPrintable(res.output));
        QVERIFY2(res.output.contains(u"user"_s), qPrintable(res.output));
    }

    void questionRequiresAnInteractiveUser()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"question-2"_s;
        call.name = u"question"_s;
        call.arguments = QJsonObject{{u"question"_s, u"Which approach?"_s}, {u"allow_freeform"_s, false}};
        const PermissionRequest req = runner.describe(call);
        QVERIFY(!req.allowFreeform);
    }

    void readingAnEmptyFileIsNotAFailure()
    {
        // The agent reads back files it just created, and a zero-byte file is a
        // legitimate read. Reporting "offset is past the end" made a valid read
        // look like a failed tool call.
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString filePath = tempDir.path() + u"/empty.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"read-empty"_s;
        call.name = u"read_file"_s;
        call.arguments = QJsonObject{{u"path"_s, u"empty.txt"_s}};
        const ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));
        QVERIFY(!res.output.isEmpty());
    }

    void unifiedDiffNumbersAZeroLengthRange()
    {
        // A hunk with no old lines is an insertion, and the unified format
        // writes its range as "<line before>,0". An omitted count means one
        // line, which no zero-length range can claim, and the line before an
        // insertion at the very top of a file is 0.
        const QString created = unifiedDiff(u"a.txt"_s, QString(), u"X\nY\n"_s);
        QVERIFY2(created.contains(u"@@ -0,0 +1,2 @@"_s), qPrintable(created));

        const QString emptied = unifiedDiff(u"a.txt"_s, u"a\nb\n"_s, QString());
        QVERIFY2(emptied.contains(u"@@ -1,2 +0,0 @@"_s), qPrintable(emptied));

        // Context around the insertion keeps both ranges non-empty, so the
        // ordinary "start" form still applies inside an existing file.
        const QString inserted = unifiedDiff(u"a.txt"_s, u"a\nb\n"_s, u"X\na\nb\n"_s);
        QVERIFY2(inserted.contains(u"@@ -1,2 +1,3 @@"_s), qPrintable(inserted));
    }

#ifdef Q_OS_UNIX
    void bashStreamsItsOutputAsItRuns()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        Sandbox sandbox(tempDir.path(), SandboxProfile::Off);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"bash-stream"_s;
        call.name = u"bash"_s;
        call.arguments = QJsonObject{{u"command"_s, u"echo one; sleep 0.4; echo two; sleep 0.4; echo three"_s}};

        QStringList progress;
        bool finished = false;
        ToolResult finalResult;
        connect(&runner, &ToolRunner::bashProgress, this, [&](const QString &id, const QString &partial) {
            if (id == call.id) {
                progress << partial;
            }
        });
        connect(&runner, &ToolRunner::bashFinished, this, [&](const ToolResult &result) {
            finished = true;
            finalResult = result;
        });

        runner.runBashAsync(call);
        // The first chunk has to arrive well before the command finishes: that
        // is the whole point of streaming it.
        QTRY_VERIFY(!progress.isEmpty() && !finished);
        QTRY_VERIFY(finished);
        QVERIFY2(finalResult.ok, qPrintable(finalResult.output));
        QVERIFY(finalResult.output.contains(u"one"_s));
        QVERIFY(finalResult.output.contains(u"three"_s));
        // Every emission is the output so far, never a bare delta.
        QVERIFY(!progress.isEmpty());
        int previousSize = 0;
        for (const QString &partial : progress) {
            QVERIFY2(partial.size() >= previousSize, "a later chunk lost output");
            previousSize = partial.size();
        }
        QVERIFY(progress.last().trimmed() == finalResult.output.trimmed());
    }
    void readFilePaginationNoteMatchesWhatWasShown()
    {
        // The "N more line(s)" note is the model's only cue that it has to read
        // on. It used to be computed from the slice the code *intended* to show
        // while the body itself was clipped at 80000 characters, so a long file
        // reported nothing left while most of it had never been displayed.
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const int totalLines = 5000;
        QString body;
        for (int i = 0; i < totalLines; ++i) {
            body += u"%1 %2\n"_s.arg(i + 1).arg(QString(60, u'x'));
        }
        const QString filePath = tempDir.path() + u"/big.txt"_s;
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(body.toUtf8());
        }

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        ToolCall call;
        call.id = u"read-1"_s;
        call.name = u"read_file"_s;
        call.arguments = QJsonObject{{u"path"_s, u"big.txt"_s}};

        const ToolResult res = runner.run(call);
        QVERIFY2(res.ok, qPrintable(res.output));

        // Everything before the note is file content, so it must stay inside
        // the observation budget and end on a line boundary.
        const int noteAt = res.output.indexOf(u"\n["_s);
        QVERIFY2(noteAt > 0, qPrintable(res.output.right(200)));
        const QString bodyShown = res.output.left(noteAt);
        QVERIFY2(bodyShown.size() <= 80000, qPrintable(QString::number(bodyShown.size())));

        // Highest line number actually displayed.
        int lastShown = 0;
        for (const QString &line : bodyShown.split(u'\n')) {
            const int pipe = line.indexOf(u'|');
            if (pipe > 0) {
                bool ok = false;
                const int number = line.left(pipe).trimmed().toInt(&ok);
                if (ok && number > lastShown) {
                    lastShown = number;
                }
            }
        }
        QVERIFY(lastShown > 0 && lastShown < totalLines);

        const QString note = res.output.mid(noteAt);
        QVERIFY2(note.contains(u"more line(s) not shown"_s), qPrintable(note));
        QVERIFY2(note.contains(u"%1 more line(s)"_s.arg(totalLines - lastShown)), qPrintable(note));
        QVERIFY2(note.contains(u"offset=%1"_s.arg(lastShown + 1)), qPrintable(note));
    }
#endif

    void testGoalReportValidation()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        Sandbox sandbox(tempDir.path(), SandboxProfile::Workspace);
        DiskDocumentBridge bridge;
        ToolRunner runner(sandbox, &bridge);

        // Missing status
        ToolCall call1;
        call1.id = u"g1"_s;
        call1.name = u"goal_report"_s;
        call1.arguments = QJsonObject{{u"reason"_s, u"done"_s}};
        ToolResult res1 = runner.run(call1);
        QVERIFY(!res1.ok);

        // Invalid status
        ToolCall call2;
        call2.id = u"g2"_s;
        call2.name = u"goal_report"_s;
        call2.arguments = QJsonObject{{u"status"_s, u"invalid"_s}, {u"reason"_s, u"why"_s}};
        ToolResult res2 = runner.run(call2);
        QVERIFY(!res2.ok);

        // Missing reason
        ToolCall call3;
        call3.id = u"g3"_s;
        call3.name = u"goal_report"_s;
        call3.arguments = QJsonObject{{u"status"_s, u"complete"_s}};
        ToolResult res3 = runner.run(call3);
        QVERIFY(!res3.ok);

        // Valid cases
        ToolCall call4;
        call4.id = u"g4"_s;
        call4.name = u"goal_report"_s;
        call4.arguments = QJsonObject{{u"status"_s, u"complete"_s}, {u"reason"_s, u"Finished task"_s}};
        ToolResult res4 = runner.run(call4);
        QVERIFY(res4.ok);

        ToolCall call5;
        call5.id = u"g5"_s;
        call5.name = u"goal_report"_s;
        call5.arguments = QJsonObject{{u"status"_s, u"failed"_s}, {u"reason"_s, u"Error occurred"_s}};
        ToolResult res5 = runner.run(call5);
        QVERIFY(res5.ok);

        ToolCall call6;
        call6.id = u"g6"_s;
        call6.name = u"goal_report"_s;
        call6.arguments = QJsonObject{{u"status"_s, u"blocked"_s}, {u"reason"_s, u"Waiting for input"_s}};
        ToolResult res6 = runner.run(call6);
        QVERIFY(res6.ok);
    }

};

QTEST_MAIN(TestTools)
#include "test_tools.moc"