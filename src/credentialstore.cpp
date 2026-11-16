/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "credentialstore.h"

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#if KATEAI_HAVE_KWALLET
#include <KF6/KWallet/kwallet.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{
namespace
{
using Backend = KateAi::CredentialStore::Backend;

QMutex &globalMutex()
{
    static QMutex mutex;
    return mutex;
}

// Process-wide backend pin (tests).
Backend g_override = Backend::Auto;
bool g_hasOverride = false;

// Memory backend, used by tests and by callers that explicitly ask for it.
QHash<QString, QString> g_memoryStore;

#if KATEAI_HAVE_KWALLET
KWallet::Wallet *g_wallet = nullptr;
bool g_walletProbed = false;

KWallet::Wallet *openWalletHandle()
{
    if (g_wallet && !g_wallet->isOpen()) {
        delete g_wallet;
        g_wallet = nullptr;
        g_walletProbed = true; // a closed wallet must not be retried forever
    }
    if (!g_wallet) {
        g_wallet = KWallet::Wallet::openWallet(KWallet::Wallet::LocalWallet(), 0, KWallet::Wallet::Synchronous);
        g_walletProbed = true;
        if (g_wallet) {
            if (!g_wallet->hasFolder(u"KateAI"_s)) {
                g_wallet->createFolder(u"KateAI"_s);
            }
            if (!g_wallet->setFolder(u"KateAI"_s)) {
                delete g_wallet;
                g_wallet = nullptr;
            }
        }
    }
    return g_wallet;
}
#endif

QString environmentOverrideName()
{
    const QByteArray raw = qgetenv("KATEAI_CREDENTIAL_BACKEND");
    if (raw.isEmpty()) {
        return {};
    }
    return QString::fromLocal8Bit(raw).trimmed().toLower();
}

bool isTestMode()
{
    if (qEnvironmentVariableIsSet("KATEAI_FORCE_CONFIG_CREDENTIALS")) {
        return true;
    }
    const QString loc = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    // QStandardPaths::setTestModeEnabled() redirects data to a temp dir; use
    // that as a signal so tests never open the developer's real wallet.
    return loc.contains(u"/tmp"_s) || loc.contains(u"\\Temp"_s, Qt::CaseInsensitive)
        || loc.contains(u"QtTest"_s, Qt::CaseInsensitive);
}

// Picks the backend an operation should use. `usedFallback` tells the caller
// that Auto could not reach the wallet and degraded to the config file, which
// is never silent: status() reports it and callers can surface it.
Backend resolveBackend(bool *usedFallback)
{
    if (usedFallback) {
        *usedFallback = false;
    }

    if (g_hasOverride) {
        return g_override;
    }
    const QString env = environmentOverrideName();
    if (env == u"memory"_s) {
        return Backend::Memory;
    }
    if (env == u"kwallet"_s) {
        return Backend::KWallet;
    }
    if (env == u"config"_s) {
        return Backend::ConfigFile;
    }

    // A test process must never open the developer's real wallet.
    if (isTestMode()) {
        return Backend::ConfigFile;
    }

#if KATEAI_HAVE_KWALLET
    if (openWalletHandle() != nullptr) {
        return Backend::KWallet;
    }
    if (usedFallback) {
        *usedFallback = true;
    }
#else
    Q_UNUSED(isTestMode);
#endif
    return Backend::ConfigFile;
}

QString backendName(Backend backend)
{
    switch (backend) {
    case Backend::Auto:
        return u"auto"_s;
    case Backend::KWallet:
        return u"KDE Wallet"_s;
    case Backend::ConfigFile:
        return u"configuration file (owner-only permissions)"_s;
    case Backend::Memory:
        return u"in-memory (test only)"_s;
    }
    return u"unknown"_s;
}

QString credentialDirectory()
{
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dataDir.isEmpty()) {
        dataDir = QDir::home().filePath(u".local/share"_s);
    }
    return dataDir + u"/kateai"_s;
}

// The fallback file is JSON, written with QSaveFile, so a crash mid-write can
// never truncate every stored credential. The pre-0.4.0 implementation used a
// KConfig file with a looser write path (and relied on a post-write chmod);
// that file is still read for migration but is never written again.
QString credentialsFilePath()
{
    return credentialDirectory() + u"/credentials.json"_s;
}

