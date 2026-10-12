#include "decklink.h"
#include "deck_source_session.h"
#include "live_audio_source.h"
#include "wasapi_raw_audio_source.h"
#include <mutex>
#include "program_audio_bridge.h"
#include "network_video_source.h"
#ifdef _WIN32
#include "program_gpu_compositor.h"
#include "uvc_source_session_manager.h"
#include "network_video.h"
#include <gst/app/gstappsink.h>
#endif
#include <QDebug>
#include <QUrl>
#include <gst/video/videooverlay.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;
#ifndef _WIN32
constexpr GstClockTime kMonitorMinimumFrameInterval = GST_SECOND / 30;
#endif


bool hasElementFactory(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

bool canUseD3d11VideoPath()
{
#ifdef _WIN32
    return hasElementFactory("d3d11convert")
        && hasElementFactory("d3d11upload")
        && hasElementFactory("videoconvert")
        && hasElementFactory("appsink");
#else
    return false;
#endif
}

void setBooleanPropertyIfAvailable(GstElement *element, const char *name,
                                   gboolean value)
{
    if (element
        && g_object_class_find_property(G_OBJECT_GET_CLASS(element), name)) {
        g_object_set(element, name, value, nullptr);
    }
}

#ifndef _WIN32
GstElement *makeVideoSink(const char *name)
{
    return gst_element_factory_make("autovideosink", name);
}
#endif
} // namespace

struct DeckSourceSession::BusCallbacks {
    std::mutex mutex;
    GstBusSyncHandler callback = nullptr;
    gpointer data = nullptr;
};

GstBusSyncReply DeckSourceSession::handleFileBusSync(
    GstBus *bus, GstMessage *message, gpointer data)
{
    auto &callbacks = **static_cast<std::shared_ptr<BusCallbacks> *>(data);
    std::lock_guard lock(callbacks.mutex);
    return callbacks.callback ? callbacks.callback(bus, message, callbacks.data) : GST_BUS_PASS;
}

DeckSourceSession::~DeckSourceSession()
{
    // The coordinator retains the entire backend after an unconfirmed global
    // shutdown. Standalone callers must follow that same ownership contract.
    if (playbin_) {
        detachCallbacks();
        if (pipelinesStopped_) {
            releaseResourcesAfterConfirmedShutdown();
        } else {
            qWarning() << "Retaining deck resources without confirmed shutdown" << index_;
        }
    }
}

void DeckSourceSession::attachSourceManagers(
    UvcSourceSessionManager *uvc, PowerPointSourceSessionManager *powerPoint,
    PdfSourceSessionManager *pdf, ProgramGpuCompositor *compositor)
{
#ifdef _WIN32
    uvcSourceSessions_ = uvc;
    powerPointSources_ = powerPoint;
    pdfSources_ = pdf;
    programGpuCompositor_ = compositor;
#else
    Q_UNUSED(uvc); Q_UNUSED(powerPoint); Q_UNUSED(pdf); Q_UNUSED(compositor);
#endif
}


bool DeckSourceSession::initialize(int deckIndex, int width, int height,
    GstBusSyncHandler callback, gpointer userData,
    guintptr videoWindow, guintptr monitorWindow)
{
    if (playbin_ || deckIndex < 0 || deckIndex > 1) return false;
    if (!vtrpon::registerProgramAudioBridge()) return false;
    index_ = deckIndex;
    outputWidth_ = width;
    outputHeight_ = height;
    callback_ = callback;
    callbackData_ = userData;
    videoWindowHandle_ = videoWindow;
    monitorWindowHandle_ = monitorWindow;
    if (!hasElementFactory("scaletempo")) {
        qWarning() << "Required playback-rate audio element scaletempo is unavailable";
        return false;
    }
    const QString deckName = index_ == 0
        ? QStringLiteral("deck-a") : QStringLiteral("deck-b");
    const QByteArray playbinName =
        (deckName + QStringLiteral("-player")).toUtf8();
    playbin_ = gst_element_factory_make("playbin",
                                            playbinName.constData());
    if (!playbin_) {
        return false;
    }

    GstElement *videoOutput = makeVideoOutput(deckName);

    GError *audioError = nullptr;
    // PROGRAM consumes timestamped PCM at a fixed 48 kHz.
    // Normalize each playback segment's tempo before crossing that
    // pipeline boundary so non-1x rates neither repeat nor drop samples.
    const QString audioDescription = QStringLiteral(
        "audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,"
        "layout=interleaved ! "
        "scaletempo name=%1-playback-tempo ! "
        "level name=%1-input-audio-level post-messages=true "
        "interval=5000000 ! "
        "volume name=%1-item-audio-volume ! "
        "vtrponaudiosink name=%1-audio-output channel=%1-audio "
        "sync=true qos=false")
        .arg(deckName);
    GstElement *audioOutput = gst_parse_bin_from_description(
        audioDescription.toUtf8().constData(), TRUE, &audioError);
    if (audioError) {
        qWarning() << "Failed to create deck audio output:"
                   << audioError->message;
        g_error_free(audioError);
    }
    if (!videoOutput || !audioOutput) {
        if (videoOutput) {
            gst_object_unref(videoOutput);
        }
        if (audioOutput) {
            gst_object_unref(audioOutput);
        }
        return false;
    }

    const QByteArray inputLevelName =
        (deckName + QStringLiteral("-input-audio-level")).toUtf8();
    const QByteArray itemVolumeName =
        (deckName + QStringLiteral("-item-audio-volume")).toUtf8();
    inputAudioLevel_ = gst_bin_get_by_name(
        GST_BIN(audioOutput), inputLevelName.constData());
    itemAudioVolume_ = gst_bin_get_by_name(
        GST_BIN(audioOutput), itemVolumeName.constData());
    if (!inputAudioLevel_ || !itemAudioVolume_) {
        gst_object_unref(videoOutput);
        gst_object_unref(audioOutput);
        return false;
    }

    g_object_set(playbin_,
                 "video-sink", videoOutput,
                 "audio-sink", audioOutput,
                 nullptr);
    bus_ = gst_element_get_bus(playbin_);
    if (!bus_) {
        return false;
    }
    fileBusCallbacks_ = std::make_shared<BusCallbacks>();
    fileBusCallbacks_->callback = callback_;
    fileBusCallbacks_->data = callbackData_;
    gst_bus_set_sync_handler(bus_, &DeckSourceSession::handleFileBusSync,
        new std::shared_ptr<BusCallbacks>(fileBusCallbacks_), [](gpointer data) {
            delete static_cast<std::shared_ptr<BusCallbacks> *>(data);
        });
    return true;
}

