/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "kateaicommand.h"

#include "chatwidget.h"
#include "plugin.h"
#include "pluginview.h"

#include <KCompletion>
#include <KLocalizedString>

#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

const QString &commandWord()
{
    static const QString word = u"kateai"_s;
    return word;
}

QString usageText()
{
    return i18n(
        "usage: kateai ask <text> | explain | fix | refactor | tests | diagnostics | symbols | "
        "debug | explain-stop | stack | break [cond] | snapshot | fix-errors | build-errors | "
        "launch-config | backtrace | terminal | blame | commit | diff-tab | graph-tab | annotations | "
        "context | usage | prompt | memory | tools | skills | skill <name> | trust-skill <name> | inspector | verify | policy | hooks | trace | trust-hooks | apply-subtask <id> | discard-subtask <id> | rewind [id] | fork | voice | stop | new | mode <id> | panel");
}

// The instruction of the one-word review subcommands. Kept next to the
// context-menu actions they mirror.
QString instructionFor(const QString &sub)
{
    if (sub == u"explain"_s) {
        return i18n("Explain this code.");
    }
    if (sub == u"fix"_s) {
        return i18n("Find and fix problems in this code. Explain the change, then apply it.");
    }
    if (sub == u"refactor"_s) {
        return i18n("Refactor this code for clarity and maintainability. Explain the change, then apply it.");
    }
    if (sub == u"tests"_s) {
        return i18n("Add or improve focused tests for this code. Explain the proposed coverage first.");
    }
    if (sub == u"diagnostics"_s) {
        return i18n("Report the diagnostics (errors and warnings) for this file with lsp_diagnostics and propose concrete fixes. Explain the change, then apply it.");
    }
    if (sub == u"symbols"_s) {
        return i18n("List this file's symbols with lsp_symbols and explain its structure and the role of each part.");
    }
    return QString();
}

} // namespace

KateAiCommand::KateAiCommand(KateAiPlugin *plugin, QObject *parent)
    : KTextEditor::Command({commandWord()}, parent)
    , m_plugin(plugin)
{
}

bool KateAiCommand::supportsRange(const QString &cmd)
{
    Q_UNUSED(cmd);
    // "kateai explain" (and friends) act on the range the command line
    // carries, e.g. the vi-mode "'<,'>kateai explain".
    return true;
}

KCompletion *KateAiCommand::completionObject(KTextEditor::View *view, const QString &cmdname)
{
    Q_UNUSED(view);
    Q_UNUSED(cmdname);
    // A fresh object per call: the command line takes ownership and deletes
    // it, so a shared one would dangle after the first completion.
    auto *completion = new KCompletion();
    completion->setIgnoreCase(true);
    completion->setItems({u"ask "_s, u"explain"_s, u"fix"_s, u"refactor"_s, u"tests"_s,
                          u"diagnostics"_s, u"symbols"_s,
                          u"debug"_s, u"explain-stop"_s, u"stack"_s, u"break "_s, u"snapshot"_s,
                          u"fix-errors"_s, u"build-errors"_s, u"launch-config"_s,
                          u"backtrace"_s, u"terminal"_s, u"blame"_s, u"commit"_s,
                          u"diff-tab"_s, u"graph-tab"_s, u"annotations"_s,
                          u"context"_s, u"usage"_s, u"prompt"_s, u"memory"_s, u"tools"_s,
                          u"skills"_s, u"skill "_s, u"trust-skill "_s,
                          u"inspector"_s, u"apply-subtask "_s, u"discard-subtask "_s,
                          u"rewind"_s, u"fork"_s, u"verify"_s, u"policy"_s,
                          u"hooks"_s, u"trace"_s, u"trust-hooks"_s, u"voice"_s,
                          u"stop"_s, u"new"_s, u"mode "_s, u"panel"_s});
    return completion;
}

bool KateAiCommand::help(KTextEditor::View *view, const QString &cmd, QString &msg)
{
    Q_UNUSED(view);
    Q_UNUSED(cmd);
    msg = usageText();
    return true;
}

