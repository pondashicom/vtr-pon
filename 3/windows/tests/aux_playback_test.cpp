#include "bgm_playback.h"
#include "async_shutdown_test_utils.h"
#include "dsk_playback.h"
#include "program_gpu_compositor.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <gst/gst.h>
#include <cmath>
#include <iostream>
#include <memory>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

template <typename Predicate>
bool await(Predicate predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(5);
    } while (timer.elapsed() < timeoutMs);
    return false;
}

bool createAvi(const QString &path, bool withAudio)
{
    QString description = QStringLiteral(
        "avimux name=mux ! filesink name=output "
        "videotestsrc num-buffers=15 pattern=ball ! "
        "video/x-raw,width=160,height=90,framerate=30/1 ! jpegenc ! queue ! mux. ");
    if (withAudio) description += QStringLiteral(
        "audiotestsrc num-buffers=25 samplesperbuffer=960 volume=0.3 ! "
        "audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved ! queue ! mux.");
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (error || !pipeline) {
        if (error) { std::cerr << error->message << '\n'; g_error_free(error); }
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    GstElement *output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    const auto encodedPath = path.toUtf8();
    g_object_set(output, "location", encodedPath.constData(), nullptr);
    gst_object_unref(output);
    GstBus *bus = gst_element_get_bus(pipeline);
    const bool started = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    GstMessage *message = started ? gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)) : nullptr;
    const bool complete = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message) gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return complete && QFileInfo(path).size() > 0;
}

void postErrorThenEos(GstElement *pipeline, const char *detail)
{
    GError *error = g_error_new_literal(g_quark_from_static_string("aux-playback-test"), 1, detail);
    gst_element_post_message(pipeline, gst_message_new_error(GST_OBJECT(pipeline), error, nullptr));
    g_error_free(error);
    gst_element_post_message(pipeline, gst_message_new_eos(GST_OBJECT(pipeline)));
}

bool bgmContracts(const QString &av, const QString &videoOnly)
{
    BgmPlayback bgm;
    if (!require(bgm.initialize(nullptr, nullptr), "BGM initializes without a physical output sink")) return false;
    const auto ready = [&] { return await([&] { bgm.pollBus(); return bgm.status() == BgmPlayback::Status::Ready; }); };
    const auto failed = [&] { return await([&] { bgm.pollBus(); return bgm.status() == BgmPlayback::Status::Error; }); };
    bool ok = require(!bgm.load(av, 400, 100) && bgm.cachedStatus() == BgmPlayback::Status::Empty,
                      "invalid BGM In/Out is rejected before changing the empty state");
    (void)bgm.load(videoOnly, 0, -1);
    ok &= require(failed() && !bgm.errorDetail().isEmpty(),
                  "video-only material cannot become a playable BGM item");
    (void)bgm.load(av, 700, -1);
    ok &= require(failed(),
                  "BGM In beyond the real media duration fails during preparation");
    if (!require(bgm.load(av, 50, 350) && ready(), "BGM reload recovers after preparation errors")) return false;
    ok &= require(std::abs(bgm.durationMs() - 500) <= 50,
                  "generated AV material provides a real half-second BGM audio stream");
    ok &= require(bgm.play(), "BGM starts its prepared audio range");
    QString detail;
    BgmPlayback::Event event = BgmPlayback::Event::None;
    const bool ended = await([&] {
        bgm.pollBus();
        event = bgm.takeEvent(detail);
        return event != BgmPlayback::Event::None;
    });
    ok &= require(ended && event == BgmPlayback::Event::EndOfStream,
                  "BGM range playback produces an actual EOS event");
    ok &= require(bgm.takeEvent(detail) == BgmPlayback::Event::None && detail.isEmpty(),
                  "BGM EOS is delivered once");
    if (!require(bgm.load(av, 0, -1) && ready(), "BGM reload clears the preceding EOS state")) return false;
    postErrorThenEos(bgm.pipeline(), "intentional BGM bus error");
    bgm.pollBus();
    ok &= require(bgm.takeEvent(detail) == BgmPlayback::Event::Error
                      && detail == QStringLiteral("intentional BGM bus error")
                      && bgm.cachedStatus() == BgmPlayback::Status::Error,
                  "a later EOS cannot replace the pending BGM error");
    ok &= require(bgm.load(av, 0, -1) && ready()
                      && bgm.takeEvent(detail) == BgmPlayback::Event::None && bgm.errorDetail().isEmpty(),
                  "successful BGM reload clears prior event and error state");
    bgm.detachCallbacks();
    ok &= require(bgm.shutdown(), "BGM confirms producer shutdown");
    return ok;
}

class DskFixture {
public:
    ~DskFixture() { (void)shutdown(); }
    bool initialize()
    {
        video.resize(320, 180);
        monitor.resize(320, 180);
        compositor = std::make_unique<ProgramGpuCompositor>(
            static_cast<std::uintptr_t>(video.winId()), static_cast<std::uintptr_t>(monitor.winId()),
            ProgramGpuCompositor::OutputGeometry{320, 180});
        if (!compositor->start() || !dsk.initialize()) return false;
        dsk.attachCompositor(compositor.get());
        for (int index = 0; index < 2; ++index)
            if (!compositor->configureDskInput(index, dsk.pipeline(index), dsk.appSink(index))) return false;
        return true;
    }
    bool shutdown()
    {
        if (stopped_) return true;
        const bool detached = awaitShutdown([&] { return !compositor || compositor->prepareShutdown(); });
        if (!detached || !awaitShutdown([&] { return !dsk.retirementPending(); })) return false;
        dsk.detachCallbacks();
        const bool producers = dsk.shutdown();
        const bool composed = !compositor || compositor->stop();
        stopped_ = detached && producers && composed;
        return stopped_;
    }
    QWidget video;
    QWidget monitor;
    std::unique_ptr<ProgramGpuCompositor> compositor;
    DskPlayback dsk;
private:
    bool stopped_ = false;
};

