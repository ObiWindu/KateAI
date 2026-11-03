/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "checkpoint.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QMutex>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <chrono>
#include <thread>
#include <QRecursiveMutex>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
// Snapshots are tags, not a branch, because a linear branch keeps every
// ancestor reachable and retention pruning would never reclaim anything.
const QString TagPrefix = QStringLiteral("refs/tags/");
const QString TagNamespace = TagPrefix + QStringLiteral("kateai/");

// Never restore through these; they hold state this plugin regenerates, plus
// automation/config the agent must not be able to plant via a snapshot.
bool isProtectedPath(const QString &relativePath)
{
    // Compared lowered: filesystems vary in case and a planted path must not
    // dodge the guard via "KATEAI.MD" or ".GIT/".
    const QString lowered = relativePath.toLower();
    if (lowered.startsWith(u".git/"_s) || lowered == u".git"_s || lowered.startsWith(u".kateai/project_graph.json"_s)) {
        return true;
    }
    // Restoring these would reinstate agent-planted automation without prompt.
    // Every file RulesLoader feeds into the system prompt belongs here, not
    // just KATEAI.md: a snapshot that captured "safe" rules would otherwise
    // write them back over the ones the user is looking at.
    if (lowered == u".kateai/mcp.json"_s || lowered.startsWith(u".kateai/mcp.json/"_s)
        || lowered == u"kateai.md"_s || lowered.startsWith(u".kateai/rules/"_s)
        || lowered.startsWith(u".kateai/modes/"_s) || lowered.startsWith(u".kateai/agents/"_s)
        || lowered == u"agents.md"_s || lowered == u"claude.md"_s
        // .clinerules/.kilocoderules are loaded as a root file *and* as a
        // directory of rule files (see RulesLoader::ruleDirectories), so both
        // the path itself and anything under it belongs to the rules.
        || lowered == u".clinerules"_s || lowered.startsWith(u".clinerules/"_s)
        || lowered == u".kilocoderules"_s || lowered.startsWith(u".kilocoderules/"_s)) {
        return true;
    }
    return false;
}

// Tag names carry a zero-padded sequence so `for-each-ref --sort=-refname`
// yields newest-first even when several snapshots land in the same second.
// `git tag` prepends refs/tags/ itself, so it must be given the short name.
QString tagName(int sequence, const QString &hash)
{
    return QStringLiteral("kateai/%1-%2").arg(sequence, 8, 10, QLatin1Char('0')).arg(hash);
}

// A checkpoint id is used as a git revision, so it has to be a commit hash.
// Short 7-char prefixes are ambiguous in large repos (git warns but picks one),
// so require the full hash: SHA-1 40, SHA-256 64.
bool isValidCheckpointId(const QString &id)
{
    static const QRegularExpression hashPattern(u"^[0-9a-f]{40,64}$"_s);
    return hashPattern.match(id).hasMatch();
}

bool isTagName(const QString &ref)
{
    return ref.startsWith(TagNamespace);
}

// Recovers the snapshot commit hash from a tag ref name. The tag carries the
// sequence and the hash ("<sequence>-<hash>"); only the hash identifies the
// commit, and it is the only part that can be used as a revision.
QString hashFromTag(const QString &ref)
{
    const QString suffix = ref.mid(TagNamespace.size());
    const int separator = suffix.lastIndexOf(QLatin1Char('-'));
    return separator > 0 ? suffix.mid(separator + 1) : suffix;
}

// Turns a full ref name back into the short name `git tag -d` expects.
QString shortTagName(const QString &ref)
{
    return ref.startsWith(TagPrefix) ? ref.mid(TagPrefix.size()) : ref;
}

