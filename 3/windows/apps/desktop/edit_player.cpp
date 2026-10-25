#include "edit_player.h"
#include "gst_frame_capture.h"
#include "audio_delay.h"
#include "audio_effects.h"
#include "async_pipeline_stop.h"
#include "async_file_preparation.h"
#include "edit_pfl_output.h"
#include "edit_pfl_protocol.h"

#include <QDebug>
#include <QFileInfo>
#include <QUrl>
#include <QWidget>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMeterFloorDb = -60.0;
constexpr GstClockTime kEditMinimumFrameInterval = GST_SECOND / 30;
constexpr qint64 kFallbackFrameDurationMs = 33;
constexpr guint kPlayFlagText = 1U << 2;
constexpr guint kPlayFlagVisualization = 1U << 3;
constexpr guint kPlayFlagDownload = 1U << 7;
constexpr guint kPlayFlagBuffering = 1U << 8;
constexpr guint kPlayFlagDeinterlace = 1U << 9;
constexpr guint kPlayFlagSoftColorBalance = 1U << 10;

struct EditPflTap {
    std::shared_ptr<EditPflFeed> feed;
    GstElement *pipeline = nullptr; // Owns this pad; valid until probe removal.
    GstSegment segment;
    explicit EditPflTap(std::shared_ptr<EditPflFeed> source, GstElement *owner)
        : feed(std::move(source)), pipeline(owner) { gst_segment_init(&segment, GST_FORMAT_TIME); }
};

GstPadProbeReturn copyEditPflAudio(GstPad *, GstPadProbeInfo *info, gpointer data)
{
    auto &tap = *static_cast<EditPflTap *>(data);
    const auto type = GST_PAD_PROBE_INFO_TYPE(info);
    if ((type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (!event) return GST_PAD_PROBE_OK;
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment && segment->format == GST_FORMAT_TIME) tap.segment = *segment;
            tap.feed->flush();
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START
                   || GST_EVENT_TYPE(event) == GST_EVENT_EOS) tap.feed->flush();
        return GST_PAD_PROBE_OK;
    }
    if ((type & GST_PAD_PROBE_TYPE_BUFFER) == 0) return GST_PAD_PROBE_OK;
    auto *audio = tap.feed->audio();
    if (!audio || !audio->enabled.load() || !audio->playing.load()
        || tap.segment.rate != 1.0) return GST_PAD_PROBE_OK;
    const auto epoch = audio->generation.load();
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) return GST_PAD_PROBE_OK;
    const auto running = gst_segment_to_running_time(&tap.segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    if (!GST_CLOCK_TIME_IS_VALID(running)) return GST_PAD_PROBE_OK;
    GstClock *clock = gst_element_get_clock(tap.pipeline);
    if (!clock) return GST_PAD_PROBE_OK;
    const auto clockNow = gst_clock_get_time(clock);
    const auto base = gst_element_get_base_time(tap.pipeline);
    gst_object_unref(clock);
    if (!GST_CLOCK_TIME_IS_VALID(base)) return GST_PAD_PROBE_OK;
    const auto due = EditPflFeed::nowNs() + static_cast<std::int64_t>(base + running)
        - static_cast<std::int64_t>(clockNow);
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    constexpr auto frameBytes = vtrpon::pfl::kChannels * sizeof(float);
    // Caps at the tap are fixed PCM. Bound work even for malformed buffers.
    if (map.size % frameBytes == 0 && map.size <= vtrpon::pfl::kRate * frameBytes) {
        const auto frames = static_cast<std::uint32_t>(map.size / frameBytes);
        const auto *samples = reinterpret_cast<const float *>(map.data);
        for (std::uint32_t offset = 0; offset < frames; offset += vtrpon::pfl::kFrames) {
            const auto count = (std::min)(vtrpon::pfl::kFrames, frames - offset);
            if (!audio->push(samples + offset * 2, count,
                    due + static_cast<std::int64_t>(offset) * GST_SECOND / vtrpon::pfl::kRate, epoch)) break;
        }
    }
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}

struct EditCadenceLimiter {
    EditCadenceLimiter()
    {
        gst_segment_init(&segment, GST_FORMAT_TIME);
    }

    void reset(bool resetSegment = false)
    {
        if (resetSegment) {
            gst_segment_init(&segment, GST_FORMAT_TIME);
        }
        lastKeptRunningTime = GST_CLOCK_TIME_NONE;
    }

    GstSegment segment;
    GstClockTime lastKeptRunningTime = GST_CLOCK_TIME_NONE;
};

