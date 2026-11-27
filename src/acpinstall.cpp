/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpinstall.h"
#include "types.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <KLocalizedString>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSysInfo>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
#include <QUrl>

#if defined(Q_OS_UNIX)
#include <cstdio>
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
bool isTarArchive(const QString &archiveUrl, const QString &archivePath)
{
    QFile file(archivePath);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.read(512);
        if (head.startsWith("PK\x03\x04") || head.startsWith("PK\x05\x06")) {
            return false;
        }
        if (head.startsWith("\x1f\x8b") || head.startsWith("BZh")
            || (head.size() >= 6 && head.startsWith("\xfd" "7zXZ"))
            || (head.size() >= 4 && head.startsWith("\x28\xb5\x2f\xfd"))
            || (head.size() >= 262 && head.mid(257, 5) == "ustar")) {
            return true;
        }
    }
    const QString path = QUrl(archiveUrl).path().toLower();
    return path.contains(u".tar"_s)
        || path.endsWith(u".tgz"_s)
        || path.endsWith(u".tbz2"_s)
        || path.endsWith(u".tbz"_s)
        || path.endsWith(u".txz"_s)
        || path.endsWith(u".tzst"_s);
}

QString findArchiveExtractor(const QString &archiveUrl, const QString &archive,
                             const QString &destination, QStringList *arguments)
{
    // Some registry entries ship tar archives (Mistral Vibe, OpenCode),
    // others ZIP files (Antigravity, Kilo). Pick the matching extractor from
    // file magic first, falling back to URL suffix.
    const bool tarArchive = isTarArchive(archiveUrl, archive);
    if (tarArchive) {
        const QString tar = QStandardPaths::findExecutable(u"tar"_s);
        if (!tar.isEmpty()) {
            *arguments = {u"-xf"_s, archive, u"-C"_s, destination};
            return tar;
        }
        return {};
    }
#if defined(Q_OS_WIN)
    const QString tar = QStandardPaths::findExecutable(u"tar"_s);
    if (!tar.isEmpty()) {
        *arguments = {u"-xf"_s, archive, u"-C"_s, destination};
        return tar;
    }
#else
    const QString unzip = QStandardPaths::findExecutable(u"unzip"_s);
    if (!unzip.isEmpty()) {
        *arguments = {u"-o"_s, archive, u"-d"_s, destination};
        return unzip;
    }
    // Minimal Linux images often ship Python without unzip. zipfile is part of
    // the standard library and handles the registry archives correctly.
    const QString python = QStandardPaths::findExecutable(u"python3"_s);
    if (!python.isEmpty()) {
        const QString zipScript = uR"PY(
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
dest = os.path.realpath(sys.argv[2])
for info in z.infolist():
    target = os.path.realpath(os.path.join(dest, info.filename))
    if target != dest and not target.startswith(dest + os.sep):
        raise SystemExit("unsafe path: " + info.filename)
    path = z.extract(info, sys.argv[2])
    mode = (info.external_attr >> 16) & 0o777
    if mode:
        os.chmod(path, mode)
)PY"_s;
        *arguments = {u"-c"_s, zipScript, archive, destination};
        return python;
    }
#endif
    return {};
}

// The registry may publish either an archive or a raw executable. The temp
// file has a generic name, so decide from the URL first and the file magic
// second. Treating a raw binary as a zip made muse-code/sigit uninstallable.
bool archiveIsArchive(const QString &archiveUrl, const QString &archivePath)
{
    const QString path = QUrl(archiveUrl).path().toLower();
    static const QStringList archiveSuffixes = {
        u".zip"_s, u".tar"_s, u".tgz"_s, u".tar.gz"_s, u".tbz2"_s, u".tar.bz2"_s,
        u".txz"_s, u".tar.xz"_s, u".tar.zst"_s, u".tzst"_s,
    };
    for (const QString &suffix : archiveSuffixes) {
        if (path.endsWith(suffix)) {
            return true;
        }
    }
    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray head = file.read(512);
    if (head.startsWith("PK\x03\x04") || head.startsWith("PK\x05\x06")
        || head.startsWith("\x1f\x8b") || head.startsWith("BZh")) {
        return true;
    }
    if (head.size() >= 6 && head.startsWith("\xfd" "7zXZ")) {
        return true;
    }
    if (head.size() >= 4 && head.startsWith("\x28\xb5\x2f\xfd")) {
        return true;
    }
    if (head.size() >= 262 && head.mid(257, 5) == "ustar") {
        return true;
    }
    return false;
}

