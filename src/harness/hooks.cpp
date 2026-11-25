/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hooks.h"

#include "sandbox.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool matchesEventMatcher(const QString &matcher, const QJsonObject &payload)
{
    if (matcher.trimmed().isEmpty()) {
        return true;
    }
    QString subject = payload.value(u"tool"_s).toObject().value(u"name"_s).toString();
    if (subject.isEmpty()) {
        subject = payload.value(u"reason"_s).toString();
    }
    if (subject.isEmpty()) {
        subject = payload.value(u"event"_s).toString();
    }
    QRegularExpression regex(matcher);
    if (regex.isValid() && regex.match(subject).hasMatch()) {
        return true;
    }
    QString wildcard = QRegularExpression::escape(matcher);
    wildcard.replace(u"\\*"_s, u".*"_s);
    return QRegularExpression(u"^%1$"_s.arg(wildcard),
                              QRegularExpression::CaseInsensitiveOption)
        .match(subject)
        .hasMatch();
}

// The parameter is not named "stdout": MSVC's stdio.h defines stdout as a
// macro, which turns the declaration into a syntax error.
QJsonObject parseHookOutput(const QByteArray &output)
{
    const QByteArray trimmed = output.trimmed();
    if (!trimmed.startsWith('{')) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(trimmed);
    return document.isObject() ? document.object() : QJsonObject();
}

QString capOutput(QString output)
{
    constexpr int maxChars = 8192;
    if (output.size() <= maxChars) {
        return output;
    }
    return output.left(maxChars / 2) + u"\n… (truncated) …\n"_s + output.right(maxChars / 2);
}

} // namespace

QStringList HookRegistry::knownEvents()
{
    return {
        u"SessionStart"_s,
        u"UserPromptSubmit"_s,
        u"PreToolUse"_s,
        u"PostToolUse"_s,
        u"PostToolUseFailure"_s,
        u"Stop"_s,
        u"PreCompact"_s,
        u"SubagentStart"_s,
        u"SubagentStop"_s,
        u"SessionEnd"_s,
    };
}

QString HookRegistry::userHooksPath()
{
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (config.isEmpty()) {
        config = QDir::home().filePath(u".config"_s);
    }
    return config + u"/kateai/hooks.json"_s;
}

QString HookRegistry::managedHooksPath()
{
    return u"/etc/kateai/hooks.json"_s;
}

void HookRegistry::clear()
{
    m_hooks.clear();
    m_workspace.clear();
    m_projectTrusted = false;
}

QString HookRegistry::trustKeyFor(const QString &workspace)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(QDir::cleanPath(workspace).toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(32));
}

QString HookRegistry::trustKey() const
{
    return trustKeyFor(m_workspace);
}

void HookRegistry::setProjectTrusted(bool trusted)
{
    m_projectTrusted = trusted;
}

bool HookRegistry::saveProjectTrust()
{
    if (m_workspace.isEmpty()) {
        return false;
    }
    KConfigGroup group(KSharedConfig::openConfig(), u"HarnessHooks"_s);
    group.writeEntry(trustKey(), m_projectTrusted);
    group.sync();
    return true;
}

void HookRegistry::setWorkspace(const QString &workspace)
{
    m_workspace = workspace;
}

bool HookRegistry::load(const QString &workspace, QString *error)
{
    clear();
    m_workspace = workspace;

    KConfigGroup group(KSharedConfig::openConfig(), u"HarnessHooks"_s);
    m_projectTrusted = group.readEntry(trustKey(), false)
        || qEnvironmentVariableIsSet("KATEAI_TRUST_PROJECT_HOOKS");

    QString localError;
    if (!loadFile(managedHooksPath(), false, error ? error : &localError)) {
        return false;
    }
    if (!loadFile(userHooksPath(), false, error ? error : &localError)) {
        return false;
    }
    if (!workspace.isEmpty()
        && !loadFile(QDir(workspace).filePath(u".kateai/hooks.json"_s), true,
                     error ? error : &localError)) {
        return false;
    }
    return true;
}