// Runs git with the shadow repository and workspace bound in. A free function
// so the worker thread can run it without touching any member of the manager
// (whose workspace may change on the UI thread while a snapshot is in git).
bool runGitProcess(const QString &gitDir, const QString &workspace,
                   const QStringList &arguments, QString *output, QString *error, int timeoutMs = 20000)
{
    if (gitDir.isEmpty()) {
        if (error) {
            *error = u"No cache directory is available."_s;
        }
        return false;
    }

    QProcess git;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"GIT_DIR"_s, gitDir);
    environment.insert(u"GIT_WORK_TREE"_s, workspace);
    // Keep the user's global git config out of a repository they do not own.
    environment.insert(u"GIT_CONFIG_NOSYSTEM"_s, u"1"_s);
    environment.insert(u"GIT_TERMINAL_PROMPT"_s, u"0"_s);
    git.setProcessEnvironment(environment);
    git.setWorkingDirectory(workspace);

    QStringList full;
    full << u"-c"_s << u"user.name=Kate AI"_s << u"-c"_s << u"user.email=kateai@localhost"_s
         << u"-c"_s << u"commit.gpgsign=false"_s << arguments;

    git.start(u"git"_s, full);
    // Drain both channels while git runs, the way ToolRunner::bash does: a
    // blocking waitForFinished() leaves the draining to Qt's internals, and a
    // `git diff` of a large staged change would sit whole in memory. stdout is
    // capped and the tail dropped with a marker instead of growing unboundedly;
    // stderr is drained every pass so a chatty git cannot block on a full pipe.
    constexpr qint64 kMaxGitOutputBytes = 8 * 1024 * 1024;
    QByteArray stdOut;
    QByteArray stdErr;
    bool capped = false;
    bool timedOut = false;
    QElapsedTimer elapsed;
    elapsed.start();
    while (true) {
        stdOut += git.readAllStandardOutput();
        stdErr += git.readAllStandardError();
        if (stdOut.size() > kMaxGitOutputBytes) {
            capped = true;
            break;
        }
        if (git.waitForFinished(50)) {
            break;
        }
        if (git.state() == QProcess::NotRunning) {
            break;
        }
        if (elapsed.elapsed() >= timeoutMs) {
            timedOut = true;
            break;
        }
    }
    if (capped || timedOut) {
        git.kill();
        git.waitForFinished(1000);
    }
    stdOut += git.readAllStandardOutput();
    stdErr += git.readAllStandardError();
    if (timedOut) {
        if (error) {
            *error = u"git timed out."_s;
        }
        return false;
    }
    if (capped) {
        if (output) {
            *output = QString::fromUtf8(stdOut.left(kMaxGitOutputBytes)) + u"\n... (output truncated)"_s;
        }
        if (error) {
            *error = u"git output exceeded the cap and was truncated."_s;
        }
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(stdOut);
    }
    if (git.exitCode() != 0) {
        if (error) {
            *error = QString::fromUtf8(stdErr).trimmed();
            if (error->isEmpty()) {
                *error = QStringLiteral("git exited with code %1").arg(git.exitCode());
            }
        }
        return false;
    }
    return true;
}