QString legacyCredentialsFilePath()
{
    return credentialDirectory() + u"/credentials.conf"_s;
}

void tightenFilePermissions(const QString &path, bool directory)
{
    const auto ownerOnly = directory
        ? (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)
        : (QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile file(path);
    if (file.exists()) {
        file.setPermissions(ownerOnly);
    }
}

void ensureCredentialDirectory()
{
    const QString dir = credentialDirectory();
    QDir().mkpath(dir);
    tightenFilePermissions(dir, true);
}

QJsonObject readCredentialObject()
{
    QFile file(credentialsFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        qWarning() << "KateAI: the credential file is not valid JSON; ignoring it";
        return {};
    }
    return document.object();
}

bool writeCredentialObject(const QJsonObject &object, QString *error)
{
    ensureCredentialDirectory();
    QSaveFile file(credentialsFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = u"The credential file could not be opened for writing."_s;
        }
        return false;
    }
    // Set the permissions on the temporary file before it is renamed into
    // place. Tightening only after commit leaves a window (and a crash window)
    // where the secrets are world-readable under a permissive umask.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = u"The credential file could not be written: %1"_s.arg(file.errorString());
        }
        return false;
    }
    tightenFilePermissions(credentialsFilePath(), false);
    return true;
}

// Pre-0.4.0 fallback: a KConfig file. Only read when it already exists, so a
// missing file does not resurrect the old insecure write path.
bool legacyCredentialGroupExists()
{
    return QFileInfo::exists(legacyCredentialsFilePath());
}

KConfigGroup legacyCredentialsGroup()
{
    return KConfigGroup(KSharedConfig::openConfig(legacyCredentialsFilePath(), KConfig::SimpleConfig),
                        u"Credentials"_s);
}

void removeLegacyCredential(const QString &id)
{
    if (!legacyCredentialGroupExists() || id.isEmpty()) {
        return;
    }
    auto config = KSharedConfig::openConfig(legacyCredentialsFilePath(), KConfig::SimpleConfig);
    KConfigGroup group(config, u"Credentials"_s);
    if (group.hasKey(id)) {
        group.deleteEntry(id);
        config->sync();
    }
}

bool storeInMemory(const QString &id, const QString &secret)
{
    QMutexLocker locker(&globalMutex());
    if (secret.isEmpty()) {
        g_memoryStore.remove(id);
    } else {
        g_memoryStore.insert(id, secret);
    }
    return true;
}

QString loadFromMemory(const QString &id, bool *ok)
{
    QMutexLocker locker(&globalMutex());
    const auto it = g_memoryStore.constFind(id);
    if (it == g_memoryStore.constEnd()) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return it.value();
}

bool removeFromMemory(const QString &id)
{
    QMutexLocker locker(&globalMutex());
    return g_memoryStore.remove(id) > 0;
}

QStringList idsFromMemory()
{
    QMutexLocker locker(&globalMutex());
    return g_memoryStore.keys();
}

bool storeInConfig(const QString &id, const QString &secret, QString *error)
{
    QJsonObject object = readCredentialObject();
    if (secret.isEmpty()) {
        const bool inJson = object.contains(id);
        const bool inLegacy = legacyCredentialGroupExists() && legacyCredentialsGroup().hasKey(id);
        if (!inJson && !inLegacy) {
            // Removing something that is not stored must not create an empty
            // credentials file (settings save writes every key, including the
            // unset ones).
            return true;
        }
        object.remove(id);
    } else {
        // Avoid rewriting the file for the common case where settings are
        // saved without changing any key.
        const bool unchanged = object.value(id).toString() == secret;
        const bool needsLegacyCleanup = legacyCredentialGroupExists()
            && legacyCredentialsGroup().hasKey(id);
        if (unchanged && !needsLegacyCleanup) {
            return true;
        }
        object.insert(id, secret);
    }
    if (!writeCredentialObject(object, error)) {
        return false;
    }
    // A successfully stored value must not remain in the legacy plaintext
    // file either. Removal is best effort: the JSON file now holds the
    // current value, and a stale legacy copy is still protected by the same
    // owner-only permission as before.
    removeLegacyCredential(id);
    return true;
}

