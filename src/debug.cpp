/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "debug.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

DebugProvider::DebugProvider(QObject *parent)
    : QObject(parent)
{
}

QString DebugProvider::stateName(DebugSessionState state)
{
    switch (state) {
    case DebugSessionState::Unavailable:
        return u"unavailable"_s;
    case DebugSessionState::Idle:
        return u"idle"_s;
    case DebugSessionState::Running:
        return u"running"_s;
    case DebugSessionState::Paused:
        return u"paused"_s;
    case DebugSessionState::Terminated:
        return u"terminated"_s;
    }
    return u"unknown"_s;
}

bool DebugProvider::isCrashStopReason(const QString &reason, const QString &description)
{
    const QString combined = (reason + u' ' + description).toLower();
    if (combined.contains(u"exception"_s)
        || combined.contains(u"sigsegv"_s)
        || combined.contains(u"sigabrt"_s)
        || combined.contains(u"sigfpe"_s)
        || combined.contains(u"sigbus"_s)
        || combined.contains(u"sigill"_s)
        || combined.contains(u"sigtrap"_s)
        || combined.contains(u"segmentation"_s)
        || combined.contains(u"assertion"_s)
        || combined.contains(u"panic"_s)
        || combined.contains(u"abort"_s)
        || combined.contains(u"crash"_s)
        || combined.contains(u"signal"_s)) {
        return true;
    }
    return false;
}

DebugSnapshot DebugProvider::captureSnapshot(const QString &label)
{
    DebugSnapshot snap;
    snap.capturedAt = QDateTime::currentDateTimeUtc();
    snap.stopInfo = currentStop();
    snap.frames = stackFrames();
    snap.locals = locals();
    snap.consoleTail = recentOutput(1200);
    if (!label.trimmed().isEmpty()) {
        snap.label = label.trimmed();
    } else if (!snap.stopInfo.file.isEmpty() && snap.stopInfo.line > 0) {
        snap.label = u"%1:%2 (%3)"_s.arg(QFileInfo(snap.stopInfo.file).fileName())
                         .arg(snap.stopInfo.line)
                         .arg(snap.capturedAt.toString(u"hh:mm:ss"_s));
    } else {
        snap.label = u"Snapshot #%1 (%2)"_s.arg(m_snapshots.size() + 1)
                         .arg(snap.capturedAt.toString(u"hh:mm:ss"_s));
    }
    m_snapshots.append(snap);
    constexpr int kMaxSnapshots = 12;
    while (m_snapshots.size() > kMaxSnapshots) {
        m_snapshots.removeFirst();
    }
    return snap;
}

QString DebugProvider::diffSnapshots(const DebugSnapshot &first, const DebugSnapshot &second)
{
    QStringList out;
    out.append(u"=== Debug Snapshot Diff: \"%1\" -> \"%2\" ==="_s.arg(first.label, second.label));
    out.append(u"Location A: %1:%2 (%3)"_s.arg(
        first.stopInfo.file.isEmpty() ? u"<unknown>"_s : first.stopInfo.file,
        QString::number(first.stopInfo.line),
        first.stopInfo.reason.isEmpty() ? u"stopped"_s : first.stopInfo.reason));
    out.append(u"Location B: %1:%2 (%3)"_s.arg(
        second.stopInfo.file.isEmpty() ? u"<unknown>"_s : second.stopInfo.file,
        QString::number(second.stopInfo.line),
        second.stopInfo.reason.isEmpty() ? u"stopped"_s : second.stopInfo.reason));

    QHash<QString, DebugVariable> varsA;
    for (const DebugVariable &v : first.locals) {
        varsA.insert(v.scope + u"::"_s + v.name, v);
    }
    QHash<QString, DebugVariable> varsB;
    for (const DebugVariable &v : second.locals) {
        varsB.insert(v.scope + u"::"_s + v.name, v);
    }

    QStringList changed;
    QStringList added;
    QStringList removed;
    for (auto it = varsB.cbegin(); it != varsB.cend(); ++it) {
        const auto aIt = varsA.constFind(it.key());
        if (aIt == varsA.cend()) {
            added.append(u"+ %1 (%2) = %3"_s.arg(it.value().name, it.value().type, it.value().value));
        } else if (aIt.value().value != it.value().value || aIt.value().type != it.value().type) {
            changed.append(u"~ %1: %2 -> %3"_s.arg(it.value().name, aIt.value().value, it.value().value));
        }
    }
    for (auto it = varsA.cbegin(); it != varsA.cend(); ++it) {
        if (!varsB.contains(it.key())) {
            removed.append(u"- %1 (%2) = %3"_s.arg(it.value().name, it.value().type, it.value().value));
        }
    }

    if (changed.isEmpty() && added.isEmpty() && removed.isEmpty()) {
        out.append(u"Locals: identical across both snapshots."_s);
    } else {
        out.append(u"Locals diff:"_s);
        out.append(changed);
        out.append(added);
        out.append(removed);
    }

    out.append(u"Stack depth: %1 -> %2"_s.arg(first.frames.size()).arg(second.frames.size()));
    const int minDepth = qMin(first.frames.size(), second.frames.size());
    int divergeIdx = -1;
    for (int i = 0; i < minDepth; ++i) {
        const auto &fa = first.frames.at(i);
        const auto &fb = second.frames.at(i);
        if (fa.function != fb.function || fa.file != fb.file || fa.line != fb.line) {
            divergeIdx = i;
            break;
        }
    }
    if (divergeIdx >= 0) {
        const auto &fa = first.frames.at(divergeIdx);
        const auto &fb = second.frames.at(divergeIdx);
        out.append(u"Stack diverges at frame #%1:\n  A: %2 (%3:%4)\n  B: %5 (%6:%7)"_s
                       .arg(divergeIdx)
                       .arg(fa.function, fa.file).arg(fa.line)
                       .arg(fb.function, fb.file).arg(fb.line));
    }
    return out.join(u'\n');
}