// The exclude list is rewritten for every snapshot: a .gitignore that was
// added or changed after the cache repository was created would otherwise be
// ignored, and the snapshot would stage the whole tree.
bool writeExcludesFor(const QString &gitDir, const QString &workspace)
{
    const QString excludePath = gitDir + u"/info/exclude"_s;
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
    // Bounded: a huge .gitignore must not be read whole into memory.
    QFile projectIgnore(workspace + u"/.gitignore"_s);
    if (projectIgnore.open(QIODevice::ReadOnly | QIODevice::Text)) {
        lines.append(QString());
        lines.append(u"# From the project .gitignore"_s);
        static constexpr qint64 kMaxGitignoreBytes = 1024 * 1024;
        lines.append(QString::fromUtf8(projectIgnore.read(kMaxGitignoreBytes)));
    }

    QFile file(excludePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    file.write(lines.join(u'\n').toUtf8());
    return true;
}

// Sequence number for the next snapshot tag. The caller holds the checkpoint
// mutex: the sequence is read and used as one unit.
int nextSequenceFor(const QString &gitDir, const QString &workspace)
{
    QString output;
    if (!runGitProcess(gitDir, workspace,
                       {u"for-each-ref"_s, u"--format=%(refname)"_s, TagNamespace}, &output, nullptr)) {
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

// Stages the workspace and writes the snapshot commit. The caller holds the
// checkpoint mutex: staging, the tree and the tag must be one unit for the
// shadow index.
QString snapshotWorkspace(const QString &gitDir, const QString &workspace, const QString &label, QString *error)
{
    writeExcludesFor(gitDir, workspace);

    // `git add -A` has no size cap of its own: warn when the workspace is
    // huge so a multi-gigabyte tree does not freeze the turn silently.
    // (The excludes above keep the usual build dirs out; this is the last
    // resort for an unexpectedly large tree.)
    {
        qint64 approxBytes = 0;
        QDirIterator walk(workspace,
                          QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
        int counted = 0;
        static constexpr qint64 kSnapshotWarnBytes = 500LL * 1024 * 1024;
        while (walk.hasNext() && counted < 20000) {
            walk.next();
            approxBytes += walk.fileInfo().size();
            ++counted;
            if (approxBytes > kSnapshotWarnBytes) {
                qWarning() << "KateAI: workspace exceeds 500MB, checkpoint staging may be slow:" << workspace;
                break;
            }
        }
    }

    // Staging first is what makes new and deleted files visible to a later
    // diff; without it `git diff <commit>` silently ignores untracked files.
    if (!runGitProcess(gitDir, workspace,
                       {u"add"_s, u"-A"_s, u"--"_s, u"."_s, u":(exclude).git"_s}, nullptr, error)) {
        return {};
    }

    QString tree;
    if (!runGitProcess(gitDir, workspace, {u"write-tree"_s}, &tree, error)) {
        return {};
    }
    tree = tree.trimmed();

    // No -p: every snapshot is an independent root, so deleting an old tag
    // really does let its commit be collected instead of pinning the chain.
    QString commit;
    if (!runGitProcess(gitDir, workspace,
                       {u"commit-tree"_s, tree, u"-m"_s, label.isEmpty() ? QStringLiteral("checkpoint") : label},
                       &commit, error)) {
        return {};
    }
    commit = commit.trimmed();

    if (!runGitProcess(gitDir, workspace,
                       {u"tag"_s, u"-f"_s, tagName(nextSequenceFor(gitDir, workspace), commit), commit},
                       nullptr, error)) {
        return {};
    }
    return commit;
}
} // namespace

CheckpointManager::CheckpointManager(QObject *parent)
    : QObject(parent)
{
}

CheckpointManager::~CheckpointManager()
{
    // A git process that is still running is stopped, so it does not keep
    // the repository locked after the manager is gone.
    for (QProcess *git : std::as_const(m_detachedProcesses)) {
        if (git && git->state() != QProcess::NotRunning) {
            git->kill();
            git->waitForFinished(1000);
        }
        if (git) {
            git->deleteLater();
        }
    }
    m_detachedProcesses.clear();
    // A snapshot worker holds m_checkpointMutex and queues its result back
    // onto this object via QueuedConnection, so a worker that finishes during
    // teardown drops its result instead of touching freed memory. Do not
    // destroy a locked mutex: wait briefly, then detach rather than block
    // teardown for 30s.
    QElapsedTimer elapsed;
    elapsed.start();
    while (m_inFlightSnapshots.loadRelaxed() > 0 && elapsed.elapsed() < 2000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (m_inFlightSnapshots.loadRelaxed() > 0) {
        // The cap above keeps teardown bounded, but proceeding means the mutex
        // below may be destroyed under a wedged worker. Say so, so the crash
        // that follows is diagnosable instead of mysterious.
        qWarning() << "KateAI: a checkpoint worker is still running at teardown; its result is dropped.";
    }
}

void CheckpointManager::setWorkspace(const QString &workspace)
{
    if (m_workspace == workspace) {
        return;
    }
    m_workspace = workspace;
    m_unavailableReason.clear();
    // The cache directory keeps one repository per project that was ever
    // opened. The ones that are not needed any more are removed.
    pruneStaleShadowRepositories();
}

void CheckpointManager::pruneStaleShadowRepositories() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        return;
    }
    const QString root = base + u"/checkpoints"_s;
    QDir dir(root);
    if (!dir.exists()) {
        return;
    }
    // A repository of a project that no longer exists is useless, and one
    // that was not touched for a month is not worth the space.
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-30);
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    // The repository of the workspace that is open right now is never pruned,
    // however old it looks: deleting it would throw away the checkpoints of
    // the project the user is actively working on.
    const QString current = m_workspace.isEmpty() ? QString() : QFileInfo(shadowRoot()).fileName();
    for (const QFileInfo &entry : entries) {
        if (!current.isEmpty() && entry.fileName() == current) {
            continue;
        }
        // Never follow symlinks out of the cache dir.
        if (entry.isSymLink()) {
            continue;
        }
        const QString marker = entry.absoluteFilePath() + u"/workspace"_s;
        QFile file(marker);
        QString workspace;
        QString fileError;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            workspace = QString::fromUtf8(file.readAll()).trimmed();
        } else if (file.exists()) {
            fileError = file.errorString();
        }
        if (!fileError.isEmpty()) {
            // Unreadable (EACCES etc.): do not delete what cannot be inspected.
            continue;
        }
        if (!workspace.isEmpty() && QFileInfo::exists(workspace)) {
            // The project is still there. It may be old, but the repository
            // belongs to a project that exists.
            continue;
        }
        // Use the marker file's mtime (last real use), not the directory's.
        QDateTime mtime = entry.lastModified();
        const QFileInfo markerInfo(marker);
        if (markerInfo.exists()) {
            mtime = markerInfo.lastModified();
        }
        if (mtime < cutoff || workspace.isEmpty()) {
            // The project is gone, or the repository was never finished.
            QDir(entry.absoluteFilePath()).removeRecursively();
        }
    }
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
    QString norm = QDir::cleanPath(m_workspace);
    // Strip a trailing slash (except the filesystem root) so "/ws" and "/ws/"
    // hash to the same shadow repository instead of two.
    while (norm.size() > 1 && norm.endsWith(QLatin1Char('/'))) {
        norm.chop(1);
    }
    const QByteArray digest = QCryptographicHash::hash(norm.toUtf8(), QCryptographicHash::Sha1).toHex();
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
    QMutexLocker locker(&m_checkpointMutex);
    if (!m_enabled || m_workspace.isEmpty()) {
        return false;
    }
    if (!m_gitChecked) {
        m_gitChecked = true;
        QProcess probe;
        probe.start(u"git"_s, QStringList{u"--version"_s});
        m_gitAvailable = probe.waitForFinished(2000) && probe.exitCode() == 0;
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
    QMutexLocker locker(&m_checkpointMutex);
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

    // TOCTOU: never init through a symlinked cache path.
    if (QFileInfo(QFileInfo(dir).absolutePath()).isSymLink() || QFileInfo(dir).isSymLink()) {
        if (error) {
            *error = u"The checkpoint cache path is a symlink; refusing to initialise."_s;
        }
        return false;
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
    // The workspace is written into the repository, so the cleanup knows
    // which project a shadow repository belongs to.
    {
        QFile marker(shadowRoot() + u"/workspace"_s);
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            marker.write(m_workspace.toUtf8());
        }
    }
    return true;
}

void CheckpointManager::writeExcludes() const
{
    writeExcludesFor(gitDir(), m_workspace);
}

void CheckpointManager::runGitDetached(const QStringList &arguments, int timeoutMs)
{
    const QString dir = gitDir();
    if (dir.isEmpty()) {
        return;
    }
    // The process is parented to the manager, so it goes away with it. The
    // result is not waited for: the command is housekeeping that nobody has
    // to see.
    auto *git = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"GIT_DIR"_s, dir);
    environment.insert(u"GIT_WORK_TREE"_s, m_workspace);
    environment.insert(u"GIT_CONFIG_NOSYSTEM"_s, u"1"_s);
    environment.insert(u"GIT_TERMINAL_PROMPT"_s, u"0"_s);
    git->setProcessEnvironment(environment);
    git->setWorkingDirectory(m_workspace);

    QStringList full;
    full << u"-c"_s << u"user.name=Kate AI"_s << u"-c"_s << u"user.email=kateai@localhost"_s
         << u"-c"_s << u"commit.gpgsign=false"_s << arguments;

    // A command that runs forever is stopped; it would keep the repository
    // locked for good.
    auto *guard = new QTimer(git);
    guard->setSingleShot(true);
    guard->setInterval(timeoutMs);
    connect(guard, &QTimer::timeout, git, [git]() {
        git->kill();
    });
    connect(git, &QProcess::finished, git, [this, git]() {
        m_detachedProcesses.removeAll(git);
        git->deleteLater();
    });
    connect(git, &QProcess::errorOccurred, git, [this, git](QProcess::ProcessError) {
        m_detachedProcesses.removeAll(git);
        git->deleteLater();
    });
    m_detachedProcesses.append(git);
    git->start(u"git"_s, full);
}

