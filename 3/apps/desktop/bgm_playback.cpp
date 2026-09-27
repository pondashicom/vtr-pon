#include "bgm_playback.h"
#include "program_audio_bridge.h"
#include "audio_delay.h"
#include "audio_equalizer.h"

#include <QDebug>
#include <QUrl>

#include <algorithm>

namespace {

constexpr guint kPlayFlagVideo = 1U << 0;
constexpr guint kPlayFlagAudio = 1U << 1;
constexpr guint kPlayFlagText = 1U << 2;
constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;

} // namespace

BgmPlayback::~BgmPlayback()
{
    if (!shutdown()) {
        qWarning() << "Skipping unsafe BGM playback release after"
                      " shutdown could not be confirmed";
        return;
    }
    if (bgmBus_) gst_object_unref(bgmBus_);
    if (bgmInputAudioLevel_) gst_object_unref(bgmInputAudioLevel_);
    if (bgmItemAudioVolume_) gst_object_unref(bgmItemAudioVolume_);
    if (bgmPlaybin_) gst_object_unref(bgmPlaybin_);
}

void BgmPlayback::detachCallbacks()
{
    if (bgmBus_) gst_bus_set_sync_handler(bgmBus_, nullptr, nullptr, nullptr);
}

bool BgmPlayback::shutdown()
{
    detachCallbacks();
    if (shutdownConfirmed_) return true;
    if (!bgmPlaybin_) {
        shutdownConfirmed_ = true;
        return true;
    }
    if (gst_element_set_state(bgmPlaybin_, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "BGM playbin" << "rejected GST_STATE_NULL";
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn result = gst_element_get_state(
        bgmPlaybin_, &current, &pending, kShutdownStateTimeout);
    if (result == GST_STATE_CHANGE_FAILURE || current != GST_STATE_NULL) {
        qWarning() << "BGM playbin" << "did not reach GST_STATE_NULL; current="
                   << gst_element_state_get_name(current) << "pending="
                   << gst_element_state_get_name(pending);
        return false;
    }
    shutdownConfirmed_ = true;
    return true;
}

void BgmPlayback::drainBus()
{
    if (!bgmBus_) return;
    while (GstMessage *message = gst_bus_pop(bgmBus_)) gst_message_unref(message);
}

bool BgmPlayback::initialize(GstBusSyncHandler callback, gpointer userData)
{
    if (bgmPlaybin_) return false;
    if (!vtrpon::registerProgramAudioBridge() || !vtrpon::registerAudioDelay() || !vtrpon::registerAudioEqualizer()) return false;
    bgmPlaybin_ = gst_element_factory_make("playbin", "bgm-player");
    GstElement *videoDiscard =
        gst_element_factory_make("fakesink", "bgm-video-discard");
    if (!bgmPlaybin_ || !videoDiscard) {
        if (videoDiscard) {
            gst_object_unref(videoDiscard);
        }
        return false;
    }
    g_object_set(videoDiscard, "sync", FALSE, "async", FALSE,
                 "enable-last-sample", FALSE, nullptr);

    GError *audioError = nullptr;
    const QString audioDescription = QStringLiteral(
        "audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,"
        "layout=interleaved ! "
        "level name=bgm-input-audio-level post-messages=true "
        "interval=5000000 ! "
        "vtrponaudioequalizer name=bgm-item-eq ! "
        "vtrponaudiodelay name=bgm-item-delay ! "
        "volume name=bgm-item-audio-volume ! "
        "vtrponaudiosink name=bgm-audio-output channel=bgm-audio "
        "sync=true qos=false");
    GstElement *audioOutput = gst_parse_bin_from_description(
        audioDescription.toUtf8().constData(), TRUE, &audioError);
    if (audioError) {
        qWarning() << "Failed to create BGM audio output:"
                   << audioError->message;
        g_error_free(audioError);
    }
    if (!audioOutput) {
        gst_object_unref(videoDiscard);
        return false;
    }

    bgmInputAudioLevel_ = gst_bin_get_by_name(
        GST_BIN(audioOutput), "bgm-input-audio-level");
    bgmItemAudioVolume_ = gst_bin_get_by_name(
        GST_BIN(audioOutput), "bgm-item-audio-volume");
    if (!bgmInputAudioLevel_ || !bgmItemAudioVolume_) {
        gst_object_unref(videoDiscard);
        gst_object_unref(audioOutput);
        return false;
    }

    guint flags = 0;
    g_object_get(bgmPlaybin_, "flags", &flags, nullptr);
    flags = (flags | kPlayFlagAudio) &
            ~(kPlayFlagVideo | kPlayFlagText);
    g_object_set(bgmPlaybin_, "flags", flags,
                 "video-sink", videoDiscard,
                 "audio-sink", audioOutput, nullptr);
    bgmBus_ = gst_element_get_bus(bgmPlaybin_);
    if (!bgmBus_) {
        return false;
    }
    gst_bus_set_sync_handler(bgmBus_, callback, userData, nullptr);
    return true;
}

bool BgmPlayback::load(const QString &filePath, qint64 inPointMs,
             qint64 outPointMs)
{
    shutdownConfirmed_ = false;
    if (!bgmPlaybin_ || filePath.isEmpty() ||
        inPointMs < 0 || (outPointMs >= 0 && outPointMs <= inPointMs)) {
        return false;
    }
    (void)gst_element_set_state(bgmPlaybin_, GST_STATE_READY);
    drainBus();
    const QByteArray uri = QUrl::fromLocalFile(filePath).toEncoded();
    g_object_set(bgmPlaybin_, "uri", uri.constData(), nullptr);
    bgmInPointMs_ = inPointMs;
    bgmOutPointMs_ = outPointMs;
    bgmRangeSeekIssued_ = false;
    bgmHasAudio_ = false;
    bgmErrorDetail_.clear();
    pendingBgmEvent_ = Event::None;
    pendingBgmEventDetail_.clear();
    bgmStatus_ = Status::Loading;
    if (gst_element_set_state(bgmPlaybin_, GST_STATE_PAUSED) ==
        GST_STATE_CHANGE_FAILURE) {
        bgmStatus_ = Status::Error;
        bgmErrorDetail_ = QStringLiteral(
            "Could not prepare the BGM playback pipeline.");
        return false;
    }
    return true;
}

bool BgmPlayback::play()
{
    shutdownConfirmed_ = false;
    refreshStatus();
    if ((bgmStatus_ != Status::Ready &&
         bgmStatus_ != Status::Paused &&
         bgmStatus_ != Status::Stopped)) {
        return bgmStatus_ == Status::Playing;
    }
    if (gst_element_set_state(bgmPlaybin_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    bgmStatus_ = Status::Playing;
    return true;
}

bool BgmPlayback::pause()
{
    shutdownConfirmed_ = false;
    if (!bgmPlaybin_) {
        return false;
    }
    if (bgmStatus_ == Status::Paused ||
        bgmStatus_ == Status::Ready ||
        bgmStatus_ == Status::Stopped) {
        return true;
    }
    if (bgmStatus_ != Status::Playing ||
        gst_element_set_state(bgmPlaybin_, GST_STATE_PAUSED) ==
            GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    bgmStatus_ = Status::Paused;
    return true;
}

BgmPlayback::StopResult BgmPlayback::stop()
{
    shutdownConfirmed_ = false;
    if (!bgmPlaybin_) {
        return StopResult::Failed;
    }
    if (bgmStatus_ == Status::Empty) {
        return StopResult::AlreadyEmpty;
    }
    if (gst_element_set_state(bgmPlaybin_, GST_STATE_PAUSED) ==
        GST_STATE_CHANGE_FAILURE) {
        return StopResult::Failed;
    }
    if (!seek(bgmInPointMs_)) {
        return StopResult::Failed;
    }
    bgmStatus_ = Status::Stopped;
    pendingBgmEvent_ = Event::None;
    pendingBgmEventDetail_.clear();
    return StopResult::Stopped;
}

bool BgmPlayback::clear()
{
    shutdownConfirmed_ = false;
    if (!bgmPlaybin_) {
        return false;
    }
    if (gst_element_set_state(bgmPlaybin_, GST_STATE_READY) ==
        GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    drainBus();
    bgmStatus_ = Status::Empty;
    bgmErrorDetail_.clear();
    bgmInPointMs_ = 0;
    bgmOutPointMs_ = -1;
    bgmRangeSeekIssued_ = false;
    bgmHasAudio_ = false;
    pendingBgmEvent_ = Event::None;
    pendingBgmEventDetail_.clear();
    return true;
}

bool BgmPlayback::seek(qint64 positionMs)
{
    shutdownConfirmed_ = false;
    if (!bgmPlaybin_ ||
        bgmStatus_ == Status::Empty ||
        bgmStatus_ == Status::Loading) {
        return false;
    }
    const qint64 upperBound = bgmOutPointMs_ >= 0
        ? bgmOutPointMs_ : (std::max)(positionMs, bgmInPointMs_);
    const qint64 target = std::clamp(positionMs, bgmInPointMs_,
                                     upperBound);
    const bool succeeded = gst_element_seek(
        bgmPlaybin_, 1.0, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                  GST_SEEK_FLAG_ACCURATE),
        GST_SEEK_TYPE_SET, target * GST_MSECOND,
        bgmOutPointMs_ >= 0 ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
        bgmOutPointMs_ >= 0 ? bgmOutPointMs_ * GST_MSECOND
                            : GST_CLOCK_TIME_NONE);
    if (succeeded) {
        pendingBgmEvent_ = Event::None;
        pendingBgmEventDetail_.clear();
    }
    return succeeded;
}

qint64 BgmPlayback::positionMs() const
{
    if (!bgmPlaybin_ ||
        bgmStatus_ == Status::Empty ||
        bgmStatus_ == Status::Loading) {
        return -1;
    }
    if (bgmStatus_ == Status::Stopped) {
        return bgmInPointMs_;
    }
    gint64 position = GST_CLOCK_TIME_NONE;
    return gst_element_query_position(bgmPlaybin_, GST_FORMAT_TIME,
                                      &position)
        ? position / GST_MSECOND : -1;
}

qint64 BgmPlayback::durationMs() const
{
    if (!bgmPlaybin_ ||
        bgmStatus_ == Status::Empty) {
        return -1;
    }
    gint64 duration = GST_CLOCK_TIME_NONE;
    return gst_element_query_duration(bgmPlaybin_, GST_FORMAT_TIME,
                                      &duration)
        ? duration / GST_MSECOND : -1;
}

BgmPlayback::Status BgmPlayback::status()
{
    refreshStatus();
    return bgmStatus_;
}

void BgmPlayback::refreshStatus()
{
    if (bgmStatus_ != Status::Loading || !bgmPlaybin_) {
        return;
    }
    GstState state = GST_STATE_NULL;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn result = gst_element_get_state(
        bgmPlaybin_, &state, &pending, 0);
    if (result == GST_STATE_CHANGE_FAILURE) {
        bgmStatus_ = Status::Error;
        bgmErrorDetail_ = QStringLiteral(
            "Could not check the BGM preparation state.");
        return;
    }
    if (result != GST_STATE_CHANGE_SUCCESS || state != GST_STATE_PAUSED ||
        pending != GST_STATE_VOID_PENDING) {
        return;
    }

    gint audioStreams = 0;
    g_object_get(bgmPlaybin_, "n-audio", &audioStreams, nullptr);
    bgmHasAudio_ = audioStreams > 0;
    if (!bgmHasAudio_) {
        bgmStatus_ = Status::Error;
        bgmErrorDetail_ = QStringLiteral(
            "No playable audio stream was found in the BGM item.");
        return;
    }

    if (!bgmRangeSeekIssued_) {
        gint64 duration = GST_CLOCK_TIME_NONE;
        if (gst_element_query_duration(bgmPlaybin_, GST_FORMAT_TIME,
                                       &duration) && duration > 0) {
            const qint64 durationMs = duration / GST_MSECOND;
            if (bgmInPointMs_ >= durationMs ||
                (bgmOutPointMs_ >= 0 && bgmOutPointMs_ > durationMs)) {
                bgmStatus_ = Status::Error;
                bgmErrorDetail_ = QStringLiteral(
                    "The BGM In or Out point exceeds the media duration.");
                return;
            }
        }
        if (bgmInPointMs_ == 0 && bgmOutPointMs_ < 0) {
            bgmStatus_ = Status::Ready;
            return;
        }
        const bool seekSucceeded = gst_element_seek(
            bgmPlaybin_, 1.0, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                      GST_SEEK_FLAG_ACCURATE),
            GST_SEEK_TYPE_SET, bgmInPointMs_ * GST_MSECOND,
            bgmOutPointMs_ >= 0 ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
            bgmOutPointMs_ >= 0 ? bgmOutPointMs_ * GST_MSECOND
                                : GST_CLOCK_TIME_NONE);
        if (!seekSucceeded) {
            bgmStatus_ = Status::Error;
            bgmErrorDetail_ = QStringLiteral(
                "Failed to prepare the BGM In/Out points.");
            return;
        }
        bgmRangeSeekIssued_ = true;
        return;
    }

    gint64 position = GST_CLOCK_TIME_NONE;
    if (!gst_element_query_position(bgmPlaybin_, GST_FORMAT_TIME,
                                    &position)) {
        return;
    }
    if (qAbs(position / GST_MSECOND - bgmInPointMs_) <= 250) {
        bgmStatus_ = Status::Ready;
    }
}

void BgmPlayback::pollBus()
{
    if (!bgmBus_) {
        return;
    }
    while (GstMessage *message = gst_bus_pop(bgmBus_)) {
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
        if (type == GST_MESSAGE_LATENCY) {
            if (!gst_bin_recalculate_latency(GST_BIN(bgmPlaybin_))) {
                qWarning() << "Could not update BGM pipeline latency";
            }
        } else if (type == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            qWarning().noquote()
                << "GST_BUS" << (error ? error->message : "Unknown error")
                << (debug ? debug : "");
            const QString detail = QString::fromUtf8(
                error ? error->message : "Unknown GStreamer error");
            g_clear_error(&error);
            g_free(debug);
            bgmErrorDetail_ = detail;
            bgmStatus_ = Status::Error;
            pendingBgmEvent_ = Event::Error;
            pendingBgmEventDetail_ = detail;
        } else if (type == GST_MESSAGE_EOS &&
                   pendingBgmEvent_ != Event::Error) {
            pendingBgmEvent_ = Event::EndOfStream;
        }
        gst_message_unref(message);
    }
}

BgmPlayback::Event BgmPlayback::takeEvent(QString &detail)
{
    const Event event = pendingBgmEvent_;
    detail = pendingBgmEventDetail_;
    pendingBgmEvent_ = Event::None;
    pendingBgmEventDetail_.clear();
    return event;
}

void BgmPlayback::setItemDelayFrames(int frames)
{
    if (!bgmPlaybin_) return;
    GstElement *output = nullptr;
    g_object_get(bgmPlaybin_, "audio-sink", &output, nullptr);
    vtrpon::setAudioDelayFrames(output, "bgm-item-delay", frames);
    if (output) gst_object_unref(output);
}

void BgmPlayback::setItemEq(double lowDb, double midDb, double highDb)
{
    if (!bgmPlaybin_) return;
    GstElement *output = nullptr;
    g_object_get(bgmPlaybin_, "audio-sink", &output, nullptr);
    if (!output) return;
    GstElement *eq = gst_bin_get_by_name(GST_BIN(output), "bgm-item-eq");
    gst_object_unref(output);
    if (!eq) return;
    vtrpon::setAudioEqualizer(eq, lowDb, midDb, highDb);
    gst_object_unref(eq);
}

void BgmPlayback::setItemVolume(double volume)
{
    if (bgmItemAudioVolume_) {
        g_object_set(bgmItemAudioVolume_, "volume",
                     std::clamp(volume, 0.0, 2.0), nullptr);
    }
}