QString findInstalledBinary(const QString &installDir, const QString &recordedCommand)
{    QString relative = recordedCommand;
    while (relative.startsWith(u"./"_s)) {
        relative.remove(0, 2);
    }
    const QString candidate = QDir(installDir).filePath(relative);
    if (QFileInfo(candidate).isFile() && !QFileInfo(candidate).isSymLink()) {
        return QFileInfo(candidate).absoluteFilePath();
    }
    // Some archives wrap their payload in a top-level directory. The registry
    // still records "./<name>", so locate the executable by name.
    const QString baseName = QFileInfo(relative).fileName();
    if (!baseName.isEmpty()) {
        QDirIterator it(installDir, {baseName}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString found = it.next();
            if (!QFileInfo(found).isSymLink()) {
                return found;
            }
        }
    }
    return {};
}

// An archive from the registry is still attacker-controlled if the registry
// entry or the download is compromised. Reject absolute paths and ".."
// components before handing it to tar/unzip, which would otherwise write
// outside the per-agent install directory (zip-slip).
bool isSafeRelativeArchivePath(const QString &entry)
{
    const QString unix = QDir::fromNativeSeparators(entry.trimmed());
    if (unix.isEmpty()) {
        return true;
    }
    if (unix.startsWith(u'/') || (unix.size() > 1 && unix.at(1) == u':')) {
        return false;
    }
    const QStringList parts = QDir::cleanPath(unix).split(u'/');
    return !parts.contains(u".."_s);
}

