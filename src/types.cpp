/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "modes.h"
#include "types.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStandardPaths>
#include <KLocalizedString>

#include <algorithm>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

static QHash<QString, QString> buildToolAliasMap()
{
    QHash<QString, QString> map;
    // ACP filesystem tools
    map[u"fs/read_text_file"_s] = u"read_file"_s;
    map[u"fs/read_file"_s] = u"read_file"_s;
    map[u"view_file"_s] = u"read_file"_s;
    map[u"client_view_file"_s] = u"read_file"_s;
    map[u"client_create_file"_s] = u"write_file"_s;
    map[u"client_edit_file"_s] = u"edit_file"_s;
    map[u"view_file_outline"_s] = u"read_file"_s;
    map[u"view_code_item"_s] = u"read_file"_s;
    map[u"read_resource"_s] = u"read_file"_s;
    map[u"fs/write_text_file"_s] = u"write_file"_s;
    map[u"fs/write_file"_s] = u"write_file"_s;
    map[u"fs/append_file"_s] = u"write_file"_s;
    map[u"write_to_file"_s] = u"write_file"_s;
    map[u"create_file"_s] = u"write_file"_s;
    map[u"fs/edit_file"_s] = u"edit_file"_s;
    map[u"fs/str_replace_editor"_s] = u"edit_file"_s;
    map[u"str_replace_editor"_s] = u"edit_file"_s;
    map[u"str_replace"_s] = u"edit_file"_s;
    map[u"replace"_s] = u"edit_file"_s;
    map[u"replace_file_content"_s] = u"edit_file"_s;
    map[u"notebook_edit"_s] = u"edit_file"_s;
    map[u"NotebookEdit"_s] = u"edit_file"_s;
    map[u"edit"_s] = u"edit_file"_s;
    map[u"multi_edit"_s] = u"multi_edit_file"_s;
    map[u"multi_edit_file"_s] = u"multi_edit_file"_s;
    map[u"fs/multi_edit_file"_s] = u"multi_edit_file"_s;
    map[u"multi_replace_file_content"_s] = u"multi_edit_file"_s;
    // apply_patch is its own tool (multi-file unified diff), not a single-file
    // edit: canonicalizing it to edit_file mangles multi-file patches.
    map[u"patch"_s] = u"apply_patch"_s;
    map[u"apply_patch"_s] = u"apply_patch"_s;
    map[u"Patch"_s] = u"apply_patch"_s;
    map[u"UpdateFile"_s] = u"apply_patch"_s;
    // ACP terminal/shell
    map[u"terminal/create"_s] = u"bash"_s;
    map[u"terminal/exec"_s] = u"bash"_s;
    map[u"terminal/run"_s] = u"bash"_s;
    map[u"terminal/send_input"_s] = u"bash"_s;
    map[u"shell/exec"_s] = u"bash"_s;
    map[u"shell"_s] = u"bash"_s;
    map[u"Shell"_s] = u"bash"_s;
    map[u"execute"_s] = u"bash"_s;
    map[u"Exec"_s] = u"bash"_s;
    map[u"run_command"_s] = u"bash"_s;
    map[u"RunCommand"_s] = u"bash"_s;
    map[u"run_shell_command"_s] = u"bash"_s;
    map[u"bash_command"_s] = u"bash"_s;
    map[u"send_command_input"_s] = u"bash"_s;
    // Directory/listing/search
    map[u"fs/list_directory"_s] = u"list_dir"_s;
    map[u"list_directory"_s] = u"list_dir"_s;
    map[u"dir_list"_s] = u"list_dir"_s;
    map[u"readdir"_s] = u"list_dir"_s;
    map[u"Read"_s] = u"read_file"_s;
    map[u"View"_s] = u"read_file"_s;
    map[u"LS"_s] = u"list_dir"_s;
    map[u"Glob"_s] = u"glob"_s;
    map[u"Grep"_s] = u"grep"_s;
    map[u"fs/grep"_s] = u"grep"_s;
    map[u"grep_search"_s] = u"grep"_s;
    map[u"GrepSearch"_s] = u"grep"_s;
    map[u"search_files"_s] = u"grep"_s;
    map[u"search_in_files"_s] = u"grep"_s;
    map[u"codebase_search"_s] = u"grep"_s;
    map[u"ripgrep"_s] = u"grep"_s;
    map[u"rg"_s] = u"grep"_s;
    map[u"fs/glob"_s] = u"glob"_s;
    map[u"find_files"_s] = u"glob"_s;
    map[u"find_by_name"_s] = u"glob"_s;
    map[u"FindByName"_s] = u"glob"_s;
    map[u"glob_files"_s] = u"glob"_s;
    map[u"list_files"_s] = u"glob"_s;
    // Web/search/planning tools that may appear under different names
    map[u"web/search"_s] = u"web_search"_s;
    map[u"search_web"_s] = u"web_search"_s;
    map[u"WebSearch"_s] = u"web_search"_s;
    map[u"google_search"_s] = u"web_search"_s;
    map[u"web/fetch"_s] = u"web_fetch"_s;
    map[u"WebFetch"_s] = u"web_fetch"_s;
    map[u"read_url_content"_s] = u"web_fetch"_s;
    map[u"fetch_url"_s] = u"web_fetch"_s;
    map[u"url_fetch"_s] = u"web_fetch"_s;
    map[u"plan/write"_s] = u"write_plan_file"_s;
    map[u"project/query"_s] = u"query_project_graph"_s;
    map[u"suggest"_s] = u"suggest"_s;
    map[u"suggestion"_s] = u"suggest"_s;
    map[u"suggest_change"_s] = u"suggest"_s;
    map[u"suggest_edit"_s] = u"suggest"_s;
    map[u"suggest_improvement"_s] = u"suggest"_s;
    map[u"propose"_s] = u"suggest"_s;
    map[u"advice"_s] = u"suggest"_s;
    map[u"question"_s] = u"question"_s;
    // Agents ask the user under several names; all park the turn the same way.
    map[u"ask_user"_s] = u"question"_s;
    map[u"ask_user_question"_s] = u"question"_s;
    map[u"ask_question"_s] = u"question"_s;
    map[u"ask_user_input"_s] = u"question"_s;
    map[u"request_user_input"_s] = u"question"_s;
    map[u"user_question"_s] = u"question"_s;
    map[u"prompt_user"_s] = u"question"_s;
    map[u"ask_human"_s] = u"question"_s;
    map[u"AskUserQuestion"_s] = u"question"_s;
    // Structured todos (Claude Code-style) and plan-mode exit.
    map[u"todo_write"_s] = u"todo_write"_s;
    map[u"TodoWrite"_s] = u"todo_write"_s;
    map[u"todo_read"_s] = u"todo_read"_s;
    map[u"TodoRead"_s] = u"todo_read"_s;
    map[u"exit_plan_mode"_s] = u"exit_plan_mode"_s;
    map[u"ExitPlanMode"_s] = u"exit_plan_mode"_s;
    map[u"think"_s] = u"think"_s;
    map[u"Think"_s] = u"think"_s;
    map[u"move_file"_s] = u"edit_file"_s;
    map[u"rename_file"_s] = u"edit_file"_s;
    map[u"delete_file"_s] = u"edit_file"_s;
    map[u"remove_file"_s] = u"edit_file"_s;
    map[u"fs/delete_file"_s] = u"edit_file"_s;
    map[u"fs/remove_file"_s] = u"edit_file"_s;
    map[u"fs/move_file"_s] = u"edit_file"_s;
    map[u"fs/rename_file"_s] = u"edit_file"_s;
    // The ACP tool kinds and the short names agents use for the same work.
    // Without these a kind-only call ("read") is not the read tool the modes
    // allow, and plan mode rejects it.
    map[u"read"_s] = u"read_file"_s;
    map[u"write"_s] = u"write_file"_s;
    map[u"list"_s] = u"list_dir"_s;
    map[u"ls"_s] = u"list_dir"_s;
    map[u"search"_s] = u"grep"_s;
    map[u"fetch"_s] = u"web_fetch"_s;
    map[u"webfetch"_s] = u"web_fetch"_s;
    map[u"websearch"_s] = u"web_search"_s;
    map[u"think_kind"_s] = u"think"_s;
    // New task/subagent
    map[u"agent/new_task"_s] = subtaskToolName();
    map[u"subagent"_s] = subtaskToolName();
    map[u"task"_s] = subtaskToolName();
    map[u"invoke_subagent"_s] = subtaskToolName();
    map[u"dispatch_agent"_s] = subtaskToolName();
    map[u"spawn_agent"_s] = subtaskToolName();
    return map;
}

static const QHash<QString, QString> &toolAliasMap()
{
    static const QHash<QString, QString> map = buildToolAliasMap();
    return map;
}

QString canonicalToolName(const QString &name)
{
    if (name.isEmpty()) {
        return name;
    }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed;
    }
    // MCP tools keep their names as-is (prefixed)
    if (trimmed.startsWith(u"mcp__"_s)) {
        return trimmed;
    }
    // Check exact aliases
    const auto &map = toolAliasMap();
    if (map.contains(trimmed)) {
        return map.value(trimmed);
    }
    // Case-insensitive fallback
    const QString lower = trimmed.toLower();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.key().toLower() == lower) {
            return it.value();
        }
    }
    // A built-in tool named in another case ("Grep", "BASH") is that tool.
    static const QHash<QString, QString> builtInByLower = [] {
        QHash<QString, QString> names;
        for (const QString &name : allBuiltInToolNames()) {
            names.insert(name.toLower(), name);
        }
        return names;
    }();
    const auto builtIn = builtInByLower.constFind(lower);
    if (builtIn != builtInByLower.constEnd()) {
        return builtIn.value();
    }
    return trimmed;
}

namespace
{

// The first non-empty string among `keys`. Objects are unwrapped one level so
// a provider that sends {"question":{"text":"..."}} still works.
QString firstTextValue(const QJsonObject &args, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = args.value(key);
        if (value.isString()) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty()) {
                return text;
            }
        } else if (value.isObject()) {
            const QString text = firstTextValue(value.toObject(),
                                                {u"text"_s, u"message"_s, u"description"_s, u"content"_s, u"label"_s});
            if (!text.isEmpty()) {
                return text;
            }
        }
    }
    return {};
}

QJsonObject questionSourceObject(const QJsonObject &args)
{
    const QJsonValue questions = args.value(u"questions"_s);
    if (questions.isArray()) {
        const QJsonArray array = questions.toArray();
        if (!array.isEmpty()) {
            if (array.first().isObject()) {
                return array.first().toObject();
            }
            if (array.first().isString()) {
                QJsonObject source;
                source.insert(u"question"_s, array.first().toString());
                return source;
            }
        }
    }
    const QJsonValue question = args.value(u"question"_s);
    if (question.isObject()) {
        return question.toObject();
    }
    return args;
}

void appendOptionValues(QStringList *out, const QJsonValue &value)
{
    if (!out) {
        return;
    }
    if (value.isString()) {
        const QString label = value.toString().trimmed();
        if (!label.isEmpty() && !out->contains(label)) {
            out->append(label);
        }
        return;
    }
    if (value.isObject()) {
        const QString label = firstTextValue(value.toObject(),
                                             {u"label"_s, u"title"_s, u"name"_s, u"text"_s, u"value"_s,
                                              u"description"_s});
        if (!label.isEmpty() && !out->contains(label)) {
            out->append(label);
        }
        return;
    }
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            appendOptionValues(out, item);
        }
    }
}

bool boolishJson(const QJsonValue &value, bool fallback)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == u"true"_s || text == u"1"_s || text == u"yes"_s) {
            return true;
        }
        if (text == u"false"_s || text == u"0"_s || text == u"no"_s) {
            return false;
        }
    }
    return fallback;
}

} // namespace

