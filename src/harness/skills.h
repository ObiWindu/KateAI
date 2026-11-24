/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace KateAi
{

// Where a skill came from. Trust and precedence follow the scope: managed and
// bundled skills are trusted because they ship with the installation, user
// skills are trusted after a scan, and project skills need an explicit
// approval keyed by the file hash.
enum class SkillScope {
    Managed,
    User,
    Project,
    Bundled,
};

struct SkillDefinition {
    QString id;
    QString name;
    QString description;
    SkillScope scope = SkillScope::Project;
    // Absolute path of SKILL.md and the skill directory that owns it.
    QString skillPath;
    QString directory;
    // Supporting files one level deep, relative to the directory. Their
    // contents are not loaded: the model can read the ones it needs.
    QStringList supportingFiles;
    QString body;
    // Frontmatter fields.
    QStringList paths;
    QStringList allowedTools;
    bool disableModelInvocation = false;
    // "inline" (the body becomes part of this turn) or "fork" (the body runs
    // in an isolated sub-agent and only its summary returns).
    QString contextMode = QStringLiteral("inline");
    QString model;
    QString effort;
    QString version;
    QString license;
    QString descriptionSource;
    // Trust state as computed at discovery. A project skill stays untrusted
    // until the user approves this exact hash.
    bool trusted = false;
    QString hash;
    QStringList warnings;
    qint64 bytes = 0;

    bool isValid() const
    {
        return !id.trimmed().isEmpty() && !skillPath.isEmpty();
    }
    bool isFork() const
    {
        return contextMode.compare(QStringLiteral("fork"), Qt::CaseInsensitive) == 0;
    }
    // Skills without paths are always active. A path-scoped skill activates
    // only when one of the paths a tool touched matches.
    bool isActiveFor(const QStringList &touchedPaths) const;
    QJsonObject toJson() const;
};

struct SkillDiscovery {
    QList<SkillDefinition> skills;
    QStringList warnings;
};

/**
 * Progressive-disclosure capability bundles (`SKILL.md`).
 *
 * Discovery reads project, user, managed, and bundled scopes. The prompt only
 * ever receives a name and a short description; the body is loaded by the
 * `skill` tool when the model asks for it. The registry also owns the trust
 * scan and the small hash-keyed approval store, so an untrusted project skill
 * cannot execute its instructions by naming itself.
 */
class SkillRegistry
{
public:
    SkillRegistry();
    explicit SkillRegistry(const QString &workspace);

    void setWorkspace(const QString &workspace);
    QString workspace() const
    {
        return m_workspace;
    }
    void reload();
    QStringList warnings() const
    {
        return m_warnings;
    }

    QList<SkillDefinition> skills() const
    {
        return m_skills;
    }
    std::optional<SkillDefinition> byId(const QString &id) const;
    bool contains(const QString &id) const
    {
        return byId(id).has_value();
    }

    // The progressive-disclosure catalog. Only model-invocable skills whose
    // `paths:` globs match the touched paths are included. The rendered text
    // never exceeds `maxChars`; descriptions are shortened first.
    QString catalog(const QStringList &touchedPaths = QStringList(),
                    int maxChars = 1500) const;
    QList<SkillDefinition> activeSkills(const QStringList &touchedPaths = QStringList(),
                                        bool includeDisabled = false) const;

    // Loads the body and supporting-file list for one skill. Re-reads from
    // disk so a long-lived session does not serve stale instructions.
    bool load(const QString &id, SkillDefinition *out, QString *error = nullptr) const;

    // Trust. `trust` writes the file hash to the approval store; `revoke`
    // removes it. A changed file has a different hash and becomes untrusted.
    bool trust(const QString &id, QString *error = nullptr);
    bool revoke(const QString &id, QString *error = nullptr);
    bool isTrusted(const QString &id) const;

    // Deterministic scan used by the trust card and by discovery. Returns
    // human-readable findings; an executable or traversal finding is critical
    // and prevents a user skill from being auto-trusted.
    static QStringList scan(const SkillDefinition &skill);
    static bool hasCriticalFinding(const QStringList &warnings);

    static QString scopeId(SkillScope scope);
    static QString scopeLabel(SkillScope scope);
    static bool matchesPath(const QStringList &globs, const QString &path);
    // Matches a skill id against an AgentSpec's allowed list. An empty list
    // allows every skill; entries may end in '*' to match a prefix, the same
    // way tool allow-lists do.
    static bool matchesAllowed(const QStringList &patterns, const QString &id);

    // Tests set an isolated store and home so they never read the developer's
    // skills or approvals.
    static void setTrustStorePath(const QString &path);
    static void setHomeForTests(const QString &home);

    // The first-party skills embedded in the binary.
    static QList<SkillDefinition> bundledSkills();

private:
    void discover();
    static SkillDefinition parseDocument(const QString &document, const QString &path,
                                         SkillScope scope, bool *ok);
    static QList<SkillDefinition> discoverDirectory(const QString &root, SkillScope scope,
                                                    QStringList *warnings);
    static QHash<QString, QString> readTrustStore(QString *error = nullptr);
    static bool writeTrustStore(const QHash<QString, QString> &entries, QString *error);

    QString m_workspace;
    QList<SkillDefinition> m_skills;
    QStringList m_warnings;
};

} // namespace KateAi