GstElement *DeckSourceSession::makeVideoOutput(const QString &deckName)
{
#ifdef _WIN32
    if (!canUseD3d11VideoPath()) {
        qWarning() << "Required D3D11 appsink elements are unavailable";
        return nullptr;
    }

    const QString inputName = deckName
        + QStringLiteral("-video-input");
    const QString appSinkName = deckName
        + QStringLiteral("-video-appsink");
    const QString description = QStringLiteral(
        "queue name=%1 max-size-buffers=2 max-size-bytes=0 "
        "max-size-time=0 ! videoconvert ! d3d11upload ! "
        "video/x-raw(memory:D3D11Memory) ! "
        "d3d11convert add-borders=true border-color=0 ! "
        "video/x-raw(memory:D3D11Memory),format=RGBA,"
        "width=%3,height=%4,pixel-aspect-ratio=1/1 ! "
        "appsink name=%2 max-buffers=2 leaky-type=downstream "
        "wait-on-eos=false sync=true qos=false "
        "enable-last-sample=false")
        .arg(inputName, appSinkName)
        .arg(outputWidth_)
        .arg(outputHeight_);

    GError *parseError = nullptr;
    GstElement *videoOutput = gst_parse_bin_from_description(
        description.toUtf8().constData(), FALSE, &parseError);
    if (parseError) {
        qWarning() << "Failed to create deck D3D11 appsink output:"
                   << parseError->message;
        g_error_free(parseError);
    }
    if (!videoOutput) {
        return nullptr;
    }

    GstElement *inputQueue = gst_bin_get_by_name(
        GST_BIN(videoOutput), inputName.toUtf8().constData());
    appSink_ = gst_bin_get_by_name(
        GST_BIN(videoOutput), appSinkName.toUtf8().constData());
    const auto releaseTemporary = [&] {
        if (inputQueue) {
            gst_object_unref(inputQueue);
        }
    };
    if (!inputQueue || !appSink_
        || !GST_IS_APP_SINK(appSink_)) {
        releaseTemporary();
        if (appSink_) {
            gst_object_unref(appSink_);
            appSink_ = nullptr;
        }
        gst_object_unref(videoOutput);
        return nullptr;
    }

    GstPad *inputPad = gst_element_get_static_pad(inputQueue, "sink");
    if (!inputPad) {
        releaseTemporary();
        gst_object_unref(appSink_);
        appSink_ = nullptr;
        gst_object_unref(videoOutput);
        return nullptr;
    }
    GstPad *ghostPad = gst_ghost_pad_new("sink", inputPad);
    gst_object_unref(inputPad);
    if (!ghostPad || !gst_element_add_pad(videoOutput, ghostPad)) {
        if (ghostPad) {
            gst_object_unref(ghostPad);
        }
        releaseTemporary();
        gst_object_unref(appSink_);
        appSink_ = nullptr;
        gst_object_unref(videoOutput);
        return nullptr;
    }

    releaseTemporary();
    return videoOutput;
#else
    const QString compositorName = deckName + QStringLiteral("-video-compositor");
    const QString inputName = deckName + QStringLiteral("-video-input");
    const QString outputName = deckName + QStringLiteral("-video-output-queue");
    const QString monitorName = deckName + QStringLiteral("-monitor-output-queue");
    const QString description = QStringLiteral(
        "compositor name=%1 background=black ! tee name=%2-video-tee "
        "queue name=%3 max-size-buffers=1 max-size-bytes=0 "
        "max-size-time=0 ! %1.sink_0 "
        "%2-video-tee. ! queue name=%4 "
        "max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
        "%2-video-tee. ! queue name=%5 leaky=downstream "
        "max-size-buffers=1 max-size-bytes=0 max-size-time=0")
        .arg(compositorName, deckName, inputName, outputName,
             monitorName);

    GError *parseError = nullptr;
    GstElement *videoOutput = gst_parse_bin_from_description(
        description.toUtf8().constData(), FALSE, &parseError);
    if (parseError) {
        qWarning() << "Failed to create native deck video output:"
                   << parseError->message;
        g_error_free(parseError);
    }
    if (!videoOutput) {
        return nullptr;
    }

    GstElement *compositor = gst_bin_get_by_name(
        GST_BIN(videoOutput), compositorName.toUtf8().constData());
    GstElement *inputQueue = gst_bin_get_by_name(
        GST_BIN(videoOutput), inputName.toUtf8().constData());
    GstElement *videoQueue = gst_bin_get_by_name(
        GST_BIN(videoOutput), outputName.toUtf8().constData());
    GstElement *monitorQueue = gst_bin_get_by_name(
        GST_BIN(videoOutput), monitorName.toUtf8().constData());
    const auto releaseTemporary = [&] {
        for (GstElement *element : {compositor, inputQueue,
                                    videoQueue, monitorQueue}) {
            if (element) {
                gst_object_unref(element);
            }
        }
    };
    if (!compositor || !inputQueue || !videoQueue || !monitorQueue) {
        releaseTemporary();
        gst_object_unref(videoOutput);
        return nullptr;
    }

    GstPad *monitorOutputPad = gst_element_get_static_pad(
        monitorQueue, "src");
    const gulong monitorProbeId = monitorOutputPad
        ? gst_pad_add_probe(
            monitorOutputPad,
            static_cast<GstPadProbeType>(
                GST_PAD_PROBE_TYPE_BUFFER
                | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
            &limitMonitorCadence, &monitorCadence_, nullptr)
        : 0;
    if (monitorProbeId == 0) {
        if (monitorOutputPad) {
            gst_object_unref(monitorOutputPad);
        }
        releaseTemporary();
        gst_object_unref(videoOutput);
        return nullptr;
    }
    const auto releaseMonitorProbe = [&] {
        if (!monitorOutputPad) {
            return;
        }
        gst_pad_remove_probe(monitorOutputPad, monitorProbeId);
        gst_object_unref(monitorOutputPad);
        monitorOutputPad = nullptr;
    };

    GstPad *videoPad = gst_element_get_static_pad(compositor, "sink_0");
    GstPad *inputPad = gst_element_get_static_pad(inputQueue, "sink");
    if (!videoPad || !inputPad) {
        if (videoPad) {
            gst_object_unref(videoPad);
        }
        if (inputPad) {
            gst_object_unref(inputPad);
        }
        releaseMonitorProbe();
        releaseTemporary();
        gst_object_unref(videoOutput);
        return nullptr;
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(videoPad),
                                     "sizing-policy")) {
        g_object_set(videoPad, "sizing-policy", 1, nullptr);
    }
    gst_object_unref(videoPad);

    GstPad *ghostPad = gst_ghost_pad_new("sink", inputPad);
    gst_object_unref(inputPad);
    if (!ghostPad || !gst_element_add_pad(videoOutput, ghostPad)) {
        if (ghostPad) {
            gst_object_unref(ghostPad);
        }
        releaseMonitorProbe();
        releaseTemporary();
        gst_object_unref(videoOutput);
        return nullptr;
    }

    const QByteArray videoSinkName =
        (deckName + QStringLiteral("-video-output-sink")).toUtf8();
    const QByteArray monitorSinkName =
        (deckName + QStringLiteral("-monitor-output-sink")).toUtf8();
    videoSink_ = makeVideoSink(videoSinkName.constData());
    monitorSink_ = makeVideoSink(monitorSinkName.constData());
    if (!videoSink_ || !monitorSink_) {
        if (videoSink_) {
            gst_object_unref(videoSink_);
            videoSink_ = nullptr;
        }
        if (monitorSink_) {
            gst_object_unref(monitorSink_);
            monitorSink_ = nullptr;
        }
        releaseMonitorProbe();
        releaseTemporary();
        gst_object_unref(videoOutput);
        return nullptr;
    }

    setBooleanPropertyIfAvailable(videoSink_,
                                  "force-aspect-ratio", TRUE);
    setBooleanPropertyIfAvailable(videoSink_,
                                  "enable-last-sample", FALSE);
    setBooleanPropertyIfAvailable(videoSink_, "sync", TRUE);
    setBooleanPropertyIfAvailable(monitorSink_,
                                  "force-aspect-ratio", TRUE);
    setBooleanPropertyIfAvailable(monitorSink_,
                                  "enable-last-sample", FALSE);
    setBooleanPropertyIfAvailable(monitorSink_, "sync", FALSE);
    setBooleanPropertyIfAvailable(monitorSink_, "qos", FALSE);
    gst_bin_add_many(GST_BIN(videoOutput), videoSink_,
                     monitorSink_, nullptr);
    if (!gst_element_link(videoQueue, videoSink_)
        || !gst_element_link(monitorQueue, monitorSink_)) {
        releaseMonitorProbe();
        releaseTemporary();
        gst_object_unref(videoOutput);
        return nullptr;
    }

    applyWindowHandle(GST_OBJECT(videoSink_),
                      videoWindowHandle_);
    applyWindowHandle(GST_OBJECT(monitorSink_),
                      monitorWindowHandle_);
    monitorCadencePad_ = monitorOutputPad;
    monitorCadenceProbeId_ = monitorProbeId;
    monitorOutputPad = nullptr;
    releaseTemporary();
    return videoOutput;
#endif
}

#ifndef _WIN32
DeckSourceSession::MonitorCadenceLimiter::MonitorCadenceLimiter()
{
    gst_segment_init(&segment, GST_FORMAT_TIME);
}
void DeckSourceSession::MonitorCadenceLimiter::reset(bool resetSegment)
{
    if (resetSegment) gst_segment_init(&segment, GST_FORMAT_TIME);
    lastKeptRunningTime = GST_CLOCK_TIME_NONE;
}
GstPadProbeReturn DeckSourceSession::limitMonitorCadence(GstPad *, GstPadProbeInfo *info,
                                      gpointer userData)
{
    auto *limiter = static_cast<MonitorCadenceLimiter *>(userData);
    const GstPadProbeType type = GST_PAD_PROBE_INFO_TYPE(info);

    if ((type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (!event) {
            return GST_PAD_PROBE_OK;
        }
        switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_SEGMENT: {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment && segment->format == GST_FORMAT_TIME) {
                limiter->segment = *segment;
                limiter->reset();
            } else {
                limiter->reset(true);
            }
            break;
        }
        case GST_EVENT_STREAM_START:
        case GST_EVENT_FLUSH_STOP:
        case GST_EVENT_EOS:
            limiter->reset(true);
            break;
        default:
            break;
        }
        return GST_PAD_PROBE_OK;
    }

    if ((type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) {
        return GST_PAD_PROBE_OK;
    }
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
        limiter->reset();
    }

    const GstClockTime timestamp = GST_CLOCK_TIME_IS_VALID(
        GST_BUFFER_PTS(buffer)) ? GST_BUFFER_PTS(buffer)
                                : GST_BUFFER_DTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(timestamp)) {
        return GST_PAD_PROBE_OK;
    }
    const GstClockTime runningTime = gst_segment_to_running_time(
        &limiter->segment, GST_FORMAT_TIME, timestamp);
    if (!GST_CLOCK_TIME_IS_VALID(runningTime)) {
        return GST_PAD_PROBE_OK;
    }

    if (GST_CLOCK_TIME_IS_VALID(limiter->lastKeptRunningTime)
        && runningTime >= limiter->lastKeptRunningTime
        && runningTime - limiter->lastKeptRunningTime
            < kMonitorMinimumFrameInterval) {
        return GST_PAD_PROBE_DROP;
    }
    limiter->lastKeptRunningTime = runningTime;
    return GST_PAD_PROBE_OK;
}

