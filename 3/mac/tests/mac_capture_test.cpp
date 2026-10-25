#include "mac_capture_input.h"
#include "program_audio_output.h"
#include "uvc_source_session_manager.h"
#include "deck_source_session.h"
#include "program_gpu_compositor.h"
#include "controller_settings.h"
#include "live_audio_source.h"
#include "uvc_fault_test_source.h"
#include "live_audio_test_source.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QDebug>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <functional>
#include <cmath>

namespace {
bool check(bool ok, const char *label) { qInfo() << (ok ? "PASS" : "FAIL") << label; return ok; }
void pump(int milliseconds) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(5); }
}
bool waitFor(const std::function<bool()> &condition, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) pump(10);
    return condition();
}

bool frameCapsChecks() {
    auto *pipeline = gst_parse_launch(
        "appsrc name=input is-live=true format=time block=false ! tee name=t "
        "t. ! queue ! videoconvert ! videoscale ! video/x-raw,format=BGRA,width=1920,height=1080 ! "
        "appsink name=program sync=false async=false max-buffers=2 drop=true wait-on-eos=false "
        "t. ! queue ! videorate drop-only=true ! videoscale ! videoconvert ! "
        "video/x-raw,format=RGBA,width=160,height=90,framerate=3/1 ! "
        "appsink name=thumbnail sync=false async=false max-buffers=2 drop=true wait-on-eos=false "
        "t. ! queue ! videorate drop-only=true ! videoscale ! videoconvert ! "
        "video/x-raw,format=RGBA,width=640,height=360,framerate=15/1 ! "
        "appsink name=edit sync=false async=false max-buffers=2 drop=true wait-on-eos=false", nullptr);
    if (!check(pipeline != nullptr, "camera frame fan-out pipeline creates")) return false;
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    auto *caps = vtrpon::macCameraFrameCaps(320, 180);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    auto *capsText = gst_caps_to_string(caps);
    qInfo() << "Native camera caps:" << capsText;
    g_free(capsText);
    gst_caps_unref(caps);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    auto *bus = gst_element_get_bus(pipeline);
    bool ok = true;
    for (int frame = 0; frame < 25; ++frame) {
        auto *buffer = gst_buffer_new_allocate(nullptr, 320 * 180 * 4, nullptr);
        GstMapInfo map{};
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            for (size_t i = 0; i < map.size; i += 4) {
                map.data[i] = 0; map.data[i+1] = 0; map.data[i+2] = 255; map.data[i+3] = 255;
            }
            gst_buffer_unmap(buffer, &map);
        }
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame, GST_SECOND, 30);
        gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        pump(35);
    }
    for (const char *name : {"program", "thumbnail", "edit"}) {
        auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), name);
        auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0);
        ok &= check(sample != nullptr, name);
        if (sample) {
            GstVideoInfo info{};
            GstMapInfo map{};
            auto *buffer = gst_sample_get_buffer(sample);
            const bool mapped = gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
                gst_buffer_map(buffer, &map, GST_MAP_READ);
            ok &= check(mapped, "negotiated frame can be mapped");
            if (mapped) {
                const bool bgra = GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRA;
                ok &= check(map.data[bgra ? 2 : 0] == 255 && map.data[bgra ? 0 : 2] == 0,
                            "camera red survives branch format conversion");
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
        gst_object_unref(sink);
    }
    auto *error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (error) {
        GError *detail = nullptr; gchar *debug = nullptr;
        gst_message_parse_error(error, &detail, &debug);
        qWarning() << (detail ? detail->message : "") << (debug ? debug : "");
        g_clear_error(&detail); g_free(debug); gst_message_unref(error);
    }
    ok &= check(error == nullptr, "no negotiation error in any shared video branch");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus); gst_object_unref(source); gst_object_unref(pipeline);
    return ok;
}

