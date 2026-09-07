#include "documentbridge.h"

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

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
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    if (contents) {
        *contents = in.readAll();
    }
    return true;
}

bool DiskDocumentBridge::writeDocument(const QString &path, const QString &contents, QString *error)
{
    if (auto *doc = openDocumentFor(path)) {
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

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = u"Failed to open file for writing: %1"_s.arg(file.errorString());
        }
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
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