bool CheckpointManager::runGit(const QStringList &arguments, QString *output, QString *error, int timeoutMs) const
{
    return runGitProcess(gitDir(), m_workspace, arguments, output, error, timeoutMs);
}

bool CheckpointManager::stageWorktree(QString *error) const
{
    // Staging first is what makes new and deleted files visible to a later
    // diff; without it `git diff <commit>` silently ignores untracked files.
    return runGit({u"add"_s, u"-A"_s, u"--"_s, u"."_s, u":(exclude).git"_s}, nullptr, error);
}

int CheckpointManager::nextSequence() const
{
    QMutexLocker locker(&m_checkpointMutex);

    return nextSequenceFor(gitDir(), m_workspace);
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
    QMutexLocker locker(&m_checkpointMutex);

    QString output;
    if (!runGit({u"for-each-ref"_s, u"--sort=-refname"_s, u"--format=%(refname)"_s, TagNamespace}, &output, nullptr)) {
        return {};
    }
    return output.split(u'\n', Qt::SkipEmptyParts);
}

QString CheckpointManager::createCheckpoint(const QString &label, QString *error)
{
    QMutexLocker locker(&m_checkpointMutex);

    if (!isAvailable()) {
        if (error) {
            *error = m_unavailableReason.isEmpty() ? u"Checkpoints are unavailable."_s : m_unavailableReason;
        }
        return {};
    }

    const QString commit = snapshotWorkspace(gitDir(), m_workspace, label, error);
    if (commit.isEmpty()) {
        return {};
    }

    prune();
    Q_EMIT checkpointCreated(commit, label);
    return commit;
}