bool hasElementFactory(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

void setBooleanPropertyIfAvailable(GstElement *element, const char *name,
                                   gboolean value)
{
    if (element
        && g_object_class_find_property(G_OBJECT_GET_CLASS(element), name)) {
        g_object_set(element, name, value, nullptr);
    }
}

bool readLevelChannels(const GValue *values, double &left, double &right)
{
    if (!values) {
        return false;
    }

    guint channelCount = 0;
    const GValue *leftValue = nullptr;
    const GValue *rightValue = nullptr;
    const GType valueType = G_VALUE_TYPE(values);
    static const GType glibArrayType = g_type_from_name("GValueArray");
    static const GType gstArrayType = g_type_from_name("GstValueArray");
    static const GType listType = g_type_from_name("GstValueList");
    if (glibArrayType != G_TYPE_INVALID
        && g_type_is_a(valueType, glibArrayType)) {
        const auto *array = static_cast<const GValueArray *>(
            g_value_get_boxed(values));
        if (array && array->n_values > 0) {
            channelCount = array->n_values;
            leftValue = &array->values[0];
            rightValue = channelCount > 1 ? &array->values[1] : leftValue;
        }
    } else if (gstArrayType != G_TYPE_INVALID
               && g_type_is_a(valueType, gstArrayType)) {
        channelCount = gst_value_array_get_size(values);
        if (channelCount > 0) {
            leftValue = gst_value_array_get_value(values, 0);
            rightValue = channelCount > 1
                ? gst_value_array_get_value(values, 1) : leftValue;
        }
    } else if (listType != G_TYPE_INVALID
               && g_type_is_a(valueType, listType)) {
        channelCount = gst_value_list_get_size(values);
        if (channelCount > 0) {
            leftValue = gst_value_list_get_value(values, 0);
            rightValue = channelCount > 1
                ? gst_value_list_get_value(values, 1) : leftValue;
        }
    }
    if (!leftValue || !rightValue
        || !G_VALUE_HOLDS_DOUBLE(leftValue)
        || !G_VALUE_HOLDS_DOUBLE(rightValue)) {
        return false;
    }

    left = g_value_get_double(leftValue);
    right = g_value_get_double(rightValue);
    return true;
}

double displayDb(double value)
{
    if (!std::isfinite(value)) {
        return kMeterFloorDb;
    }
    return std::clamp(value, kMeterFloorDb, 6.0);
}

GstPadProbeReturn limitEditCadence(GstPad *, GstPadProbeInfo *info,
                                   gpointer userData)
{
    auto *limiter = static_cast<EditCadenceLimiter *>(userData);
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
            < kEditMinimumFrameInterval) {
        return GST_PAD_PROBE_DROP;
    }
    limiter->lastKeptRunningTime = runningTime;
    return GST_PAD_PROBE_OK;
}

} // namespace

class EditPlayer::Impl final
{
public:
    explicit Impl(QWidget *videoWindow)
        : videoWindow_(videoWindow)
        , videoWindowHandle_(static_cast<guintptr>(videoWindow->winId()))
    {
        playbin_ = gst_element_factory_make("playbin", "edit-player");
        if (!playbin_) {
            return;
        }

        GstElement *videoOutput = makeVideoOutput();
        GstElement *audioOutput = makeAudioOutput();
        if (!videoOutput || !audioOutput) {
            if (videoOutput) {
                gst_object_unref(videoOutput);
            }
            if (audioOutput) {
                gst_object_unref(audioOutput);
            }
            return;
        }

        guint playFlags = 0;
        g_object_get(playbin_, "flags", &playFlags, nullptr);
        playFlags &= ~(kPlayFlagText
                       | kPlayFlagVisualization
                       | kPlayFlagDownload
                       | kPlayFlagBuffering
                       | kPlayFlagDeinterlace
                       | kPlayFlagSoftColorBalance);
        g_object_set(playbin_,
                     "flags", playFlags,
                     "video-sink", videoOutput,
                     "audio-sink", audioOutput,
                     nullptr);
        // playbin consumes the floating references supplied through its sink
        // properties. Releasing them here leaves playbin with dangling sinks.

        bus_ = gst_element_get_bus(playbin_);
        if (!bus_ || !inputAudioLevelElement_ || !nominalAudioVolumeElement_
            || !audioLevelElement_) {
            return;
        }
        gst_bus_set_sync_handler(bus_, &Impl::handleBusSync, this, nullptr);
        available_ = true;
    }

    ~Impl()
    {
        shutdown();
        detachCallbacks();
        if (!shutdownConfirmed_) {
            // The process is exiting. Preserve backend storage rather than
            // releasing it while a streaming thread may still be active.
            qWarning() << "Skipping unsafe EDIT backend release after shutdown"
                          " could not be confirmed";
            return;
        }
        if (bus_) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (audioLevelElement_) {
            gst_object_unref(audioLevelElement_);
            audioLevelElement_ = nullptr;
        }
        if (nominalAudioVolumeElement_) {
            gst_object_unref(nominalAudioVolumeElement_);
            nominalAudioVolumeElement_ = nullptr;
        }
        if (inputAudioLevelElement_) {
            gst_object_unref(inputAudioLevelElement_);
            inputAudioLevelElement_ = nullptr;
        }
        if (videoSink_) {
            gst_object_unref(videoSink_);
            videoSink_ = nullptr;
        }
        if (videoCadencePad_) {
            if (videoCadenceProbeId_ != 0) {
                gst_pad_remove_probe(videoCadencePad_,
                                     videoCadenceProbeId_);
                videoCadenceProbeId_ = 0;
            }
            gst_object_unref(videoCadencePad_);
            videoCadencePad_ = nullptr;
        }
        if (playbin_) {
            gst_object_unref(playbin_);
            playbin_ = nullptr;
        }
    }

    bool isAvailable() const { return available_ && !shuttingDown_; }
    bool mediaOffline() const { return mediaOffline_; }
    bool hasMedia() const { return isAvailable() && hasMedia_ && !stopJob_ && !preparation_; }
    bool isBusy() const { return (stopJob_ && !stopJob_->done()) || preparation_ || !pendingLoad_.isEmpty(); }
    bool isPlaying() const
    {
        if (!hasMedia()) {
            return false;
        }
        GstState current = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        return gst_element_get_state(playbin_, &current, &pending, 0)
            != GST_STATE_CHANGE_FAILURE
            && (current == GST_STATE_PLAYING || pending == GST_STATE_PLAYING);
    }

    bool canReleaseAfterShutdown() const { return shutdownConfirmed_; }
    std::shared_ptr<EditPflFeed> pflFeed() const { return pflFeed_; }