bool archiveEntriesAreSafe(const QString &archiveUrl, const QString &archive, QString *badEntry)
{
    const bool tarArchive = isTarArchive(archiveUrl, archive);
    QString program;
    QStringList args;
    bool verboseModeCheck = false;
    bool pythonChecked = false;
    if (tarArchive) {
        program = QStandardPaths::findExecutable(u"tar"_s);
        if (!program.isEmpty()) {
            // -tvf lists mode + symlink/hardlink targets so links escaping the
            // install directory can be rejected before extraction.
            args = {u"-tvf"_s, archive};
            verboseModeCheck = true;
        } else {
            program = QStandardPaths::findExecutable(u"python3"_s);
            if (!program.isEmpty()) {
                const QString tarCheck = uR"PY(
import posixpath, sys, tarfile
with tarfile.open(sys.argv[1], "r:*") as tf:
    for m in tf.getmembers():
        n = m.name.replace("\\", "/")
        if n.startswith("/") or (len(n) > 1 and n[1] == ":") or ".." in posixpath.normpath(n).split("/"):
            print("UNSAFE:" + m.name)
            sys.exit(2)
        if m.isdev() or m.isfifo():
            print("UNSAFE:" + m.name)
            sys.exit(2)
        if m.issym() or m.islnk():
            t = m.linkname.replace("\\", "/")
            if t.startswith("/") or (len(t) > 1 and t[1] == ":"):
                print("UNSAFE:" + m.name + " -> " + m.linkname)
                sys.exit(2)
            base = posixpath.dirname(n) if m.issym() else ""
            resolved = posixpath.normpath(posixpath.join(base, t))
            if resolved == ".." or resolved.startswith("../"):
                print("UNSAFE:" + m.name + " -> " + m.linkname)
                sys.exit(2)
)PY"_s;
                args = {u"-c"_s, tarCheck, archive};
                pythonChecked = true;
            }
        }
    } else {
        program = QStandardPaths::findExecutable(u"python3"_s);
        if (!program.isEmpty()) {
            const QString zipCheck = uR"PY(
import posixpath, stat, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as zf:
    for info in zf.infolist():
        n = info.filename.replace("\\", "/")
        if n.startswith("/") or (len(n) > 1 and n[1] == ":") or ".." in posixpath.normpath(n).split("/"):
            print("UNSAFE:" + info.filename)
            sys.exit(2)
        mode = info.external_attr >> 16
        if stat.S_ISLNK(mode):
            print("UNSAFE_SYMLINK:" + info.filename)
            sys.exit(2)
)PY"_s;
            args = {u"-c"_s, zipCheck, archive};
            pythonChecked = true;
        } else {
            program = QStandardPaths::findExecutable(u"unzip"_s);
            if (!program.isEmpty()) {
                args = {u"-Z"_s, archive};
                verboseModeCheck = true;
            }
        }
    }
    if (program.isEmpty()) {
        // Fail closed when no archive inspector is available.
        if (badEntry) {
            *badEntry = u"no archive inspector available"_s;
        }
        return false;
    }
    QProcess list;
    list.start(program, args);
    if (!list.waitForFinished(15000)) {
        list.kill();
        list.waitForFinished(1000);
        if (badEntry) {
            *badEntry = u"archive listing timed out"_s;
        }
        return false;
    }
    if (list.exitStatus() != QProcess::NormalExit || list.exitCode() != 0) {
        const QString out = QString::fromLocal8Bit(list.readAllStandardOutput()).trimmed();
        if (badEntry) {
            *badEntry = out.isEmpty() ? u"archive listing failed"_s : out;
        }
        return false;
    }
    if (pythonChecked) {
        return true;
    }
    const QStringList lines = QString::fromLocal8Bit(list.readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (verboseModeCheck) {
            if (tarArchive) {
                const QChar kind = line.at(0);
                // Reject block/char devices and FIFOs outright.
                if (kind == u'b' || kind == u'c' || kind == u'p') {
                    if (badEntry) *badEntry = line;
                    return false;
                }
                // tar -tvf format: perms owner/group size date time path [-> target | link to target]
                const QStringList cols = line.split(u' ', Qt::SkipEmptyParts);
                if (cols.size() < 6) {
                    continue;
                }
                const QString remainder = cols.mid(5).join(u' ');
                if (kind == u'l' || kind == u'h') {
                    const QString sep = (kind == u'l') ? u" -> "_s : u" link to "_s;
                    const qsizetype sepIdx = remainder.indexOf(sep);
                    if (sepIdx < 0) {
                        if (badEntry) *badEntry = remainder;
                        return false;
                    }
                    const QString entryPath = remainder.left(sepIdx);
                    const QString linkTarget = remainder.mid(sepIdx + sep.size());
                    if (!isSafeRelativeArchivePath(entryPath) || !isSafeRelativeArchivePath(linkTarget)) {
                        if (badEntry) *badEntry = remainder;
                        return false;
                    }
                    // For symlinks, also verify the target resolved relative to the link's parent stays inside root.
                    if (kind == u'l') {
                        const QString parent = QFileInfo(QDir::fromNativeSeparators(entryPath)).path();
                        const QString joined = (parent.isEmpty() || parent == u"."_s)
                            ? linkTarget
                            : (parent + u'/' + linkTarget);
                        if (!isSafeRelativeArchivePath(joined)) {
                            if (badEntry) *badEntry = remainder;
                            return false;
                        }
                    }
                } else if (!isSafeRelativeArchivePath(remainder)) {
                    if (badEntry) *badEntry = remainder;
                    return false;
                }
            } else {
                // unzip -Z header/footer lines start with "Archive:" or digit counts;
                // member lines begin with a Unix permission mode (e.g. -rwxr-xr-x, drwxr-xr-x, lrwxrwxrwx).
                if (line.startsWith(u"Archive:"_s) || line.startsWith(u"Zip file size:"_s)
                    || line.at(0).isDigit()) {
                    continue;
                }
                if (line.startsWith(u'l')) {
                    if (badEntry) *badEntry = line;
                    return false;
                }
                const QStringList cols = line.split(u' ', Qt::SkipEmptyParts);
                if (cols.size() >= 9) {
                    const QString entryPath = cols.mid(8).join(u' ');
                    if (!isSafeRelativeArchivePath(entryPath)) {
                        if (badEntry) *badEntry = entryPath;
                        return false;
                    }
                }
            }
        } else if (!isSafeRelativeArchivePath(line)) {
            if (badEntry) *badEntry = line;
            return false;
        }
    }
    return true;
}
} // namespace