void CheckpointManager::createCheckpointAsync(const QString &label)
{
    // The availability probe and the one-time repository creation stay on the
    // manager's thread: they touch the cached git flags, and the probe is a
    // single cheap `git --version` after the first call.
    if (!isAvailable()) {
        Q_EMIT failed(m_unavailableReason.isEmpty() ? u"Checkpoints are unavailable."_s : m_unavailableReason);
        return;
    }

    // Capture the state here rather than reading members from the worker:
    // setWorkspace() may run on the UI thread while the snapshot is in git.
    const QString dir = gitDir();
    const QString workspace = m_workspace;
    // Staging a big tree on the first snapshot can take seconds; running the
    // git commands on the UI thread froze the editor for the whole time. The
    // snapshot runs in the global thread pool instead, and the result is
    // marshalled back to the manager's thread (prune() and the signals stay
    // there, because prune() may start detached QProcess objects).
    // Counted before start(): the destructor waits on this, and a worker
    // that is queued but not yet running must still be waited for.
    m_inFlightSnapshots.ref();
    QThreadPool::globalInstance()->start([this, dir, workspace, label]() {
        // Balanced in every path below: the destructor waits on this counter
        // before the mutex it guards goes away.
        const auto release = qScopeGuard([this] { m_inFlightSnapshots.deref(); });
        QString error;
        QString commit;
        {
            // The shadow index is shared with the UI thread (diffs, restores);
            // the same mutex that serialises those serialises this too.
            QMutexLocker locker(&m_checkpointMutex);
            commit = snapshotWorkspace(dir, workspace, label, &error);
        }
        QMetaObject::invokeMethod(this, [this, commit, label, error]() {
            if (commit.isEmpty()) {
                Q_EMIT failed(error.isEmpty() ? u"Checkpoints are unavailable."_s : error);
                return;
            }
            prune();
            Q_EMIT checkpointCreated(commit, label);
        }, Qt::QueuedConnection);
    });
}

