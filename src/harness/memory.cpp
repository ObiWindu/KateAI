/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "memory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

QString s_homeForTests;

QString homePath()
{
    if (!s_homeForTests.isEmpty()) {
        return s_homeForTests;
    }
    return QDir::homePath();
}

QString configHome()
{
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (config.isEmpty()) {
        config = QDir(homePath()).filePath(u".config"_s);
    }
    return config;
}

int scopeRank(MemoryScope scope)
{
    switch (scope) {
    case MemoryScope::Managed:
        return 0;
    case MemoryScope::User:
        return 1;
    case MemoryScope::Project:
        return 2;
    case MemoryScope::Nested:
        return 3;
    case MemoryScope::Local:
        return 4;
    }
    return 2;
}

struct Frontmatter {
    bool present = false;
    QHash<QString, QString> keys;
    QStringList paths;
    QString body;
    int priority = 0;
    bool alwaysApply = true;
    QString description;
};

bool validGlob(const QString &glob)
{
    const QString trimmed = glob.trimmed();
    return !trimmed.isEmpty() && !trimmed.startsWith(u'/')
        && !trimmed.contains(u".."_s) && !trimmed.contains(u'\\');
}

Frontmatter parseFrontmatter(const QString &text)
{
    Frontmatter parsed;
    // A UTF-8 BOM before the opening fence is common on Windows. The first
    // line must still be recognised, so strip it before looking.
    QString source = text;
    if (source.startsWith(QChar(0xFEFF))) {
        source.remove(0, 1);
    }
    const QStringList lines = source.split(u'\n');
    if (lines.isEmpty() || lines.first().trimmed() != u"---"_s) {
        parsed.body = source;
        return parsed;
    }
    int end = -1;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == u"---"_s) {
            end = i;
            break;
        }
    }
    if (end < 0) {
        parsed.body = source;
        return parsed;
    }
    parsed.present = true;
    parsed.body = lines.mid(end + 1).join(u'\n');

    QString currentListKey;
    for (int i = 1; i < end; ++i) {
        const QString line = lines.at(i);
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(u'#')) {
            continue;
        }
        if (trimmed.startsWith(u'-') && !currentListKey.isEmpty()) {
            const QString value = trimmed.mid(1).trimmed();
            if (currentListKey == u"paths"_s && validGlob(value)) {
                parsed.paths.append(value);
            }
            continue;
        }
        currentListKey.clear();
        const int colon = trimmed.indexOf(u':');
        if (colon <= 0) {
            continue;
        }
        const QString key = trimmed.left(colon).trimmed().toLower();
        QString value = trimmed.mid(colon + 1).trimmed();
        if (value.size() >= 2
            && ((value.startsWith(u'"') && value.endsWith(u'"'))
                || (value.startsWith(u'\'') && value.endsWith(u'\'')))) {
            value = value.mid(1, value.size() - 2);
        }
        parsed.keys.insert(key, value);
        if (key == u"paths"_s) {
            if (value.startsWith(u'[') && value.endsWith(u']')) {
                value = value.mid(1, value.size() - 2);
            }
            for (QString part : value.split(u',', Qt::SkipEmptyParts)) {
                part = part.trimmed();
                if (part.size() >= 2
                    && ((part.startsWith(u'"') && part.endsWith(u'"'))
                        || (part.startsWith(u'\'') && part.endsWith(u'\'')))) {
                    part = part.mid(1, part.size() - 2).trimmed();
                }
                if (validGlob(part)) {
                    parsed.paths.append(part);
                }
            }
            if (value.trimmed().isEmpty()) {
                currentListKey = key;
            }
        } else if (key == u"priority"_s) {
            bool ok = false;
            const int priority = value.toInt(&ok);
            if (ok) {
                parsed.priority = priority;
            }
        } else if (key == u"alwaysapply"_s || key == u"always_apply"_s) {
            parsed.alwaysApply = value.compare(u"false"_s, Qt::CaseInsensitive) != 0;
        } else if (key == u"description"_s) {
            parsed.description = value;
        }
    }
    if (!parsed.paths.isEmpty()) {
        // A path-scoped file is lazy unless it explicitly opts into always.
        const QString explicitAlways = parsed.keys.value(u"alwaysapply"_s,
                                                         parsed.keys.value(u"always_apply"_s));
        if (explicitAlways.isEmpty()) {
            parsed.alwaysApply = false;
        }
    }
    return parsed;
}

