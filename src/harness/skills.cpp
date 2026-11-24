/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "skills.h"

#include "memory.h"
#include "../modes.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

constexpr qint64 kMaxSkillBytes = 64 * 1024;
constexpr int kMaxSupportingFiles = 100;

QString g_trustStorePath;
QString g_homeOverride;

QString configHome()
{
    if (!g_homeOverride.isEmpty()) {
        return g_homeOverride + u"/.config"_s;
    }
    const QString xdg = QProcessEnvironment::systemEnvironment().value(u"XDG_CONFIG_HOME"_s);
    if (!xdg.trimmed().isEmpty()) {
        return xdg;
    }
    const QString home = QProcessEnvironment::systemEnvironment().value(u"HOME"_s);
    return home.isEmpty() ? QString() : home + u"/.config"_s;
}

QString trustStorePath()
{
    if (!g_trustStorePath.isEmpty()) {
        return g_trustStorePath;
    }
    const QString env = QProcessEnvironment::systemEnvironment().value(u"KATEAI_SKILL_TRUST_FILE"_s);
    if (!env.trimmed().isEmpty()) {
        return env;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + u"/skill-trust.json"_s;
}

QString normalizedKey(const QString &key)
{
    return key.trimmed().toLower().replace(u'-', u'_');
}

QHash<QString, QString> normalizedFrontmatter(const QHash<QString, QString> &keys)
{
    QHash<QString, QString> out;
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
        out.insert(normalizedKey(it.key()), it.value().trimmed());
    }
    return out;
}

QString unquote(const QString &value)
{
    QString v = value.trimmed();
    if (v.size() >= 2
        && ((v.startsWith(u'"') && v.endsWith(u'"'))
            || (v.startsWith(u'\'') && v.endsWith(u'\'')))) {
        v = v.mid(1, v.size() - 2);
    }
    return v.trimmed();
}

QStringList parseList(const QString &raw)
{
    QString value = raw.trimmed();
    if (value.isEmpty()) {
        return {};
    }
    if (value.startsWith(u'[') && value.endsWith(u']')) {
        value = value.mid(1, value.size() - 2);
    }
    QStringList out;
    const QStringList parts = value.contains(u'\n') ? value.split(u'\n', Qt::SkipEmptyParts)
                                                    : value.split(u',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        QString item = part.trimmed();
        if (item.startsWith(u'-')) {
            item = item.mid(1).trimmed();
        }
        item = unquote(item);
        if (!item.isEmpty() && !out.contains(item)) {
            out.append(item);
        }
    }
    return out;
}

bool parseBool(const QString &raw, bool fallback)
{
    const QString value = unquote(raw).toLower();
    if (value.isEmpty()) {
        return fallback;
    }
    return value == u"true"_s || value == u"yes"_s || value == u"1"_s
        || value == u"on"_s;
}

QString slug(const QString &value)
{
    QString out;
    bool dash = false;
    for (const QChar c : value) {
        if (c.isLetterOrNumber()) {
            out.append(c.toLower());
            dash = false;
        } else if (!dash && !out.isEmpty()) {
            out.append(u'-');
            dash = true;
        }
    }
    while (out.endsWith(u'-')) {
        out.chop(1);
    }
    return out;
}

bool pathInside(const QString &path, const QString &root)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonical.isEmpty() || canonicalRoot.isEmpty()) {
        return false;
    }
    return canonical == canonicalRoot
        || canonical.startsWith(canonicalRoot + QDir::separator());
}