bool KateAiCommand::exec(KTextEditor::View *view, const QString &cmd, QString &msg,
                         const KTextEditor::Range &range)
{
    if (!m_plugin) {
        return false;
    }

    // The command line hands over the arguments; the vi command line may
    // still include the command word itself. Accept both.
    QString args = cmd.trimmed();
    if (args.startsWith(commandWord(), Qt::CaseInsensitive)) {
        const QString rest = args.mid(commandWord().size());
        if (rest.isEmpty() || rest.at(0).isSpace()) {
            args = rest.trimmed();
        }
    }

    // Dispatch to the view of the window the command was used in; the editor
    // view identifies it. A command with no matching window (an editor view
    // without a Kate AI panel) falls back to the first one.
    KateAiView *target = nullptr;
    for (KateAiView *candidate : m_plugin->views()) {
        if (candidate && candidate->ownsEditorView(view)) {
            target = candidate;
            break;
        }
    }
    if (!target) {
        for (KateAiView *candidate : m_plugin->views()) {
            if (candidate) {
                target = candidate;
                break;
            }
        }
    }
    if (!target || !target->chat()) {
        msg = i18n("Kate AI is not available in this window.");
        return false;
    }

    const QString sub = args.section(u' ', 0, 0).toLower();
    const QString rest = args.section(u' ', 1).trimmed();

    if (sub == u"context"_s) {
        target->chat()->showContextReport();
        return true;
    }
    if (sub == u"usage"_s) {
        target->chat()->showUsageReport();
        return true;
    }
    if (sub == u"prompt"_s) {
        target->chat()->showPromptReport();
        return true;
    }
    if (sub == u"memory"_s) {
        target->chat()->showMemoryReport();
        return true;
    }
    if (sub == u"tools"_s) {
        target->chat()->showToolsReport();
        return true;
    }
    if (sub == u"skills"_s) {
        target->chat()->showSkillsReport();
        return true;
    }
    if (sub == u"skill"_s) {
        if (rest.isEmpty()) {
            target->chat()->showSkillsReport();
            return true;
        }
        QString error;
        const QString prompt = target->chat()->agent()->skillInvocation(rest, &error);
        if (prompt.isEmpty()) {
            msg = error.isEmpty() ? i18n("Kate AI: skill not found.") : error;
            return false;
        }
        target->showPanel();
        target->chat()->ask(prompt);
        msg = i18n("Kate AI: running skill %1.", rest);
        return true;
    }
    if (sub == u"trust-skill"_s) {
        if (rest.isEmpty()) {
            msg = i18n("usage: kateai trust-skill <name>");
            return false;
        }
        QString error;
        if (!target->chat()->agent()->trustSkill(rest, &error)) {
            msg = error.isEmpty() ? i18n("Kate AI: could not trust that skill.") : error;
            return false;
        }
        msg = i18n("Kate AI: trusted skill %1.", rest);
        return true;
    }
    if (sub == u"inspector"_s) {
        target->chat()->showInspector();
        return true;
    }
    if (sub == u"apply-subtask"_s || sub == u"discard-subtask"_s) {
        if (rest.isEmpty()) {
            msg = i18n("usage: kateai %1 <task-id>", sub);
            return false;
        }
        QString error;
        const bool ok = sub == u"apply-subtask"_s
            ? target->chat()->agent()->applySubagentWorktree(rest, &error)
            : target->chat()->agent()->discardSubagentWorktree(rest, &error);
        if (!ok) {
            msg = error.isEmpty() ? i18n("Kate AI: that sub-agent has no isolated patch.") : error;
            return false;
        }
        msg = sub == u"apply-subtask"_s
            ? i18n("Kate AI: applied the patch from %1.", rest)
            : i18n("Kate AI: discarded the worktree from %1.", rest);
        return true;
    }
    if (sub == u"verify"_s) {
        target->chat()->showVerificationReport();
        return true;
    }
    if (sub == u"policy"_s) {
        target->chat()->showPolicyReport();
        return true;
    }
    if (sub == u"hooks"_s) {
        target->chat()->showHooksReport();
        return true;
    }
    if (sub == u"trust-hooks"_s) {
        if (!target->chat()->agent()->trustProjectHooks()) {
            msg = i18n("Kate AI: no project hooks to trust (or the hooks flag is off).");
            return false;
        }
        msg = i18n("Kate AI: project hooks trusted.");
        return true;
    }
    if (sub == u"trace"_s) {
        target->chat()->showTraceReport();
        return true;
    }
    if (sub == u"stop"_s) {
        target->chat()->agent()->abort();
        msg = i18n("Kate AI: stopped.");
        return true;
    }
    if (sub == u"new"_s || sub == u"newchat"_s) {
        target->newChat();
        msg = i18n("Kate AI: new chat.");
        return true;
    }
    if (sub == u"fork"_s) {
        target->chat()->forkConversation();
        msg = i18n("Kate AI: forked the conversation.");
        return true;
    }
    if (sub == u"rewind"_s) {
        const QList<CheckpointInfo> checkpoints = target->chat()->agent()->checkpoints();
        if (checkpoints.isEmpty()) {
            msg = i18n("Kate AI: there is no checkpoint to rewind to.");
            return false;
        }
        QString id = rest;
        if (id.isEmpty()) {
            id = checkpoints.first().id;
        } else {
            for (const CheckpointInfo &checkpoint : checkpoints) {
                if (checkpoint.id.startsWith(id)) {
                    id = checkpoint.id;
                    break;
                }
            }
        }
        QString error;
        if (!target->chat()->agent()->restoreCheckpoint(id, &error)) {
            msg = error.isEmpty() ? i18n("Kate AI: could not restore the checkpoint.") : error;
            return false;
        }
        msg = i18n("Kate AI: restored checkpoint files %1. The conversation is unchanged.", id.left(8));
        return true;
    }
    if (sub == u"panel"_s || sub == u"show"_s) {
        target->showPanel();
        return true;
    }
    if (sub == u"mode"_s) {
        if (rest.isEmpty()) {
            msg = i18n("usage: kateai mode <id>");
            return false;
        }
        target->chat()->applyMode(rest);
        return true;
    }
    if (sub == u"debug"_s || sub == u"explain-stop"_s) {
        target->diagnoseDebugStop();
        msg = i18n("Kate AI: diagnosing debugger stop.");
        return true;
    }
    if (sub == u"stack"_s) {
        target->explainDebugStack();
        msg = i18n("Kate AI: analyzing call stack.");
        return true;
    }
    if (sub == u"break"_s) {
        QString bpMsg;
        const bool ok = target->setBreakpointAtCursor(rest, &bpMsg);
        msg = bpMsg.isEmpty() ? (ok ? i18n("Breakpoint updated.") : i18n("Could not set breakpoint.")) : bpMsg;
        return ok;
    }
    if (sub == u"snapshot"_s) {
        target->snapshotDebugState();
        msg = i18n("Kate AI: captured debugger snapshot.");
        return true;
    }
    if (sub == u"fix-errors"_s) {
        target->fixDiagnostics();
        msg = i18n("Kate AI: fixing diagnostics.");
        return true;
    }
    if (sub == u"build-errors"_s) {
        target->fixBuildErrors();
        msg = i18n("Kate AI: fixing build errors.");
        return true;
    }
    if (sub == u"launch-config"_s) {
        target->generateLaunchJson();
        msg = i18n("Kate AI: generated .vscode/launch.json.");
        return true;
    }
    if (sub == u"backtrace"_s) {
        target->analyzeBacktrace();
        msg = i18n("Kate AI: analyzing backtrace.");
        return true;
    }
    if (sub == u"terminal"_s) {
        target->explainTerminalSelection();
        msg = i18n("Kate AI: analyzing terminal selection.");
        return true;
    }
    if (sub == u"blame"_s) {
        target->explainGitBlame();
        msg = i18n("Kate AI: explaining line history.");
        return true;
    }
    if (sub == u"commit"_s) {
        target->generateCommitMessage();
        msg = i18n("Kate AI: drafting commit message.");
        return true;
    }
    if (sub == u"diff-tab"_s) {
        target->openReviewDiffTab();
        msg = i18n("Kate AI: opened review diff tab.");
        return true;
    }
    if (sub == u"graph-tab"_s) {
        target->openProjectGraphTab();
        msg = i18n("Kate AI: opened project graph tab.");
        return true;
    }
    if (sub == u"annotations"_s) {
        target->toggleAnnotationGutter();
        msg = i18n("Kate AI: toggled AI edit gutter annotations.");
        return true;
    }
    if (sub == u"voice"_s) {
        target->toggleVoicePrompt();
        msg = i18n("Kate AI: toggled voice prompt.");
        return true;
    }

    // Everything else sends a prompt. The one-word subcommands carry a fixed
    // instruction and act on the selection, the command line's range, or the
    // current line, exactly like the editor context menu.
    QString instruction;
    QString question;
    if (sub.isEmpty() || sub == u"ask"_s) {
        instruction = QString();
        question = rest;
        if (sub.isEmpty()) {
            // A bare "kateai" with a range explains it, like a one-word
            // subcommand would; without a range there is nothing to do.
            if (!range.isValid() || range.isEmpty() || !view) {
                msg = usageText();
                return false;
            }
            instruction = instructionFor(u"explain"_s);
        }
    } else {
        instruction = instructionFor(sub);
        if (instruction.isEmpty()) {
            msg = i18n("Kate AI: unknown subcommand.\n%1", usageText());
            return false;
        }
    }

    if (!view || !view->document()) {
        msg = i18n("Kate AI: no document to take the selection from.");
        return false;
    }

    // The snippet: the command line's range wins, then the selection, then
    // the current line -- the same order the context menu actions use.
    QString snippet;
    if (range.isValid() && !range.isEmpty()) {
        snippet = view->document()->text(range);
    }
    if (snippet.isEmpty() && view->selection()) {
        snippet = view->selectionText();
    }
    if (snippet.isEmpty()) {
        snippet = view->document()->line(view->cursorPosition().line());
    }

    const QString path = view->document()->url().toLocalFile();
    const QString name = path.isEmpty() ? view->document()->documentName() : path;
    QString prompt;
    if (question.isEmpty()) {
        prompt = i18n("%1\n\nFile: %2\n\nCode:\n%3", instruction, name, snippet);
    } else if (snippet.isEmpty()) {
        prompt = question;
    } else {
        prompt = i18n("%1\n\nFile: %2\n\nCode:\n%3", question, name, snippet);
    }

    target->showPanel();
    target->chat()->ask(prompt);
    msg = i18n("Kate AI: prompt sent.");
    return true;
}

} // namespace KateAi

#include "kateaicommand.moc"
