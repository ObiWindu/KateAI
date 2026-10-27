/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "documentbridge.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringConverter>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QTextStream>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// The encoding of a file that is not valid UTF-8. Decoding it as UTF-8 would
// replace every non-ASCII byte with U+FFFD, and writing it back would
// destroy the file. The whole file is scanned, not just its head: a file that
// is valid UTF-8 for the first 64 KiB and latin-1 after it was misdetected
// before, and the write-back then silently corrupted the tail. The scan stops
// at the first decoding error, so a clean UTF-8 file still costs only one
// extra sequential read (the callers read it again right after).
QStringConverter::Encoding encodingFor(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringConverter::Utf8;
    }
    QStringDecoder decoder(QStringConverter::Utf8);
    char buffer[65536];
    while (true) {
        const qint64 read = file.read(buffer, sizeof(buffer));
        if (read <= 0) {
            break;
        }
        decoder.decode(QByteArrayView(buffer, static_cast<qsizetype>(read)));
        if (decoder.hasError()) {
            return QStringConverter::System;
        }
    }
    // An incomplete multi-byte sequence at the very end of the file is also
    // a decoding error; only the flush call can see it.
    decoder.decode(QByteArrayView());
    if (decoder.hasError()) {
        return QStringConverter::System;
    }
    return QStringConverter::Utf8;
}

} // namespace

bool DocumentBridge::readDocumentLines(const QString &path, int startLine, int maxLines,
                                       QStringList *page, int *totalLines) const
{
    QString contents;
    if (!readDocument(path, &contents)) {
        return false;
    }

    QStringList lines = contents.split(u'\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    if (totalLines) {
        *totalLines = lines.size();
    }
    if (page) {
        *page = lines.mid(qMax(0, startLine - 1), qMax(0, maxLines));
    }
    return true;
}

} // namespace KateAi

// The open KTextEditor document for a local path, or null when Kate does not
// have it open. The canonical (normalized) path is compared, so a path with
// symlinks or ../ segments still finds its document.
KTextEditor::Document *KateAi::documentForPath(const QString &path)
{
    // Get the global KTextEditor instance to access open documents
    auto *editor = KTextEditor::Editor::instance();
    if (!editor) {
        return nullptr;
    }
    if (path.trimmed().isEmpty()) {
        // Empty path previously resolved to CWD and could spuriously match a
        // document open on the working directory.
        return nullptr;
    }

    // Get the canonical (normalized) path for comparison
    const QString canonical = QFileInfo(path).canonicalFilePath().isEmpty() ? QFileInfo(path).absoluteFilePath()
                                                                            : QFileInfo(path).canonicalFilePath();

    // Iterate through all open documents to find a match
    for (KTextEditor::Document *doc : editor->documents()) {
        const QString local = doc->url().toLocalFile();
        if (local.isEmpty()) {
            continue;
        }

        // Get the canonical path of the document's local file
        const QString other = QFileInfo(local).canonicalFilePath().isEmpty() ? QFileInfo(local).absoluteFilePath()
                                                                             : QFileInfo(local).canonicalFilePath();

        // Check if the paths match (either canonical or local)
        if (QDir::cleanPath(other) == QDir::cleanPath(canonical) || QDir::cleanPath(local) == QDir::cleanPath(path)) {
            return doc;
        }
    }
    return nullptr;
}

namespace KateAi
{


bool DocumentBridge::restoreDocument(const QString &path, const QString &contents, QString *error)
{
    // A bridge without editor access restores by writing the file.
    return writeDocument(path, contents, error);
}

bool DocumentBridge::saveDocumentIfOpen(const QString &path, QString *error)
{
    // There is no editor buffer to save: nothing to do either way, so the
    // return value says "the path is settled" without an error.
    Q_UNUSED(path);
    Q_UNUSED(error);
    return true;
}

bool DiskDocumentBridge::readDocument(const QString &path, QString *contents) const
{
    if (path.trimmed().isEmpty()) {
        return false;
    }
    if (auto *doc = documentForPath(path)) {
        if (contents) {
            *contents = doc->text();
        }
        return true;
    }

    QFile file(path);
    // The file is opened in binary mode: the text mode translates the line
    // endings on Windows, which would change the file just by reading it.
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QTextStream in(&file);
    in.setEncoding(encodingFor(path));
    if (contents) {
        *contents = in.readAll();
    }
    return true;
}

bool DiskDocumentBridge::readDocumentLines(const QString &path, int startLine, int maxLines,
                                           QStringList *page, int *totalLines) const
{
    QStringList requestedLines;
    const int firstLine = qMax(0, startLine - 1);
    const int lineLimit = qMax(0, maxLines);
    int lineCount = 0;

    if (auto *doc = documentForPath(path)) {
        lineCount = doc->lines();
        if (lineCount > 0 && doc->line(lineCount - 1).isEmpty()) {
            --lineCount;
        }
        if (page) {
            const int lastLine = qMin(lineCount, firstLine + lineLimit);
            for (int index = firstLine; index < lastLine; ++index) {
                requestedLines.append(doc->line(index));
            }
        }
    } else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        QTextStream in(&file);
        in.setEncoding(encodingFor(path));
        // The loop ends when readLine() returns a null string. atEnd() is
        // unreliable on a buffered stream and can report the end too early,
        // which would cut the file short.
        for (;;) {
            const QString line = in.readLine();
            if (line.isNull()) {
                break;
            }
            if (page && lineCount >= firstLine && lineCount - firstLine < lineLimit) {
                requestedLines.append(line);
            }
            ++lineCount;
        }
    }