QString acpCurrentPlatformKey()
{
#if defined(Q_OS_WIN)
    if (QSysInfo::currentCpuArchitecture().startsWith(u"arm"_s)
        || QSysInfo::currentCpuArchitecture().startsWith(u"aarch64"_s)) {
        return u"windows-aarch64"_s;
    }
    return u"windows-x86_64"_s;
#elif defined(Q_OS_MAC)
    if (QSysInfo::currentCpuArchitecture().startsWith(u"arm"_s)
        || QSysInfo::currentCpuArchitecture().startsWith(u"aarch64"_s)) {
        return u"darwin-aarch64"_s;
    }
    return u"darwin-x86_64"_s;
#elif defined(Q_OS_LINUX)
    // The registry publishes separate musl targets. Installing the glibc build
    // on Alpine (and similar) produces a binary the loader cannot start, so
    // select the musl key when the host itself is musl.
    bool musl = false;
    bool loaderDetected = false;
    QFile maps(u"/proc/self/maps"_s);
    if (maps.open(QIODevice::ReadOnly)) {
        const QByteArray loaded = maps.read(256 * 1024);
        if (loaded.contains("ld-musl-")) {
            musl = true;
            loaderDetected = true;
        } else if (loaded.contains("ld-linux") || loaded.contains("ld64") || loaded.contains("ld.so")) {
            loaderDetected = true;
        }
    }
    if (!loaderDetected) {
        // Some containers hide /proc/self/maps; fall back to the common loader
        // paths. Musl images always ship one of them.
        for (const QString &candidate : {u"/lib/ld-musl-x86_64.so.1"_s, u"/lib/ld-musl-aarch64.so.1"_s,
                                         u"/usr/lib/ld-musl-x86_64.so.1"_s, u"/usr/lib/ld-musl-aarch64.so.1"_s}) {
            if (QFileInfo::exists(candidate)) {
                musl = true;
                break;
            }
        }
    }
    const bool arm = QSysInfo::currentCpuArchitecture().startsWith(u"arm"_s)
        || QSysInfo::currentCpuArchitecture().startsWith(u"aarch64"_s);
    if (musl) {
        return arm ? u"linux-aarch64-musl"_s : u"linux-x86_64-musl"_s;
    }
    return arm ? u"linux-aarch64"_s : u"linux-x86_64"_s;
#else
    return u"unknown"_s;
#endif
}

bool acpNeedsPageShim()
{
#if defined(Q_OS_LINUX)
    const long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 4096;
#else
    return false;
#endif
}

// A library that is loaded into the agent process through LD_PRELOAD must come
// from a place the user cannot write to: /tmp is world-writable and the
// working directory belongs to the project, so a file planted there would be
// loaded into the agent. Only the installation directory and the system
// directories are searched, and the file must not be writable by anyone else.
static bool isTrustedShimSource(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        return false;
    }
    if (info.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther)) {
        return false;
    }
    const QFileInfo dirInfo(info.absolutePath());
    if (dirInfo.permissions() & QFileDevice::WriteOther) {
        return false;
    }
    return true;
}

static bool filesHaveSameBytes(const QString &aPath, const QString &bPath)
{
    const QFileInfo aInfo(aPath);
    const QFileInfo bInfo(bPath);
    if (!aInfo.exists() || !bInfo.exists() || aInfo.isSymLink() || bInfo.isSymLink()
        || aInfo.size() != bInfo.size()) {
        return false;
    }
    QFile a(aPath);
    QFile b(bPath);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly)) {
        return false;
    }
    return a.readAll() == b.readAll();
}

