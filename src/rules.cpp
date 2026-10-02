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

    auto readFile = [&blocks](const QString &path, const QString &label) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        QByteArray contents = file.read(MaxFileBytes);
        if (contents.isEmpty()) {
            return false;
        }
        if (contents.size() == MaxFileBytes) {
            contents += "\n... (rules truncated)";
        }
        blocks.append({label, QString::fromUtf8(contents).trimmed()});
        return true;
    };

    const QStringList dirs = ruleDirectories(workspace);
    QStringList modeFiles;

    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists()) {
            continue;
        }
        const QFileInfoList entries =
            dir.entryInfoList({QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
        for (const QFileInfo &info : entries) {
            if (blocks.size() + modeFiles.size() >= MaxFiles) {
                break;
            }
            const QString label = dir.dirName() + u"/"_s + info.fileName();
            if (!modeId.isEmpty() && info.fileName() == modeId + u".md"_s) {
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
        out += u"<rules source=\"global\">\n"_s + globalRules.trimmed() + u"\n</rules>\n"_s;
    }
    for (const Block &block : blocks) {
        if (block.content.trimmed().isEmpty()) {
            continue;
        }
        out += u"<rules source=\""_s + block.source.toHtmlEscaped() + u"\">\n"_s + block.content + u"\n</rules>\n"_s;
    }
    return out;
}

} // namespace KateAi