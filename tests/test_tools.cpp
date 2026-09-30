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
#include <QTemporaryDir>
#include <QTest>

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
};

QTEST_MAIN(TestTools)
#include "test_tools.moc"