#endif

bool DeckSourceSession::stopPipeline(GstElement *pipeline, const char *description)
{
    if (!pipeline) {
        return true;
    }
    if (gst_element_set_state(pipeline, GST_STATE_NULL)
        == GST_STATE_CHANGE_FAILURE) {
        qWarning() << description << "rejected GST_STATE_NULL";
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn result = gst_element_get_state(
        pipeline, &current, &pending, kShutdownStateTimeout);
    if (result == GST_STATE_CHANGE_FAILURE || current != GST_STATE_NULL) {
        qWarning() << description << "did not reach GST_STATE_NULL; current="
                   << gst_element_state_get_name(current) << "pending="
                   << gst_element_state_get_name(pending);
        return false;
    }
    return true;
}

void DeckSourceSession::drainBus(GstBus *bus)
{
    if (!bus) {
        return;
    }
    while (GstMessage *message = gst_bus_pop(bus)) {
        gst_message_unref(message);
    }
}

void DeckSourceSession::applyWindowHandle(GstObject *object, guintptr windowHandle)
{
    if (object && windowHandle != 0 && GST_IS_VIDEO_OVERLAY(object)) {
        gst_video_overlay_set_window_handle(
            GST_VIDEO_OVERLAY(object), windowHandle);
    }
}

void DeckSourceSession::releaseResourcesAfterConfirmedShutdown()
{
    if (!pipelinesStopped_) return;
#ifdef _WIN32
    (void)releaseUvcAudio();
#endif
#ifndef _WIN32
    if (monitorCadencePad_) {
        if (monitorCadenceProbeId_ != 0) {
            gst_pad_remove_probe(monitorCadencePad_,
                                 monitorCadenceProbeId_);
            monitorCadenceProbeId_ = 0;
        }
        gst_object_unref(monitorCadencePad_);
        monitorCadencePad_ = nullptr;
    }
#endif
    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
    if (inputAudioLevel_) {
        gst_object_unref(inputAudioLevel_);
        inputAudioLevel_ = nullptr;
    }
    if (itemAudioVolume_) {
        gst_object_unref(itemAudioVolume_);
        itemAudioVolume_ = nullptr;
    }
    if (playbin_) {
        gst_object_unref(playbin_);
        playbin_ = nullptr;
    }
#ifdef _WIN32
    if (appSink_) {
        gst_object_unref(appSink_);
        appSink_ = nullptr;
    }
#else
    videoSink_ = nullptr;
    monitorSink_ = nullptr;
#endif
}

void DeckSourceSession::detachCallbacks()
{
    if (fileBusCallbacks_) {
        std::lock_guard lock(fileBusCallbacks_->mutex);
        fileBusCallbacks_->callback = nullptr;
        fileBusCallbacks_->data = nullptr;
    }
    if (bus_) gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
#ifdef _WIN32
    if (uvcAudioBus_) gst_bus_set_sync_handler(uvcAudioBus_, nullptr, nullptr, nullptr);
#else
    if (monitorCadencePad_) {
        if (monitorCadenceProbeId_) {
            gst_pad_remove_probe(monitorCadencePad_, monitorCadenceProbeId_);
            monitorCadenceProbeId_ = 0;
        }
        gst_object_unref(monitorCadencePad_);
        monitorCadencePad_ = nullptr;
    }
#endif
}

bool DeckSourceSession::stopPipelinesForShutdown()
{
    fileRequest_.reset();
    if (auto wait = AsyncFilePreparation::retire(filePreparation_)) wait();
    bool confirmed = stopPipeline(playbin_, "dual-deck playbin");
#ifdef _WIN32
    pendingSource_.reset();
    pendingAudioClock_.reset();
    confirmed = (!uvcAudioRetirement_ || uvcAudioRetirement_->succeeded()) && confirmed;
    confirmed = stopPipeline(uvcAudioPipeline_, "UVC mapped audio pipeline") && confirmed;
#endif
    pipelinesStopped_ = confirmed;
    return confirmed;
}

bool DeckSourceSession::retirementPending() const
{
#ifdef _WIN32
    return uvcAudioRetirement_ && !uvcAudioRetirement_->done();
#else
    return false;
#endif
}

bool DeckSourceSession::fileStopConfirmed() const
{
    if (!playbin_) return true;
    if (filePreparationPending()) return false;
    GstState state = GST_STATE_VOID_PENDING;
    return gst_element_get_state(playbin_, &state, nullptr, 0) != GST_STATE_CHANGE_FAILURE
        && state == GST_STATE_NULL;
}

void DeckSourceSession::setNativeWindowHandles(guintptr videoWindow, guintptr monitorWindow)
{
    videoWindowHandle_ = videoWindow;
    monitorWindowHandle_ = monitorWindow;
#ifndef _WIN32
    applyWindowHandle(GST_OBJECT(videoSink_), videoWindowHandle_);
    applyWindowHandle(GST_OBJECT(monitorSink_), monitorWindowHandle_);
#endif
}

GstElement *DeckSourceSession::appSink() const
{
#ifdef _WIN32
    return appSink_;
#else
    return nullptr;
#endif
}
GstElement *DeckSourceSession::videoSink() const
{
#ifndef _WIN32
    return videoSink_;
#else
    return nullptr;
#endif
}
GstElement *DeckSourceSession::monitorSink() const
{
#ifndef _WIN32
    return monitorSink_;
#else
    return nullptr;
#endif
}
GstElement *DeckSourceSession::inputAudioLevel() const
{
#ifdef _WIN32
    if (sourceType_ == Type::Uvc) return uvcInputAudioLevel_;
#endif
    return sourceType_ == Type::File ? inputAudioLevel_ : nullptr;
}
GstElement *DeckSourceSession::audioPipeline() const
{
#ifdef _WIN32
    if (sourceType_ == Type::Uvc) return uvcAudioPipeline_;
#endif
    return playbin_;
}
GstBus *DeckSourceSession::mappedAudioBus() const
{
#ifdef _WIN32
    return uvcAudioBus_;
#else
    return nullptr;
#endif
}


#ifdef _WIN32
bool DeckSourceSession::releaseUvcAudio()
{
    if (!uvcAudioPipeline_) return true;
    // Detach callbacks and the PCM route before handing all references to the
    // worker. No UI poller can observe or reuse the retiring audio pipeline.
    if (uvcAudioBus_) gst_bus_set_sync_handler(uvcAudioBus_, nullptr, nullptr, nullptr);
    const auto sinkName = QStringLiteral("deck-%1-uvc-audio-output").arg(index_ == 0 ? "a" : "b").toUtf8();
    auto *sink = gst_bin_get_by_name(GST_BIN(uvcAudioPipeline_), sinkName.constData());
    vtrpon::disconnectProgramAudioProducer(sink);
    if (sink) gst_object_unref(sink);
    auto *pipeline = uvcAudioPipeline_;
    auto *bus = uvcAudioBus_;
    auto *volume = uvcItemAudioVolume_;
    auto *level = uvcInputAudioLevel_;
    auto *source = uvcAudioSource_;
    auto receiver = std::move(networkAudioReceiver_);
    auto deckLinkReceiver = std::move(deckLinkAudioReceiver_);
    uvcAudioPipeline_ = nullptr;
    uvcAudioBus_ = nullptr;
    uvcItemAudioVolume_ = nullptr;
    uvcInputAudioLevel_ = nullptr;
    uvcAudioSource_ = nullptr;
    uvcAudioRetirement_ = AsyncPipelineStop::start(pipeline, [pipeline, bus, volume, level, source, receiver = std::move(receiver)]() mutable {
        receiver.reset();
        if (bus) gst_object_unref(bus);
        if (volume) gst_object_unref(volume);
        if (level) gst_object_unref(level);
        if (source) gst_object_unref(source);
        gst_object_unref(pipeline);
    });
    uvcAudioDeviceId_.clear();
    return !uvcAudioRetirement_->done() || uvcAudioRetirement_->succeeded();
}

bool DeckSourceSession::startUvcAudio(const QString &audioDeviceId,
    double itemVolume, GstClock *audioClock, QString *errorDetail)
{
    if (errorDetail) {
        errorDetail->clear();
    }
    if (!releaseUvcAudio()) {
        if (errorDetail) {
            *errorDetail = QStringLiteral(
                "Could not stop the previous mapped audio input.");
        }
        return false;
    }
    if (audioDeviceId.isEmpty()) {
        return true;
    }
    const bool network = (vtrpon::isNetworkVideoSource(audioDeviceId) || vtrpon::isDeckLinkSource(audioDeviceId));
    if (!network && !vtrpon::registerWasapiRawAudioSource()) {
        if (errorDetail) {
            *errorDetail = QStringLiteral(
                "The shared RAW audio input is unavailable.");
        }
        return false;
    }

    const QString deckName = index_ == 0
        ? QStringLiteral("deck-a") : QStringLiteral("deck-b");
    const QString input = network
        ? QStringLiteral("appsrc name=%1-uvc-audio-source is-live=true format=time do-timestamp=true block=false max-buffers=0 max-bytes=0 max-time=200000000 leaky-type=downstream ! ")
        : QStringLiteral("vtrponwasapirawsrc name=%1-uvc-audio-source ! ");
    const QString description = (input + QStringLiteral(
        "queue max-size-buffers=0 max-size-bytes=0 "
        "max-size-time=200000000 ! "
        "audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,"
        "layout=interleaved ! "
        "level name=%1-uvc-input-audio-level post-messages=true "
        "interval=5000000 ! "
        "volume name=%1-uvc-item-audio-volume ! "
        "vtrponaudiosink name=%1-uvc-audio-output channel=%1-audio "
        "sync=true qos=false"))
            .arg(deckName);
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(
        description.toUtf8().constData(), &parseError);
    if (parseError) {
        if (errorDetail) {
            *errorDetail = QString::fromUtf8(parseError->message);
        }
        qWarning() << "Failed to create mapped UVC audio pipeline:"
                   << parseError->message;
        g_error_free(parseError);
    }
    if (!pipeline || !GST_IS_PIPELINE(pipeline)) {
        if (pipeline) {
            gst_object_unref(pipeline);
        }
        if (errorDetail && errorDetail->isEmpty()) {
            *errorDetail = QStringLiteral(
                "Could not create the mapped UVC audio pipeline.");
        }
        return false;
    }

    uvcAudioPipeline_ = pipeline;
    uvcAudioSource_ = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (deckName + QStringLiteral("-uvc-audio-source"))
            .toUtf8().constData());
    uvcInputAudioLevel_ = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (deckName + QStringLiteral("-uvc-input-audio-level"))
            .toUtf8().constData());
    uvcItemAudioVolume_ = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (deckName + QStringLiteral("-uvc-item-audio-volume"))
            .toUtf8().constData());
    uvcAudioBus_ = gst_element_get_bus(pipeline);
    if (!uvcAudioSource_ || !uvcInputAudioLevel_
        || !uvcItemAudioVolume_ || !uvcAudioBus_) {
        if (errorDetail && errorDetail->isEmpty()) {
            *errorDetail = QStringLiteral(
                "Could not create the mapped UVC audio elements.");
        }
        (void)releaseUvcAudio();
        return false;
    }

    const QByteArray encodedDeviceId = audioDeviceId.toUtf8();
    if (!network) g_object_set(uvcAudioSource_, "device", encodedDeviceId.constData(), nullptr);
    g_object_set(uvcItemAudioVolume_, "volume", itemVolume, nullptr);
    if (audioClock) {
        gst_pipeline_use_clock(GST_PIPELINE(pipeline), audioClock);
    }
    gst_bus_set_sync_handler(uvcAudioBus_, callback_,
                             callbackData_, nullptr);
    uvcAudioDeviceId_ = audioDeviceId;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        if (errorDetail && errorDetail->isEmpty()) {
            *errorDetail = QStringLiteral(
                "Could not start the mapped audio input.");
        }
        (void)releaseUvcAudio();
        return false;
    }
    if (network && vtrpon::isDeckLinkSource(audioDeviceId)) deckLinkAudioReceiver_ = std::make_shared<vtrpon::DeckLinkReceiver>(audioDeviceId, uvcAudioSource_, true);
    else if (network) networkAudioReceiver_ = std::make_shared<vtrpon::NetworkVideoReceiver>(
        audioDeviceId, uvcAudioSource_, true);
    return true;
}

