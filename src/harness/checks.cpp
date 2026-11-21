/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "checks.h"

#include "sandbox.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

constexpr int kMaxCommands = 3;
constexpr int kMaxOutputChars = 20000;

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QStringList splitCommandLine(QString line)
{
    line = line.trimmed();
    if (line.startsWith(u"- "_s) || line.startsWith(u"* "_s)) {
        line = line.mid(2).trimmed();
    }
    // Prefer the first inline-code span: "- run `ctest --output-on-failure`".
    static const QRegularExpression code(uR"(`([^`]+)`)"_s);
    const QRegularExpressionMatch match = code.match(line);
    if (match.hasMatch()) {
        return {match.captured(1).trimmed()};
    }
    // Otherwise keep the leading command and drop a prose suffix.
    const int separator = line.indexOf(u" — "_s);
    if (separator > 0) {
        line = line.left(separator);
    }
    line = line.trimmed();
    return line.isEmpty() ? QStringList() : QStringList{line};
}

void appendUnique(CheckPlan *plan, const CheckCommand &command, const QString &source)
{
    if (command.command.trimmed().isEmpty() || plan->commands.size() >= kMaxCommands) {
        return;
    }
    for (const CheckCommand &existing : std::as_const(plan->commands)) {
        if (existing.command == command.command) {
            return;
        }
    }
    plan->commands.append(command);
    if (!plan->sources.contains(source)) {
        plan->sources.append(source);
    }
}

QString packageManager(const QString &workspace)
{
    if (QFileInfo::exists(workspace + u"/pnpm-lock.yaml"_s)) {
        return u"pnpm"_s;
    }
    if (QFileInfo::exists(workspace + u"/yarn.lock"_s)) {
        return u"yarn"_s;
    }
    return u"npm"_s;
}

} // namespace

QJsonObject CheckCommand::toJson() const
{
    QJsonObject object;
    object.insert(u"command"_s, command);
    if (!paths.isEmpty()) {
        object.insert(u"paths"_s, QJsonArray::fromStringList(paths));
    }
    object.insert(u"timeoutMs"_s, timeoutMs);
    return object;
}

QString CheckResult::summary() const
{
    if (timedOut) {
        return u"%1 — timed out"_s.arg(command);
    }
    return u"%1 — %2 (exit %3)"_s
        .arg(command, ok ? u"passed"_s : u"failed"_s)
        .arg(exitCode);
}

QStringList CheckDiscovery::parseInstructionCommands(const QString &text)
{
    QStringList commands;
    static const QRegularExpression heading(uR"(^#{1,6}\s+(.+)$)"_s);
    bool inCommands = false;
    const QStringList lines = text.split(u'\n');
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = heading.match(line);
        if (match.hasMatch()) {
            const QString title = match.captured(1).trimmed().toLower();
            inCommands = title == u"commands"_s || title == u"checks"_s
                || title == u"build & test"_s || title == u"build and test"_s;
            continue;
        }
        if (!inCommands) {
            continue;
        }
        if (!line.trimmed().startsWith(u"- "_s) && !line.trimmed().startsWith(u"* "_s)) {
            continue;
        }
        commands.append(splitCommandLine(line));
    }
    return commands;
}

bool CheckDiscovery::pathRelevant(const CheckCommand &command, const QStringList &changedPaths)
{
    if (command.paths.isEmpty() || changedPaths.isEmpty()) {
        return true;
    }
    for (const QString &changed : changedPaths) {
        for (const QString &pattern : command.paths) {
            if (Sandbox::globMatch(pattern, changed)
                || Sandbox::globMatch(pattern, QDir::cleanPath(changed))) {
                return true;
            }
        }
    }
    return false;
}

