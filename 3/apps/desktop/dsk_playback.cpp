#include "dsk_playback.h"
#ifdef _WIN32
#include "program_gpu_compositor.h"
#include <QDebug>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>
#include <gst/app/gstappsink.h>
#include <algorithm>

namespace {
constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;
constexpr guint kPlayFlagVideo = 1U << 0;
constexpr guint kPlayFlagAudio = 1U << 1;
constexpr guint kPlayFlagText = 1U << 2;


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
    return hasElementFactory("d3d11convert")
        && hasElementFactory("d3d11upload")
        && hasElementFactory("videoconvert")
        && hasElementFactory("appsink");
}

void setBooleanPropertyIfAvailable(GstElement *element, const char *name,
                                   gboolean value)
{
    if (element
        && g_object_class_find_property(G_OBJECT_GET_CLASS(element), name)) {
        g_object_set(element, name, value, nullptr);
    }
}

} // namespace

DskPlayback::~DskPlayback()
{
    if (!shutdown()) {
        qWarning() << "Retaining DSK producer references after unconfirmed shutdown";
        return;
    }
    for (Dsk &dsk : dsks_) releaseDsk(dsk);
}

bool DskPlayback::initialize()
{
    if (initialized_ || shutdownRequested_) return false;
    for (int index = 0; index < static_cast<int>(dsks_.size()); ++index) {
        if (dsks_[index].playbin || !initializeDsk(index)) return false;
    }
    initialized_ = true;
    return true;
}

GstElement *DskPlayback::pipeline(int index) const
{
    return index >= 0 && index < static_cast<int>(dsks_.size()) ? dsks_[index].playbin : nullptr;
}

GstElement *DskPlayback::appSink(int index) const
{
    return index >= 0 && index < static_cast<int>(dsks_.size()) ? dsks_[index].appSink : nullptr;
}

void DskPlayback::attachCompositor(ProgramGpuCompositor *compositor)
{
    programGpuCompositor_ = compositor;
}

void DskPlayback::detachCallbacks()
{
    // Appsink callbacks belong to the compositor and are detached by its owner.
    for (Dsk &dsk : dsks_) {
        if (dsk.bus) gst_bus_set_sync_handler(dsk.bus, nullptr, nullptr, nullptr);
    }
}

bool DskPlayback::retirementPending() const
{
    for (const Dsk &dsk : dsks_) if (dsk.stopJob && !dsk.stopJob->done()) return true;
    return false;
}

bool DskPlayback::shutdown()
{
    shutdownRequested_ = true;
    detachCallbacks();
    if (shutdownConfirmed_) return true;
    bool confirmed = true;
    for (Dsk &dsk : dsks_) {
        dsk.pendingPath.clear();
        if (dsk.stopJob) confirmed = dsk.stopJob->succeeded() && confirmed;
        else confirmed = stopPipeline(dsk.playbin, "DSK playbin") && confirmed;
    }
    shutdownConfirmed_ = confirmed;
    return confirmed;
}


bool DskPlayback::cue(const QString &filePath, qint64 inPointMs,
            qint64 outPointMs)
{
    if (!initialized_ || shutdownRequested_ || filePath.isEmpty() || !programGpuCompositor_) {
        return false;
    }
    if (inPointMs < 0
        || (outPointMs >= 0 && outPointMs <= inPointMs)) {
        pendingDskEvent_ = DskEvent::CueError;
        pendingDskEventDetail_ = QStringLiteral(
            "The DSK In or Out point is invalid.");
        return false;
    }
    if (dskCrossfadeActive_) return false;
    if (!clearCue()) {
        return false;
    }

    dskCueDeck_ = dskActiveDeck_ == 0 ? 1 : 0;
    Dsk &dsk = dsks_[dskCueDeck_];
    if (!resetDskDeck(dskCueDeck_)) return false;
    dsk.pendingPath = filePath;
    dsk.inPointMs = inPointMs;
    dsk.outPointMs = outPointMs;
    dsk.status = DskStatus::Loading;
    pendingDskEvent_ = DskEvent::None;
    pendingDskEventDetail_.clear();
    refreshDskStatus();
    return dsk.status != DskStatus::Error;
}

