/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "checkpoint.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
// Snapshots are tags, not a branch, because a linear branch keeps every
// ancestor reachable and retention pruning would never reclaim anything.
const QString TagPrefix = QStringLiteral("refs/tags/");
const QString TagNamespace = TagPrefix + QStringLiteral("kateai/");

// Never restore through these; they hold state this plugin regenerates.
bool isProtectedPath(const QString &relativePath)
{
    return relativePath.startsWith(u".git/"_s) || relativePath == u".git"_s || relativePath.startsWith(u".kateai/project_graph.json"_s);
}

// Tag names carry a zero-padded sequence so `for-each-ref --sort=-refname`
// yields newest-first even when several snapshots land in the same second.
// `git tag` prepends refs/tags/ itself, so it must be given the short name.
QString tagName(int sequence, const QString &hash)
{
    return QStringLiteral("kateai/%1-%2").arg(sequence, 8, 10, QLatin1Char('0')).arg(hash);
}

bool isTagName(const QString &ref)
{
    return ref.startsWith(TagNamespace);
}

// Recovers the snapshot commit hash from a tag ref name.
QString hashFromTag(const QString &ref)
{
    return ref.mid(TagNamespace.size());
}

// Turns a full ref name back into the short name `git tag -d` expects.
QString shortTagName(const QString &ref)
{
    return ref.startsWith(TagPrefix) ? ref.mid(TagPrefix.size()) : ref;
}
} // namespace

CheckpointManager::CheckpointManager(QObject *parent)
    : QObject(parent)
{
}

CheckpointManager::~CheckpointManager() = default;

void CheckpointManager::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    m_unavailableReason.clear();
}

void CheckpointManager::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

void CheckpointManager::setRetention(int retention)
{
    m_retention = qBound(2, retention, 500);
}

QString CheckpointManager::shadowRoot() const
{
    if (m_workspace.isEmpty()) {
        return {};
    }
    const QByteArray digest = QCryptographicHash::hash(m_workspace.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        return {};
    }
    return base + u"/checkpoints/"_s + QString::fromLatin1(digest);
}

QString CheckpointManager::gitDir() const
{
    const QString root = shadowRoot();
    return root.isEmpty() ? QString() : root + u"/repo/.git"_s;
}

bool CheckpointManager::isAvailable() const
{
    if (!m_enabled || m_workspace.isEmpty()) {
        return false;
    }
    if (!m_gitChecked) {
        m_gitChecked = true;
        QProcess probe;
        probe.start(u"git"_s, QStringList{u"--version"_s});
        m_gitAvailable = probe.waitForFinished(5000) && probe.exitCode() == 0;
        if (!m_gitAvailable) {
            m_unavailableReason = u"git was not found on PATH"_s;
        }
    }
    if (!m_gitAvailable) {
        return false;
    }
    // Creating the repository is a cheap side effect worth doing eagerly so
    // the first checkpoint in a turn does not pay for it.
    return ensureRepository();
}

bool CheckpointManager::ensureRepository(QString *error) const
{
    if (m_workspace.isEmpty()) {
        if (error) {
            *error = u"No workspace is open."_s;
        }
        return false;
    }
    if (!QFileInfo(m_workspace).isDir()) {
        if (error) {
            *error = u"The workspace directory no longer exists."_s;
        }
        return false;
    }
    const QString dir = gitDir();
    if (dir.isEmpty()) {
        if (error) {
            *error = u"No writable cache directory is available."_s;
        }
        return false;
    }
    if (QFileInfo(dir).isDir()) {
        return true;
    }

    QDir().mkpath(QFileInfo(dir).absolutePath());
    QProcess init;
    init.start(u"git"_s, QStringList{u"init"_s, u"--quiet"_s, QFileInfo(dir).absolutePath()});
    if (!init.waitForFinished(15000) || init.exitCode() != 0) {
        if (error) {
            *error = QString::fromUtf8(init.readAllStandardError()).trimmed();
            if (error->isEmpty()) {
                *error = u"git init failed."_s;
            }
        }
        return false;
    }
    // Run "git init" from inside the workspace so the repository picks up the
    // project's branch name, then bind the real workspace as its work tree.
    writeExcludes();
    return true;
}

void CheckpointManager::writeExcludes() const
{
    const QString excludePath = gitDir() + u"/info/exclude"_s;
    QDir().mkpath(QFileInfo(excludePath).absolutePath());

    QStringList lines = {
        u"# Kate AI checkpoints. The project history is managed by its own repo."_s,
        u"/.git/"_s,
        u".kateai/project_graph.json"_s,
        QString(),
        u"# Common build and dependency directories."_s,
        u"node_modules/"_s,
        u"__pycache__/"_s,
        u".venv/"_s,
        u".mypy_cache/"_s,
        u".pytest_cache/"_s,
        u"target/"_s,
        u".next/"_s,
        u".cache/"_s,
    };

    // Reuse the project's own ignore rules so snapshots stay small.
    QFile projectIgnore(m_workspace + u"/.gitignore"_s);
    if (projectIgnore.open(QIODevice::ReadOnly | QIODevice::Text)) {
        lines.append(QString());
        lines.append(u"# From the project .gitignore"_s);
        lines.append(QString::fromUtf8(projectIgnore.readAll()));
    }

    QFile file(excludePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(lines.join(u'\n').toUtf8());
    }
}

