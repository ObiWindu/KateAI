/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "voiceinput.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef Q_OS_UNIX
#include <signal.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

constexpr int kMaxRecordingMs = 5 * 60 * 1000;
constexpr int kHttpTimeoutMs = 60000;
constexpr int kMicProbeTimeoutMs = 2000;

std::function<VoiceInput::MicrophoneStatus(const QString &)> &probeOverride()
{
    static std::function<VoiceInput::MicrophoneStatus(const QString &)> fn;
    return fn;
}

quint16 readLe16(const uchar *p)
{
    return static_cast<quint16>(p[0]) | (static_cast<quint16>(p[1]) << 8);
}

quint32 readLe32(const uchar *p)
{
    return static_cast<quint32>(p[0])
        | (static_cast<quint32>(p[1]) << 8)
        | (static_cast<quint32>(p[2]) << 16)
        | (static_cast<quint32>(p[3]) << 24);
}

void appendLe16(QByteArray &out, quint16 v)
{
    out.append(static_cast<char>(v & 0xFF));
    out.append(static_cast<char>((v >> 8) & 0xFF));
}

void appendLe32(QByteArray &out, quint32 v)
{
    out.append(static_cast<char>(v & 0xFF));
    out.append(static_cast<char>((v >> 8) & 0xFF));
    out.append(static_cast<char>((v >> 16) & 0xFF));
    out.append(static_cast<char>((v >> 24) & 0xFF));
}

QStringList splitCommandLine(const QString &commandLine)
{
    QStringList tokens;
    QString current;
    QChar quote;
    bool escaped = false;

    for (int i = 0; i < commandLine.size(); ++i) {
        const QChar ch = commandLine.at(i);
        if (escaped) {
            current.append(ch);
            escaped = false;
            continue;
        }
        if (ch == u'\\' && quote != u'\'') {
            escaped = true;
            continue;
        }
        if (!quote.isNull()) {
            if (ch == quote) {
                quote = QChar();
            } else {
                current.append(ch);
            }
            continue;
        }
        if (ch == u'"' || ch == u'\'') {
            quote = ch;
            continue;
        }
        if (ch.isSpace()) {
            if (!current.isEmpty()) {
                tokens.append(current);
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        tokens.append(current);
    }
    return tokens;
}

QString findLocalWhisperCli()
{
    for (const QString &candidate : {u"whisper-cli"_s, u"whisper-cpp"_s, u"whisper"_s}) {
        const QString path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty()) {
            return path;
        }
    }
    return {};
}

QString normalizeTranscriptionEndpoint(const QString &rawUrl)
{
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (trimmed.endsWith(u"/audio/transcriptions"_s, Qt::CaseInsensitive)
        || trimmed.endsWith(u"/transcriptions"_s, Qt::CaseInsensitive)) {
        return trimmed;
    }
    return joinApiEndpoint(trimmed, u"/v1/audio/transcriptions"_s);
}

QString cleanTranscriptText(const QString &raw)
{
    QString text = raw;
    // Strip optional whisper.cpp timestamp prefixes like "[00:00:00.000 --> 00:00:03.200]  "
    static const QRegularExpression kTimestampRe(
        QStringLiteral(R"(^\s*\[\d{2}:\d{2}:\d{2}(?:\.\d+)?\s*-->\s*\d{2}:\d{2}:\d{2}(?:\.\d+)?\]\s*)"),
        QRegularExpression::MultilineOption);
    text.replace(kTimestampRe, QString());
    // Collapse consecutive blank lines/whitespace into clean prose
    const QStringList lines = text.split(u'\n');
    QStringList kept;
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (!t.isEmpty()) {
            kept.append(t);
        }
    }
    return kept.join(u' ').trimmed();
}

bool matchesConfiguredDevice(const QString &configured, const QString &name, const QString &description)
{
    const QString want = configured.trimmed();
    if (want.isEmpty()) {
        return true;
    }
    return name.compare(want, Qt::CaseInsensitive) == 0
        || description.compare(want, Qt::CaseInsensitive) == 0
        || name.contains(want, Qt::CaseInsensitive)
        || description.contains(want, Qt::CaseInsensitive);
}

bool isAlsaCaptureAccessible()
{
#ifdef Q_OS_LINUX
    const QDir sndDir(u"/dev/snd"_s);
    if (!sndDir.exists()) {
        return false;
    }
    const QFileInfoList entries = sndDir.entryInfoList({u"pcmC*c"_s}, QDir::System | QDir::Files);
    if (entries.isEmpty()) {
        return true; // Non-standard ALSA plugin/virtual device
    }
    for (const QFileInfo &fi : entries) {
        if (fi.isReadable()) {
            return true;
        }
    }
    return false;
#else
    return true;
#endif
}

QString noMicDefaultReason()
{
    return i18n("No microphone is available or usable.");
}

} // namespace

VoiceInput::VoiceInput(QObject *parent)
    : QObject(parent)
{
    m_micStatus.available = false;
    m_micStatus.reason = noMicDefaultReason();

    m_network = new QNetworkAccessManager(this);

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(250);
    connect(m_tickTimer, &QTimer::timeout, this, [this]() {
        if (m_state != State::Recording) {
            m_tickTimer->stop();
            return;
        }
        const int elapsed = static_cast<int>(m_recordElapsed.elapsed());
        Q_EMIT recordingTick(elapsed);
        if (elapsed >= kMaxRecordingMs) {
            stopAndProcess();
        }
    });

    m_stopTimeout = new QTimer(this);
    m_stopTimeout->setSingleShot(true);
    m_stopTimeout->setInterval(2500);
    connect(m_stopTimeout, &QTimer::timeout, this, [this]() {
        if (m_recordProcess && m_recordProcess->state() != QProcess::NotRunning) {
            m_recordProcess->terminate();
            if (!m_recordProcess->waitForFinished(400)) {
                m_recordProcess->kill();
            }
        }
    });

    m_httpTimeout = new QTimer(this);
    m_httpTimeout->setSingleShot(true);
    m_httpTimeout->setInterval(kHttpTimeoutMs);
    connect(m_httpTimeout, &QTimer::timeout, this, [this]() {
        if (m_reply) {
            m_reply->abort();
        }
    });
}

VoiceInput::~VoiceInput()
{
    cancelMicProbe();
    cleanupSession();
}

void VoiceInput::setSettings(const Settings &settings)
{
    const bool deviceChanged = m_settings.voiceInputDevice.trimmed() != settings.voiceInputDevice.trimmed();
    m_settings = settings;
    if (deviceChanged && m_state == State::Idle) {
        refreshMicrophoneAvailability();
    }
}

void VoiceInput::setMicrophoneProbeOverrideForTesting(std::function<MicrophoneStatus(const QString &)> probe)
{
    probeOverride() = std::move(probe);
}

void VoiceInput::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(state);
}

void VoiceInput::applyMicrophoneStatus(const MicrophoneStatus &status, bool forceEmit)
{
    const bool changed = forceEmit || !m_micProbeCompleted
        || m_micStatus.available != status.available
        || m_micStatus.deviceName != status.deviceName
        || m_micStatus.reason != status.reason
        || m_micStatus.backend != status.backend;
    m_micStatus = status;
    m_micProbeCompleted = true;
    if (changed) {
        const QString detail = status.available
            ? (status.deviceName.isEmpty() ? i18n("Microphone ready") : status.deviceName)
            : (status.reason.isEmpty() ? noMicDefaultReason() : status.reason);
        Q_EMIT microphoneAvailabilityChanged(status.available, detail);
    }
}

void VoiceInput::cancelMicProbe()
{
    m_micProbeInFlight = false;
    m_micProbeStep = ProbeStep::None;
    if (m_micProbeProcess) {
        m_micProbeProcess->disconnect(this);
        if (m_micProbeProcess->state() != QProcess::NotRunning) {
            m_micProbeProcess->kill();
            m_micProbeProcess->waitForFinished(200);
        }
        m_micProbeProcess->deleteLater();
        m_micProbeProcess = nullptr;
    }
}