bool DskPlayback::startPendingCue(int dskIndex)
{
    Dsk &dsk = dsks_[dskIndex];
    const QString filePath = std::move(dsk.pendingPath);
    dsk.pendingPath.clear();
    const auto inPointMs = dsk.inPointMs;
    const auto outPointMs = dsk.outPointMs;
    if (!enforceDskAudioIsolation(dsk.playbin)) {
        setDskError(dskIndex, QStringLiteral("DSK audio isolation could not be enforced."), true);
        return false;
    }
    drainBus(dsk.bus);
    programGpuCompositor_->clearDskFrame(dskIndex);

    const QMimeType mimeType = QMimeDatabase().mimeTypeForFile(
        filePath, QMimeDatabase::MatchContent);
    dsk.isStillImage = mimeType.name().startsWith(
        QStringLiteral("image/"));
    dsk.inPointMs = dsk.isStillImage ? 0 : inPointMs;
    dsk.outPointMs = dsk.isStillImage ? -1 : outPointMs;
    dsk.rangeSeekIssued = dsk.isStillImage
        || (dsk.inPointMs == 0 && dsk.outPointMs < 0);
    GstElement *imageFreeze = nullptr;
    if (dsk.isStillImage) {
        imageFreeze = gst_element_factory_make(
            "imagefreeze", "dsk-image-freeze");
        if (!imageFreeze) {
            setDskError(dskIndex, QStringLiteral(
                "Could not create imagefreeze for the still-image DSK."),
                true);
            return false;
        }
        gst_object_ref_sink(imageFreeze);
    }
    g_object_set(dsk.playbin, "video-filter", imageFreeze, nullptr);
    if (imageFreeze) {
        gst_object_unref(imageFreeze);
    }

    const QByteArray uri = QUrl::fromLocalFile(filePath).toEncoded();
    g_object_set(dsk.playbin, "uri", uri.constData(), nullptr);
    dsk.status = DskStatus::Loading;
    dsk.errorDetail.clear();
    pendingDskEvent_ = DskEvent::None;
    pendingDskEventDetail_.clear();
    if (gst_element_set_state(dsk.playbin, GST_STATE_PAUSED)
        == GST_STATE_CHANGE_FAILURE) {
        if (dsk.status != DskStatus::Error) {
            setDskError(dskIndex, QStringLiteral(
                "Could not transition the DSK media to PAUSED."), true);
        }
        return false;
    }
    return true;
}