QString firstStringToolArgument(const QJsonObject &args, const QStringList &keys)
{
    return firstTextValue(args, keys);
}

QString questionTextFromToolArguments(const QJsonObject &args)
{
    const QJsonObject source = questionSourceObject(args);
    QString text = firstTextValue(source, {u"question"_s, u"prompt"_s, u"message"_s, u"text"_s,
                                          u"header"_s, u"title"_s, u"description"_s});
    if (text.isEmpty()) {
        text = firstTextValue(args, {u"question"_s, u"prompt"_s, u"message"_s, u"text"_s,
                                    u"header"_s, u"title"_s, u"description"_s});
    }
    return text;
}

QStringList questionOptionsFromToolArguments(const QJsonObject &args)
{
    QStringList options;
    const QJsonObject source = questionSourceObject(args);
    for (const QString &key : {u"options"_s, u"choices"_s, u"answers"_s}) {
        appendOptionValues(&options, source.value(key));
        appendOptionValues(&options, args.value(key));
    }
    return options;
}

bool questionAllowFreeformFromToolArguments(const QJsonObject &args, bool fallback)
{
    const QJsonObject source = questionSourceObject(args);
    if (source.contains(u"allow_freeform"_s)) {
        return boolishJson(source.value(u"allow_freeform"_s), fallback);
    }
    if (source.contains(u"allowFreeform"_s)) {
        return boolishJson(source.value(u"allowFreeform"_s), fallback);
    }
    if (args.contains(u"allow_freeform"_s)) {
        return boolishJson(args.value(u"allow_freeform"_s), fallback);
    }
    if (args.contains(u"allowFreeform"_s)) {
        return boolishJson(args.value(u"allowFreeform"_s), fallback);
    }
    return fallback;
}

QString suggestionTextFromToolArguments(const QJsonObject &args)
{
    QString text = firstTextValue(args, {u"suggestion"_s, u"message"_s, u"text"_s, u"description"_s,
                                        u"content"_s, u"advice"_s, u"recommendation"_s, u"title"_s,
                                        u"prompt"_s});
    if (!text.isEmpty()) {
        return text;
    }
    const QJsonValue suggestions = args.value(u"suggestions"_s);
    if (suggestions.isArray() && !suggestions.toArray().isEmpty()) {
        const QJsonValue first = suggestions.toArray().first();
        if (first.isString()) {
            return first.toString().trimmed();
        }
        if (first.isObject()) {
            return firstTextValue(first.toObject(),
                                  {u"suggestion"_s, u"text"_s, u"message"_s, u"description"_s,
                                   u"content"_s, u"advice"_s, u"recommendation"_s});
        }
    }
    return {};
}

namespace
{

// Strips the a/ or b/ prefix git-style diffs carry, and quotes around paths.
QString stripDiffPath(QString p)
{
    p = p.trimmed();
    if (p.startsWith(u'"') && p.endsWith(u'"') && p.size() >= 2) {
        p = p.mid(1, p.size() - 2);
    }
    if (p == u"/dev/null"_s) {
        return p;
    }
    // Strip a timestamp some tools append after a tab.
    const int tab = p.indexOf(u'\t');
    if (tab >= 0) {
        p = p.left(tab);
    }
    if ((p.startsWith(u"a/"_s) || p.startsWith(u"b/"_s)) && p.size() > 2) {
        p = p.mid(2);
    }
    return p.trimmed();
}

// Parses the "@@ -oldStart[,oldCount] +newStart[,newCount] @@" header.
bool parseHunkHeader(const QString &line, PatchHunk *hunk)
{
    static const QRegularExpression re(u"^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@"_s);
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch()) {
        return false;
    }
    hunk->oldStart = m.captured(1).toInt();
    hunk->oldCount = m.captured(2).isEmpty() ? 1 : m.captured(2).toInt();
    hunk->newStart = m.captured(3).toInt();
    hunk->newCount = m.captured(4).isEmpty() ? 1 : m.captured(4).toInt();
    return true;
}

} // namespace

QList<PatchFile> parseUnifiedPatch(const QString &patch)
{
    QList<PatchFile> files;
    const QStringList lines = patch.split(u'\n');
    PatchFile *current = nullptr;
    PatchHunk *hunk = nullptr;
    bool inHunk = false;
    // Number of old/new lines still expected in the current hunk; when both
    // reach zero the hunk is complete and the next @@ or --- starts a new one.
    int oldLeft = 0;
    int newLeft = 0;

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        // A new file header is a "--- path" immediately followed by
        // "+++ path". Inside a hunk, that pair is the only reliable way to
        // tell it from a removed line whose content starts with "-- " (which
        // is encoded as "--- ...").
        const bool headerPair = line.startsWith(u"--- "_s) && i + 1 < lines.size()
            && lines.at(i + 1).startsWith(u"+++ "_s);
        // A hunk is complete once both declared counts are consumed. Close it
        // before looking at headers, so the next "--- file" is a new file
        // section rather than one more (zero-length) hunk line. A trailing
        // "\ No newline at end of file" marker still belongs to the hunk, so
        // it keeps the hunk open for one more line.
        if (inHunk && oldLeft <= 0 && newLeft <= 0 && !line.startsWith(u'\\')) {
            inHunk = false;
            hunk = nullptr;
        }
        // A removed line whose content begins with "-- " is encoded as
        // "--- ...", and an added line whose content begins with "++ " as
        // "+++ ...". Inside a hunk they are content, not file headers unless
        // the following line is the matching "+++".
        if ((!inHunk || headerPair) && line.startsWith(u"--- "_s)) {
            // A new file section begins. Hunks of the previous file end here.
            inHunk = false;
            hunk = nullptr;
            PatchFile file;
            file.oldPath = stripDiffPath(line.mid(4));
            if (file.oldPath == u"/dev/null"_s) {
                file.isNew = true;
            }
            files.append(file);
            current = &files.last();
            continue;
        }
        if (!current) {
            continue;
        }
        if (!inHunk && line.startsWith(u"+++ "_s)) {
            current->path = stripDiffPath(line.mid(4));
            if (current->path == u"/dev/null"_s) {
                current->isDelete = true;
                current->path = current->oldPath;
            }
            if (current->path.isEmpty()) {
                current->path = current->oldPath;
            }
            inHunk = false;
            hunk = nullptr;
            continue;
        }
        if (line.startsWith(u"diff --git"_s) || line.startsWith(u"index "_s) || line.startsWith(u"new file mode"_s)
            || line.startsWith(u"deleted file mode"_s) || line.startsWith(u"similarity index"_s)
            || line.startsWith(u"rename from"_s) || line.startsWith(u"rename to"_s)) {
            inHunk = false;
            hunk = nullptr;
            continue;
        }
        if (line.startsWith(u"@@"_s)) {
            PatchHunk h;
            if (!parseHunkHeader(line, &h)) {
                continue;
            }
            current->hunks.append(h);
            hunk = &current->hunks.last();
            oldLeft = h.oldCount;
            newLeft = h.newCount;
            inHunk = true;
            continue;
        }
        if (!inHunk || !hunk) {
            continue;
        }
        if (line.startsWith(u'\\')) {
            // "\ No newline at end of file" belongs to the preceding line.
            hunk->lines.append(line);
            continue;
        }
        if (line.startsWith(u'-')) {
            if (oldLeft <= 0) {
                inHunk = false;
                hunk = nullptr;
                continue;
            }
            hunk->lines.append(line);
            --oldLeft;
            continue;
        }
        if (line.startsWith(u'+')) {
            if (newLeft <= 0) {
                inHunk = false;
                hunk = nullptr;
                continue;
            }
            hunk->lines.append(line);
            --newLeft;
            continue;
        }
        // Context line (or an empty line, which in a hunk means a context line
        // whose content is empty).
        if (oldLeft <= 0 && newLeft <= 0) {
            inHunk = false;
            hunk = nullptr;
            continue;
        }
        hunk->lines.append(line.isEmpty() ? u" "_s : line);
        --oldLeft;
        --newLeft;
    }
    // Drop file sections that carry no hunks and no clear target (prose that
    // merely looked like a diff header).
    QList<PatchFile> out;
    for (const PatchFile &file : files) {
        if (file.path.isEmpty() && file.hunks.isEmpty()) {
            continue;
        }
        out.append(file);
    }
    return out;
}

QString applyPatchToContent(const QString &content, const QList<PatchHunk> &hunks, QString *error)
{
    // A non-empty file with a trailing newline splits into a final empty
    // element; keeping it makes positions stable and re-joins identically. An
    // empty file uses the same sentinel so a hunk that adds lines produces a
    // file ending in a newline, which is what unified diff lines mean unless a
    // "\ No newline at end of file" marker says otherwise.
    QStringList out = content.isEmpty() ? QStringList{QString()} : content.split(u'\n');
    bool newSideHasNoFinalNewline = false;
    // Apply hunks from last to first so earlier positions stay valid.
    QList<PatchHunk> ordered = hunks;
    std::sort(ordered.begin(), ordered.end(), [](const PatchHunk &a, const PatchHunk &b) {
        return a.oldStart > b.oldStart;
    });
    for (const PatchHunk &hunk : ordered) {
        // Unified diffs write a zero-length old range as "<line before>,0".
        // Creating a file at the top uses "@@ -0,0", so oldStart 0 is valid
        // exactly when the hunk removes nothing. That insertion lands at 0.
        const bool emptyOldRange = hunk.oldStart == 0 && hunk.oldCount == 0;
        const int start = hunk.oldStart > 0 ? hunk.oldStart - 1 : 0;
        if ((hunk.oldStart < 1 && !emptyOldRange) || start < 0
            || hunk.oldCount < 0 || hunk.newCount < 0) {
            if (error) {
                *error = u"patch hunk has an invalid start line (%1)"_s.arg(hunk.oldStart);
            }
            return {};
        }
        // A model-supplied number can be INT_MAX. Do the range arithmetic
        // before subtracting/adding so the bounds check itself cannot wrap.
        if (start > out.size()) {
            if (error) {
                *error = u"patch hunk at line %1 is past the end of the file (%2 lines)"_s.arg(hunk.oldStart).arg(out.size());
            }
            return {};
        }
        // A trailing "\ No newline at end of file" marker applies to the line
        // before it. If that line survives in the new content, the result must
        // not end with a newline.
        if (!hunk.lines.isEmpty()) {
            const int last = hunk.lines.size() - 1;
            if (hunk.lines.at(last).startsWith(u'\\') && last > 0) {
                const QString &previousLine = hunk.lines.at(last - 1);
                if (!previousLine.isEmpty()) {
                    const QChar previous = previousLine.at(0);
                    if (previous == u'+' || previous == u' ') {
                        newSideHasNoFinalNewline = true;
                    } else if (previous == u'-') {
                        newSideHasNoFinalNewline = false;
                    }
                }
            }
        }
        // Verify the context/removed lines match the current content.
        QStringList oldLines;
        QStringList newLines;
        for (const QString &l : hunk.lines) {
            if (l.startsWith(u'\\')) {
                continue;
            }
            if (l.startsWith(u'-')) {
                oldLines.append(l.mid(1));
            } else if (l.startsWith(u'+')) {
                newLines.append(l.mid(1));
            } else {
                oldLines.append(l.mid(1));
                newLines.append(l.mid(1));
            }
        }
        // Verify the declared counts match what was collected. A truncated
        // stream or a hand-edited @@ count must fail here rather than apply a
        // partial hunk to the file.
        if (oldLines.size() != hunk.oldCount || newLines.size() != hunk.newCount) {
            if (error) {
                *error = u"patch hunk at line %1 declares %2 removed/%3 added line(s) but carries %4/%5"_s
                             .arg(hunk.oldStart).arg(hunk.oldCount).arg(hunk.newCount)
                             .arg(oldLines.size()).arg(newLines.size());
            }
            return {};
        }
        const qint64 start64 = start;
        const qint64 oldSize = oldLines.size();
        if (start64 > out.size() || oldSize > qint64(out.size()) - start64) {
            if (error) {
                *error = u"patch hunk at line %1 is past the end of the file (%2 lines)"_s.arg(hunk.oldStart).arg(out.size());
            }
            return {};
        }
        for (int i = 0; i < oldLines.size(); ++i) {
            if (out.at(start + i) != oldLines.at(i)) {
                if (error) {
                    *error = u"patch does not apply at line %1: expected %2, found %3"_s
                                 .arg(hunk.oldStart + i)
                                 .arg(oldLines.at(i), out.at(start + i));
                }
                return {};
            }
        }
        // Replace the old range with the new lines.
        out.erase(out.begin() + start, out.begin() + start + oldLines.size());
        for (int i = 0; i < newLines.size(); ++i) {
            out.insert(out.begin() + start + i, newLines.at(i));
        }
    }
    QString joined = out.join(u'\n');
    if (newSideHasNoFinalNewline && joined.endsWith(u'\n')) {
        joined.chop(1);
    }
    if (joined.isNull()) {
        // QStringList::join() returns a null string for an empty result, which
        // callers used to mistake for failure/deletion. An empty file is a
        // successful patch result; keep it non-null.
        return QStringLiteral("");
    }
    return joined;
}

