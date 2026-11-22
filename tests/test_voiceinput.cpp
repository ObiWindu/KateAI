/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "chatwidget.h"
#include "promptedit.h"
#include "sessionstoreisolation.h"
#include "voiceinput.h"

#include <QApplication>
#include <QFile>
#include <QHelpEvent>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QToolTip>

#include <cmath>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

namespace
{

QVector<qint16> makeNoisySpeechSamples(int sampleRate = 16000)
{
    // 0.4s noise-only lead-in + 0.6s 440 Hz tone ("speech") with noise + 0.4s noise tail
    const int total = static_cast<int>(sampleRate * 1.4);
    QVector<qint16> out(total);
    quint32 rng = 0x12345678u;
    auto nextNoise = [&rng]() -> double {
        rng = rng * 1664525u + 1013904223u;
        return (static_cast<double>((rng >> 16) & 0xFFFF) / 32768.0) - 1.0;
    };

    const int speechStart = static_cast<int>(sampleRate * 0.4);
    const int speechEnd = static_cast<int>(sampleRate * 1.0);
    for (int i = 0; i < total; ++i) {
        double v = 0.04; // DC offset
        v += 0.03 * std::sin(2.0 * M_PI * 30.0 * i / sampleRate); // 30 Hz sub-band rumble
        v += 0.025 * nextNoise(); // broadband background hiss
        if (i >= speechStart && i < speechEnd) {
            v += 0.35 * std::sin(2.0 * M_PI * 440.0 * i / sampleRate);
        }
        out[i] = static_cast<qint16>(std::lrint(std::clamp(v, -1.0, 1.0) * 32767.0));
    }
    return out;
}

double rmsRange(const QVector<qint16> &samples, int begin, int end)
{
    double sq = 0.0;
    const int count = std::max(1, end - begin);
    for (int i = begin; i < end && i < samples.size(); ++i) {
        const double v = static_cast<double>(samples[i]) / 32768.0;
        sq += v * v;
    }
    return std::sqrt(sq / count);
}

// Lightweight in-process HTTP server for Whisper /audio/transcriptions + chat tests.
class MockVoiceHttpServer : public QObject
{
    Q_OBJECT

public:
    explicit MockVoiceHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MockVoiceHttpServer::handleConnection);
    }

    bool start()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const
    {
        return m_server.serverPort();
    }

    QString transcriptionResponseText = u"Refactor the audio parser"_s;
    int transcriptionRequests = 0;
    int chatRequests = 0;
    QByteArray lastTranscriptionBody;
    QString lastChatPrompt;
    QString lastAuthHeader;

private:
    void handleConnection()
    {
        while (QTcpSocket *sock = m_server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(sock, &QTcpSocket::readyRead, this, [this, sock, buffer]() {
                buffer->append(sock->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                const QByteArray headerBlock = buffer->left(headerEnd);
                int contentLength = 0;
                QString auth;
                for (const QByteArray &line : headerBlock.split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        contentLength = line.mid(15).trimmed().toInt();
                    } else if (line.toLower().startsWith("authorization:")) {
                        auth = QString::fromUtf8(line.mid(14).trimmed());
                    }
                }
                if (buffer->size() < headerEnd + 4 + contentLength) {
                    return;
                }

                const QByteArray firstLine = headerBlock.left(headerBlock.indexOf('\r'));
                const QByteArray body = buffer->mid(headerEnd + 4, contentLength);

                if (firstLine.contains("/audio/transcriptions")) {
                    ++transcriptionRequests;
                    lastTranscriptionBody = body;
                    lastAuthHeader = auth;
                    const QByteArray json = QStringLiteral("{\"text\":\"%1\"}").arg(transcriptionResponseText).toUtf8();
                    const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                            "Content-Type: application/json\r\n"
                                            "Content-Length: " + QByteArray::number(json.size()) + "\r\n"
                                            "Connection: close\r\n\r\n" + json;
                    sock->write(resp);
                    sock->disconnectFromHost();
                    return;
                }

                if (firstLine.contains("/models")) {
                    const QByteArray json = "{\"data\":[{\"id\":\"mock-model\"}]}";
                    const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                            "Content-Type: application/json\r\n"
                                            "Content-Length: " + QByteArray::number(json.size()) + "\r\n"
                                            "Connection: close\r\n\r\n" + json;
                    sock->write(resp);
                    sock->disconnectFromHost();
                    return;
                }

                if (firstLine.contains("/chat/completions")) {
                    ++chatRequests;
                    lastChatPrompt = QString::fromUtf8(body);
                    const QByteArray sse =
                        "data: {\"choices\":[{\"delta\":{\"content\":\"Done.\"},\"finish_reason\":null}]}\n\n"
                        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                        "data: [DONE]\n\n";
                    const QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                            "Content-Type: text/event-stream\r\n"
                                            "Content-Length: " + QByteArray::number(sse.size()) + "\r\n"
                                            "Connection: close\r\n\r\n" + sse;
                    sock->write(resp);
                    sock->disconnectFromHost();
                    return;
                }

                sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                sock->disconnectFromHost();
            });
            connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

    QTcpServer m_server;
};

} // namespace

