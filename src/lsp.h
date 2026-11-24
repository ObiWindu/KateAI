/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace KateAi
{

// One language-server / editor diagnostic. Severity follows the LSP scale:
// 1 = Error, 2 = Warning, 3 = Information, 4 = Hint.
struct LspDiagnostic {
    QString path; // absolute path of the file
    int line = 0; // 1-based
    int column = 0; // 1-based, 0 = unknown
    int severity = 2;
    QString message;
    QString source; // e.g. the language server name, or "marks"
};

// One symbol of a document (class, function, ...). Lines are 1-based;
// endLine == 0 means the extent could not be determined.
struct LspSymbol {
    QString name;
    QString kind; // "class", "function", ...
    int line = 0;
    int column = 0;
    int endLine = 0;
};

struct LspLocation {
    QString path; // absolute path
    int line = 0; // 1-based
    int column = 0;
};

/**
 * The editor's language-server surface, as seen from the agent side. The core
 * (ToolRunner, AcpClient) only knows this interface; the Kate-facing
 * implementation (KateLspBridge) probes Kate's LSP client plugin at runtime and
 * degrades gracefully when it is absent or its internals change.
 *
 * The query methods have default "not provided" implementations: a provider
 * that cannot answer a query returns empty, and the tools fall back to the
 * project graph and plain text search.
 */
class LspProvider : public QObject
{
    Q_OBJECT

public:
    explicit LspProvider(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~LspProvider() override = default;

    // True when a language-server-capable plugin is loaded in this editor.
    virtual bool isAvailable() const = 0;
    // "loaded" / "none" today; richer states when the plugin exposes them.
    virtual QString serverState(const QString &path) const = 0;
    // Diagnostics for one file, or for every known file when path is empty.
    virtual QList<LspDiagnostic> diagnostics(const QString &path) const = 0;
    // Recent server log/show messages, newest last.
    virtual QStringList recentMessages(int maxCount) const = 0;

    virtual QList<LspSymbol> documentSymbols(const QString &path) const
    {
        Q_UNUSED(path);
        return {};
    }
    virtual QString hover(const QString &path, int line, int column) const
    {
        Q_UNUSED(path);
        Q_UNUSED(line);
        Q_UNUSED(column);
        return {};
    }
    virtual QList<LspLocation> definitions(const QString &path, int line, int column) const
    {
        Q_UNUSED(path);
        Q_UNUSED(line);
        Q_UNUSED(column);
        return {};
    }
    virtual QList<LspLocation> references(const QString &path, int line, int column, bool includeDeclaration) const
    {
        Q_UNUSED(path);
        Q_UNUSED(line);
        Q_UNUSED(column);
        Q_UNUSED(includeDeclaration);
        return {};
    }
    virtual QStringList codeActions(const QString &path, int line, int column) const
    {
        Q_UNUSED(path);
        Q_UNUSED(line);
        Q_UNUSED(column);
        return {};
    }

Q_SIGNALS:
    // The diagnostic set of one file changed (path empty = global change).
    void diagnosticsChanged(const QString &path);
    void availabilityChanged();
};

} // namespace KateAi