QString resolvePath(const QString &path, const QString &workspace)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (QFileInfo(trimmed).isAbsolute() || workspace.trimmed().isEmpty()) {
        return QDir::cleanPath(trimmed);
    }
    return QDir::cleanPath(QDir(workspace).absoluteFilePath(trimmed));
}

QJsonObject normalizeRawInput(const QJsonValue &rawInput)
{
    if (rawInput.isObject()) {
        return rawInput.toObject();
    }
    if (rawInput.isArray()) {
        const QJsonArray arr = rawInput.toArray();
        bool isContent = false;
        for (const QJsonValue &v : arr) {
            if (v.isObject() && v.toObject().contains(u"type"_s)) {
                isContent = true;
                break;
            }
        }
        if (isContent) {
            return {};
        }
        QJsonObject merged;
        for (const QJsonValue &v : arr) {
            if (v.isObject()) {
                const QJsonObject o = v.toObject();
                for (auto it = o.begin(); it != o.end(); ++it) {
                    merged.insert(it.key(), it.value());
                }
            }
        }
        return merged;
    }
    if (rawInput.isString()) {
        const QJsonDocument doc = QJsonDocument::fromJson(rawInput.toString().toUtf8());
        if (doc.isObject()) {
            return doc.object();
        }
    }
    return {};
}

QStringList toolNameAliases(const QString &canonicalName)
{
    QStringList aliases;
    const auto &map = toolAliasMap();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.value() == canonicalName) {
            aliases.append(it.key());
        }
    }
    // Also add canonical name itself
    if (!aliases.contains(canonicalName)) {
        aliases.append(canonicalName);
    }
    return aliases;
}

QString providerId(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return u"openai"_s;
    case Provider::OpenRouter:
        return u"openrouter"_s;
    case Provider::DeepSeek:
        return u"deepseek"_s;
    case Provider::OpenAICompatible:
        return u"openai-compatible"_s;
    case Provider::ClaudeCompatible:
        return u"claude-compatible"_s;
    case Provider::Kilo:
        return u"kilo"_s;
    case Provider::Acp:
        return u"acp"_s;
    case Provider::OpenCode:
        return u"opencode"_s;
    case Provider::Telegram:
        return u"telegram"_s;
    case Provider::Grok:
    default:
        return u"grok"_s;
    }
}

QString providerLabel(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return i18n("OpenAI");
    case Provider::OpenRouter:
        return i18n("OpenRouter");
    case Provider::DeepSeek:
        return i18n("DeepSeek");
    case Provider::OpenAICompatible:
        return i18n("OpenAI Compatible");
    case Provider::ClaudeCompatible:
        return i18n("Claude Compatible");
    case Provider::Kilo:
        return i18n("Kilo.ai");
    case Provider::Acp:
        return i18n("ACP");
    case Provider::OpenCode:
        return i18n("OpenCode Zen");
    case Provider::Grok:
    default:
        return i18n("Grok (xAI)");
    }
}

Provider providerFromId(const QString &id)
{
    if (id == u"openai"_s) {
        return Provider::OpenAI;
    }
    if (id == u"openrouter"_s) {
        return Provider::OpenRouter;
    }
    if (id == u"deepseek"_s) {
        return Provider::DeepSeek;
    }
    if (id == u"openai-compatible"_s) {
        return Provider::OpenAICompatible;
    }
    if (id == u"claude-compatible"_s) {
        return Provider::ClaudeCompatible;
    }
    if (id == u"kilo"_s) {
        return Provider::Kilo;
    }
    if (id == u"acp"_s) {
        return Provider::Acp;
    }
    if (id == u"opencode"_s) {
        return Provider::OpenCode;
    }
    if (id == u"telegram"_s) {
        return Provider::Telegram;
    }
    return Provider::Grok;
}

QString providerBaseUrl(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
        return u"https://api.openai.com/v1"_s;
    case Provider::OpenRouter:
        return u"https://openrouter.ai/api/v1"_s;
    case Provider::DeepSeek:
        return u"https://api.deepseek.com"_s;
    case Provider::OpenAICompatible:
        return u"http://localhost:11434/v1"_s;
    case Provider::ClaudeCompatible:
        return u"https://api.anthropic.com/v1"_s;
    case Provider::Kilo:
        return u"https://api.kilo.ai/v1"_s;
    case Provider::Acp:
        return u"http://localhost:8080"_s;
    case Provider::OpenCode:
        return u"https://opencode.ai/zen/v1"_s;
    case Provider::Grok:
    default:
        return u"https://api.x.ai/v1"_s;
    }
}

QString providerBaseUrl(const Settings &settings)
{
    switch (settings.provider) {
    case Provider::OpenAI:
        return u"https://api.openai.com/v1"_s;
    case Provider::OpenRouter:
        return u"https://openrouter.ai/api/v1"_s;
    case Provider::DeepSeek:
        return settings.deepseekUrl.isEmpty() ? u"https://api.deepseek.com"_s : settings.deepseekUrl;
    case Provider::OpenAICompatible:
        // An empty base URL would build a malformed request URL, so the
        // provider default is used when the field was cleared.
        return settings.openaiCompatibleUrl.isEmpty() ? u"https://api.openai.com/v1"_s : settings.openaiCompatibleUrl;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleUrl.isEmpty() ? u"https://api.anthropic.com/v1"_s : settings.claudeCompatibleUrl;
    case Provider::OpenCode:
        return settings.opencodeUrl.isEmpty() ? u"https://opencode.ai/zen/v1"_s : settings.opencodeUrl;
    case Provider::Kilo:
        return u"https://api.kilo.ai/v1"_s;
    case Provider::Acp:
        return settings.acpUrl;
    case Provider::Grok:
    default:
        return u"https://api.x.ai/v1"_s;
    }
}

QString joinApiEndpoint(const QString &baseUrl, const QString &endpoint)
{
    QString base = baseUrl.trimmed();
    QString path = endpoint;
    while (base.endsWith(u'/')) {
        base.chop(1);
    }
    // A base that already ends in /v1 and an endpoint that starts with /v1/
    // name the same version segment; keep one copy. This covers the default
    // Anthropic base (https://api.anthropic.com/v1) and a user who entered a
    // bare host with an endpoint that includes /v1.
    if (path.startsWith(u"/v1/"_s) && base.endsWith(u"/v1"_s)) {
        path = path.mid(3);
    }
    if (!path.startsWith(u'/')) {
        path.prepend(u'/');
    }
    return base + path;
}

QStringList defaultModels(Provider provider)
{
    // Intentionally empty. Model names used to be hard-coded here, which went
    // stale the moment a provider shipped a model, and offered users models
    // their key cannot reach. The list now comes from the provider itself via
    // LlmClient::fetchModels(); this function remains so callers have one place
    // to ask, and it reports "nothing known yet" honestly.
    Q_UNUSED(provider)
    return {};
}

bool providerSupportsModelListing(Provider provider)
{
    switch (provider) {
    case Provider::OpenAI:
    case Provider::OpenRouter:
    case Provider::DeepSeek:
    case Provider::OpenAICompatible:
    case Provider::ClaudeCompatible:
    case Provider::Kilo:
    case Provider::Grok:
    case Provider::OpenCode:
        return true;
    case Provider::Acp:
    case Provider::Telegram:
        break;
    }
    // ACP agents and Telegram advertise themselves; there is no catalogue endpoint.
    return false;
}

bool usesAcpNative(const Settings &settings)
{
    return settings.provider == Provider::Acp && settings.apiFormat == ApiFormat::AcpNative;
}

bool providerRequiresApiKey(const Settings &settings, Provider provider)
{
    if (provider == Provider::Acp && settings.apiFormat == ApiFormat::AcpNative) {
        return false;
    }
    return true;
}

bool providerIsSelectable(const Settings &settings, Provider provider)
{
    if (provider == Provider::Acp) {
        return true;
    }
    Settings copy = settings;
    copy.provider = provider;
    return !apiKeyFor(copy).trimmed().isEmpty();
}

QString permissionModeId(PermissionMode mode)
{
    switch (mode) {
    case PermissionMode::AcceptEdits:
        return u"acceptEdits"_s;
    case PermissionMode::AlwaysApprove:
        return u"alwaysApprove"_s;
    case PermissionMode::Ask:
    default:
        return u"ask"_s;
    }
}

QString permissionModeLabel(PermissionMode mode)
{
    switch (mode) {
    case PermissionMode::AcceptEdits:
        return i18n("Accept edits");
    case PermissionMode::AlwaysApprove:
        return i18n("Always approve");
    case PermissionMode::Ask:
    default:
        return i18n("Ask");
    }
}

PermissionMode permissionModeFromId(const QString &id)
{
    if (id == u"acceptEdits"_s) {
        return PermissionMode::AcceptEdits;
    }
    if (id == u"alwaysApprove"_s || id == u"bypassPermissions"_s) {
        return PermissionMode::AlwaysApprove;
    }
    return PermissionMode::Ask;
}

QString sandboxProfileId(SandboxProfile profile)
{
    switch (profile) {
    case SandboxProfile::Off:
        return u"off"_s;
    case SandboxProfile::ReadOnly:
        return u"read-only"_s;
    case SandboxProfile::Strict:
        return u"strict"_s;
    case SandboxProfile::Workspace:
    default:
        return u"workspace"_s;
    }
}

QString sandboxProfileLabel(SandboxProfile profile)
{
    switch (profile) {
    case SandboxProfile::Off:
        return i18n("Off");
    case SandboxProfile::ReadOnly:
        return i18n("Read-only");
    case SandboxProfile::Strict:
        return i18n("Strict");
    case SandboxProfile::Workspace:
    default:
        return i18n("Workspace");
    }
}

SandboxProfile sandboxProfileFromId(const QString &id)
{
    if (id == u"off"_s) {
        return SandboxProfile::Off;
    }
    if (id == u"read-only"_s || id == u"readonly"_s) {
        return SandboxProfile::ReadOnly;
    }
    if (id == u"strict"_s) {
        return SandboxProfile::Strict;
    }
    return SandboxProfile::Workspace;
}