QString wildcardRegex(const QString &glob)
{
    QString pattern;
    pattern.reserve(glob.size() * 2);
    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob.at(i);
        if (c == u'*') {
            if (i + 1 < glob.size() && glob.at(i + 1) == u'*') {
                ++i;
                // "**/" means "zero or more directories". Treating it as a
                // required ".*/" made **/*.cpp miss a root-level main.cpp.
                if (i + 1 < glob.size() && glob.at(i + 1) == u'/') {
                    pattern += u"(?:.*/)?"_s;
                    ++i;
                } else {
                    pattern += u".*"_s;
                }
            } else {
                pattern += u"[^/]*"_s;
            }
        } else if (c == u'?') {
            pattern += u"[^/]"_s;
        } else {
            pattern += QRegularExpression::escape(QString(c));
        }
    }
    return pattern;
}

// Truncate a QString to a UTF-8 byte budget without splitting a code point.
QString truncateUtf8(const QString &text, qint64 maxBytes)
{
    if (maxBytes <= 0) {
        return QString();
    }
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() <= maxBytes) {
        return text;
    }
    int cut = static_cast<int>(qMin<qint64>(maxBytes, bytes.size()));
    while (cut > 0 && cut < bytes.size()
           && (static_cast<unsigned char>(bytes.at(cut)) & 0xC0) == 0x80) {
        --cut;
    }
    return QString::fromUtf8(bytes.left(cut));
}

QString canonicalOrAbsolute(const QString &path)
{
    const QFileInfo info(path);
    if (info.exists()) {
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            return canonical;
        }
    }
    return info.absoluteFilePath();
}

void collectFiles(const QString &dirPath, const QStringList &names, QStringList *out)
{
    for (const QString &name : names) {
        const QString path = QDir(dirPath).filePath(name);
        if (QFileInfo(path).isFile() && !out->contains(path)) {
            out->append(path);
        }
    }
}

void collectRuleDir(const QString &dirPath, QStringList *out, bool includeLocal,
                    bool filterModes, const MemoryLoader::Config &config,
                    QSet<QString> *modePaths)
{
    QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }
    const QFileInfoList entries = dir.entryInfoList(
        {QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.txt")},
        QDir::Files, QDir::Name);
    for (const QFileInfo &info : entries) {
        if (!includeLocal && info.fileName().endsWith(u".local.md"_s)) {
            continue;
        }
        const QString base = info.completeBaseName();
        const bool knownMode = filterModes && config.knownModeIds.contains(base);
        if (knownMode && base != config.modeId) {
            continue;
        }
        if (!out->contains(info.absoluteFilePath())) {
            out->append(info.absoluteFilePath());
        }
        if (knownMode && base == config.modeId && modePaths) {
            modePaths->insert(info.absoluteFilePath());
        }
    }
}

} // namespace

MemoryLoader::MemoryLoader(const QString &workspace)
    : MemoryLoader(workspace, Config())
{
}

MemoryLoader::MemoryLoader(const QString &workspace, const Config &config)
    : m_workspace(QDir(workspace).absolutePath())
    , m_config(config)
{
    if (workspace.isEmpty()) {
        m_workspace.clear();
    }
}

void MemoryLoader::setHomeForTests(const QString &home)
{
    s_homeForTests = home;
}

void MemoryLoader::setWorkspace(const QString &workspace)
{
    const QString absolute = workspace.isEmpty() ? QString() : QDir(workspace).absolutePath();
    if (absolute == m_workspace) {
        return;
    }
    m_workspace = absolute;
    reset();
}

void MemoryLoader::setModeContext(const QString &modeId, const QStringList &knownModeIds)
{
    QStringList sorted = knownModeIds;
    sorted.removeDuplicates();
    sorted.sort();
    if (m_config.modeId == modeId && m_config.knownModeIds == sorted) {
        return;
    }
    m_config.modeId = modeId;
    m_config.knownModeIds = sorted;
    // Rule selection changed, so the discovered roots and loaded files must be
    // rebuilt. Workspace and user files are re-read once on the next load.
    reset();
}