void VoiceInput::refreshMicrophoneAvailability()
{
    if (m_micProbeInFlight) {
        return;
    }
    if (probeOverride()) {
        applyMicrophoneStatus(probeOverride()(m_settings.voiceInputDevice.trimmed()), !m_micProbeCompleted);
        return;
    }

    QStringList dummyArgs;
    if (detectCaptureCommand(&dummyArgs, u"/dev/null"_s, m_settings.voiceInputDevice).isEmpty()) {
        MicrophoneStatus status;
        status.available = false;
        status.reason = i18n("No microphone is available or usable: no audio capture tool found (install pipewire, pulseaudio-utils, alsa-utils, or ffmpeg).");
        applyMicrophoneStatus(status, !m_micProbeCompleted);
        return;
    }

    m_micProbeInFlight = true;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    runNextMicProbeStep(ProbeStep::Ffmpeg);
#else
    runNextMicProbeStep(ProbeStep::Pactl);
#endif
}

void VoiceInput::runNextMicProbeStep(ProbeStep step)
{
    if (!m_micProbeInFlight) {
        return;
    }
    m_micProbeStep = step;

    QString program;
    QStringList args;
    ProbeStep fallbackStep = ProbeStep::None;

    switch (step) {
    case ProbeStep::Pactl:
        program = QStandardPaths::findExecutable(u"pactl"_s);
        args = {u"list"_s, u"sources"_s};
        fallbackStep = ProbeStep::Wpctl;
        break;
    case ProbeStep::Wpctl:
        program = QStandardPaths::findExecutable(u"wpctl"_s);
        args = {u"status"_s};
        fallbackStep = ProbeStep::Arecord;
        break;
    case ProbeStep::Arecord:
        program = QStandardPaths::findExecutable(u"arecord"_s);
        args = {u"-l"_s};
        fallbackStep = ProbeStep::None;
        break;
    case ProbeStep::Ffmpeg:
        program = QStandardPaths::findExecutable(u"ffmpeg"_s);
#if defined(Q_OS_MACOS)
        args = {u"-hide_banner"_s, u"-f"_s, u"avfoundation"_s, u"-list_devices"_s, u"true"_s, u"-i"_s, QString()};
#elif defined(Q_OS_WIN)
        args = {u"-hide_banner"_s, u"-list_devices"_s, u"true"_s, u"-f"_s, u"dshow"_s, u"-i"_s, u"dummy"_s};
#else
        args = {};
#endif
        fallbackStep = ProbeStep::None;
        break;
    case ProbeStep::None:
        break;
    }

    if (program.isEmpty()) {
        if (fallbackStep != ProbeStep::None) {
            runNextMicProbeStep(fallbackStep);
        } else {
            MicrophoneStatus status;
            status.available = false;
            status.reason = i18n("No microphone is available or usable: unable to detect an audio input device.");
            finishMicProbe(status);
        }
        return;
    }

    auto *proc = new QProcess(this);
    m_micProbeProcess = proc;
    const QString configuredDevice = m_settings.voiceInputDevice.trimmed();

    QTimer *timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    timeout->setInterval(kMicProbeTimeoutMs);
    connect(timeout, &QTimer::timeout, proc, [proc]() {
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
        }
    });

    connect(proc, &QProcess::errorOccurred, this, [this, proc, fallbackStep](QProcess::ProcessError) {
        if (m_micProbeProcess != proc) {
            return;
        }
        m_micProbeProcess = nullptr;
        proc->deleteLater();
        if (fallbackStep != ProbeStep::None) {
            runNextMicProbeStep(fallbackStep);
        } else {
            MicrophoneStatus status;
            status.available = false;
            status.reason = i18n("No microphone is available or usable.");
            finishMicProbe(status);
        }
    });

    connect(proc, &QProcess::finished, this,
            [this, proc, step, fallbackStep, configuredDevice](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_micProbeProcess != proc) {
                    return;
                }
                const QString out = QString::fromUtf8(proc->readAllStandardOutput());
                const QString err = QString::fromUtf8(proc->readAllStandardError());
                m_micProbeProcess = nullptr;
                proc->deleteLater();

                if (step == ProbeStep::Pactl) {
                    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                        const MicrophoneStatus parsed = parsePactlSourcesOutput(out, configuredDevice);
                        if (parsed.available) {
                            finishMicProbe(parsed);
                            return;
                        }
                        // If a PulseAudio/PipeWire daemon responded cleanly with zero real
                        // microphones, check arecord only if the user explicitly configured
                        // an ALSA device name (e.g., hw:0,0).
                        if (configuredDevice.startsWith(u"hw:"_s, Qt::CaseInsensitive)
                            || configuredDevice.startsWith(u"plughw:"_s, Qt::CaseInsensitive)) {
                            runNextMicProbeStep(ProbeStep::Arecord);
                            return;
                        }
                        finishMicProbe(parsed);
                        return;
                    }
                    runNextMicProbeStep(fallbackStep);
                    return;
                }

                if (step == ProbeStep::Wpctl) {
                    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                        const MicrophoneStatus parsed = parseWpctlStatusOutput(out, configuredDevice);
                        if (parsed.available || !out.contains(u"Audio"_s)) {
                            finishMicProbe(parsed);
                            return;
                        }
                        finishMicProbe(parsed);
                        return;
                    }
                    runNextMicProbeStep(fallbackStep);
                    return;
                }

                if (step == ProbeStep::Arecord) {
                    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                        MicrophoneStatus parsed = parseArecordListOutput(out, configuredDevice);
                        if (parsed.available && !isAlsaCaptureAccessible()) {
                            parsed.available = false;
                            parsed.reason = i18n("No microphone is available or usable: permission denied accessing audio capture devices.");
                        }
                        finishMicProbe(parsed);
                        return;
                    }
                    MicrophoneStatus status;
                    status.available = false;
                    status.reason = i18n("No microphone is available or usable: no audio capture device detected.");
                    finishMicProbe(status);
                    return;
                }

                if (step == ProbeStep::Ffmpeg) {
                    // ffmpeg -list_devices writes the device table to stderr and exits non-zero.
                    finishMicProbe(parseFfmpegDevicesOutput(out + u"\n"_s + err, configuredDevice));
                    return;
                }
            });

    timeout->start();
    proc->start(program, args);
}

void VoiceInput::finishMicProbe(const MicrophoneStatus &status)
{
    m_micProbeInFlight = false;
    m_micProbeStep = ProbeStep::None;
    applyMicrophoneStatus(status, !m_micProbeCompleted);
}

