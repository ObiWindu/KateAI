/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "args.h"

#include "types.h"

#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

const QStringList &providerIds()
{
    static const QStringList ids{
        u"grok"_s,          u"openai"_s,      u"openrouter"_s, u"deepseek"_s,
        u"openai-compatible"_s, u"claude-compatible"_s, u"kilo"_s, u"acp"_s,
        u"opencode"_s,
    };
    return ids;
}

bool isProviderId(const QString &id)
{
    return providerIds().contains(id);
}

QString takeValue(const QStringList &arguments, int *index, const QString &name,
                  const QString &inlineValue, bool hasInline, QString *error)
{
    if (hasInline) {
        return inlineValue;
    }
    if (*index + 1 >= arguments.size()) {
        if (error) {
            *error = u"%1 needs a value."_s.arg(name);
        }
        return QString();
    }
    ++(*index);
    return arguments.at(*index);
}

} // namespace

QString HeadlessOptions::usage()
{
    return u"Usage: kateai-headless [run|replay|acp] [options]\n"
           u"\n"
           u"Run one prompt non-interactively and write a patch, events and trace.\n"
           u"  -p, --prompt <text>          Prompt to run (required for run)\n"
           u"  -C, --workspace <dir>        Workspace root (default: current directory)\n"
           u"  -o, --output-dir <dir>       Artifact directory (events, summary, patch, trace)\n"
           u"      --provider <id>          grok|openai|openrouter|deepseek|openai-compatible|\n"
           u"                               claude-compatible|kilo|opencode\n"
           u"      --base-url <url>         Override the provider base URL\n"
           u"  -m, --model <id>             Model id\n"
           u"      --api-key <key>          API key for this run (never written to disk)\n"
           u"      --permission <mode>      ask|accept-edits|auto|bypass (default: auto)\n"
           u"      --sandbox <profile>      off|workspace|read-only|strict (default: workspace)\n"
           u"      --mode <id>              Agent mode (default: code)\n"
           u"      --max-turns <n>          Stop after n model requests (0 = unlimited)\n"
           u"      --max-cost <usd>         Soft budget; the turn completes with a notice\n"
           u"      --no-verify              Disable the verification gate for this run\n"
           u"      --no-persist             Do not create a conversation row in the session DB\n"
           u"      --conversation <id>      Continue an explicit conversation id\n"
           u"      --answer <text>          Answer the first question automatically\n"
           u"      --jsonl                  Stream harness events to stdout as JSONL\n"
           u"      --quiet                  Do not stream assistant text to stdout\n"
           u"\n"
           u"Replay a recorded event log without calling a model.\n"
           u"  kateai-headless replay --conversation <id> [--session-db <path>]\n"
           u"\n"
           u"Serve ACP over stdio for another editor or host.\n"
           u"  kateai-headless acp --stdio [--workspace <dir>] [provider options]\n"
           u"\n"
           u"Exit codes: 0 success, 1 error, 2 turn/verification failure,\n"
           u"            3 budget exhausted, 4 permission required, 5 question required.\n"_s;
}