void MemoryLoader::reset()
{
    m_files.clear();
    m_rootPaths.clear();
    m_loadedPaths.clear();
    m_loadedHashes.clear();
    m_nestedDirs.clear();
    m_modeSpecificPaths.clear();
    m_managedPaths.clear();
    m_rootsDiscovered = false;
}

QString MemoryLoader::scopeId(MemoryScope scope)
{
    switch (scope) {
    case MemoryScope::Managed:
        return u"managed"_s;
    case MemoryScope::User:
        return u"user"_s;
    case MemoryScope::Project:
        return u"project"_s;
    case MemoryScope::Nested:
        return u"nested"_s;
    case MemoryScope::Local:
        return u"local"_s;
    }
    return u"project"_s;
}

QString MemoryLoader::scopeLabel(MemoryScope scope)
{
    switch (scope) {
    case MemoryScope::Managed:
        return u"managed policy"_s;
    case MemoryScope::User:
        return u"user instructions"_s;
    case MemoryScope::Project:
        return u"project instructions"_s;
    case MemoryScope::Nested:
        return u"nested project instructions"_s;
    case MemoryScope::Local:
        return u"local (not committed)"_s;
    }
    return u"project instructions"_s;
}

bool MemoryLoader::matchesPath(const QStringList &globs, const QString &path)
{
    if (globs.isEmpty()) {
        return true;
    }
    QString normalized = path;
    normalized.replace(u'\\', u'/');
    while (normalized.startsWith(u"./"_s)) {
        normalized.remove(0, 2);
    }
    for (const QString &glob : globs) {
        const QString pattern = wildcardRegex(glob.trimmed());
        QRegularExpression regex(u"^(?:"_s + pattern + u")$"_s,
                                 QRegularExpression::CaseInsensitiveOption);
        if (regex.match(normalized).hasMatch()) {
            return true;
        }
        // A bare directory name in `paths:` matches everything below it.
        if (!glob.contains(u'*') && (normalized == glob || normalized.startsWith(glob + u"/"_s))) {
            return true;
        }
    }
    return false;
}

void MemoryLoader::discoverRoots()
{
    if (m_rootsDiscovered) {
        return;
    }
    m_rootsDiscovered = true;

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString managedOverride = env.value(u"KATEAI_MANAGED_MEMORY"_s);
    if (!managedOverride.isEmpty()) {
        for (const QString &path : managedOverride.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
            if (QFileInfo(path).isFile()) {
                m_managedPaths.insert(canonicalOrAbsolute(path));
                if (!m_rootPaths.contains(path)) {
                    m_rootPaths.append(path);
                }
            }
        }
    } else if (QFileInfo(u"/etc/kateai/AGENTS.md"_s).isFile()) {
        m_managedPaths.insert(canonicalOrAbsolute(u"/etc/kateai/AGENTS.md"_s));
        m_rootPaths.append(u"/etc/kateai/AGENTS.md"_s);
    }

    const QString config = configHome();
    collectFiles(config + u"/kateai"_s, {u"AGENTS.md"_s, u"CLAUDE.md"_s}, &m_rootPaths);
    collectRuleDir(config + u"/kateai/memory"_s, &m_rootPaths, true, false, m_config, nullptr);
    collectFiles(homePath() + u"/.claude"_s, {u"CLAUDE.md"_s}, &m_rootPaths);

    if (!m_workspace.isEmpty()) {
        // Root files are precedence-ordered: an explicit override first, then
        // the cross-agent conventions, then Kate's own file. The Cline/Kilo
        // rule files are single files at the workspace root.
        collectFiles(m_workspace,
                     {u"AGENTS.override.md"_s, u"AGENTS.md"_s, u"CLAUDE.md"_s, u"KATEAI.md"_s,
                      u".clinerules"_s, u".kilocoderules"_s},
                     &m_rootPaths);
        collectRuleDir(m_workspace + u"/.kateai/rules"_s, &m_rootPaths, true, true,
                       m_config, &m_modeSpecificPaths);
        collectRuleDir(m_workspace + u"/.claude/rules"_s, &m_rootPaths, true, true,
                       m_config, &m_modeSpecificPaths);
        collectRuleDir(m_workspace + u"/.agents/rules"_s, &m_rootPaths, true, true,
                       m_config, &m_modeSpecificPaths);
        collectRuleDir(m_workspace + u"/.kateai/memory"_s, &m_rootPaths, true, false,
                       m_config, nullptr);
        // Local, uncommitted overrides last.
        const QFileInfoList localFiles = QDir(m_workspace).entryInfoList(
            {QStringLiteral("*.local.md")}, QDir::Files, QDir::Name);
        for (const QFileInfo &info : localFiles) {
            if (!m_rootPaths.contains(info.absoluteFilePath())) {
                m_rootPaths.append(info.absoluteFilePath());
            }
        }
    }
}