bool DskPlayback::takeCue(bool playOnShow, bool crossfade,
                float videoOpacity)
{
    if (!initialized_ || shutdownRequested_ || !programGpuCompositor_
        || dskCueDeck_ < 0
        || dskCueDeck_ >= static_cast<int>(dsks_.size())) {
        return false;
    }
    Dsk &incoming = dsks_[dskCueDeck_];
    if (incoming.status != DskStatus::Ready
        || !programGpuCompositor_->dskFrameReady(dskCueDeck_)) {
        return false;
    }

    if (!incoming.isStillImage && playOnShow) {
        if (gst_element_set_state(incoming.playbin, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
            setDskError(dskCueDeck_, QStringLiteral(
                "Could not play the DSK media."), true);
            return false;
        }
    }

    const int incomingDeck = dskCueDeck_;
    const int outgoingDeck = dskActiveDeck_;
    dskCueDeck_ = -1;
    dskActiveDeck_ = incomingDeck;
    incoming.status = DskStatus::On;
    incoming.errorDetail.clear();
    incoming.playing = playOnShow;
    videoOpacity = std::clamp(videoOpacity, 0.0F, 1.0F);
    dskCrossfadeActive_ = crossfade && outgoingDeck >= 0
        && outgoingDeck != incomingDeck;
    dskCrossfadeIncomingDeck_ = dskCrossfadeActive_ ? incomingDeck : -1;
    dskCrossfadeOutgoingDeck_ = dskCrossfadeActive_ ? outgoingDeck : -1;
    dskCrossfadeProgress_ = dskCrossfadeActive_ ? 0.0F : 1.0F;
    dskCrossfadeOutgoingStartOpacity_ = dskCrossfadeActive_
        ? dskCurrentOpacity_ : 0.0F;
    dskCurrentOpacity_ = videoOpacity;

    if (!applyDskLayers()) {
        (void)gst_element_set_state(incoming.playbin, GST_STATE_PAUSED);
        setDskError(incomingDeck, QStringLiteral(
            "Could not display the DSK GPU frame."), false);
        return false;
    }
    if (!dskCrossfadeActive_ && outgoingDeck >= 0
        && outgoingDeck != incomingDeck) {
        resetDskDeck(outgoingDeck);
    }
    return true;
}

bool DskPlayback::crossfadeActive() const
{
    return dskCrossfadeActive_;
}

bool DskPlayback::setCrossfadeProgress(float progress)
{
    if (!initialized_ || shutdownRequested_ || !dskCrossfadeActive_) {
        return false;
    }
    dskCrossfadeProgress_ = std::clamp(progress, 0.0F, 1.0F);
    if (!applyDskLayers()) {
        return false;
    }
    if (dskCrossfadeProgress_ >= 1.0F) {
        const int outgoingDeck = dskCrossfadeOutgoingDeck_;
        dskCrossfadeActive_ = false;
        dskCrossfadeIncomingDeck_ = -1;
        dskCrossfadeOutgoingDeck_ = -1;
        dskCrossfadeOutgoingStartOpacity_ = 0.0F;
        resetDskDeck(outgoingDeck);
    }
    return true;
}

bool DskPlayback::setVideoOpacity(float opacity)
{
    if (!initialized_ || shutdownRequested_ || dskActiveDeck_ < 0) {
        return false;
    }
    dskCurrentOpacity_ = std::clamp(opacity, 0.0F, 1.0F);
    return applyDskLayers();
}

bool DskPlayback::clearCue()
{
    if (!initialized_ || shutdownRequested_ || !programGpuCompositor_) {
        return false;
    }
    if (dskCueDeck_ < 0) {
        return true;
    }
    const int cueDeck = dskCueDeck_;
    dskCueDeck_ = -1;
    return resetDskDeck(cueDeck);
}

bool DskPlayback::clear()
{
    if (!initialized_ || shutdownRequested_ || !programGpuCompositor_) {
        return false;
    }
    if (!programGpuCompositor_->setDskLayers(-1, 0.0F, -1, 0.0F)) return false;
    bool cleared = true;
    for (int index = 0; index < static_cast<int>(dsks_.size()); ++index) {
        cleared = resetDskDeck(index) && cleared;
    }
    if (!cleared) {
        return false;
    }
    dskActiveDeck_ = -1;
    dskCueDeck_ = -1;
    dskCrossfadeActive_ = false;
    dskCrossfadeIncomingDeck_ = -1;
    dskCrossfadeOutgoingDeck_ = -1;
    dskCrossfadeProgress_ = 1.0F;
    dskCrossfadeOutgoingStartOpacity_ = 0.0F;
    dskCurrentOpacity_ = 0.0F;
    pendingDskEvent_ = DskEvent::None;
    pendingDskEventDetail_.clear();
    return true;
}

bool DskPlayback::play()
{
    if (!initialized_ || shutdownRequested_ || dskActiveDeck_ < 0
        || dsks_[dskActiveDeck_].status != DskStatus::On) {
        return false;
    }
    Dsk &dsk = dsks_[dskActiveDeck_];
    if (dsk.playing) return true;
    // Still-image pipelines hold their original frame; their duration is
    // advanced by the controller's logical clock, never by imagefreeze.
    if (!dsk.isStillImage
        && gst_element_set_state(dsk.playbin, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        setDskError(dskActiveDeck_, QStringLiteral(
            "Could not play the DSK media."), false);
        return false;
    }
    dsk.playing = true;
    return true;
}

bool DskPlayback::pause()
{
    if (!initialized_ || shutdownRequested_ || dskActiveDeck_ < 0
        || dsks_[dskActiveDeck_].status != DskStatus::On) {
        return false;
    }
    Dsk &dsk = dsks_[dskActiveDeck_];
    if (gst_element_set_state(dsk.playbin, GST_STATE_PAUSED)
            == GST_STATE_CHANGE_FAILURE) {
        setDskError(dskActiveDeck_, QStringLiteral(
            "Could not pause the DSK media."), false);
        return false;
    }
    dsk.playing = false;
    return true;
}

bool DskPlayback::isPlaying() const
{
    return initialized_ && !shutdownRequested_ && dskActiveDeck_ >= 0
        && dsks_[dskActiveDeck_].status == DskStatus::On
        && dsks_[dskActiveDeck_].playing;
}

bool DskPlayback::pauseAtEnd()
{
    return pause();
}

bool DskPlayback::restartFromIn(bool play)
{
    if (!initialized_ || shutdownRequested_ || dskActiveDeck_ < 0) {
        return false;
    }
    // Retire the previous iteration's EOS before seeking. Do not drain away
    // errors: they must remain available to the controller's error handler.
    pollBus();
    Dsk &dsk = dsks_[dskActiveDeck_];
    if (dsk.status != DskStatus::On) return false;
    if (dsk.isStillImage) {
        dsk.playing = play;
        return true;
    }
    if (!seekDskRange(dskActiveDeck_, static_cast<GstSeekFlags>(
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE))) {
        setDskError(dskActiveDeck_, QStringLiteral(
            "Could not return the DSK media to its In point."), false);
        return false;
    }
    const GstState target = play ? GST_STATE_PLAYING : GST_STATE_PAUSED;
    if (gst_element_set_state(dsk.playbin, target)
        == GST_STATE_CHANGE_FAILURE) {
        setDskError(dskActiveDeck_,
            play ? QStringLiteral("Could not start repeated DSK playback.")
                 : QStringLiteral("Could not stop the DSK media at its In point."),
            false);
        return false;
    }
    if (pendingDskEvent_ == DskEvent::EndOfStream) {
        pendingDskEvent_ = DskEvent::None;
        pendingDskEventDetail_.clear();
    }
    dsk.playing = play;
    return true;
}

DskPlayback::DskStatus DskPlayback::status()
{
    refreshDskStatus();
    if (dskActiveDeck_ >= 0) {
        return dsks_[dskActiveDeck_].status == DskStatus::Error
            ? DskStatus::Error : DskStatus::On;
    }
    return dskCueDeck_ >= 0 ? dsks_[dskCueDeck_].status
                            : DskStatus::Empty;
}

DskPlayback::DskStatus DskPlayback::cueStatus()
{
    refreshDskStatus();
    return dskCueDeck_ >= 0 ? dsks_[dskCueDeck_].status
                            : DskStatus::Empty;
}

qint64 DskPlayback::positionMs() const
{
    return queryDskTime(dskActiveDeck_, GST_QUERY_POSITION);
}

qint64 DskPlayback::durationMs() const
{
    return queryDskTime(dskActiveDeck_, GST_QUERY_DURATION);
}

QString DskPlayback::errorDetail() const
{
    if (dskCueDeck_ >= 0 && !dsks_[dskCueDeck_].errorDetail.isEmpty()) {
        return dsks_[dskCueDeck_].errorDetail;
    }
    return dskActiveDeck_ >= 0 ? dsks_[dskActiveDeck_].errorDetail
                               : QString();
}

DskPlayback::DskEvent DskPlayback::takeEvent(QString &detail)
{
    const DskEvent result = pendingDskEvent_;
    detail = pendingDskEventDetail_;
    pendingDskEvent_ = DskEvent::None;
    pendingDskEventDetail_.clear();
    return result;
}

void DskPlayback::pollBus()
{
    refreshDskStatus();
    for (int index = 0; index < static_cast<int>(dsks_.size()); ++index) {
        Dsk &dsk = dsks_[index];
        if (!dsk.bus || dsk.stopJob || dsk.status == DskStatus::Empty) {
            continue;
        }
        while (GstMessage *message = gst_bus_pop(dsk.bus)) {
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
                    << "GST_BUS"
                    << (error ? error->message : "Unknown error")
                    << (debug ? debug : "");
                const QString detail = QString::fromUtf8(
                    error ? error->message
                          : "Unknown DSK GStreamer error");
                g_clear_error(&error);
                g_free(debug);
                setDskError(index, detail, index == dskCueDeck_);
            } else if (type == GST_MESSAGE_EOS) {
                if (index == dskActiveDeck_
                    && dsk.status == DskStatus::On) {
                    dsk.playing = false;
                    pendingDskEvent_ = DskEvent::EndOfStream;
                    pendingDskEventDetail_.clear();
                } else if (index == dskCueDeck_
                           && dsk.status == DskStatus::Loading) {
                    setDskError(index, QStringLiteral(
                        "Could not retrieve video frames from the DSK media."),
                        true);
                }
            }
            gst_message_unref(message);
        }
    }
    refreshDskStatus();
}

void DskPlayback::setDskError(int dskIndex, const QString &detail, bool cueError)
{
    if (dskIndex < 0 || dskIndex >= static_cast<int>(dsks_.size())) {
        return;
    }
    Dsk &dsk = dsks_[dskIndex];
    dsk.status = DskStatus::Error;
    dsk.playing = false;
    dsk.errorDetail = detail;
    const bool outgoingTransitionError = !cueError
        && dskIndex != dskActiveDeck_
        && dskIndex == dskCrossfadeOutgoingDeck_;
    if (programGpuCompositor_) {
        programGpuCompositor_->clearDskFrame(dskIndex);
        if (outgoingTransitionError) {
            dskCrossfadeActive_ = false;
            dskCrossfadeIncomingDeck_ = -1;
            dskCrossfadeOutgoingDeck_ = -1;
            dskCrossfadeProgress_ = 1.0F;
            dskCrossfadeOutgoingStartOpacity_ = 0.0F;
            (void)applyDskLayers();
        } else if (!cueError) {
            (void)programGpuCompositor_->setDskLayers(
                -1, 0.0F, -1, 0.0F);
        }
    }
    pendingDskEvent_ = cueError || outgoingTransitionError
        ? DskEvent::CueError : DskEvent::Error;
    pendingDskEventDetail_ = detail;
}

bool DskPlayback::seekDskRange(int dskIndex, GstSeekFlags flags)
{
    if (dskIndex < 0 || dskIndex >= static_cast<int>(dsks_.size())) {
        return false;
    }
    Dsk &dsk = dsks_[dskIndex];
    if (!dsk.playbin) {
        return false;
    }
    return gst_element_seek(
        dsk.playbin, 1.0, GST_FORMAT_TIME, flags,
        GST_SEEK_TYPE_SET, dsk.inPointMs * GST_MSECOND,
        dsk.outPointMs >= 0 ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
        dsk.outPointMs >= 0
            ? dsk.outPointMs * GST_MSECOND
            : GST_CLOCK_TIME_NONE);
}

qint64 DskPlayback::queryDskTime(int dskIndex, GstQueryType type) const
{
    if (dskIndex < 0 || dskIndex >= static_cast<int>(dsks_.size())
        || !dsks_[dskIndex].playbin) {
        return -1;
    }
    gint64 value = GST_CLOCK_TIME_NONE;
    const bool succeeded = type == GST_QUERY_POSITION
        ? gst_element_query_position(dsks_[dskIndex].playbin,
                                     GST_FORMAT_TIME, &value)
        : gst_element_query_duration(dsks_[dskIndex].playbin,
                                     GST_FORMAT_TIME, &value);
    return succeeded && GST_CLOCK_TIME_IS_VALID(value)
        ? value / GST_MSECOND : -1;
}

bool DskPlayback::applyDskLayers()
{
    if (!programGpuCompositor_) {
        return false;
    }
    if (dskCrossfadeActive_) {
        return programGpuCompositor_->setDskLayers(
            dskCrossfadeIncomingDeck_,
            dskCrossfadeProgress_ * dskCurrentOpacity_,
            dskCrossfadeOutgoingDeck_,
            (1.0F - dskCrossfadeProgress_)
                * dskCrossfadeOutgoingStartOpacity_);
    }
    return programGpuCompositor_->setDskLayers(
        dskActiveDeck_, dskActiveDeck_ >= 0 ? dskCurrentOpacity_ : 0.0F,
        -1, 0.0F);
}

bool DskPlayback::resetDskDeck(int dskIndex)
{
    if (dskIndex < 0 || dskIndex >= static_cast<int>(dsks_.size())) {
        return true;
    }
    Dsk &dsk = dsks_[dskIndex];
    dsk.pendingPath.clear();
    if (dsk.stopJob) return !dsk.stopJob->done() || dsk.stopJob->succeeded();
    if (dsk.status == DskStatus::Empty) return true;
    if (programGpuCompositor_) programGpuCompositor_->clearDskFrame(dskIndex);
    auto *pipeline = dsk.playbin;
    auto *bus = dsk.bus;
    dsk.stopJob = AsyncPipelineStop::start(pipeline, [pipeline, bus] {
        drainBus(bus);
        g_object_set(pipeline, "video-filter", nullptr, "uri", nullptr, nullptr);
    });
    if (dsk.stopJob->done() && !dsk.stopJob->succeeded()) {
        setDskError(dskIndex, QStringLiteral("Could not start the DSK stop worker."), true);
        return false;
    }
    dsk.status = DskStatus::Empty;
    dsk.errorDetail.clear();
    dsk.isStillImage = false;
    dsk.playing = false;
    dsk.rangeSeekIssued = false;
    dsk.inPointMs = 0;
    dsk.outPointMs = -1;
    return true;
}

void DskPlayback::finishRetirements()
{
    for (int index = 0; index < static_cast<int>(dsks_.size()); ++index) {
        auto &dsk = dsks_[index];
        if (dsk.stopJob) {
            if (!dsk.stopJob->done()) continue;
            if (!dsk.stopJob->succeeded()) {
                dsk.pendingPath.clear();
                if (dsk.status != DskStatus::Error)
                    setDskError(index, QStringLiteral("Could not stop the DSK pipeline."), true);
                continue; // Keep the deck unavailable and its resources retained.
            }
            dsk.stopJob.reset();
            if (programGpuCompositor_) programGpuCompositor_->clearDskFrame(index);
        }
        if (!shutdownRequested_ && !dsk.pendingPath.isEmpty()) (void)startPendingCue(index);
    }
}

void DskPlayback::refreshDskStatus()
{
    finishRetirements();
    if (programGpuCompositor_
        && programGpuCompositor_->takeDskRenderError()) {
        const QString detail = QStringLiteral(
            "GPU composition of the DSK frame failed. Base video will continue.");
        if (dskActiveDeck_ >= 0) {
            (void)gst_element_set_state(
                dsks_[dskActiveDeck_].playbin, GST_STATE_PAUSED);
            setDskError(dskActiveDeck_, detail, false);
        } else if (dskCueDeck_ >= 0) {
            setDskError(dskCueDeck_, detail, true);
        }
        return;
    }
    if (dskCueDeck_ < 0 || !programGpuCompositor_) {
        return;
    }
    const int cueDeck = dskCueDeck_;
    Dsk &dsk = dsks_[cueDeck];
    if (dsk.stopJob || !dsk.pendingPath.isEmpty() || dsk.status != DskStatus::Loading) {
        return;
    }

    GstState state = GST_STATE_NULL;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn stateResult = gst_element_get_state(
        dsk.playbin, &state, &pending, 0);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        const QString detail = QStringLiteral(
            "Could not check the DSK ready state.");
        setDskError(cueDeck, detail, true);
        return;
    }
    if (stateResult != GST_STATE_CHANGE_SUCCESS
        || state != GST_STATE_PAUSED
        || pending != GST_STATE_VOID_PENDING) {
        return;
    }

    if (!dsk.rangeSeekIssued) {
        gint64 duration = GST_CLOCK_TIME_NONE;
        if (gst_element_query_duration(dsk.playbin, GST_FORMAT_TIME,
                                       &duration)
            && duration > 0) {
            const qint64 durationMs = duration / GST_MSECOND;
            if (dsk.inPointMs >= durationMs
                || (dsk.outPointMs >= 0
                    && dsk.outPointMs > durationMs)) {
                const QString detail = QStringLiteral(
                    "The DSK In or Out point exceeds the media duration.");
                setDskError(cueDeck, detail, true);
                return;
            }
        }
        programGpuCompositor_->clearDskFrame(cueDeck);
        if (!seekDskRange(cueDeck, static_cast<GstSeekFlags>(
                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE))) {
            const QString detail = QStringLiteral(
                "Could not prepare the DSK item at its In point.");
            setDskError(cueDeck, detail, true);
            return;
        }
        dsk.rangeSeekIssued = true;
        return;
    }

    if (!programGpuCompositor_->dskFrameReady(cueDeck)) {
        return;
    }
    if (!dsk.isStillImage
        && (dsk.inPointMs != 0 || dsk.outPointMs >= 0)) {
        gint64 position = GST_CLOCK_TIME_NONE;
        if (!gst_element_query_position(dsk.playbin, GST_FORMAT_TIME,
                                        &position)
            || qAbs(position / GST_MSECOND - dsk.inPointMs) > 250) {
            return;
        }
    }
    dsk.status = DskStatus::Ready;
}

