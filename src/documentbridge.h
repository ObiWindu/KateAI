/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace KTextEditor
{
class Document;
}

namespace KateAi
{

class DocumentBridge
{
public:
    virtual ~DocumentBridge() = default;
    virtual bool readDocument(const QString &path, QString *contents) const = 0;
    virtual bool readDocumentLines(const QString &path, int startLine, int maxLines,
                                   QStringList *page, int *totalLines) const;
    virtual bool writeDocument(const QString &path, const QString &contents, QString *error) = 0;
    // Restores a file the agent changed to an earlier content. The default is
    // a plain write; the Kate bridge can instead put the content back into the
    // open buffer, which is the review semantics of edit-in-buffer mode.
    virtual bool restoreDocument(const QString &path, const QString &contents, QString *error);
    // Saves the document when the path is open in the editor and has unsaved
    // content. Returns true when there is nothing to do (the path is not open)
    // as well, so callers can call it unconditionally after accepting an edit.
    virtual bool saveDocumentIfOpen(const QString &path, QString *error);
};

class DiskDocumentBridge : public DocumentBridge
{
public:
    bool readDocument(const QString &path, QString *contents) const override;
    bool readDocumentLines(const QString &path, int startLine, int maxLines,
                           QStringList *page, int *totalLines) const override;
    bool writeDocument(const QString &path, const QString &contents, QString *error) override;
    bool restoreDocument(const QString &path, const QString &contents, QString *error) override;
    bool saveDocumentIfOpen(const QString &path, QString *error) override;

    // Edit-in-buffer mode: writes to documents that are open in the editor
    // land in the buffer and stay unsaved, so the user reviews and saves
    // through Kate's own modified-state and undo machinery.
    void setEditInBuffer(bool enabled)
    {
        m_editInBuffer = enabled;
    }
    bool editInBuffer() const
    {
        return m_editInBuffer;
    }
    // Forgets which open documents carry an unsaved edit the bridge applied
    // itself. Called at the start of a turn; see the comment in the .cpp.
    void resetBufferedEdits();

private:
    // The disk half of writeDocument, shared with restoreDocument for paths
    // that Kate does not have open.
    bool writeDocumentToDisk(const QString &path, const QString &contents, QString *error);

    bool m_editInBuffer = false;
    // Open paths whose current modification is the bridge's own unsaved
    // edit, so a later write may build on it instead of refusing.
    QSet<QString> m_bufferedPaths;
};

// The open KTextEditor document for a local path, or null when Kate does not
// have it open. Shared by the bridge and the plugin view, so the banner and
// the write path agree on what "open" means.
KTextEditor::Document *documentForPath(const QString &path);

} // namespace KateAi
