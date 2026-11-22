/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>

namespace KateAi
{

struct StoredToolResult {
    QString id;
    qint64 totalChars = 0;
    // Head + tail with an explicit truncation marker. Empty when the text did
    // not need storing.
    QString preview;
};

/**
 * Content store for tool output that would otherwise flood the context window.
 *
 * The full text lives in a per-user cache directory and the conversation only
 * carries a bounded preview plus an id. `read_tool_result` pages the stored
 * text back. Ids are validated, so a model cannot use the tool to read an
 * arbitrary path.
 */
class ToolResults
{
public:
    static QString directory();

    // Stores `text` and returns a preview. Very large inputs are capped at
    // 32 MiB so a runaway command cannot fill the disk.
    static StoredToolResult store(const QString &text, int headChars = 8000, int tailChars = 4000);

    // Reads a slice of a stored result. `offset` is a character offset, not a
    // line number. Returns false and fills `error` for an unknown/invalid id.
    static bool read(const QString &id, qint64 offset, int limit, QString *out,
                     qint64 *totalChars, QString *error);

    // Overrides the directory for tests. An empty value restores the default.
    static void setDirectoryForTests(const QString &directory);

    // Evicts the oldest stored results until the directory fits `maxBytes`.
    // Returns the number of files removed. Never touches files outside the
    // store directory.
    static int prune(qint64 maxBytes = 200LL * 1024 * 1024);

private:
    static bool validId(const QString &id);
    static QString s_directoryOverride;
};

} // namespace KateAi