bool nativeChecks() {
    QString cameraError, audioError;
    const auto cameras = vtrpon::macCameraDevices(&cameraError);
    const auto inputs = vtrpon::macAudioInputDevices(&audioError);
    qInfo() << "Native cameras:" << cameras.size() << cameraError << "audio inputs:" << inputs.size() << audioError;
    bool ok = true;
    for (const auto &camera : cameras) {
        qInfo() << "Camera:" << camera.displayName << camera.devicePath;
        ok &= check(camera.devicePath.startsWith("avfoundation:") && !camera.displayName.isEmpty(), "camera identity uses native UID");
    }
    for (const auto &input : inputs) {
        qInfo() << "Audio input:" << input.displayName << input.deviceId;
        ok &= check(input.deviceId.startsWith("coreaudio-input:") && !input.isDefault, "audio identity is an explicit native UID");
    }
    ok &= check(vtrpon::registerMacCaptureSources(), "native source registration");
    for (const auto &[factory, id] : {std::pair{"vtrponavfvideosrc", "avfoundation:VTRPON-missing-camera"},
                                       std::pair{"vtrponcoreaudiosrc", "coreaudio-input:VTRPON-missing-input"}}) {
        auto *pipeline = gst_pipeline_new(nullptr);
        auto *source = gst_element_factory_make(factory, "selected-input");
        auto *sink = gst_element_factory_make("fakesink", nullptr);
        if (!check(source && sink, "native source elements")) return false;
        g_object_set(source, "device", id, nullptr);
        gst_bin_add_many(GST_BIN(pipeline), source, sink, nullptr);
        ok &= check(gst_element_link(source, sink), "native source exposes a usable pad");
        auto job = AsyncPipelineStart::start(pipeline);
        ok &= check(waitFor([&] { return job->done(); }) && !job->succeeded(), "missing native UID fails without opening a default device");
        auto *bus = gst_element_get_bus(pipeline);
        auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        ok &= check(message != nullptr, "native failure is reported on the pipeline bus");
        if (message) {
            GError *error = nullptr; gst_message_parse_error(message, &error, nullptr);
            ok &= check(error && QString::fromUtf8(error->message).contains("missing or disconnected"), "device-unavailable error remains explicit");
            if (error) g_error_free(error); gst_message_unref(message);
        }
        auto stop = AsyncPipelineStop::start(pipeline, {}, AsyncPipelineStart::retire(job));
        ok &= check(waitFor([&] { return stop->done(); }) && stop->succeeded(), "failed native capture stops safely");
        gst_object_unref(bus); gst_object_unref(pipeline);
    }
    UvcDeviceMonitor videoMonitor; AudioInputDeviceMonitor audioMonitor;
    pump(250);
    videoMonitor.requestShutdown(); audioMonitor.requestShutdown();
    ok &= check(waitFor([&] { return videoMonitor.shutdownComplete() && audioMonitor.shutdownComplete(); }), "native monitor workers stop without UI waits");
    return ok;
}