class TestVoiceInput : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void cleanup()
    {
        VoiceInput::setMicrophoneProbeOverrideForTesting({});
    }

    void testWavEncodeParseAndDenoiseImprovesSnr()
    {
        const QVector<qint16> rawSamples = makeNoisySpeechSamples(16000);
        const QByteArray wav = VoiceInput::encodePcm16Wav(rawSamples, 16000, 1);

        QVector<qint16> parsed;
        int sr = 0;
        int ch = 0;
        QString err;
        QVERIFY2(VoiceInput::parsePcm16Wav(wav, &parsed, &sr, &ch, &err), qPrintable(err));
        QCOMPARE(sr, 16000);
        QCOMPARE(ch, 1);
        QCOMPARE(parsed.size(), rawSamples.size());

        QByteArray denoisedWav;
        QVERIFY2(VoiceInput::denoiseWavBuffer(wav, &denoisedWav, &err), qPrintable(err));

        QVector<qint16> clean;
        QVERIFY2(VoiceInput::parsePcm16Wav(denoisedWav, &clean, &sr, &ch, &err), qPrintable(err));
        QCOMPARE(clean.size(), rawSamples.size());

        const double rawNoise = rmsRange(rawSamples, 800, 5600);
        const double rawSpeech = rmsRange(rawSamples, 7200, 15200);
        const double cleanNoise = rmsRange(clean, 800, 5600);
        const double cleanSpeech = rmsRange(clean, 7200, 15200);

        const double rawSnr = rawSpeech / std::max(1e-6, rawNoise);
        const double cleanSnr = cleanSpeech / std::max(1e-6, cleanNoise);
        QVERIFY2(cleanSnr > rawSnr * 3.0,
                 qPrintable(QStringLiteral("expected denoiser SNR improvement: raw=%1 clean=%2")
                                .arg(rawSnr)
                                .arg(cleanSnr)));
    }

    void testPactlParsingFiltersMonitorAndUnpluggedSources()
    {
        // Only a dummy sink monitor exists -> NO microphone available
        const QString monitorOnly = QStringLiteral(
            "Source #44\n"
            "\tState: SUSPENDED\n"
            "\tName: auto_null.monitor\n"
            "\tDescription: Monitor of Dummy Output\n"
            "\tMonitor of Sink: auto_null\n"
            "\tProperties:\n"
            "\t\tdevice.class = \"monitor\"\n");
        const auto s1 = VoiceInput::parsePactlSourcesOutput(monitorOnly);
        QVERIFY(!s1.available);
        QVERIFY(s1.reason.contains(u"No microphone"_s, Qt::CaseInsensitive));

        // Real hardware mic whose only port is unplugged ("not available") -> unusable
        const QString unplugged = QStringLiteral(
            "Source #51\n"
            "\tState: SUSPENDED\n"
            "\tName: alsa_input.pci-0000_00_1f.3.analog-stereo\n"
            "\tDescription: Built-in Audio Analog Stereo\n"
            "\tMonitor of Sink: n/a\n"
            "\tPorts:\n"
            "\t\tanalog-input-front-mic: Front Microphone (type: Mic, priority: 8500, not available)\n"
            "\tActive Port: analog-input-front-mic\n");
        const auto s2 = VoiceInput::parsePactlSourcesOutput(unplugged);
        QVERIFY(!s2.available);
        QVERIFY(s2.reason.contains(u"unplugged"_s, Qt::CaseInsensitive));

        // Usable USB microphone alongside a monitor source -> available!
        const QString usableMic = monitorOnly + QStringLiteral(
            "Source #62\n"
            "\tState: SUSPENDED\n"
            "\tName: alsa_input.usb-Blue_Microphones_Yeti-00.analog-stereo\n"
            "\tDescription: Yeti Stereo Microphone\n"
            "\tMonitor of Sink: n/a\n"
            "\tPorts:\n"
            "\t\tanalog-input-mic: Microphone (type: Mic, priority: 8700, availability unknown)\n"
            "\tActive Port: analog-input-mic\n");
        const auto s3 = VoiceInput::parsePactlSourcesOutput(usableMic);
        QVERIFY(s3.available);
        QCOMPARE(s3.deviceName, u"Yeti Stereo Microphone"_s);

        // Explicit missing configured device -> unavailable
        const auto s4 = VoiceInput::parsePactlSourcesOutput(usableMic, u"nonexistent_mic"_s);
        QVERIFY(!s4.available);
        QVERIFY(s4.reason.contains(u"nonexistent_mic"_s));
    }

    void testWpctlAndArecordAndFfmpegParsers()
    {
        const QString wpctlEmpty = QStringLiteral(
            "Audio\n"
            " ├─ Sinks:\n"
            " │  *   43. Dummy Output [vol: 1.00]\n"
            " ├─ Sources:\n"
            " │\n"
            " └─ Streams:\n");
        QVERIFY(!VoiceInput::parseWpctlStatusOutput(wpctlEmpty).available);

        const QString wpctlWithMic = QStringLiteral(
            "Audio\n"
            " ├─ Sinks:\n"
            " │  *   43. Speakers [vol: 1.00]\n"
            " ├─ Sources:\n"
            " │  *   52. Internal Microphone [vol: 0.85]\n"
            " └─ Streams:\n");
        const auto wp = VoiceInput::parseWpctlStatusOutput(wpctlWithMic);
        QVERIFY(wp.available);
        QCOMPARE(wp.deviceName, u"Internal Microphone"_s);

        QVERIFY(!VoiceInput::parseArecordListOutput(u"arecord: device_list:279: no soundcards found..."_s).available);
        const QString arecordOk = QStringLiteral(
            "**** List of CAPTURE Hardware Devices ****\n"
            "card 1: Mic [USB Condenser], device 0: USB Audio [USB Audio]\n");
        const auto al = VoiceInput::parseArecordListOutput(arecordOk);
        QVERIFY(al.available);
        QVERIFY(al.deviceName.contains(u"USB Condenser"_s));

        const QString avf = QStringLiteral(
            "[AVFoundation indev @ 0x1] AVFoundation video devices:\n"
            "[AVFoundation indev @ 0x1] [0] FaceTime HD Camera\n"
            "[AVFoundation indev @ 0x1] AVFoundation audio devices:\n"
            "[AVFoundation indev @ 0x1] [0] MacBook Pro Microphone\n");
        const auto mac = VoiceInput::parseFfmpegDevicesOutput(avf);
        QVERIFY(mac.available);
        QCOMPARE(mac.deviceName, u"MacBook Pro Microphone"_s);
    }

    void testBackendResolution()
    {
        Settings s;
        s.voiceProvider = u"openai"_s;
        s.openaiApiKey = u"sk-openai-123"_s;
        QString target, key;
        QCOMPARE(VoiceInput::resolveBackend(s, &target, &key), u"openai"_s);
        QCOMPARE(target, u"https://api.openai.com/v1/audio/transcriptions"_s);
        QCOMPARE(key, u"sk-openai-123"_s);

        s.voiceProvider = u"openai_compatible"_s;
        s.voiceEndpoint = u"http://127.0.0.1:9000/v1"_s;
        s.voiceApiKey = u"sk-custom"_s;
        QCOMPARE(VoiceInput::resolveBackend(s, &target, &key), u"openai_compatible"_s);
        QCOMPARE(target, u"http://127.0.0.1:9000/v1/audio/transcriptions"_s);
        QCOMPARE(key, u"sk-custom"_s);

        s.voiceProvider = u"local_cli"_s;
        s.voiceLocalCommand = u"whisper-cli -m model.bin -f {file}"_s;
        QCOMPARE(VoiceInput::resolveBackend(s, &target, &key), u"local_cli"_s);
        QCOMPARE(target, u"whisper-cli -m model.bin -f {file}"_s);
    }

    void testHttpDenoiseAndTranscriptionPipeline()
    {
        MockVoiceHttpServer server;
        QVERIFY(server.start());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString wavPath = dir.filePath(u"input.wav"_s);
        QFile f(wavPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(VoiceInput::encodePcm16Wav(makeNoisySpeechSamples(), 16000, 1));
        f.close();

        Settings s;
        s.voiceProvider = u"openai_compatible"_s;
        s.voiceEndpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port());
        s.voiceApiKey = u"test-voice-key"_s;
        s.voiceModel = u"whisper-1"_s;
        s.voiceLanguage = u"en"_s;
        s.voiceNoiseRemoval = true;

        VoiceInput voice;
        voice.setSettings(s);

        QList<VoiceInput::State> seenStates;
        connect(&voice, &VoiceInput::stateChanged, this, [&seenStates](VoiceInput::State st) {
            seenStates.append(st);
        });
        QSignalSpy transcribedSpy(&voice, &VoiceInput::transcribed);
        QSignalSpy errorSpy(&voice, &VoiceInput::errorOccurred);

        voice.processAudioFile(wavPath);
        QTRY_COMPARE_WITH_TIMEOUT(transcribedSpy.count(), 1, 5000);

        QCOMPARE(errorSpy.count(), 0);
        QCOMPARE(transcribedSpy.at(0).at(0).toString(), u"Refactor the audio parser"_s);
        QCOMPARE(server.transcriptionRequests, 1);
        QCOMPARE(server.lastAuthHeader, u"Bearer test-voice-key"_s);
        QVERIFY(server.lastTranscriptionBody.contains("RIFF"));
        QVERIFY(server.lastTranscriptionBody.contains("WAVE"));
        QVERIFY(seenStates.contains(VoiceInput::State::Denoising));
        QVERIFY(seenStates.contains(VoiceInput::State::Transcribing));
        QCOMPARE(voice.state(), VoiceInput::State::Idle);
    }

    void testLocalCliTranscriptionPipeline()
    {
#ifdef KATEAI_PYTHON3
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString wavPath = dir.filePath(u"input.wav"_s);
        QFile f(wavPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(VoiceInput::encodePcm16Wav(makeNoisySpeechSamples(), 16000, 1));
        f.close();

        const QString scriptPath = dir.filePath(u"mock_whisper.py"_s);
        QFile script(scriptPath);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "import sys, os\n"
            "p = sys.argv[1]\n"
            "assert os.path.exists(p) and os.path.getsize(p) > 44\n"
            "print('[00:00:00.000 --> 00:00:02.100]  Explain the build pipeline')\n");
        script.close();

        Settings s;
        s.voiceProvider = u"local_cli"_s;
        s.voiceLocalCommand = QStringLiteral("\"%1\" \"%2\" {file}")
                                  .arg(QStringLiteral(KATEAI_PYTHON3), scriptPath);
        s.voiceNoiseRemoval = true;

        VoiceInput voice;
        voice.setSettings(s);
        QSignalSpy transcribedSpy(&voice, &VoiceInput::transcribed);
        QSignalSpy errorSpy(&voice, &VoiceInput::errorOccurred);

        voice.processAudioFile(wavPath);
        QTRY_COMPARE_WITH_TIMEOUT(transcribedSpy.count(), 1, 5000);

        QCOMPARE(errorSpy.count(), 0);
        QCOMPARE(transcribedSpy.at(0).at(0).toString(), u"Explain the build pipeline"_s);
#else
        QSKIP("Python3 interpreter not available");
#endif
    }

    void testChatWidgetVoiceButtonAvailabilityTooltipAndSendToChat()
    {
        isolateSessionStore();

        // 1. Start with NO mic available -> button inside PromptEdit must be disabled
        //    and hovering over it must show the information tooltip.
        bool micAvailable = false;
        VoiceInput::setMicrophoneProbeOverrideForTesting([&micAvailable](const QString &) {
            VoiceInput::MicrophoneStatus st;
            st.available = micAvailable;
            if (micAvailable) {
                st.deviceName = u"Studio USB Mic"_s;
            } else {
                st.reason = u"No microphone is available or usable: no audio input device detected."_s;
            }
            return st;
        });

        ChatWidget chat;
        chat.resize(480, 640);
        chat.show();
        QApplication::processEvents();

        PromptEdit *prompt = chat.promptEdit();
        QVERIFY(prompt != nullptr);
        QPushButton *micBtn = prompt->voiceButton();
        QVERIFY(micBtn != nullptr);
        QCOMPARE(micBtn->parentWidget(), prompt);
        QVERIFY(!chat.isMicrophoneAvailable());
        QVERIFY(!micBtn->isEnabled());
        QVERIFY(micBtn->toolTip().contains(u"No microphone is available or usable"_s));

        // Hover tooltip works even though the button is disabled.
        const QPoint center = micBtn->geometry().center();
        QHelpEvent hover(QEvent::ToolTip, center, prompt->mapToGlobal(center));
        QToolTip::hideText();
        QVERIFY(QApplication::sendEvent(prompt, &hover));
        QVERIFY(QToolTip::text().contains(u"No microphone is available or usable"_s));

        // Panel shortcut exists.
        auto *shortcut = chat.findChild<QShortcut *>(u"shortcutVoicePrompt"_s);
        QVERIFY(shortcut != nullptr);

        // 2. Plug in a usable mic -> refresh enables the button!
        micAvailable = true;
        chat.refreshMicrophoneAvailability();
        QVERIFY(chat.isMicrophoneAvailable());
        QVERIFY(micBtn->isEnabled());
        QVERIFY(micBtn->toolTip().contains(u"Studio USB Mic"_s));

        // 3. End-to-end: voice audio -> noise removal -> transcription -> sent to chat!
        MockVoiceHttpServer server;
        QVERIFY(server.start());
        server.transcriptionResponseText = u"Summarize voice prompting"_s;

        Settings s;
        s.provider = Provider::OpenAICompatible;
        s.apiFormat = ApiFormat::OpenAICompatible;
        s.openaiCompatibleUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port());
        s.openaiCompatibleApiKey = u"sk-chat-test"_s;
        s.openaiCompatibleModel = u"mock-model"_s;
        s.voiceProvider = u"openai_compatible"_s;
        s.voiceEndpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.port());
        s.voiceApiKey = u"sk-voice-test"_s;
        s.voiceNoiseRemoval = true;
        s.voiceAutoSend = true;
        chat.setSettings(s);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        chat.agent()->setWorkspace(dir.path());
        const QString wavPath = dir.filePath(u"spoken.wav"_s);
        QFile f(wavPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(VoiceInput::encodePcm16Wav(makeNoisySpeechSamples(), 16000, 1));
        f.close();

        QSignalSpy submitSpy(&chat, &ChatWidget::aboutToSubmit);
        chat.voiceInput()->processAudioFile(wavPath);

        QTRY_COMPARE_WITH_TIMEOUT(submitSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(server.chatRequests, 1, 5000);
        QVERIFY(server.lastChatPrompt.contains(u"Summarize voice prompting"_s));
        QTRY_VERIFY_WITH_TIMEOUT(!chat.agent()->isBusy(), 5000);

        // 4. With voiceAutoSend=false, transcription populates the prompt without auto-sending.
        s.voiceAutoSend = false;
        chat.setSettings(s);
        server.transcriptionResponseText = u"Draft voice prompt"_s;
        chat.voiceInput()->processAudioFile(wavPath);
        QTRY_COMPARE_WITH_TIMEOUT(prompt->toPlainText(), u"Draft voice prompt"_s, 5000);
        QCOMPARE(submitSpy.count(), 1);
    }
};

QTEST_MAIN(TestVoiceInput)
#include "test_voiceinput.moc"