bool MemoryLoader::alreadyLoaded(const QString &canonicalPath, const QString &hash) const
{
    if (m_loadedPaths.contains(canonicalPath)) {
        return true;
    }
    // AGENTS.md and CLAUDE.md often point at the same content (a symlink or a
    // copy). The model only needs it once.
    return !hash.isEmpty() && m_loadedHashes.contains(hash);
}

bool MemoryLoader::addFile(const QString &path, MemoryScope scope, const QString &displayPath,
                           bool modeSpecific)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return false;
    }
    const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                                 : info.canonicalFilePath();
    if (m_loadedPaths.contains(canonical)) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray raw = file.read(m_config.maxFileBytes + 1);
    file.close();
    if (raw.isEmpty()) {
        return false;
    }
    const bool truncated = raw.size() > m_config.maxFileBytes;
    const QByteArray kept = truncated ? raw.left(m_config.maxFileBytes) : raw;
    QString text = QString::fromUtf8(kept);
    if (truncated && text.endsWith(QChar(0xFFFD))) {
        text.chop(1);
    }

    const Frontmatter front = parseFrontmatter(text);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
    if (alreadyLoaded(canonical, hash)) {
        m_loadedPaths.append(canonical);
        m_loadedHashes.insert(hash);
        return false;
    }

    MemoryFile memory;
    memory.path = path;
    memory.displayPath = displayPath.isEmpty()
        ? (m_workspace.isEmpty() ? info.fileName()
                                 : QDir(m_workspace).relativeFilePath(info.absoluteFilePath()))
        : displayPath;
    memory.scope = scope;
    memory.content = front.body.trimmed();
    memory.bytes = kept.size();
    memory.hash = hash;
    memory.priority = front.priority;
    memory.paths = front.paths;
    memory.alwaysApply = front.alwaysApply;
    memory.description = front.description;
    memory.truncated = truncated;
    memory.modeSpecific = modeSpecific
        || m_modeSpecificPaths.contains(info.absoluteFilePath())
        || m_modeSpecificPaths.contains(canonical);
    if (memory.content.isEmpty()) {
        m_loadedPaths.append(canonical);
        m_loadedHashes.insert(hash);
        return false;
    }

    m_loadedPaths.append(canonical);
    m_loadedHashes.insert(hash);
    m_files.append(memory);
    return true;
}

