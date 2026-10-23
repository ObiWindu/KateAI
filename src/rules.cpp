/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QStringList RulesLoader::ruleDirectories(const QString &workspace)
{
    if (workspace.isEmpty()) {
        return {};
    }
    return {
        workspace + u"/.kateai/rules"_s,
        // Kilo Code / Cline compatible locations.
        workspace + u"/.clinerules"_s,
        workspace + u"/.kilocoderules"_s,
    };
}

QStringList RulesLoader::rootRuleFiles(const QString &workspace)
{
    if (workspace.isEmpty()) {
        return {};
    }
    return {
        workspace + u"/AGENTS.md"_s,
        workspace + u"/CLAUDE.md"_s,
    };
}

QList<RulesLoader::Block> RulesLoader::load(const QString &workspace, const QString &modeId)
{
    QList<Block> blocks;
    if (workspace.isEmpty()) {
        return blocks;
    }

    // The budget for all the rules of this project.
    int usedBytes = 0;

    auto readFile = [&blocks, &usedBytes](const QString &path, const QString &label) {
        if (usedBytes >= MaxTotalBytes) {
            return false;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        const qint64 fileSize = file.size();
        const int room = MaxTotalBytes - usedBytes;
        const QByteArray contents = file.read(qMin(MaxFileBytes, room));
        if (contents.isEmpty()) {
            return false;
        }
        usedBytes += contents.size();
        // The cut is on a byte boundary and can split a multi-byte character
        // in half. The text is decoded first; a trailing incomplete sequence
        // decodes to U+FFFD, which is dropped. Only a genuine cut needs this:
        // a file read whole is used as-is.
        QString text = QString::fromUtf8(contents);
        const bool truncated = contents.size() < fileSize;
        if (truncated && text.endsWith(QChar(0xFFFD))) {
            text.chop(1);
        }
        if (truncated) {
            const QString marker = u"\n... (rules truncated)"_s;
            if (contents.size() > marker.size()) {
                // The marker counts against the same budget as the payload,
                // or every truncated file would push the total past
                // MaxTotalBytes by its own marker.
                while (text.size() + marker.size() > contents.size() && !text.isEmpty()) {
                    text.chop(1);
                }
                text += marker;
            }
        }
        blocks.append({label, text.trimmed()});
        return true;
    };

    const QStringList dirs = ruleDirectories(workspace);
    QStringList modeFiles;

    // ".clinerules" and ".kilocoderules" are files at the root of the project
    // in the convention they come from, not directories. Treated as
    // directories, they were silently ignored.
    for (const QString &path : dirs) {
        if (path.endsWith(u"/.clinerules"_s) || path.endsWith(u"/.kilocoderules"_s)) {
            if (QFileInfo::exists(path) && QFileInfo(path).isFile()) {
                readFile(path, QFileInfo(path).fileName());
            }
        }
    }

    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists() || !QFileInfo(dir.absolutePath()).isDir()) {
            continue;
        }
        const QFileInfoList entries =
            dir.entryInfoList({QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
        for (const QFileInfo &info : entries) {
            if (blocks.size() + modeFiles.size() >= MaxFiles) {
                break;
            }
            const QString label = dir.dirName() + u"/"_s + info.fileName();
            if (!modeId.isEmpty() && (info.fileName() == modeId + u".md"_s
                                      || info.fileName() == modeId + u".markdown"_s
                                      || info.fileName() == modeId + u".txt"_s)) {
                // Applied last so mode-specific rules win.
                modeFiles.append(info.absoluteFilePath() + QLatin1Char('\n') + label);
                continue;
            }
            readFile(info.absoluteFilePath(), label);
        }
    }

    for (const QString &path : rootRuleFiles(workspace)) {
        if (blocks.size() + modeFiles.size() >= MaxFiles) {
            break;
        }
        readFile(path, QFileInfo(path).fileName());
    }

    for (const QString &entry : modeFiles) {
        if (blocks.size() >= MaxFiles) {
            break;
        }
        const int newline = entry.indexOf(u'\n');
        readFile(entry.left(newline), entry.mid(newline + 1));
    }

    return blocks;
}

QString RulesLoader::render(const QList<Block> &blocks, const QString &globalRules)
{
    QString out;
    if (!globalRules.trimmed().isEmpty()) {
        // The same neutralising as for the project rules.
        QString content = globalRules.trimmed();
        content.replace(u"</rules"_s, u"<\\/rules"_s);
        out += u"<rules source=\"global\">\n"_s + content + u"\n</rules>\n"_s;
    }
    for (const Block &block : blocks) {
        if (block.content.trimmed().isEmpty()) {
            continue;
        }
        // A rule file that contains "</rules>" would close the wrapper and put
        // the rest of its content into the prompt as free text. The closing
        // tag is neutralised.
        QString content = block.content;
        content.replace(u"</rules"_s, u"<\\/rules"_s);
        out += u"<rules source=\""_s + block.source.toHtmlEscaped() + u"\">\n"_s + content + u"\n</rules>\n"_s;
    }
    return out;
}

} // namespace KateAi