static bool installShimAtomically(const QString &src, const QString &target)
{
    const QString staged = target + u".tmp-"_s
        + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    if (!QFile::copy(src, staged)) {
        QFile::remove(staged);
        return false;
    }
    QFile::setPermissions(staged, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                      | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                      | QFileDevice::ReadOther | QFileDevice::ExeOther);
#if defined(Q_OS_UNIX)
    if (::rename(QFile::encodeName(staged).constData(), QFile::encodeName(target).constData()) == 0) {
        return true;
    }
    QFile::remove(staged);
    return false;
#else
    QFile::remove(target);
    if (QFile::rename(staged, target)) {
        return true;
    }
    QFile::remove(staged);
    return false;
#endif
}

QString acpEnsurePageShim(const QString &dir)
{
    if (dir.isEmpty()) {
        return {};
    }
    const QString target = QDir(dir).filePath(u"libpage4k.so"_s);

    // The directories the shim may come from: the installation and the system
    // directories. The working directory and /tmp are deliberately not on the
    // list - a project or another user on the machine could plant a library
    // there, and it would then be loaded into the agent.
    const QStringList trustedDirs = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + u"/../lib"_s,
        QCoreApplication::applicationDirPath() + u"/../lib64"_s,
        QDir::homePath() + u"/.local/lib/kateai"_s,
        QDir::homePath() + u"/.local/lib64/kateai"_s,
        u"/usr/lib/kateai"_s,
        u"/usr/lib64/kateai"_s,
    };
    for (const QString &base : trustedDirs) {
        const QString src = QDir(base).filePath(u"libpage4k.so"_s);
        if (!isTrustedShimSource(src)) {
            continue;
        }
        if (filesHaveSameBytes(src, target)) {
            return target;
        }
        QDir().mkpath(dir);
        if (installShimAtomically(src, target)) {
            return target;
        }
    }

    // If no prebuilt binary is found, compile the shim that ships with the
    // plugin. Only the installation directory is used: a "src/page4k_shim.c"
    // in the working directory belongs to the project, and compiling it would
    // run code the project supplied.
    QString cc = QStandardPaths::findExecutable(u"gcc"_s);
    if (cc.isEmpty()) {
        cc = QStandardPaths::findExecutable(u"clang"_s);
    }
    if (cc.isEmpty()) {
        cc = QStandardPaths::findExecutable(u"cc"_s);
    }
    if (!cc.isEmpty()) {
        const QString srcPath = QDir(QCoreApplication::applicationDirPath()).filePath(u"page4k_shim.c"_s);
        if (isTrustedShimSource(srcPath)) {
            const QFileInfo targetInfo(target);
            const QFileInfo srcInfo(srcPath);
            if (!targetInfo.exists() || srcInfo.lastModified() > targetInfo.lastModified()) {
                QDir().mkpath(dir);
                const QString staged = target + u".tmp-"_s
                    + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
                QProcess compile;
                compile.start(cc, {u"-shared"_s, u"-fPIC"_s, u"-O2"_s, u"-fno-omit-frame-pointer"_s,
                                   u"-o"_s, staged, srcPath, u"-ldl"_s, u"-lpthread"_s});
                if (compile.waitForFinished(5000) && compile.exitCode() == 0 && QFileInfo::exists(staged)) {
                    QFile::setPermissions(staged, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                      | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                      | QFileDevice::ReadOther | QFileDevice::ExeOther);
#if defined(Q_OS_UNIX)
                    if (::rename(QFile::encodeName(staged).constData(), QFile::encodeName(target).constData()) == 0) {
                        return target;
                    }
#else
                    QFile::remove(target);
                    if (QFile::rename(staged, target)) {
                        return target;
                    }
#endif
                }
                QFile::remove(staged);
            }
        }
    }

    if (isTrustedShimSource(target)) {
        return target;
    }

    return {};
}

