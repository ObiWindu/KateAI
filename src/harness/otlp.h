/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "telemetry.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

class QNetworkReply;

namespace KateAi
{

/**
 * Opt-in OTLP/HTTP JSON export.
 *
 * The default endpoint is empty, so a fresh install never talks to a network
 * collector. Export failures are reported through a signal and never touch a
 * turn: observability is best-effort, the conversation is not.
 */
class OtlpExporter : public QObject
{
    Q_OBJECT

public:
    explicit OtlpExporter(QObject *parent = nullptr);

    void setEndpoint(const QUrl &endpoint);
    QUrl endpoint() const
    {
        return m_endpoint;
    }
    void setServiceName(const QString &name);
    void setHeaders(const QJsonObject &headers);
    void setMaxAttempts(int attempts);

    bool isConfigured() const;

    // Pure conversion, exposed for golden tests. `serviceVersion` is included
    // in the OTLP scope so a collector can separate plugin releases.
    static QJsonObject payload(const QList<Span> &spans, const QString &serviceName,
                               const QString &serviceVersion);

    // Posts a batch. Returns the request id; 0 means no request was made.
    int exportSpans(const QList<Span> &spans);
    void cancel();

Q_SIGNALS:
    // httpStatus is 0 when the request never reached the collector.
    void exportFinished(bool ok, int httpStatus, const QString &error);

private:
    void send(const QList<Span> &spans);
    QUrl tracesEndpoint() const;

    QNetworkAccessManager m_network;
    QUrl m_endpoint;
    QString m_serviceName = QStringLiteral("kateai");
    QJsonObject m_headers;
    int m_maxAttempts = 3;
    int m_attempt = 0;
    int m_requestId = 0;
    QNetworkReply *m_reply = nullptr;
    QList<Span> m_pending;
    bool m_cancelled = false;
};

} // namespace KateAi
