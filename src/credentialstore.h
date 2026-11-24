/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace KateAi
{

// Secure storage for provider credentials.
//
// Every backend that can persist a credential is funnelled through this class,
// so the rest of the plugin never writes an API key into its configuration file
// directly. Backends, strongest first:
//
//   KWallet     - the KDE Wallet service (KF6::Wallet). Available when the
//                 framework was found at build time and a wallet can be opened.
//   ConfigFile  - a dedicated atomic JSON file (credentials.json) with
//                 owner-only permissions. This is a documented fallback for
//                 systems without a working wallet; it is not presented as
//                 cryptographically secure and never lives in the main config.
//   Memory      - process-local, used by tests and by --version style probes.
//
// Auto resolves the backend on each operation and can be pinned for tests with
// KATEAI_CREDENTIAL_BACKEND=memory|kwallet|config or setBackendOverride().
class CredentialStore
{
public:
    enum class Backend {
        Auto,
        KWallet,
        ConfigFile,
        Memory,
    };

    struct Status {
        Backend backend = Backend::ConfigFile;
        // Human-readable name, suitable for a diagnostic report.
        QString name;
        // True only when the credential at rest is protected by the desktop
        // credential service rather than by filesystem permissions alone.
        bool secure = false;
    };

    // Pin/clear the backend for this process. Tests use this so they never touch
    // the developer's real wallet or configuration.
    static void setBackendOverride(Backend backend);
    static void clearBackendOverride();

    // Resolves the backend that operations will actually use. In Auto mode this
    // may open the wallet; a failed open falls back to the config file and
    // reports that here.
    static Status status();
    static bool walletAvailable();
    static bool isSecureBackendActive();

    // Empty `secret` removes the entry, which is how the config page clears a
    // key. Returns false (and fills `error`) when the backend refused the write;
    // the caller must then keep the previous value instead of pretending the
    // credential is stored securely.
    static bool store(const QString &id, const QString &secret, QString *error = nullptr);
    static QString load(const QString &id, bool *ok = nullptr);
    static bool contains(const QString &id);
    static bool remove(const QString &id);
    static QStringList storedIds();

    // Moves the pre-0.4.0 plaintext keys out of the main KConfig group. The
    // legacy entry is deleted only after its value has been stored successfully,
    // so a failing backend never destroys the user's existing credentials.
    // `idsToConfigKeys` maps credential id -> legacy KConfig entry name.
    static bool migrateFromKeys(const QHash<QString, QString> &idsToConfigKeys, QString *error = nullptr);
    // The concrete mapping for the keys SettingsStore has always written.
    static bool migrateLegacyKateConfig(QString *error = nullptr);

    // Log hygiene: mask values that look like credentials and any value that
    // happens to be stored. Diagnostics and bug reports should run their text
    // through redact() before leaving the process.
    static QString redact(const QString &text);
    static QString redactPatterns(const QString &text);

    // Drops the cached wallet handle and the memory backend contents. Test
    // support; a real session never calls it.
    static void resetForTests();
};

} // namespace KateAi