CheckPlan CheckDiscovery::discover(const QString &workspace, const QStringList &changedPaths)
{
    CheckPlan plan;
    if (workspace.isEmpty()) {
        return plan;
    }
    const QDir root(workspace);

    // 1. Explicit project configuration.
    const QString configPath = root.filePath(u".kateai/config.json"_s);
    if (QFileInfo::exists(configPath)) {
        const QJsonDocument document = QJsonDocument::fromJson(readFile(configPath).toUtf8());
        if (document.isObject()) {
            const QJsonValue verify = document.object().value(u"verify"_s);
            if (verify.isArray()) {
                for (const QJsonValue &value : verify.toArray()) {
                    CheckCommand command;
                    if (value.isString()) {
                        command.command = value.toString();
                    } else if (value.isObject()) {
                        const QJsonObject object = value.toObject();
                        command.command = object.value(u"command"_s).toString();
                        for (const QJsonValue &path : object.value(u"paths"_s).toArray()) {
                            command.paths.append(path.toString());
                        }
                        command.timeoutMs = object.value(u"timeoutMs"_s).toInt(command.timeoutMs);
                    }
                    if (!command.command.isEmpty()
                        && pathRelevant(command, changedPaths)) {
                        appendUnique(&plan, command, configPath);
                    }
                }
            }
        }
    }

    // 2. Project instruction files.
    for (const QString &name : {u"AGENTS.md"_s, u"KATEAI.md"_s, u"CLAUDE.md"_s}) {
        const QString path = root.filePath(name);
        if (!QFileInfo::exists(path)) {
            continue;
        }
        for (const QString &line : parseInstructionCommands(readFile(path))) {
            CheckCommand command;
            command.command = line;
            appendUnique(&plan, command, path);
        }
    }

    // 3. Build-system detection.
    QString ctestBuildDir;
    for (const QString &candidate : {u"build"_s, u"Build"_s, u"out"_s}) {
        if (QFileInfo::exists(root.filePath(candidate + u"/CTestTestfile.cmake"_s))) {
            ctestBuildDir = candidate;
            break;
        }
    }
    if (ctestBuildDir.isEmpty() && QFileInfo::exists(root.filePath(u"CTestTestfile.cmake"_s))) {
        ctestBuildDir = u"."_s;
    }
    if (!ctestBuildDir.isEmpty()) {
        CheckCommand command;
        command.command = ctestBuildDir == u"."_s
            ? u"ctest --output-on-failure"_s
            : u"ctest --test-dir %1 --output-on-failure"_s.arg(ctestBuildDir);
        command.timeoutMs = 300000;
        appendUnique(&plan, command, u"CMake/CTest"_s);
    }

    if (QFileInfo::exists(root.filePath(u"Cargo.toml"_s))) {
        CheckCommand command;
        command.command = u"cargo test"_s;
        command.timeoutMs = 300000;
        appendUnique(&plan, command, u"Cargo"_s);
    }

    if (QFileInfo::exists(root.filePath(u"package.json"_s))) {
        const QJsonDocument document =
            QJsonDocument::fromJson(readFile(root.filePath(u"package.json"_s)).toUtf8());
        const QJsonObject scripts = document.object().value(u"scripts"_s).toObject();
        if (scripts.contains(u"test"_s)) {
            CheckCommand command;
            command.command = packageManager(workspace) + u" test"_s;
            command.timeoutMs = 300000;
            appendUnique(&plan, command, u"package.json"_s);
        }
    }

    if (QFileInfo::exists(root.filePath(u"pyproject.toml"_s))
        || QFileInfo::exists(root.filePath(u"pytest.ini"_s))
        || QFileInfo::exists(root.filePath(u"tox.ini"_s))) {
        CheckCommand command;
        command.command = u"pytest -q"_s;
        command.timeoutMs = 300000;
        appendUnique(&plan, command, u"pytest"_s);
    }

    if (QFileInfo::exists(root.filePath(u"Makefile"_s))) {
        const QString makefile = readFile(root.filePath(u"Makefile"_s));
        CheckCommand command;
        if (QRegularExpression(uR"(^test\s*:)"_s, QRegularExpression::MultilineOption)
                .match(makefile)
                .hasMatch()) {
            command.command = u"make test"_s;
        } else if (QRegularExpression(uR"(^check\s*:)"_s, QRegularExpression::MultilineOption)
                       .match(makefile)
                       .hasMatch()) {
            command.command = u"make check"_s;
        }
        if (!command.command.isEmpty()) {
            command.timeoutMs = 300000;
            appendUnique(&plan, command, u"Makefile"_s);
        }
    }

    if (plan.commands.isEmpty() && QFileInfo::exists(root.filePath(u".git"_s))) {
        CheckCommand command;
        command.command = u"git diff --check"_s;
        command.timeoutMs = 30000;
        appendUnique(&plan, command, u"fallback"_s);
    }

    if (plan.commands.size() >= kMaxCommands) {
        plan.note = u"At most %1 checks run per verification."_s.arg(kMaxCommands);
    }
    return plan;
}