    if (page) {
        *page = requestedLines;
    }
    if (totalLines) {
        *totalLines = lineCount;
    }
    return true;
}

bool DiskDocumentBridge::writeDocument(const QString &path, const QString &contents, QString *error)
{
    if (path.trimmed().isEmpty()) {
        if (error) {
            *error = u"Path must not be empty."_s;
        }
        return false;
    }
    if (auto *doc = documentForPath(path)) {
        // The document is open and has unsaved changes that are not ours:
        // replacing the whole text would throw them away. An unsaved edit the
        // bridge itself applied earlier in buffer mode is "ours" and is exactly
        // the working state the next edit builds on.
        if (doc->isModified() && !m_bufferedPaths.contains(path)) {
            if (error) {
                *error = u"The document has unsaved changes; save it before the agent writes to it."_s;
            }
            return false;
        }
        if (!doc->setText(contents)) {
            if (error) {
                *error = u"Failed to update the open document."_s;
            }
            return false;
        }
        if (m_editInBuffer) {
            // The change stays in the buffer on purpose: the user reviews it
            // through Kate's modified state and undo, and the edit tracker
            // settles it -- accept saves, reject restores. Until then, tools
            // that read the file from disk still see the previous state.
            m_bufferedPaths.insert(path);
            return true;
        }
        m_bufferedPaths.remove(path);
        if (doc->isModified() && !doc->url().isEmpty() && !doc->save()) {
            if (error) {
                *error = u"The open document was updated but could not be saved."_s;
            }
            return false;
        }
        return true;
    }

    return writeDocumentToDisk(path, contents, error);
}

bool DiskDocumentBridge::restoreDocument(const QString &path, const QString &contents, QString *error)
{
    if (auto *doc = documentForPath(path)) {
        // A rejected edit puts the buffer back to the content it had before
        // the turn, also when the document is modified: in buffer mode the
        // modification is the agent's own edit being reverted, and in disk
        // mode the only modified state that reaches this path is the agent's
        // unsaved leftover of a failed save. The pre-edit content came from
        // the same buffer, so nothing of the user's is thrown away.
        if (!doc->setText(contents)) {
            if (error) {
                *error = u"Failed to restore the open document."_s;
            }
            return false;
        }
        m_bufferedPaths.remove(path);
        return true;
    }

    return writeDocumentToDisk(path, contents, error);
}

bool DiskDocumentBridge::saveDocumentIfOpen(const QString &path, QString *error)
{
    if (auto *doc = documentForPath(path)) {
        // An accepted edit is kept: the buffer content becomes the file.
        if (doc->isModified() && !doc->save()) {
            if (error) {
                *error = u"The open document could not be saved: %1"_s.arg(doc->url().toDisplayString());
            }
            return false;
        }
        m_bufferedPaths.remove(path);
        return true;
    }
    return true;
}

// Forgets which open documents carry an unsaved edit of the bridge's own.
// Called when a new turn starts; a path the user saved or reverted in the
// meantime is harmless, and a path that is still modified stops the agent
// from overwriting the user's changes, which is the safe direction.
void DiskDocumentBridge::resetBufferedEdits()
{
    m_bufferedPaths.clear();
}

bool DiskDocumentBridge::writeDocumentToDisk(const QString &path, const QString &contents, QString *error)
{
    const QDir dir = QFileInfo(path).dir();
    if (!dir.exists() && !dir.mkpath(u"."_s)) {
        if (error) {
            *error = u"Failed to create directory: %1"_s.arg(dir.absolutePath());
        }
        return false;
    }

    // A file that was not UTF-8 is written back in the encoding it had, but
    // only when the new content survives the round trip: an unrepresentable
    // character would otherwise be silently replaced with '?' and destroy
    // data while the tool reports success.
    const QStringConverter::Encoding encoding = encodingFor(path);
    if (encoding != QStringConverter::Utf8) {
        QStringEncoder encoder(encoding);
        QStringDecoder decoder(encoding);
        const QString roundTrip = decoder.decode(encoder.encode(contents));
        if (roundTrip != contents) {
            if (error) {
                *error = u"The new content cannot be represented in the file's original encoding; "
                         "save it as UTF-8 instead."_s;
            }
            return false;
        }
    }

    QSaveFile file(path);
    // Binary mode again: the text mode would translate the line endings of
    // the content, which already carries the line endings of the file.
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = u"Failed to open file for writing: %1"_s.arg(file.errorString());
        }
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(encoding);
    out << contents;
    if (!file.commit()) {
        if (error) {
            *error = u"Failed to write file: %1"_s.arg(file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace KateAi