QString acpPageShimPath(const QString &agentId)
{
    return acpEnsurePageShim(acpBinaryInstallDir(agentId));
}

AcpBinaryInstaller::AcpBinaryInstaller(const QJsonObject &registryAgent,
                                       const QString &platformKey,
                                       QNetworkAccessManager *network,
                                       QObject *parent)
    : QObject(parent)
    , m_agent(registryAgent)
    , m_platformKey(platformKey.isEmpty() ? acpCurrentPlatformKey() : platformKey)
    , m_network(network ? network : new QNetworkAccessManager(this))
{
    const QJsonObject binary = m_agent.value(u"distribution"_s).toObject().value(u"binary"_s).toObject();
    m_platformDistribution = binary.value(m_platformKey).toObject();
    m_archiveUrl = m_platformDistribution.value(u"archive"_s).toString();
    m_command = m_platformDistribution.value(u"cmd"_s).toString();
}

AcpBinaryInstaller::~AcpBinaryInstaller()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
    }
    if (m_extractor) {
        m_extractor->kill();
        m_extractor->deleteLater();
    }
    if (m_archive) {
        m_archive->deleteLater();
    }
}

void AcpBinaryInstaller::start()
{
    if (m_platformDistribution.isEmpty()) {
        Q_EMIT failed(i18n("No binary available for this platform."));
        return;
    }
    if (m_archiveUrl.isEmpty() || m_command.isEmpty()) {
        Q_EMIT failed(i18n("No download URL available for this binary."));
        return;
    }

    m_installDir = acpBinaryInstallDir(m_agent.value(u"id"_s).toString());
    if (!QDir().mkpath(m_installDir)) {
        Q_EMIT failed(i18n("Could not create the install directory %1.", m_installDir));
        return;
    }
    // Extract into a sibling staging directory and swap it in only after the
    // executable is found. A failed update must not leave stale binaries that
    // the next launch still resolves.
    m_stagingDir = m_installDir + u".staging-"_s
        + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    if (!QDir().mkpath(m_stagingDir)) {
        Q_EMIT failed(i18n("Could not create the staging directory."));
        return;
    }

    m_archive = new QTemporaryFile(QDir::tempPath() + u"/kateai-acp-XXXXXX.archive"_s, this);
    m_archive->setAutoRemove(true);
    if (!m_archive->open()) {
        Q_EMIT failed(i18n("Could not create a temporary file for the download."));
        cleanUp();
        return;
    }

    Q_EMIT progress(i18n("Downloading %1…", m_agent.value(u"name"_s).toString()));
    QNetworkRequest request{QUrl(m_archiveUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // A stalled CDN must not leave the installer alive forever.
    request.setTransferTimeout(60000);
    m_reply = m_network->get(request);

    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_done || !m_reply || !m_archive) {
            return;
        }
        const QByteArray chunk = m_reply->readAll();
        if (m_archive->write(chunk) != chunk.size()) {
            fail(i18n("Download failed: could not write archive to disk."));
            return;
        }
        m_hash.addData(chunk);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (m_done || !m_reply) {
            return;
        }
        // A registry download is a program archive. Bound it so a broken or
        // malicious endpoint cannot fill the disk before the checksum runs.
        constexpr qint64 kMaxArchiveBytes = 1024LL * 1024 * 1024;
        if (received > kMaxArchiveBytes || total > kMaxArchiveBytes) {
            // fail() -> cleanUp() disconnects and aborts m_reply safely.
            fail(i18n("The agent archive is larger than 1 GB and was not downloaded."));
            return;
        }
        if (total > 0) {
            Q_EMIT progress(i18n("Downloading %1… %2 / %3 MB",
                               m_agent.value(u"name"_s).toString(),
                               received / (1024 * 1024), total / (1024 * 1024)));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        if (m_done || !m_reply || !m_archive) {
            return;
        }
        if (m_reply->error() != QNetworkReply::NoError) {
            fail(i18n("Download failed: %1", m_reply->errorString()));
            return;
        }
        const QByteArray tail = m_reply->readAll();
        if (!tail.isEmpty()) {
            if (m_archive->write(tail) != tail.size()) {
                fail(i18n("Download failed: could not write archive to disk."));
                return;
            }
            m_hash.addData(tail);
        }
        m_archive->flush();
        m_archive->close();

        const QString expectedHash = m_platformDistribution.value(u"sha256"_s).toString();
        const QString actualHash = QString::fromLatin1(m_hash.result().toHex());
        if (expectedHash.isEmpty()) {
            qWarning() << "KateAI: ACP registry entry has no sha256; installing"
                       << m_agent.value(u"id"_s).toString() << "without checksum verification";
        }
        if (!expectedHash.isEmpty() && actualHash.compare(expectedHash, Qt::CaseInsensitive) != 0) {
            fail(i18n("Download failed: checksum mismatch."));
            return;
        }
        extractArchive();
    });
}