QString loadFromConfig(const QString &id, bool *ok)
{
    const QJsonObject object = readCredentialObject();
    const auto it = object.constFind(id);
    if (it != object.constEnd()) {
        if (ok) {
            *ok = true;
        }
        return it.value().toString();
    }
    if (legacyCredentialGroupExists()) {
        const KConfigGroup group = legacyCredentialsGroup();
        if (group.hasKey(id)) {
            if (ok) {
                *ok = true;
            }
            return group.readEntry(id, QString());
        }
    }
    if (ok) {
        *ok = false;
    }
    return {};
}

bool removeFromConfig(const QString &id)
{
    QJsonObject object = readCredentialObject();
    object.remove(id);
    if (!writeCredentialObject(object, nullptr)) {
        return false;
    }
    removeLegacyCredential(id);
    return true;
}

QStringList idsFromConfig()
{
    QStringList ids = readCredentialObject().keys();
    if (legacyCredentialGroupExists()) {
        for (const QString &key : legacyCredentialsGroup().keyList()) {
            if (!ids.contains(key)) {
                ids.append(key);
            }
        }
    }
    return ids;
}

#if KATEAI_HAVE_KWALLET
bool storeInWallet(const QString &id, const QString &secret, QString *error)
{
    QMutexLocker locker(&globalMutex());
    KWallet::Wallet *wallet = openWalletHandle();
    if (!wallet) {
        if (error) {
            *error = u"The KDE Wallet is not available. Credentials were not stored."_s;
        }
        return false;
    }
    if (secret.isEmpty() && !wallet->hasEntry(id)) {
        // Idempotent removal: settings save clears every unset provider key,
        // and an absent entry is already the desired state.
        return true;
    }
    const int result = secret.isEmpty() ? wallet->removeEntry(id) : wallet->writePassword(id, secret);
    if (result != 0) {
        if (error) {
            *error = u"The KDE Wallet refused to store the credential (error %1)."_s.arg(result);
        }
        return false;
    }
    return true;
}

QString loadFromWallet(const QString &id, bool *ok)
{
    QMutexLocker locker(&globalMutex());
    KWallet::Wallet *wallet = openWalletHandle();
    if (!wallet) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    QString value;
    const int result = wallet->readPassword(id, value);
    if (result != 0) {
        if (ok) {
            *ok = false;
        }
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return value;
}

bool removeFromWallet(const QString &id)
{
    QMutexLocker locker(&globalMutex());
    KWallet::Wallet *wallet = openWalletHandle();
    return wallet && wallet->removeEntry(id) == 0;
}

QStringList idsFromWallet()
{
    QMutexLocker locker(&globalMutex());
    KWallet::Wallet *wallet = openWalletHandle();
    if (!wallet) {
        return {};
    }
    bool ok = false;
    const auto entries = wallet->entriesList(&ok);
    return ok ? entries.keys() : QStringList();
}
#endif

} // namespace

void CredentialStore::setBackendOverride(Backend backend)
{
    QMutexLocker locker(&globalMutex());
    if (backend == Backend::Auto) {
        // "Auto" is not a concrete backend; pinning it used to make every
        // operation fall through to the unreachable branch and fail. Clearing
        // the override restores the normal detection order.
        g_hasOverride = false;
        g_override = Backend::Auto;
        return;
    }
    g_override = backend;
    g_hasOverride = true;
}

void CredentialStore::clearBackendOverride()
{
    QMutexLocker locker(&globalMutex());
    g_hasOverride = false;
    g_override = Backend::Auto;
}

CredentialStore::Status CredentialStore::status()
{
    bool usedFallback = false;
    const Backend backend = resolveBackend(&usedFallback);
    Status s;
    s.backend = backend;
    s.secure = (backend == Backend::KWallet);
    s.name = backendName(backend);
    if (usedFallback) {
        s.name += u" (KDE Wallet unavailable, falling back)"_s;
    }
    return s;
}

bool CredentialStore::walletAvailable()
{
#if KATEAI_HAVE_KWALLET
    if (isTestMode()) {
        return false;
    }
    QMutexLocker locker(&globalMutex());
    return openWalletHandle() != nullptr;
#else
    return false;
#endif
}

bool CredentialStore::isSecureBackendActive()
{
    return status().secure;
}