VoiceInput::MicrophoneStatus VoiceInput::parsePactlSourcesOutput(const QString &output, const QString &configuredDevice)
{
    MicrophoneStatus result;
    result.backend = u"pulse"_s;

    struct SourceInfo {
        QString name;
        QString description;
        QString monitorOfSink;
        QString deviceClass;
        QString mediaClass;
        QString activePort;
        bool hasPorts = false;
        bool activePortUnavailable = false;
        bool anyPortAvailable = false;
    };

    QList<SourceInfo> sources;
    SourceInfo current;
    bool inSource = false;
    bool inPorts = false;

    const QStringList lines = output.split(u'\n');
    for (const QString &rawLine : lines) {
        const QString trimmed = rawLine.trimmed();
        if (rawLine.startsWith(u"Source #"_s)) {
            if (inSource) {
                sources.append(current);
            }
            current = SourceInfo{};
            inSource = true;
            inPorts = false;
            continue;
        }
        if (!inSource) {
            continue;
        }
        if (trimmed == u"Ports:"_s) {
            inPorts = true;
            current.hasPorts = true;
            continue;
        }
        if (inPorts && rawLine.startsWith(u"\t\t"_s)) {
            const int colon = trimmed.indexOf(u':');
            const QString portName = colon > 0 ? trimmed.left(colon).trimmed() : QString();
            const bool unavailable = trimmed.contains(u"not available"_s, Qt::CaseInsensitive);
            if (!unavailable) {
                current.anyPortAvailable = true;
            }
            if (!portName.isEmpty() && !current.activePort.isEmpty() && portName == current.activePort) {
                current.activePortUnavailable = unavailable;
            }
            continue;
        }
        if (inPorts && !rawLine.startsWith(u"\t\t"_s)) {
            inPorts = false;
        }
        if (trimmed.startsWith(u"Name:"_s)) {
            current.name = trimmed.mid(5).trimmed();
        } else if (trimmed.startsWith(u"Description:"_s)) {
            current.description = trimmed.mid(12).trimmed();
        } else if (trimmed.startsWith(u"Monitor of Sink:"_s)) {
            current.monitorOfSink = trimmed.mid(16).trimmed();
        } else if (trimmed.startsWith(u"Active Port:"_s)) {
            current.activePort = trimmed.mid(12).trimmed();
        } else if (trimmed.startsWith(u"device.class ="_s)) {
            current.deviceClass = trimmed.section(u'"', 1, 1);
        } else if (trimmed.startsWith(u"media.class ="_s)) {
            current.mediaClass = trimmed.section(u'"', 1, 1);
        }
    }
    if (inSource) {
        sources.append(current);
    }

    // Re-evaluate activePortUnavailable because Active Port appears after Ports: in pactl output.
    if (!sources.isEmpty()) {
        int sourceIdx = -1;
        bool scanningPorts = false;
        for (const QString &rawLine : lines) {
            const QString trimmed = rawLine.trimmed();
            if (rawLine.startsWith(u"Source #"_s)) {
                ++sourceIdx;
                scanningPorts = false;
                continue;
            }
            if (sourceIdx < 0 || sourceIdx >= sources.size()) {
                continue;
            }
            if (trimmed == u"Ports:"_s) {
                scanningPorts = true;
                continue;
            }
            if (scanningPorts && rawLine.startsWith(u"\t\t"_s)) {
                const int colon = trimmed.indexOf(u':');
                const QString portName = colon > 0 ? trimmed.left(colon).trimmed() : QString();
                if (!portName.isEmpty() && portName == sources[sourceIdx].activePort
                    && trimmed.contains(u"not available"_s, Qt::CaseInsensitive)) {
                    sources[sourceIdx].activePortUnavailable = true;
                }
            } else if (scanningPorts) {
                scanningPorts = false;
            }
        }
    }

    bool sawUnpluggedMic = false;
    bool sawAnyRealMic = false;

    for (const SourceInfo &src : sources) {
        if (src.name.isEmpty()) {
            continue;
        }
        // Skip sink loopback monitors and dummy null sources.
        const bool isMonitor = src.name.endsWith(u".monitor"_s, Qt::CaseInsensitive)
            || (!src.monitorOfSink.isEmpty() && src.monitorOfSink.compare(u"n/a"_s, Qt::CaseInsensitive) != 0)
            || src.deviceClass.compare(u"monitor"_s, Qt::CaseInsensitive) == 0
            || src.mediaClass.compare(u"Audio/Sink"_s, Qt::CaseInsensitive) == 0;
        if (isMonitor) {
            continue;
        }
        if (src.name.compare(u"auto_null"_s, Qt::CaseInsensitive) == 0) {
            continue;
        }
        sawAnyRealMic = true;

        // If every port on the device is explicitly marked "not available" (unplugged jack),
        // treat this source as unusable.
        if (src.hasPorts && src.activePortUnavailable && !src.anyPortAvailable) {
            sawUnpluggedMic = true;
            continue;
        }

        if (!matchesConfiguredDevice(configuredDevice, src.name, src.description)) {
            continue;
        }

        result.available = true;
        result.deviceName = src.description.isEmpty() ? src.name : src.description;
        return result;
    }

    result.available = false;
    if (!configuredDevice.trimmed().isEmpty() && sawAnyRealMic) {
        result.reason = i18n("No microphone is available or usable: configured input device '%1' was not found.",
                             configuredDevice.trimmed());
    } else if (sawUnpluggedMic) {
        result.reason = i18n("No microphone is available or usable: microphone port is unplugged or unavailable.");
    } else {
        result.reason = i18n("No microphone is available or usable: no audio input device detected.");
    }
    return result;
}

VoiceInput::MicrophoneStatus VoiceInput::parseWpctlStatusOutput(const QString &output, const QString &configuredDevice)
{
    MicrophoneStatus result;
    result.backend = u"pipewire"_s;

    bool inAudio = false;
    bool inSources = false;
    const QStringList lines = output.split(u'\n');
    static const QRegularExpression kSourceEntryRe(QStringLiteral(R"((\*)?\s*(\d+)\.\s+(.+?)(?:\s+\[.*)?$)"));

    for (const QString &rawLine : lines) {
        const QString trimmed = rawLine.trimmed();
        if (trimmed == u"Audio"_s) {
            inAudio = true;
            inSources = false;
            continue;
        }
        if (inAudio && (trimmed == u"Video"_s || trimmed == u"Settings"_s)) {
            inAudio = false;
            inSources = false;
            continue;
        }
        if (!inAudio) {
            continue;
        }
        if (trimmed.contains(u"Sources:"_s)) {
            inSources = true;
            continue;
        }
        if (inSources && (trimmed.contains(u"Sinks:"_s) || trimmed.contains(u"Filters:"_s)
                          || trimmed.contains(u"Streams:"_s) || trimmed.contains(u"Devices:"_s))) {
            inSources = false;
            continue;
        }
        if (!inSources) {
            continue;
        }
        // Strip tree drawing characters (│, ├, └, ─)
        QString cleaned = trimmed;
        cleaned.remove(QRegularExpression(QStringLiteral(R"([│├└─])")));
        cleaned = cleaned.trimmed();
        if (cleaned.isEmpty()) {
            continue;
        }
        const QRegularExpressionMatch m = kSourceEntryRe.match(cleaned);
        if (!m.hasMatch()) {
            continue;
        }
        const QString name = m.captured(3).trimmed();
        if (name.isEmpty() || name.contains(u"Dummy"_s, Qt::CaseInsensitive)
            || name.contains(u"Monitor of"_s, Qt::CaseInsensitive)) {
            continue;
        }
        if (!matchesConfiguredDevice(configuredDevice, name, name)) {
            continue;
        }
        result.available = true;
        result.deviceName = name;
        return result;
    }

    result.available = false;
    result.reason = configuredDevice.trimmed().isEmpty()
        ? i18n("No microphone is available or usable: no PipeWire audio input source detected.")
        : i18n("No microphone is available or usable: configured input device '%1' was not found.",
               configuredDevice.trimmed());
    return result;
}

VoiceInput::MicrophoneStatus VoiceInput::parseArecordListOutput(const QString &output, const QString &configuredDevice)
{
    MicrophoneStatus result;
    result.backend = u"alsa"_s;

    static const QRegularExpression kCardRe(
        QStringLiteral(R"(^card\s+(\d+):\s+([^\[]+)\[([^\]]+)\],\s*device\s+(\d+):\s+([^\[]+)\[([^\]]+)\])"));

    const QStringList lines = output.split(u'\n');
    for (const QString &rawLine : lines) {
        const QRegularExpressionMatch m = kCardRe.match(rawLine.trimmed());
        if (!m.hasMatch()) {
            continue;
        }
        const QString cardNum = m.captured(1);
        const QString cardLabel = m.captured(3).trimmed();
        const QString devNum = m.captured(4);
        const QString devLabel = m.captured(6).trimmed();
        const QString hwId = QStringLiteral("hw:%1,%2").arg(cardNum, devNum);
        const QString fullName = QStringLiteral("%1 — %2").arg(cardLabel, devLabel);
        if (!matchesConfiguredDevice(configuredDevice, hwId, fullName)) {
            continue;
        }
        result.available = true;
        result.deviceName = fullName;
        return result;
    }

    result.available = false;
    result.reason = i18n("No microphone is available or usable: no ALSA capture device detected.");
    return result;
}