void AcpBinaryInstaller::installRawBinary()
{
    QString relative = m_command;
    while (relative.startsWith(u"./"_s) || relative.startsWith(u".\\"_s)) {
        relative.remove(0, 2);
    }
    if (relative.isEmpty()) {
        relative = QUrl(m_archiveUrl).fileName();
    }
    if (relative.isEmpty()) {
        fail(i18n("The registry entry does not name the downloaded executable."));
        return;
    }
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(relative));
    if (QDir::isAbsolutePath(clean) || clean.startsWith(u"../"_s) || clean == u".."_s) {
        fail(i18n("The registry entry names an unsafe executable path."));
        return;
    }
    const QString target = QDir(m_stagingDir).filePath(clean);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        fail(i18n("Could not create the install directory."));
        return;
    }
    QFile::remove(target);
    if (!QFile::copy(m_archive->fileName(), target)) {
        fail(i18n("Could not install the downloaded agent executable."));
        return;
    }
    QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                     | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                     | QFileDevice::ReadOther | QFileDevice::ExeOther);
    finishInstall(QFileInfo(target).absoluteFilePath());
}

void AcpBinaryInstaller::extractArchive()
{
    if (!archiveIsArchive(m_archiveUrl, m_archive->fileName())) {
        // Some registry entries publish a raw executable rather than an
        // archive (muse-code, sigit). Extractors rejected those downloads.
        Q_EMIT progress(i18n("Installing %1…", m_agent.value(u"name"_s).toString()));
        installRawBinary();
        return;
    }
    Q_EMIT progress(i18n("Extracting %1…", m_agent.value(u"name"_s).toString()));
    QString badEntry;
    if (!archiveEntriesAreSafe(m_archiveUrl, m_archive->fileName(), &badEntry)) {
        fail(i18n("The archive contains an unsafe path and was not extracted: %1", badEntry));
        return;
    }
    QStringList extractorArgs;
    const QString extractor = findArchiveExtractor(m_archiveUrl, m_archive->fileName(),
                                                   m_stagingDir, &extractorArgs);
    if (extractor.isEmpty()) {
        fail(i18n("Could not extract the archive: install unzip or python3 and try again."));
        return;
    }

    m_extractor = new QProcess(this);
    auto *extractTimer = new QTimer(m_extractor);
    extractTimer->setSingleShot(true);
    extractTimer->setInterval(5 * 60 * 1000);
    connect(extractTimer, &QTimer::timeout, this, [this]() {
        if (m_done) {
            return;
        }
        fail(i18n("Could not extract the archive: extraction timed out."));
    });
    connect(m_extractor, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_done) {
            return;
        }
        fail(i18n("Could not extract the archive: the extractor failed to start."));
    });
    connect(m_extractor, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, extractTimer](int code, QProcess::ExitStatus status) {
        extractTimer->stop();
        if (m_done || !m_extractor) {
            return;
        }
        if (code != 0 || status != QProcess::NormalExit) {
            const QString output = QString::fromLocal8Bit(m_extractor->readAllStandardError()).trimmed();
            fail(i18n("Could not extract the archive: %1", output));
            return;
        }
        const QString executable = findInstalledBinary(m_stagingDir, m_command);
        if (executable.isEmpty()) {
            fail(i18n("The archive did not contain %1.", m_command));
            return;
        }
        finishInstall(executable);
    });
    m_extractor->start(extractor, extractorArgs);
    extractTimer->start();
}

