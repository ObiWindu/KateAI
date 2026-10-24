/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QCryptographicHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryFile;

namespace KateAi
{

// Platform key used by the ACP registry binary distributions.
QString acpCurrentPlatformKey();

// True when the system runtime page size exceeds 4KB (e.g. 16KB on aarch64 Linux).
bool acpNeedsPageShim();

// Resolves or ensures the 4KB page size emulation shim library path for binary
// agents that require a 4KB page size (such as Google Antigravity's agy_acp_server).
QString acpPageShimPath(const QString &agentId);

// Ensures libpage4k.so is present in the specified directory, copying or compiling it
// from source if needed.
QString acpEnsurePageShim(const QString &dir);

// Downloads and unpacks a binary-distributed ACP registry agent. The archive
// is installed below acpBinaryInstallDir(id), and the finished signal carries
// the registry entry updated with an absolute command and launch arguments.
class AcpBinaryInstaller : public QObject
{
    Q_OBJECT

public:
    explicit AcpBinaryInstaller(const QJsonObject &registryAgent,
                                const QString &platformKey = QString(),
                                QNetworkAccessManager *network = nullptr,
                                QObject *parent = nullptr);
    ~AcpBinaryInstaller() override;

    void start();
    QString installDir() const { return m_installDir; }

Q_SIGNALS:
    void progress(const QString &message);
    void finished(const QJsonObject &installedAgent);
    void failed(const QString &message);

private:
    void fail(const QString &message);
    void cleanUp();
    void extractArchive();
    void finishInstall(const QString &executable);

    QJsonObject m_agent;
    QJsonObject m_platformDistribution;
    QString m_platformKey;
    QString m_archiveUrl;
    QString m_command;
    QString m_installDir;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTemporaryFile *m_archive = nullptr;
    QProcess *m_extractor = nullptr;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    bool m_done = false;
};

} // namespace KateAi
