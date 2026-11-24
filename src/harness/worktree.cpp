/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "worktree.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryFile>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{

QString g_cacheRootOverride;

struct GitResult {
    bool ok = false;
    int exitCode = -1;
    QString out;
    QString err;
};

GitResult runGit(const QString &workingDir, const QStringList &arguments, int timeoutMs = 20000)
{
    GitResult result;
    QProcess git;
    git.setWorkingDirectory(workingDir);
    git.setProgram(u"git"_s);
    git.setArguments(arguments);
    // Keep the user's global git hooks/signing out of a throwaway worktree,
    // but retain identity and credential helpers for a normal repository.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(u"GIT_TERMINAL_PROMPT"_s, u"0"_s);
    git.setProcessEnvironment(env);
    git.start();
    if (!git.waitForStarted(5000)) {
        result.err = u"git could not be started"_s;
        return result;
    }
    if (!git.waitForFinished(timeoutMs)) {
        git.kill();
        git.waitForFinished(1000);
        result.err = u"git timed out"_s;
        return result;
    }
    result.exitCode = git.exitCode();
    result.out = QString::fromUtf8(git.readAllStandardOutput());
    result.err = QString::fromUtf8(git.readAllStandardError());
    result.ok = git.exitStatus() == QProcess::NormalExit && result.exitCode == 0;
    return result;
}

QString safeName(const QString &value)
{
    QString out;
    for (const QChar c : value) {
        if (c.isLetterOrNumber() || c == u'-' || c == u'_') {
            out.append(c);
        } else if (!out.endsWith(u'-')) {
            out.append(u'-');
        }
    }
    while (out.startsWith(u'-')) {
        out.remove(0, 1);
    }
    while (out.endsWith(u'-')) {
        out.chop(1);
    }
    return out.isEmpty() ? u"task"_s : out.left(64);
}

bool pathUnder(const QString &path, const QString &root)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonical.isEmpty() || canonicalRoot.isEmpty()) {
        return false;
    }
    return canonical == canonicalRoot
        || canonical.startsWith(canonicalRoot + QDir::separator());
}

} // namespace

void SubagentWorktree::setCacheRootForTests(const QString &path)
{
    g_cacheRootOverride = path;
}

QString SubagentWorktree::cacheRoot()
{
    if (!g_cacheRootOverride.isEmpty()) {
        return g_cacheRootOverride;
    }
    const QString env = QProcessEnvironment::systemEnvironment().value(u"KATEAI_WORKTREE_ROOT"_s);
    if (!env.trimmed().isEmpty()) {
        return env.trimmed();
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + u"/kateai"_s;
    }
    return base + u"/worktrees"_s;
}

bool SubagentWorktree::isAvailable(const QString &workspace, QString *reason)
{
    if (reason) {
        reason->clear();
    }
    if (workspace.isEmpty() || !QFileInfo(workspace).isDir()) {
        if (reason) {
            *reason = u"The workspace is not a directory."_s;
        }
        return false;
    }
    const GitResult version = runGit(workspace, {u"--version"_s}, 5000);
    if (!version.ok) {
        if (reason) {
            *reason = u"git is not available: "_s + version.err.trimmed();
        }
        return false;
    }
    const GitResult inside = runGit(workspace, {u"rev-parse"_s, u"--is-inside-work-tree"_s}, 5000);
    if (!inside.ok || inside.out.trimmed() != u"true"_s) {
        if (reason) {
            *reason = u"The workspace is not a git working tree."_s;
        }
        return false;
    }
    const GitResult root = runGit(workspace, {u"rev-parse"_s, u"--show-toplevel"_s}, 5000);
    if (!root.ok || root.out.trimmed().isEmpty()) {
        if (reason) {
            *reason = u"Could not determine the repository root."_s;
        }
        return false;
    }
    return true;
}

QString SubagentWorktree::repositoryRoot(const QString &workspace)
{
    const GitResult root = runGit(workspace, {u"rev-parse"_s, u"--show-toplevel"_s}, 5000);
    return root.ok ? QDir::cleanPath(root.out.trimmed()) : QString();
}