VoiceInput::MicrophoneStatus VoiceInput::parseFfmpegDevicesOutput(const QString &output, const QString &configuredDevice)
{
    MicrophoneStatus result;
#if defined(Q_OS_MACOS)
    result.backend = u"avfoundation"_s;
#elif defined(Q_OS_WIN)
    result.backend = u"dshow"_s;
#endif

    // macOS AVFoundation format:
    // [AVFoundation indev @ ...] AVFoundation audio devices:
    // [AVFoundation indev @ ...] [0] MacBook Pro Microphone
    bool inAvfAudio = false;
    static const QRegularExpression kAvfDevRe(QStringLiteral(R"(\[(\d+)\]\s+(.+)$)"));
    // Windows DirectShow format:
    // [dshow @ ...] "Microphone (USB Audio)" (audio)
    static const QRegularExpression kDshowRe(QStringLiteral(R"(\"([^\"]+)\"\s+\(audio\))"));

    const QStringList lines = output.split(u'\n');
    for (const QString &rawLine : lines) {
        const QString trimmed = rawLine.trimmed();
        if (trimmed.contains(u"AVFoundation audio devices:"_s, Qt::CaseInsensitive)) {
            inAvfAudio = true;
            continue;
        }
        if (trimmed.contains(u"AVFoundation video devices:"_s, Qt::CaseInsensitive)) {
            inAvfAudio = false;
            continue;
        }
        if (inAvfAudio) {
            const QRegularExpressionMatch m = kAvfDevRe.match(trimmed);
            if (m.hasMatch()) {
                const QString id = m.captured(1);
                const QString name = m.captured(2).trimmed();
                if (matchesConfiguredDevice(configuredDevice, id, name)) {
                    result.available = true;
                    result.deviceName = name;
                    result.backend = u"avfoundation"_s;
                    return result;
                }
            }
        }
        const QRegularExpressionMatch dm = kDshowRe.match(trimmed);
        if (dm.hasMatch()) {
            const QString name = dm.captured(1).trimmed();
            if (matchesConfiguredDevice(configuredDevice, name, name)) {
                result.available = true;
                result.deviceName = name;
                result.backend = u"dshow"_s;
                return result;
            }
        }
    }

    result.available = false;
    result.reason = i18n("No microphone is available or usable: no audio input device detected.");
    return result;
}

VoiceInput::MicrophoneStatus VoiceInput::probeMicrophone(const QString &configuredDevice)
{
    if (probeOverride()) {
        return probeOverride()(configuredDevice.trimmed());
    }

    QStringList dummyArgs;
    if (detectCaptureCommand(&dummyArgs, u"/dev/null"_s, configuredDevice).isEmpty()) {
        MicrophoneStatus status;
        status.available = false;
        status.reason = i18n("No microphone is available or usable: no audio capture tool found (install pipewire, pulseaudio-utils, alsa-utils, or ffmpeg).");
        return status;
    }

    auto runTool = [](const QString &exe, const QStringList &args, QString *stdoutOut, QString *stderrOut) -> int {
        QProcess proc;
        proc.start(exe, args);
        if (!proc.waitForStarted(1000)) {
            return -1;
        }
        if (!proc.waitForFinished(kMicProbeTimeoutMs)) {
            proc.kill();
            proc.waitForFinished(200);
            return -1;
        }
        if (stdoutOut) {
            *stdoutOut = QString::fromUtf8(proc.readAllStandardOutput());
        }
        if (stderrOut) {
            *stderrOut = QString::fromUtf8(proc.readAllStandardError());
        }
        return proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : -1;
    };

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (!ffmpeg.isEmpty()) {
        QString out, err;
#if defined(Q_OS_MACOS)
        runTool(ffmpeg, {u"-hide_banner"_s, u"-f"_s, u"avfoundation"_s, u"-list_devices"_s, u"true"_s, u"-i"_s, QString()}, &out, &err);
#else
        runTool(ffmpeg, {u"-hide_banner"_s, u"-list_devices"_s, u"true"_s, u"-f"_s, u"dshow"_s, u"-i"_s, u"dummy"_s}, &out, &err);
#endif
        return parseFfmpegDevicesOutput(out + u"\n"_s + err, configuredDevice);
    }
#else
    const QString pactl = QStandardPaths::findExecutable(u"pactl"_s);
    if (!pactl.isEmpty()) {
        QString out;
        if (runTool(pactl, {u"list"_s, u"sources"_s}, &out, nullptr) == 0) {
            const MicrophoneStatus parsed = parsePactlSourcesOutput(out, configuredDevice);
            if (parsed.available) {
                return parsed;
            }
            if (!configuredDevice.startsWith(u"hw:"_s, Qt::CaseInsensitive)
                && !configuredDevice.startsWith(u"plughw:"_s, Qt::CaseInsensitive)) {
                return parsed;
            }
        }
    }

    const QString wpctl = QStandardPaths::findExecutable(u"wpctl"_s);
    if (!wpctl.isEmpty()) {
        QString out;
        if (runTool(wpctl, {u"status"_s}, &out, nullptr) == 0) {
            return parseWpctlStatusOutput(out, configuredDevice);
        }
    }

    const QString arecord = QStandardPaths::findExecutable(u"arecord"_s);
    if (!arecord.isEmpty()) {
        QString out;
        if (runTool(arecord, {u"-l"_s}, &out, nullptr) == 0) {
            MicrophoneStatus parsed = parseArecordListOutput(out, configuredDevice);
            if (parsed.available && !isAlsaCaptureAccessible()) {
                parsed.available = false;
                parsed.reason = i18n("No microphone is available or usable: permission denied accessing audio capture devices.");
            }
            return parsed;
        }
    }
#endif

    MicrophoneStatus status;
    status.available = false;
    status.reason = i18n("No microphone is available or usable: no audio capture device detected.");
    return status;
}