bool sharedVideoChecks() {
    UvcFaultTest::pattern.store(4);
    bool ok = check(UvcFaultTest::install("vtrponavfvideosrc"), "process-local camera fixture");
    ok &= check(substituteLiveAudioSource("vtrponcoreaudiosrc"), "process-local mapped audio fixture");
    QWidget output, monitor; output.resize(640, 360); monitor.resize(320, 180); output.show(); monitor.show(); pump(100);
    ProgramGpuCompositor compositor(output.winId(), monitor.winId());
    if (!check(compositor.start(), "actual Metal compositor")) return false;
    UvcSourceSessionManager manager(false);
    const QString camera = "avfoundation:shared-test-camera";
    manager.setAvailableDevicePaths({camera});
    manager.setRegisteredDevicePaths({camera});
    manager.setPreviewDevicePaths({camera});
    manager.setEditPreviewPath(camera);
    int editFrames = 0, failures = 0;
    QObject::connect(&manager, &UvcSourceSessionManager::editPreviewReady, [&](const QString &, const QImage &image) { if (!image.isNull()) ++editFrames; });
    QObject::connect(&manager, &UvcSourceSessionManager::sourceError, [&](const QString &, const QString &) { ++failures; });
    ok &= check(waitFor([&] { return manager.sourceAvailable(camera) && editFrames > 0; }), "shared thumbnail and EDIT receive frames");
    ok &= check(UvcFaultTest::liveSources.load() == 1, "thumbnail and EDIT share one camera");
    DeckSourceSession a, b;
    ok &= check(a.initialize(0, 1920, 1080) && b.initialize(1, 1920, 1080), "usual deck resources");
    a.attachSourceManagers(&manager, nullptr, nullptr, &compositor);
    b.attachSourceManagers(&manager, nullptr, nullptr, &compositor);
    compositor.configureDeckInput(0, a.pipeline(), a.appSink());
    compositor.configureDeckInput(1, b.pipeline(), b.appSink());
    auto prepare = [&](DeckSourceSession &deck, const QString &audio = QString{}) {
        const auto first = deck.prepare({DualDeckPlayer::DeckSource::Type::Uvc, camera, audio});
        if (first.state == DualDeckPlayer::CueStatus::Error) return false;
        return waitFor([&] { return deck.refreshPreparation().state != DualDeckPlayer::CueStatus::Loading; }) &&
            deck.refreshPreparation().state == DualDeckPlayer::CueStatus::Ready;
    };
    ok &= check(prepare(a) && prepare(b, vtrpon::macAudioInputId("mapped-test")), "both decks attach the shared Mac input and become ready");
    ok &= check(!a.hasAudio() && b.hasAudio(), "mapped UAC audio is attached only to the selected deck");
    ok &= check(UvcFaultTest::liveSources.load() == 1, "both decks, EDIT and thumbnail still use one capture");
    compositor.setDeckLayers(0, 1, -1, 0); pump(200);
    auto readback = compositor.captureStillFrame();
    QImage first = readback ? readback() : QImage{};
    ok &= check(!first.isNull() && first.size() == QSize(1920, 1080), "shared input reaches the final 1080p Metal frame");
    if (!first.isNull()) {
        const auto pixel = first.pixelColor(960, 540);
        ok &= check(pixel.red() > 240 && pixel.green() < 15 && pixel.blue() < 15, "camera colors reach Metal without channel swaps");
    }
    compositor.setDeckLayers(1, .5, 0, .5); pump(100);
    ok &= check(a.sourceIsPlaying() && b.sourceIsPlaying(), "shared input survives a two-deck transition");
    a.setLiveFrameUpdates(false); pump(80);
    ok &= check(a.uvcProgramPaused() && manager.sourceIsPlaying(camera), "pausing one deck keeps the shared capture running");
    a.setLiveFrameUpdates(true);
    ok &= check(a.standbyAfterTake().succeeded && b.clearAfterOffAir().succeeded, "TAKE and OFF AIR release only deck bindings");
    ok &= check(waitFor([&] { return !b.retirementPending(); }) && testAudioSources.load() == 0, "mapped UAC audio retires after OFF AIR");
    ok &= check(manager.sourceIsPlaying(camera) && UvcFaultTest::liveSources.load() == 1, "EDIT continues after both decks release");
    // A camera disappearing never stops an unrelated producing pipeline.
    auto *unrelated = gst_parse_launch("videotestsrc is-live=true pattern=blue ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=30/1 ! appsink name=frames max-buffers=2 drop=true sync=false wait-on-eos=false", nullptr);
    auto *unrelatedSink = gst_bin_get_by_name(GST_BIN(unrelated), "frames");
    compositor.configureDeckInput(0, unrelated, unrelatedSink);
    compositor.setDeckLayers(0, 1, -1, 0);
    gst_element_set_state(unrelated, GST_STATE_PLAYING);
    pump(150);
    const auto renderedBefore = compositor.renderedFrameCount();
    manager.setAvailableDevicePaths({});
    pump(100);
    ok &= check(manager.sourceOffline(camera) && failures == 1, "disconnect reports the affected input offline once");
    GstState state = GST_STATE_NULL; gst_element_get_state(unrelated, &state, nullptr, GST_SECOND);
    ok &= check(state == GST_STATE_PLAYING && compositor.isAvailable() && compositor.renderedFrameCount() > renderedBefore, "unrelated pipeline and final compositor survive camera loss");
    readback = compositor.captureStillFrame();
    const auto unaffected = readback ? readback() : QImage{};
    ok &= check(!unaffected.isNull() && unaffected.pixelColor(960, 540).blue() > 240, "unrelated on-air frame stays blue after camera loss");
    manager.setAvailableDevicePaths({camera});
    ok &= check(waitFor([&] { return manager.sourceAvailable(camera); }), "same native identity recovers after reappearance");
    // A blocked stop must leave the event loop responsive and retain storage.
    UvcFaultTest::holdStop.store(true); UvcFaultTest::stopEntered.store(false);
    manager.setRegisteredDevicePaths({}); manager.setPreviewDevicePaths({}); manager.setEditPreviewPath({});
    int ticks = 0; QTimer timer; timer.setInterval(5); QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; }); timer.start();
    pump(180);
    ok &= check(UvcFaultTest::stopEntered.load() && ticks > 5 && UvcFaultTest::liveSources.load() == 1, "blocked capture stop retains resources while GUI timers continue");
    UvcFaultTest::holdStop.store(false);
    ok &= check(waitFor([&] { return UvcFaultTest::liveSources.load() == 0; }), "late capture stop safely releases its source");
    manager.requestShutdown(); ok &= check(waitFor([&] { return manager.shutdownComplete(); }), "manager shutdown confirmed");
    compositor.prepareShutdown();
    ok &= check(a.stopPipelinesForShutdown() && b.stopPipelinesForShutdown() && compositor.stop(), "deck and Metal shutdown confirmed");
    a.releaseResourcesAfterConfirmedShutdown(); b.releaseResourcesAfterConfirmedShutdown();
    gst_element_set_state(unrelated, GST_STATE_NULL); gst_object_unref(unrelatedSink); gst_object_unref(unrelated);
    return ok;
}

