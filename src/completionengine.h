/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace KateAi
{

// The editor state one completion request is built from. The cursor position
// travels with the request: a response that arrives after the cursor moved is
// dropped by the caller through isStale(), never rendered as a suggestion for
// a place the user has already left.
struct CompletionPromptContext {
    // Code before the cursor, snapped to a line start, at most the configured
    // number of characters.
    QString prefix;
    // Code after the cursor, snapped to a line start/end, at most the
    // configured number of characters.
    QString suffix;
    QString language;
    QString path;
    int cursorLine = 0;
    int cursorColumn = 0;
};

/**
 * Local, cheap completions at the cursor — the requestable half of the
 * in-editor completion feature. Knows nothing about Kate views: it turns a
 * buffer plus a cursor into a provider request and hands the answer back with
 * the context it was built at.
 *
 * Deliberately separate from AgentLoop: completions must not interleave with a
 * running chat turn (setAgentBusy() is a hard off-switch), and a chat abort
 * must never kill an unrelated completion.
 *
 * One in-flight request at a time; requesting again cancels the previous one.
 * The per-minute budget is enforced here, not in the UI.
 */
class CompletionEngine : public QObject
{
    Q_OBJECT

public:
    explicit CompletionEngine(QObject *parent = nullptr);
    ~CompletionEngine() override;

    void setSettings(Settings settings)
    {
        m_settings = std::move(settings);
    }
    // Hard off-switch while the chat agent is streaming.
    void setAgentBusy(bool busy)
    {
        m_agentBusy = busy;
    }
    // True when the budget window has room and the agent is not streaming.
    bool canRequestNow() const;

    // Cancels any in-flight request and issues a new one for the context.
    void request(const CompletionPromptContext &context);
    void cancel();
    bool isBusy() const
    {
        return m_reply != nullptr;
    }

    // --- Pure helpers, shared with the tests --------------------------------

    // Builds the prefix/suffix pair from a document's text and a cursor,
    // bounded by the configured character budgets and snapped to line starts
    // so the prompt never starts or ends mid-statement by accident.
    static CompletionPromptContext buildContext(const QString &text, int cursorLine, int cursorColumn,
                                                const QString &path, const QString &language,
                                                int prefixChars, int suffixChars);
    // The chat-style prompt: a system message that pins the model to "output
    // only the completion", and one user message carrying prefix and suffix.
    static QList<ChatMessage> buildMessages(const CompletionPromptContext &context);
    // Sequences that stop a chatty model from explaining itself.
    static QStringList stopSequences();
    // A completion response is only usable at the position it was requested
    // for; anything else is stale and must not be rendered.
    static bool isStale(const CompletionPromptContext &request, int cursorLine, int cursorColumn);
    // The FIM prompt for providers that expose a fill-in-the-middle
    // completion endpoint. Capability flag: providerSupportsFim().
    static QString fimPrompt(const CompletionPromptContext &context, Provider provider);

Q_SIGNALS:
    // The context is the one the request was built at; the caller drops the
    // result through isStale() when the cursor has moved since.
    void completed(const QString &completion, const KateAi::CompletionPromptContext &context);
    void failed(const QString &error);

private:
    void handleReplyFinished();
    void recordRequestTime();
    void pruneBudgetWindow() const;

    Settings m_settings;
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    CompletionPromptContext m_activeContext;
    bool m_agentBusy = false;
    // Start times (msecs since epoch) of the requests of the last minute.
    mutable QList<qint64> m_requestTimes;
};

} // namespace KateAi

Q_DECLARE_METATYPE(KateAi::CompletionPromptContext)
