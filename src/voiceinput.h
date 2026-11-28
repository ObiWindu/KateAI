/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "types.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QTimer;

namespace KateAi
{

// Captures microphone audio into a temporary 16 kHz mono PCM WAV file,
// removes background noise (via ffmpeg's audio filter graph when installed,
// falling back to a built-in pure-C++ DSP pipeline), and transcribes speech
// into text using either an OpenAI-compatible /audio/transcriptions endpoint
// or a local Whisper CLI executable.
class VoiceInput : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Recording,
        Denoising,
        Transcribing,
    };
    Q_ENUM(State)

    struct MicrophoneStatus {
        bool available = false;
        QString deviceName;
        QString reason;
        // Backend hint discovered alongside the device ("pipewire", "pulse",
        // "alsa", "avfoundation", "dshow", or empty).
        QString backend;
    };

    explicit VoiceInput(QObject *parent = nullptr);
    ~VoiceInput() override;

    void setNetworkManager(QNetworkAccessManager *network);
    void setSettings(const Settings &settings);
    Settings settings() const
    {
        return m_settings;
    }

    State state() const
    {
        return m_state;
    }
    bool isActive() const
    {
        return m_state != State::Idle;
    }

    // Current cached microphone availability/usability status. Call
    // refreshMicrophoneAvailability() to probe asynchronously without blocking
    // the UI thread.
    MicrophoneStatus microphoneStatus() const
    {
        return m_micStatus;
    }
    bool isMicrophoneAvailable() const
    {
        return m_micStatus.available;
    }
    bool isMicrophoneProbePending() const
    {
        return m_micProbeInFlight;
    }
    void refreshMicrophoneAvailability();

    // Starts recording if Idle; stops and processes if Recording; cancels if
    // currently Denoising or Transcribing.
    void toggle();
    void startRecording();
    void stopAndProcess();
    void cancel();

    // Runs the noise-removal and transcription stages directly on an existing
    // WAV file (used by tests and callers that already hold recorded audio).
    void processAudioFile(const QString &wavPath);

    // Pure-C++ 16-bit PCM WAV codec and DSP denoiser (DC-offset removal,
    // 80 Hz high-pass filter, adaptive noise-floor estimation with smooth
    // RMS/spectral noise gate, and speech-level normalization). Used both as
    // the zero-dependency fallback when ffmpeg is absent and in unit tests.
    static bool parsePcm16Wav(const QByteArray &wavData, QVector<qint16> *samples, int *sampleRate, int *channels,
                              QString *error = nullptr);
    static QByteArray encodePcm16Wav(const QVector<qint16> &samples, int sampleRate = 16000, int channels = 1);
    static bool denoiseWavBuffer(const QByteArray &inWav, QByteArray *outWav, QString *error = nullptr);

    // Discovers the best available audio capture tool on this system
    // (pw-record, parecord, arecord, ffmpeg) and populates its command-line
    // arguments for 16 kHz mono 16-bit PCM WAV recording.
    static QString detectCaptureCommand(QStringList *argsOut, const QString &outputWavPath,
                                        const QString &device = QString(),
                                        const QString &preferredBackend = QString());
    // Returns a human-readable summary of detected capture / denoise tools for
    // the settings UI.
    static QString detectedToolsSummary();

    // Synchronous microphone availability/usability probe (for config UI and
    // tests). Checks both capture tool availability and that at least one real,
    // usable microphone input source exists (ignoring sink monitors and
    // unplugged ports).
    static MicrophoneStatus probeMicrophone(const QString &configuredDevice = QString());

    // Pure output parsers for each platform's device enumeration tools, exposed
    // for deterministic unit testing.
    static MicrophoneStatus parsePactlSourcesOutput(const QString &output, const QString &configuredDevice = QString());
    static MicrophoneStatus parseWpctlStatusOutput(const QString &output, const QString &configuredDevice = QString());
    static MicrophoneStatus parseArecordListOutput(const QString &output, const QString &configuredDevice = QString());
    static MicrophoneStatus parseFfmpegDevicesOutput(const QString &output, const QString &configuredDevice = QString());

    // Test hook to override OS microphone probing deterministically. Pass an
    // empty std::function to restore real system probing.
    static void setMicrophoneProbeOverrideForTesting(std::function<MicrophoneStatus(const QString &)> probe);

    // Resolves the transcription backend ("openai", "openai_compatible",
    // "local_cli", or empty if unconfigured) along with the target endpoint URL
    // or CLI command template and optional bearer API key.
    static QString resolveBackend(const Settings &settings, QString *targetOut = nullptr, QString *apiKeyOut = nullptr);

Q_SIGNALS:
    void stateChanged(KateAi::VoiceInput::State state);
    void microphoneAvailabilityChanged(bool available, const QString &detail);
    void recordingTick(int elapsedMs);
    void transcribed(const QString &text);
    void errorOccurred(const QString &message);
    void cancelled();

private:
    enum class ProbeStep {
        None,
        Pactl,
        Wpctl,
        Arecord,
        Ffmpeg,
    };

    void setState(State state);
    void applyMicrophoneStatus(const MicrophoneStatus &status, bool forceEmit = false);
    void runNextMicProbeStep(ProbeStep step);
    void finishMicProbe(const MicrophoneStatus &status);
    void cancelMicProbe();

    void cleanupSession();
    void beginDenoise(const QString &rawWavPath);
    void runBuiltinDenoiseAndTranscribe(const QString &rawWavPath);
    void beginTranscription(const QString &wavPath);
    void transcribeViaHttp(const QString &wavPath, const QString &endpointUrl, const QString &apiKey);
    void transcribeViaCli(const QString &wavPath, const QString &cliSpec);
    void finishWithText(const QString &rawText);
    void fail(const QString &message);
    bool ensureTempDir(QString *error);

    Settings m_settings;
    State m_state = State::Idle;
    bool m_stoppingRecording = false;
    bool m_cancelling = false;

    MicrophoneStatus m_micStatus;
    bool m_micProbeCompleted = false;
    bool m_micProbeInFlight = false;
    ProbeStep m_micProbeStep = ProbeStep::None;
    QPointer<QProcess> m_micProbeProcess;

    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_rawWavPath;
    QString m_cleanWavPath;

    QPointer<QProcess> m_recordProcess;
    QPointer<QProcess> m_denoiseProcess;
    QPointer<QProcess> m_cliProcess;
    QNetworkAccessManager *ensureNetworkManager();

    QPointer<QNetworkAccessManager> m_sharedNetwork;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;

    QTimer *m_tickTimer = nullptr;
    QTimer *m_stopTimeout = nullptr;
    QTimer *m_httpTimeout = nullptr;
    QElapsedTimer m_recordElapsed;
};

} // namespace KateAi