bool DeckSourceSession::releasePdfAfterDetach(const QString &operation, bool nonBlocking)
{
    Q_UNUSED(nonBlocking);
    const bool released = pdfSources_->releaseProgramDeck(index_);
    if (!released) qWarning() << operation << "could not release PDF ownership for deck" << index_;
    // The worker retains the detached input; this does not confirm physical stop.
    return released;
}
#endif

bool DeckSourceSession::prepareFile(const QString &filePath, qint64 inPointMs,
             qint64 outPointMs, double playbackRate)
{
    if (filePath.isEmpty() || inPointMs < 0 || !std::isfinite(playbackRate)
        || playbackRate <= 0.0
        || (outPointMs >= 0 && outPointMs <= inPointMs)) {
        return false;
    }

    if (!beginSource(Type::File)) {
        return false;
    }
    drainBus(bus_);
#ifdef _WIN32
    programGpuCompositor_->clearDeckFrame(index_);
#endif


    g_object_set(itemAudioVolume_, "volume", 1.0, nullptr);
    hasVideo_ = false;
    hasAudio_ = false;
    inPointMs_ = inPointMs;
    outPointMs_ = outPointMs;
    playbackRate_ = playbackRate;

    preparationState_ = PreparationState::Loading;
    preparationError_.clear();
    rangeSeekIssued_ = false;
    preparationDeadline_.setRemainingTime(10000);

    mediaOffline_ = false;
    // Start on the next readiness poll. Rapid selection changes replace this
    // value before opening any file or allocating another retired pipeline.
    fileRequest_ = AsyncFilePreparation::Request{filePath, inPointMs, outPointMs, playbackRate};
    return true;
}

bool DeckSourceSession::prepareUvc(const QString &devicePath, const QString &mappedAudioDeviceId,
    double itemVolume, GstClock *audioClock)
{
    const bool audioOnly = vtrpon::isLiveAudioSource(devicePath);
    const QString audioDeviceId = audioOnly ? vtrpon::liveAudioDeviceId(devicePath)
        : (vtrpon::isNetworkVideoSource(devicePath) || vtrpon::isDeckLinkSource(devicePath)) ? devicePath : mappedAudioDeviceId;
#ifdef _WIN32
    if (devicePath.isEmpty() || (audioOnly && audioDeviceId.isEmpty())) {
        preparationError_ = QStringLiteral(
            "UVC device identification is missing.");
        return false;
    }
    if (!beginSource(Type::Uvc)) {
        return false;
    }
    programGpuCompositor_->clearDeckFrame(index_);
    if (audioOnly) {
        QString detail;
        if (!startUvcAudio(audioDeviceId, itemVolume, audioClock, &detail)) {
            preparationState_ = PreparationState::Error;
            preparationError_ = detail;
            return false;
        }
        uvcDevicePath_ = devicePath;
        uvcProgramPaused_ = false;
        hasVideo_ = false;
        hasAudio_ = true;
        inPointMs_ = 0;
        outPointMs_ = -1;
        preparationState_ = PreparationState::Loading;
        preparationError_.clear();
        preparationDeadline_.setRemainingTime(10000);
        return true;
    }
    UvcSourceSessionManager::ProgramSource source;
    QString detail;
    if (!uvcSourceSessions_->bindProgramDeck(
            index_, devicePath, source, &detail)) {
        preparationState_ = PreparationState::Error;
        // An empty detail is the manager's pending-retirement result. Preserve
        // it so prepare() queues this request instead of reporting a CUE error.
        preparationError_ = detail;
        return false;
    }
    if (!programGpuCompositor_->configureDeckInput(
            index_, source.pipeline, source.appSink)) {
        uvcSourceSessions_->releaseProgramDeck(index_);
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral(
            "Could not connect the UVC feed to the standby player.");
        return false;
    }
    if (!audioDeviceId.isEmpty()
        && !startUvcAudio(audioDeviceId, itemVolume, audioClock, &detail)) {
        (void)programGpuCompositor_->configureDeckInput(
            index_, playbin_, appSink_);
        uvcSourceSessions_->releaseProgramDeck(index_);
        programGpuCompositor_->clearDeckFrame(index_);
        preparationState_ = PreparationState::Error;
        preparationError_ = detail.isEmpty()
            ? QStringLiteral("Could not start the mapped audio input.")
            : detail;
        return false;
    }
    programGpuCompositor_->setDeckFrameUpdatesEnabled(index_, true);
    uvcDevicePath_ = devicePath;
    uvcErrorGeneration_ = 0;
    uvcProgramPaused_ = false;
    hasVideo_ = false;
    hasAudio_ = !audioDeviceId.isEmpty();
    inPointMs_ = 0;
    outPointMs_ = -1;
    preparationState_ = PreparationState::Loading;
    preparationError_.clear();
    rangeSeekIssued_ = false;
    return true;
#else
    Q_UNUSED(devicePath); Q_UNUSED(audioDeviceId); Q_UNUSED(itemVolume); Q_UNUSED(audioClock);
    preparationError_ = QStringLiteral(
        "UVC input is available only on Windows.");
    return false;
#endif
}

