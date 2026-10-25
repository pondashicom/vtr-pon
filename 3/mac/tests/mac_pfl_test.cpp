#include "edit_player.h"
#include "edit_pfl_output.h"
#include "edit_pfl_protocol.h"
#include "edit_pfl_renderer.h"
#include "audio_output_settings.h"
#include "controller_settings.h"
#include "program_audio_output.h"

#include <QApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>

namespace {
std::mutex logMutex;
QString pflMessages;
void logMessage(QtMsgType, const QMessageLogContext &, const QString &message) {
    if (message.startsWith(QStringLiteral("PFL stdout"))) {
        std::lock_guard lock(logMutex);
        pflMessages += message;
    }
    const auto encoded = message.toUtf8();
    std::fprintf(stderr, "%s\n", encoded.constData());
}
bool check(bool condition, const char *message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << std::endl;
    return condition;
}
void pump(int ms, EditPlayer *edit = nullptr) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        if (edit) edit->processBusMessages();
        QThread::msleep(5);
    }
}
bool until(const std::function<bool()> &done, int timeout = 7000, EditPlayer *edit = nullptr) {
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < timeout) pump(5, edit);
    return done();
}
QString tone(const QString &directory) {
    const auto path = directory + QStringLiteral("/pfl-880Hz-quiet.wav");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 48000 * 45;
    stream.writeRawData("RIFF", 4); stream << quint32(36 + frames * 4);
    stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1)
        << quint16(2) << quint32(48000) << quint32(192000) << quint16(4) << quint16(16);
    stream.writeRawData("data", 4); stream << quint32(frames * 4);
    for (quint32 i = 0; i < frames; ++i) {
        const auto sample = static_cast<qint16>(std::sin(i * 2.0 * 3.141592653589793 * 880.0 / 48000.0) * 327.0);
        stream << sample << sample;
    }
    return path;
}
bool actualProgramRoute(ProgramAudioOutput &output, const QString &uid) {
    auto *sink = gst_bin_get_by_name(GST_BIN(output.pipeline()), "program-audio-output");
    gint device = 0; g_object_get(sink, "device", &device, nullptr); gst_object_unref(sink);
    const AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef actual = nullptr; UInt32 size = sizeof(actual);
    const auto expected = CFStringCreateWithCharacters(nullptr,
        reinterpret_cast<const UniChar *>(uid.utf16()), uid.size());
    const bool matches = AudioObjectGetPropertyData(device, &address, 0, nullptr,
        &size, &actual) == noErr && actual && CFEqual(actual, expected);
    if (actual) CFRelease(actual);
    CFRelease(expected);
    return matches;
}
struct ProgramProbe {
    std::atomic<unsigned> buffers{0};
    std::atomic<bool> contaminated{false};
    std::atomic<std::int64_t> lastNs{0}, maximumGapNs{0};
    static GstPadProbeReturn capture(GstPad *, GstPadProbeInfo *info, gpointer user) {
        auto &self = *static_cast<ProgramProbe *>(user);
        const auto now = EditPflFeed::nowNs();
        const auto previous = self.lastNs.exchange(now);
        if (previous) self.maximumGapNs.store(std::max(self.maximumGapNs.load(), now - previous));
        GstMapInfo map{};
        if (gst_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), &map, GST_MAP_READ)) {
            const auto *samples = reinterpret_cast<const float *>(map.data);
            for (gsize i = 0; i < map.size / sizeof(float); ++i)
                if (!std::isfinite(samples[i]) || std::abs(samples[i]) > 0.00001F)
                    self.contaminated = true;
            gst_buffer_unmap(GST_PAD_PROBE_INFO_BUFFER(info), &map);
            ++self.buffers;
        }
        return GST_PAD_PROBE_OK;
    }
};
bool rendererContract() {
    using namespace vtrpon::pfl;
    auto audio = std::make_unique<SharedAudio>();
    float samples[kFrames * 2]; std::fill(std::begin(samples), std::end(samples), 0.25F);
    audio->enabled = 1; audio->playing = 1;
    for (unsigned i = 0; i < kSlots; ++i)
        audio->push(samples, kFrames, 1000000000LL + i * 10000000LL, 1);
    if (!check(!audio->push(samples, kFrames, 2000000000LL, 1), "full ring drops without waiting")) return false;
    Renderer renderer;
    float output[kFrames * 2]{};
    renderer.render(*audio, output, kFrames, 1000000000LL + kLatencyNs);
    bool ok = check(output[800] > 0.249F, "timestamped PCM reaches renderer after ramp");
    audio->playing = 0;
    renderer.render(*audio, output, kFrames, 1010000000LL + kLatencyNs);
    ok &= check(std::abs(output[800]) < 0.00001F, "pause becomes silence");
    audio->generation.fetch_add(1); audio->playing = 1;
    renderer.render(*audio, output, kFrames, 1020000000LL + kLatencyNs);
    ok &= check(std::all_of(std::begin(output), std::end(output), [](float f) { return f == 0; }),
                "seek generation never replays stale PCM");
    return ok;
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    qInstallMessageHandler(logMessage);
    gst_init(nullptr, nullptr);
    const auto args = app.arguments();
    if (args.size() == 3 && args[1] == QStringLiteral("--orphan-parent")) {
        auto feed = std::make_shared<EditPflFeed>();
        EditPflOutput output(feed);
        output.setDeviceId(args[2]); output.setEnabled(true);
        if (!until([&] { return output.enabled(); })) return 1;
        const auto pid = output.findChild<QProcess *>()->processId();
        std::cout << "PFL_CHILD=" << pid << std::endl;
        while (true) pump(100); // This fixture is killed by the test parent.
    }
    const bool real = args.size() == 4 && args[1] == QStringLiteral("--devices");
    if (args.size() != 1 && !real) return 2;
    const QString programUid = real ? args[2] : QString{};
    const QString pflUid = real ? args[3] : QString{};
    if (real && (programUid.isEmpty() || pflUid.isEmpty() || programUid == pflUid)) return 2;
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    app.setProperty("vtrpon.settingsFile", directory.filePath(QStringLiteral("user.ini")));
    bool ok = rendererContract();
    auto feed = std::make_shared<EditPflFeed>();
    ok &= check(feed->available() && feed->sharedDescriptor() >= 3,
                "anonymous POSIX shared mapping exists");
    const auto start = EditPflFeed::nowNs(); QThread::msleep(2);
    ok &= check(EditPflFeed::nowNs() > start + 1000000, "Mac monotonic clock advances");
    ProgramAudioOutput program;
    if (!check(program.initialize(nullptr, nullptr, programUid, real ? nullptr : "fakesink"),
               "independent ON AIR output initializes")) return 1;
    program.updateDevice(true);
    ok &= check(until([&] { program.updateDevice(true); return !program.deviceChanging(); })
        && program.ready(), "ON AIR output becomes ready");
    if (real) ok &= check(actualProgramRoute(program, programUid), "ON AIR opens its requested HAL endpoint");
    auto *pad = gst_element_get_static_pad(program.masterLevel(), "src");
    ProgramProbe probe;
    const auto probeId = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                                          &ProgramProbe::capture, &probe, nullptr);
    // ON AIR stays silent. PFL-only PCM at its separate output must never mix
    // into this real post-master stream, even during failure and child restart.
    QWidget surface; surface.resize(320, 180);
    EditPlayer edit(&surface);
    const auto path = tone(directory.path());
    if (!check(!path.isEmpty() && edit.isAvailable() && edit.load(path), "EDIT loads quiet tone")) return 1;
    pump(300, &edit);
    ok &= check(edit.setNominalVolume(0) && edit.play(), "EDIT plays with nominal fader at silence");
    auto *audio = edit.pflFeed()->audio();
    if (!check(audio != nullptr, "EDIT owns private PCM transport")) return 1;
    audio->enabled = 1;
    pump(1400, &edit);
    ok &= check(edit.positionMs() > 1000 && audio->dropped.load() > 0,
                "EDIT timeline continues with a stalled PFL reader");
    vtrpon::pfl::Packet packet;
    float peak = 0;
    while (audio->pop(packet))
        for (unsigned i = 0; i < packet.frames * 2; ++i) peak = std::max(peak, std::abs(packet.samples[i]));
    ok &= check(peak > 0.008F && edit.audioLevel().leftPeakDb <= -59,
                "PFL receives pre-fader PCM while EDIT post-fader meter stays silent");
    const auto epoch = audio->generation.load();
    ok &= check(edit.seekAccurate(3000), "EDIT accepts seek with PFL attached"); pump(250, &edit);
    ok &= check(audio->generation.load() != epoch, "seek invalidates old PFL packets");
    edit.pause();
    ok &= check(audio->playing.load() == 0, "EDIT pause closes PFL PCM gate");
    EditPflOutput output(edit.pflFeed());
    unsigned errors = 0;
    QObject::connect(&output, &EditPflOutput::outputError, &app, [&](const QString &) { ++errors; });
    auto *child = output.findChild<QProcess *>();
    output.setDeviceId(QStringLiteral("VTRPON-PFL-MISSING-UID")); output.setEnabled(true);
    edit.play(); const auto before = edit.positionMs();
    ok &= check(until([&] { return errors > 0 && child->state() == QProcess::NotRunning; }, 7000, &edit)
        && !output.enabled() && output.deviceId() == QStringLiteral("VTRPON-PFL-MISSING-UID")
        && audio->state.load() == static_cast<unsigned>(vtrpon::pfl::State::Failed)
        && audio->error.load() == static_cast<unsigned>(kAudioHardwareBadDeviceError),
        "missing UID fails without default fallback and preserves selection");
    pump(300, &edit);
    ok &= check(edit.positionMs() > before + 200 && program.ready(), "PFL failure does not stop EDIT or ON AIR");
    if (real) {
        ok &= check(vtrpon::saveEditPflOutputDeviceId(pflUid), "PFL UID saved to isolated settings");
        output.setDeviceId(vtrpon::savedEditPflOutputDeviceId()); output.setEnabled(true);
        if (!check(until([&] { return output.enabled(); }, 7000, &edit),
                   "selected CoreAudio PFL output becomes ready")) return 1;
        pump(1800, &edit);
        {
            std::lock_guard lock(logMutex);
            ok &= check(pflMessages.contains(QStringLiteral("ready uid=") + pflUid + QStringLiteral(" device=")),
                        "PFL AudioUnit confirms the requested UID");
            float renderedPeak = 0;
            auto matches = QRegularExpression(QStringLiteral("peak=([0-9.]+)")).globalMatch(pflMessages);
            while (matches.hasNext()) renderedPeak = std::max(renderedPeak, matches.next().captured(1).toFloat());
            ok &= check(renderedPeak > 0.006F, "real PFL AudioUnit callback receives audible pre-fader tone");
        }
        ok &= check(audio->read.load() != audio->write.load() || audio->heartbeatNs.load() > start,
                    "real PFL callback heartbeat advances");
        output.setEnabled(false); pump(250, &edit);
        ok &= check(!output.enabled() && audio->enabled.load() == 0, "PFL OFF closes output gate");
        output.setEnabled(true); pump(300, &edit);
        ok &= check(output.enabled(), "PFL ON resumes the independent output");
        const auto stoppedAt = edit.positionMs();
        const auto childPid = static_cast<pid_t>(child->processId());
        if (!check(childPid > 1 && kill(childPid, SIGSTOP) == 0,
                   "private PFL child can be stalled")) return 1;
        ok &= check(until([&] { return !output.enabled() && child->state() == QProcess::NotRunning; }, 6000, &edit),
                    "stalled PFL is detected and only its child is killed");
        ok &= check(edit.positionMs() > stoppedAt + 2000 && program.ready(), "timelines survive stalled PFL output");
        output.setEnabled(true);
        ok &= check(until([&] { return output.enabled(); }, 7000, &edit), "PFL reopens after its child was killed");
        output.setDeviceId(programUid);
        ok &= check(until([&] { return output.enabled() && output.deviceId() == programUid; }, 7000, &edit),
                    "PFL device change asynchronously replaces the child");
        output.setDeviceId(pflUid);
        ok &= check(until([&] { return output.enabled() && output.deviceId() == pflUid; }, 7000, &edit),
                    "PFL can return to its independent device");
        QProcess parent;
        parent.setProcessChannelMode(QProcess::MergedChannels);
        parent.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--orphan-parent"), pflUid});
        QString log;
        const QRegularExpression pidPattern(QStringLiteral("PFL_CHILD=(\\d+)"));
        ok &= check(until([&] { log += QString::fromUtf8(parent.readAll()); return pidPattern.match(log).hasMatch(); }),
                    "parent-death fixture starts a real output child");
        const auto match = pidPattern.match(log);
        const auto orphanPid = static_cast<pid_t>(match.captured(1).toLongLong());
        parent.kill(); parent.waitForFinished(3000);
        ok &= check(orphanPid > 1 && until([&] { return kill(orphanPid, 0) != 0; }, 3000, &edit),
                    "PFL child exits after abrupt parent death");
        ok &= check(actualProgramRoute(program, programUid), "PFL operations preserve ON AIR endpoint");
    }
    ok &= check(probe.buffers.load() > 10 && !probe.contaminated.load(),
                "ON AIR post-master PCM continues without PFL audio contamination");
    std::cout << "ON AIR maximum buffer gap ms=" << probe.maximumGapNs.load() / 1000000.0 << std::endl;
    ok &= check(probe.maximumGapNs.load() < 500000000LL, "ON AIR has no 500 ms buffer stall during PFL operations");
    output.shutdown(); edit.requestShutdown();
    ok &= check(until([&] { return output.shutdownComplete() && edit.shutdownComplete(); }, 7000, &edit),
                "PFL child and EDIT stop before fixture destruction");
    edit.shutdown();
    gst_pad_remove_probe(pad, probeId); gst_object_unref(pad);
    ok &= check(program.stop(), "ON AIR output stops independently");
    std::cout << "MAC_PFL result=" << (ok ? "PASS" : "FAIL") << std::endl;
    return ok ? 0 : 1;
}
