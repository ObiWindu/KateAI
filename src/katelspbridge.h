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

class QAbstractItemModel;
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
 * ("lspclientplugin") and Kate's per-window DiagnosticsView at runtime without
 * linking against private Kate headers, so a Kate without the plugin (or with
 * changed internals) degrades cleanly to KTextEditor::Document::marks().
 *
 * What it supplies:
 *  - availability of Kate's LSP client plugin and its window/show messages
 *    (LSPClientPlugin::showMessage);
 *  - rich diagnostics (real message text, exact line/column range, severity,
 *    source, code, related locations, and unopened workspace files) read from
 *    Kate's DiagnosticsView model (aggregating LSP, katebuildplugin, ESLint,
 *    and any other DiagnosticsProvider), falling back to document marks when
 *    DiagnosticsView is unavailable.
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

    // Extracts structured diagnostics from Kate's DiagnosticsView source model
    // (top-level file items with child diagnostic items). Exposed publicly so
    // unit tests can exercise the exact model layout without a live Kate window.
    static QList<LspDiagnostic> readDiagnosticsFromModel(const QAbstractItemModel *model,
                                                         const QString &wantedCanonicalPath = QString());

private Q_SLOTS:
    // The parameter type must be the enum itself, not int: the string-based
    // connect to LSPClientPlugin::showMessage matches on the normalized
    // signature, and "int" would not match "KTextEditor::Message::MessageType".
    void onLspMessage(KTextEditor::Message::MessageType level, const QString &msg);
    void onPluginCreated(const QString &name, QObject *plugin);
    void onPluginDeleted(const QString &name, QObject *plugin);
    void onPluginViewCreated(const QString &name, QObject *pluginView);
    void onDiagnosticsModelChanged();
    void pollOpenDocuments();

private:
    void probeLspPlugin();
    void probeDiagnosticsView();
    const QAbstractItemModel *diagnosticsSourceModel() const;
    void trackDocument(KTextEditor::Document *document);
    void refreshDocumentMarks(KTextEditor::Document *document);

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QPointer<QObject> m_lspPlugin;
    QPointer<QAbstractItemModel> m_diagnosticsModel;
    QStringList m_messages; // ring buffer of LSP window/show messages
    // Canonical path -> (errorCount, warningCount), the cached state the
    // polling timer compares against to emit diagnosticsChanged.
    QHash<QString, QPair<int, int>> m_markCounts;
    QHash<KTextEditor::Document *, QString> m_docPaths;
    QTimer *m_pollTimer = nullptr;
};

} // namespace KateAi