GstElement *DskPlayback::makeDskVideoOutput(int index)
{
    if (!canUseD3d11VideoPath()) {
        qWarning() << "Required D3D11 DSK elements are unavailable";
        return nullptr;
    }

    const QString sinkName = QStringLiteral("dsk-%1-video-appsink").arg(index);
    const QString description = QStringLiteral(
        "queue max-size-buffers=2 "
        "max-size-bytes=0 max-size-time=0 ! "
        "videoconvert ! video/x-raw,format=BGRA ! "
        "d3d11upload ! "
        "video/x-raw(memory:D3D11Memory),format=BGRA ! "
        "appsink name=%1 max-buffers=2 "
        "leaky-type=downstream wait-on-eos=false sync=true "
        "qos=false enable-last-sample=false").arg(sinkName);
    GError *parseError = nullptr;
    GstElement *videoOutput = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &parseError);
    if (parseError) {
        qWarning() << "Failed to create DSK D3D11 appsink output:"
                   << parseError->message;
        g_error_free(parseError);
    }
    if (!videoOutput) {
        return nullptr;
    }

    Dsk &dsk = dsks_[index];
    const QByteArray sinkNameUtf8 = sinkName.toUtf8();
    dsk.appSink = gst_bin_get_by_name(
        GST_BIN(videoOutput), sinkNameUtf8.constData());
    if (!dsk.appSink || !GST_IS_APP_SINK(dsk.appSink)) {
        if (dsk.appSink) {
            gst_object_unref(dsk.appSink);
            dsk.appSink = nullptr;
        }
        gst_object_unref(videoOutput);
        return nullptr;
    }
    return videoOutput;
}