    void refreshWindowHandle()
    {
        if (!videoWindow_ || shuttingDown_ || stopJob_) {
            return;
        }
        videoWindowHandle_ = static_cast<guintptr>(videoWindow_->winId());
        applyWindowHandle(GST_OBJECT(videoSink_));
        if (videoSink_ && GST_IS_VIDEO_OVERLAY(videoSink_)) {
            gst_video_overlay_expose(GST_VIDEO_OVERLAY(videoSink_));
        }
    }

    void requestShutdown()
    {
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        pflFeed_->setPlaying(false);
        available_ = false;
        hasMedia_ = false;
        mediaDurationMs_ = -1;
        scanActive_ = false;
        scanStartPositionMs_ = 0;
        detachCallbacks();
        pendingLoad_.clear();
        if (!stopJob_) stopJob_ = AsyncPipelineStop::start(playbin_, {},
            AsyncFilePreparation::retire(std::exchange(preparation_, {})));
        resetAudioLevel();
    }

    bool shutdownComplete() const { return shuttingDown_ && stopJob_ && stopJob_->done(); }

    void shutdown()
    {
        requestShutdown();
        shutdownConfirmed_ = shutdownComplete() && stopJob_->succeeded();
    }

    bool load(const QString &filePath)
    {
        if (!isAvailable()) {
            return false;
        }
        if (filePath.isEmpty()) return false;
        const QFileInfo fileInfo(filePath);
        if (stopJob_ && stopJob_->done() && !stopJob_->succeeded()) return false;
        pendingEvent_ = Event::None;
        pendingEventDetail_.clear();
        pendingLoad_ = fileInfo.absoluteFilePath();
        mediaOffline_ = false;
        preparationDeadline_.setRemainingTime(AsyncFilePreparation::timeoutMs);
        beginUnload();
        return true;
    }

    void beginUnload()
    {
        if (stopJob_) return;
        pflFeed_->setPlaying(false);
        pflFeed_->flush();
        hasMedia_ = false;
        mediaDurationMs_ = -1;
        scanActive_ = false;
        scanStartPositionMs_ = 0;
        audioMeterActive_ = false;
        resetAudioLevel();
        auto *pipeline = playbin_;
        auto *bus = bus_;
        stopJob_ = AsyncPipelineStop::start(pipeline, [pipeline, bus] {
            g_object_set(pipeline, "uri", nullptr, nullptr);
            while (auto *message = gst_bus_pop(bus)) gst_message_unref(message);
        }, AsyncFilePreparation::retire(std::exchange(preparation_, {})));
    }

    bool finishUnload()
    {
        if (!stopJob_) return true;
        if (!stopJob_->done()) return false;
        if (!stopJob_->succeeded()) {
            pendingLoad_.clear();
            setEvent(Event::Error, QStringLiteral("Could not stop the EDIT pipeline."));
            return false;
        }
        stopJob_.reset();
        videoCadence_.reset(true); // Streaming probes have stopped.
        return true;
    }

    bool unload()
    {
        mediaOffline_ = false;
        pendingLoad_.clear();
        if (!isAvailable()) return false;
        if (!stopJob_ && !hasMedia_ && !preparation_) return true;
        beginUnload();
        return finishUnload();
    }

    void completePendingLoad()
    {
        if (!finishUnload()) {
            if (!pendingLoad_.isEmpty() && preparationDeadline_.hasExpired()) {
                pendingLoad_.clear();
                setEvent(Event::Error, QStringLiteral("The previous EDIT source is still stopping."));
            }
            return;
        }
        if (!pendingLoad_.isEmpty()) {
            preparation_ = AsyncFilePreparation::start(playbin_, {std::exchange(pendingLoad_, {})},
                static_cast<int>(preparationDeadline_.remainingTime()));
        }
        if (!preparation_) return;
        const auto state = preparation_->state();
        if (state == AsyncFilePreparation::State::Loading) return;
        if (state == AsyncFilePreparation::State::Ready) {
            preparation_.reset();
            hasMedia_ = true;
            return;
        }
        const bool offline = state == AsyncFilePreparation::State::Offline;
        mediaOffline_ = offline;
        const auto detail = offline ? AsyncFilePreparation::offlineError()
            : preparation_->done() ? preparation_->result().error
                                   : QStringLiteral("Media preparation was cancelled.");
        beginUnload();
        setEvent(offline ? Event::MediaOffline : Event::Error, detail);
    }

    bool play()
    {
        if (!setStateIfLoaded(GST_STATE_PLAYING)) {
            return false;
        }
        audioMeterActive_ = true;
        pflFeed_->setPlaying(true);
        return true;
    }

    bool pause()
    {
        pflFeed_->setPlaying(false);
        if (!setStateIfLoaded(GST_STATE_PAUSED)) {
            return false;
        }
        resetAudioLevel();
        return true;
    }

    bool stop()
    {
        pflFeed_->setPlaying(false);
        if (!hasMedia()) {
            return false;
        }
        if (gst_element_set_state(playbin_, GST_STATE_READY)
            == GST_STATE_CHANGE_FAILURE) {
            return false;
        }
        resetAudioLevel();
        return gst_element_set_state(playbin_, GST_STATE_PAUSED)
            != GST_STATE_CHANGE_FAILURE;
    }

    qint64 positionMs() const
    {
        if (!hasMedia()) {
            return -1;
        }
        gint64 position = 0;
        return gst_element_query_position(playbin_, GST_FORMAT_TIME,
                                          &position)
            ? position / GST_MSECOND : -1;
    }