QString apiKeyFor(const Settings &settings)
{
    switch (settings.provider) {
    case Provider::OpenAI:
        return settings.openaiApiKey;
    case Provider::OpenRouter:
        return settings.openrouterApiKey;
    case Provider::DeepSeek:
        return settings.deepseekApiKey;
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleApiKey;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleApiKey;
    case Provider::Kilo:
        return settings.kiloApiKey;
    case Provider::Acp:
        return settings.acpApiKey;
    case Provider::OpenCode:
        return settings.opencodeApiKey;
    case Provider::Grok:
    default:
        return settings.grokApiKey;
    }
}

QString modelFor(const Settings &settings)
{
    switch (settings.provider) {
    case Provider::OpenAI:
        return settings.openaiModel;
    case Provider::OpenRouter:
        return settings.openrouterModel;
    case Provider::DeepSeek:
        return settings.deepseekModel;
    case Provider::OpenAICompatible:
        return settings.openaiCompatibleModel;
    case Provider::ClaudeCompatible:
        return settings.claudeCompatibleModel;
    case Provider::Kilo:
        return settings.kiloModel;
    case Provider::Acp:
        return settings.acpModel;
    case Provider::OpenCode:
        return settings.opencodeModel;
    case Provider::Grok:
    default:
        return settings.grokModel;
    }
}

// True when the provider exposes a fill-in-the-middle completion endpoint
// (/completions with prompt markers): DeepSeek's documented FIM beta and the
// OpenAI-compatible local servers (vLLM, LM Studio, Ollama) that translate
// the generic markers. Everything else takes the chat-completions path.
bool providerSupportsFim(Provider provider)
{
    return provider == Provider::DeepSeek || provider == Provider::OpenAICompatible;
}

QStringList pickerModelsFor(const Settings &settings, Provider provider, const QStringList &catalog)
{
    QStringList models;
    for (const QString &model : catalog) {
        const QString trimmed = model.trimmed();
        if (!trimmed.isEmpty() && !models.contains(trimmed)) {
            models.append(trimmed);
        }
    }
    Settings slot = settings;
    slot.provider = provider;
    const QString configured = modelFor(slot).trimmed();
    if (!configured.isEmpty() && !models.contains(configured)) {
        models.prepend(configured);
    }
    return models;
}

static QJsonObject toolDef(const QString &name, const QString &description, const QJsonObject &properties, const QStringList &required, bool strict = false)
{
    QJsonObject fn;
    fn.insert(u"name"_s, name);
    fn.insert(u"description"_s, description);
    QJsonObject params;
    params.insert(u"type"_s, u"object"_s);
    params.insert(u"properties"_s, properties);
    QJsonArray req;
    for (const QString &item : required) {
        req.append(item);
    }
    params.insert(u"required"_s, req);
    fn.insert(u"parameters"_s, params);
    fn.insert(u"strict"_s, strict);
    QJsonObject tool;
    tool.insert(u"type"_s, u"function"_s);
    tool.insert(u"function"_s, fn);
    return tool;
}