bool CheckpointManager::runGit(const QStringList &arguments, QString *output, QString *error, int timeoutMs) const
{
    const QString dir = gitDir();
    if (dir.isEmpty()) {
        if (error) {
            *error = u"No cache directory is available."_s;
        }
        return false;
    }

    QProcess git;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"GIT_DIR"_s, dir);
    environment.insert(u"GIT_WORK_TREE"_s, m_workspace);
    // Keep the user's global git config out of a repository they do not own.
    environment.insert(u"GIT_CONFIG_NOSYSTEM"_s, u"1"_s);
    environment.insert(u"GIT_TERMINAL_PROMPT"_s, u"0"_s);
    git.setProcessEnvironment(environment);
    git.setWorkingDirectory(m_workspace);

    QStringList full;
    full << u"-c"_s << u"user.name=Kate AI"_s << u"-c"_s << u"user.email=kateai@localhost"_s
         << u"-c"_s << u"commit.gpgsign=false"_s << arguments;

    git.start(u"git"_s, full);
    if (!git.waitForFinished(timeoutMs)) {
        git.kill();
        git.waitForFinished(1000);
        if (error) {
            *error = u"git timed out."_s;
        }
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(git.readAllStandardOutput());
    }
    if (git.exitCode() != 0) {
        if (error) {
            *error = QString::fromUtf8(git.readAllStandardError()).trimmed();
            if (error->isEmpty()) {
                *error = QStringLiteral("git exited with code %1").arg(git.exitCode());
            }
        }
        return false;
    }
    return true;
}

bool CheckpointManager::stageWorktree(QString *error) const
{
    // Staging first is what makes new and deleted files visible to a later
    // diff; without it `git diff <commit>` silently ignores untracked files.
    return runGit({u"add"_s, u"-A"_s, u"--"_s, u"."_s, u":(exclude).git"_s}, nullptr, error);
}

int CheckpointManager::nextSequence() const
{
    QString output;
    if (!runGit({u"for-each-ref"_s, u"--format=%(refname)"_s, TagNamespace}, &output, nullptr)) {
        return 1;
    }
    int highest = 0;
    const QStringList refs = output.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &ref : refs) {
        if (!isTagName(ref)) {
            continue;
        }
        const int lastSlash = ref.lastIndexOf(QLatin1Char('/'));
        const int separator = ref.lastIndexOf(QLatin1Char('-'));
        if (separator <= lastSlash) {
            continue;
        }
        bool ok = false;
        const int value = ref.mid(lastSlash + 1, separator - lastSlash - 1).toInt(&ok);
        if (ok) {
            highest = qMax(highest, value);
        }
    }
    return highest + 1;
}

QStringList CheckpointManager::checkpointHashes() const
{
    QStringList hashes;
    for (const QString &ref : checkpointRefs()) {
        hashes.append(hashFromTag(ref));
    }
    return hashes;
}

QStringList CheckpointManager::checkpointRefs() const
{
    QString output;
    if (!runGit({u"for-each-ref"_s, u"--sort=-refname"_s, u"--format=%(refname)"_s, TagNamespace}, &output, nullptr)) {
        return {};
    }
    return output.split(u'\n', Qt::SkipEmptyParts);
}

QString CheckpointManager::createCheckpoint(const QString &label, QString *error)
{
    if (!isAvailable()) {
        if (error && m_unavailableReason.isEmpty()) {
            *error = u"Checkpoints are unavailable."_s;
        }
        return {};
    }

    if (!stageWorktree(error)) {
        return {};
    }

    QString tree;
    if (!runGit({u"write-tree"_s}, &tree, error)) {
        return {};
    }
    tree = tree.trimmed();

    // No -p: every snapshot is an independent root, so deleting an old tag
    // really does let its commit be collected instead of pinning the chain.
    QString commit;
    if (!runGit({u"commit-tree"_s, tree, u"-m"_s, label.isEmpty() ? QStringLiteral("checkpoint") : label}, &commit, error)) {
        return {};
    }
    commit = commit.trimmed();

    if (!runGit({u"tag"_s, u"-f"_s, tagName(nextSequence(), commit), commit}, nullptr, error)) {
        return {};
    }

    prune();
    Q_EMIT checkpointCreated(commit, label);
    return commit;
}