bool HookRegistry::loadFile(const QString &path, bool project, QString *error)
{
    if (!QFileInfo::exists(path)) {
        return true;
    }
    const QJsonDocument document = QJsonDocument::fromJson(readFile(path).toUtf8());
    if (!document.isObject()) {
        if (error) {
            *error = u"Hook file %1 is not valid JSON."_s.arg(path);
        }
        return false;
    }
    QJsonObject root = document.object();
    QJsonObject entries = root.value(u"hooks"_s).toObject();
    if (entries.isEmpty() && !root.contains(u"hooks"_s)) {
        entries = root;
    }

    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QString event = it.key();
        if (!knownEvents().contains(event)) {
            continue;
        }
        if (!it.value().isArray()) {
            continue;
        }
        for (const QJsonValue &value : it.value().toArray()) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject object = value.toObject();
            HookDefinition hook;
            hook.event = event;
            hook.matcher = object.value(u"matcher"_s).toString();
            hook.timeoutMs = object.value(u"timeoutMs"_s).toInt(5000);
            hook.blockOnFailure = object.value(u"onFailure"_s).toString() == u"block"_s;
            hook.shell = object.value(u"shell"_s).toBool(false);
            hook.source = path;
            hook.project = project;

            const QJsonValue command = object.value(u"command"_s);
            if (command.isArray()) {
                for (const QJsonValue &part : command.toArray()) {
                    hook.argv.append(part.toString());
                }
            } else if (command.isString()) {
                const QString text = command.toString();
                hook.argv = hook.shell ? QStringList{u"/bin/sh"_s, u"-c"_s, text}
                                       : QProcess::splitCommand(text);
            }
            if (!hook.argv.isEmpty() && !hook.argv.first().isEmpty()) {
                m_hooks.append(hook);
            }
        }
    }
    return true;
}

QStringList HookRegistry::hookIds() const
{
    QStringList ids;
    for (const HookDefinition &hook : m_hooks) {
        ids.append(u"%1:%2:%3"_s
                       .arg(hook.event, QFileInfo(hook.source).fileName(),
                            hook.argv.value(0)));
    }
    return ids;
}

bool HookRegistry::hasHooks(const QString &event) const
{
    for (const HookDefinition &hook : m_hooks) {
        if (hook.event == event && (!hook.project || m_projectTrusted)) {
            return true;
        }
    }
    return false;
}

QList<HookDefinition> HookRegistry::hooks(const QString &event) const
{
    QList<HookDefinition> result;
    for (const HookDefinition &hook : m_hooks) {
        if (hook.event == event && (!hook.project || m_projectTrusted)) {
            result.append(hook);
        }
    }
    return result;
}

HookOutcome HookRegistry::run(const QString &event, const QJsonObject &payload,
                              const Sandbox *sandbox)
{
    HookOutcome combined;
    for (const HookDefinition &hook : hooks(event)) {
        if (!matchesEventMatcher(hook.matcher, payload)) {
            continue;
        }
        const HookOutcome outcome = runOne(hook, payload, sandbox);
        if (!outcome.updatedInput.isEmpty()) {
            for (auto it = outcome.updatedInput.constBegin();
                 it != outcome.updatedInput.constEnd(); ++it) {
                combined.updatedInput.insert(it.key(), it.value());
            }
        }
        if (!outcome.additionalContext.isEmpty()) {
            combined.additionalContext += outcome.additionalContext + u'\n';
        }
        combined.warnings.append(outcome.warnings);
        if (outcome.failed || outcome.timedOut) {
            combined.failed = combined.failed || outcome.failed;
            combined.timedOut = combined.timedOut || outcome.timedOut;
        }
        if (outcome.blocked) {
            combined.blocked = true;
            combined.reason = outcome.reason;
            combined.hookId = outcome.hookId;
            return combined;
        }
    }
    return combined;
}