    qint64 durationMs() const
    {
        if (!hasMedia()) {
            return -1;
        }
        if (mediaDurationMs_ > 0) {
            return mediaDurationMs_;
        }
        gint64 duration = 0;
        if (!gst_element_query_duration(playbin_, GST_FORMAT_TIME,
                                        &duration)) {
            return -1;
        }
        const qint64 queriedDurationMs = duration / GST_MSECOND;
        if (queriedDurationMs > 0) {
            mediaDurationMs_ = queriedDurationMs;
        }
        return queriedDurationMs;
    }

    qint64 frameDurationMs() const
    {
        gint numerator = 0;
        gint denominator = 0;
        if (!videoFrameRate(numerator, denominator)) {
            return kFallbackFrameDurationMs;
        }

        const guint64 durationNs = gst_util_uint64_scale_round(
            GST_SECOND, static_cast<guint64>(denominator),
            static_cast<guint64>(numerator));
        return (std::max)(
            qint64{1},
            static_cast<qint64>((durationNs + GST_MSECOND / 2)
                                / GST_MSECOND));
    }

    qint64 frameStepTargetMs(qint64 positionMs, int frameCount) const
    {
        gint numerator = 0;
        gint denominator = 0;
        if (!videoFrameRate(numerator, denominator)) {
            return (std::max)(
                qint64{0},
                positionMs + static_cast<qint64>(frameCount)
                                 * kFallbackFrameDurationMs);
        }

        const long double frameIndex =
            static_cast<long double>((std::max)(positionMs, qint64{0}))
            * numerator / (1000.0L * denominator);
        const qint64 currentFrameIndex =
            static_cast<qint64>(std::llround(frameIndex));
        const qint64 targetFrameIndex = (std::max)(
            qint64{0}, currentFrameIndex + static_cast<qint64>(frameCount));
        const long double targetMs =
            static_cast<long double>(targetFrameIndex) * 1000.0L
            * denominator / numerator;
        // Seek at or just after the nominal frame boundary. Rounding down can
        // leave an accurate seek inside the preceding frame at fractional
        // rates such as 30000/1001.
        return static_cast<qint64>(std::ceil(targetMs));
    }

    bool seek(qint64 positionMs)
    {
        return seek(positionMs, static_cast<GstSeekFlags>(
                                    GST_SEEK_FLAG_FLUSH
                                    | GST_SEEK_FLAG_KEY_UNIT));
    }

    bool seekAccurate(qint64 positionMs)
    {
        return seek(positionMs, static_cast<GstSeekFlags>(
                                    GST_SEEK_FLAG_FLUSH
                                    | GST_SEEK_FLAG_ACCURATE));
    }

    bool seek(qint64 positionMs, GstSeekFlags flags)
    {
        pflFeed_->flush();
        if (!hasMedia()) {
            return false;
        }
        const qint64 duration = durationMs();
        const qint64 target = duration >= 0
            ? std::clamp(positionMs, qint64{0}, duration)
            : (std::max)(positionMs, qint64{0});
        return gst_element_seek_simple(playbin_, GST_FORMAT_TIME, flags,
                                       target * GST_MSECOND);
    }