bool DeckSourceSession::preparePowerPoint(const QString &filePath)
{
#ifdef _WIN32
    if (filePath.isEmpty() || !powerPointSources_) {
        preparationError_ = QStringLiteral(
            "PowerPoint playback is unavailable.");
        return false;
    }
    if (!beginSource(Type::PowerPoint)) {
        return false;
    }
    programGpuCompositor_->clearDeckFrame(index_);
    PowerPointSourceSessionManager::ProgramSource source;
    QString detail;
    if (!powerPointSources_->bindProgramDeck(
            index_, filePath, source, &detail)) {
        preparationState_ = PreparationState::Error;
        preparationError_ = detail.isEmpty()
            ? QStringLiteral("Could not start the PowerPoint slide show.")
            : detail;
        return false;
    }
    nativeInputAttached_ = false;
    powerPointFilePath_ = filePath;
    powerPointErrorGeneration_ = 0;
    hasVideo_ = false;
    hasAudio_ = false;
    inPointMs_ = 0;
    outPointMs_ = -1;
    preparationState_ = PreparationState::Loading;
    preparationError_.clear();
    rangeSeekIssued_ = false;
    return true;
#else
    Q_UNUSED(filePath);
    preparationError_ = QStringLiteral(
        "PowerPoint playback is available only on Windows.");
    return false;
#endif
}

bool DeckSourceSession::preparePdf(const QString &filePath)
{
#ifdef _WIN32
    if (filePath.isEmpty() || !pdfSources_) {
        preparationError_ = QStringLiteral("PDF playback is unavailable.");
        return false;
    }
    if (!beginSource(Type::Pdf)) {
        return false;
    }
    programGpuCompositor_->clearDeckFrame(index_);
    PdfSourceSessionManager::ProgramSource source;
    QString detail;
    if (!pdfSources_->bindProgramDeck(
            index_, filePath, source, &detail)) {
        preparationState_ = PreparationState::Error;
        preparationError_ = detail.isEmpty()
            ? QStringLiteral("Could not start the PDF source.")
            : detail;
        return false;
    }
    nativeInputAttached_ = false;
    pdfFilePath_ = filePath;
    pdfErrorGeneration_ = 0;
    hasVideo_ = false;
    hasAudio_ = false;
    inPointMs_ = 0;
    outPointMs_ = -1;
    preparationState_ = PreparationState::Loading;
    preparationError_.clear();
    rangeSeekIssued_ = false;
    return true;
#else
    Q_UNUSED(filePath);
    preparationError_ = QStringLiteral(
        "PDF playback is available only on Windows.");
    return false;
#endif
}

DeckSourceSession::PreparationResult DeckSourceSession::prepare(
    const DualDeckPlayer::DeckSource &source, double itemVolume, GstClock *audioClock)
{
    if (!playbin_) return {PreparationState::Error, QStringLiteral("No standby player is available.")};
#ifdef _WIN32
    const auto defer = [&] {
        pendingSource_ = source;
        pendingItemVolume_ = itemVolume;
        pendingAudioClock_.reset();
        if (audioClock) pendingAudioClock_ = std::shared_ptr<GstClock>(
            GST_CLOCK(gst_object_ref(audioClock)), [](GstClock *clock) { gst_object_unref(clock); });
        preparationState_ = PreparationState::Loading;
        preparationError_.clear();
        return PreparationResult{preparationState_, {}};
    };
    if (uvcAudioPipeline_ || uvcAudioRetirement_) {
        if (!clearForReplacement().succeeded)
            return {PreparationState::Error, QStringLiteral("Could not release the previous standby input.")};
        if (uvcAudioRetirement_ && !uvcAudioRetirement_->done()) {
            return defer();
        }
        if (uvcAudioRetirement_ && !uvcAudioRetirement_->succeeded()) {
            preparationState_ = PreparationState::Error;
            preparationError_ = QStringLiteral("The previous mapped UVC audio could not be stopped. The deck cannot be reused.");
            return {preparationState_, preparationError_};
        }
        uvcAudioRetirement_.reset();
    }
#endif
    pipelinesStopped_ = false;
    fileStateChangeFailed_ = false;
    bool succeeded = false;
    switch (source.type) {
    case DualDeckPlayer::DeckSource::Type::File:
        succeeded = prepareFile(source.locator, source.inPointMs, source.outPointMs,
                                source.playbackRate); break;
    case DualDeckPlayer::DeckSource::Type::Uvc:
        succeeded = prepareUvc(source.locator, source.audioDeviceId, itemVolume, audioClock); break;
    case DualDeckPlayer::DeckSource::Type::PowerPoint:
        succeeded = preparePowerPoint(source.locator); break;
    case DualDeckPlayer::DeckSource::Type::Pdf:
        succeeded = preparePdf(source.locator); break;
    }
#ifdef _WIN32
    if (!succeeded && source.type == DualDeckPlayer::DeckSource::Type::Uvc
        && preparationError_.isEmpty()) return defer();
#endif
    if (!succeeded) preparationState_ = PreparationState::Error;
    return {preparationState_, preparationError_, fileStateChangeFailed_};
}


bool DeckSourceSession::beginSource(Type sourceType)
{
    if (!clearForReplacement().succeeded) {
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral("Could not stop the previous input on the standby player.");
        return false;
    }
#ifdef _WIN32
    if (sourceType == Type::Uvc && !uvcSourceSessions_) {
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral(
            "The shared UVC session is unavailable.");
        return false;
    }
    if (sourceType == Type::PowerPoint
        && !powerPointSources_) {
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral(
            "The PowerPoint source manager is unavailable.");
        return false;
    }
    if (sourceType == Type::Pdf && !pdfSources_) {
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral(
            "The PDF source manager is unavailable.");
        return false;
    }
#else
    if (sourceType != Type::File) {
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral(
            "This input type is unavailable on the current platform.");
        return false;
    }
#endif
    sourceType_ = sourceType;
    return true;
}