void AcpBinaryInstaller::finishInstall(const QString &executable)
{
    // The archive was unpacked into a staging directory. Swap it in only now
    // that the executable was found, so a failed update cannot leave the old
    // binary in place while reporting success.
    QString relative = QDir(m_stagingDir).relativeFilePath(executable);
    if (relative.isEmpty() || relative.startsWith(u"../"_s) || QDir::isAbsolutePath(relative)) {
        fail(i18n("The extracted executable is outside the install directory."));
        return;
    }
    // Move any previous install aside before moving staging into place so a
    // failed rename restores the working binary instead of leaving nothing.
    QString backupDir;
    if (QFileInfo::exists(m_installDir)) {
        backupDir = m_installDir + u".bak-"_s + QUuid::createUuid().toString(QUuid::Id128);
        if (!QDir().rename(m_installDir, backupDir)) {
            fail(i18n("Could not replace the existing agent installation."));
            return;
        }
    }
    if (!QDir().rename(m_stagingDir, m_installDir)) {
        if (!backupDir.isEmpty()) {
            QDir().rename(backupDir, m_installDir);
        }
        fail(i18n("Could not move the installed agent into place."));
        return;
    }
    if (!backupDir.isEmpty()) {
        QDir(backupDir).removeRecursively();
    }
    m_stagingDir.clear();
    const QString installedExecutable = QDir(m_installDir).filePath(relative);

    QFile::setPermissions(installedExecutable, QFile::permissions(installedExecutable)
                                                   | QFileDevice::ExeOwner
                                                   | QFileDevice::ExeGroup
                                                   | QFileDevice::ExeOther);

    QJsonObject installed = m_agent;
    installed.insert(u"command"_s, installedExecutable);
    // The platform entry carries the launch arguments ("acp" for opencode);
    // dropping them made the binary start its TUI instead of its ACP server
    // and stall the handshake.
    QJsonArray launchArgs;
    for (const QJsonValue &arg : m_platformDistribution.value(u"args"_s).toArray()) {
        launchArgs.append(arg.toString());
    }
    installed.insert(u"args"_s, launchArgs);
    installed.insert(u"distributionType"_s, u"binary"_s);
    installed.insert(u"binaryArchiveUrl"_s, m_archiveUrl);
    installed.insert(u"binaryPlatform"_s, m_platformKey);
    installed.insert(u"binaryInstallDir"_s, m_installDir);

    if (m_agent.value(u"id"_s).toString() == u"antigravity-acp"_s
        && m_command.contains(u"agy_acp_server"_s, Qt::CaseInsensitive)
        && acpNeedsPageShim()) {
        acpEnsurePageShim(m_installDir);
    }

    m_done = true;
    cleanUp();
    Q_EMIT finished(installed);
}

void AcpBinaryInstaller::fail(const QString &message)
{
    if (m_done) {
        return;
    }
    m_done = true;
    cleanUp();
    Q_EMIT failed(message);
}

void AcpBinaryInstaller::cleanUp()
{
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        if (reply->isRunning()) {
            reply->abort();
        }
        reply->deleteLater();
    }
    if (m_extractor) {
        QProcess *extractor = m_extractor;
        m_extractor = nullptr;
        extractor->disconnect(this);
        if (extractor->state() != QProcess::NotRunning) {
            extractor->kill();
            extractor->waitForFinished(1000);
        }
        extractor->deleteLater();
    }
    if (m_archive) {
        m_archive->deleteLater();
        m_archive = nullptr;
    }
    if (!m_stagingDir.isEmpty()) {
        QDir(m_stagingDir).removeRecursively();
        m_stagingDir.clear();
    }
}

} // namespace KateAi
