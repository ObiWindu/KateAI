/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace KateAi
{

class Sandbox;

enum class PolicyVerdict {
    NoMatch,
    Allow,
    Ask,
    Deny,
    Review,
};

QString policyVerdictId(PolicyVerdict verdict);

struct PolicyMatch {
    QString tool;
    QString path;
    QStringList commandPrefixes;
    QString commandRegex;
    QString mcpServer;
    QString mcpTool;
    QString risk;
};

struct PolicyRule {
    QString id;
    QString scope; // managed, user, project, session
    QString sourceFile;
    PolicyMatch match;
    PolicyVerdict verdict = PolicyVerdict::NoMatch;
    QString reason;
};

struct PolicyDecision {
    PolicyVerdict verdict = PolicyVerdict::NoMatch;
    QString ruleId;
    QString reason;
};

struct PolicyWarning {
    QString file;
    QString message;
};

/**
 * Layered, deny-first policy rules.
 *
 * Rules are data (`.kateai/policy.json`, the user policy and the managed
 * policy). Paths are resolved and symlinks followed before matching. A deny
 * anywhere beats an allow anywhere, and the immutable hardline floor beats
 * every mode including "always approve".
 */
class PolicyEngine
{
public:
    void clear();
    bool load(const QString &workspace, QString *error = nullptr, bool allowProjectAllow = false);
    bool loadFiles(const QStringList &files, QString *error = nullptr, bool allowProjectAllow = true);

    QList<PolicyRule> rules() const
    {
        return m_rules;
    }
    QStringList loadedFiles() const
    {
        return m_files;
    }
    QList<PolicyWarning> warnings() const
    {
        return m_warnings;
    }
    bool isEmpty() const
    {
        return m_rules.isEmpty();
    }

    PolicyDecision evaluate(const QString &toolName, const QJsonObject &arguments,
                            const Sandbox &sandbox, const QString &workspace) const;

    static bool hardline(const QString &toolName, const QJsonObject &arguments,
                         const Sandbox &sandbox, QString *reason = nullptr);
    static QString userPolicyPath();
    static QString managedPolicyPath();

private:
    QList<PolicyRule> m_rules;
    QStringList m_files;
    QList<PolicyWarning> m_warnings;
};

} // namespace KateAi