void MemoryLoader::loadNestedFor(const QString &relativePath)
{
    if (m_workspace.isEmpty() || relativePath.isEmpty()) {
        return;
    }
    QString normalized = relativePath;
    normalized.replace(u'\\', u'/');
    if (normalized.startsWith(u'/')) {
        const QString absolute = QDir::cleanPath(normalized);
        if (!absolute.startsWith(m_workspace + u'/')) {
            return;
        }
        normalized = QDir(m_workspace).relativeFilePath(absolute);
    }
    if (!normalized.startsWith(u'/')
        && normalized.split(u'/', Qt::SkipEmptyParts).contains(u".."_s)) {
        return;
    }
    QString dirRelative = QFileInfo(normalized).path();
    if (dirRelative == u"."_s) {
        return;
    }
    const QString canonicalWorkspace = QFileInfo(m_workspace).canonicalFilePath();
    // Walk from the deepest directory to the workspace root. Every level is
    // checked once; the display order is fixed later.
    while (!dirRelative.isEmpty() && dirRelative != u"."_s) {
        const QString dirAbsolute = QDir(m_workspace).absoluteFilePath(dirRelative);
        if (m_nestedDirs.contains(dirAbsolute)) {
            break;
        }
        // A symlinked directory inside the workspace must not make the loader
        // read an AGENTS.md or rules directory from outside the workspace.
        const QString canonicalDir = QFileInfo(dirAbsolute).canonicalFilePath();
        if (!canonicalWorkspace.isEmpty() && !canonicalDir.isEmpty()
            && canonicalDir != canonicalWorkspace
            && !canonicalDir.startsWith(canonicalWorkspace + u'/')) {
            break;
        }
        m_nestedDirs.insert(dirAbsolute);
        const QStringList names = {u"AGENTS.md"_s, u"CLAUDE.md"_s, u"KATEAI.md"_s};
        for (const QString &name : names) {
            const QString path = QDir(dirAbsolute).filePath(name);
            if (!QFileInfo(path).isFile()) {
                continue;
            }
            // The frontmatter decides whether this nested file applies to the
            // touched path; a nested AGENTS.md without paths applies to its
            // whole subtree, which includes the path being touched.
            addFile(path, MemoryScope::Nested,
                    QDir(m_workspace).relativeFilePath(path));
        }
        QStringList nestedRules;
        collectRuleDir(dirAbsolute + u"/.kateai/rules"_s, &nestedRules, true, true,
                       m_config, &m_modeSpecificPaths);
        collectRuleDir(dirAbsolute + u"/.claude/rules"_s, &nestedRules, true, true,
                       m_config, &m_modeSpecificPaths);
        for (const QString &path : nestedRules) {
            const bool local = path.endsWith(u".local.md"_s);
            addFile(path, local ? MemoryScope::Local : MemoryScope::Nested,
                    QDir(m_workspace).relativeFilePath(path));
        }
        const QFileInfoList localFiles = QDir(dirAbsolute).entryInfoList(
            {QStringLiteral("*.local.md")}, QDir::Files, QDir::Name);
        for (const QFileInfo &info : localFiles) {
            addFile(info.absoluteFilePath(), MemoryScope::Local,
                    QDir(m_workspace).relativeFilePath(info.absoluteFilePath()));
        }
        const int slash = dirRelative.lastIndexOf(u'/');
        if (slash < 0) {
            break;
        }
        dirRelative = dirRelative.left(slash);
    }
}