bool HeadlessArgs::parse(const QStringList &arguments, HeadlessOptions *options, QString *error)
{
    if (!options) {
        if (error) {
            *error = u"No options object."_s;
        }
        return false;
    }
    HeadlessOptions parsed;
    bool modeExplicit = false;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == u"run"_s || argument == u"replay"_s || argument == u"acp"_s) {
            if (modeExplicit) {
                if (error) {
                    *error = u"Only one mode may be given."_s;
                }
                return false;
            }
            parsed.mode = argument;
            modeExplicit = true;
            continue;
        }
        if (argument == u"-h"_s || argument == u"--help"_s) {
            parsed.help = true;
            continue;
        }
        if (argument == u"-v"_s || argument == u"--version"_s) {
            parsed.version = true;
            continue;
        }
        if (!argument.startsWith(u'-')) {
            if (error) {
                *error = u"Unexpected argument: %1"_s.arg(argument);
            }
            return false;
        }

        QString name = argument;
        QString inlineValue;
        bool hasInline = false;
        const int equals = argument.indexOf(u'=');
        if (equals > 0) {
            name = argument.left(equals);
            inlineValue = argument.mid(equals + 1);
            hasInline = true;
        }

        auto value = [&](const QString &flag) {
            return takeValue(arguments, &i, flag, inlineValue, hasInline, error);
        };

        if (name == u"-p"_s || name == u"--prompt"_s) {
            parsed.prompt = value(name);
        } else if (name == u"-C"_s || name == u"--workspace"_s) {
            parsed.workspace = value(name);
        } else if (name == u"-o"_s || name == u"--output-dir"_s) {
            parsed.outputDir = value(name);
        } else if (name == u"--provider"_s) {
            parsed.provider = value(name);
            if (!isProviderId(parsed.provider)) {
                if (error) {
                    *error = u"Unknown provider '%1'. Known: %2"_s.arg(
                        parsed.provider, providerIds().join(u", "_s));
                }
                return false;
            }
        } else if (name == u"--base-url"_s) {
            parsed.baseUrl = value(name);
            const QUrl url(parsed.baseUrl);
            if (!url.isValid() || (url.scheme() != u"http"_s && url.scheme() != u"https"_s)) {
                if (error) {
                    *error = u"--base-url must be an http(s) URL."_s;
                }
                return false;
            }
        } else if (name == u"-m"_s || name == u"--model"_s) {
            parsed.model = value(name);
        } else if (name == u"--api-key"_s) {
            parsed.apiKey = value(name);
        } else if (name == u"--permission"_s) {
            parsed.permission = value(name).toLower();
            if (parsed.permission != u"ask"_s && parsed.permission != u"accept-edits"_s
                && parsed.permission != u"auto"_s && parsed.permission != u"bypass"_s) {
                if (error) {
                    *error = u"--permission must be ask, accept-edits, auto or bypass."_s;
                }
                return false;
            }
        } else if (name == u"--sandbox"_s) {
            parsed.sandbox = value(name).toLower();
            if (parsed.sandbox != u"off"_s && parsed.sandbox != u"workspace"_s
                && parsed.sandbox != u"read-only"_s && parsed.sandbox != u"strict"_s) {
                if (error) {
                    *error = u"--sandbox must be off, workspace, read-only or strict."_s;
                }
                return false;
            }
        } else if (name == u"--mode"_s) {
            parsed.modeId = value(name);
        } else if (name == u"--max-turns"_s) {
            bool ok = false;
            parsed.maxTurns = value(name).toInt(&ok);
            if (!ok || parsed.maxTurns < 0) {
                if (error) {
                    *error = u"--max-turns must be a non-negative integer."_s;
                }
                return false;
            }
        } else if (name == u"--max-cost"_s) {
            bool ok = false;
            parsed.maxCostUsd = value(name).toDouble(&ok);
            if (!ok || parsed.maxCostUsd < 0.0) {
                if (error) {
                    *error = u"--max-cost must be a non-negative number."_s;
                }
                return false;
            }
        } else if (name == u"--conversation"_s) {
            parsed.conversationId = value(name);
        } else if (name == u"--session-db"_s) {
            const QString path = value(name);
            qputenv("KATEAI_SESSION_DB", path.toUtf8());
        } else if (name == u"--answer"_s) {
            parsed.answer = value(name);
        } else if (name == u"--no-verify"_s) {
            parsed.verify = false;
        } else if (name == u"--verify"_s) {
            parsed.verify = true;
        } else if (name == u"--no-persist"_s) {
            parsed.persist = false;
        } else if (name == u"--jsonl"_s || name == u"--json"_s) {
            parsed.jsonl = true;
        } else if (name == u"--quiet"_s) {
            parsed.quiet = true;
        } else if (name == u"--stdio"_s) {
            parsed.stdio = true;
        } else {
            if (error) {
                *error = u"Unknown option: %1"_s.arg(name);
            }
            return false;
        }
        if (hasInline && name != u"--config"_s) {
            hasInline = false;
        }
    }
    *options = parsed;
    return true;
}

} // namespace KateAi
