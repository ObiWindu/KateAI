/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "credentialstore.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestCredentials : public QObject
{
    Q_OBJECT

private:
    static QString credentialFile()
    {
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + u"/kateai/credentials.json"_s;
    }

    static void removeCredentialFile()
    {
        QFile::remove(credentialFile());
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + u"/kateai/credentials.conf"_s);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        removeCredentialFile();
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::Memory);
    }

    void cleanup()
    {
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::Memory);
        removeCredentialFile();
    }

    void storeAndLoadRoundTrips()
    {
        QVERIFY(CredentialStore::store(u"api-key"_s, u"secret-value"_s));
        QVERIFY(CredentialStore::contains(u"api-key"_s));
        bool ok = false;
        QCOMPARE(CredentialStore::load(u"api-key"_s, &ok), u"secret-value"_s);
        QVERIFY(ok);
    }

    void emptySecretRemovesEntry()
    {
        CredentialStore::store(u"api-key"_s, u"secret-value"_s);
        QVERIFY(CredentialStore::store(u"api-key"_s, QString()));
        QVERIFY(!CredentialStore::contains(u"api-key"_s));
    }

    void removeWorks()
    {
        CredentialStore::store(u"api-key"_s, u"secret-value"_s);
        QVERIFY(CredentialStore::remove(u"api-key"_s));
        QVERIFY(!CredentialStore::contains(u"api-key"_s));
    }

    void redactMasksStoredValue()
    {
        CredentialStore::store(u"api-key"_s, u"supersecretvalue123"_s);
        const QString text = u"token=supersecretvalue123 end"_s;
        QVERIFY(!CredentialStore::redact(text).contains(u"supersecretvalue123"_s));
    }

    void storedIdsListsEntries()
    {
        CredentialStore::store(u"a"_s, u"1"_s);
        CredentialStore::store(u"b"_s, u"2"_s);
        const QStringList ids = CredentialStore::storedIds();
        QVERIFY(ids.contains(u"a"_s));
        QVERIFY(ids.contains(u"b"_s));
    }

    void redactMasksApiKeyAssignments()
    {
        // The pattern-based redactor used to reference a capture group it did
        // not have, so an API key in a diagnostic line survived untouched.
        const QString text = u"request: x-api-key: abcdefghijklmnopqrstuvwxyz123456\n"_s;
        const QString redacted = CredentialStore::redactPatterns(text);
        QVERIFY(!redacted.contains(u"abcdefghijklmnopqrstuvwxyz123456"_s));
        QVERIFY(redacted.contains(u"x-api-key"_s));
        QVERIFY(redacted.contains(u"REDACTED"_s));
    }

    void autoOverrideRestoresDetection()
    {
        // Pinning Auto used to select the abstract "auto" backend, which every
        // operation rejected. It must mean "no pin" instead.
        CredentialStore::setBackendOverride(CredentialStore::Backend::Auto);
        QVERIFY(CredentialStore::store(u"api-key"_s, u"secret-value"_s));
        bool ok = false;
        QCOMPARE(CredentialStore::load(u"api-key"_s, &ok), u"secret-value"_s);
        QVERIFY(ok);
    }

    void configBackendWritesAnOwnerOnlyJsonFile()
    {
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::ConfigFile);
        QString error;
        QVERIFY2(CredentialStore::store(u"api-key"_s, u"config-secret-value"_s, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(credentialFile()));

#ifdef Q_OS_UNIX
        const QFileDevice::Permissions permissions = QFileInfo(credentialFile()).permissions();
        QVERIFY2(!permissions.testFlag(QFileDevice::ReadGroup), "the credential file is group-readable");
        QVERIFY2(!permissions.testFlag(QFileDevice::ReadOther), "the credential file is world-readable");
#endif

        // The JSON file must be replaced atomically; no half-written temp file
        // may be left behind in the credential directory.
        const QStringList leftovers = QDir(QFileInfo(credentialFile()).absolutePath())
                                          .entryList({u".credentials.json.*"_s}, QDir::Files);
        QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(u", "_s)));

        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::ConfigFile);
        bool ok = false;
        QCOMPARE(CredentialStore::load(u"api-key"_s, &ok), u"config-secret-value"_s);
        QVERIFY(ok);
    }

    void legacyPlaintextKeyIsMigratedAndRemoved()
    {
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::Memory);

        KConfigGroup legacy(KSharedConfig::openConfig(), u"KateAI"_s);
        legacy.writeEntry(u"GrokApiKey"_s, u"legacy-secret-value"_s);
        legacy.sync();

        const QHash<QString, QString> mapping{{u"grok-api-key"_s, u"GrokApiKey"_s}};
        QString error;
        QVERIFY2(CredentialStore::migrateFromKeys(mapping, &error), qPrintable(error));

        bool ok = false;
        QCOMPARE(CredentialStore::load(u"grok-api-key"_s, &ok), u"legacy-secret-value"_s);
        QVERIFY(ok);
        QVERIFY(legacy.readEntry(u"GrokApiKey"_s, QString()).isEmpty());
    }

    // A misconfigured backend must not report success, and an existing legacy
    // key must survive a failed migration so the next attempt can retry.
    void failedWalletStoreKeepsTheLegacyKey()
    {
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::KWallet);

        KConfigGroup legacy(KSharedConfig::openConfig(), u"KateAI"_s);
        legacy.writeEntry(u"OpenAIApiKey"_s, u"still-needed"_s);
        legacy.sync();

        const QHash<QString, QString> mapping{{u"openai-api-key"_s, u"OpenAIApiKey"_s}};
        QString error;
        // Without KWallet compiled in (or with no wallet running) this fails;
        // if the build has a working wallet it succeeds and clears the key.
        const bool migrated = CredentialStore::migrateFromKeys(mapping, &error);
        if (!migrated) {
            QCOMPARE(legacy.readEntry(u"OpenAIApiKey"_s, QString()), u"still-needed"_s);
            QVERIFY(!error.isEmpty());
        }
        CredentialStore::resetForTests();
        CredentialStore::setBackendOverride(CredentialStore::Backend::Memory);
        legacy.deleteEntry(u"OpenAIApiKey"_s);
        legacy.sync();
    }
};

QTEST_GUILESS_MAIN(TestCredentials)
#include "test_credentials.moc"