bool DskPlayback::enforceDskAudioIsolation(GstElement *playbin)
{
    if (!playbin) {
        return false;
    }

    guint flags = 0;
    g_object_get(playbin, "flags", &flags, nullptr);
    flags = (flags | kPlayFlagVideo)
        & ~(kPlayFlagAudio | kPlayFlagText);
    g_object_set(playbin,
                 "flags", flags,
                 "volume", 0.0,
                 "mute", TRUE,
                 nullptr);

    guint appliedFlags = 0;
    gdouble appliedVolume = 1.0;
    gboolean appliedMute = FALSE;
    GstElement *audioSink = nullptr;
    g_object_get(playbin,
                 "flags", &appliedFlags,
                 "volume", &appliedVolume,
                 "mute", &appliedMute,
                 "audio-sink", &audioSink,
                 nullptr);
    const GstElementFactory *factory = audioSink
        ? gst_element_get_factory(audioSink) : nullptr;
    const gchar *factoryName = factory
        ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
        : nullptr;
    const bool discardSink = g_strcmp0(factoryName, "fakesink") == 0;
    if (audioSink) {
        gst_object_unref(audioSink);
    }
    return (appliedFlags & kPlayFlagAudio) == 0U
        && appliedVolume == 0.0 && appliedMute && discardSink;
}

