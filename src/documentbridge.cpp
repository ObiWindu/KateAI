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

static KTextEditor::Document *openDocumentFor(const QString &path)
{
    // Get the global KTextEditor instance to access open documents
    auto *editor = KTextEditor::Editor::instance();
    if (!editor) {
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

bool DiskDocumentBridge::readDocument(const QString &path, QString *contents) const
{
    if (auto *doc = openDocumentFor(path)) {
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

    if (auto *doc = openDocumentFor(path)) {
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
    if (auto *doc = openDocumentFor(path)) {
        // The document is open and the user has unsaved changes in it.
        // Replacing the whole text would throw those changes away.
        if (doc->isModified()) {
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
        if (doc->isModified() && !doc->url().isEmpty() && !doc->save()) {
            if (error) {
                *error = u"The open document was updated but could not be saved."_s;
            }
            return false;
        }
        return true;
    }

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
