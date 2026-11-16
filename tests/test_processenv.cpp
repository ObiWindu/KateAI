/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "processenv.h"

#include <QProcessEnvironment>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestProcessEnv : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void secretSuffixesAreDetected()
    {
        const QStringList secretNames = {
            u"OPENAI_API_KEY"_s,
            u"SOME_SERVICE_APIKEY"_s,
            u"AWS_ACCESS_KEY"_s,
            u"CLIENT_SECRET"_s,
            u"SIGNING_SECRET_KEY"_s,
            u"GITHUB_TOKEN"_s,
            u"DB_PASSWORD"_s,
            u"LOGIN_PASSWD"_s,
            u"DEPLOY_PRIVATE_KEY"_s,
            u"GOOGLE_APPLICATION_CREDENTIALS"_s,
            u"TOKEN"_s,
            u"PASSWORD"_s,
        };
        for (const QString &name : secretNames) {
            QVERIFY2(isSecretEnvironmentName(name), qPrintable(name));
        }
    }

    void ordinaryNamesAreNotSecret()
    {
        const QStringList ordinary = {
            u"PATH"_s,
            u"HOME"_s,
            u"LANG"_s,
            u"KDE_FULL_SESSION"_s,
            u"EDITOR"_s,
            u"PWD"_s,
            // Contains "KEY" but is not a credential.
            u"KEYBOARD_LAYOUT"_s,
        };
        for (const QString &name : ordinary) {
            QVERIFY2(!isSecretEnvironmentName(name), qPrintable(name));
        }
    }

    void emptyNameIsNotSecret()
    {
        QVERIFY(!isSecretEnvironmentName(QString()));
        QVERIFY(!isSecretEnvironmentName(u"   "_s));
    }

    void sanitizedEnvironmentDropsSecretsAndKeepsEssentials()
    {
        QProcessEnvironment base;
        base.insert(u"PATH"_s, u"/usr/bin:/bin"_s);
        base.insert(u"HOME"_s, u"/home/tester"_s);
        base.insert(u"OPENAI_API_KEY"_s, u"sk-secret"_s);
        base.insert(u"MY_SERVICE_TOKEN"_s, u"token-value"_s);
        base.insert(u"LANG"_s, u"C.UTF-8"_s);

        const QProcessEnvironment sanitized = sanitizedProcessEnvironment(base);

        QVERIFY(sanitized.contains(u"PATH"_s));
        QCOMPARE(sanitized.value(u"PATH"_s), u"/usr/bin:/bin"_s);
        QVERIFY(sanitized.contains(u"HOME"_s));
        QVERIFY(sanitized.contains(u"LANG"_s));
        QVERIFY(!sanitized.contains(u"OPENAI_API_KEY"_s));
        QVERIFY(!sanitized.contains(u"MY_SERVICE_TOKEN"_s));
    }

    void explicitlyRequestedSecretsCanBeReadded()
    {
        // The sanitizer is a starting point; a caller that deliberately needs a
        // variable (an MCP server's configured env block) adds it back.
        QProcessEnvironment base;
        base.insert(u"GITHUB_TOKEN"_s, u"ambient"_s);

        QProcessEnvironment env = sanitizedProcessEnvironment(base);
        QVERIFY(!env.contains(u"GITHUB_TOKEN"_s));
        env.insert(u"GITHUB_TOKEN"_s, u"configured"_s);
        QCOMPARE(env.value(u"GITHUB_TOKEN"_s), u"configured"_s);
    }
};

QTEST_GUILESS_MAIN(TestProcessEnv)
#include "test_processenv.moc"