WorktreeResult SubagentWorktree::create(const QString &workspace, const QString &taskId)
{
    WorktreeResult result;
    QString reason;
    if (!isAvailable(workspace, &reason)) {
        result.error = reason;
        return result;
    }
    const QString root = repositoryRoot(workspace);
    if (root.isEmpty()) {
        result.error = u"Could not determine the repository root."_s;
        return result;
    }
    const GitResult head = runGit(root, {u"rev-parse"_s, u"HEAD"_s}, 10000);
    if (!head.ok || head.out.trimmed().isEmpty()) {
        result.error = u"Could not resolve HEAD: "_s + head.err.trimmed();
        return result;
    }
    result.baseCommit = head.out.trimmed();

    const QString repoKey = QString::fromLatin1(
        QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    const QString base = cacheRoot();
    QDir().mkpath(base);
    const QString path = base + u"/kateai-%1-%2"_s.arg(repoKey, safeName(taskId));
    if (QFileInfo::exists(path)) {
        // A previous run was killed before cleanup. The path is under our
        // cache root, so removing it is safe; then prune the stale entry.
        runGit(root, {u"worktree"_s, u"remove"_s, u"--force"_s, path}, 20000);
        QDir(path).removeRecursively();
        runGit(root, {u"worktree"_s, u"prune"_s}, 10000);
    }
    const GitResult add = runGit(root,
        {u"worktree"_s, u"add"_s, u"--detach"_s, path, result.baseCommit}, 60000);
    if (!add.ok) {
        result.error = u"Could not create the isolated worktree: "_s + add.err.trimmed();
        return result;
    }
    if (!QFileInfo(path).isDir()) {
        result.error = u"The isolated worktree was not created."_s;
        return result;
    }
    result.path = path;
    result.ok = true;
    return result;
}

bool SubagentWorktree::remove(const QString &workspace, const QString &worktreePath, QString *error)
{
    if (error) {
        error->clear();
    }
    if (worktreePath.isEmpty()) {
        return true;
    }
    if (!pathUnder(worktreePath, cacheRoot())) {
        if (error) {
            *error = u"Refusing to remove a path outside the worktree cache."_s;
        }
        return false;
    }
    const QString root = repositoryRoot(workspace);
    bool ok = false;
    if (!root.isEmpty()) {
        const GitResult removed = runGit(root, {u"worktree"_s, u"remove"_s, u"--force"_s, worktreePath}, 30000);
        ok = removed.ok;
        runGit(root, {u"worktree"_s, u"prune"_s}, 10000);
    }
    if (!ok) {
        ok = QDir(worktreePath).removeRecursively();
    }
    if (!ok && error) {
        *error = u"Could not remove the isolated worktree."_s;
    }
    return ok;
}

QString SubagentWorktree::diff(const QString &worktreePath, const QString &baseCommit, QString *error)
{
    if (error) {
        error->clear();
    }
    if (worktreePath.isEmpty() || baseCommit.isEmpty()) {
        if (error) {
            *error = u"The worktree or its base commit is missing."_s;
        }
        return {};
    }
    // Stage first so added files (and deletions) appear in the diff. The index
    // belongs to the throwaway worktree, so this does not touch the parent.
    const GitResult add = runGit(worktreePath, {u"add"_s, u"-A"_s, u"--"_s, u"."_s}, 30000);
    if (!add.ok) {
        if (error) {
            *error = u"Could not stage the isolated changes: "_s + add.err.trimmed();
        }
        return {};
    }
    const GitResult diff = runGit(worktreePath,
        {u"diff"_s, u"--cached"_s, u"--binary"_s, u"--no-color"_s, u"--no-ext-diff"_s, baseCommit}, 60000);
    if (!diff.ok) {
        if (error) {
            *error = u"Could not read the isolated diff: "_s + diff.err.trimmed();
        }
        return {};
    }
    return diff.out;
}

QStringList SubagentWorktree::changedFiles(const QString &worktreePath, const QString &baseCommit,
                                           QString *error)
{
    if (error) {
        error->clear();
    }
    QStringList files;
    if (worktreePath.isEmpty() || baseCommit.isEmpty()) {
        return files;
    }
    runGit(worktreePath, {u"add"_s, u"-A"_s, u"--"_s, u"."_s}, 30000);
    const GitResult names = runGit(worktreePath,
        {u"diff"_s, u"--cached"_s, u"--name-only"_s, baseCommit}, 30000);
    if (!names.ok) {
        if (error) {
            *error = names.err.trimmed();
        }
        return files;
    }
    for (const QString &line : names.out.split(u'\n', Qt::SkipEmptyParts)) {
        const QString file = line.trimmed();
        if (!file.isEmpty() && !files.contains(file)) {
            files.append(file);
        }
    }
    return files;
}

bool SubagentWorktree::apply(const QString &workspace, const QString &patch, QString *error)
{
    if (error) {
        error->clear();
    }
    if (patch.trimmed().isEmpty()) {
        return true;
    }
    if (workspace.isEmpty()) {
        if (error) {
            *error = u"The workspace is empty."_s;
        }
        return false;
    }
    const QString root = repositoryRoot(workspace);
    if (root.isEmpty()) {
        if (error) {
            *error = u"The workspace is not inside a git repository."_s;
        }
        return false;
    }
    QTemporaryFile file(QDir::tempPath() + u"/kateai-worktree-XXXXXX.patch"_s);
    if (!file.open()) {
        if (error) {
            *error = u"Could not write the patch: "_s + file.errorString();
        }
        return false;
    }
    file.write(patch.toUtf8());
    file.flush();
    const QString path = file.fileName();
    const GitResult check = runGit(root, {u"apply"_s, u"--check"_s, u"--whitespace=nowarn"_s, path}, 30000);
    if (!check.ok) {
        if (error) {
            *error = u"The patch does not apply cleanly: "_s + check.err.trimmed();
        }
        return false;
    }
    const GitResult applied = runGit(root, {u"apply"_s, u"--whitespace=nowarn"_s, path}, 30000);
    if (!applied.ok) {
        if (error) {
            *error = u"The patch could not be applied: "_s + applied.err.trimmed();
        }
        return false;
    }
    return true;
}

} // namespace KateAi