bool DeckSourceSession::replaceFilePipeline()
{
#ifdef _WIN32
    if (sourceType_ != Type::File || !programGpuCompositor_) return false;
    if (fileRequest_ && !filePreparation_) {
        fileRequest_.reset();
        sourceType_ = Type::None;
        hasVideo_ = hasAudio_ = false;
        inPointMs_ = 0;
        outPointMs_ = -1;
        preparationState_ = PreparationState::Empty;
        preparationError_.clear();
        return true;
    }
    std::erase_if(retiredFiles_, [](const auto &job) { return job->succeeded(); });
    // Bound retained driver resources if several independent stops never finish.
    if (retiredFiles_.size() >= 4) return false;
    auto retired = std::make_shared<DeckSourceSession>();
    if (!retired->initialize(index_, outputWidth_, outputHeight_, callback_, callbackData_)) {
        retired->detachCallbacks();
        if (retired->stopPipelinesForShutdown()) retired->releaseResourcesAfterConfirmedShutdown();
        return false;
    }
    // Drain old GPU callbacks before their storage/graph can be retired. A failed
    // detach leaves the original graph owned and prevents an unsafe replacement.
    if (!programGpuCompositor_->configureDeckInput(index_, retired->pipeline(), retired->appSink())) {
        retired->detachCallbacks();
        if (retired->stopPipelinesForShutdown()) retired->releaseResourcesAfterConfirmedShutdown();
        return false;
    }
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(retired->pipeline()), clock);
    gst_object_unref(clock);
    detachCallbacks();
    const char *sinkName = index_ == 0 ? "deck-a-audio-output" : "deck-b-audio-output";
    GstElement *audioBin = nullptr;
    g_object_get(playbin_, "audio-sink", &audioBin, nullptr);
    if (audioBin) {
        auto *sink = gst_bin_get_by_name(GST_BIN(audioBin), sinkName);
        vtrpon::disconnectProgramAudioProducer(sink);
        if (sink) gst_object_unref(sink);
        gst_object_unref(audioBin);
    }
    std::swap(playbin_, retired->playbin_);
    std::swap(bus_, retired->bus_);
    std::swap(fileBusCallbacks_, retired->fileBusCallbacks_);
    std::swap(inputAudioLevel_, retired->inputAudioLevel_);
    std::swap(itemAudioVolume_, retired->itemAudioVolume_);
    std::swap(appSink_, retired->appSink_);
    std::swap(filePreparation_, retired->filePreparation_);
    // The worker captures only the abandoned graph. It cannot call back into
    // this deck, the player, or the compositor, even after application shutdown.
    auto stopJob = AsyncPipelineStop::start(retired->pipeline(), [retired] {
        retired->pipelinesStopped_ = true;
    }, AsyncFilePreparation::retire(retired->filePreparation_));
    const bool stopStarted = !stopJob->done() || stopJob->succeeded();
    retiredFiles_.push_back(std::move(stopJob));
    sourceType_ = Type::None;
    hasVideo_ = hasAudio_ = false;
    inPointMs_ = 0;
    outPointMs_ = -1;
    playbackRate_ = 1.0;
    preparationState_ = PreparationState::Empty;
    preparationError_.clear();
    preparationDeadline_ = QDeadlineTimer(QDeadlineTimer::Forever);
    return stopStarted;
#else
    // Non-Windows builds have native video sinks bound to the old graph.
    return clearForReplacement().succeeded;
#endif
}

