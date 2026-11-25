/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpsession.h"
#include "agentloop.h"
#include "args.h"
#include "harness/acpserver.h"
#include "harness/eventstore.h"
#include "harness/replay.h"
#include "session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

namespace
{

void printOut(const QString &text)
{
    std::fwrite(text.toUtf8().constData(), 1, static_cast<size_t>(text.toUtf8().size()), stdout);
    std::fflush(stdout);
}

void printErr(const QString &text)
{
    std::fwrite(text.toUtf8().constData(), 1, static_cast<size_t>(text.toUtf8().size()), stderr);
    std::fflush(stderr);
}

bool writeFile(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = u"Cannot write %1: %2"_s.arg(path, file.errorString());
        }
        return false;
    }
    file.write(contents);
    if (!file.commit()) {
        if (error) {
            *error = u"Cannot commit %1: %2"_s.arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

QString gitDiff(const QString &workspace)
{
    QProcess git;
    git.setWorkingDirectory(workspace);
    git.start(u"git"_s, {u"-C"_s, workspace, u"diff"_s, u"--no-ext-diff"_s, u"--binary"_s});
    if (!git.waitForFinished(30000) || git.exitStatus() != QProcess::NormalExit
        || git.exitCode() != 0) {
        return QString();
    }
    QString patch = QString::fromUtf8(git.readAllStandardOutput());

    git.start(u"git"_s, {u"-C"_s, workspace, u"ls-files"_s, u"--others"_s,
                         u"--exclude-standard"_s});
    if (!git.waitForFinished(30000) || git.exitCode() != 0) {
        return patch;
    }
    const QStringList untracked =
        QString::fromUtf8(git.readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts);
    for (const QString &relative : untracked) {
        const QString absolute = QDir(workspace).filePath(relative);
        QProcess diff;
        diff.start(u"git"_s, {u"diff"_s, u"--no-index"_s, u"--binary"_s, u"--"_s,
                              QStringLiteral("/dev/null"), absolute});
        if (!diff.waitForFinished(30000)) {
            continue;
        }
        patch += QString::fromUtf8(diff.readAllStandardOutput());
    }
    return patch;
}

QJsonObject replayJson(const ReplayReport &report)
{
    QJsonArray errors;
    for (const QString &error : report.errors) {
        errors.append(error);
    }
    QJsonArray steps;
    for (const ReplayStep &step : report.steps) {
        QJsonArray names;
        for (const QString &name : step.toolNames) {
            names.append(name);
        }
        steps.append(QJsonObject{{u"stepId"_s, step.stepId},
                                 {u"turnId"_s, step.turnId},
                                 {u"toolCalls"_s, names},
                                 {u"assistantText"_s, step.assistantText}});
    }
    return QJsonObject{{u"ok"_s, report.ok},
                       {u"errors"_s, errors},
                       {u"toolCalls"_s, report.toolCalls},
                       {u"toolResults"_s, report.toolResults},
                       {u"steps"_s, steps},
                       {u"finalAssistant"_s, report.finalAssistant}};
}

int exitCodeFor(const HeadlessOptions &options, bool ok, const QString &stopReason,
                const QString &error)
{
    if (ok) {
        return 0;
    }
    if (stopReason == u"permission"_s) {
        return 4;
    }
    if (stopReason == u"question"_s) {
        return 5;
    }
    if (options.maxTurns > 0
        && (error.contains(u"budget"_s, Qt::CaseInsensitive)
            || error.contains(u"model request"_s, Qt::CaseInsensitive))) {
        return 3;
    }
    if (options.maxCostUsd > 0.0
        && error.contains(u"budget"_s, Qt::CaseInsensitive)) {
        return 3;
    }
    return 2;
}

int runReplay(const HeadlessOptions &options)
{
    if (options.conversationId.isEmpty()) {
        printErr(u"kateai-headless replay needs --conversation <id>.\n"_s);
        return 1;
    }
    EventStore store;
    QString error;
    if (!store.open(&error)) {
        printErr(error + QStringLiteral("\n"));
        return 1;
    }
    const QList<HarnessEvent> events =
        store.read(options.conversationId, 0, -1, &error);
    if (!error.isEmpty()) {
        printErr(error + QStringLiteral("\n"));
        return 1;
    }
    if (events.isEmpty()) {
        printErr(u"No events for conversation %1.\n"_s.arg(options.conversationId));
        return 2;
    }
    if (options.jsonl) {
        for (const HarnessEvent &event : events) {
            QJsonObject line;
            line.insert(u"seq"_s, event.seq);
            line.insert(u"timestampMs"_s, event.timestampMs);
            line.insert(u"turnId"_s, event.turnId);
            line.insert(u"stepId"_s, event.stepId);
            line.insert(u"type"_s, event.type);
            line.insert(u"payload"_s, event.payload);
            printOut(QString::fromUtf8(QJsonDocument(line).toJson(QJsonDocument::Compact))
                     + QStringLiteral("\n"));
        }
    }
    const ReplayReport report = Replay::analyze(events);
    printOut(QString::fromUtf8(
                 QJsonDocument(replayJson(report)).toJson(QJsonDocument::Indented))
             + QStringLiteral("\n"));
    return report.ok ? 0 : 2;
}

int runPrompt(const HeadlessOptions &options)
{
    if (options.prompt.trimmed().isEmpty()) {
        printErr(u"kateai-headless run needs --prompt <text>.\n\n"_s + HeadlessOptions::usage());
        return 1;
    }
    QString outputError;
    if (!options.outputDir.isEmpty()
        && !QDir().mkpath(options.outputDir)) {
        printErr(u"Cannot create the output directory %1.\n"_s.arg(options.outputDir));
        return 1;
    }

    HeadlessSession session(options, false);
    QString error;
    if (!session.initialize(&error)) {
        printErr(error + QStringLiteral("\n"));
        return 1;
    }

    QFile eventsFile;
    if (!options.outputDir.isEmpty()) {
        eventsFile.setFileName(QDir(options.outputDir).filePath(u"events.jsonl"_s));
        if (!eventsFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            printErr(u"Cannot write %1.\n"_s.arg(eventsFile.fileName()));
        }
    }

    QObject::connect(&session, &HeadlessSession::assistantDelta, &session,
                     [&options](const QString &delta) {
                         if (!options.quiet && !options.jsonl) {
                             printOut(delta);
                         }
                     });
    QObject::connect(&session, &HeadlessSession::harnessEvent, &session,
                     [&options, &eventsFile](const QString &type, const QJsonObject &payload) {
                         QJsonObject line;
                         line.insert(u"type"_s, type);
                         line.insert(u"payload"_s, payload);
                         const QByteArray encoded =
                             QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
                         if (eventsFile.isOpen()) {
                             eventsFile.write(encoded);
                             eventsFile.flush();
                         }
                         if (options.jsonl) {
                             printOut(QString::fromUtf8(encoded));
                         }
                     });
    QObject::connect(&session, &HeadlessSession::notice, &session, [&options](const QString &text) {
        if (!options.quiet && !options.jsonl) {
            printErr(text + QStringLiteral("\n"));
        }
    });

    QObject::connect(&session, &HeadlessSession::finished, &session,
                     [&options, &session, &eventsFile](bool ok, const QString &stopReason,
                                                       const QString &error) {
                         if (eventsFile.isOpen()) {
                             eventsFile.close();
                         }
                         const int code = exitCodeFor(options, ok, stopReason, error);
                         QJsonObject summary = session.summary();
                         summary.insert(u"ok"_s, ok);
                         summary.insert(u"stopReason"_s, stopReason);
                         summary.insert(u"error"_s, error);
                         summary.insert(u"exitCode"_s, code);
                         if (!options.outputDir.isEmpty()) {
                             const QDir dir(options.outputDir);
                             QString writeError;
                             if (!writeFile(dir.filePath(u"summary.json"_s),
                                            QJsonDocument(summary).toJson(QJsonDocument::Indented),
                                            &writeError)) {
                                 printErr(writeError + QStringLiteral("\n"));
                             }
                             if (session.loop()) {
                                 QString traceError;
                                 session.loop()->exportTelemetry(dir.filePath(u"trace.jsonl"_s),
                                                                &traceError);
                             }
                             const QString patch =
                                 gitDiff(session.loop() ? session.loop()->workspace()
                                                        : options.workspace);
                             if (!patch.isEmpty()) {
                                 writeFile(dir.filePath(u"patch.diff"_s), patch.toUtf8(),
                                           nullptr);
                             }
                         }
                         if (!options.quiet && !options.jsonl) {
                             printOut(QStringLiteral("\n"));
                             if (!ok) {
                                 printErr(u"kateai-headless: "_s
                                          + (error.isEmpty() ? u"the turn failed."_s : error)
                                          + QStringLiteral("\n"));
                             }
                         }
                         if (options.jsonl) {
                             printOut(QString::fromUtf8(
                                          QJsonDocument(summary).toJson(QJsonDocument::Compact))
                                      + QStringLiteral("\n"));
                         }
                         QCoreApplication::exit(code);
                     });

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(30 * 60 * 1000);
    QObject::connect(&timeout, &QTimer::timeout, &session, [&session] {
        printErr(u"kateai-headless: the run timed out.\n"_s);
        session.abort();
    });
    timeout.start();

    session.start(options.prompt);
    return QCoreApplication::exec();
}

int runAcp(const HeadlessOptions &options)
{
    AcpServer server([options](const QString &sessionId, const QString &cwd,
                               QString *error) -> AcpServerSession * {
        HeadlessOptions sessionOptions = options;
        sessionOptions.mode = u"run"_s;
        sessionOptions.workspace = cwd;
        sessionOptions.conversationId = sessionId;
        sessionOptions.persist = true;
        sessionOptions.quiet = true;
        auto *session = new HeadlessAcpSession(sessionOptions);
        if (!session->initialize(error)) {
            delete session;
            return nullptr;
        }
        return session;
    });
    QObject::connect(&server, &AcpServer::stdioClosed, &server, [] {
        QCoreApplication::quit();
    });
    QObject::connect(&server, &AcpServer::protocolError, &server, [](const QString &message) {
        printErr(message + QStringLiteral("\n"));
    });
    server.startStdio();
    return QCoreApplication::exec();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"kateai-headless"_s);
    QCoreApplication::setApplicationVersion(QStringLiteral(KATEAI_VERSION));

    HeadlessOptions options;
    QString parseError;
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }
    if (!HeadlessArgs::parse(arguments, &options, &parseError)) {
        printErr(parseError + u"\n\n"_s + HeadlessOptions::usage());
        return 1;
    }
    if (options.help) {
        printOut(HeadlessOptions::usage());
        return 0;
    }
    if (options.version) {
        printOut(u"kateai-headless %1\n"_s.arg(QStringLiteral(KATEAI_VERSION)));
        return 0;
    }
    if (options.mode == u"replay"_s) {
        return runReplay(options);
    }
    if (options.mode == u"acp"_s) {
        if (!options.stdio) {
            printErr(u"The acp mode currently supports --stdio only.\n"_s);
            return 1;
        }
        return runAcp(options);
    }
    return runPrompt(options);
}