QString VoiceInput::detectCaptureCommand(QStringList *argsOut, const QString &outputWavPath,
                                         const QString &device, const QString &preferredBackend)
{
    if (argsOut) {
        argsOut->clear();
    }
    const QString trimmedDevice = device.trimmed();

#if defined(Q_OS_MACOS)
    Q_UNUSED(preferredBackend)
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (!ffmpeg.isEmpty()) {
        if (argsOut) {
            *argsOut = {u"-y"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s,
                        u"-f"_s, u"avfoundation"_s,
                        u"-i"_s, u":"_s + (trimmedDevice.isEmpty() ? u"0"_s : trimmedDevice),
                        u"-ac"_s, u"1"_s, u"-ar"_s, u"16000"_s, u"-c:a"_s, u"pcm_s16le"_s,
                        outputWavPath};
        }
        return ffmpeg;
    }
    return {};
#elif defined(Q_OS_WIN)
    Q_UNUSED(preferredBackend)
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (!ffmpeg.isEmpty()) {
        if (argsOut) {
            const QString audioSpec = trimmedDevice.isEmpty() ? u"audio=default"_s : (u"audio="_s + trimmedDevice);
            *argsOut = {u"-y"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s,
                        u"-f"_s, u"dshow"_s, u"-i"_s, audioSpec,
                        u"-ac"_s, u"1"_s, u"-ar"_s, u"16000"_s, u"-c:a"_s, u"pcm_s16le"_s,
                        outputWavPath};
        }
        return ffmpeg;
    }
    return {};
#else
    const bool explicitAlsa = preferredBackend == u"alsa"_s
        || trimmedDevice.startsWith(u"hw:"_s, Qt::CaseInsensitive)
        || trimmedDevice.startsWith(u"plughw:"_s, Qt::CaseInsensitive);

    if (!explicitAlsa) {
        const QString pwRecord = QStandardPaths::findExecutable(u"pw-record"_s);
        if (!pwRecord.isEmpty()) {
            if (argsOut) {
                *argsOut = {u"--format=s16"_s, u"--rate=16000"_s, u"--channels=1"_s};
                if (!trimmedDevice.isEmpty()) {
                    argsOut->append(u"--target"_s);
                    argsOut->append(trimmedDevice);
                }
                argsOut->append(outputWavPath);
            }
            return pwRecord;
        }

        const QString paRecord = QStandardPaths::findExecutable(u"parecord"_s);
        if (!paRecord.isEmpty()) {
            if (argsOut) {
                *argsOut = {u"--file-format=wav"_s, u"--format=s16le"_s, u"--rate=16000"_s, u"--channels=1"_s};
                if (!trimmedDevice.isEmpty()) {
                    argsOut->append(u"--device="_s + trimmedDevice);
                }
                argsOut->append(outputWavPath);
            }
            return paRecord;
        }
    }

    const QString arecord = QStandardPaths::findExecutable(u"arecord"_s);
    if (!arecord.isEmpty()) {
        if (argsOut) {
            *argsOut = {u"-q"_s, u"-f"_s, u"S16_LE"_s, u"-r"_s, u"16000"_s, u"-c"_s, u"1"_s, u"-t"_s, u"wav"_s};
            if (!trimmedDevice.isEmpty()) {
                argsOut->append(u"-D"_s);
                argsOut->append(trimmedDevice);
            }
            argsOut->append(outputWavPath);
        }
        return arecord;
    }

    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (!ffmpeg.isEmpty()) {
        if (argsOut) {
            const QString inputFormat = explicitAlsa ? u"alsa"_s : u"pulse"_s;
            *argsOut = {u"-y"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s,
                        u"-f"_s, inputFormat, u"-i"_s, trimmedDevice.isEmpty() ? u"default"_s : trimmedDevice,
                        u"-ac"_s, u"1"_s, u"-ar"_s, u"16000"_s, u"-c:a"_s, u"pcm_s16le"_s,
                        outputWavPath};
        }
        return ffmpeg;
    }
    return {};
#endif
}

QString VoiceInput::detectedToolsSummary()
{
    QStringList dummyArgs;
    const QString capture = detectCaptureCommand(&dummyArgs, u"test.wav"_s);
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    const QString localWhisper = findLocalWhisperCli();

    QStringList parts;
    parts.append(capture.isEmpty()
                     ? i18n("Capture: none found")
                     : i18n("Capture: %1", QFileInfo(capture).fileName()));
    parts.append(ffmpeg.isEmpty()
                     ? i18n("Noise removal: built-in DSP")
                     : i18n("Noise removal: ffmpeg + built-in DSP fallback"));
    if (!localWhisper.isEmpty()) {
        parts.append(i18n("Local CLI: %1", QFileInfo(localWhisper).fileName()));
    }
    return parts.join(u"  ·  "_s);
}

QString VoiceInput::resolveBackend(const Settings &settings, QString *targetOut, QString *apiKeyOut)
{
    if (targetOut) {
        targetOut->clear();
    }
    if (apiKeyOut) {
        apiKeyOut->clear();
    }

    const QString mode = settings.voiceProvider.trimmed().toLower();
    const QString voiceKey = settings.voiceApiKey.trimmed();
    const QString openaiKey = settings.openaiApiKey.trimmed();
    const QString voiceEndpoint = settings.voiceEndpoint.trimmed();
    const QString localCmd = settings.voiceLocalCommand.trimmed();

    auto fillOpenAI = [&]() -> QString {
        const QString key = !voiceKey.isEmpty() ? voiceKey : openaiKey;
        if (key.isEmpty()) {
            return {};
        }
        if (targetOut) {
            *targetOut = u"https://api.openai.com/v1/audio/transcriptions"_s;
        }
        if (apiKeyOut) {
            *apiKeyOut = key;
        }
        return u"openai"_s;
    };

    auto fillCompatible = [&]() -> QString {
        const QString base = !voiceEndpoint.isEmpty() ? voiceEndpoint : settings.openaiCompatibleUrl.trimmed();
        const QString url = normalizeTranscriptionEndpoint(base);
        if (url.isEmpty()) {
            return {};
        }
        if (targetOut) {
            *targetOut = url;
        }
        if (apiKeyOut) {
            *apiKeyOut = !voiceKey.isEmpty() ? voiceKey : settings.openaiCompatibleApiKey.trimmed();
        }
        return u"openai_compatible"_s;
    };

    auto fillLocalCli = [&]() -> QString {
        QString spec = localCmd;
        if (spec.isEmpty()) {
            const QString bin = findLocalWhisperCli();
            if (!bin.isEmpty()) {
                spec = QStringLiteral("\"%1\" {file}").arg(bin);
            }
        }
        if (spec.isEmpty()) {
            return {};
        }
        if (targetOut) {
            *targetOut = spec;
        }
        return u"local_cli"_s;
    };

    if (mode == u"openai"_s) {
        return fillOpenAI();
    }
    if (mode == u"openai_compatible"_s) {
        return fillCompatible();
    }
    if (mode == u"local_cli"_s) {
        return fillLocalCli();
    }

    // Auto resolution order: explicit custom endpoint -> explicit local command
    // -> OpenAI key -> local whisper binary in PATH.
    if (!voiceEndpoint.isEmpty()) {
        return fillCompatible();
    }
    if (!localCmd.isEmpty()) {
        return fillLocalCli();
    }
    if (const QString openai = fillOpenAI(); !openai.isEmpty()) {
        return openai;
    }
    if (const QString local = fillLocalCli(); !local.isEmpty()) {
        return local;
    }
    return {};
}

bool VoiceInput::parsePcm16Wav(const QByteArray &wavData, QVector<qint16> *samples, int *sampleRate, int *channels,
                               QString *error)
{
    if (wavData.size() < 44) {
        if (error) {
            *error = i18n("WAV audio is too short.");
        }
        return false;
    }
    const auto *data = reinterpret_cast<const uchar *>(wavData.constData());
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        if (error) {
            *error = i18n("Invalid WAV header.");
        }
        return false;
    }

    bool foundFmt = false;
    bool foundData = false;
    quint16 audioFormat = 0;
    quint16 numChannels = 0;
    quint32 sr = 0;
    quint16 bitsPerSample = 0;
    int dataOffset = 0;
    int dataSize = 0;

    int pos = 12;
    while (pos + 8 <= wavData.size()) {
        const quint32 chunkSize = readLe32(data + pos + 4);
        const int chunkDataStart = pos + 8;
        if (chunkDataStart > wavData.size()) {
            break;
        }
        const int availableBytes = wavData.size() - chunkDataStart;
        const int clampedChunkSize = static_cast<int>(std::min<quint32>(chunkSize, static_cast<quint32>(availableBytes)));

        if (std::memcmp(data + pos, "fmt ", 4) == 0 && clampedChunkSize >= 16) {
            audioFormat = readLe16(data + chunkDataStart);
            numChannels = readLe16(data + chunkDataStart + 2);
            sr = readLe32(data + chunkDataStart + 4);
            bitsPerSample = readLe16(data + chunkDataStart + 14);
            foundFmt = true;
        } else if (std::memcmp(data + pos, "data", 4) == 0) {
            dataOffset = chunkDataStart;
            // Some capture tools leave chunkSize=0 or 0xFFFFFFFF if interrupted.
            dataSize = (chunkSize == 0 || chunkSize == 0xFFFFFFFFu) ? availableBytes : clampedChunkSize;
            foundData = true;
            break;
        }

        const qint64 step = 8LL + chunkSize + (chunkSize & 1u);
        if (step <= 8 || pos + step > wavData.size()) {
            break;
        }
        pos += static_cast<int>(step);
    }

    if (!foundFmt || !foundData) {
        if (error) {
            *error = i18n("WAV audio is missing fmt or data chunk.");
        }
        return false;
    }
    if (audioFormat != 1 || bitsPerSample != 16 || numChannels == 0 || sr == 0) {
        if (error) {
            *error = i18n("Unsupported WAV format (expected 16-bit PCM).");
        }
        return false;
    }

    const int totalSamples = dataSize / 2;
    if (samples) {
        samples->resize(totalSamples);
        for (int i = 0; i < totalSamples; ++i) {
            (*samples)[i] = static_cast<qint16>(readLe16(data + dataOffset + i * 2));
        }
    }
    if (sampleRate) {
        *sampleRate = static_cast<int>(sr);
    }
    if (channels) {
        *channels = static_cast<int>(numChannels);
    }
    return true;
}