QJsonArray toolDefinitions(const ToolAccess &access)
{
    QJsonArray tools;

    tools.append(toolDef(u"read_file"_s,
                         u"Read a UTF-8 text file. Use offset/limit (1-based lines) for large files."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"offset"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"First line to return (1-based). \"line\" is accepted as an alias."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Alias of offset, matching ACP's fs/read_text_file shape."_s}}},
                             {u"limit"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Maximum number of lines to return."_s}}},
                         },
                         {u"path"_s},
                         false));

    tools.append(toolDef(u"read_tool_result"_s,
                         u"Read a page of a tool output that was too large to keep in the conversation. "
                         u"The truncation marker in that output contains the id. Pages are character ranges; "
                         u"continue with the returned offset until the end."_s,
                         QJsonObject{
                             {u"id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The tool_result id from the truncation marker."_s}}},
                             {u"offset"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Character offset to start at (default 0)."_s}}},
                             {u"limit"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Maximum characters to return (default 20000, max 80000)."_s}}},
                         },
                         {u"id"_s},
                         false));

    tools.append(toolDef(u"write_file"_s,
                         u"Create or overwrite a UTF-8 text file with the given contents."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"content"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Full file contents to write."_s}}},
                         },
                         {u"path"_s, u"content"_s},
                         false));

    tools.append(toolDef(u"edit_file"_s,
                         u"Replace exact text in a file. old_string must match exactly, including whitespace. "
                         u"By default it must occur once; set replace_all to true to replace every occurrence."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"old_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Exact text to find."_s}}},
                             {u"new_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Replacement text."_s}}},
                             {u"replace_all"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, replace every occurrence instead of requiring a unique match."_s}}},
                         },
                         {u"path"_s, u"old_string"_s, u"new_string"_s},
                         false));


    QJsonObject chunkProperties{
        {u"old_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Exact text to find in the file."_s}}},
        {u"new_string"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Replacement text."_s}}},
        {u"replace_all"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, replace every occurrence instead of requiring a unique match."_s}}},
    };
    QJsonObject chunkItem{
        {u"type"_s, u"object"_s},
        {u"properties"_s, chunkProperties},
        {u"required"_s, QJsonArray{u"old_string"_s, u"new_string"_s}},
    };
    tools.append(toolDef(u"multi_edit_file"_s,
                         u"Perform multiple non-contiguous edits to the same file in a single tool call. "
                         u"Each edit specifies an exact old_string to replace with new_string."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Path relative to the workspace or absolute."_s}}},
                             {u"edits"_s, QJsonObject{{u"type"_s, u"array"_s}, {u"items"_s, chunkItem}, {u"description"_s, u"Array of edit chunks to apply in order."_s}}},
                         },
                         {u"path"_s, u"edits"_s},
                         false));

    tools.append(toolDef(u"apply_patch"_s,
                         u"Apply a unified diff (git-style, possibly covering several files) in one call. "
                         u"Use this for multi-file changes instead of many edit_file calls. The patch must apply "
                         u"cleanly at the recorded line numbers; the whole patch is atomic - if any hunk fails, "
                         u"no file is changed."_s,
                         QJsonObject{
                             {u"patch"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The unified diff, with --- a/<path> / +++ b/<path> headers and @@ hunks."_s}}},
                         },
                         {u"patch"_s},
                         false));

    tools.append(toolDef(u"list_dir"_s,
                         u"List files and directories in a folder."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Directory to list. Defaults to the workspace root."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"grep"_s,
                         u"Search file contents with a regular expression. Returns path:line:content."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Regular expression to search for."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File or directory to search. Defaults to the workspace."_s}}},
                             {u"glob"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional filename glob such as *.cpp."_s}}},
                             {u"case_insensitive"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, match without regard to case."_s}}},
                             {u"context"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Number of context lines to include before and after each match."_s}}},
                         },
                         {u"pattern"_s},
                         false));

    tools.append(toolDef(u"glob"_s,
                         u"Find files whose paths match a glob pattern."_s,
                         QJsonObject{
                             {u"pattern"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Glob such as **/*.h or src/**/*.cpp."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional directory to search under. Defaults to the workspace root."_s}}},
                         },
                         {u"pattern"_s},
                         false));

    tools.append(toolDef(u"bash"_s,
                         u"Run a shell command in the workspace. Commands are sandboxed according to the active profile."_s,
                         QJsonObject{
                             {u"command"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Shell command to execute."_s}}},
                         },
                         {u"command"_s},
                         false));


    tools.append(toolDef(u"query_project_graph"_s,
                         u"Query the indexed project graph: files, symbols, imports, and relationships. "
                         u"Use this before blindly searching when you need structure or dependencies."_s,
                         QJsonObject{
                             {u"query_type"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"summary, nodes, edges, dependencies, dependents, find_related, find_path, dependency_chain"_s}}},
                             {u"node_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Node id, path, or name for node-scoped queries"_s}}},
                             {u"relationship"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional relationship filter: imports, calls, extends, contains, references"_s}}},
                             {u"source_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Source node for path finding"_s}}},
                             {u"target_id"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Target node for path finding"_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"lsp_diagnostics"_s,
                         u"Report language-server/editor diagnostics (errors, warnings) for one file, or workspace-wide when path is omitted. "
                         u"Read-only. Check this after edits instead of only reading test output. When no LSP server is running, "
                         u"the tool says so and you should fall back to a build/lint via bash."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File to check, relative to the workspace or absolute. Omit for a workspace-wide summary."_s}}},
                             {u"severity"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"enum"_s, QJsonArray{u"error"_s, u"warning"_s, u"info"_s, u"hint"_s}}, {u"description"_s, u"Only report this severity and above. Omit for everything."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"lsp_symbols"_s,
                         u"List the symbols (classes, functions) of a file with their line numbers. Read-only."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File to list, relative to the workspace or absolute."_s}}},
                         },
                         {u"path"_s},
                         false));

    tools.append(toolDef(u"lsp_hover"_s,
                         u"Show hover information for the symbol at a position: the enclosing symbol and its declaration. Read-only."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File, relative to the workspace or absolute."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based line of the symbol."_s}}},
                             {u"column"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based column. Defaults to 1."_s}}},
                         },
                         {u"path"_s, u"line"_s},
                         false));

    tools.append(toolDef(u"lsp_definition"_s,
                         u"Find where the symbol at a position is defined. Read-only."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File, relative to the workspace or absolute."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based line of the symbol."_s}}},
                             {u"column"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based column. Defaults to 1."_s}}},
                             {u"symbol"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Override the symbol name instead of taking it from the position."_s}}},
                         },
                         {u"path"_s, u"line"_s},
                         false));

    tools.append(toolDef(u"lsp_references"_s,
                         u"Find all references to the symbol at a position across the workspace. Read-only."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File, relative to the workspace or absolute."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based line of the symbol."_s}}},
                             {u"column"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based column. Defaults to 1."_s}}},
                             {u"symbol"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Override the symbol name instead of taking it from the position."_s}}},
                             {u"include_declaration"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"Include the declaration itself. Defaults to true."_s}}},
                         },
                         {u"path"_s, u"line"_s},
                         false));

    tools.append(toolDef(u"lsp_code_actions"_s,
                         u"List the quick fixes / code actions the language server offers at a position (e.g. on a diagnostic line). Read-only."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File, relative to the workspace or absolute."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based line. Defaults to 1."_s}}},
                             {u"column"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based column. Defaults to 1."_s}}},
                         },
                         {u"path"_s},
                         false));

    tools.append(toolDef(u"debug_status"_s,
                         u"Inspect the live Kate debugger (DAP/GDB/LLDB) state: session status (idle/running/paused/terminated), active target, stop reason/location, threads, and configured targets. Read-only."_s,
                         QJsonObject{
                             {u"include_output"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"Include recent debugger console/IO output. Defaults to false."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"debug_stack"_s,
                         u"Read the call stack frames of the paused debuggee (or a specific thread) with function names, files, and 1-based line numbers. Read-only."_s,
                         QJsonObject{
                             {u"thread_id"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Optional thread id. Omit for the active stopped thread."_s}}},
                             {u"max_frames"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Maximum frames to return (default 40)."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"debug_locals"_s,
                         u"Read local variables, types, values, and children in the active (or specified) stack frame while paused. Read-only."_s,
                         QJsonObject{
                             {u"frame_level"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Optional 0-based frame level. Omit for the active frame."_s}}},
                             {u"filter"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional substring filter on variable name."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"debug_breakpoints"_s,
                         u"List all configured debugger breakpoints (file, line, condition, hit count, logpoint message, enabled/verified). Read-only."_s,
                         QJsonObject{},
                         {},
                         false));

    tools.append(toolDef(u"debug_select_frame"_s,
                         u"Select a stack frame (0 = innermost) to inspect its local variables and source context. Non-mutating inspection."_s,
                         QJsonObject{
                             {u"frame_level"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"0-based stack frame level to select."_s}}},
                         },
                         {u"frame_level"_s},
                         false));

    tools.append(toolDef(u"debug_select_thread"_s,
                         u"Select a thread by id to inspect its call stack and local variables. Non-mutating inspection."_s,
                         QJsonObject{
                             {u"thread_id"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Thread id from debug_status."_s}}},
                         },
                         {u"thread_id"_s},
                         false));

    tools.append(toolDef(u"debug_snapshot"_s,
                         u"Capture a snapshot of the current paused debugger state (stop location, call stack, locals) or diff two captured snapshots to compare a working stop against a failing stop. Read-only."_s,
                         QJsonObject{
                             {u"action"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"enum"_s, QJsonArray{u"capture"_s, u"list"_s, u"diff"_s, u"clear"_s}}, {u"description"_s, u"capture (default), list, diff, or clear."_s}}},
                             {u"label"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional label when capturing a snapshot (e.g. 'before_fix' or 'iter_1')."_s}}},
                             {u"first"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based index of the first snapshot for diff (defaults to second-to-last)."_s}}},
                             {u"second"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based index of the second snapshot for diff (defaults to latest)."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"debug_set_breakpoint"_s,
                         u"Set or remove a breakpoint (line, conditional, hit-count, or logpoint) in a workspace source file, or clear all breakpoints."_s,
                         QJsonObject{
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Workspace file path (relative or absolute). Required unless clear_all is true."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based line number. Required unless clear_all is true."_s}}},
                             {u"condition"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional boolean expression condition for a conditional breakpoint."_s}}},
                             {u"hit_condition"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional hit count condition."_s}}},
                             {u"log_message"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional logpoint message (logs without stopping)."_s}}},
                             {u"remove"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, remove the breakpoint at path:line instead of setting it."_s}}},
                             {u"clear_all"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, clear all breakpoints."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"debug_step"_s,
                         u"Control execution of the debuggee (continue, step_over, step_into, step_out, run_to_cursor, pause) and wait for the next stop event."_s,
                         QJsonObject{
                             {u"action"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"enum"_s, QJsonArray{u"continue"_s, u"step_over"_s, u"step_into"_s, u"step_out"_s, u"run_to_cursor"_s, u"pause"_s}}, {u"description"_s, u"Execution step action."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"File path for run_to_cursor."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"1-based line for run_to_cursor."_s}}},
                             {u"timeout_ms"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Max ms to wait for stop (default 15000)."_s}}},
                         },
                         {u"action"_s},
                         false));

    tools.append(toolDef(u"debug_eval"_s,
                         u"Evaluate an expression in the paused debugger's active stack frame. Note: arbitrary expressions may call target functions and have side effects."_s,
                         QJsonObject{
                             {u"expression"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Expression to evaluate in the active frame."_s}}},
                             {u"timeout_ms"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Max ms to wait for evaluation (default 5000)."_s}}},
                         },
                         {u"expression"_s},
                         false));

    tools.append(toolDef(u"debug_session"_s,
                         u"Start, restart, stop, hot_reload, or hot_restart Kate's debug session, or reload .vscode/launch.json targets."_s,
                         QJsonObject{
                             {u"action"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"enum"_s, QJsonArray{u"start"_s, u"restart"_s, u"stop"_s, u"hot_reload"_s, u"hot_restart"_s, u"reload_launch_json"_s}}, {u"description"_s, u"Session lifecycle action."_s}}},
                             {u"target"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional target name to select before starting."_s}}},
                             {u"timeout_ms"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Max ms to wait (default 15000)."_s}}},
                         },
                         {u"action"_s},
                         false));

    tools.append(toolDef(u"debug_console"_s,
                         u"Send a raw command to Kate's DAP/GDB debugger console REPL (e.g. 'whereami', 'modules', 'blist', 'info registers')."_s,
                         QJsonObject{
                             {u"command"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Console command to execute in the debugger REPL."_s}}},
                         },
                         {u"command"_s},
                         false));

    tools.append(toolDef(u"build_targets"_s,
                         u"Discover workspace build targets from Kate's Build plugin, CMake File API / CMakeLists.txt, Cargo, npm, Go, Make, and locate compile_commands.json. Read-only."_s,
                         QJsonObject{},
                         {},
                         false));

    tools.append(toolDef(u"build_run"_s,
                         u"Trigger a build in Kate's Build plugin (default target or a named target). Build diagnostics flow into Kate's Diagnostics view where lsp_diagnostics can read them."_s,
                         QJsonObject{
                             {u"target"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional target name. Omit to build the default target."_s}}},
                         },
                         {},
                         false));

    tools.append(toolDef(u"terminal_run"_s,
                         u"Send a command to Kate's embedded Konsole terminal panel so the user can observe interactive/long-running output directly."_s,
                         QJsonObject{
                             {u"command"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Shell command to run in Kate's embedded terminal."_s}}},
                             {u"working_dir"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional workspace-relative or absolute directory to cd into first."_s}}},
                         },
                         {u"command"_s},
                         false));

    tools.append(toolDef(u"write_plan_file"_s,
                         u"Write the implementation plan for a complicated, multi-step task to a temporary file "
                         u"the user can open and follow while you work. Pass the full plan as markdown: the goal, "
                         u"the ordered steps, the files each step touches, and how it will be verified. Repeated "
                         u"calls overwrite the same file, so keep it current as the work moves. The file lives "
                         u"outside the workspace and is never part of the project."_s,
                         QJsonObject{
                             {u"content"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Full plan as markdown: goal, ordered steps, files touched per step, verification."_s}}},
                         },
                         {u"content"_s},
                         false));

    tools.append(toolDef(u"suggest"_s,
                         u"Post a suggestion for the user to accept or dismiss, instead of changing anything yourself. "
                         u"Use it for improvements the user did not ask for: a refactor you noticed, a bug nearby, a "
                         u"cleaner way to do something, or a follow-up worth considering. The suggestion appears as a "
                         u"card in the transcript with Accept and Dismiss buttons; Accept opens the anchored line when "
                         u"a path is given. Never suggest something you have already made."_s,
                         QJsonObject{
                             {u"suggestion"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"What you are suggesting, as markdown. Be concrete: what to change, where, and why it helps."_s}}},
                             {u"path"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Optional file the suggestion is about, relative to the workspace or absolute."_s}}},
                             {u"line"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Optional 1-based line in that file the suggestion is about."_s}}},
                         },
                         {u"suggestion"_s},
                         false));

    tools.append(toolDef(u"question"_s,
                         u"Ask the user a question and wait for their answer before continuing. Use it when the next "
                         u"step genuinely depends on something only they can decide: a choice between approaches, a "
                         u"missing requirement, an ambiguous instruction, or a destructive action you want confirmed. "
                         u"Offer the likely answers as options so they can reply with one tap; the user may always "
                         u"type their own answer as well. The answer arrives as this tool's result, so do not ask the "
                         u"same question twice. Do not use it for anything you can work out yourself."_s,
                         QJsonObject{
                             {u"question"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"What you are asking the user, as a complete question."_s}}},
                             {u"options"_s, QJsonObject{{u"type"_s, u"array"_s}, {u"items"_s, QJsonObject{{u"type"_s, u"string"_s}}}, {u"description"_s, u"Two to four short, mutually exclusive answers the user is likely to pick. Omit when there is nothing to choose between."_s}}},
                             {u"allow_freeform"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If false, the user must pick one of the options. Defaults to true."_s}}},
                         },
                         {u"question"_s},
                         false));

    tools.append(toolDef(u"todo_write"_s,
                         u"Replace the task list with the given todos, each with a status of pending, in_progress, "
                         u"or completed. Use it to plan a multi-step task and to keep the user's checklist current "
                         u"as steps finish: write the full list every time, marking what is done. Read-only: it "
                         u"changes no file, only the checklist shown to the user."_s,
                         QJsonObject{
                             {u"todos"_s, QJsonObject{{u"type"_s, u"array"_s}, {u"items"_s, QJsonObject{
                                 {u"type"_s, u"object"_s},
                                 {u"properties"_s, QJsonObject{
                                     {u"content"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"What has to be done."_s}}},
                                     {u"status"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"enum"_s, QJsonArray{u"pending"_s, u"in_progress"_s, u"completed"_s}}, {u"description"_s, u"Defaults to pending."_s}}},
                                 }},
                                 {u"required"_s, QJsonArray{u"content"_s}},
                             }}, {u"description"_s, u"The full todo list; replaces the previous one."_s}}},
                         },
                         {u"todos"_s},
                         false));

    tools.append(toolDef(u"todo_read"_s,
                         u"Return the current task list. Read-only."_s,
                         QJsonObject{},
                         {},
                         false));

    tools.append(toolDef(u"think"_s,
                         u"Think through a problem out loud before acting: analyze the request, weigh alternatives, "
                         u"and state the plan. The thought is recorded in the transcript's reasoning block. Read-only."_s,
                         QJsonObject{
                             {u"thought"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The reasoning to record."_s}}},
                         },
                         {u"thought"_s},
                         false));

    tools.append(toolDef(u"exit_plan_mode"_s,
                         u"Leave plan mode once the plan has been presented and approved, so the turn can start "
                         u"making edits. Outside plan mode this just reports that plan mode is off. Read-only: it "
                         u"changes no file."_s,
                         QJsonObject{},
                         {},
                         false));

    tools.append(toolDef(u"web_search"_s,
                             u"Search the public web and return ranked results with titles, URLs and snippets. "
                             u"Use it for library documentation, error messages, API references, or anything outside "
                             u"this workspace. Prefer a specific query over a broad one; cite the URL when you use a result."_s,
                             QJsonObject{
                                 {u"query"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The search query."_s}}},
                                 {u"max_results"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"How many results to return (1-20). Defaults to 5."_s}}},
                                 {u"site"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Restrict results to this site, e.g. \"doc.rust-lang.org\". Omit for the whole web."_s}}},
                             },
                             {u"query"_s},
                             false));

    tools.append(toolDef(u"web_fetch"_s,
                             u"Fetch a web page and return its readable text, with scripts and navigation stripped. "
                             u"Use it after web_search to read a specific result instead of relying on the snippet. "
                             u"Only http and https URLs are fetched."_s,
                             QJsonObject{
                                 {u"url"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The absolute http(s) URL to fetch."_s}}},
                                 {u"timeout_ms"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Per-request timeout in milliseconds (minimum 1000). Defaults to the configured web timeout."_s}}},
                                 {u"max_bytes"_s, QJsonObject{{u"type"_s, u"integer"_s}, {u"description"_s, u"Reject a body larger than this many bytes; capped at 5 MiB. Defaults to 5 MiB."_s}}},
                             },
                             {u"url"_s},
                             false));


tools.append(toolDef(subtaskToolName(),
                          u"Spawn a sub-agent to handle a self-contained piece of work, then return its result to you. "
                          u"Give it a complete task description with all context it needs, since it cannot see this "
                          u"conversation. Independent subtasks requested in the same response run in parallel, so batch "
                          u"them. Pick an agent by name when one fits, or a mode when you need a specific tool set. "
                          u"Returns the sub-agent's final answer."_s,
                          QJsonObject{
                              {u"description"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"The full task for the sub-agent, including the goal, relevant files, and the expected output format."_s}}},
                              {u"agent"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"A named agent that suits the work: scout (read-only questions), architect (design and plans), coder (implementation), debugger (root-cause fixes), reviewer (check someone else's work), or a custom agent."_s}}},
                              {u"mode"_s, QJsonObject{{u"type"_s, u"string"_s}, {u"description"_s, u"Mode id to run the sub-agent in, overriding the agent's default: code, ask, architect, debug, orchestrator, or a custom mode. Defaults to the agent's mode, or code."_s}}},
                              {u"include_transcript"_s, QJsonObject{{u"type"_s, u"boolean"_s}, {u"description"_s, u"If true, the result also includes the sub-agent's tool log, so you can see how it reached its answer. Use it when you need to verify the work."_s}}},
                          },
                          {u"description"_s},
                          false));

    if (access.allowAll) {
        return tools;
    }

    QJsonArray allowedTools;
    for (const QJsonValue &tool : tools) {
        const QString name = tool.toObject().value(u"function"_s).toObject().value(u"name"_s).toString();
        if (access.allows(name)) {
            allowedTools.append(tool);
        }
    }
    return allowedTools;
}

QString defaultSystemPrompt(const QString &workspace)
{
    return u"You are Kate AI, a coding agent inside the Kate text editor.\n"
           "You solve the user's request by inspecting the workspace with tools, making focused edits, and verifying the result.\n"
           "\n"
           "How to work:\n"
           "- Start by locating the relevant code (query_project_graph, glob, grep, list_dir) before guessing paths.\n"
           "- Read only the files and ranges you need. Prefer offset/limit on large files.\n"
           "- Prefer edit_file or multi_edit_file for surgical changes. Use multi_edit_file when modifying multiple non-contiguous locations in the same file. Use replace_all when the same unique snippet should change everywhere.\n"
           "- Use write_file only for new files or complete rewrites. Do not invent files outside the workspace.\n"
           "- Match existing style, naming, imports, and architecture. Do not add unrelated refactors or comments.\n"
           "- After edits, verify with a focused read, test, build, or lint. Treat tool output as evidence.\n"
           "- If a tool fails, diagnose the actual error and change approach; do not retry the identical call.\n"
           "- Prefer one purposeful batch of tools over speculative exploration.\n"
           "- Shell commands must be non-interactive. Do not request secrets or write credential files.\n"
                      "\n"
                      "WEB RESEARCH:\n"
                      "- Use web_search for anything outside this workspace: library and API documentation, error messages, "
                      "release notes, or the current state of a dependency. Prefer it over answering from memory when a version "
                      "or API could have changed.\n"
                      "- Read a result with web_fetch before relying on it; snippets alone are often misleading.\n"
                      "- Cite the URL when a web result informs your answer.\n"
                      "- Never send workspace contents, file contents, secrets, or user code to a search provider. Queries only.\n"
           "\n"
           "LSP & DIAGNOSTICS:\n"
           "- lsp_diagnostics reports language-server/editor errors and warnings for a file or the whole workspace; check it after edits instead of only reading test output.\n"
           "- lsp_symbols lists a file's classes and functions with line numbers; lsp_hover, lsp_definition and lsp_references answer symbol questions without guessing; lsp_code_actions lists the quick fixes offered at a position.\n"
           "- When no language server is running for a file these tools say so; fall back to a build/lint/test via bash and to grep.\n"
           "\n"
           "DEBUGGER, BUILD & TERMINAL:\n"
           "- When Kate's debugger (kategdbplugin) is active/paused, use debug_status, debug_stack, debug_locals, debug_breakpoints, debug_select_frame, debug_select_thread, and debug_snapshot to inspect live runtime state before editing.\n"
           "- Use debug_set_breakpoint, debug_step, debug_eval, debug_session, and debug_console when actively reproducing or stepping through a bug. Always prefer read-only inspection over debug_eval with side effects.\n"
           "- Use build_targets to discover workspace build targets and compile_commands.json, build_run to trigger Kate's Build plugin, and terminal_run only when the user should observe output in Kate's embedded Konsole (use bash when you need captured stdout/stderr).\n"
           "\n"
           "REASONING & PLANNING (required):\n"
           "1. THINKING: Before every response, output your reasoning in a <thinking> block. Analyze the request, consider alternatives, plan steps, and anticipate issues. The user sees this block - it stays open above your answer. Be thorough.\n"
           "2. PLAN: After thinking, record the plan with todo_write as a list of concrete, verifiable steps with statuses (pending/in_progress/completed), and keep it current as you go - the user sees it as a live checklist. For a complicated, multi-step task also write the full plan to the plan file with write_plan_file; the user can open that file and follow the implementation as it happens.\n"
            "3. EXECUTION: Follow your plan step by step. After completing each step, call todo_write again with that step marked completed. When the plan was presented in plan mode and the user approved it, call exit_plan_mode to leave plan mode and start editing.\n"
            "- For multi-file changes, prefer apply_patch with one unified diff over many separate edit_file calls.\n"
             "- When you notice an improvement the user did not ask for, post it with suggest instead of making the change: it becomes a card the user can accept or dismiss.\n"
             "- When the next step depends on a decision only the user can make, stop and ask with question instead of guessing: it pauses the turn and shows them your question with the likely answers.\n"
            "\n"
           "THINKING PROTOCOL (detailed):\n"
           "- Your <thinking> block MUST come FIRST, before any visible text.\n"
           "- Include in thinking: problem analysis, root cause hypotheses, alternative approaches considered, risk assessment, file/dependency mapping, and a detailed step-by-step plan.\n"
           "- Be honest about uncertainty. If you don't know something, say so in thinking and plan to investigate.\n"
           "- The thinking block is your private workspace - use it fully. There is no penalty for thorough reasoning.\n"
           "\n"
           "PLAN FORMAT (structured):\n"
           "- After </thinking>, output a plan under '## Plan' or '## Implementation Plan' heading.\n"
           "- Use numbered steps (1., 2., 3.) with concrete, verifiable actions.\n"
           "- Each step = ONE tool call or a small batch of related calls.\n"
           "- Good: 'Read auth/login.cpp lines 40-80 to understand token handling'\n"
           "- Good: 'Edit auth/login.cpp to fix token refresh logic'\n"
           "- Good: 'Run tests for auth module to verify fix'\n"
           "- Bad: 'Fix the login bug' (too vague)\n"
           "- Bad: 'Explore the codebase' (not actionable)\n"
           "\n"
           "EXECUTION DISCIPLINE:\n"
           "- Execute ONE plan step per model turn when possible.\n"
           "- After each step, briefly narrate what you learned/changed and what's next (1-2 sentences).\n"
           "- If a step fails, diagnose in thinking, then adapt the plan - don't blindly retry.\n"
           "- Mark completed steps in the plan by outputting an updated plan with 'completed: true'.\n"
           "\n"
           "VERIFICATION REQUIREMENT:\n"
           "- After ANY file mutation, you MUST verify with a focused read, test, build, or lint.\n"
           "- Verification is not optional - it's part of the step.\n"
           "- If verification fails, thinking must analyze the failure and plan a targeted fix.\n"
           "\n"
           "COLLABORATION STYLE:\n"
           "- Your visible response should be conversational and useful to the user.\n"
           "- Narrate progress naturally: 'I'll check the auth module first, then apply the fix and run tests.'\n"
           "- Never expose tool names, JSON, or internal protocol in visible text.\n"
           "- When done, give a concise summary: what changed, how verified, any follow-ups.\n"
           "\n"
           "Workspace root: "_s
        + workspace;
}

QString truncateSafe(const QString &text, int maxLength)
{
    if (maxLength <= 0) {
        return QString();
    }
    if (text.length() <= maxLength) {
        return text;
    }
    int end = maxLength;
    // A cut between the two halves of a surrogate pair would leave half a
    // character behind, so the cut moves back to the start of the pair.
    if (end > 0 && end < text.length() && text.at(end - 1).isHighSurrogate()) {
        --end;
    }
    return text.left(end);
}

QString compressText(const QString &text, int maxLength, bool enabled)
{
    if (!enabled || text.length() <= maxLength) {
        return text;
    }

    // Truncate and add indicator. The cut is taken from the budget so the
    // result never exceeds the length the caller asked for.
    const QString indicator = u"... (truncated)"_s;
    if (maxLength <= indicator.size()) {
        return truncateSafe(text, maxLength);
    }
    const int room = qMax(0, maxLength - indicator.size());
    QString result = truncateSafe(text, room);
    result += indicator;
    return result;
}

// Smart context compression - preserves important parts while reducing size
QString smartCompressContext(const QString &text, int maxLength, bool enabled)
{
    if (!enabled || text.length() <= maxLength) {
        return text;
    }

    // Strategy: Keep first 30% (context/setup), last 50% (recent/important), summarize middle
    int keepStart = maxLength * 30 / 100;
    int keepEnd = maxLength * 50 / 100;
    int summaryBudget = maxLength - keepStart - keepEnd - 50; // 50 for summary marker

    if (summaryBudget < 100) {
        // Fall back to simple truncation if budget too small
        return compressText(text, maxLength, true);
    }

    // The cuts go through truncateSafe(): a cut in the middle of an astral
    // character would leave half of it behind.
    QString start = truncateSafe(text, keepStart);
    QString end = text.right(keepEnd);
    if (!end.isEmpty() && end.at(0).isLowSurrogate()) {
        // The tail starts in the second half of a surrogate pair.
        end.remove(0, 1);
    }

    // Create a summary of what was in the middle
    QString middle = text.mid(keepStart, text.length() - keepStart - keepEnd);
    int middleLines = middle.count(u'\n');
    int middleChars = middle.length();

    QString summary = u"\n[... %1 lines, %2 chars compressed ...]\n"_s.arg(middleLines).arg(middleChars);

    return start + summary + end;
}

// Compress a list of messages intelligently
QList<ChatMessage> compressMessageHistory(const QList<ChatMessage> &messages,
                                           int maxMessages,
                                           int maxTotalChars,
                                           bool enabled)
{
    // maxMessages <= 0 means "do not trim". Falling through with a
    // non-positive keep count would return the system prompt and nothing else.
    if (!enabled || maxMessages <= 0 || messages.size() <= maxMessages) {
        return messages;
    }

    QList<ChatMessage> result;
    // Always keep system message
    if (!messages.isEmpty() && messages.first().role == ChatMessage::Role::System) {
        result.append(messages.first());
    }

    // Keep last N messages, but never drop every non-system message. The
    // budget is counted in messages the result can hold: the system message
    // was already taken out of it.
    const int nonSystem = messages.size() - result.size();
    const int keepCount = qBound(1, qMin(maxMessages - result.size(), nonSystem), nonSystem);
    int keepFrom = messages.size() - keepCount;
    // Never begin the kept window with a Tool result whose declaring
    // assistant message was dropped: OpenAI/Anthropic reject that request.
    const int firstBody = result.size();
    while (keepFrom > firstBody && messages.at(keepFrom).role == ChatMessage::Role::Tool) {
        --keepFrom;
    }
    if (keepFrom > firstBody && messages.at(keepFrom - 1).role == ChatMessage::Role::Assistant
        && !messages.at(keepFrom - 1).toolCalls.isEmpty()) {
        --keepFrom;
    }
    for (int i = keepFrom; i < messages.size(); ++i) {
        result.append(messages[i]);
    }

    // If still over char budget, compress older messages
    int totalChars = 0;
    for (const auto &msg : result) {
        totalChars += msg.content.length() + msg.thinking.length();
    }

    if (totalChars > maxTotalChars && result.size() > 2) {
        // Compress the oldest non-system message
        for (int i = 1; i < result.size() - 1; ++i) {
            ChatMessage &msg = result[i];
            if (msg.content.length() > 500) {
                msg.content = smartCompressContext(msg.content, 500, true);
            }
            if (msg.thinking.length() > 1000) {
                msg.thinking = smartCompressContext(msg.thinking, 1000, true);
            }
        }
    }

    return result;
}

// Structured planning helpers ------------------------------------------------
QJsonArray parsePlanFromText(const QString &text)
{
    QJsonArray plan;
    const QStringList markers = QStringList()
        << u"## plan"_s << u"## implementation plan"_s << u"## plan (structured)"_s
        << u"plan:"_s << u"implementation plan:"_s << u"steps:"_s << u"action plan:"_s;
    // The marker has to open a line. A bare "plan:" is also the tail of
    // "explanation:", and "steps:" turns up in ordinary prose - matched
    // anywhere in the text, they started a plan in the middle of a sentence.
    // The earliest marker wins, not the first one in the list: a text that
    // contains both "Plan:" and "## Implementation Plan" has to start at the
    // place where the plan really begins.
    int start = -1;
    int offset = 0;
    const QStringList lines = text.split(u'\n');
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        const QString lowerTrimmed = trimmed.toLower();
        if (!lowerTrimmed.isEmpty()) {
            // The offset of the marker in the text: the line start plus the
            // indentation in front of it.
            int indent = 0;
            while (indent < line.size() && line.at(indent).isSpace()) {
                ++indent;
            }
            for (const QString &m : markers) {
                if (!lowerTrimmed.startsWith(m)) {
                    continue;
                }
                const int index = offset + indent;
                if (start < 0 || index < start) {
                    start = index;
                }
            }
        }
        offset += line.size() + 1;
    }
    if (start < 0) {
        return plan;
    }
    int i = start;
    while (i < text.size() && text[i] != u'\n') {
        ++i;
    }
    ++i;
    int lineStart = i;
    while (i <= text.size() && plan.size() < 20) {
        if (i == text.size() || text[i] == u'\n') {
            const QString line = text.mid(lineStart, i - lineStart).trimmed();
            if (!line.isEmpty()) {
                QString step = line;
                // Handle various list formats: "1. ", "1) ", "- ", "* ", "Step 1: "
                int s = 0;
                while (s < step.size() && (step[s].isDigit() || step[s] == u'.' || step[s] == u')' || step[s] == u'(')) {
                    ++s;
                }
                if (s > 0 && s < step.size() && (step[s] == u' ' || step[s] == u'.' || step[s] == u')')) {
                    step = step.mid(s).trimmed();
                } else if (step.startsWith(u"- "_s) || step.startsWith(u"* "_s) || step.startsWith(u"• "_s)) {
                    step = step.mid(2).trimmed();
                } else if (step.startsWith(u"step "_s, Qt::CaseInsensitive)) {
                    // The colon belongs to the prefix. It has to be removed
                    // before the digits are skipped, otherwise the "Step 1:"
                    // form keeps its colon and the marker test below can never
                    // match it.
                    step = step.mid(5).trimmed();
                    if (step.startsWith(u":"_s)) {
                        step = step.mid(1).trimmed();
                    }
                }
                if (!step.isEmpty()) {
                    QJsonObject obj;
                    obj.insert(u"id"_s, u"step%1"_s.arg(plan.size() + 1));
                    obj.insert(u"description"_s, step);
                    obj.insert(u"completed"_s, false);
                    obj.insert(u"inProgress"_s, false);
                    plan.append(obj);
                }
            }
            // Always advance, including past the final line: stopping at
            // text.size() would re-process the empty terminal segment forever.
            ++i;
            lineStart = i;
        } else {
            ++i;
        }
    }
    return plan;
}

QJsonArray mergePlanIntoAssistantMessage(const QJsonArray &existingPlan,
                                         const QString &assistantText)
{
    const QJsonArray fresh = parsePlanFromText(assistantText);
    if (fresh.isEmpty()) {
        return existingPlan;
    }
    // The state of a step is matched by its id first and by its wording
    // second. Matching the wording alone loses the state of every step the
    // agent reworded, and the checklist silently resets mid-task.
    QHash<QString, bool> completedById;
    QHash<QString, bool> completedByDesc;
    for (const QJsonValue &v : existingPlan) {
        const QJsonObject o = v.toObject();
        const bool completed = o.value(u"completed"_s).toBool();
        const QString id = o.value(u"id"_s).toString();
        if (!id.isEmpty()) {
            completedById.insert(id, completed);
        }
        completedByDesc.insert(o.value(u"description"_s).toString().toLower(), completed);
    }
    QJsonArray merged;
    for (const QJsonValue &v : fresh) {
        QJsonObject o = v.toObject();
        const QString desc = o.value(u"description"_s).toString();
        const QString id = o.value(u"id"_s).toString();
        if (completedById.contains(id)) {
            o.insert(u"completed"_s, completedById.value(id));
        } else {
            o.insert(u"completed"_s, completedByDesc.value(desc.toLower(), false));
        }
        merged.append(o);
    }
    return merged;
}

QJsonArray markPlanStepCompleted(const QJsonArray &plan, const QString &stepId)
{
    QJsonArray out;
    for (const QJsonValue &v : plan) {
        QJsonObject o = v.toObject();
        if (o.value(u"id"_s).toString() == stepId) {
            o.insert(u"completed"_s, true);
            o.insert(u"inProgress"_s, false);
        }
        out.append(o);
    }
    return out;
}

bool planIsComplete(const QJsonArray &plan)
{
    for (const QJsonValue &v : plan) {
        if (!v.toObject().value(u"completed"_s).toBool()) {
            return false;
        }
    }
    return !plan.isEmpty();
}

QString apiFormatId(ApiFormat format)
{
    switch (format) {
    case ApiFormat::AnthropicCompatible:
        return u"anthropic-compatible"_s;
    case ApiFormat::AcpNative:
        return u"acp-native"_s;
    case ApiFormat::OpenAICompatible:
    default:
        return u"openai-compatible"_s;
    }
}

QString apiFormatLabel(ApiFormat format)
{
    switch (format) {
    case ApiFormat::AnthropicCompatible:
        return i18n("Anthropic/Claude Compatible");
    case ApiFormat::AcpNative:
        return i18n("ACP native (stdio JSON-RPC)");
    case ApiFormat::OpenAICompatible:
    default:
        return i18n("OpenAI Compatible");
    }
}

ApiFormat apiFormatFromId(const QString &id)
{
    if (id == u"anthropic-compatible"_s) {
        return ApiFormat::AnthropicCompatible;
    }
    if (id == u"acp-native"_s) {
        return ApiFormat::AcpNative;
    }
    return ApiFormat::OpenAICompatible;
}

QList<AcpAgentPreset> acpAgentPresets()
{
    return {
        {u"grok-build"_s, i18n("Grok Build"), u"grok"_s, u"agent stdio"_s, u"XAI_API_KEY"_s},
        {u"antigravity-acp"_s, i18n("Antigravity (agy)"), u"agy"_s, u"acp"_s, u"GEMINI_API_KEY"_s},
        {u"claude-acp"_s, i18n("Claude Agent"), u"npx"_s, u"-y @agentclientprotocol/claude-agent-acp"_s, u"ANTHROPIC_API_KEY"_s},
        {u"codex-acp"_s, i18n("Codex"), u"npx"_s, u"-y @agentclientprotocol/codex-acp"_s, u"OPENAI_API_KEY"_s},
        {u"gemini"_s, i18n("Gemini CLI"), u"gemini"_s, u"--acp"_s, u"GEMINI_API_KEY"_s},
        {u"github-copilot-cli"_s, i18n("GitHub Copilot"), u"copilot"_s, u"--acp"_s, {}},
        {u"goose"_s, i18n("goose"), u"goose"_s, u"acp"_s, {}},
        {u"opencode"_s, i18n("OpenCode"), u"opencode"_s, u"acp"_s, {}},
        {u"cline"_s, i18n("Cline"), u"npx"_s, u"-y cline --acp"_s, {}},
        {u"qwen-code"_s, i18n("Qwen Code"), u"npx"_s, u"-y @qwen-code/qwen-code --acp"_s, {}},
        {u"auggie"_s, i18n("Auggie CLI"), u"npx"_s, u"-y @augmentcode/auggie --acp"_s, {}},
        {u"cursor"_s, i18n("Cursor"), u"cursor-agent"_s, u"acp"_s, {}},
        {u"mistral-vibe"_s, i18n("Mistral Vibe"), u"vibe-acp"_s, u""_s, u"MISTRAL_API_KEY"_s},
        {u"mcode"_s, i18n("MiniMax Code"), u"mcode"_s, u"acp"_s, u""_s},
        {u"custom"_s, i18n("Custom"), {}, {}, {}},
    };
}

AcpAgentPreset acpAgentPreset(const QString &id)
{
    for (const AcpAgentPreset &preset : acpAgentPresets()) {
        if (preset.id == id) {
            return preset;
        }
    }
    return {u"custom"_s, i18n("Custom"), {}, {}, {}};
}

QList<AcpAgentPreset> acpAgentChoices(const Settings &settings)
{
    const QList<AcpAgentPreset> presets = acpAgentPresets();
    const QJsonArray installed =
        QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();

    QList<AcpAgentPreset> choices;
    choices.reserve(presets.size() + installed.size());
    // Agents installed from the ACP registry sit above the Custom escape hatch,
    // which stays last so the typed launch line is never the default pick.
    qsizetype customIndex = presets.size();
    for (qsizetype i = 0; i < presets.size(); ++i) {
        if (presets.at(i).id == u"custom"_s) {
            customIndex = i;
            break;
        }
        choices.append(presets.at(i));
    }
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        const QString id = agent.value(u"id"_s).toString().trimmed();
        if (id.isEmpty()) {
            continue;
        }
        bool known = false;
        for (const AcpAgentPreset &choice : choices) {
            if (choice.id == id) {
                known = true;
                break;
            }
        }
        // An installed agent that is also a preset keeps the preset entry: its
        // label is localized and its launch line matches the upstream one.
        if (known) {
            continue;
        }
        QString label = agent.value(u"name"_s).toString().trimmed();
        choices.append({id, label.isEmpty() ? id : label, {}, {}, {}});
    }
    for (qsizetype i = customIndex; i < presets.size(); ++i) {
        choices.append(presets.at(i));
    }
    return choices;
}

AcpAgentPreset acpAgentLaunchLine(const Settings &settings, const QString &agentId)
{
    const QJsonArray installed =
        QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        if (agent.value(u"id"_s).toString() != agentId) {
            continue;
        }
        QStringList args;
        for (const QJsonValue &arg : agent.value(u"args"_s).toArray()) {
            // acpArgs is a command-line string parsed with
            // QProcess::splitCommand(), so an argument containing whitespace
            // must be quoted here or it turns into several arguments.
            const QString text = arg.toString();
            if (text.contains(u' ') || text.contains(u'\t') || text.contains(u'"')) {
                QString escaped = text;
                escaped.replace(u'"', u"\\\""_s);
                args.append(u"\"%1\""_s.arg(escaped));
            } else {
                args.append(text);
            }
        }
        QString label = agent.value(u"name"_s).toString().trimmed();
        return {agentId, label.isEmpty() ? agentId : label,
                agent.value(u"command"_s).toString(),
                args.join(u' '),
                agent.value(u"apiKeyEnv"_s).toString()};
    }
    AcpAgentPreset preset = acpAgentPreset(agentId);
    if (preset.id == u"custom"_s) {
        // Neither the registry nor the presets know this agent: it is a Custom
        // one whose launch line the user typed. Callers key off the empty
        // command, which means "do not overwrite the launch fields".
        return {agentId, {}, {}, {}, {}};
    }
    return preset;
}

QString acpEffectiveCommand(const Settings &settings)
{
    const QString command = settings.acpCommand.trimmed();
    if (!command.isEmpty()) {
        return command;
    }
    return acpAgentLaunchLine(settings, settings.acpAgentId).command;
}

QString acpEffectiveArgs(const Settings &settings)
{
    const QString args = settings.acpArgs.trimmed();
    if (!args.isEmpty()) {
        return args;
    }
    const AcpAgentPreset launch = acpAgentLaunchLine(settings, settings.acpAgentId);
    const QString configuredCmd = settings.acpCommand.trimmed();
    const QString configuredBase = QFileInfo(configuredCmd).baseName().toLower();
    const QString launchBase = QFileInfo(launch.command).baseName().toLower();
    if (!configuredCmd.isEmpty() && !launch.command.isEmpty()
        && configuredBase != launchBase) {
        // The user/registry pointed this agent id at a different executable
        // than the built-in preset (e.g. legacy `./agy_acp_server.par` vs
        // `agy`): do not inherit the preset's subcommand args.
        return {};
    }
    return launch.args;
}

QString acpEffectiveApiKeyEnv(const Settings &settings)
{
    const QString env = settings.acpApiKeyEnv.trimmed();
    if (!env.isEmpty()) {
        return env;
    }
    return acpAgentLaunchLine(settings, settings.acpAgentId).apiKeyEnv;
}

bool acpAgentIsGrok(const Settings &settings)
{
    return QFileInfo(acpEffectiveCommand(settings)).fileName() == u"grok"_s;
}

QString acpBinaryInstallDir(const QString &agentId)
{
    // Tests and packagers can point the installer somewhere else. Otherwise
    // binary registry agents live next to the rest of Kate AI's user data.
    const QByteArray overrideDir = qgetenv("KATEAI_ACP_AGENTS_DIR");
    QString root;
    if (!overrideDir.isEmpty()) {
        root = QString::fromLocal8Bit(overrideDir);
    } else {
        root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (root.isEmpty()) {
            root = QDir::home().filePath(u".local/share"_s);
        }
        root += u"/kateai/acp-agents"_s;
    }

    QString safeId = agentId;
    for (QChar &character : safeId) {
        if (!character.isLetterOrNumber() && character != u'.' && character != u'-' && character != u'_') {
            character = u'_';
        }
    }
    if (safeId.isEmpty() || safeId == u"."_s || safeId == u".."_s) {
        safeId = u"agent"_s;
    }
    return root + u'/' + safeId;
}

static bool isBinaryInstallEntry(const QJsonObject &agent)
{
    return agent.value(u"distributionType"_s).toString() == u"binary"_s
        || !agent.value(u"binaryArchiveUrl"_s).toString().isEmpty();
}

bool acpAgentHasBinaryInstall(const Settings &settings)
{
    const QJsonArray installed = QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        if (agent.value(u"id"_s).toString() == settings.acpAgentId && isBinaryInstallEntry(agent)) {
            return true;
        }
    }
    return false;
}

QString acpInstalledBinaryCommand(const Settings &settings)
{
    const QJsonArray installed = QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        if (agent.value(u"id"_s).toString() != settings.acpAgentId) {
            continue;
        }
        if (!isBinaryInstallEntry(agent)) {
            return {};
        }

        const QString command = agent.value(u"command"_s).toString();
        if (command.isEmpty()) {
            return {};
        }
        const QFileInfo commandInfo(command);
        if (commandInfo.isAbsolute()) {
            if (commandInfo.exists() && commandInfo.isExecutable()) {
                return commandInfo.absoluteFilePath();
            }
            return {};
        }

        // The registry records the executable relative to the unpacked archive
        // ("./agy_acp_server.par"). Kate installs the archive into its own data
        // directory, so resolve the command against that directory instead of
        // the process working directory. Prefer the directory recorded at
        // install time: an env override or a moved data dir must not make the
        // installed agent invisible.
        QString relative = command;
        while (relative.startsWith(u"./"_s)) {
            relative.remove(0, 2);
        }
        QString installRoot = agent.value(u"binaryInstallDir"_s).toString().trimmed();
        if (installRoot.isEmpty()) {
            installRoot = acpBinaryInstallDir(settings.acpAgentId);
        }
        const QDir installDir(installRoot);
        const QString candidate = QDir::cleanPath(installDir.absoluteFilePath(relative));
        const QString installRootPath = installDir.absolutePath();
        if (candidate != installRootPath && !candidate.startsWith(installRootPath + u'/')) {
            return {};
        }
        const QFileInfo candidateInfo(candidate);
        if (candidateInfo.exists() && candidateInfo.isExecutable() && !candidateInfo.isSymLink()) {
            return candidateInfo.absoluteFilePath();
        }
        return {};
    }
    return {};
}

bool acpAgentIsMinimaxCode(const Settings &settings)
{
    if (settings.acpAgentId == u"minimax-code"_s || settings.acpAgentId == u"mcode"_s) {
        return true;
    }
    const QString command = acpEffectiveCommand(settings);
    const QString baseName = QFileInfo(command).fileName();
    return baseName == u"mcode"_s;
}

bool acpAgentIsAntigravity(const Settings &settings)
{
    if (settings.acpAgentId == u"antigravity-acp"_s
        || settings.acpAgentId == u"agy"_s
        || settings.acpAgentId == u"antigravity"_s) {
        return true;
    }
    const QString command = acpEffectiveCommand(settings);
    const QString baseName = QFileInfo(command).fileName().toLower();
    return baseName == u"agy"_s || baseName == u"agy.exe"_s || baseName.contains(u"agy_acp_server"_s);
}

bool acpAgentIsLegacyAntigravityServer(const Settings &settings)
{
    const QString command = acpEffectiveCommand(settings);
    const QString baseName = QFileInfo(command).fileName().toLower();
    return baseName.contains(u"agy_acp_server"_s);
}

AcpAgentQuirks acpAgentQuirks(const QString &agentId, const QString &command)
{
    AcpAgentQuirks q;
    const QString baseName = QFileInfo(command).fileName();
    const QString lowerBase = baseName.toLower();
    if (agentId == u"grok-build"_s || lowerBase == u"grok"_s) {
        // grok agent stdio --model <model> --always-approve
        q.modelFlag = true;
    } else if (agentId == u"minimax-code"_s || lowerBase == u"mcode"_s) {
        q.stripNpmTokens = true;
        q.needsAcpSubcommand = true;
    } else if (lowerBase.contains(u"agy_acp_server"_s)) {
        // Legacy gflags-based standalone binary: reject Chromium's standard
        // switch, and the sanitizer + hugepage flags keep it running cleanly on
        // >4KB-page and no-IPv6 machines.
        q.stripArgs = {u"--disable-features=CrOSHugepageRemapAndLockInZygote"_s};
        q.extraArgs = {u"--enforce_kernel_ipv6_support=false"_s,
                       u"--core_register_sanitization_enabled=false"_s,
                       u"--core_sanitization_enabled=false"_s,
                       u"--boringssl_sanitization_enabled=false"_s,
                       u"--hugepage_text=false"_s};
        q.spoofZedClientInfo = true;
    } else if (lowerBase == u"agy"_s || lowerBase == u"agy.exe"_s
               || agentId == u"antigravity-acp"_s || agentId == u"agy"_s || agentId == u"antigravity"_s) {
        // Modern Antigravity CLI (`agy acp`): needs the `acp` subcommand so it
        // speaks stdio JSON-RPC instead of opening its interactive TUI. Strip
        // any legacy `agy_acp_server` gflags that might have been persisted in
        // user settings, because `agy acp` rejects unknown flags and exits 1.
        q.needsAcpSubcommand = true;
        q.stripArgs = {u"--uid="_s,
                       u"--disable-features=CrOSHugepageRemapAndLockInZygote"_s,
                       u"--enforce_kernel_ipv6_support=false"_s,
                       u"--core_register_sanitization_enabled=false"_s,
                       u"--core_sanitization_enabled=false"_s,
                       u"--boringssl_sanitization_enabled=false"_s,
                       u"--hugepage_text=false"_s};
    } else if (agentId == u"opencode"_s || lowerBase == u"opencode"_s) {
        q.needsAcpSubcommand = true;
    } else if (agentId == u"claude-acp"_s || command.contains(u"claude-agent-acp"_s)) {
        q.modelFlag = true;
    } else if (agentId == u"gemini"_s || lowerBase == u"gemini"_s) {
        q.modelFlag = true;
    }
    return q;
}

bool acpAgentIsMistralVibe(const Settings &settings)
{
    if (settings.acpAgentId == u"mistral-vibe"_s) {
        return true;
    }
    const QString command = acpEffectiveCommand(settings);
    return QFileInfo(command).fileName() == u"vibe-acp"_s;
}

QString acpAgentDisplayName(const Settings &settings)
{
    if (!usesAcpNative(settings)) {
        return i18n("ACP");
    }
    const QJsonArray installed = QJsonDocument::fromJson(settings.acpInstalledAgents.toUtf8()).array();
    for (const QJsonValue &value : installed) {
        const QJsonObject agent = value.toObject();
        if (agent.value(u"id"_s).toString() == settings.acpAgentId) {
            const QString name = agent.value(u"name"_s).toString().trimmed();
            if (!name.isEmpty()) return name;
        }
    }
    const AcpAgentPreset preset = acpAgentPreset(settings.acpAgentId);
    if (preset.id != u"custom"_s && !preset.label.isEmpty()) {
        return preset.label;
    }
    const QString command = acpEffectiveCommand(settings);
    if (!command.isEmpty()) {
        return QFileInfo(command).fileName();
    }
    return i18n("ACP agent");
}

QString acpAgentIdMatching(const QString &command, const QString &args)
{
    const QString trimmedCommand = command.trimmed();
    const QString trimmedArgs = args.trimmed();
    const QList<AcpAgentPreset> presets = acpAgentPresets();
    for (const AcpAgentPreset &preset : presets) {
        if (preset.id == u"custom"_s) {
            continue;
        }
        if (trimmedCommand == preset.command && trimmedArgs == preset.args) {
            return preset.id;
        }
    }
    if (trimmedCommand.isEmpty() || (trimmedCommand == u"grok"_s && (trimmedArgs.isEmpty() || trimmedArgs == u"agent stdio"_s))) {
        return u"grok-build"_s;
    }
    if (trimmedCommand == u"mcode"_s && (trimmedArgs.isEmpty() || trimmedArgs == u"acp"_s)) {
        return u"mcode"_s;
    }
    const QString cmdBase = QFileInfo(trimmedCommand).baseName().toLower();
    if ((cmdBase == u"agy"_s || trimmedCommand.contains(u"agy_acp_server"_s))
        && (trimmedArgs.isEmpty() || trimmedArgs == u"acp"_s || trimmedArgs.contains(u"agy_acp_server"_s))) {
        return u"antigravity-acp"_s;
    }
    return u"custom"_s;
}

QStringList acpAgentAvailableModels(const Settings &settings, const QString &agentId)
{
    QStringList models;
    const QString effectiveAgentId = agentId.isEmpty() ? settings.acpAgentId : agentId;

    // Populated when a session reports configOptions or the legacy models field.
    const QJsonDocument doc = QJsonDocument::fromJson(settings.acpAgentModels.toUtf8());
    if (doc.isObject()) {
        const QJsonValue modelsValue = doc.object().value(effectiveAgentId);
        if (modelsValue.isArray()) {
            for (const QJsonValue &modelValue : modelsValue.toArray()) {
                const QString model = modelValue.toString().trimmed();
                if (!model.isEmpty() && !models.contains(model)) {
                    models.append(model);
                }
            }
        }
    }

    if (effectiveAgentId == settings.acpAgentId && !settings.acpModel.isEmpty() && !models.contains(settings.acpModel)) {
        models.prepend(settings.acpModel);
    }

    return models;
}

} // namespace KateAi