QString hashDocument(const SkillDefinition &skill)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(skill.body.toUtf8());
    for (const QString &file : skill.supportingFiles) {
        hash.addData(file.toUtf8());
        const QFileInfo info(skill.directory + u'/' + file);
        hash.addData(QByteArray::number(info.size()));
        QFile content(info.absoluteFilePath());
        if (content.open(QIODevice::ReadOnly)) {
            hash.addData(content.read(64 * 1024));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString readFileBounded(const QString &path, qint64 maxBytes, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.size() > maxBytes) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes) {
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return QString::fromUtf8(bytes);
}

bool looksLikeScript(const QString &name)
{
    static const QStringList suffixes = {
        u".sh"_s, u".bash"_s, u".zsh"_s, u".fish"_s, u".ps1"_s, u".bat"_s,
        u".cmd"_s, u".exe"_s, u".dll"_s, u".so"_s, u".dylib"_s, u".py"_s,
        u".pl"_s, u".rb"_s, u".js"_s, u".ts"_s, u".jar"_s, u".app"_s,
    };
    for (const QString &suffix : suffixes) {
        if (name.endsWith(suffix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString firstParty(const QString &id, const QString &description, const QString &body)
{
    return u"---\nname: %1\ndescription: %2\n---\n%3"_s.arg(id, description, body);
}

} // namespace

bool SkillDefinition::isActiveFor(const QStringList &touchedPaths) const
{
    if (paths.isEmpty()) {
        return true;
    }
    for (const QString &path : touchedPaths) {
        if (SkillRegistry::matchesPath(paths, path)) {
            return true;
        }
    }
    return false;
}

QJsonObject SkillDefinition::toJson() const
{
    QJsonObject object;
    object.insert(u"id"_s, id);
    object.insert(u"name"_s, name);
    object.insert(u"description"_s, description);
    object.insert(u"scope"_s, SkillRegistry::scopeId(scope));
    object.insert(u"path"_s, skillPath);
    object.insert(u"trusted"_s, trusted);
    object.insert(u"hash"_s, hash);
    object.insert(u"paths"_s, QJsonArray::fromStringList(paths));
    object.insert(u"allowedTools"_s, QJsonArray::fromStringList(allowedTools));
    object.insert(u"disableModelInvocation"_s, disableModelInvocation);
    object.insert(u"context"_s, contextMode);
    object.insert(u"model"_s, model);
    object.insert(u"effort"_s, effort);
    object.insert(u"version"_s, version);
    object.insert(u"license"_s, license);
    object.insert(u"bytes"_s, static_cast<double>(bytes));
    object.insert(u"warnings"_s, QJsonArray::fromStringList(warnings));
    return object;
}

SkillRegistry::SkillRegistry()
{
    discover();
}

SkillRegistry::SkillRegistry(const QString &workspace)
    : m_workspace(workspace)
{
    discover();
}

void SkillRegistry::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    reload();
}

void SkillRegistry::reload()
{
    discover();
}

QString SkillRegistry::scopeId(SkillScope scope)
{
    switch (scope) {
    case SkillScope::Managed:
        return u"managed"_s;
    case SkillScope::User:
        return u"user"_s;
    case SkillScope::Project:
        return u"project"_s;
    case SkillScope::Bundled:
        return u"bundled"_s;
    }
    return u"project"_s;
}

QString SkillRegistry::scopeLabel(SkillScope scope)
{
    switch (scope) {
    case SkillScope::Managed:
        return u"Managed"_s;
    case SkillScope::User:
        return u"User"_s;
    case SkillScope::Project:
        return u"Project"_s;
    case SkillScope::Bundled:
        return u"Bundled"_s;
    }
    return u"Project"_s;
}

bool SkillRegistry::matchesPath(const QStringList &globs, const QString &path)
{
    return MemoryLoader::matchesPath(globs, path);
}

void SkillRegistry::setTrustStorePath(const QString &path)
{
    g_trustStorePath = path;
}

void SkillRegistry::setHomeForTests(const QString &home)
{
    g_homeOverride = home;
}

SkillDefinition SkillRegistry::parseDocument(const QString &document, const QString &path,
                                             SkillScope scope, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    const DocumentFrontmatter frontmatter = ModeRegistry::parseFrontmatter(document);
    const QHash<QString, QString> keys = normalizedFrontmatter(frontmatter.keys);

    const QString declaredName = unquote(keys.value(u"name"_s));
    const QString fallbackId = slug(QFileInfo(path).absolutePath().section(u'/', -1));
    const QString id = slug(declaredName.isEmpty() ? fallbackId : declaredName);
    const QString description = unquote(keys.value(u"description"_s));
    if (id.isEmpty() || description.isEmpty()) {
        return {};
    }

    SkillDefinition skill;
    skill.id = id;
    skill.name = declaredName.isEmpty() ? id : declaredName;
    skill.description = description;
    skill.scope = scope;
    skill.skillPath = path;
    skill.directory = QFileInfo(path).absolutePath();
    skill.body = frontmatter.body.trimmed();
    skill.bytes = skill.body.toUtf8().size();
    skill.paths = parseList(keys.value(u"paths"_s, keys.value(u"path"_s)));
    skill.allowedTools = parseList(keys.value(u"allowed_tools"_s,
                                             keys.value(u"allowedTools"_s)));
    skill.disableModelInvocation = parseBool(
        keys.value(u"disable_model_invocation"_s, keys.value(u"disableModelInvocation"_s)),
        false);
    const QString context = keys.value(u"context"_s).trimmed().toLower();
    skill.contextMode = context == u"fork"_s ? u"fork"_s : u"inline"_s;
    skill.model = unquote(keys.value(u"model"_s));
    skill.effort = unquote(keys.value(u"effort"_s));
    skill.version = unquote(keys.value(u"version"_s));
    skill.license = unquote(keys.value(u"license"_s));

    const QFileInfoList entries = QDir(skill.directory).entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : entries) {
        if (info.fileName() == u"SKILL.md"_s || info.fileName() == u"skill.md"_s) {
            continue;
        }
        if (!pathInside(info.absoluteFilePath(), skill.directory)) {
            continue;
        }
        skill.supportingFiles.append(info.fileName());
        if (skill.supportingFiles.size() >= kMaxSupportingFiles) {
            break;
        }
    }
    skill.hash = hashDocument(skill);
    skill.warnings = scan(skill);

    if (ok) {
        *ok = true;
    }
    return skill;
}

QList<SkillDefinition> SkillRegistry::discoverDirectory(const QString &root, SkillScope scope,
                                                        QStringList *warnings)
{
    QList<SkillDefinition> out;
    QDir dir(root);
    if (!dir.exists()) {
        return out;
    }
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString skillPath = entry.absoluteFilePath() + u"/SKILL.md"_s;
        if (!QFileInfo::exists(skillPath)) {
            continue;
        }
        if (!pathInside(skillPath, root)) {
            if (warnings) {
                warnings->append(u"Skipped a skill whose SKILL.md resolves outside its directory: "_s
                                 + skillPath);
            }
            continue;
        }
        bool read = false;
        const QString document = readFileBounded(skillPath, kMaxSkillBytes, &read);
        if (!read) {
            if (warnings) {
                warnings->append(u"Skipped a skill that is unreadable or larger than 64 KiB: "_s
                                 + skillPath);
            }
            continue;
        }
        bool parsed = false;
        SkillDefinition skill = parseDocument(document, skillPath, scope, &parsed);
        if (!parsed) {
            if (warnings) {
                warnings->append(u"Skipped a skill without a valid name and description: "_s
                                 + skillPath);
            }
            continue;
        }
        out.append(skill);
    }
    return out;
}

void SkillRegistry::discover()
{
    m_skills.clear();
    m_warnings.clear();

    QList<SkillDefinition> discovered;
    QSet<QString> seen;

    auto appendAll = [&discovered, &seen](const QList<SkillDefinition> &items) {
        for (const SkillDefinition &item : items) {
            if (seen.contains(item.id)) {
                continue;
            }
            seen.insert(item.id);
            discovered.append(item);
        }
    };

    if (!m_workspace.isEmpty()) {
        const QStringList projectRoots = {
            m_workspace + u"/.kateai/skills"_s,
            m_workspace + u"/.agents/skills"_s,
            m_workspace + u"/.claude/skills"_s,
        };
        for (const QString &root : projectRoots) {
            appendAll(discoverDirectory(root, SkillScope::Project, &m_warnings));
        }
    }

    const QStringList userRoots = {
        configHome() + u"/kateai/skills"_s,
        configHome() + u"/.claude/skills"_s,
    };
    for (const QString &root : userRoots) {
        appendAll(discoverDirectory(root, SkillScope::User, &m_warnings));
    }

    QStringList managedRoots;
    const QString managed = QProcessEnvironment::systemEnvironment().value(u"KATEAI_MANAGED_SKILLS"_s);
    if (!managed.trimmed().isEmpty()) {
        managedRoots.append(managed.trimmed());
    }
#ifdef Q_OS_UNIX
    managedRoots.append(u"/etc/kateai/skills"_s);
#endif
    for (const QString &root : managedRoots) {
        appendAll(discoverDirectory(root, SkillScope::Managed, &m_warnings));
    }

    appendAll(bundledSkills());

    const QHash<QString, QString> trust = readTrustStore();
    for (SkillDefinition &skill : discovered) {
        if (skill.scope == SkillScope::Managed || skill.scope == SkillScope::Bundled) {
            skill.trusted = true;
        } else if (skill.scope == SkillScope::User) {
            const QString stored = trust.value(skill.id);
            skill.trusted = stored == u"revoked"_s
                ? false
                : (stored == skill.hash || !hasCriticalFinding(skill.warnings));
        } else {
            skill.trusted = trust.value(skill.id) == skill.hash;
        }
    }

    // An untrusted project skill may shadow a bundled skill with the same id,
    // but it must not make the trusted built-in unreachable. Restore the
    // bundled definition in that case.
    const QList<SkillDefinition> bundled = bundledSkills();
    for (const SkillDefinition &builtin : bundled) {
        for (SkillDefinition &skill : discovered) {
            if (skill.id == builtin.id && !skill.trusted) {
                skill = builtin;
                skill.trusted = true;
                break;
            }
        }
    }

    std::sort(discovered.begin(), discovered.end(), [](const SkillDefinition &a, const SkillDefinition &b) {
        return a.id < b.id;
    });
    m_skills = discovered;
}

std::optional<SkillDefinition> SkillRegistry::byId(const QString &id) const
{
    const QString wanted = slug(id);
    for (const SkillDefinition &skill : m_skills) {
        if (skill.id == wanted) {
            return skill;
        }
    }
    return std::nullopt;
}

QList<SkillDefinition> SkillRegistry::activeSkills(const QStringList &touchedPaths,
                                                   bool includeDisabled) const
{
    QList<SkillDefinition> out;
    for (const SkillDefinition &skill : m_skills) {
        if (!includeDisabled && skill.disableModelInvocation) {
            continue;
        }
        if (!skill.isActiveFor(touchedPaths)) {
            continue;
        }
        out.append(skill);
    }
    return out;
}

QString SkillRegistry::catalog(const QStringList &touchedPaths, int maxChars) const
{
    const QList<SkillDefinition> active = activeSkills(touchedPaths);
    if (active.isEmpty()) {
        return QString();
    }
    QString out;
    for (const SkillDefinition &skill : active) {
        QString line = u"- %1: %2"_s.arg(skill.id, skill.description.simplified());
        if (out.size() + line.size() + 1 > maxChars) {
            const int room = maxChars - out.size() - 1;
            if (room <= 8) {
                break;
            }
            line = line.left(room - 1) + u"…"_s;
            out += line;
            break;
        }
        if (!out.isEmpty()) {
            out += u'\n';
        }
        out += line;
    }
    return out;
}

bool SkillRegistry::load(const QString &id, SkillDefinition *out, QString *error) const
{
    if (error) {
        error->clear();
    }
    const std::optional<SkillDefinition> found = byId(id);
    if (!found) {
        if (error) {
            *error = u"There is no skill named '%1'."_s.arg(id);
        }
        return false;
    }
    // Bundled skills are embedded in the binary; their path is a stable label,
    // not a readable file.
    if (found->skillPath.startsWith(u":/"_s)) {
        if (out) {
            *out = *found;
        }
        return true;
    }
    bool read = false;
    const QString document = readFileBounded(found->skillPath, kMaxSkillBytes, &read);
    if (!read) {
        if (error) {
            *error = u"The skill file is unreadable or larger than 64 KiB."_s;
        }
        return false;
    }
    bool parsed = false;
    SkillDefinition loaded = parseDocument(document, found->skillPath, found->scope, &parsed);
    if (!parsed || loaded.id != found->id) {
        if (error) {
            *error = u"The skill file changed and is no longer valid."_s;
        }
        return false;
    }
    loaded.trusted = found->trusted;
    if (loaded.scope == SkillScope::Project) {
        const QHash<QString, QString> trust = readTrustStore();
        loaded.trusted = trust.value(loaded.id) == loaded.hash;
    }
    if (out) {
        *out = loaded;
    }
    return true;
}

bool SkillRegistry::isTrusted(const QString &id) const
{
    const std::optional<SkillDefinition> found = byId(id);
    if (!found) {
        return false;
    }
    if (found->scope == SkillScope::Managed || found->scope == SkillScope::Bundled) {
        return true;
    }
    if (found->scope == SkillScope::User) {
        const QString stored = readTrustStore().value(found->id);
        return stored == u"revoked"_s
            ? false
            : (stored == found->hash || !hasCriticalFinding(found->warnings));
    }
    return readTrustStore().value(found->id) == found->hash;
}

bool SkillRegistry::trust(const QString &id, QString *error)
{
    if (error) {
        error->clear();
    }
    const std::optional<SkillDefinition> found = byId(id);
    if (!found) {
        if (error) {
            *error = u"There is no skill named '%1'."_s.arg(id);
        }
        return false;
    }
    if (found->scope == SkillScope::Managed || found->scope == SkillScope::Bundled) {
        return true;
    }
    QHash<QString, QString> entries = readTrustStore();
    entries.insert(found->id, found->hash);
    if (!writeTrustStore(entries, error)) {
        return false;
    }
    for (SkillDefinition &skill : m_skills) {
        if (skill.id == found->id) {
            skill.trusted = true;
        }
    }
    return true;
}

bool SkillRegistry::revoke(const QString &id, QString *error)
{
    if (error) {
        error->clear();
    }
    const std::optional<SkillDefinition> found = byId(id);
    if (!found) {
        if (error) {
            *error = u"There is no skill named '%1'."_s.arg(id);
        }
        return false;
    }
    QHash<QString, QString> entries = readTrustStore();
    entries.insert(found->id, u"revoked"_s);
    if (!writeTrustStore(entries, error)) {
        return false;
    }
    for (SkillDefinition &skill : m_skills) {
        if (skill.id == found->id) {
            skill.trusted = false;
        }
    }
    return true;
}

QStringList SkillRegistry::scan(const SkillDefinition &skill)
{
    QStringList warnings;

    static const QRegularExpression injection(
        u"(?i)(ignore (all )?(previous|prior|above) instructions|disregard (the )?(system|developer) prompt|"
        u"reveal (your )?(system prompt|hidden instructions)|you are now|"
        u"do not (tell|inform) the user|override (the )?(safety|system) )"_s);
    static const QRegularExpression traversal(u"(^|[\\s\"'(])[.][.]([/\\\\]|[\\s\"')]|$)"_s);
    static const QRegularExpression network(
        u"(?i)(^|[;&|]\\s*)(curl|wget|ncat|netcat|nc|ssh|scp|rsync|telnet)\\b"_s);

    const QString text = skill.body;
    if (injection.match(text).hasMatch()) {
        warnings.append(u"Prompt-injection phrase in the skill body."_s);
    }
    if (traversal.match(text).hasMatch() || text.contains(u"../"_s) || text.contains(u"..\\"_s)) {
        warnings.append(u"Path traversal in the skill body."_s);
    }
    if (network.match(text).hasMatch()) {
        warnings.append(u"Network command in the skill body."_s);
    }

    for (const QString &file : skill.supportingFiles) {
        const QFileInfo info(skill.directory + u'/' + file);
        if (!pathInside(info.absoluteFilePath(), skill.directory)) {
            warnings.append(u"Supporting file resolves outside the skill directory: %1"_s.arg(file));
        }
        if (info.isExecutable() || looksLikeScript(file)) {
            warnings.append(u"Executable or script in the skill directory: %1"_s.arg(file));
        }
        bool read = false;
        const QString content = readFileBounded(info.absoluteFilePath(), kMaxSkillBytes, &read);
        if (read) {
            if (injection.match(content).hasMatch()) {
                warnings.append(u"Prompt-injection phrase in supporting file: %1"_s.arg(file));
            }
            if (traversal.match(content).hasMatch() || content.contains(u"../"_s)
                || content.contains(u"..\\"_s)) {
                warnings.append(u"Path traversal in supporting file: %1"_s.arg(file));
            }
            if (network.match(content).hasMatch()) {
                warnings.append(u"Network command in supporting file: %1"_s.arg(file));
            }
        }
    }
    return warnings;
}

bool SkillRegistry::hasCriticalFinding(const QStringList &warnings)
{
    for (const QString &warning : warnings) {
        if (warning.contains(u"Prompt-injection"_s, Qt::CaseInsensitive)
            || warning.contains(u"Path traversal"_s, Qt::CaseInsensitive)
            || warning.contains(u"Executable"_s, Qt::CaseInsensitive)
            || warning.contains(u"Network command"_s, Qt::CaseInsensitive)
            || warning.contains(u"resolves outside"_s, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QHash<QString, QString> SkillRegistry::readTrustStore(QString *error)
{
    if (error) {
        error->clear();
    }
    QFile file(trustStorePath());
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) {
        if (error) {
            *error = u"Could not read the skill trust store."_s;
        }
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        if (error) {
            *error = u"The skill trust store is not a JSON object."_s;
        }
        return {};
    }
    QHash<QString, QString> entries;
    const QJsonObject object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isString()) {
            entries.insert(it.key(), it.value().toString());
        }
    }
    return entries;
}

bool SkillRegistry::writeTrustStore(const QHash<QString, QString> &entries, QString *error)
{
    if (error) {
        error->clear();
    }
    const QString path = trustStorePath();
    if (path.isEmpty()) {
        if (error) {
            *error = u"The skill trust store path is unavailable."_s;
        }
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject object;
    QStringList keys = entries.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys) {
        object.insert(key, entries.value(key));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = u"Could not write the skill trust store: %1"_s.arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = u"Could not commit the skill trust store: %1"_s.arg(file.errorString());
        }
        return false;
    }
    return true;
}

QList<SkillDefinition> SkillRegistry::bundledSkills()
{
    const QList<QPair<QString, QString>> documents = {
        {u"diagnose-test-failure"_s,
         firstParty(u"diagnose-test-failure"_s,
                    u"Diagnose a failing test, form a hypothesis, fix the smallest cause, and prove the test passes. Use when a test or build fails and the cause is not obvious."_s,
                    u"1. Reproduce the failure with the narrowest command that still fails.\n"
                    u"2. Read the failing assertion, the code under test, and the last change that touched either.\n"
                    u"3. State one hypothesis and the observation that would confirm it.\n"
                    u"4. Make the smallest change that tests the hypothesis.\n"
                    u"5. Re-run the narrow command, then the surrounding suite.\n"
                    u"6. Report: command, output, root cause, and the change.\n"
                    u"Do not weaken or skip the test to make it pass. If the test is wrong, say so and propose the test change separately.\n"_s)},
        {u"review-diff"_s,
         firstParty(u"review-diff"_s,
                    u"Review the current diff for correctness, edge cases, missing tests, and consistency with the surrounding code. Use before accepting or committing a change."_s,
                    u"1. Inspect the diff and the files it touches; read enough surrounding code to judge the change.\n"
                    u"2. Look for correctness bugs, unhandled errors, security issues, and behaviour changes outside the stated goal.\n"
                    u"3. Check tests: do they cover the new behaviour and the failure path? Does an existing test need updating?\n"
                    u"4. Report findings by severity. Cite file and line. Do not restyle code or make unrelated changes.\n"
                    u"5. If everything is sound, say so explicitly and name the checks that support that conclusion.\n"_s)},
        {u"prepare-release"_s,
         firstParty(u"prepare-release"_s,
                    u"Prepare a release: verify the tree, update versions and changelogs, run the test suite, and produce the release notes. Use only when the user asks for a release."_s,
                    u"1. Confirm the requested version and that the working tree contains only intended changes.\n"
                    u"2. Update the version in every metadata file the project uses, not only the build file.\n"
                    u"3. Write the changelog from the actual diff, grouping user-visible changes and fixes.\n"
                    u"4. Run the project's full test suite and the packaging checks that can run locally.\n"
                    u"5. Report the exact commands, results, changed files, and any step that could not run. Do not publish or tag without explicit approval.\n"_s)},
    };

    QList<SkillDefinition> out;
    for (const QPair<QString, QString> &document : documents) {
        bool parsed = false;
        SkillDefinition skill = parseDocument(document.second,
                                              u":/kateai/skills/%1/SKILL.md"_s.arg(document.first),
                                              SkillScope::Bundled, &parsed);
        if (parsed) {
            skill.trusted = true;
            out.append(skill);
        }
    }
    return out;
}

} // namespace KateAi