QByteArray VoiceInput::encodePcm16Wav(const QVector<qint16> &samples, int sampleRate, int channels)
{
    const int sr = std::max(8000, sampleRate);
    const int ch = std::max(1, channels);
    const quint32 dataBytes = static_cast<quint32>(samples.size() * 2);
    const quint32 byteRate = static_cast<quint32>(sr * ch * 2);
    const quint16 blockAlign = static_cast<quint16>(ch * 2);

    QByteArray out;
    out.reserve(44 + static_cast<int>(dataBytes));
    out.append("RIFF", 4);
    appendLe32(out, 36 + dataBytes);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    appendLe32(out, 16); // PCM fmt chunk size
    appendLe16(out, 1);  // PCM format
    appendLe16(out, static_cast<quint16>(ch));
    appendLe32(out, static_cast<quint32>(sr));
    appendLe32(out, byteRate);
    appendLe16(out, blockAlign);
    appendLe16(out, 16); // bits per sample
    out.append("data", 4);
    appendLe32(out, dataBytes);
    for (qint16 s : samples) {
        appendLe16(out, static_cast<quint16>(s));
    }
    return out;
}

bool VoiceInput::denoiseWavBuffer(const QByteArray &inWav, QByteArray *outWav, QString *error)
{
    QVector<qint16> raw;
    int sampleRate = 16000;
    int channels = 1;
    if (!parsePcm16Wav(inWav, &raw, &sampleRate, &channels, error)) {
        return false;
    }
    if (raw.isEmpty()) {
        if (outWav) {
            *outWav = encodePcm16Wav({}, sampleRate, 1);
        }
        return true;
    }

    // 1. Mix down to mono float in [-1, 1].
    const int frameCount = raw.size() / channels;
    QVector<double> mono(frameCount, 0.0);
    for (int i = 0; i < frameCount; ++i) {
        double sum = 0.0;
        for (int c = 0; c < channels; ++c) {
            sum += static_cast<double>(raw[i * channels + c]) / 32768.0;
        }
        mono[i] = sum / channels;
    }

    // 2. Remove DC offset.
    double mean = 0.0;
    for (double v : mono) {
        mean += v;
    }
    mean /= std::max(1, frameCount);
    for (double &v : mono) {
        v -= mean;
    }

    // 3. First-order high-pass filter at 80 Hz to remove hum/rumble.
    const double dt = 1.0 / static_cast<double>(sampleRate);
    const double rc = 1.0 / (2.0 * M_PI * 80.0);
    const double alpha = rc / (rc + dt);
    double prevIn = 0.0;
    double prevOut = 0.0;
    for (int i = 0; i < frameCount; ++i) {
        const double y = alpha * (prevOut + mono[i] - prevIn);
        prevIn = mono[i];
        prevOut = y;
        mono[i] = y;
    }

    // 4. Adaptive noise-floor estimation (20 ms frames) + smooth Wiener-style gain gate.
    const int winSize = std::max(64, sampleRate / 50); // 20 ms
    const int numWindows = std::max(1, (frameCount + winSize - 1) / winSize);
    QVector<double> winRms(numWindows, 0.0);
    for (int w = 0; w < numWindows; ++w) {
        const int start = w * winSize;
        const int end = std::min(frameCount, start + winSize);
        double sq = 0.0;
        for (int i = start; i < end; ++i) {
            sq += mono[i] * mono[i];
        }
        winRms[w] = std::sqrt(sq / std::max(1, end - start));
    }

    QVector<double> sortedRms = winRms;
    std::sort(sortedRms.begin(), sortedRms.end());
    // Estimate stationary background noise floor from the 20th percentile energy frame.
    const double noiseFloor = sortedRms[std::min(sortedRms.size() - 1, sortedRms.size() / 5)];
    const double peakRms = sortedRms.last();

    // Only apply noise gating if there is meaningful separation or audible noise floor.
    if (noiseFloor > 1e-6) {
        const double threshold = std::max(noiseFloor * 2.2, 0.002);
        const double minGain = 0.08; // ~-22 dB suppression of stationary background noise
        double smoothedGain = minGain;
        const double attack = 0.35;
        const double release = 0.04;

        for (int w = 0; w < numWindows; ++w) {
            const double rms = winRms[w];
            double targetGain = 1.0;
            if (rms <= noiseFloor * 1.15) {
                targetGain = minGain;
            } else if (rms < threshold) {
                const double ratio = (rms - noiseFloor * 1.15) / std::max(1e-9, threshold - noiseFloor * 1.15);
                // Smooth Wiener-like knee
                const double snr = ratio * ratio;
                targetGain = minGain + (1.0 - minGain) * snr;
            }

            const int start = w * winSize;
            const int end = std::min(frameCount, start + winSize);
            for (int i = start; i < end; ++i) {
                const double coeff = (targetGain > smoothedGain) ? attack : release;
                smoothedGain += coeff * (targetGain - smoothedGain);
                mono[i] *= smoothedGain;
            }
        }
    }

    // 5. Gentle peak normalization for speech clarity (only boost if real speech exists above noise).
    double peak = 0.0;
    for (double v : mono) {
        peak = std::max(peak, std::abs(v));
    }
    if (peak > 1e-4 && peakRms > noiseFloor * 1.6) {
        const double targetPeak = 0.85;
        const double gain = std::min(6.0, targetPeak / peak);
        if (gain > 1.05 || peak > 0.98) {
            const double applyGain = peak > 0.98 ? (targetPeak / peak) : gain;
            for (double &v : mono) {
                v *= applyGain;
            }
        }
    }

    QVector<qint16> outSamples(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        const double clamped = std::clamp(mono[i], -1.0, 1.0);
        outSamples[i] = static_cast<qint16>(std::lrint(clamped * 32767.0));
    }

    if (outWav) {
        *outWav = encodePcm16Wav(outSamples, sampleRate, 1);
    }
    return true;
}

void VoiceInput::toggle()
{
    switch (m_state) {
    case State::Idle:
        startRecording();
        break;
    case State::Recording:
        stopAndProcess();
        break;
    case State::Denoising:
    case State::Transcribing:
        cancel();
        break;
    }
}

bool VoiceInput::ensureTempDir(QString *error)
{
    m_tempDir = std::make_unique<QTemporaryDir>();
    if (!m_tempDir->isValid()) {
        if (error) {
            *error = i18n("Could not create a temporary directory for voice recording.");
        }
        m_tempDir.reset();
        return false;
    }
    return true;
}