void CheckpointManager::prune()
{
    if (checkpointHashes().size() <= m_retention) {
        return;
    }
    // Newest-first, so everything past the retention window goes.
    const QStringList refs = checkpointRefs();
    for (int i = m_retention; i < refs.size(); ++i) {
        runGit({u"tag"_s, u"-d"_s, shortTagName(refs.at(i))}, nullptr, nullptr, 5000);
    }
    runGit({u"reflog"_s, u"expire"_s, u"--expire=now"_s, u"--expire-unreachable=now"_s, u"--all"_s}, nullptr, nullptr, 10000);
    runGit({u"gc"_s, u"--prune=now"_s, u"--quiet"_s}, nullptr, nullptr, 30000);
}

QList<CheckpointInfo> CheckpointManager::checkpoints() const
{
    QList<CheckpointInfo> out;
    if (!isAvailable()) {
        return out;
    }
    QString log;
    // `git for-each-ref --format` passes text through verbatim, so the
    // separator has to be a real control character: the %x1f escape only
    // works with --pretty.
    const QChar separator(0x1f);
    const QString format = QStringLiteral("--format=%(objectname)%1%(creatordate:iso-strict)%1%(contents:subject)").arg(separator);
    if (!runGit({u"for-each-ref"_s, u"--sort=-refname"_s, format, TagNamespace}, &log, nullptr)) {
        return out;
    }
    const QStringList lines = log.split(u'\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split(separator);
        if (parts.size() < 3) {
            continue;
        }
        CheckpointInfo info;
        info.id = parts.at(0);
        info.shortId = info.id.left(8);
        info.createdAt = QDateTime::fromString(parts.at(1), Qt::ISODate);
        info.label = parts.at(2);
        out.append(info);
    }
    return out;
}

int CheckpointManager::changedFileCount(const QString &id) const
{
    if (!isAvailable() || id.isEmpty()) {
        return 0;
    }
    stageWorktree();
    QString output;
    if (!runGit({u"diff"_s, u"--cached"_s, u"--name-only"_s, id}, &output, nullptr)) {
        return 0;
    }
    const QStringList files = output.split(u'\n', Qt::SkipEmptyParts);
    return static_cast<int>(files.size());
}

QString CheckpointManager::diffAgainstCheckpoint(const QString &id, int maxLines) const
{
    if (!isAvailable() || id.isEmpty()) {
        return {};
    }
    stageWorktree();
    QString diff;
    if (!runGit({u"diff"_s, u"--cached"_s, u"--no-color"_s, id}, &diff, nullptr)) {
        return {};
    }
    if (diff.trimmed().isEmpty()) {
        return {};
    }
    const QStringList lines = diff.split(u'\n');
    if (lines.size() > maxLines) {
        QString head = lines.mid(0, maxLines).join(u'\n');
        head += u"\n... (diff truncated, "_s + QString::number(lines.size() - maxLines) + u" more lines)"_s;
        return head;
    }
    return diff;
}

bool CheckpointManager::restoreCheckpoint(const QString &id, QString *error)
{
    if (!isAvailable()) {
        if (error) {
            *error = m_unavailableReason.isEmpty() ? u"Checkpoints are unavailable."_s : m_unavailableReason;
        }
        return false;
    }
    if (id.isEmpty()) {
        if (error) {
            *error = u"No checkpoint was selected."_s;
        }
        return false;
    }

    QString listed;
    stageWorktree();
    if (!runGit({u"ls-files"_s}, &listed, error)) {
        return false;
    }
    const QStringList currentList = listed.split(u'\n', Qt::SkipEmptyParts);
        const QSet<QString> currentFiles(currentList.cbegin(), currentList.cend());

    if (!runGit({u"read-tree"_s, id}, nullptr, error)) {
        return false;
    }
    if (!runGit({u"checkout-index"_s, u"-a"_s, u"-f"_s}, nullptr, error)) {
        return false;
    }

    QString restored;
    if (!runGit({u"ls-files"_s}, &restored, nullptr)) {
        return true;
    }
    const QStringList restoredList = restored.split(u'\n', Qt::SkipEmptyParts);
        const QSet<QString> restoredFiles(restoredList.cbegin(), restoredList.cend());

    // Files that existed in the snapshot we rolled back from but are absent in
    // the checkpoint were added after it, so they must be removed.
    for (const QString &relativePath : currentFiles) {
        if (restoredFiles.contains(relativePath) || isProtectedPath(relativePath)) {
            continue;
        }
        const QString absolute = m_workspace + QLatin1Char('/') + relativePath;
        const QFileInfo info(absolute);
        if (info.isDir()) {
            QDir(absolute).removeRecursively();
        } else if (info.exists()) {
            QFile::remove(absolute);
        }
    }
    return true;
}

} // namespace KateAi