bool audioChecks() {
    testAudioSourceConfigure = [](GstElement *source) {
        g_object_set(source, "samplesperbuffer", 480, nullptr); // 10 ms, matching the real UAC trace.
        auto *pad = gst_element_get_static_pad(source, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *info, gpointer) {
                auto *buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
                GST_PAD_PROBE_INFO_DATA(info) = buffer;
                // Actual osxaudiosrc 1.28.1 under the shared system clock:
                // every capture block is clamped to PTS=0, despite valid PCM.
                GST_BUFFER_PTS(buffer) = 0;
                return GST_PAD_PROBE_OK;
            }, nullptr, nullptr);
        gst_object_unref(pad);
        vtrpon::installMacAudioClockMapping(source);
    };
    bool ok = check(substituteLiveAudioSource("vtrponcoreaudiosrc"), "process-local audio fixture");
    QWidget output, monitor; output.show(); monitor.show(); pump(40);
    ProgramGpuCompositor compositor(output.winId(), monitor.winId());
    if (!compositor.start()) return false;
    UvcSourceSessionManager manager(false);
    DeckSourceSession deck; ok &= deck.initialize(0, 1920, 1080);
    deck.attachSourceManagers(&manager, nullptr, nullptr, &compositor);
    compositor.configureDeckInput(0, deck.pipeline(), deck.appSink());
    ProgramAudioOutput audioOutput;
    ok &= check(audioOutput.initialize(nullptr, nullptr, {}, "fakesink"), "production master audio graph");
    audioOutput.updateDevice(true);
    ok &= check(waitFor([&] { audioOutput.updateDevice(true); return audioOutput.ready(); }), "master output ready");
    audioOutput.setDeckAudible(0, true);
    std::atomic<float> masterPeak{-1};
    auto *masterPad = gst_element_get_static_pad(audioOutput.masterLevel(), "src");
    const auto masterProbe = gst_pad_add_probe(masterPad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *info, gpointer value) {
            GstMapInfo map{};
            if (gst_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), &map, GST_MAP_READ)) {
                const auto *samples = reinterpret_cast<const float *>(map.data);
                float maximum = 0;
                for (size_t i = 0; i < map.size / sizeof(float); ++i) maximum = std::max(maximum, std::abs(samples[i]));
                static_cast<std::atomic<float> *>(value)->store(maximum);
                gst_buffer_unmap(GST_PAD_PROBE_INFO_BUFFER(info), &map);
            }
            return GST_PAD_PROBE_OK;
        }, &masterPeak, nullptr);
    const auto id = vtrpon::macAudioInputId("test-uid");
    UvcAudioMappingStore mapping;
    ok &= check(mapping.setAudioDeviceIdFor("avfoundation:test", id) && UvcAudioMappingStore{}.audioDeviceIdFor("avfoundation:test") == id, "camera/audio mapping persists native identities in isolated settings");
    const auto audio = vtrpon::liveAudioSourcePath(id);
    holdTestAudioOpen.store(true); testAudioOpenEntered.store(false);
    const auto start = deck.prepare({DualDeckPlayer::DeckSource::Type::Uvc, audio});
    ok &= check(start.state == DualDeckPlayer::CueStatus::Loading, "audio input starts asynchronously");
    int ticks = 0; QTimer timer; timer.setInterval(5); QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; }); timer.start(); pump(120);
    ok &= check(testAudioOpenEntered.load() && ticks > 5 && deck.refreshPreparation().state == DualDeckPlayer::CueStatus::Loading, "blocked audio open never freezes the GUI or reports ready");
    holdTestAudioOpen.store(false);
    ok &= check(waitFor([&] { return deck.refreshPreparation().state == DualDeckPlayer::CueStatus::Ready; }) && deck.hasAudio() && !deck.hasVideo(), "selected live audio becomes ready after activation");
    gchar *savedId = nullptr;
    auto *source = gst_bin_get_by_name(GST_BIN(deck.audioPipeline()), "deck-a-uvc-audio-source");
    g_object_get(source, "device", &savedId, nullptr);
    ok &= check(QString::fromUtf8(savedId) == id, "audio pipeline opens the selected persistent identity"); g_free(savedId); gst_object_unref(source);
    deck.setItemVolume(.25);
    auto *volume = gst_bin_get_by_name(GST_BIN(deck.audioPipeline()), "deck-a-uvc-item-audio-volume");
    auto *pad = gst_element_get_static_pad(volume, "src");
    struct PcmState {
        std::atomic<float> peak{-1};
        std::atomic<int> nonAdvancing{0};
        GstClockTime previous = GST_CLOCK_TIME_NONE;
    } pcm;
    auto &peak = pcm.peak;
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad *, GstPadProbeInfo *info, gpointer value) {
        auto &state = *static_cast<PcmState *>(value);
        auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (GST_CLOCK_TIME_IS_VALID(state.previous) && GST_BUFFER_PTS(buffer) <= state.previous)
            ++state.nonAdvancing;
        state.previous = GST_BUFFER_PTS(buffer);
        GstMapInfo map{};
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const auto *samples = reinterpret_cast<const float *>(map.data);
            float maximum = 0;
            for (size_t i = 0; i < map.size / sizeof(float); ++i) maximum = std::max(maximum, std::abs(samples[i]));
            state.peak.store(maximum);
            gst_buffer_unmap(buffer, &map);
        }
        return GST_PAD_PROBE_OK;
    }, &pcm, nullptr);
    gst_object_unref(pad); gst_object_unref(volume);
    ok &= check(waitFor([&] { return peak.load() > .05f && peak.load() < .07f; }), "real live PCM is converted to 48 kHz stereo and receives item gain");
    pump(1500); // Cross the mixer's discontinuity grace period, not just the first block.
    ok &= check(pcm.nonAdvancing.load() == 0, "native clock mapping produces advancing capture timestamps");
    ok &= check(waitFor([&] { return masterPeak.load() > .05f && masterPeak.load() < .07f; }),
                "live input crosses PCM bridge and actual master stage with item gain");
    qInfo() << "Live input master peak" << masterPeak.load();
    audioOutput.setMasterVolume(.5);
    ok &= check(waitFor([&] { return masterPeak.load() > .025f && masterPeak.load() < .035f; }), "master fader changes live input PCM");
    audioOutput.setMasterMuted(true);
    ok &= check(waitFor([&] { return masterPeak.load() == 0; }), "master mute silences live PCM");
    audioOutput.setMasterMuted(false); audioOutput.setMasterVolume(1);
    deck.setItemVolume(0);
    ok &= check(waitFor([&] { return peak.load() == 0 && masterPeak.load() == 0; }), "item mute makes live PCM silent through master");
    ok &= check(deck.clearAfterOffAir().succeeded && waitFor([&] { return !deck.retirementPending(); }), "mapped audio stops asynchronously after OFF AIR");
    ok &= check(testAudioSources.load() == 0, "audio source released after confirmed stop");
    // Retiring during activation waits for the opener without reviving a deck.
    holdTestAudioOpen.store(true); testAudioOpenEntered.store(false);
    deck.prepare({DualDeckPlayer::DeckSource::Type::Uvc, audio});
    ok &= waitFor([&] { return testAudioOpenEntered.load(); });
    ok &= check(deck.clearAfterOffAir().succeeded && deck.retirementPending(), "OFF AIR detaches an input whose driver is still opening");
    pump(80);
    ok &= check(testAudioSources.load() == 1 && deck.sourceType() == DeckSourceSession::Type::None, "late opener retains storage without restoring the old deck");
    holdTestAudioOpen.store(false);
    ok &= check(waitFor([&] { return !deck.retirementPending(); }) && testAudioSources.load() == 0, "late audio activation is stopped and released in order");
    const auto missing = vtrpon::liveAudioSourcePath("missing-input");
    deck.prepare({DualDeckPlayer::DeckSource::Type::Uvc, missing});
    ok &= check(waitFor([&] { return deck.refreshPreparation().state == DualDeckPlayer::CueStatus::Error; }), "audio activation failure is never ready");
    deck.clearAfterOffAir(); waitFor([&] { return !deck.retirementPending(); });
    compositor.prepareShutdown(); ok &= deck.stopPipelinesForShutdown() && compositor.stop(); deck.releaseResourcesAfterConfirmedShutdown();
    manager.requestShutdown(); waitFor([&] { return manager.shutdownComplete(); });
    gst_pad_remove_probe(masterPad, masterProbe); gst_object_unref(masterPad);
    audioOutput.detachCallbacks();
    ok &= check(audioOutput.stop(), "master output shuts down");
    return ok;
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv); gst_init(&argc, &argv);
    QTemporaryDir directory; if (!directory.isValid()) return 2;
    app.setProperty("vtrpon.settingsFile", directory.filePath("capture.ini"));
    const bool native = app.arguments().contains("--native");
    const bool frames = app.arguments().contains("--frames");
    const bool ok = frames ? frameCapsChecks() : native ? nativeChecks() : (sharedVideoChecks() && audioChecks());
    qInfo() << "Mac capture" << (frames ? "frames" : native ? "native" : "shared") << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