void VoiceInput::startRecording()
{
    if (m_state != State::Idle) {
        return;
    }
    if (!m_micProbeCompleted) {
        applyMicrophoneStatus(probeMicrophone(m_settings.voiceInputDevice), true);
    }
    if (!m_micStatus.available) {
        fail(m_micStatus.reason.isEmpty() ? noMicDefaultReason() : m_micStatus.reason);
        return;
    }

    QString dirErr;
    if (!ensureTempDir(&dirErr)) {
        fail(dirErr);
        return;
    }

    m_rawWavPath = QDir(m_tempDir->path()).filePath(u"voice_raw.wav"_s);
    m_cleanWavPath = QDir(m_tempDir->path()).filePath(u"voice_clean.wav"_s);

    QStringList args;
    const QString program = detectCaptureCommand(&args, m_rawWavPath, m_settings.voiceInputDevice, m_micStatus.backend);
    if (program.isEmpty()) {
        fail(i18n("No audio capture tool found (install pipewire, pulseaudio-utils, alsa-utils, or ffmpeg)."));
        return;
    }

    m_stoppingRecording = false;
    m_cancelling = false;

    auto *proc = new QProcess(this);
    m_recordProcess = proc;

    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (m_recordProcess != proc || m_cancelling) {
            return;
        }
        if (m_stoppingRecording && err == QProcess::Crashed) {
            // SIGINT / terminate on some tools reports Crashed after writing the WAV.
            return;
        }
        const QString msg = proc->errorString();
        cleanupSession();
        refreshMicrophoneAvailability();
        fail(i18n("Could not start microphone capture: %1", msg));
    });

    connect(proc, &QProcess::finished, this, [this, proc](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_recordProcess != proc) {
            return;
        }
        m_tickTimer->stop();
        m_stopTimeout->stop();
        const QString stderrText = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        m_recordProcess = nullptr;
        proc->deleteLater();

        if (m_cancelling) {
            cleanupSession();
            setState(State::Idle);
            Q_EMIT cancelled();
            return;
        }

        if (!m_stoppingRecording) {
            // Exited unexpectedly before user stopped recording.
            cleanupSession();
            refreshMicrophoneAvailability();
            fail(stderrText.isEmpty()
                     ? i18n("Microphone capture stopped unexpectedly (exit code %1).", exitCode)
                     : i18n("Microphone capture failed: %1", stderrText));
            return;
        }

        Q_UNUSED(exitStatus);
        const QFileInfo fi(m_rawWavPath);
        if (!fi.exists() || fi.size() <= 44) {
            cleanupSession();
            fail(i18n("No audio was captured from the microphone."));
            return;
        }

        if (m_settings.voiceNoiseRemoval) {
            beginDenoise(m_rawWavPath);
        } else {
            beginTranscription(m_rawWavPath);
        }
    });

    proc->start(program, args);
    if (!proc->waitForStarted(1500)) {
        const QString msg = proc->errorString();
        cleanupSession();
        refreshMicrophoneAvailability();
        fail(i18n("Could not start microphone capture (%1): %2", QFileInfo(program).fileName(), msg));
        return;
    }

    m_recordElapsed.start();
    setState(State::Recording);
    Q_EMIT recordingTick(0);
    m_tickTimer->start();
}

void VoiceInput::stopAndProcess()
{
    if (m_state != State::Recording || m_stoppingRecording) {
        return;
    }
    m_stoppingRecording = true;
    m_tickTimer->stop();

    if (!m_recordProcess || m_recordProcess->state() == QProcess::NotRunning) {
        if (QFileInfo::exists(m_rawWavPath) && QFileInfo(m_rawWavPath).size() > 44) {
            if (m_settings.voiceNoiseRemoval) {
                beginDenoise(m_rawWavPath);
            } else {
                beginTranscription(m_rawWavPath);
            }
        } else {
            cleanupSession();
            fail(i18n("No audio was captured from the microphone."));
        }
        return;
    }

    m_stopTimeout->start();
    const QString progName = QFileInfo(m_recordProcess->program()).fileName().toLower();
    if (progName.startsWith(u"ffmpeg"_s)) {
        m_recordProcess->write("q\n");
        m_recordProcess->closeWriteChannel();
    } else {
#ifdef Q_OS_UNIX
        const qint64 pid = m_recordProcess->processId();
        if (pid > 0) {
            ::kill(static_cast<pid_t>(pid), SIGINT);
        } else {
            m_recordProcess->terminate();
        }
#else
        m_recordProcess->terminate();
#endif
    }
}

void VoiceInput::cancel()
{
    if (m_state == State::Idle) {
        return;
    }
    m_cancelling = true;
    cleanupSession();
    setState(State::Idle);
    Q_EMIT cancelled();
}

void VoiceInput::processAudioFile(const QString &wavPath)
{
    if (m_state != State::Idle) {
        return;
    }
    if (!QFileInfo::exists(wavPath)) {
        fail(i18n("Audio file does not exist: %1", wavPath));
        return;
    }
    QString dirErr;
    if (!ensureTempDir(&dirErr)) {
        fail(dirErr);
        return;
    }
    m_cancelling = false;
    m_stoppingRecording = false;
    m_rawWavPath = wavPath;
    m_cleanWavPath = QDir(m_tempDir->path()).filePath(u"voice_clean.wav"_s);

    if (m_settings.voiceNoiseRemoval) {
        beginDenoise(m_rawWavPath);
    } else {
        beginTranscription(m_rawWavPath);
    }
}

void VoiceInput::beginDenoise(const QString &rawWavPath)
{
    setState(State::Denoising);

    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (ffmpeg.isEmpty()) {
        runBuiltinDenoiseAndTranscribe(rawWavPath);
        return;
    }

    auto *proc = new QProcess(this);
    m_denoiseProcess = proc;
    const QStringList args = {
        u"-y"_s,
        u"-hide_banner"_s,
        u"-loglevel"_s, u"error"_s,
        u"-i"_s, rawWavPath,
        u"-af"_s, u"highpass=f=80,lowpass=f=7600,afftdn=nf=-25:nt=w,dynaudnorm=f=150:g=15:p=0.9"_s,
        u"-ac"_s, u"1"_s,
        u"-ar"_s, u"16000"_s,
        u"-c:a"_s, u"pcm_s16le"_s,
        m_cleanWavPath,
    };

    connect(proc, &QProcess::errorOccurred, this, [this, proc, rawWavPath](QProcess::ProcessError) {
        if (m_denoiseProcess != proc || m_cancelling) {
            return;
        }
        m_denoiseProcess = nullptr;
        proc->deleteLater();
        runBuiltinDenoiseAndTranscribe(rawWavPath);
    });

    connect(proc, &QProcess::finished, this, [this, proc, rawWavPath](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_denoiseProcess != proc) {
            return;
        }
        m_denoiseProcess = nullptr;
        proc->deleteLater();

        if (m_cancelling) {
            return;
        }
        if (exitStatus == QProcess::NormalExit && exitCode == 0
            && QFileInfo::exists(m_cleanWavPath) && QFileInfo(m_cleanWavPath).size() > 44) {
            beginTranscription(m_cleanWavPath);
            return;
        }
        // Fallback to built-in C++ denoiser when ffmpeg lacks a filter (e.g. afftdn).
        runBuiltinDenoiseAndTranscribe(rawWavPath);
    });

    proc->start(ffmpeg, args);
}

void VoiceInput::runBuiltinDenoiseAndTranscribe(const QString &rawWavPath)
{
    if (m_cancelling) {
        return;
    }
    QFile inFile(rawWavPath);
    if (!inFile.open(QIODevice::ReadOnly)) {
        beginTranscription(rawWavPath);
        return;
    }
    const QByteArray rawBytes = inFile.readAll();
    inFile.close();

    QByteArray cleanBytes;
    QString err;
    if (!denoiseWavBuffer(rawBytes, &cleanBytes, &err)) {
        // If WAV format wasn't 16-bit PCM, fall back to sending raw recording.
        beginTranscription(rawWavPath);
        return;
    }

    QFile outFile(m_cleanWavPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        beginTranscription(rawWavPath);
        return;
    }
    outFile.write(cleanBytes);
    outFile.close();

    beginTranscription(m_cleanWavPath);
}

