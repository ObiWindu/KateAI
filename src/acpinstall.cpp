/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpinstall.h"
#include "types.h"

#include <QCoreApplication>
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

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{
QString findArchiveExtractor(const QString &archiveUrl, const QString &archive,
                             const QString &destination, QStringList *arguments)
{
    // Some registry entries ship tar archives (Mistral Vibe, OpenCode),
    // others ZIP files (Antigravity, Kilo). Pick the matching extractor.
    const bool tarArchive = archiveUrl.contains(u".tar"_s, Qt::CaseInsensitive)
        || archiveUrl.endsWith(u".tgz"_s, Qt::CaseInsensitive);
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
        *arguments = {u"-c"_s,
                      u"import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])"_s,
                      archive, destination};
        return python;
    }
#endif
    return {};
}

QString findInstalledBinary(const QString &installDir, const QString &recordedCommand)
{
    QString relative = recordedCommand;
    while (relative.startsWith(u"./"_s)) {
        relative.remove(0, 2);
    }
    const QString candidate = QDir(installDir).filePath(relative);
    if (QFileInfo(candidate).isFile()) {
        return QFileInfo(candidate).absoluteFilePath();
    }
    // Some archives wrap their payload in a top-level directory. The registry
    // still records "./<name>", so locate the executable by name.
    const QString baseName = QFileInfo(relative).fileName();
    if (!baseName.isEmpty()) {
        QDirIterator it(installDir, {baseName}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            return it.next();
        }
    }
    return {};
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
    if (QSysInfo::currentCpuArchitecture().startsWith(u"arm"_s)
        || QSysInfo::currentCpuArchitecture().startsWith(u"aarch64"_s)) {
        return u"linux-aarch64"_s;
    }
    return u"linux-x86_64"_s;
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

QString acpEnsurePageShim(const QString &dir)
{
    if (dir.isEmpty()) {
        return {};
    }
    const QString target = QDir(dir).filePath(u"libpage4k.so"_s);
    if (QFileInfo::exists(target)) {
        return target;
    }

    // Check if a built libpage4k.so exists in known directories
    const QStringList knownPaths = {
        QCoreApplication::applicationDirPath() + u"/libpage4k.so"_s,
        QCoreApplication::applicationDirPath() + u"/../lib/libpage4k.so"_s,
        QCoreApplication::applicationDirPath() + u"/../lib64/libpage4k.so"_s,
        QDir::current().filePath(u"build/bin/libpage4k.so"_s),
        QDir::current().filePath(u"src/page4k_shim.so"_s),
        u"/usr/lib64/kateai/libpage4k.so"_s,
        u"/usr/lib/kateai/libpage4k.so"_s,
        u"/tmp/libpage4k.so"_s,
    };
    for (const QString &src : knownPaths) {
        if (QFileInfo::exists(src)) {
            QDir().mkpath(dir);
            if (QFile::copy(src, target)) {
                QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                | QFileDevice::ReadOther | QFileDevice::ExeOther);
                return target;
            }
        }
    }

    // If no prebuilt binary is found, try compiling page4k_shim.c if gcc/cc is available
    const QString gcc = QStandardPaths::findExecutable(u"gcc"_s);
    const QString cc = gcc.isEmpty() ? QStandardPaths::findExecutable(u"cc"_s) : gcc;
    if (!cc.isEmpty()) {
        const QString srcPath = QDir::current().filePath(u"src/page4k_shim.c"_s);
        if (QFileInfo::exists(srcPath)) {
            QDir().mkpath(dir);
            QProcess compile;
            compile.start(cc, {u"-shared"_s, u"-fPIC"_s, u"-O2"_s, u"-o"_s, target, srcPath, u"-ldl"_s, u"-lpthread"_s});
            if (compile.waitForFinished(5000) && compile.exitCode() == 0 && QFileInfo::exists(target)) {
                QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                | QFileDevice::ReadOther | QFileDevice::ExeOther);
                return target;
            }
        }
    }

    return {};
}

QString acpPageShimPath(const QString &agentId)
{
    const QString installDir = acpBinaryInstallDir(agentId);
    const QString target = QDir(installDir).filePath(u"libpage4k.so"_s);
    if (QFileInfo::exists(target)) {
        return target;
    }
    return acpEnsurePageShim(installDir);
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
    m_reply = m_network->get(request);

    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        const QByteArray chunk = m_reply->readAll();
        m_archive->write(chunk);
        m_hash.addData(chunk);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            Q_EMIT progress(i18n("Downloading %1… %2 / %3 MB",
                               m_agent.value(u"name"_s).toString(),
                               received / (1024 * 1024), total / (1024 * 1024)));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        if (m_reply->error() != QNetworkReply::NoError) {
            fail(i18n("Download failed: %1", m_reply->errorString()));
            return;
        }
        const QByteArray tail = m_reply->readAll();
        if (!tail.isEmpty()) {
            m_archive->write(tail);
            m_hash.addData(tail);
        }
        m_archive->flush();
        m_archive->close();

        const QString expectedHash = m_platformDistribution.value(u"sha256"_s).toString();
        const QString actualHash = QString::fromLatin1(m_hash.result().toHex());
        if (!expectedHash.isEmpty() && actualHash.compare(expectedHash, Qt::CaseInsensitive) != 0) {
            fail(i18n("Download failed: checksum mismatch."));
            return;
        }
        extractArchive();
    });
}

void AcpBinaryInstaller::extractArchive()
{
    Q_EMIT progress(i18n("Extracting %1…", m_agent.value(u"name"_s).toString()));
    QStringList extractorArgs;
    const QString extractor = findArchiveExtractor(m_archiveUrl, m_archive->fileName(),
                                                   m_installDir, &extractorArgs);
    if (extractor.isEmpty()) {
        fail(i18n("Could not extract the archive: install unzip or python3 and try again."));
        return;
    }

    m_extractor = new QProcess(this);
    connect(m_extractor, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        fail(i18n("Could not extract the archive: the extractor failed to start."));
    });
    connect(m_extractor, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        if (m_done) {
            return;
        }
        if (code != 0 || status != QProcess::NormalExit) {
            const QString output = QString::fromLocal8Bit(m_extractor->readAllStandardError()).trimmed();
            fail(i18n("Could not extract the archive: %1", output));
            return;
        }
        const QString executable = findInstalledBinary(m_installDir, m_command);
        if (executable.isEmpty()) {
            fail(i18n("The archive did not contain %1.", m_command));
            return;
        }
        finishInstall(executable);
    });
    m_extractor->start(extractor, extractorArgs);
}

void AcpBinaryInstaller::finishInstall(const QString &executable)
{
    QFile::setPermissions(executable, QFile::permissions(executable)
                                         | QFileDevice::ExeOwner
                                         | QFileDevice::ExeGroup
                                         | QFileDevice::ExeOther);

    QJsonObject installed = m_agent;
    installed.insert(u"command"_s, executable);
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

    if (m_agent.value(u"id"_s).toString() == u"antigravity-acp"_s && acpNeedsPageShim()) {
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
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_extractor) {
        m_extractor->deleteLater();
        m_extractor = nullptr;
    }
    if (m_archive) {
        m_archive->deleteLater();
        m_archive = nullptr;
    }
}

} // namespace KateAi
