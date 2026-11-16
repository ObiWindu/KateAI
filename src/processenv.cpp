/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "processenv.h"

#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

bool isSecretEnvironmentName(const QString &name)
{
    const QString upper = name.trimmed().toUpper();
    if (upper.isEmpty()) {
        return false;
    }

    // Generic suffixes used by most providers and CI systems.
    static const QStringList suffixes = {
        u"_API_KEY"_s,
        u"_APIKEY"_s,
        u"_ACCESS_KEY"_s,
        u"_SECRET"_s,
        u"_SECRET_KEY"_s,
        u"_TOKEN"_s,
        u"_PASSWORD"_s,
        u"_PASSWD"_s,
        u"_PRIVATE_KEY"_s,
        u"_CREDENTIALS"_s,
    };
    for (const QString &suffix : suffixes) {
        if (upper.endsWith(suffix)) {
            return true;
        }
    }

    // Provider-specific names that do not follow the suffix convention.
    static const QStringList exact = {
        // Bare names some shells and tools use in addition to the suffixes.
        u"TOKEN"_s,
        u"SECRET"_s,
        u"PASSWORD"_s,
        u"PASSWD"_s,
        u"API_KEY"_s,
        u"APIKEY"_s,
        u"ACCESS_KEY"_s,
        u"PRIVATE_KEY"_s,
        u"CREDENTIALS"_s,
        u"AWS_ACCESS_KEY_ID"_s,
        u"AWS_SECRET_ACCESS_KEY"_s,
        u"AWS_SESSION_TOKEN"_s,
        u"GOOGLE_APPLICATION_CREDENTIALS"_s,
        u"GITHUB_TOKEN"_s,
        u"GH_TOKEN"_s,
        u"GITLAB_TOKEN"_s,
        u"NPM_TOKEN"_s,
        u"HF_TOKEN"_s,
        u"ANTHROPIC_API_KEY"_s,
        u"OPENAI_API_KEY"_s,
        u"DEEPSEEK_API_KEY"_s,
        u"OPENROUTER_API_KEY"_s,
        u"XAI_API_KEY"_s,
        u"GROK_CODE_XAI_API_KEY"_s,
    };
    return exact.contains(upper);
}

QProcessEnvironment sanitizedProcessEnvironment(const QProcessEnvironment &base)
{
    QProcessEnvironment out;
    const QStringList names = base.keys();
    for (const QString &name : names) {
        if (!isSecretEnvironmentName(name)) {
            out.insert(name, base.value(name));
        }
    }
    return out;
}

QProcessEnvironment sanitizedProcessEnvironment()
{
    return sanitizedProcessEnvironment(QProcessEnvironment::systemEnvironment());
}

} // namespace KateAi
