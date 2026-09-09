#pragma once

#include "types.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>

class QNetworkReply;

namespace KateAi
{

class LlmClient : public QObject
{
    Q_OBJECT

public:
    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;

    void setSettings(Settings settings)
    {
        m_settings = std::move(settings);
    }

    bool isBusy() const
    {
        return m_reply != nullptr;
    }

    void complete(const QList<ChatMessage> &messages);
    void fetchModels(Provider provider);
    void abort();

    static QJsonArray messagesToJson(const QList<ChatMessage> &messages);
    static CompletionChunk parseSseLine(const QByteArray &line, QHash<int, ToolCall> *acc);

Q_SIGNALS:
    void textDelta(const QString &delta);
    void finished(const QString &fullText, const QList<ToolCall> &toolCalls);
    void failed(const QString &error);
    void modelsReceived(Provider provider, const QStringList &models);
    void modelsFailed(Provider provider, const QString &error);

private:
    void handleReadyRead();
    void handleFinished();
    void handleModelsFinished(QNetworkReply *reply, Provider provider);
    void resetCompletionState();
    void emitCompletedOnce();
    QList<ToolCall> completedToolsFromAccumulator();

    Settings m_settings;
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_buffer;
    QString m_text;
    QHash<int, ToolCall> m_toolAcc;
    QList<ToolCall> m_completedTools;
    QString m_completionError;
    bool m_sawDone = false;
    bool m_finishEmitted = false;
    bool m_abortRequested = false;
};

} // namespace KateAi
