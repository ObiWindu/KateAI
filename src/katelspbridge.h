/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "lsp.h"

#include <KTextEditor/Message>

#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>

class QTimer;

namespace KTextEditor
{
class Document;
class MainWindow;
}

namespace KateAi
{

/**
 * The Kate-facing LspProvider. It talks to Kate's LSP client plugin
 * ("katelspclientplugin") the same way this codebase already talks to the
 * project plugin: through the QObject the editor hands out, probed at runtime
 * instead of linked against private headers, so a Kate without the plugin (or
 * with a changed one) just reports "not available".
 *
 * What it can supply today:
 *  - availability of the LSP plugin and its window/show messages
 *    (LSPClientPlugin::showMessage is a public signal);
 *  - diagnostics from the document marks of type Error/Warning, which is the
 *    public KTextEditor surface diagnostic marks are exposed on.
 *
 * Richer LSP queries (documentSymbol, hover, definition, references, code
 * actions) are left to the default "not provided" implementations; the agent
 * tools fall back to the project graph and text search for those.
 */
class KateLspBridge : public LspProvider
{
    Q_OBJECT

public:
    explicit KateLspBridge(QObject *parent = nullptr);
    ~KateLspBridge() override;

    void setMainWindow(KTextEditor::MainWindow *mainWindow);

    bool isAvailable() const override;
    QString serverState(const QString &path) const override;
    QList<LspDiagnostic> diagnostics(const QString &path) const override;
    QStringList recentMessages(int maxCount) const override;

private Q_SLOTS:
    // The parameter type must be the enum itself, not int: the string-based
    // connect to LSPClientPlugin::showMessage matches on the normalized
    // signature, and "int" would not match "KTextEditor::Message::MessageType".
    void onLspMessage(KTextEditor::Message::MessageType level, const QString &msg);
    void onPluginCreated(const QString &name, QObject *plugin);
    void onPluginDeleted(const QString &name, QObject *plugin);
    void onPluginViewCreated(const QString &name, QObject *pluginView);
    void pollOpenDocuments();

private:
    void probeLspPlugin();
    void trackDocument(KTextEditor::Document *document);
    void refreshDocumentMarks(KTextEditor::Document *document);

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QPointer<QObject> m_lspPlugin;
    QStringList m_messages; // ring buffer of LSP window/show messages
    // Canonical path -> (errorCount, warningCount), the cached state the
    // polling timer compares against to emit diagnosticsChanged.
    QHash<QString, QPair<int, int>> m_markCounts;
    QHash<KTextEditor::Document *, QString> m_docPaths;
    QTimer *m_pollTimer = nullptr;
};

} // namespace KateAi