DeckSourceSession::ReleaseResult DeckSourceSession::clearForReplacement()
{
#ifdef _WIN32
    pendingSource_.reset();
    pendingAudioClock_.reset();
#endif
    ReleaseResult result;
#ifdef _WIN32
    if (sourceType_ == Type::File) {
        result.succeeded = replaceFilePipeline();
        result.detached = result.ownershipReleased = result.succeeded;
        return result;
    }
#endif
    if (filePreparationPending()) return result;
    filePreparation_.reset();
    if (!playbin_ || gst_element_set_state(playbin_, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE)
        return result;
    result.stopConfirmed = fileStopConfirmed();
#ifdef _WIN32
    if (sourceType_ == Type::Uvc || sourceType_ == Type::PowerPoint || sourceType_ == Type::Pdf) {
        result.stopConfirmed = false;
        if (!programGpuCompositor_
            || (sourceType_ == Type::Uvc && !uvcSourceSessions_)
            || (sourceType_ == Type::PowerPoint && !powerPointSources_)
            || (sourceType_ == Type::Pdf && !pdfSources_)
            || !programGpuCompositor_->configureDeckInput(index_, playbin_, appSink_))
            return result;
        result.detached = true;
        if (sourceType_ == Type::Uvc) {
            if (!vtrpon::isLiveAudioSource(uvcDevicePath_))
                uvcSourceSessions_->releaseProgramDeck(index_);
        } else if (sourceType_ == Type::PowerPoint) {
            const bool stopped = powerPointSources_->releaseProgramDeck(index_);
            result.stopConfirmed = stopped && fileStopConfirmed();
            if (!stopped) {
                // PowerPoint relinquishes ownership even if NULL was not confirmed.
                result.stopConfirmed = false;
                qWarning() << "PowerPoint deck teardown did not confirm for deck" << index_;
            }
        } else {
            if (!releasePdfAfterDetach(QStringLiteral("CUE deck replacement"))) return result;
            result.stopConfirmed = false;
        }
    }
    if (!releaseUvcAudio()) {
        result.stopConfirmed = false;
        return result;
    }
    if (uvcAudioRetirement_ && !uvcAudioRetirement_->succeeded()) result.stopConfirmed = false;
    uvcDevicePath_.clear();
    uvcErrorGeneration_ = 0;
    uvcProgramPaused_ = false;
    powerPointFilePath_.clear();
    powerPointErrorGeneration_ = 0;
    pdfFilePath_.clear();
    pdfErrorGeneration_ = 0;
    if (programGpuCompositor_) {
        programGpuCompositor_->setDeckFrameUpdatesEnabled(index_, true);
        programGpuCompositor_->clearDeckFrame(index_);
    }
#endif
    g_object_set(playbin_, "uri", nullptr, nullptr);
    sourceType_ = Type::None;
    hasVideo_ = false;
    hasAudio_ = false;
    inPointMs_ = 0;
    outPointMs_ = -1;
    playbackRate_ = 1.0;
    preparationState_ = PreparationState::Empty;
    result.succeeded = true;
    result.detached = true;
    result.ownershipReleased = true;
    return result;
}

DeckSourceSession::ReleaseResult DeckSourceSession::standbyAfterTake()
{
    ReleaseResult result;
    bool succeeded = true;
    switch (sourceType_) {
    case Type::File:
#ifdef _WIN32
        succeeded = replaceFilePipeline();
        result.detached = result.ownershipReleased = succeeded;
#else
        succeeded = !filePreparationPending() && gst_element_set_state(playbin_, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE;
#endif
        break;
    case Type::Uvc:
#ifdef _WIN32
        succeeded = programGpuCompositor_->configureDeckInput(index_, playbin_, appSink_);
        result.detached = succeeded;
        if (succeeded) {
            if (!vtrpon::isLiveAudioSource(uvcDevicePath_))
                uvcSourceSessions_->releaseProgramDeck(index_);
            succeeded = releaseUvcAudio();
        }
        if (succeeded) {
            uvcDevicePath_.clear();
            uvcErrorGeneration_ = 0;
            uvcProgramPaused_ = false;
            result.ownershipReleased = true;
        }
#else
        succeeded = false;
#endif
        break;
    case Type::PowerPoint:
#ifdef _WIN32
        succeeded = programGpuCompositor_->configureDeckInput(index_, playbin_, appSink_);
        result.detached = succeeded;
        if (succeeded) {
            succeeded = powerPointSources_ != nullptr;
            if (succeeded) {
                result.stopConfirmed = powerPointSources_->releaseProgramDeck(index_);
                if (!result.stopConfirmed)
                    qWarning() << "PowerPoint standby teardown did not confirm for deck" << index_;
                powerPointFilePath_.clear();
                powerPointErrorGeneration_ = 0;
                result.ownershipReleased = true;
            }
        }
#else
        succeeded = false;
#endif
        break;
    case Type::Pdf:
#ifdef _WIN32
        succeeded = pdfSources_ != nullptr;
        if (succeeded) {
            pdfSources_->suspendProgramDeck(index_);
            result.detached = programGpuCompositor_->tryConfigureDeckInput(index_, playbin_, appSink_);
            if (!result.detached) {
                // TAKE is already committed. Retain cleanup identity without rollback.
                qWarning() << "TAKE completed, but PDF deck detach remains pending for deck" << index_;
            } else {
                result.ownershipReleased = releasePdfAfterDetach(QStringLiteral("TAKE standby"), true);
                result.stopConfirmed = false;
                if (result.ownershipReleased) {
                    pdfFilePath_.clear();
                    pdfErrorGeneration_ = 0;
                }
            }
        }
#else
        succeeded = false;
#endif
        break;
    case Type::None:
        result.detached = result.ownershipReleased = result.stopConfirmed = true;
        break;
    }
    result.succeeded = succeeded;
#ifdef _WIN32
    if (succeeded && programGpuCompositor_) programGpuCompositor_->clearDeckFrame(index_);
#endif
    return result;
}

DeckSourceSession::ReleaseResult DeckSourceSession::clearAfterOffAir()
{
#ifdef _WIN32
    pendingSource_.reset();
    pendingAudioClock_.reset();
#endif
    ReleaseResult result;
#ifdef _WIN32
    if (sourceType_ == Type::File) {
        result.succeeded = replaceFilePipeline();
        result.detached = result.ownershipReleased = result.succeeded;
        return result;
    }
#endif
    if (filePreparationPending()) return result;
    bool cleanupComplete = true;
    if (gst_element_set_state(playbin_, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
        cleanupComplete = false;
        qWarning() << "OFF AIR completed, but deck rejected GST_STATE_NULL for deck" << index_;
    }
    result.stopConfirmed = fileStopConfirmed();
    result.detached = sourceType_ == Type::File || sourceType_ == Type::None;
    g_object_set(playbin_, "uri", nullptr, nullptr);
#ifdef _WIN32
    if (sourceType_ == Type::Uvc) {
        result.stopConfirmed = false;
        if (!uvcSourceSessions_ || !programGpuCompositor_->configureDeckInput(index_, playbin_, appSink_)) {
            cleanupComplete = false;
            qWarning() << "OFF AIR completed, but UVC deck detach failed for deck" << index_;
        } else {
            result.detached = true;
            if (!vtrpon::isLiveAudioSource(uvcDevicePath_))
                uvcSourceSessions_->releaseProgramDeck(index_);
        }
    } else if (sourceType_ == Type::PowerPoint) {
        result.stopConfirmed = false;
        if (!powerPointSources_ || !programGpuCompositor_->configureDeckInput(index_, playbin_, appSink_)) {
            cleanupComplete = false;
            qWarning() << "OFF AIR completed, but PowerPoint deck detach failed for deck" << index_;
        } else {
            result.detached = true;
            const bool stopped = powerPointSources_->releaseProgramDeck(index_);
            result.stopConfirmed = stopped && fileStopConfirmed();
            if (!stopped) {
                qWarning() << "OFF AIR completed, but PowerPoint deck teardown did not confirm for deck" << index_;
            }
        }
    } else if (sourceType_ == Type::Pdf) {
        result.stopConfirmed = false;
        if (pdfSources_) pdfSources_->suspendProgramDeck(index_);
        if (!pdfSources_ || !programGpuCompositor_->tryConfigureDeckInput(index_, playbin_, appSink_)) {
            cleanupComplete = false;
            qWarning() << "OFF AIR completed, but PDF deck detach failed for deck" << index_;
        } else {
            result.detached = true;
            const bool released = releasePdfAfterDetach(QStringLiteral("OFF AIR"), true);
            result.stopConfirmed = false;
            cleanupComplete = released && cleanupComplete;
        }
    }
    if (!releaseUvcAudio()) {
        cleanupComplete = false;
        result.stopConfirmed = false;
        qWarning() << "OFF AIR completed, but mapped UVC audio teardown did not confirm for deck" << index_;
    }
    if (uvcAudioRetirement_ && !uvcAudioRetirement_->succeeded()) result.stopConfirmed = false;
    programGpuCompositor_->clearDeckFrame(index_);
#endif
    hasVideo_ = false;
    hasAudio_ = false;
    if (cleanupComplete) {
        sourceType_ = Type::None;
        inPointMs_ = 0;
        outPointMs_ = -1;
        preparationState_ = PreparationState::Empty;
#ifdef _WIN32
        uvcDevicePath_.clear();
        uvcErrorGeneration_ = 0;
        uvcProgramPaused_ = false;
        powerPointFilePath_.clear();
        powerPointErrorGeneration_ = 0;
        pdfFilePath_.clear();
        pdfErrorGeneration_ = 0;
#endif
    } else {
        qWarning() << "OFF AIR retained deck ownership for cleanup retry on deck" << index_;
    }
    result.succeeded = cleanupComplete;
    result.ownershipReleased = cleanupComplete;
    return result;
}


void DeckSourceSession::refreshPreparationState()
{
    if (preparationState_ != PreparationState::Loading) {
        return;
    }
    if (sourceType_ == Type::File && preparationDeadline_.hasExpired()) {
        mediaOffline_ = true;
        preparationState_ = PreparationState::Error;
        preparationError_ = AsyncFilePreparation::offlineError();
        return;
    }
    if (index_ < 0) {
        preparationState_ = PreparationState::Error;
        preparationError_ = QStringLiteral("No standby player is available.");
        return;
    }

#ifdef _WIN32
    if (sourceType_ == Type::PowerPoint) {
        if (!powerPointSources_) {
            preparationState_ = PreparationState::Error;
            preparationError_ = QStringLiteral("PowerPoint source manager is unavailable.");
            return;
        }
        if (!nativeInputAttached_) {
            const auto source = powerPointSources_->programSource(index_);
            if (!source.isValid()) return;
            if (!programGpuCompositor_->configureDeckInput(index_, source.pipeline, source.appSink)) {
                preparationState_ = PreparationState::Error;
                preparationError_ = QStringLiteral("Could not prepare PowerPoint for ON AIR.");
                return;
            }
            nativeInputAttached_ = true;
            programGpuCompositor_->setDeckFrameUpdatesEnabled(index_, true);
        }
        if (powerPointSources_->sourceIsReady(index_)
            && programGpuCompositor_->deckFrameReady(index_)) {
            hasVideo_ = true;
            hasAudio_ = false;
            preparationState_ = PreparationState::Ready;
        }
        return;
    }
    if (sourceType_ == Type::Pdf) {
        if (!pdfSources_) {
            preparationState_ = PreparationState::Error;
            preparationError_ = QStringLiteral("PDF source manager is unavailable.");
            return;
        }
        if (!nativeInputAttached_) {
            const auto source = pdfSources_->programSource(index_);
            if (!source.isValid()) return;
            if (!programGpuCompositor_->configureDeckInput(index_, source.pipeline, source.appSink)) {
                preparationState_ = PreparationState::Error;
                preparationError_ = QStringLiteral("Could not prepare PDF for ON AIR.");
                return;
            }
            nativeInputAttached_ = true;
            programGpuCompositor_->setDeckFrameUpdatesEnabled(index_, true);
        }
        if (programGpuCompositor_->deckFrameReady(index_)) {
            hasVideo_ = true;
            hasAudio_ = false;
            preparationState_ = PreparationState::Ready;
        }
        return;
    }
    if (sourceType_ == Type::Uvc) {
        if (vtrpon::isLiveAudioSource(uvcDevicePath_)) {
            GstState state = GST_STATE_NULL;
            const auto result = uvcAudioPipeline_
                ? gst_element_get_state(uvcAudioPipeline_, &state, nullptr, 0)
                : GST_STATE_CHANGE_FAILURE;
            if (result == GST_STATE_CHANGE_FAILURE || preparationDeadline_.hasExpired()) {
                preparationState_ = PreparationState::Error;
                preparationError_ = QStringLiteral("Could not prepare the live audio input.");
            } else if (state == GST_STATE_PLAYING) {
                hasVideo_ = false;
                hasAudio_ = true;
                preparationState_ = PreparationState::Ready;
            }
            return;
        }
        if (!uvcSourceSessions_->sourceIsPlaying(
                uvcDevicePath_)) {
            preparationState_ = PreparationState::Error;
            if (preparationError_.isEmpty()) {
                preparationError_ = QStringLiteral(
                    "Could not check whether the UVC input is ready.");
            }
            return;
        }
        if (programGpuCompositor_->deckFrameReady(index_)) {
            hasVideo_ = true;
            hasAudio_ = uvcAudioPipeline_ != nullptr;
            preparationState_ = PreparationState::Ready;
        }
        return;
    }
#endif
    if (sourceType_ != Type::File) {
        return;
    }
    if (fileRequest_) {
        filePreparation_ = AsyncFilePreparation::start(playbin_, *fileRequest_,
            static_cast<int>(preparationDeadline_.remainingTime()));
        fileRequest_.reset();
    }
    if (!filePreparation_) return;
    const auto state = filePreparation_->state();
    if (state == AsyncFilePreparation::State::Loading) return;
    if (state != AsyncFilePreparation::State::Ready) {
        mediaOffline_ = state == AsyncFilePreparation::State::Offline;
        preparationState_ = PreparationState::Error;
        preparationError_ = mediaOffline_ ? AsyncFilePreparation::offlineError()
            : filePreparation_->done() ? filePreparation_->result().error
                                      : QStringLiteral("Media preparation was cancelled.");
        return;
    }
    hasVideo_ = filePreparation_->result().hasVideo;
    hasAudio_ = filePreparation_->result().hasAudio;
#ifdef _WIN32
    if (hasVideo_ && !programGpuCompositor_->deckFrameReady(index_)) {
        if (preparationDeadline_.hasExpired()) {
            preparationState_ = PreparationState::Error;
            preparationError_ = QStringLiteral("No video frame arrived before the preparation deadline.");
        }
        return;
    }
#endif
    filePreparation_.reset();
    preparationState_ = PreparationState::Ready;
}

DeckSourceSession::PreparationResult DeckSourceSession::refreshPreparation()
{
#ifdef _WIN32
    if (pendingSource_) {
        if (uvcAudioRetirement_ && !uvcAudioRetirement_->done())
            return {PreparationState::Loading, {}};
        if (pendingSource_->type == DualDeckPlayer::DeckSource::Type::Uvc
            && uvcSourceSessions_ && uvcSourceSessions_->sourceIsRetiring(pendingSource_->locator))
            return {PreparationState::Loading, {}};
        const auto source = *pendingSource_;
        const auto clock = pendingAudioClock_;
        const auto volume = pendingItemVolume_;
        pendingSource_.reset();
        pendingAudioClock_.reset();
        return prepare(source, volume, clock.get());
    }
#endif
    QString detail;
    if (takeSourceError(detail)) {
        preparationState_ = PreparationState::Error;
        preparationError_ = detail;
    }
    refreshPreparationState();
    return {preparationState_, preparationError_};
}

bool DeckSourceSession::setFileState(GstState state)
{
    if (sourceType_ != Type::File || !playbin_ || filePreparationActive()) return false;
#ifndef _WIN32
    if (state == GST_STATE_PAUSED)
        setBooleanPropertyIfAvailable(monitorSink_, "async", FALSE);
#endif
    return gst_element_set_state(playbin_, state) != GST_STATE_CHANGE_FAILURE;
}

qint64 DeckSourceSession::positionMs() const
{
    if (sourceType_ != Type::File || !playbin_ || filePreparationActive()) return -1;
    gint64 position = 0;
    return gst_element_query_position(playbin_, GST_FORMAT_TIME, &position)
        ? position / GST_MSECOND : -1;
}
qint64 DeckSourceSession::durationMs() const
{
    if (sourceType_ != Type::File || !playbin_ || filePreparationActive()) return -1;
    gint64 duration = 0;
    return gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration)
        ? duration / GST_MSECOND : -1;
}

bool DeckSourceSession::seek(qint64 positionMs)
{
    if (sourceType_ != Type::File || !playbin_ || filePreparationActive()) return false;
    const qint64 upperBound = outPointMs_ >= 0 ? outPointMs_ : (std::max)(positionMs, inPointMs_);
    const qint64 target = std::clamp(positionMs, inPointMs_, upperBound);
    return gst_element_seek(playbin_, playbackRate_, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, target * GST_MSECOND,
        outPointMs_ >= 0 ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
        outPointMs_ >= 0 ? outPointMs_ * GST_MSECOND : GST_CLOCK_TIME_NONE);
}

bool DeckSourceSession::sourceIsPlaying() const
{
#ifdef _WIN32
    if (sourceType_ == Type::PowerPoint)
        return powerPointSources_ && powerPointSources_->sourceIsPlaying(index_);
    if (sourceType_ == Type::Pdf)
        return pdfSources_ && pdfSources_->sourceIsPlaying(index_);
    if (sourceType_ == Type::Uvc && vtrpon::isLiveAudioSource(uvcDevicePath_)) {
        GstState state = GST_STATE_NULL;
        return uvcAudioPipeline_ &&
            gst_element_get_state(uvcAudioPipeline_, &state, nullptr, 0) != GST_STATE_CHANGE_FAILURE &&
            state == GST_STATE_PLAYING;
    }
    if (sourceType_ == Type::Uvc)
        return uvcSourceSessions_ && uvcSourceSessions_->sourceIsPlaying(uvcDevicePath_);
#endif
    if (sourceType_ != Type::File || !playbin_ || filePreparationActive()) return false;
    // A flushing seek re-prerolls PAUSED while the transport target stays PLAYING.
    GST_OBJECT_LOCK(playbin_);
    const bool playing = GST_STATE_TARGET(playbin_) == GST_STATE_PLAYING
        && GST_STATE_RETURN(playbin_) != GST_STATE_CHANGE_FAILURE;
    GST_OBJECT_UNLOCK(playbin_);
    return playing;
}

void DeckSourceSession::setLiveFrameUpdates(bool enabled)
{
#ifdef _WIN32
    if (programGpuCompositor_)
        programGpuCompositor_->setDeckFrameUpdatesEnabled(index_, enabled);
    if (sourceType_ == Type::Uvc) uvcProgramPaused_ = !enabled;
#else
    Q_UNUSED(enabled);
#endif
}

void DeckSourceSession::setItemVolume(double volume)
{
    GstElement *volumeElement = itemAudioVolume_;
#ifdef _WIN32
    if (sourceType_ == Type::Uvc) volumeElement = uvcItemAudioVolume_;
#endif
    if (volumeElement) g_object_set(volumeElement, "volume", volume, nullptr);
}

bool DeckSourceSession::takeSourceError(QString &detail)
{
#ifdef _WIN32
    if (sourceType_ == Type::Uvc && uvcSourceSessions_ && !uvcDevicePath_.isEmpty()
        && !vtrpon::isLiveAudioSource(uvcDevicePath_))
        return uvcSourceSessions_->sourceErrorAfter(uvcDevicePath_, uvcErrorGeneration_, uvcErrorGeneration_, detail);
    if (sourceType_ == Type::PowerPoint && powerPointSources_ && !powerPointFilePath_.isEmpty())
        return powerPointSources_->sourceErrorAfter(index_, powerPointErrorGeneration_, powerPointErrorGeneration_, detail);
    if (sourceType_ == Type::Pdf && pdfSources_ && !pdfFilePath_.isEmpty())
        return pdfSources_->sourceErrorAfter(index_, pdfErrorGeneration_, pdfErrorGeneration_, detail);
#else
    Q_UNUSED(detail);
#endif
    return false;
}


bool DeckSourceSession::setRate(double rate)
{
    if (!playbin_ || sourceType_ != Type::File || filePreparationActive()
        || rate == 0.0) {
        return false;
    }

    // Changing speed is not a transport command. Apply the new segment
    // rate without changing the deck's current PAUSED/PLAYING state.

    gint64 position = 0;
    if (!gst_element_query_position(playbin_, GST_FORMAT_TIME,
                                    &position)) {
        return false;
    }

    if (rate > 0.0) {
        // scaletempo derives its factor from SEGMENT events. An instant
        // rate change does not send a new segment through this audio
        // branch, leaving scaletempo at 1x while the segment runs at the
        // requested rate. Always issue a flushing seek so video and the
        // time-stretched audio receive the same segment rate.
        const GstSeekFlags flags = static_cast<GstSeekFlags>(
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);
        const bool succeeded = gst_element_seek(
            playbin_, rate, GST_FORMAT_TIME, flags,
            GST_SEEK_TYPE_SET, position,
            outPointMs_ >= 0 ? GST_SEEK_TYPE_SET
                                  : GST_SEEK_TYPE_NONE,
            outPointMs_ >= 0
                ? outPointMs_ * GST_MSECOND
                : GST_CLOCK_TIME_NONE);
        if (succeeded) {
            playbackRate_ = rate;
        }
        return succeeded;
    }

    const GstSeekFlags flags = static_cast<GstSeekFlags>(
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);
    const bool succeeded = gst_element_seek(
        playbin_, rate, GST_FORMAT_TIME, flags,
        GST_SEEK_TYPE_SET, 0,
        GST_SEEK_TYPE_SET, position);
    if (succeeded) {
        // Reverse playback keeps the legacy seek behavior: a later
        // absolute seek resumes in the normal forward direction.
        playbackRate_ = 1.0;
    }
    return succeeded;
}

PowerPointNavigationState DeckSourceSession::powerPointNavigationState()
{
#ifdef _WIN32
    if (sourceType_ == Type::PowerPoint && powerPointSources_) return powerPointSources_->navigationState(index_);
#endif
    return {};
}

PowerPointCommandResponse DeckSourceSession::nextPowerPointSlide()
{
#ifdef _WIN32
    if (sourceType_ == Type::PowerPoint && powerPointSources_) return powerPointSources_->next(index_);
#endif
    return {};
}

PowerPointCommandResponse DeckSourceSession::previousPowerPointSlide()
{
#ifdef _WIN32
    if (sourceType_ == Type::PowerPoint && powerPointSources_) return powerPointSources_->previous(index_);
#endif
    return {};
}

PdfNavigationState DeckSourceSession::pdfNavigationState()
{
#ifdef _WIN32
    if (sourceType_ == Type::Pdf && pdfSources_) return pdfSources_->navigationState(index_);
#endif
    return {};
}

PdfCommandResponse DeckSourceSession::nextPdfPage()
{
#ifdef _WIN32
    if (sourceType_ == Type::Pdf && pdfSources_) return pdfSources_->next(index_);
#endif
    return {};
}

PdfCommandResponse DeckSourceSession::previousPdfPage()
{
#ifdef _WIN32
    if (sourceType_ == Type::Pdf && pdfSources_) return pdfSources_->previous(index_);
#endif
    return {};
}