bool DskPlayback::initializeDsk(int index)
{
    Dsk &dsk = dsks_[index];
    const QByteArray playerName =
        QStringLiteral("dsk-%1-player").arg(index).toUtf8();
    dsk.playbin = gst_element_factory_make("playbin", playerName.constData());
    GstElement *videoOutput = makeDskVideoOutput(index);
    const QByteArray discardSinkName =
        QStringLiteral("dsk-%1-audio-discard").arg(index).toUtf8();
    GstElement *audioDiscard = gst_element_factory_make(
        "fakesink", discardSinkName.constData());
    if (!dsk.playbin || !videoOutput || !audioDiscard) {
        if (videoOutput) {
            gst_object_unref(videoOutput);
        }
        if (audioDiscard) {
            gst_object_unref(audioDiscard);
        }
        return false;
    }

    // DSK is a video-only key/fill layer. It must never enter the Program
    // audio graph or inherit ITEM/master fader values. Audio selection is
    // disabled; mute/zero volume and a discard sink are independent safety
    // barriers if the playbin flags are changed accidentally later.
    g_object_set(audioDiscard,
                 "sync", FALSE,
                 "async", FALSE,
                 "enable-last-sample", FALSE,
                 nullptr);
    guint flags = 0;
    g_object_get(dsk.playbin, "flags", &flags, nullptr);
    flags = (flags | kPlayFlagVideo)
        & ~(kPlayFlagAudio | kPlayFlagText);
    g_object_set(dsk.playbin,
                 "flags", flags,
                 "video-sink", videoOutput,
                 "audio-sink", audioDiscard,
                 "volume", 0.0,
                 "mute", TRUE,
                 nullptr);
    // playbin owns the floating sink reference, matching videoOutput.

    if (!enforceDskAudioIsolation(dsk.playbin)) {
        qWarning() << "DSK audio isolation could not be enforced";
        return false;
    }
    dsk.bus = gst_element_get_bus(dsk.playbin);
    return dsk.bus != nullptr;
}

void DskPlayback::releaseDsk(Dsk &dsk)
{
    if (dsk.bus) {
        gst_object_unref(dsk.bus);
        dsk.bus = nullptr;
    }
    if (dsk.playbin) {
        gst_object_unref(dsk.playbin);
        dsk.playbin = nullptr;
    }
    if (dsk.appSink) {
        gst_object_unref(dsk.appSink);
        dsk.appSink = nullptr;
    }
}

bool DskPlayback::stopPipeline(GstElement *pipeline, const char *description)
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

void DskPlayback::drainBus(GstBus *bus)
{
    if (!bus) {
        return;
    }
    while (GstMessage *message = gst_bus_pop(bus)) {
        gst_message_unref(message);
    }
}

#endif