bool audioIsIsolated(GstElement *pipeline)
{
    guint flags = 0;
    gboolean muted = FALSE;
    gdouble volume = 1.0;
    GstElement *sink = nullptr;
    g_object_get(pipeline, "flags", &flags, "mute", &muted, "volume", &volume,
                 "audio-sink", &sink, nullptr);
    GstElementFactory *factory = sink ? gst_element_get_factory(sink) : nullptr;
    const bool discarded = factory && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "fakesink") == 0;
    if (sink) gst_object_unref(sink);
    return !(flags & (1U << 1)) && muted && volume == 0.0 && discarded;
}

bool dskContracts(const QString &av)
{
    DskFixture fixture;
    if (!require(fixture.initialize(), "DSK initializes with two video-only producers")) return false;
    auto &dsk = fixture.dsk;
    // Normal CUE must reestablish the audio barriers even after a stray flag
    // change; the discard sink remains in place throughout this test.
    guint flags = 0;
    g_object_get(dsk.pipeline(0), "flags", &flags, nullptr);
    g_object_set(dsk.pipeline(0), "flags", flags | (1U << 1), "mute", FALSE, "volume", 1.0, nullptr);
    if (!require(dsk.cue(av, 100, 400) && await([&] {
            dsk.pollBus(); return dsk.cueStatus() == DskPlayback::DskStatus::Ready;
        }), "AV DSK prepares a real video frame at its In point")) return false;
    bool ok = require(audioIsIsolated(dsk.pipeline(0)) && audioIsIsolated(dsk.pipeline(1)),
                      "DSK CUE restores disabled audio, mute, zero volume, and the discard sink");
    ok &= require(dsk.takeCue(true, false, 1.0F), "DSK starts its prepared video range");
    QString detail;
    DskPlayback::DskEvent event = DskPlayback::DskEvent::None;
    const auto awaitEvent = [&] {
        return await([&] {
            dsk.pollBus(); event = dsk.takeEvent(detail);
            return event != DskPlayback::DskEvent::None;
        });
    };
    ok &= require(awaitEvent() && event == DskPlayback::DskEvent::EndOfStream,
                  "DSK video with an audio track reaches its range EOS without enabling audio");
    ok &= require(audioIsIsolated(dsk.pipeline(0)) && dsk.pauseAtEnd() && dsk.restartFromIn(false),
                  "DSK end hold and paused restart preserve its audio isolation");
    ok &= require(await([&] { return std::abs(dsk.positionMs() - 100) <= 80; }),
                  "DSK paused restart returns to its configured In point");
    ok &= require(dsk.restartFromIn(true) && awaitEvent() && event == DskPlayback::DskEvent::EndOfStream,
                  "DSK restart produces a fresh EOS from the same video range");
    ok &= require(dsk.restartFromIn(false), "DSK can return to a paused frame before error delivery");
    ok &= require(!dsk.isPlaying() && dsk.play() && dsk.isPlaying(),
                  "manual DSK Play resumes a paused frame");
    ok &= require(await([&] { dsk.pollBus(); return dsk.positionMs() >= 200; }),
                  "manual DSK Play advances the real video");
    ok &= require(dsk.pause() && !dsk.isPlaying(), "manual DSK Pause reports held state");
    const qint64 held = dsk.positionMs();
    QThread::msleep(100);
    ok &= require(std::abs(dsk.positionMs() - held) < 80,
                  "manual DSK Pause holds the video position");
    ok &= require(dsk.play() && await([&] { dsk.pollBus(); return !dsk.isPlaying(); }),
                  "manual resume reaches real EOS and clears playing state");
    ok &= require(dsk.restartFromIn(false) && dsk.takeEvent(detail) == DskPlayback::DskEvent::None,
                  "restart retires the latched EOS of the previous iteration");
    ok &= require(audioIsIsolated(dsk.pipeline(0)) && audioIsIsolated(dsk.pipeline(1)),
                  "manual DSK transport never enables source audio");
    postErrorThenEos(dsk.pipeline(0), "intentional DSK bus error");
    dsk.pollBus();
    ok &= require(dsk.takeEvent(detail) == DskPlayback::DskEvent::Error
                      && detail == QStringLiteral("intentional DSK bus error")
                      && dsk.status() == DskPlayback::DskStatus::Error,
                  "a later EOS cannot replace an active DSK error");
    ok &= require(!dsk.play() && !dsk.pause() && !dsk.isPlaying(),
                  "manual transport cannot revive an errored DSK producer");
    ok &= require(fixture.shutdown(), "DSK producer and compositor shutdown are confirmed");
    return ok;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    gst_init(nullptr, nullptr);
    for (const char *name : {"videotestsrc", "jpegenc", "audiotestsrc", "avimux", "filesink", "jpegdec", "avidemux"}) {
        GstElementFactory *factory = gst_element_factory_find(name);
        if (!require(factory != nullptr, name)) return 1;
        gst_object_unref(factory);
    }
    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary fixture directory is available")) return 1;
    const QString av = directory.path() + QStringLiteral("/audio-video.avi");
    const QString videoOnly = directory.path() + QStringLiteral("/video-only.avi");
    if (!require(createAvi(av, true) && createAvi(videoOnly, false), "temporary AV and video-only fixtures are generated")) return 1;
    bool ok = bgmContracts(av, videoOnly);
    ok &= dskContracts(av);
    std::cout << "Auxiliary playback contract: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