QString DebugProvider::contextSummary(int maxFrames, int maxLocals, int maxOutputChars) const
{
    if (!isAvailable()) {
        return {};
    }
    const DebugSessionState state = sessionState();
    const QList<DebugBreakpoint> bps = breakpoints();
    if (state == DebugSessionState::Idle && bps.isEmpty()) {
        return u"Debugger: plugin loaded (idle)"_s;
    }

    QStringList lines;
    const QString target = activeTarget();
    lines.append(target.isEmpty()
                     ? u"Debugger: %1"_s.arg(stateName(state))
                     : u"Debugger: %1 (target: %2)"_s.arg(stateName(state), target));

    if (state == DebugSessionState::Paused) {
        const DebugStopInfo stop = currentStop();
        QString stopLine = u"Stopped"_s;
        if (!stop.reason.isEmpty()) {
            stopLine += u" (%1)"_s.arg(stop.reason);
        }
        if (!stop.description.isEmpty()) {
            stopLine += u": %1"_s.arg(stop.description);
        }
        if (!stop.file.isEmpty() && stop.line > 0) {
            stopLine += u" at %1:%2"_s.arg(stop.file).arg(stop.line);
        }
        if (stop.threadId >= 0) {
            stopLine += u" [thread %1]"_s.arg(stop.threadId);
        }
        lines.append(stopLine);

        const QList<DebugStackFrame> frames = stackFrames();
        if (!frames.isEmpty()) {
            lines.append(u"Call stack:"_s);
            const int count = qMin(frames.size(), qMax(1, maxFrames));
            for (int i = 0; i < count; ++i) {
                const DebugStackFrame &f = frames.at(i);
                const QString marker = f.active ? u">"_s : u" "_s;
                QString loc = f.file.isEmpty() ? u"<unknown>"_s : f.file;
                if (f.line > 0) {
                    loc += u":%1"_s.arg(f.line);
                }
                lines.append(u"  %1#%2 %3 (%4)"_s.arg(marker).arg(f.level).arg(f.function, loc));
            }
            if (frames.size() > count) {
                lines.append(u"  ... (%1 more frames)"_s.arg(frames.size() - count));
            }
        }

        const QList<DebugVariable> vars = locals();
        if (!vars.isEmpty()) {
            lines.append(u"Locals:"_s);
            const int count = qMin(vars.size(), qMax(1, maxLocals));
            for (int i = 0; i < count; ++i) {
                const DebugVariable &v = vars.at(i);
                QString val = v.value;
                if (val.size() > 140) {
                    val = val.left(140) + u"..."_s;
                }
                lines.append(v.type.isEmpty()
                                 ? u"  - %1 = %2"_s.arg(v.name, val)
                                 : u"  - %1 (%2) = %3"_s.arg(v.name, v.type, val));
            }
            if (vars.size() > count) {
                lines.append(u"  ... (%1 more variables)"_s.arg(vars.size() - count));
            }
        }
    }

    if (!bps.isEmpty()) {
        QStringList bpItems;
        const int cap = qMin(bps.size(), 12);
        for (int i = 0; i < cap; ++i) {
            const DebugBreakpoint &bp = bps.at(i);
            QString item;
            if (bp.kind == u"function"_s) {
                item = u"fn:%1"_s.arg(bp.functionName);
            } else {
                item = u"%1:%2"_s.arg(bp.file).arg(bp.line);
            }
            if (!bp.condition.isEmpty()) {
                item += u" [when %1]"_s.arg(bp.condition);
            }
            if (!bp.enabled) {
                item += u" (disabled)"_s;
            }
            bpItems.append(item);
        }
        lines.append(u"Breakpoints (%1): %2"_s.arg(bps.size()).arg(bpItems.join(u", "_s)));
    }

    if (state == DebugSessionState::Paused || state == DebugSessionState::Terminated) {
        const QString tail = recentOutput(maxOutputChars).trimmed();
        if (!tail.isEmpty()) {
            lines.append(u"Recent debugger output:\n%1"_s.arg(tail));
        }
    }

    return lines.join(u'\n');
}