bool CredentialStore::store(const QString &id, const QString &secret, QString *error)
{
    if (id.isEmpty()) {
        if (error) {
            *error = u"A credential needs a non-empty id."_s;
        }
        return false;
    }

    const Backend backend = resolveBackend(nullptr);
    switch (backend) {
    case Backend::Memory:
        return storeInMemory(id, secret);
    case Backend::ConfigFile:
        return storeInConfig(id, secret, error);
#if KATEAI_HAVE_KWALLET
    case Backend::KWallet:
        return storeInWallet(id, secret, error);
#endif
    case Backend::Auto:
        break;
    }

    // Unreachable: resolveBackend never returns Auto.
    if (error) {
        *error = u"No credential backend is configured."_s;
    }
    return false;
}

QString CredentialStore::load(const QString &id, bool *ok)
{
    if (id.isEmpty()) {
        if (ok) {
            *ok = false;
        }
        return {};
    }

    const Backend backend = resolveBackend(nullptr);
    switch (backend) {
    case Backend::Memory:
        return loadFromMemory(id, ok);
    case Backend::ConfigFile:
        return loadFromConfig(id, ok);
#if KATEAI_HAVE_KWALLET
    case Backend::KWallet:
        return loadFromWallet(id, ok);
#endif
    case Backend::Auto:
        break;
    }
    if (ok) {
        *ok = false;
    }
    return {};
}

bool CredentialStore::contains(const QString &id)
{
    if (id.isEmpty()) {
        return false;
    }
    const Backend backend = resolveBackend(nullptr);
    switch (backend) {
    case Backend::Memory:
        return idsFromMemory().contains(id);
    case Backend::ConfigFile: {
        const QString value = loadFromConfig(id, nullptr);
        return !value.isEmpty() || idsFromConfig().contains(id);
    }
#if KATEAI_HAVE_KWALLET
    case Backend::KWallet: {
        QMutexLocker locker(&globalMutex());
        KWallet::Wallet *wallet = openWalletHandle();
        return wallet && wallet->hasEntry(id);
    }
#endif
    case Backend::Auto:
        break;
    }
    return false;
}

bool CredentialStore::remove(const QString &id)
{
    if (id.isEmpty()) {
        return false;
    }
    const Backend backend = resolveBackend(nullptr);
    switch (backend) {
    case Backend::Memory:
        return removeFromMemory(id);
    case Backend::ConfigFile:
        return removeFromConfig(id);
#if KATEAI_HAVE_KWALLET
    case Backend::KWallet:
        return removeFromWallet(id);
#endif
    case Backend::Auto:
        break;
    }
    return false;
}

QStringList CredentialStore::storedIds()
{
    const Backend backend = resolveBackend(nullptr);
    switch (backend) {
    case Backend::Memory:
        return idsFromMemory();
    case Backend::ConfigFile:
        return idsFromConfig();
#if KATEAI_HAVE_KWALLET
    case Backend::KWallet:
        return idsFromWallet();
#endif
    case Backend::Auto:
        break;
    }
    return {};
}

bool CredentialStore::migrateFromKeys(const QHash<QString, QString> &idsToConfigKeys, QString *error)
{
    if (idsToConfigKeys.isEmpty()) {
        return true;
    }

    auto config = KSharedConfig::openConfig();
    KConfigGroup legacy(config, u"KateAI"_s);

    // Most sessions have no legacy keys at all. Check before touching the
    // file so load() does not rewrite kateairc on every settings reload.
    bool hasLegacyValue = false;
    for (auto it = idsToConfigKeys.constBegin(); it != idsToConfigKeys.constEnd(); ++it) {
        if (!legacy.readEntry(it.value(), QString()).trimmed().isEmpty()) {
            hasLegacyValue = true;
            break;
        }
    }
    if (!hasLegacyValue) {
        return true;
    }

    bool allOk = true;
    bool changed = false;
    QString lastError;

    for (auto it = idsToConfigKeys.constBegin(); it != idsToConfigKeys.constEnd(); ++it) {
        const QString &id = it.key();
        const QString &legacyKey = it.value();

        const QString value = legacy.readEntry(legacyKey, QString());
        if (value.trimmed().isEmpty()) {
            continue; // nothing to migrate; a cleared key stays cleared
        }

        // Already migrated on an earlier run: do not touch the live entry.
        bool loadedOk = false;
        const QString existing = load(id, &loadedOk);
        if (loadedOk && !existing.isEmpty()) {
            // The secure copy is authoritative; drop the plaintext duplicate
            // so it cannot reappear if the secure store is later cleared.
            legacy.deleteEntry(legacyKey);
            changed = true;
            continue;
        }

        QString storeError;
        if (!store(id, value, &storeError)) {
            allOk = false;
            lastError = storeError;
            qWarning() << "KateAI: could not move credential" << id << "into secure storage:" << storeError;
            continue;
        }

        // Only a confirmed store clears the plaintext copy. A failed store
        // leaves the legacy entry exactly as it was, so migration can retry.
        legacy.deleteEntry(legacyKey);
        changed = true;
    }

    if (changed) {
        config->sync();
    }

    if (!allOk && error) {
        *error = lastError;
    }
    return allOk;
}