void CheckpointManager::prune()
{
    QMutexLocker locker(&m_checkpointMutex);

    if (checkpointHashes().size() <= m_retention) {
        return;
    }
    // Newest-first, so everything past the retention window goes.
    const QStringList refs = checkpointRefs();
    for (int i = m_retention; i < refs.size(); ++i) {
        runGit({u"tag"_s, u"-d"_s, shortTagName(refs.at(i))}, nullptr, nullptr, 5000);
    }
    // The housekeeping runs in the background. "git gc" walks the whole
    // object store and can take seconds; doing that on the UI thread froze
    // the editor for the whole time.
    //
    // The pruning uses the default grace period and not "--prune=now": a
    // snapshot is written in two steps (the commit, then the tag), and a
    // collection that runs in between would delete the commit that the tag
    // is about to point at.
    runGitDetached({u"reflog"_s, u"expire"_s, u"--expire=now"_s, u"--expire-unreachable=now"_s, u"--all"_s});
    runGitDetached({u"gc"_s, u"--quiet"_s});
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
    if (!isAvailable() || !isValidCheckpointId(id) || !checkpointHashes().contains(id)) {
        return 0;
    }
    // The mutex is held for the whole call: the staging and the diff run git
    // commands that touch the same index, and two of them at once race for
    // the index lock.
    QMutexLocker locker(&m_checkpointMutex);
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
    if (!isAvailable() || !isValidCheckpointId(id) || !checkpointHashes().contains(id)) {
        return {};
    }
    // The same reason as in changedFileCount(): the git commands touch the
    // shared index.
    QMutexLocker locker(&m_checkpointMutex);
    stageWorktree();
    QString diff;
    if (!runGit({u"diff"_s, u"--cached"_s, u"--no-color"_s, id}, &diff, nullptr)) {
        return {};
    }
    if (diff.trimmed().isEmpty()) {
        return {};
    }
    // Bounded: an uncapped maxLines lets a huge diff grow without limit.
    const int capped = qBound(1, maxLines, 5000);
    const QStringList lines = diff.split(u'\n');
    if (lines.size() > capped) {
        QString head = lines.mid(0, capped).join(u'\n');
        head += u"\n... (diff truncated, "_s + QString::number(lines.size() - capped) + u" more lines)"_s;
        return head;
    }
    return diff;
}

