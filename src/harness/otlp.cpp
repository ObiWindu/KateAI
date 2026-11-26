/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "otlp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

QJsonValue otlpValue(const QJsonValue &value)
{
    if (value.isBool()) {
        return QJsonObject{{u"boolValue"_s, value.toBool()}};
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == static_cast<double>(static_cast<qint64>(number))) {
            return QJsonObject{{u"intValue"_s, QString::number(static_cast<qint64>(number))}};
        }
        return QJsonObject{{u"doubleValue"_s, number}};
    }
    if (value.isString()) {
        return QJsonObject{{u"stringValue"_s, value.toString()}};
    }
    if (value.isArray() || value.isObject()) {
        return QJsonObject{
            {u"stringValue"_s,
             QString::fromUtf8(QJsonDocument(
                                   value.isArray() ? QJsonDocument(value.toArray())
                                                   : QJsonDocument(value.toObject()))
                                   .toJson(QJsonDocument::Compact))}};
    }
    return QJsonObject{{u"stringValue"_s, QString()}};
}

QJsonArray otlpAttributes(const QJsonObject &attributes)
{
    QJsonArray array;
    for (auto it = attributes.constBegin(); it != attributes.constEnd(); ++it) {
        array.append(QJsonObject{{u"key"_s, it.key()}, {u"value"_s, otlpValue(it.value())}});
    }
    return array;
}

QString unixNano(qint64 milliseconds)
{
    return QString::number(milliseconds * 1000000LL);
}

} // namespace

OtlpExporter::OtlpExporter(QObject *parent)
    : QObject(parent)
{
}

void OtlpExporter::setEndpoint(const QUrl &endpoint)
{
    m_endpoint = endpoint;
}

void OtlpExporter::setServiceName(const QString &name)
{
    m_serviceName = name.isEmpty() ? u"kateai"_s : name;
}

void OtlpExporter::setHeaders(const QJsonObject &headers)
{
    m_headers = headers;
}

void OtlpExporter::setMaxAttempts(int attempts)
{
    m_maxAttempts = qBound(1, attempts, 10);
}

bool OtlpExporter::isConfigured() const
{
    return m_endpoint.isValid() && !m_endpoint.isEmpty()
        && (m_endpoint.scheme() == u"http"_s || m_endpoint.scheme() == u"https"_s);
}

QUrl OtlpExporter::tracesEndpoint() const
{
    QUrl url = m_endpoint;
    QString path = url.path();
    if (!path.endsWith(u"/v1/traces"_s)) {
        if (!path.endsWith(u'/')) {
            path += u'/';
        }
        path += u"v1/traces"_s;
        url.setPath(path);
    }
    return url;
}

QJsonObject OtlpExporter::payload(const QList<Span> &spans, const QString &serviceName,
                                  const QString &serviceVersion)
{
    QJsonArray otlpSpans;
    for (const Span &span : spans) {
        QJsonObject object;
        object.insert(u"traceId"_s, span.otlpTraceId());
        object.insert(u"spanId"_s, span.otlpSpanId());
        if (!span.parentSpanId.isEmpty()) {
            object.insert(u"parentSpanId"_s, span.otlpParentSpanId());
        }
        object.insert(u"name"_s, span.name);
        object.insert(u"kind"_s, span.kind == u"chat"_s ? 3 : 1); // CLIENT / INTERNAL
        object.insert(u"startTimeUnixNano"_s, unixNano(span.startMs));
        object.insert(u"endTimeUnixNano"_s, unixNano(qMax(span.endMs, span.startMs)));
        object.insert(u"attributes"_s, otlpAttributes(span.attributes));
        QJsonObject status;
        status.insert(u"code"_s, span.open ? 2 : 1); // ERROR / OK
        object.insert(u"status"_s, status);
        otlpSpans.append(object);
    }

    QJsonObject resource;
    resource.insert(u"attributes"_s, QJsonArray{
        QJsonObject{{u"key"_s, u"service.name"_s},
                    {u"value"_s, QJsonObject{{u"stringValue"_s, serviceName}}}},
        QJsonObject{{u"key"_s, u"service.version"_s},
                    {u"value"_s, QJsonObject{{u"stringValue"_s, serviceVersion}}}},
    });

    QJsonObject scope;
    scope.insert(u"name"_s, u"kateai.harness"_s);
    scope.insert(u"version"_s, serviceVersion);

    QJsonObject scopeSpans;
    scopeSpans.insert(u"scope"_s, scope);
    scopeSpans.insert(u"spans"_s, otlpSpans);

    QJsonObject resourceSpans;
    resourceSpans.insert(u"resource"_s, resource);
    resourceSpans.insert(u"scopeSpans"_s, QJsonArray{scopeSpans});

    return QJsonObject{{u"resourceSpans"_s, QJsonArray{resourceSpans}}};
}

int OtlpExporter::exportSpans(const QList<Span> &spans)
{
    if (!isConfigured() || spans.isEmpty()) {
        return 0;
    }
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
    m_pending = spans;
    m_attempt = 0;
    m_cancelled = false;
    ++m_requestId;
    send(m_pending);
    return m_requestId;
}

void OtlpExporter::cancel()
{
    m_cancelled = true;
    m_pending.clear();
    if (m_reply) {
        m_reply->abort();
    }
}

void OtlpExporter::send(const QList<Span> &spans)
{
    if (m_cancelled) {
        return;
    }
    ++m_attempt;
    QNetworkRequest request(tracesEndpoint());
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // A collector that accepts the connection and then stalls must not pin the
    // span batch and the request body for the lifetime of the loop.
    request.setTransferTimeout(30000);
    for (auto it = m_headers.constBegin(); it != m_headers.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    }
#ifdef KATEAI_VERSION
    const QString version = QStringLiteral(KATEAI_VERSION);
#else
    const QString version = u"1.0.0"_s;
#endif
    const QByteArray body = QJsonDocument(payload(spans, m_serviceName, version))
                                .toJson(QJsonDocument::Compact);
    m_reply = m_network.post(request, body);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (!m_reply) {
            return;
        }
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = reply->error() == QNetworkReply::NoError
            ? QString()
            : reply->errorString();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        reply->deleteLater();
        if (ok) {
            m_pending.clear();
            Q_EMIT exportFinished(true, status, QString());
            return;
        }
        if (!m_cancelled && m_attempt < m_maxAttempts) {
            const int delay = 500 * m_attempt;
            QTimer::singleShot(delay, this, [this] {
                send(m_pending);
            });
            return;
        }
        // The retry budget is spent; release the batch before reporting the
        // failure. The exporter is usually deleted right after the signal.
        m_pending.clear();
        Q_EMIT exportFinished(false, status, error);
    });
}

} // namespace KateAi