QList<MemoryFile> MemoryLoader::load(const QStringList &touchedPaths)
{
    discoverRoots();

    auto scopeFor = [this](const QString &path) {
        const QString canonical = canonicalOrAbsolute(path);
        if (m_managedPaths.contains(path) || m_managedPaths.contains(canonical)) {
            return MemoryScope::Managed;
        }
        const QString config = configHome();
        if (path.startsWith(config + u"/kateai/"_s)
            || path.startsWith(homePath() + u"/.claude/"_s)) {
            return MemoryScope::User;
        }
        if (!m_workspace.isEmpty() && path.startsWith(m_workspace + u'/')) {
            const QString relative = QDir(m_workspace).relativeFilePath(path);
            // Local means an uncommitted project override, not a user file
            // that happens to be named *.local.md.
            if (path.endsWith(u".local.md"_s)) {
                return MemoryScope::Local;
            }
            // Root configuration directories are project-level, even though
            // their files are more than one path component deep.
            const QString first = relative.section(u'/', 0, 0);
            if (first == u".kateai"_s || first == u".claude"_s || first == u".agents"_s) {
                return MemoryScope::Project;
            }
            if (relative.contains(u'/')) {
                return MemoryScope::Nested;
            }
        }
        return MemoryScope::Project;
    };

    // Path-scoped matching is always relative to the workspace, even when a
    // caller passes an absolute path from a tool.
    QStringList normalizedTouched;
    for (const QString &path : touchedPaths) {
        QString normalized = path;
        normalized.replace(u'\\', u'/');
        if (!m_workspace.isEmpty() && QDir::isAbsolutePath(normalized)) {
            if (!normalized.startsWith(m_workspace + u'/')) {
                continue;
            }
            normalized = QDir(m_workspace).relativeFilePath(normalized);
        }
        if (!normalized.isEmpty() && !normalizedTouched.contains(normalized)) {
            normalizedTouched.append(normalized);
        }
    }

    for (const QString &path : std::as_const(m_rootPaths)) {
        const QString display = m_workspace.isEmpty()
            ? QFileInfo(path).fileName()
            : (path.startsWith(m_workspace + u'/')
                   ? QDir(m_workspace).relativeFilePath(path)
                   : QFileInfo(path).fileName());
        addFile(path, scopeFor(path), display);
    }
    for (const QString &path : std::as_const(normalizedTouched)) {
        loadNestedFor(path);
    }

    // Path-scoped files are lazy: include them only when a tool has touched a
    // matching path. A file with no `paths:` applies to everything.
    QList<MemoryFile> applicable;
    for (const MemoryFile &file : std::as_const(m_files)) {
        if (!file.alwaysApply && file.paths.isEmpty()) {
            continue;
        }
        if (!file.paths.isEmpty()) {
            bool matched = false;
            for (const QString &path : std::as_const(normalizedTouched)) {
                if (matchesPath(file.paths, path)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
        }
        applicable.append(file);
    }
    return applyBudget(applicable, m_config);
}

QList<MemoryFile> MemoryLoader::discover(const QString &workspace, const QStringList &touchedPaths)
{
    return discover(workspace, touchedPaths, Config());
}

QList<MemoryFile> MemoryLoader::discover(const QString &workspace, const QStringList &touchedPaths,
                                         const Config &config)
{
    MemoryLoader loader(workspace, config);
    return loader.load(touchedPaths);
}

QList<MemoryFile> MemoryLoader::applyBudget(QList<MemoryFile> files, const Config &config)
{
    std::stable_sort(files.begin(), files.end(), [](const MemoryFile &a, const MemoryFile &b) {
        const int rankA = scopeRank(a.scope);
        const int rankB = scopeRank(b.scope);
        if (rankA != rankB) {
            return rankA < rankB;
        }
        // Mode-specific rules override the generic rules in the same scope.
        if (a.modeSpecific != b.modeSpecific) {
            return !a.modeSpecific;
        }
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.displayPath < b.displayPath;
    });

    QList<MemoryFile> kept;
    qint64 total = 0;
    for (MemoryFile file : std::as_const(files)) {
        if (kept.size() >= config.maxFiles) {
            break;
        }
        const qint64 room = config.maxTotalBytes - total;
        if (room <= 0) {
            break;
        }
        const QString marker = u"\n... (memory truncated by the harness budget)\n"_s;
        const qint64 markerBytes = marker.toUtf8().size();
        qint64 contentBytes = file.content.toUtf8().size();
        const qint64 allowed = qMin<qint64>(config.maxFileBytes, room);
        if (allowed <= markerBytes / 2) {
            continue;
        }
        const qint64 contentBudget = qMax<qint64>(0, allowed - markerBytes);
        bool truncated = file.truncated;
        if (contentBytes > contentBudget) {
            file.content = truncateUtf8(file.content, contentBudget);
            truncated = true;
        }
        if (truncated) {
            file.content += marker;
        }
        contentBytes = file.content.toUtf8().size();
        file.bytes = contentBytes;
        file.truncated = truncated;
        total += contentBytes;
        kept.append(file);
    }
    return kept;
}

QString MemoryLoader::render(const QList<MemoryFile> &files)
{
    static const QRegularExpression closingTag(uR"(</(memory|scope)\b)"_s,
                                               QRegularExpression::CaseInsensitiveOption);
    QString out;
    if (files.isEmpty()) {
        return out;
    }
    out += u"<memory order=\"managed,user,project,nested,local\">\n"_s;
    for (const MemoryFile &file : files) {
        if (file.content.trimmed().isEmpty()) {
            continue;
        }
        QString content = file.content;
        content.replace(closingTag, u"<\\/\\1"_s);
        out += u"<scope name=\""_s + scopeId(file.scope) + u"\" source=\""_s
            + file.displayPath.toHtmlEscaped() + u"\" priority=\""_s
            + QString::number(file.priority) + u"\">\n"_s + content.trimmed() + u"\n</scope>\n"_s;
    }
    out += u"</memory>\n"_s;
    return out;
}

} // namespace KateAi
