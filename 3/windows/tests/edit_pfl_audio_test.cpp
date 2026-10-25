#include "edit_player.h"
#include "async_shutdown_test_utils.h"
#include "edit_pfl_output.h"
#include "edit_pfl_protocol.h"
#include "edit_pfl_renderer.h"
#include "audio_output_settings.h"

#include <QApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <gst/gst.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

namespace {
bool require(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
void pump(EditPlayer &player, int ms) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(); player.processBusMessages();
        QThread::msleep(5);
    }
}
QString createTone(const QString &directory) {
    const auto path = directory + QStringLiteral("/pfl-tone.wav");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 samples = 48000 * 12;
    stream.writeRawData("RIFF", 4); stream << quint32(36 + samples * 4);
    stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1)
        << quint16(2) << quint32(48000) << quint32(192000) << quint16(4) << quint16(16);
    stream.writeRawData("data", 4); stream << quint32(samples * 4);
    for (quint32 i = 0; i < samples; ++i) {
        const auto sample = static_cast<qint16>(std::sin(i * 2.0 * 3.141592653589793 * 2500.0 / 48000.0) * 8192.0);
        stream << sample << sample;
    }
    return path;
}
bool rendererContract() {
    using namespace vtrpon::pfl;
    auto audio = std::make_unique<SharedAudio>();
    float source[kFrames * 2]; std::fill(std::begin(source), std::end(source), 0.25F);
    const auto epoch = audio->generation.load();
    for (std::uint32_t i = 0; i < kSlots; ++i)
        if (!audio->push(source, kFrames, 1000000000LL + i * 10000000LL, epoch)) return false;
    bool ok = require(!audio->push(source, kFrames, 2000000000LL, epoch), "full queue must drop immediately");
    audio->enabled = 1; audio->playing = 1;
    Renderer renderer;
    float output[kFrames * 2]{};
    renderer.render(*audio, output, kFrames, 1000000000LL + kLatencyNs);
    ok &= require(output[800] > 0.249F, "PCM reaches output after onset ramp");
    audio->enabled = 0;
    renderer.render(*audio, output, kFrames, 1010000000LL + kLatencyNs);
    ok &= require(std::abs(output[800]) < 0.00001F, "OFF becomes silence");
    audio->generation.fetch_add(1); audio->enabled = 1;
    renderer.render(*audio, output, kFrames, 1020000000LL + kLatencyNs);
    ok &= require(std::all_of(std::begin(output), std::end(output), [](float f) { return f == 0.0F; }),
                  "a flushed queue must never replay old audio");
    ok &= require(vtrpon::sameAudioOutputDevice({}, QStringLiteral("ABC"), QStringLiteral("abc")), "default alias equality");
    ok &= require(!vtrpon::sameAudioOutputDevice({}, {}, QStringLiteral("abc")), "No Device is never a conflict");
    // Device clock observations may move by milliseconds between blocks.
    // A continuous tone must not acquire block-boundary jumps from that jitter.
    audio = std::make_unique<SharedAudio>();
    audio->enabled = 1; audio->playing = 1;
    Renderer continuous;
    float previous = 0, maxStep = 0;
    for (std::uint32_t block = 0; block < 50; ++block) {
        for (std::uint32_t i = 0; i < kFrames; ++i) {
            const auto sample = static_cast<float>(0.25 * std::sin(
                (block * kFrames + i) * 2.0 * 3.141592653589793 * 440.0 / kRate));
            source[i * 2] = source[i * 2 + 1] = sample;
        }
        audio->push(source, kFrames, 1000000000LL + block * 10000000LL, 1);
        const std::int64_t jitter = block == 0 ? 0 : block % 2 ? 2000000 : -2000000;
        continuous.render(*audio, output, kFrames, 1000000000LL + block * 10000000LL + kLatencyNs + jitter);
        for (std::uint32_t i = 0; i < kFrames; ++i) {
            maxStep = (std::max)(maxStep, std::abs(output[i * 2] - previous));
            previous = output[i * 2];
        }
    }
    std::cout << "clock_jitter_max_step=" << maxStep << "\n";
    ok &= require(maxStep < 0.02F, "device clock jitter must not cause sample discontinuities");
    audio = std::make_unique<SharedAudio>();
    audio->enabled = 1; audio->playing = 1;
    Renderer recovering;
    std::fill(std::begin(source), std::end(source), 0.25F);
    audio->push(source, kFrames, 1000000000LL, 1);
    recovering.render(*audio, output, kFrames, 1000000000LL + kLatencyNs);
    // A delayed output callback must resume current PCM, not spend minutes
    // catching up at the small normal clock-drift correction rate.
    audio->push(source, kFrames, 2000000000LL, 1);
    recovering.render(*audio, output, kFrames, 2000000000LL + kLatencyNs);
    ok &= require(output[800] > 0.249F, "PFL recovers current audio after a delayed output callback");
    return ok;
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir settingsDirectory;
    if (!require(settingsDirectory.isValid(), "isolated settings directory exists")) return 1;
    app.setProperty("vtrpon.settingsFile", settingsDirectory.filePath(QStringLiteral("user.ini")));
    const QList<AudioOutputDevice> devices{{QStringLiteral("PROGRAM-ID"), QStringLiteral("Program")},
                                         {QStringLiteral("PFL-ID"), QStringLiteral("PFL")}};
    const auto present = vtrpon::missingAudioOutputs(devices, QStringLiteral("PROGRAM-ID"),
                                                    {}, QStringLiteral("pfl-id"));
    if (!require(!present.program && !present.pfl, "default Program and case-insensitive PFL are found")) return 1;
    const auto missing = vtrpon::missingAudioOutputs({}, {}, QStringLiteral("PROGRAM-ID"),
                                                    QStringLiteral("PFL-ID"));
    if (!require(missing.program && missing.pfl, "unplugging both endpoints reports both missing")) return 1;
    const auto noPfl = vtrpon::missingAudioOutputs(devices, QStringLiteral("PROGRAM-ID"), {}, {});
    if (!require(!noPfl.program && !noPfl.pfl, "unconfigured PFL is not a background warning")) return 1;
    const auto noFallback = vtrpon::missingAudioOutputs(devices, QStringLiteral("PROGRAM-ID"),
                                                       QStringLiteral("MISSING-ID"), {});
    if (!require(noFallback.program, "explicit missing Program never falls back to default")) return 1;
    const auto missingPfl = vtrpon::missingAudioOutputs(devices, QStringLiteral("PROGRAM-ID"),
                                                       {}, QStringLiteral("MISSING-PFL-ID"));
    if (!require(!missingPfl.program && missingPfl.pfl,
            "missing PFL remains missing even when the default Program endpoint is available")) return 1;
    const auto reconnected = vtrpon::missingAudioOutputs(devices, {}, QStringLiteral("PROGRAM-ID"),
                                                        QStringLiteral("PFL-ID"));
    if (!require(!reconnected.program && !reconnected.pfl, "reconnected endpoints clear their missing state")) return 1;
    const auto args = app.arguments();
    const auto defaultPfl = QString::fromLatin1(vtrpon::kSystemDefaultPflDeviceId);
    if (!require(!vtrpon::missingAudioOutputs(devices, QStringLiteral("PFL-ID"),
            QStringLiteral("PROGRAM-ID"), defaultPfl).pfl, "PFL system default resolves to an active endpoint")) return 1;
    if (!require(vtrpon::missingAudioOutputs(devices, {}, QStringLiteral("PROGRAM-ID"),
            defaultPfl).pfl, "PFL system default without an endpoint remains missing")) return 1;
    if (!require(vtrpon::sameAudioOutputDevice({}, defaultPfl, QStringLiteral("PROGRAM-ID")),
            "default PFL and default Program show the same-device warning")) return 1;
    gst_init(nullptr, nullptr);
    if (!rendererContract()) return 1;
    QTemporaryDir directory;
    const auto mediaIndex = args.indexOf(QStringLiteral("--media"));
    const auto path = mediaIndex >= 0 && mediaIndex + 1 < args.size()
        ? args[mediaIndex + 1] : createTone(directory.path());
    if (path.isEmpty()) return 2;
    QWidget surface; surface.resize(320, 180);
    EditPlayer edit(&surface);
    bool ok = require(edit.isAvailable() && edit.load(path), "EDIT backend loads tone");
    pump(edit, 250);
    ok &= require(edit.setNominalVolume(0.0) && edit.play(), "EDIT plays with nominal silence");
    auto *audio = edit.pflFeed()->audio();
    if (!require(audio != nullptr, "private shared memory exists")) return 3;
    audio->enabled = 1;
    // Intentionally never drain PFL: this reproduces a dead/blocked output.
    pump(edit, 1500);
    ok &= require(edit.positionMs() > 1200 && audio->dropped.load() > 0,
                  "EDIT advances when its PFL consumer is stalled");
    vtrpon::pfl::Packet packet;
    float peak = 0;
    while (audio->pop(packet))
        for (std::uint32_t i = 0; i < packet.frames * 2; ++i) peak = (std::max)(peak, std::abs(packet.samples[i]));
    if (mediaIndex < 0)
        ok &= require(peak > 0.20F, "PFL is tapped before nominal volume");
    ok &= require(edit.audioLevel().leftPeakDb <= -59.0, "existing post-fader meter stays silent");
    if (mediaIndex < 0) {
        ok &= require(edit.setNominalDelayFrames(6), "EDIT accepts 6-frame nominal delay");
        ok &= require(!edit.setNominalDelayFrames(7), "EDIT rejects out-of-range delay");
        pump(edit, 250);
        ok &= require(edit.setNominalDelayFrames(0), "EDIT returns to delay OFF");
        ok &= require(edit.setNominalEq(0.0, -15.0, 0.0), "EDIT accepts MG MID -15 dB");
        pump(edit, 150);
        while (audio->pop(packet)) {}
        pump(edit, 150);
        float filteredPeak = 0;
        while (audio->pop(packet))
            for (std::uint32_t i = 0; i < packet.frames * 2; ++i)
                filteredPeak = (std::max)(filteredPeak, std::abs(packet.samples[i]));
        const double expectedPeak = 0.25 * std::pow(10.0, -15.0 / 20.0);
        std::cout << "MG MID PFL peak=" << filteredPeak << " expected=" << expectedPeak << '\n';
        ok &= require(std::abs(filteredPeak - expectedPeak) < 0.003,
                      "2.5 kHz nominal EQ applies -15 dB to PFL before the silent nominal fader");
        ok &= require(edit.setNominalEq(0.0, 0.0, 0.0), "EDIT restores flat EQ");
    }
    const auto beforeSeek = audio->generation.load();
    ok &= require(edit.seekAccurate(3000), "seek remains operational with PFL attached");
    pump(edit, 300);
    ok &= require(audio->generation.load() != beforeSeek && edit.positionMs() >= 3200,
                  "seek invalidates old audio and advances");
    edit.pause();
    ok &= require(audio->playing.load() == 0, "pause closes the PCM gate");
    EditPflOutput output(edit.pflFeed());
    int outputErrors = 0;
    QObject::connect(&output, &EditPflOutput::outputError, &app,
                     [&](const QString &) { ++outputErrors; });
    output.setDeviceId(QStringLiteral("invalid-pfl-endpoint-for-isolation-test"));
    output.setEnabled(true);
    edit.play();
    const auto beforeFailure = edit.positionMs();
    pump(edit, 1200);
    ok &= require(!output.enabled() && edit.positionMs() > beforeFailure + 900,
                  "PFL device failure does not stop EDIT");
    ok &= require(outputErrors > 0 &&
                  output.deviceId() == QStringLiteral("invalid-pfl-endpoint-for-isolation-test"),
                  "PFL device failure reports an error and preserves the selected endpoint");
    const auto deviceIndex = args.indexOf(QStringLiteral("--audition-device"));
    if (deviceIndex >= 0 && deviceIndex + 1 < args.size()) {
        output.setDeviceId(args[deviceIndex + 1]); output.setEnabled(true);
        pump(edit, 1800);
        ok &= require(output.enabled(), "selected real PFL endpoint opens");
        output.setEnabled(false); pump(edit, 400);
        output.setEnabled(true); pump(edit, 1800);
        ok &= require(output.enabled(), "PFL toggles without rebuilding EDIT");
        bool canEnableAtLastSignal = false;
        const auto stateConnection = QObject::connect(&output, &EditPflOutput::stateChanged,
            &app, [&] { canEnableAtLastSignal = output.canEnable(); });
        output.setEnabled(false);
        output.setDeviceId(QStringLiteral("another-endpoint-while-off"));
        pump(edit, 600);
        ok &= require(output.canEnable() && canEnableAtLastSignal && !output.enabled(),
                      "OFF device change re-enables the button after the old child exits");
        QObject::disconnect(stateConnection);
        std::cout << "AUDITION_COMPLETE\n";
    }
    output.shutdown();
    QElapsedTimer retirement;
    retirement.start();
    ok &= require(edit.load(path) && edit.load(path), "rapid EDIT replacement accepts the latest load");
    ok &= require(retirement.elapsed() < 100 && !edit.hasMedia() && !edit.play(),
                  "EDIT replacement returns promptly and gates transport until old media stops");
    pump(edit, 400);
    ok &= require(edit.hasMedia() && edit.play(), "EDIT replacement becomes usable after stop completion");
    retirement.restart();
    (void)edit.unload();
    ok &= require(retirement.elapsed() < 100 && !edit.hasMedia(), "EDIT unload never waits on the caller");
    pump(edit, 400);
    ok &= require(edit.unload() && !edit.hasMedia(), "EDIT unload completion permits item removal");
    ok &= require(edit.load(path), "EDIT can load again after unloading");
    (void)edit.unload();
    pump(edit, 400);
    ok &= require(!edit.hasMedia(), "unload cancels a pending EDIT load without resurrecting it");

    const auto stillPath = directory.filePath(QStringLiteral("capture-source.png"));
    QImage still(1920, 1080, QImage::Format_RGB32);
    still.fill(QColor("#4080c0"));
    ok &= require(still.save(stillPath) && edit.load(stillPath), "capture fixture loads");
    VideoFrameCapture capture;
    for (int i = 0; i < 100 && !capture; ++i) {
      pump(edit, 30);
      capture = edit.captureStillFrame();
    }
    const QImage captured = capture ? capture() : QImage{};
    ok &= require(captured.size() == QSize(1920, 1080) &&
                  captured.pixelColor(960, 540) == QColor("#4080c0"),
                  "paused EDIT GPU frame is captured at 1920x1080 with correct pixels");
    (void)edit.unload();
    ok &= require(!edit.captureStillFrame(), "unloaded EDIT cannot capture a stale frame");
    capture = {};
    pump(edit, 300);
    edit.requestShutdown();
    ok &= require(awaitShutdown([&] { return edit.shutdownComplete() && output.shutdownComplete(); }),
                  "EDIT stop worker and PFL child finish before fixture destruction");
    edit.shutdown();
    std::cout << "pre_fader_peak=" << peak << " dropped=" << audio->dropped.load()
              << " result=" << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
