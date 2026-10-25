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
    return i18n("usage: kateai ask <text> | explain | fix | refactor | tests | stop | new | mode <id> | panel");
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
