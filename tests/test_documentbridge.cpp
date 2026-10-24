/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "documentbridge.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace

/**
 * Covers the Kate-facing half of DiskDocumentBridge: the interplay of open
 * documents, edit-in-buffer mode and the review settlement.
 *
 * The regression net for the buffer mode is about who may overwrite whose
 * unsaved changes: the agent must not build on the user's uncommitted work,
 * but must keep building on its own unsaved edit within one turn.
 */
class TestDocumentBridge : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void testDocumentForPathFindsOpenDocument();
    void testDiskWriteToOpenDocumentSaves();
    void testBufferWriteKeepsChangeUnsaved();
    void testAcceptSavesBuffer();
    void testRejectRestoresBuffer();
    void testBufferWriteRefusesUserModifiedDocument();
    void testBufferWriteBuildsOnOwnUnsavedEdit();
    void testResetBufferedEditsRevertsProtection();
    void testRejectRestoresClosedFileOnDisk();

private:
    QTemporaryDir m_dir;
    KTextEditor::Document *m_document = nullptr;
    QString m_path;
};

void TestDocumentBridge::initTestCase()
{
    auto *editor = KTextEditor::Editor::instance();
    QVERIFY2(editor, "no editor instance available");
    m_path = QDir(m_dir.path()).filePath(u"buffer.cpp"_s);
    QVERIFY(QFile::exists(m_dir.path()));

    m_document = editor->createDocument(nullptr);
    QVERIFY2(m_document, "no document could be created");
    m_document->openUrl(QUrl::fromLocalFile(m_path));
    m_document->setText(u"original\n"_s);
    QVERIFY(m_document->save());
    QVERIFY(!m_document->isModified());
}

void TestDocumentBridge::cleanup()
{
    // Every case leaves the document saved and clean, so the next one starts
    // from the same known state.
    if (m_document) {
        m_document->save();
    }
}

void TestDocumentBridge::testDocumentForPathFindsOpenDocument()
{
    QCOMPARE(documentForPath(m_path), m_document);
    // A path that is not open resolves to nothing, not to a stray document.
    QVERIFY(!documentForPath(QDir(m_dir.path()).filePath(u"other.cpp"_s)));
}

void TestDocumentBridge::testDiskWriteToOpenDocumentSaves()
{
    DiskDocumentBridge bridge;
    QString error;
    QVERIFY(bridge.writeDocument(m_path, u"disk mode\n"_s, &error));
    // The write of a clean open document saves: the disk and the buffer
    // agree, and the user is not left with a modified flag they did not
    // create.
    QVERIFY(!m_document->isModified());
    QCOMPARE(readFile(m_path), u"disk mode\n"_s);
    QCOMPARE(m_document->text(), u"disk mode\n"_s);
}

void TestDocumentBridge::testBufferWriteKeepsChangeUnsaved()
{
    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    const QString before = readFile(m_path);
    QString error;
    QVERIFY(bridge.writeDocument(m_path, u"agent edit\n"_s, &error));

    // The buffer carries the change; the disk deliberately still holds the
    // previous state, which is the whole point of the review flow.
    QCOMPARE(m_document->text(), u"agent edit\n"_s);
    QVERIFY(m_document->isModified());
    QCOMPARE(readFile(m_path), before);
}

void TestDocumentBridge::testAcceptSavesBuffer()
{
    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    QString error;
    QVERIFY(bridge.writeDocument(m_path, u"agent edit\n"_s, &error));
    QVERIFY(bridge.saveDocumentIfOpen(m_path, &error));

    // Accepting keeps the change: the buffer content becomes the file.
    QVERIFY(!m_document->isModified());
    QCOMPARE(readFile(m_path), u"agent edit\n"_s);
}

void TestDocumentBridge::testRejectRestoresBuffer()
{
    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    // The restore target is the pre-edit content, which in buffer mode is
    // what the disk still holds.
    const QString before = readFile(m_path);
    QString error;
    QVERIFY(bridge.writeDocument(m_path, u"agent edit\n"_s, &error));
    QVERIFY(bridge.restoreDocument(m_path, before, &error));

    // Rejecting puts the buffer back to the pre-edit content, even though the
    // document was modified -- the modification was the agent's own edit.
    // The modified flag itself stays set (Kate flags any programmatic setText,
    // equal content included), but the content is the pre-turn state again.
    QCOMPARE(m_document->text(), before);
    QCOMPARE(readFile(m_path), before);
}

void TestDocumentBridge::testBufferWriteRefusesUserModifiedDocument()
{
    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    // The user has uncommitted work the agent has never touched.
    const QString before = readFile(m_path);
    m_document->setText(u"user work\n"_s);
    QVERIFY(m_document->isModified());

    QString error;
    QVERIFY(!bridge.writeDocument(m_path, u"agent edit\n"_s, &error));
    QVERIFY(!error.isEmpty());
    // The refusal is the whole protection: the user's work stays untouched.
    QCOMPARE(m_document->text(), u"user work\n"_s);
    QCOMPARE(readFile(m_path), before);
}

void TestDocumentBridge::testBufferWriteBuildsOnOwnUnsavedEdit()
{
    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    const QString before = readFile(m_path);
    QString error;
    QVERIFY(bridge.writeDocument(m_path, u"first edit\n"_s, &error));

    // The second edit of the same turn must build on the first, although the
    // document is modified now. Refusing here would break every multi-edit
    // turn in buffer mode after the first change.
    QVERIFY(bridge.writeDocument(m_path, u"second edit\n"_s, &error));
    QCOMPARE(m_document->text(), u"second edit\n"_s);
    QVERIFY(m_document->isModified());
    QCOMPARE(readFile(m_path), before);
}

void TestDocumentBridge::testResetBufferedEditsRevertsProtection()
{
    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    QString error;
    QVERIFY(bridge.writeDocument(m_path, u"first edit\n"_s, &error));

    // A new turn starts: the unsaved edit is no longer "ours", so the agent
    // must not overwrite it. This is the safe direction for a stale reset.
    bridge.resetBufferedEdits();
    QVERIFY(!bridge.writeDocument(m_path, u"second turn\n"_s, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(m_document->text(), u"first edit\n"_s);
}

void TestDocumentBridge::testRejectRestoresClosedFileOnDisk()
{
    const QString closedPath = QDir(m_dir.path()).filePath(u"closed.cpp"_s);
    {
        QFile file(closedPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("before\n");
    }

    DiskDocumentBridge bridge;
    bridge.setEditInBuffer(true);
    QString error;
    QVERIFY(bridge.restoreDocument(closedPath, u"restored\n"_s, &error));

    // A path Kate does not have open restores by writing the file; there is
    // no buffer to put the content into.
    QCOMPARE(readFile(closedPath), u"restored\n"_s);
}

} // namespace KateAi

// QApplication, not QTEST_MAIN's QGuiApplication: creating a real KTextEditor
// document pulls in widget machinery.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    KateAi::TestDocumentBridge tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "test_documentbridge.moc"