HookOutcome HookRegistry::runOne(const HookDefinition &hook, const QJsonObject &payload,
                                 const Sandbox *sandbox)
{
    HookOutcome outcome;
    outcome.hookId = hook.source + u":" + hook.event;

    QStringList argv = hook.argv;
    if (sandbox) {
        QString wrapError;
        const QString program = argv.takeFirst();
        argv = sandbox->wrapArgv(program, argv, &wrapError, m_workspace);
        if (argv.isEmpty()) {
            outcome.failed = true;
            outcome.reason = wrapError.isEmpty()
                ? u"The sandbox refused to run hook %1."_s.arg(hook.event)
                : wrapError;
            outcome.blocked = hook.blockOnFailure;
            return outcome;
        }
    }

    QJsonObject input = payload;
    input.insert(u"event"_s, hook.event);
    input.insert(u"workspace"_s, m_workspace);
    const QByteArray stdinData = QJsonDocument(input).toJson(QJsonDocument::Compact);

    QProcess process;
    process.setWorkingDirectory(m_workspace);
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (sandbox) {
        process.setProcessEnvironment(sandbox->processEnvironment());
    }
    process.setProgram(argv.takeFirst());
    process.setArguments(argv);
    process.start();
    if (!process.waitForStarted(5000)) {
        outcome.failed = true;
        outcome.reason = u"Hook %1 could not start: %2"_s.arg(hook.event, process.errorString());
        outcome.blocked = hook.blockOnFailure;
        return outcome;
    }
    process.write(stdinData);
    process.closeWriteChannel();
    constexpr qint64 kMaxDrainedBytes = 8 * 1024 * 1024;
    QByteArray drained;
    QElapsedTimer elapsed;
    elapsed.start();
    bool outputCapped = false;
    while (true) {
        drained += process.readAll();
        if (drained.size() > kMaxDrainedBytes) {
            outputCapped = true;
            process.kill();
            process.waitForFinished(1000);
            break;
        }
        if (process.waitForFinished(20)) {
            break;
        }
        if (process.state() == QProcess::NotRunning) {
            break;
        }
        if (elapsed.elapsed() >= qMax(100, hook.timeoutMs)) {
            outcome.timedOut = true;
            process.kill();
            process.waitForFinished(1000);
            outcome.reason = u"Hook %1 timed out after %2 ms."_s.arg(hook.event).arg(hook.timeoutMs);
            outcome.blocked = hook.blockOnFailure;
            outcome.warnings.append(outcome.reason);
            return outcome;
        }
    }
    drained += process.readAll();

    QString output = capOutput(QString::fromUtf8(drained.left(kMaxDrainedBytes)));
    if (outputCapped) {
        output += u"\n… (hook output exceeded 8 MB and was stopped) …"_s;
    }
    const QJsonObject parsed = parseHookOutput(output.toUtf8());
    outcome.updatedInput = parsed.value(u"updatedInput"_s).toObject();
    outcome.additionalContext = parsed.value(u"additionalContext"_s).toString();
    const QString decision = parsed.value(u"decision"_s).toString().toLower();
    if (decision == u"deny"_s || decision == u"block"_s) {
        outcome.blocked = true;
        outcome.reason = parsed.value(u"reason"_s).toString(
            u"Hook %1 blocked the action."_s.arg(hook.event));
        return outcome;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 2) {
        outcome.blocked = true;
        outcome.reason = parsed.value(u"reason"_s).toString(
            output.trimmed().isEmpty() ? u"Hook %1 blocked the action."_s.arg(hook.event)
                                       : output.trimmed().left(1000));
        return outcome;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        outcome.failed = true;
        outcome.reason = output.trimmed().isEmpty()
            ? u"Hook %1 exited with status %2."_s.arg(hook.event).arg(process.exitCode())
            : output.trimmed().left(1000);
        outcome.blocked = hook.blockOnFailure;
        outcome.warnings.append(outcome.reason);
    }
    return outcome;
}

} // namespace KateAi