QList<CheckResult> CheckRunner::run(const QString &workspace, const CheckPlan &plan,
                                    const Sandbox *sandbox, int totalTimeoutMs)
{
    QList<CheckResult> results;
    qint64 remaining = qMax(0, totalTimeoutMs);

    for (const CheckCommand &check : plan.commands) {
        if (results.size() >= kMaxCommands || remaining <= 0) {
            break;
        }
        CheckResult result;
        result.command = check.command;

        QString wrapError;
        QStringList argv;
        if (sandbox) {
            argv = sandbox->wrapCommand(check.command, &wrapError, workspace);
        } else {
#ifdef Q_OS_WIN
            argv = {u"cmd.exe"_s, u"/c"_s, check.command};
#else
            argv = {u"/bin/sh"_s, u"-c"_s, check.command};
#endif
        }
        if (argv.isEmpty()) {
            result.output = wrapError.isEmpty()
                ? u"The sandbox refused to wrap the check command."_s
                : wrapError;
            results.append(result);
            continue;
        }

        QProcess process;
        process.setWorkingDirectory(workspace);
        process.setProcessChannelMode(QProcess::MergedChannels);
        if (sandbox) {
            process.setProcessEnvironment(sandbox->processEnvironment());
        }
        const QString program = argv.takeFirst();
        process.setProgram(program);
        process.setArguments(argv);

        const int timeout = static_cast<int>(qMin<qint64>(
            qMin(check.timeoutMs > 0 ? check.timeoutMs : 120000, remaining), 600000));
        QElapsedTimer timer;
        timer.start();
        process.start();
        if (!process.waitForStarted(5000)) {
            result.output = u"The check could not be started: %1"_s.arg(process.errorString());
            result.durationMs = timer.elapsed();
            results.append(result);
            remaining -= result.durationMs;
            continue;
        }
        if (!process.waitForFinished(timeout)) {
            result.timedOut = true;
            process.kill();
            if (!process.waitForFinished(2000)) {
                process.terminate();
                process.waitForFinished(1000);
            }
        }
        result.durationMs = timer.elapsed();
        result.exitCode = static_cast<int>(process.exitCode());
        result.ok = !result.timedOut && process.exitStatus() == QProcess::NormalExit
            && result.exitCode == 0;
        QString output = QString::fromUtf8(process.readAll());
        if (output.size() > kMaxOutputChars) {
            const int head = kMaxOutputChars / 2;
            const int tail = kMaxOutputChars - head;
            output = output.left(head) + u"\n… (truncated) …\n"_s + output.right(tail);
        }
        result.output = output.trimmed();
        results.append(result);
        remaining -= result.durationMs;
    }
    return results;
}

QString CheckRunner::evidence(const QList<CheckResult> &results)
{
    if (results.isEmpty()) {
        return u"No checks were configured."_s;
    }
    QStringList lines;
    for (const CheckResult &result : results) {
        lines.append(result.summary());
        if (!result.output.isEmpty()) {
            lines.append(result.output.left(4000));
        }
    }
    return lines.join(u'\n');
}

QString VerificationMiddleware::id() const
{
    return u"verification"_s;
}

VerificationMiddleware::VerificationMiddleware(QString workspace, const Sandbox *sandbox,
                                               int maxAttempts)
    : m_workspace(std::move(workspace))
    , m_sandbox(sandbox)
    , m_maxAttempts(qMax(0, maxAttempts))
{
}

void VerificationMiddleware::setPlan(const CheckPlan &plan)
{
    m_plan = plan;
}

MiddlewareResult VerificationMiddleware::onStop(TurnContext &context)
{
    if (m_maxAttempts == 0 || !context.mutationOccurred || context.verificationPassed) {
        return MiddlewareResult::next();
    }
    if (m_plan.isEmpty()) {
        m_plan = CheckDiscovery::discover(m_workspace, context.changedPaths.values());
    }
    if (m_plan.isEmpty()) {
        return MiddlewareResult::next();
    }
    if (context.verificationAttempts >= m_maxAttempts) {
        return MiddlewareResult::stop(
            u"Verification did not pass after %1 attempts."_s.arg(m_maxAttempts), id());
    }

    ++context.verificationAttempts;
    context.phase = TurnPhase::Verifying;
    m_results = CheckRunner::run(m_workspace, m_plan, m_sandbox);
    const bool passed = !m_results.isEmpty()
        && std::all_of(m_results.cbegin(), m_results.cend(),
                       [](const CheckResult &result) { return result.ok; });
    if (passed) {
        context.verificationPassed = true;
        return MiddlewareResult::next();
    }
    return MiddlewareResult::retry(
        u"Verification failed:\n"_s + CheckRunner::evidence(m_results), id());
}

} // namespace KateAi
