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
        // Database and session credentials that do not use the suffixes.
        u"PGPASSWORD"_s,
        u"MYSQL_PWD"_s,
        u"REDISCLI_AUTH"_s,
        u"SSH_AUTH_SOCK"_s,
        u"KRB5CCNAME"_s,
        u"DBUS_SESSION_BUS_ADDRESS"_s,
        u"DISPLAY"_s,
        u"WAYLAND_DISPLAY"_s,
        u"XAUTHORITY"_s,
        // Programs that execute other programs or change code loading.
        u"BASH_ENV"_s,
        u"ENV"_s,
        u"GIT_ASKPASS"_s,
        u"GIT_EXTERNAL_DIFF"_s,
        u"GIT_SSH_COMMAND"_s,
        u"LD_PRELOAD"_s,
        u"LD_LIBRARY_PATH"_s,
        u"DYLD_LIBRARY_PATH"_s,
        u"DYLD_INSERT_LIBRARIES"_s,
        u"NODE_OPTIONS"_s,
        u"PYTHONSTARTUP"_s,
        u"PERL5OPT"_s,
        u"RUBYOPT"_s,
        u"JAVA_TOOL_OPTIONS"_s,
        u"PAGER"_s,
        u"LESSOPEN"_s,
        u"MANPAGER"_s,
    };
    if (exact.contains(upper)) {
        return true;
    }
    // Shell functions are exported as BASH_FUNC_name%%.
    if (upper.startsWith(u"BASH_FUNC_"_s)) {
        return true;
    }
    return false;
}

// A URL that carries a password in its userinfo part:
// "postgres://user:secret@host/db". The name says nothing about it -
// "DATABASE_URL" is not a secret by name - but the value is a password, and a
// sandboxed command would inherit it.
static bool valueCarriesCredentials(const QString &value)
{
    const int schemeEnd = value.indexOf(u"://"_s);
    if (schemeEnd < 0) {
        // Scheme-less DSNs ("user:password@host/db") are accepted by several
        // client libraries. A colon before the first '@' is the password.
        const int at = value.indexOf(u'@');
        return at > 0 && value.left(at).contains(u':') && !value.left(at).contains(u' ');
    }
    const int authorityStart = schemeEnd + 3;
    // Look at the whole userinfo up to the first '@', not just up to the
    // first '/': a password containing an unescaped slash would otherwise be
    // cut off before the '@' and the whole value would be treated as non-secret.
    const int at = value.indexOf(u'@', authorityStart);
    if (at < 0) {
        return false;
    }
    const QString userinfo = value.mid(authorityStart, at - authorityStart);
    // A userinfo without a colon is a user name, not a secret.
    return userinfo.contains(u':');
}

QProcessEnvironment sanitizedProcessEnvironment(const QProcessEnvironment &base)
{
    QProcessEnvironment out;
    const QStringList names = base.keys();
    for (const QString &name : names) {
        const QString value = base.value(name);
        if (isSecretEnvironmentName(name) || valueCarriesCredentials(value)) {
            continue;
        }
        out.insert(name, value);
    }
    return out;
}

QProcessEnvironment sanitizedProcessEnvironment()
{
    return sanitizedProcessEnvironment(QProcessEnvironment::systemEnvironment());
}

} // namespace KateAi