bool CredentialStore::migrateLegacyKateConfig(QString *error)
{
    static const QHash<QString, QString> mapping = {
        {u"grok-api-key"_s, u"GrokApiKey"_s},
        {u"openai-api-key"_s, u"OpenAIApiKey"_s},
        {u"openrouter-api-key"_s, u"OpenRouterApiKey"_s},
        {u"deepseek-api-key"_s, u"DeepSeekApiKey"_s},
        {u"openai-compatible-api-key"_s, u"OpenAICompatibleApiKey"_s},
        {u"claude-compatible-api-key"_s, u"ClaudeCompatibleApiKey"_s},
        {u"kilo-api-key"_s, u"KiloApiKey"_s},
        {u"acp-api-key"_s, u"AcpApiKey"_s},
        {u"opencode-api-key"_s, u"OpenCodeApiKey"_s},
        {u"web-search-api-key"_s, u"WebSearchApiKey"_s},
    };
    return migrateFromKeys(mapping, error);
}

QString CredentialStore::redactPatterns(const QString &text)
{
    if (text.isEmpty()) {
        return text;
    }

    struct Pattern {
        QRegularExpression re;
        QString replacement;
    };

    static const QList<Pattern> patterns = {
        // OpenAI / Anthropic style keys, GitHub tokens, generic sk- secrets.
        {QRegularExpression(uR"(\b(?:sk|pk|rk)-[A-Za-z0-9_\-]{12,})"_s), u"[REDACTED-KEY]"_s},
        {QRegularExpression(uR"(\bgh[pousr]_[A-Za-z0-9]{20,})"_s), u"[REDACTED-TOKEN]"_s},
        {QRegularExpression(uR"(Bearer\s+[A-Za-z0-9._~+/=\-]{12,})"_s, QRegularExpression::CaseInsensitiveOption),
         u"Bearer [REDACTED]"_s},
        // "api_key": "..." / api-key=... / x-api-key: ...
        // The whole key/value assignment is matched; only the value becomes
        // the redaction marker. (The previous expression had no capture group,
        // so the replacement referenced a group that did not exist.)
        {QRegularExpression(uR"(((?:api[_-]?key|api[_-]?token|access[_-]?token|client[_-]?secret)["']?\s*[:=]\s*["']?)[A-Za-z0-9._~+/=\-]{12,})"_s,
                            QRegularExpression::CaseInsensitiveOption),
         u"\\1[REDACTED]"_s},
    };

    QString out = text;
    for (const Pattern &pattern : patterns) {
        out.replace(pattern.re, pattern.replacement);
    }
    return out;
}

QString CredentialStore::redact(const QString &text)
{
    QString out = redactPatterns(text);

    // Mask any stored credential value that appears verbatim, whatever shape
    // it has. The set is small (a handful of provider keys) and this only runs
    // on text that is about to leave the process.
    const QStringList ids = storedIds();
    for (const QString &id : ids) {
        bool ok = false;
        const QString value = load(id, &ok);
        if (ok && value.size() >= 8) {
            out.replace(value, u"[REDACTED]"_s);
        }
    }
    return out;
}

void CredentialStore::resetForTests()
{
    QMutexLocker locker(&globalMutex());
    g_memoryStore.clear();
    g_hasOverride = false;
    g_override = Backend::Auto;
#if KATEAI_HAVE_KWALLET
    if (g_wallet) {
        delete g_wallet;
        g_wallet = nullptr;
    }
    g_walletProbed = false;
#endif
}

} // namespace KateAi