void VoiceInput::beginTranscription(const QString &wavPath)
{
    if (m_cancelling) {
        return;
    }
    setState(State::Transcribing);

    QString target;
    QString apiKey;
    const QString backend = resolveBackend(m_settings, &target, &apiKey);
    if (backend.isEmpty()) {
        fail(i18n("Voice transcription is not configured. Add an OpenAI API key, an OpenAI-compatible Whisper endpoint, or a local Whisper CLI in Kate AI Settings → Voice."));
        return;
    }

    if (backend == u"local_cli"_s) {
        transcribeViaCli(wavPath, target);
    } else {
        transcribeViaHttp(wavPath, target, apiKey);
    }
}

void VoiceInput::transcribeViaHttp(const QString &wavPath, const QString &endpointUrl, const QString &apiKey)
{
    auto *file = new QFile(wavPath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        fail(i18n("Could not read recorded audio for transcription."));
        return;
    }
    if (file->size() <= 0) {
        delete file;
        fail(i18n("Recorded audio is empty."));
        return;
    }

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, u"audio/wav"_s);
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       u"form-data; name=\"file\"; filename=\"voice.wav\""_s);
    // Stream from disk rather than copying the whole recording into a body
    // part: a five-minute capture is ~9.6 MB and the copy would sit beside the
    // raw, denoised and decoded buffers. The multipart takes the file as its
    // child so it lives until the reply is sent.
    filePart.setBodyDevice(file);
    file->setParent(multiPart);
    multiPart->append(filePart);

    const QString model = m_settings.voiceModel.trimmed().isEmpty()
        ? u"whisper-1"_s
        : m_settings.voiceModel.trimmed();
    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader, u"form-data; name=\"model\""_s);
    modelPart.setBody(model.toUtf8());
    multiPart->append(modelPart);

    QHttpPart formatPart;
    formatPart.setHeader(QNetworkRequest::ContentDispositionHeader, u"form-data; name=\"response_format\""_s);
    formatPart.setBody("json");
    multiPart->append(formatPart);

    const QString lang = m_settings.voiceLanguage.trimmed();
    if (!lang.isEmpty()) {
        QHttpPart langPart;
        langPart.setHeader(QNetworkRequest::ContentDispositionHeader, u"form-data; name=\"language\""_s);
        langPart.setBody(lang.toUtf8());
        multiPart->append(langPart);
    }

    QNetworkRequest req{QUrl(endpointUrl)};
    if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    }

    QNetworkReply *reply = m_network->post(req, multiPart);
    multiPart->setParent(reply);
    m_reply = reply;
    m_httpTimeout->start();

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_reply != reply) {
            reply->deleteLater();
            return;
        }
        m_httpTimeout->stop();
        m_reply = nullptr;

        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError netErr = reply->error();
        const QString errStr = reply->errorString();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (m_cancelling) {
            return;
        }
        if (netErr != QNetworkReply::NoError) {
            QString detail = errStr;
            const QJsonDocument errDoc = QJsonDocument::fromJson(body);
            if (errDoc.isObject()) {
                const QJsonValue errVal = errDoc.object().value(u"error"_s);
                if (errVal.isObject() && !errVal.toObject().value(u"message"_s).toString().isEmpty()) {
                    detail = errVal.toObject().value(u"message"_s).toString();
                } else if (errVal.isString() && !errVal.toString().isEmpty()) {
                    detail = errVal.toString();
                }
            }
            fail(httpStatus > 0
                     ? i18n("Voice transcription failed (HTTP %1): %2", httpStatus, detail)
                     : i18n("Voice transcription failed: %1", detail));
            return;
        }

        QString text;
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            text = doc.object().value(u"text"_s).toString();
        } else {
            text = QString::fromUtf8(body);
        }
        finishWithText(text);
    });
}

void VoiceInput::transcribeViaCli(const QString &wavPath, const QString &cliSpec)
{
    QStringList tokens = splitCommandLine(cliSpec);
    if (tokens.isEmpty()) {
        fail(i18n("Local transcription command is empty."));
        return;
    }

    bool replacedFile = false;
    const QString lang = m_settings.voiceLanguage.trimmed();
    for (QString &tok : tokens) {
        if (tok.contains(u"{file}"_s)) {
            tok.replace(u"{file}"_s, wavPath);
            replacedFile = true;
        }
        if (tok.contains(u"{language}"_s)) {
            tok.replace(u"{language}"_s, lang);
        }
    }
    if (!replacedFile) {
        tokens.append(wavPath);
    }

    const QString program = tokens.takeFirst();
    auto *proc = new QProcess(this);
    m_cliProcess = proc;
    if (m_tempDir && m_tempDir->isValid()) {
        proc->setWorkingDirectory(m_tempDir->path());
    }

    connect(proc, &QProcess::errorOccurred, this, [this, proc, program](QProcess::ProcessError) {
        if (m_cliProcess != proc || m_cancelling) {
            return;
        }
        const QString err = proc->errorString();
        m_cliProcess = nullptr;
        proc->deleteLater();
        fail(i18n("Could not run local transcription command '%1': %2", program, err));
    });

    connect(proc, &QProcess::finished, this, [this, proc, wavPath](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_cliProcess != proc) {
            return;
        }
        const QString out = QString::fromUtf8(proc->readAllStandardOutput());
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        m_cliProcess = nullptr;
        proc->deleteLater();

        if (m_cancelling) {
            return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            fail(err.isEmpty()
                     ? i18n("Local transcription command exited with code %1.", exitCode)
                     : i18n("Local transcription failed: %1", err));
            return;
        }

        QString text = out;
        if (text.trimmed().isEmpty()) {
            // Python whisper / whisper-cli -otxt may write a sidecar .txt file.
            const QFileInfo fi(wavPath);
            const QStringList candidates = {
                wavPath + u".txt"_s,
                QDir(fi.absolutePath()).filePath(fi.completeBaseName() + u".txt"_s),
            };
            for (const QString &candidate : candidates) {
                QFile txtFile(candidate);
                if (txtFile.open(QIODevice::ReadOnly)) {
                    text = QString::fromUtf8(txtFile.readAll());
                    txtFile.close();
                    if (!text.trimmed().isEmpty()) {
                        break;
                    }
                }
            }
        }
        finishWithText(text);
    });

    proc->start(program, tokens);
}

void VoiceInput::finishWithText(const QString &rawText)
{
    const QString cleaned = cleanTranscriptText(rawText);
    cleanupSession();
    setState(State::Idle);
    if (cleaned.isEmpty()) {
        Q_EMIT errorOccurred(i18n("No speech was detected in the recording."));
        return;
    }
    Q_EMIT transcribed(cleaned);
}

void VoiceInput::fail(const QString &message)
{
    cleanupSession();
    setState(State::Idle);
    Q_EMIT errorOccurred(message);
}

void VoiceInput::cleanupSession()
{
    if (m_tickTimer) {
        m_tickTimer->stop();
    }
    if (m_stopTimeout) {
        m_stopTimeout->stop();
    }
    if (m_httpTimeout) {
        m_httpTimeout->stop();
    }
    auto stopProcess = [this](QPointer<QProcess> &proc) {
        if (!proc) {
            return;
        }
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(300);
        }
        proc->deleteLater();
        proc = nullptr;
    };
    stopProcess(m_recordProcess);
    stopProcess(m_denoiseProcess);
    stopProcess(m_cliProcess);

    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    m_stoppingRecording = false;
    m_rawWavPath.clear();
    m_cleanWavPath.clear();
    m_tempDir.reset();
}

} // namespace KateAi