    bool startForwardScan(double speed)
    {
        if (!hasMedia() || !std::isfinite(speed) || speed <= 1.0
            || scanActive_) {
            return false;
        }

        const qint64 duration = durationMs();
        const qint64 position = positionMs();
        if (duration <= 0 || position < 0) {
            return false;
        }

        const qint64 clampedPosition = std::clamp(
            position, qint64{0}, duration);
        if (clampedPosition >= duration) {
            return false;
        }

        if (pendingEvent_ == Event::EndOfStream
            || pendingEvent_ == Event::ScanBoundary) {
            pendingEvent_ = Event::None;
            pendingEventDetail_.clear();
        }

        const GstSeekFlags flags = static_cast<GstSeekFlags>(
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE
            | GST_SEEK_FLAG_TRICKMODE
            | GST_SEEK_FLAG_TRICKMODE_NO_AUDIO);
        if (!gst_element_seek(
                playbin_, speed, GST_FORMAT_TIME, flags,
                GST_SEEK_TYPE_SET,
                static_cast<gint64>(clampedPosition) * GST_MSECOND,
                GST_SEEK_TYPE_SET,
                static_cast<gint64>(duration) * GST_MSECOND)) {
            return false;
        }

        scanActive_ = true;
        scanStartPositionMs_ = clampedPosition;
        audioMeterActive_ = false;
        resetAudioLevel();
        if (gst_element_set_state(playbin_, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
            scanActive_ = false;
            (void)gst_element_seek_simple(
                playbin_, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(
                    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                clampedPosition * GST_MSECOND);
            (void)gst_element_set_state(playbin_, GST_STATE_PAUSED);
            return false;
        }
        return true;
    }

    bool stopForwardScan(bool resumePlayback, bool boundaryReached,
                         qint64 &positionMs)
    {
        if (!hasMedia() || !scanActive_) {
            return false;
        }

        const qint64 duration = durationMs();
        qint64 target = this->positionMs();
        if (boundaryReached) {
            target = duration;
        }
        if (target < 0) {
            target = scanStartPositionMs_;
        }
        if (duration >= 0) {
            target = std::clamp(target, qint64{0}, duration);
        } else {
            target = (std::max)(target, qint64{0});
        }

        scanActive_ = false;
        scanStartPositionMs_ = 0;
        if (pendingEvent_ == Event::ScanBoundary) {
            pendingEvent_ = Event::None;
            pendingEventDetail_.clear();
        }

        const GstSeekFlags flags = static_cast<GstSeekFlags>(
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);
        if (!gst_element_seek(
                playbin_, 1.0, GST_FORMAT_TIME, flags,
                GST_SEEK_TYPE_SET,
                static_cast<gint64>(target) * GST_MSECOND,
                GST_SEEK_TYPE_SET,
                static_cast<gint64>(duration) * GST_MSECOND)) {
            (void)gst_element_set_state(playbin_, GST_STATE_PAUSED);
            resetAudioLevel();
            positionMs = target;
            return false;
        }

        const GstState restoreState = resumePlayback
            ? GST_STATE_PLAYING : GST_STATE_PAUSED;
        if (gst_element_set_state(playbin_, restoreState)
            == GST_STATE_CHANGE_FAILURE) {
            (void)gst_element_set_state(playbin_, GST_STATE_PAUSED);
            resetAudioLevel();
            positionMs = target;
            return false;
        }

        audioMeterActive_ = resumePlayback;
        pflFeed_->setPlaying(resumePlayback);
        if (!resumePlayback) {
            resetAudioLevel();
        }
        positionMs = target;
        return true;
    }

    bool setNominalDelayFrames(int frames)
    {
        if (stopJob_ || !playbin_) return false;
        GstElement *output = nullptr;
        g_object_get(playbin_, "audio-sink", &output, nullptr);
        const bool result = vtrpon::setAudioDelayFrames(output, "edit-nominal-delay", frames);
        if (output) gst_object_unref(output);
        return result;
    }

    bool setNominalEq(double lowDb, double midDb, double highDb)
    {
        if (stopJob_ || !playbin_ || !std::isfinite(lowDb) || !std::isfinite(midDb) || !std::isfinite(highDb)) return false;
        GstElement *output = nullptr;
        g_object_get(playbin_, "audio-sink", &output, nullptr);
        if (!output) return false;
        GstElement *effects = gst_bin_get_by_name(GST_BIN(output), "edit-nominal-effects");
        gst_object_unref(output);
        if (!effects) return false;
        vtrpon::setAudioEffectEqualizer(effects, lowDb, midDb, highDb);
        gst_object_unref(effects);
        return true;
    }

    bool setNominalVolume(double linearGain)
    {
        if (stopJob_ || !nominalAudioVolumeElement_ || !std::isfinite(linearGain)
            || linearGain < 0.0) {
            return false;
        }
        g_object_set(nominalAudioVolumeElement_, "volume", linearGain,
                     nullptr);
        return true;
    }

    AudioLevel inputAudioLevel() const { return inputAudioLevel_; }
    AudioLevel audioLevel() const { return audioLevel_; }

    void processBusMessages()
    {
        if (!bus_ || shuttingDown_) {
            return;
        }
        completePendingLoad();
        if (stopJob_ || preparation_) return;
        while (GstMessage *message = gst_bus_pop(bus_)) {
            const GstMessageType type = GST_MESSAGE_TYPE(message);
            if (type == GST_MESSAGE_STATE_CHANGED) {
              GstState oldState, newState, pendingState;
              gst_message_parse_state_changed(message, &oldState, &newState,
                                              &pendingState);
              qDebug() << "GST_STATE" << GST_MESSAGE_SRC_NAME(message)
                       << gst_element_state_get_name(oldState) << "->"
                       << gst_element_state_get_name(newState) << "pending="
                       << gst_element_state_get_name(pendingState);
            }
            if (type == GST_MESSAGE_ERROR) {
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                qWarning().noquote()
                    << "GST_BUS" << (error ? error->message : "Unknown error")
                    << (debug ? debug : "");
                const QString detail = QString::fromUtf8(
                    error ? error->message : "Unknown GStreamer error");
                const bool offline = AsyncFilePreparation::isOfflineError(error);
                g_clear_error(&error);
                g_free(debug);
                // A GStreamer error invalidates the loaded media. Reuse the
                // bounded NULL transition used by ordinary unload instead of
                // leaving EDIT controls enabled against an unchecked backend.
                if (!unload()) {
                    hasMedia_ = false;
                    resetAudioLevel();
                }
                mediaOffline_ = offline;
                setEvent(offline ? Event::MediaOffline : Event::Error, detail);
            } else if (type == GST_MESSAGE_SEGMENT_DONE) {
                if (scanActive_) {
                    setEvent(Event::ScanBoundary, QString());
                }
            } else if (type == GST_MESSAGE_EOS) {
                setEvent(scanActive_ ? Event::ScanBoundary
                                     : Event::EndOfStream,
                         QString());
            } else if (type == GST_MESSAGE_ELEMENT) {
                if (GST_MESSAGE_SRC(message)
                    == GST_OBJECT(inputAudioLevelElement_)) {
                    updateAudioLevel(message, inputAudioLevel_);
                } else if (GST_MESSAGE_SRC(message)
                           == GST_OBJECT(audioLevelElement_)) {
                    updateAudioLevel(message, audioLevel_);
                }
            }
            gst_message_unref(message);
            if (stopJob_) break;
        }
    }

    Event takeEvent(QString &detail)
    {
        const Event event = pendingEvent_;
        detail = pendingEventDetail_;
        pendingEvent_ = Event::None;
        pendingEventDetail_.clear();
        return event;
    }

private:
    bool videoFrameRate(gint &numerator, gint &denominator) const
    {
        if (!hasMedia() || !videoCadencePad_) {
            return false;
        }
        GstCaps *caps = gst_pad_get_current_caps(videoCadencePad_);
        if (!caps) {
            return false;
        }
        GstVideoInfo info;
        const bool parsed = gst_video_info_from_caps(&info, caps);
        gst_caps_unref(caps);
        if (!parsed || GST_VIDEO_INFO_FPS_N(&info) <= 0
            || GST_VIDEO_INFO_FPS_D(&info) <= 0) {
            return false;
        }
        numerator = GST_VIDEO_INFO_FPS_N(&info);
        denominator = GST_VIDEO_INFO_FPS_D(&info);
        return true;
    }

    void detachCallbacks()
    {
        if (callbacksDetached_) {
            return;
        }
        callbacksDetached_ = true;
        if (pflPad_) {
            if (pflProbeId_) gst_pad_remove_probe(pflPad_, pflProbeId_);
            pflProbeId_ = 0;
            gst_object_unref(pflPad_);
            pflPad_ = nullptr;
        }
        if (bus_) {
            gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
        }
        if (videoCadencePad_ && videoCadenceProbeId_ != 0) {
            gst_pad_remove_probe(videoCadencePad_, videoCadenceProbeId_);
            videoCadenceProbeId_ = 0;
        }
    }

public:
    VideoFrameCapture captureStillFrame() const {
        return hasMedia_ && !isBusy() && !shuttingDown_ ? captureSinkFrame(videoSink_) : VideoFrameCapture{};
    }

private:
    GstElement *makeVideoOutput()
    {
        const bool useD3d11 = hasElementFactory("d3d11compositor")
            && hasElementFactory("d3d11convert")
            && hasElementFactory("d3d11upload")
            && hasElementFactory("d3d11videosink");
        const QString description = useD3d11
            ? QStringLiteral(
                "d3d11compositor name=edit-video-compositor "
                "background=black ! "
                "video/x-raw(memory:D3D11Memory),width=1280,height=720,"
                "pixel-aspect-ratio=1/1,format=BGRA ! "
                "queue name=edit-video-output max-size-buffers=1 "
                "max-size-bytes=0 max-size-time=0 "
                "queue name=edit-video-input max-size-buffers=1 "
                "max-size-bytes=0 max-size-time=0 ! "
                "d3d11upload ! "
                "d3d11convert add-borders=true ! "
                "video/x-raw(memory:D3D11Memory),width=1280,height=720,"
                "pixel-aspect-ratio=1/1,format=BGRA ! "
                "edit-video-compositor.sink_0")
            : QStringLiteral(
                "queue name=edit-video-input max-size-buffers=2 "
                "max-size-bytes=0 max-size-time=0 leaky=downstream ! "
                "videoscale add-borders=true ! "
                "video/x-raw,width=1280,height=720,pixel-aspect-ratio=1/1 ! "
                "autovideosink name=edit-video-sink");

        GError *parseError = nullptr;
        GstElement *output = gst_parse_bin_from_description(
            description.toUtf8().constData(), useD3d11 ? FALSE : TRUE,
            &parseError);
        if (parseError) {
            qWarning() << "Failed to create EDIT video output:"
                       << parseError->message;
            g_error_free(parseError);
        }
        if (!output) {
            return nullptr;
        }
        GstElement *inputQueue = gst_bin_get_by_name(
            GST_BIN(output), "edit-video-input");
        GstElement *outputQueue = useD3d11
            ? gst_bin_get_by_name(GST_BIN(output), "edit-video-output")
            : nullptr;
        GstElement *videoCompositor = useD3d11
            ? gst_bin_get_by_name(GST_BIN(output), "edit-video-compositor")
            : nullptr;
        if (!inputQueue || (useD3d11 && (!outputQueue || !videoCompositor))) {
            if (inputQueue) {
                gst_object_unref(inputQueue);
            }
            if (outputQueue) {
                gst_object_unref(outputQueue);
            }
            if (videoCompositor) {
                gst_object_unref(videoCompositor);
            }
            gst_object_unref(output);
            return nullptr;
        }

        if (useD3d11) {
            GstPad *inputPad = gst_element_get_static_pad(inputQueue, "sink");
            GstPad *ghostPad = inputPad
                ? gst_ghost_pad_new("sink", inputPad) : nullptr;
            if (inputPad) {
                gst_object_unref(inputPad);
            }
            if (!ghostPad || !gst_element_add_pad(output, ghostPad)) {
                if (ghostPad) {
                    gst_object_unref(ghostPad);
                }
                gst_object_unref(inputQueue);
                gst_object_unref(outputQueue);
                gst_object_unref(videoCompositor);
                gst_object_unref(output);
                return nullptr;
            }

            GstElement *sink = gst_element_factory_make(
                "d3d11videosink", "edit-video-sink");
            if (!sink || !gst_bin_add(GST_BIN(output), sink)
                || !gst_element_link(outputQueue, sink)) {
                if (sink && !GST_OBJECT_PARENT(sink)) {
                    gst_object_unref(sink);
                }
                gst_object_unref(inputQueue);
                gst_object_unref(outputQueue);
                gst_object_unref(videoCompositor);
                gst_object_unref(output);
                return nullptr;
            }
        }

        videoSink_ = gst_bin_get_by_name(GST_BIN(output), "edit-video-sink");
        if (!videoSink_) {
            gst_object_unref(inputQueue);
            if (outputQueue) {
                gst_object_unref(outputQueue);
            }
            if (videoCompositor) {
                gst_object_unref(videoCompositor);
            }
            gst_object_unref(output);
            return nullptr;
        }
        if (videoCompositor) {
            GstPad *compositorInputPad = gst_element_get_static_pad(
                videoCompositor, "sink_0");
            if (compositorInputPad
                && g_object_class_find_property(
                    G_OBJECT_GET_CLASS(compositorInputPad), "sizing-policy")) {
                g_object_set(compositorInputPad, "sizing-policy", 1, nullptr);
            }
            if (compositorInputPad) {
                gst_object_unref(compositorInputPad);
            }
            gst_object_unref(videoCompositor);
        }
        if (outputQueue) {
            gst_object_unref(outputQueue);
        }
        videoCadencePad_ = gst_element_get_static_pad(inputQueue, "src");
        gst_object_unref(inputQueue);
        videoCadenceProbeId_ = videoCadencePad_
            ? gst_pad_add_probe(
                videoCadencePad_,
                static_cast<GstPadProbeType>(
                    GST_PAD_PROBE_TYPE_BUFFER
                    | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                &limitEditCadence, &videoCadence_, nullptr)
            : 0;
        if (videoCadenceProbeId_ == 0) {
            if (videoCadencePad_) {
                gst_object_unref(videoCadencePad_);
                videoCadencePad_ = nullptr;
            }
            gst_object_unref(output);
            return nullptr;
        }
        setBooleanPropertyIfAvailable(videoSink_, "force-aspect-ratio", TRUE);
        setBooleanPropertyIfAvailable(videoSink_, "enable-last-sample", TRUE);
        setBooleanPropertyIfAvailable(videoSink_, "sync", TRUE);
        setBooleanPropertyIfAvailable(videoSink_, "qos", TRUE);
        setBooleanPropertyIfAvailable(videoSink_, "external-window-only", TRUE);
        applyWindowHandle(GST_OBJECT(videoSink_));
        return output;
    }

    GstElement *makeAudioOutput()
    {
        if (!vtrpon::registerAudioDelay() || !vtrpon::registerAudioEffects()) return nullptr;
        const QString description = QStringLiteral(
            // Decode and meter EDIT audio without opening an output device.
            // Keep the input meter before the nominal-volume stage so SIG
            // remains available even when the fader is at silence.
            "audioconvert ! audioresample ! "
            "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
            "level name=edit-input-audio-level post-messages=true "
            "interval=20000000 ! "
            "vtrponaudioeffects name=edit-nominal-effects slot=edit ! "
            "vtrponaudiodelay name=edit-nominal-delay ! "
            "volume name=edit-nominal-audio-volume ! "
            "level name=edit-audio-level post-messages=true "
            "interval=20000000 ! "
            "fakesink name=edit-audio-output sync=true qos=false "
            "enable-last-sample=false");
        GError *parseError = nullptr;
        GstElement *output = gst_parse_bin_from_description(
            description.toUtf8().constData(), TRUE, &parseError);
        if (parseError) {
            qWarning() << "Failed to create EDIT audio output:"
                       << parseError->message;
            g_error_free(parseError);
        }
        if (!output) {
            return nullptr;
        }
        inputAudioLevelElement_ = gst_bin_get_by_name(
            GST_BIN(output), "edit-input-audio-level");
        nominalAudioVolumeElement_ = gst_bin_get_by_name(
            GST_BIN(output), "edit-nominal-audio-volume");
        if (nominalAudioVolumeElement_ && pflFeed_->available()) {
            pflPad_ = gst_element_get_static_pad(nominalAudioVolumeElement_, "sink");
            if (pflPad_) {
                pflProbeId_ = gst_pad_add_probe(pflPad_, static_cast<GstPadProbeType>(
                    GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM
                    | GST_PAD_PROBE_TYPE_EVENT_FLUSH), &copyEditPflAudio,
                    new EditPflTap(pflFeed_, playbin_),
                    [](gpointer data) { delete static_cast<EditPflTap *>(data); });
            }
        }
        audioLevelElement_ = gst_bin_get_by_name(
            GST_BIN(output), "edit-audio-level");
        if (!inputAudioLevelElement_ || !nominalAudioVolumeElement_
            || !audioLevelElement_) {
            if (audioLevelElement_) {
                gst_object_unref(audioLevelElement_);
                audioLevelElement_ = nullptr;
            }
            if (nominalAudioVolumeElement_) {
                gst_object_unref(nominalAudioVolumeElement_);
                nominalAudioVolumeElement_ = nullptr;
            }
            if (inputAudioLevelElement_) {
                gst_object_unref(inputAudioLevelElement_);
                inputAudioLevelElement_ = nullptr;
            }
            gst_object_unref(output);
            return nullptr;
        }
        return output;
    }

    bool setStateIfLoaded(GstState state)
    {
        return hasMedia()
            && gst_element_set_state(playbin_, state)
                != GST_STATE_CHANGE_FAILURE;
    }

    void drainBus()
    {
        if (!bus_) {
            return;
        }
        while (GstMessage *message = gst_bus_pop(bus_)) {
            gst_message_unref(message);
        }
    }

    void resetAudioLevel()
    {
        pflFeed_->setPlaying(false);
        audioMeterActive_ = false;
        inputAudioLevel_ = {};
        audioLevel_ = {};
    }

    void updateAudioLevel(GstMessage *message, AudioLevel &level)
    {
        if (!audioMeterActive_) {
            return;
        }
        const GstStructure *structure = gst_message_get_structure(message);
        if (!structure || !gst_structure_has_name(structure, "level")) {
            return;
        }
        double leftPeak = kMeterFloorDb;
        double rightPeak = kMeterFloorDb;
        if (!readLevelChannels(gst_structure_get_value(structure, "peak"),
                               leftPeak, rightPeak)) {
            return;
        }
        level.leftPeakDb = displayDb(leftPeak);
        level.rightPeakDb = displayDb(rightPeak);
        level.clipped = leftPeak >= 0.0 || rightPeak >= 0.0;
    }

    void setEvent(Event event, const QString &detail)
    {
        if (pendingEvent_ == Event::Error && event != Event::Error) {
            return;
        }
        pendingEvent_ = event;
        pendingEventDetail_ = detail;
    }

    static GstBusSyncReply handleBusSync(GstBus *, GstMessage *message,
                                         gpointer data)
    {
        auto *player = static_cast<Impl *>(data);
        if (!gst_is_video_overlay_prepare_window_handle_message(message)) {
            return GST_BUS_PASS;
        }
        player->applyWindowHandle(GST_MESSAGE_SRC(message));
        gst_message_unref(message);
        return GST_BUS_DROP;
    }

    void applyWindowHandle(GstObject *object)
    {
        if (object && videoWindowHandle_ != 0
            && GST_IS_VIDEO_OVERLAY(object)) {
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(object), videoWindowHandle_);
        }
    }

    std::shared_ptr<AsyncPipelineStop> stopJob_;
    std::shared_ptr<AsyncFilePreparation> preparation_;
    QDeadlineTimer preparationDeadline_{QDeadlineTimer::Forever};
    bool mediaOffline_ = false;
    QString pendingLoad_;
    GstElement *playbin_ = nullptr;
    std::shared_ptr<EditPflFeed> pflFeed_ = std::make_shared<EditPflFeed>();
    GstPad *pflPad_ = nullptr;
    gulong pflProbeId_ = 0;
    GstElement *videoSink_ = nullptr;
    GstElement *inputAudioLevelElement_ = nullptr;
    GstElement *nominalAudioVolumeElement_ = nullptr;
    GstElement *audioLevelElement_ = nullptr;
    GstBus *bus_ = nullptr;
    GstPad *videoCadencePad_ = nullptr;
    gulong videoCadenceProbeId_ = 0;
    QWidget *videoWindow_ = nullptr;
    guintptr videoWindowHandle_ = 0;
    EditCadenceLimiter videoCadence_;
    AudioLevel inputAudioLevel_;
    AudioLevel audioLevel_;
    Event pendingEvent_ = Event::None;
    QString pendingEventDetail_;
    bool available_ = false;
    bool hasMedia_ = false;
    bool shuttingDown_ = false;
    bool callbacksDetached_ = false;
    bool shutdownConfirmed_ = false;
    bool audioMeterActive_ = false;
    mutable qint64 mediaDurationMs_ = -1;
    bool scanActive_ = false;
    qint64 scanStartPositionMs_ = 0;
};

EditPlayer::EditPlayer(QWidget *videoWindow)
    : impl_(std::make_unique<Impl>(videoWindow))
{
}

EditPlayer::~EditPlayer()
{
    if (!impl_) {
        return;
    }
    impl_->shutdown();
    if (!impl_->canReleaseAfterShutdown()) {
        // Preserve backend storage only when GStreamer could not confirm that
        // its streaming threads reached a terminal state.
        (void)impl_.release();
    }
}

VideoFrameCapture EditPlayer::captureStillFrame() const { return impl_->captureStillFrame(); }

bool EditPlayer::isAvailable() const { return impl_->isAvailable(); }
void EditPlayer::requestShutdown() { impl_->requestShutdown(); }
bool EditPlayer::shutdownComplete() const { return impl_->shutdownComplete(); }
bool EditPlayer::isBusy() const { return impl_->isBusy(); }
bool EditPlayer::mediaOffline() const { return impl_->mediaOffline(); }
bool EditPlayer::hasMedia() const { return impl_->hasMedia(); }
bool EditPlayer::isPlaying() const { return impl_->isPlaying(); }
void EditPlayer::shutdown() { impl_->shutdown(); }
void EditPlayer::refreshWindowHandle() { impl_->refreshWindowHandle(); }
bool EditPlayer::load(const QString &filePath) { return impl_->load(filePath); }
bool EditPlayer::unload() { return impl_->unload(); }
bool EditPlayer::play() { return impl_->play(); }
bool EditPlayer::pause() { return impl_->pause(); }
bool EditPlayer::stop() { return impl_->stop(); }
qint64 EditPlayer::positionMs() const { return impl_->positionMs(); }
qint64 EditPlayer::durationMs() const { return impl_->durationMs(); }
qint64 EditPlayer::frameDurationMs() const { return impl_->frameDurationMs(); }
qint64 EditPlayer::frameStepTargetMs(qint64 positionMs, int frameCount) const
{
    return impl_->frameStepTargetMs(positionMs, frameCount);
}
bool EditPlayer::seek(qint64 positionMs) { return impl_->seek(positionMs); }
bool EditPlayer::seekAccurate(qint64 positionMs)
{
    return impl_->seekAccurate(positionMs);
}
bool EditPlayer::startForwardScan(double speed)
{
    return impl_->startForwardScan(speed);
}
bool EditPlayer::stopForwardScan(bool resumePlayback, bool boundaryReached,
                                 qint64 &positionMs)
{
    return impl_->stopForwardScan(resumePlayback, boundaryReached, positionMs);
}
bool EditPlayer::setNominalDelayFrames(int frames) { return impl_->setNominalDelayFrames(frames); }

bool EditPlayer::setNominalEq(double lowDb, double midDb, double highDb)
{
    return impl_->setNominalEq(lowDb, midDb, highDb);
}

bool EditPlayer::setNominalVolume(double linearGain)
{
    return impl_->setNominalVolume(linearGain);
}
std::shared_ptr<EditPflFeed> EditPlayer::pflFeed() const { return impl_->pflFeed(); }
EditPlayer::AudioLevel EditPlayer::inputAudioLevel() const
{
    return impl_->inputAudioLevel();
}
EditPlayer::AudioLevel EditPlayer::audioLevel() const
{
    return impl_->audioLevel();
}
void EditPlayer::processBusMessages() { impl_->processBusMessages(); }
EditPlayer::Event EditPlayer::takeEvent(QString &detail)
{
    return impl_->takeEvent(detail);
}