namespace
{

QStringList discoverCmakeExecutables(const QString &workspace)
{
    QStringList execs;
    const QStringList buildDirs = {u"build"_s, u"cmake-build-debug"_s, u"out/build"_s};
    for (const QString &relBuild : buildDirs) {
        const QString replyDir = QDir(workspace).absoluteFilePath(relBuild + u"/.cmake/api/v1/reply"_s);
        QDir dir(replyDir);
        if (!dir.exists()) {
            continue;
        }
        const QStringList targetFiles = dir.entryList({u"target-*.json"_s}, QDir::Files);
        for (const QString &tf : targetFiles) {
            QFile f(dir.absoluteFilePath(tf));
            if (!f.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            if (obj.value(u"type"_s).toString() != u"EXECUTABLE"_s) {
                continue;
            }
            const QJsonArray artifacts = obj.value(u"artifacts"_s).toArray();
            for (const QJsonValue &artVal : artifacts) {
                const QString relArt = artVal.toObject().value(u"path"_s).toString();
                if (!relArt.isEmpty()) {
                    const QString full = QDir::cleanPath(
                        QDir(workspace).absoluteFilePath(relBuild + u'/' + relArt));
                    if (!execs.contains(full)) {
                        execs.append(full);
                    }
                }
            }
        }
    }

    // Fallback: parse add_executable(...) in root CMakeLists.txt
    if (execs.isEmpty()) {
        QFile cmakeFile(QDir(workspace).absoluteFilePath(u"CMakeLists.txt"_s));
        if (cmakeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(cmakeFile.read(128 * 1024));
            static const QRegularExpression addExecRe(
                QStringLiteral("add_executable\\s*\\(\\s*([A-Za-z0-9_.-]+)"),
                QRegularExpression::CaseInsensitiveOption);
            auto it = addExecRe.globalMatch(content);
            while (it.hasNext() && execs.size() < 5) {
                const QString name = it.next().captured(1);
                if (!name.startsWith(u'$')) {
                    execs.append(u"${workspaceFolder}/build/%1"_s.arg(name));
                }
            }
        }
    }
    return execs;
}

} // namespace

QJsonDocument DebugProvider::generateLaunchConfig(const QString &workspace, QString *summary)
{
    QJsonArray configs;
    QStringList detected;
    const QDir root(workspace);

    // 1. CMake / C / C++
    if (root.exists(u"CMakeLists.txt"_s) || root.exists(u"Makefile"_s) || root.exists(u"meson.build"_s)) {
        detected.append(u"C/C++"_s);
        QStringList execs = discoverCmakeExecutables(workspace);
        if (execs.isEmpty()) {
            execs.append(u"${workspaceFolder}/build/app"_s);
        }
        const int cap = qMin(execs.size(), 5);
        for (int i = 0; i < cap; ++i) {
            QString prog = execs.at(i);
            const QString label = QFileInfo(prog).fileName();
            if (prog.startsWith(workspace + u'/')) {
                prog = u"${workspaceFolder}/"_s + prog.mid(workspace.size() + 1);
            }
            configs.append(QJsonObject{
                {u"name"_s, u"GDB: %1"_s.arg(label)},
                {u"type"_s, u"gdb"_s},
                {u"request"_s, u"launch"_s},
                {u"program"_s, prog},
                {u"args"_s, QJsonArray{}},
                {u"cwd"_s, u"${workspaceFolder}"_s},
            });
        }
    }

    // 2. Rust (Cargo)
    if (root.exists(u"Cargo.toml"_s)) {
        detected.append(u"Rust"_s);
        QString crateName = QFileInfo(workspace).fileName();
        QFile cargo(root.absoluteFilePath(u"Cargo.toml"_s));
        if (cargo.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(cargo.read(32 * 1024));
            static const QRegularExpression nameRe(QStringLiteral("^\\s*name\\s*=\\s*\"([^\"]+)\""),
                                                   QRegularExpression::MultilineOption);
            const auto m = nameRe.match(text);
            if (m.hasMatch()) {
                crateName = m.captured(1);
            }
        }
        configs.append(QJsonObject{
            {u"name"_s, u"LLDB: %1 (debug)"_s.arg(crateName)},
            {u"type"_s, u"lldb-dap"_s},
            {u"request"_s, u"launch"_s},
            {u"program"_s, u"${workspaceFolder}/target/debug/%1"_s.arg(crateName)},
            {u"args"_s, QJsonArray{}},
            {u"cwd"_s, u"${workspaceFolder}"_s},
        });
    }

    // 3. Python
    if (root.exists(u"pyproject.toml"_s) || root.exists(u"setup.py"_s) || root.exists(u"requirements.txt"_s)
        || root.exists(u"main.py"_s) || root.exists(u"app.py"_s)) {
        detected.append(u"Python"_s);
        QString entry = u"${file}"_s;
        if (root.exists(u"main.py"_s)) {
            entry = u"${workspaceFolder}/main.py"_s;
        } else if (root.exists(u"app.py"_s)) {
            entry = u"${workspaceFolder}/app.py"_s;
        }
        configs.append(QJsonObject{
            {u"name"_s, u"Python: Debug Entry"_s},
            {u"type"_s, u"python"_s},
            {u"request"_s, u"launch"_s},
            {u"program"_s, entry},
            {u"cwd"_s, u"${workspaceFolder}"_s},
        });
        configs.append(QJsonObject{
            {u"name"_s, u"Python: Pytest"_s},
            {u"type"_s, u"python"_s},
            {u"request"_s, u"launch"_s},
            {u"module"_s, u"pytest"_s},
            {u"cwd"_s, u"${workspaceFolder}"_s},
        });
    }

    // 4. Go
    if (root.exists(u"go.mod"_s)) {
        detected.append(u"Go"_s);
        configs.append(QJsonObject{
            {u"name"_s, u"Go: Launch Package"_s},
            {u"type"_s, u"go"_s},
            {u"request"_s, u"launch"_s},
            {u"mode"_s, u"auto"_s},
            {u"program"_s, u"${workspaceFolder}"_s},
        });
    }

    // 5. Node / TypeScript
    if (root.exists(u"package.json"_s)) {
        detected.append(u"Node/JS"_s);
        QString mainEntry = u"${workspaceFolder}/index.js"_s;
        QFile pkg(root.absoluteFilePath(u"package.json"_s));
        if (pkg.open(QIODevice::ReadOnly)) {
            const QString m = QJsonDocument::fromJson(pkg.readAll()).object().value(u"main"_s).toString();
            if (!m.isEmpty()) {
                mainEntry = u"${workspaceFolder}/"_s + m;
            }
        }
        configs.append(QJsonObject{
            {u"name"_s, u"Node: Launch Program"_s},
            {u"type"_s, u"node"_s},
            {u"request"_s, u"launch"_s},
            {u"program"_s, mainEntry},
            {u"cwd"_s, u"${workspaceFolder}"_s},
        });
    }

    if (configs.isEmpty()) {
        configs.append(QJsonObject{
            {u"name"_s, u"GDB: Launch Executable"_s},
            {u"type"_s, u"gdb"_s},
            {u"request"_s, u"launch"_s},
            {u"program"_s, u"${workspaceFolder}/a.out"_s},
            {u"args"_s, QJsonArray{}},
            {u"cwd"_s, u"${workspaceFolder}"_s},
        });
    }

    if (summary) {
        *summary = detected.isEmpty()
            ? u"Generated default GDB launch configuration."_s
            : u"Generated %1 launch configuration(s) for %2."_s.arg(configs.size()).arg(detected.join(u", "_s));
    }

    QJsonObject doc;
    doc.insert(u"version"_s, u"0.2.0"_s);
    doc.insert(u"configurations"_s, configs);
    return QJsonDocument(doc);
}

} // namespace KateAi