bool CheckpointManager::restoreCheckpoint(const QString &id, QString *error)
{
    QMutexLocker locker(&m_checkpointMutex);

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
    // The id reaches git as a revision, and git reads an argument that starts
    // with a dash as an option. Only a commit hash is accepted.
    if (!isValidCheckpointId(id)) {
        if (error) {
            *error = u"Not a valid checkpoint id."_s;
        }
        return false;
    }
    // Only checkpoints this manager created may be restored: a dangling or
    // planted commit hash that merely looks like hex must not reach read-tree.
    if (!checkpointHashes().contains(id)) {
        if (error) {
            *error = u"Unknown checkpoint id."_s;
        }
        return false;
    }

    QString listed;
    if (!stageWorktree(error)) {
        return false;
    }
    if (!runGit({u"ls-files"_s, u"-z"_s}, &listed, error)) {
        return false;
    }
    const QStringList currentList = listed.split(QChar(u'\0'), Qt::SkipEmptyParts);
    const QSet<QString> currentFiles(currentList.cbegin(), currentList.cend());

    if (!runGit({u"read-tree"_s, id}, nullptr, error)) {
        return false;
    }
    // checkout-index -a writes every file in the index, including paths the
    // snapshot must never restore: a snapshot taken while .kateai/mcp.json or
    // a mode file held agent-planted content would write it back over the
    // working copy. isProtectedPath() guards the deletion loop below, not the
    // overwrite, so drop those entries from the index first and the checkout
    // leaves the current files alone. -z keeps non-ASCII names unquoted, so
    // the paths reach update-index exactly as git stored them.
    QString indexed;
    if (runGit({u"ls-files"_s, u"-z"_s}, &indexed, nullptr)) {
        const QStringList entries = indexed.split(QChar(u'\0'), Qt::SkipEmptyParts);
        for (const QString &relativePath : entries) {
            if (isProtectedPath(relativePath)) {
                runGit({u"update-index"_s, u"--force-remove"_s, u"--"_s, relativePath}, nullptr, nullptr);
            }
        }
        // Symlink guard: never let checkout-index write through a symlinked
        // workspace path. Remove symlinked targets first so git recreates a
        // regular file instead of following the link.
        const QString cleanWorkspace = QDir::cleanPath(m_workspace);
        for (const QString &relativePath : entries) {
            if (isProtectedPath(relativePath)) {
                continue;
            }
            const QString absolute = QDir::cleanPath(m_workspace + QLatin1Char('/') + relativePath);
            if (absolute != cleanWorkspace && !absolute.startsWith(cleanWorkspace + QLatin1Char('/'))) {
                runGit({u"update-index"_s, u"--force-remove"_s, u"--"_s, relativePath}, nullptr, nullptr);
                continue;
            }
            if (QFileInfo(absolute).isSymLink()) {
                QFile::remove(absolute);
                if (QFileInfo(absolute).isSymLink()) {
                    runGit({u"update-index"_s, u"--force-remove"_s, u"--"_s, relativePath}, nullptr, nullptr);
                }
            }
        }
    }
    if (!runGit({u"checkout-index"_s, u"-a"_s, u"-f"_s}, nullptr, error)) {
        return false;
    }

    QString restored;
    if (!runGit({u"ls-files"_s, u"-z"_s}, &restored, nullptr)) {
        if (error) {
            *error = u"Could not list restored files; the workspace was left untouched."_s;
        }
        return false;
    }
    const QStringList restoredList = restored.split(QChar(u'\0'), Qt::SkipEmptyParts);
    const QSet<QString> restoredFiles(restoredList.cbegin(), restoredList.cend());

    // Files that existed in the snapshot we rolled back from but are absent in
    // the checkpoint were added after it, so they must be removed.
    const QString cleanWorkspace = QDir::cleanPath(m_workspace);
    for (const QString &relativePath : currentFiles) {
        if (restoredFiles.contains(relativePath) || isProtectedPath(relativePath)) {
            continue;
        }
        const QString absolute = QDir::cleanPath(m_workspace + QLatin1Char('/') + relativePath);
        if (absolute != cleanWorkspace && !absolute.startsWith(cleanWorkspace + QLatin1Char('/'))) {
            continue;
        }
        const QFileInfo info(absolute);
        if (info.isSymLink()) {
            QFile::remove(absolute);
            continue;
        }
        if (info.isDir()) {
            QDir(absolute).removeRecursively();
        } else if (info.exists()) {
            QFile::remove(absolute);
        }
    }
    return true;
}

} // namespace KateAi