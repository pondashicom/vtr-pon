#include "decklink.h"
#include "program_audio_output.h"
#include "program_audio_bridge.h"
#include "audio_delay.h"
#include "audio_effects.h"
#include "audio_equalizer.h"
#include "network_video.h"

#include <QDebug>
#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace {

constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;

} // namespace

struct ProgramAudioOutput::BusCallbacks {
    std::mutex mutex;
    QObject *context = nullptr;
    ProgramAudioOutput *owner = nullptr;
    GstBusSyncHandler callback = nullptr;
    gpointer userData = nullptr;
    bool latencyQueued = false;
};

struct ProgramAudioOutput::DeviceChange {
    std::atomic_bool done{false};
    bool succeeded = false;
    bool outputReady = false;
    QString id;
};

ProgramAudioOutput::~ProgramAudioOutput()
{
    if (!stop()) {
        qWarning() << "Skipping unsafe Program audio output release after"
                      " shutdown could not be confirmed";
        return;
    }

    if (programBus_) {
        gst_object_unref(programBus_);
    }
    for (GstElement *element : {bgmControlVolume_, bgmManualFadeVolume_,
                               bgmTransportVolume_, programAudioVolume_,
                               programAudioLevel_, programAudioEffects_, bgmPostFaderAudioLevel_}) {
        if (element) {
            gst_object_unref(element);
        }
    }
    for (GstElement *effects : deckEffects_) {
        if (effects) gst_object_unref(effects);
    }
    for (GstElement *level : programItemAudioLevels_) {
        if (level) {
            gst_object_unref(level);
        }
    }
    for (GstElement *volume : takeVolumes_) {
        if (volume) {
            gst_object_unref(volume);
        }
    }
    if (programOutput_) {
        gst_object_unref(programOutput_);
    }
}

bool ProgramAudioOutput::initialize(GstBusSyncHandler callback, gpointer userData,
                                    const QString &initialDeviceId,
                                    const char *sinkFactory)
{
    if (programOutput_ || shutdownRequested_) {
        return false;
    }
    if (!vtrpon::registerProgramAudioBridge() || !vtrpon::registerAudioDelay() || !vtrpon::registerAudioEffects()) {
        qWarning() << "Failed to register the PROGRAM audio transport";
        return false;
    }
    if (!sinkFactory) {
#ifdef _WIN32
        sinkFactory = "wasapi2sink";
#else
        sinkFactory = "autoaudiosink";
#endif
    }
    const QString description = QStringLiteral(
        "audiomixer name=program-audio-mixer "
        "force-live=true ignore-inactive-pads=true ! "
        "audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
        "vtrponaudioeffects name=program-audio-effects slot=master ! "
        "vtrponaudiodelay name=program-audio-delay ! "
        "volume name=program-audio-volume ! "
        "level name=program-audio-level post-messages=true "
        "interval=5000000 ! "
        "%1 name=program-audio-output "
        "vtrponaudiosrc name=deck-a-audio-source channel=deck-a-audio "
        "buffer-time=200000000 latency-time=10000000 ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,"
        "layout=interleaved ! "
        "vtrponaudioeffects name=deck-a-item-effects slot=item ! "
        "vtrponaudiodelay name=deck-a-item-delay ! "
        "level name=deck-a-item-audio-level post-messages=true "
        "interval=5000000 ! "
        "volume name=deck-a-take-volume volume=0.0 ! "
        "program-audio-mixer. "
        "vtrponaudiosrc name=deck-b-audio-source channel=deck-b-audio "
        "buffer-time=200000000 latency-time=10000000 ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,"
        "layout=interleaved ! "
        "vtrponaudioeffects name=deck-b-item-effects slot=item ! "
        "vtrponaudiodelay name=deck-b-item-delay ! "
        "level name=deck-b-item-audio-level post-messages=true "
        "interval=5000000 ! "
        "volume name=deck-b-take-volume volume=0.0 ! "
        "program-audio-mixer. "
        "vtrponaudiosrc name=bgm-audio-source channel=bgm-audio "
        "buffer-time=200000000 latency-time=10000000 ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,"
        "layout=interleaved ! "
        "volume name=bgm-control-volume volume=1.0 ! "
        "volume name=bgm-manual-fade-volume volume=1.0 ! "
        "volume name=bgm-transport-volume volume=1.0 ! "
        "level name=bgm-post-fader-audio-level post-messages=true "
        "interval=5000000 ! "
        "program-audio-mixer.").arg(QString::fromLatin1(sinkFactory));

    GError *parseError = nullptr;
    programOutput_ = gst_parse_launch(description.toUtf8().constData(),
                                      &parseError);
    if (parseError) {
        qWarning() << "Failed to create dual-deck Program output:"
                   << parseError->message;
        g_error_free(parseError);
    }
    if (!programOutput_ || !GST_IS_PIPELINE(programOutput_)) {
        return false;
    }

    requestedProgramDevice_ = initialDeviceId;
    appliedProgramDevice_ = requestedProgramDevice_;
    if (GstElement *sink = gst_bin_get_by_name(GST_BIN(programOutput_),
                                              "program-audio-output")) {
        const auto id = appliedProgramDevice_.toUtf8();
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "device")) {
            g_object_set(sink, "device", id.isEmpty() ? nullptr : id.constData(),
                         nullptr);
        }
        gst_object_unref(sink);
    }

    takeVolumes_[0] = gst_bin_get_by_name(
        GST_BIN(programOutput_), "deck-a-take-volume");
    deckEffects_[0] = gst_bin_get_by_name(GST_BIN(programOutput_), "deck-a-item-effects");
    deckEffects_[1] = gst_bin_get_by_name(GST_BIN(programOutput_), "deck-b-item-effects");
    takeVolumes_[1] = gst_bin_get_by_name(
        GST_BIN(programOutput_), "deck-b-take-volume");
    programItemAudioLevels_[0] = gst_bin_get_by_name(
        GST_BIN(programOutput_), "deck-a-item-audio-level");
    programItemAudioLevels_[1] = gst_bin_get_by_name(
        GST_BIN(programOutput_), "deck-b-item-audio-level");
    programAudioVolume_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "program-audio-volume");
    programAudioEffects_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "program-audio-effects");
    programAudioLevel_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "program-audio-level");
    if (programAudioLevel_) {
        auto *pad = gst_element_get_static_pad(programAudioLevel_, "src");
        if (pad) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *pad, GstPadProbeInfo *info, gpointer pipeline) -> GstPadProbeReturn {
                    auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                    qint64 timecode = vtrpon::networkVideoTimecode();
                    // The audio mixer can produce buffers ahead of the sink.
                    // Map their scheduled running time onto the common NDI clock
                    // instead of stamping them at the probe's arrival time.
                    auto *event = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
                    auto *clock = gst_element_get_clock(GST_ELEMENT(pipeline));
                    if (event && clock && GST_BUFFER_PTS_IS_VALID(buffer)) {
                        const GstSegment *segment = nullptr;
                        gst_event_parse_segment(event, &segment);
                        const auto running = segment && segment->format == GST_FORMAT_TIME
                            ? gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer))
                            : GST_CLOCK_TIME_NONE;
                        const auto base = gst_element_get_base_time(GST_ELEMENT(pipeline));
                        if (GST_CLOCK_TIME_IS_VALID(running) && GST_CLOCK_TIME_IS_VALID(base))
                            timecode += (qint64(base + running) - qint64(gst_clock_get_time(clock))) / 100;
                    }
                    if (event) gst_event_unref(event);
                    if (clock) gst_object_unref(clock);
                    vtrpon::publishNetworkAudio(buffer, timecode);
                    vtrpon::publishDeckLinkAudio(buffer, timecode);
                    return GST_PAD_PROBE_OK;
                }, programOutput_, nullptr);
            gst_object_unref(pad);
        }
    }
    bgmControlVolume_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "bgm-control-volume");
    bgmPostFaderAudioLevel_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "bgm-post-fader-audio-level");
    bgmManualFadeVolume_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "bgm-manual-fade-volume");
    bgmTransportVolume_ = gst_bin_get_by_name(
        GST_BIN(programOutput_), "bgm-transport-volume");
    if (!takeVolumes_[0] || !takeVolumes_[1] || !deckEffects_[0] || !deckEffects_[1]
        || !programItemAudioLevels_[0] || !programItemAudioLevels_[1]
        || !programAudioVolume_ || !programAudioLevel_ || !programAudioEffects_
        || !bgmControlVolume_ || !bgmManualFadeVolume_
        || !bgmTransportVolume_ || !bgmPostFaderAudioLevel_) {
        return false;
    }

    setMasterEq(masterEqDb_[0], masterEqDb_[1], masterEqDb_[2]);
    programBus_ = gst_element_get_bus(programOutput_);
    if (!programBus_) {
        return false;
    }
    busCallbacks_ = std::make_shared<BusCallbacks>();
    busCallbacks_->context = &callbackContext_;
    busCallbacks_->owner = this;
    busCallbacks_->callback = callback;
    busCallbacks_->userData = userData;
    gst_bus_set_sync_handler(programBus_, &ProgramAudioOutput::handleBusSync,
        new std::shared_ptr<BusCallbacks>(busCallbacks_), [](gpointer data) {
            delete static_cast<std::shared_ptr<BusCallbacks> *>(data);
        });
    return true;
}

GstBusSyncReply ProgramAudioOutput::handleBusSync(GstBus *bus, GstMessage *message,
                                                 gpointer userData)
{
    const auto callbacks = *static_cast<std::shared_ptr<BusCallbacks> *>(userData);
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_LATENCY) {
        std::lock_guard<std::mutex> lock(callbacks->mutex);
        if (callbacks->context && !callbacks->latencyQueued) {
            callbacks->latencyQueued = true;
            // Never recalculate from a GStreamer posting thread: latency queries
            // can wait for streaming locks held by that same thread. The Qt
            // event also runs before the player's next 100 ms bus polling tick.
            const bool queued = QMetaObject::invokeMethod(callbacks->context, [callbacks] {
                ProgramAudioOutput *owner = nullptr;
                {
                    std::lock_guard<std::mutex> lock(callbacks->mutex);
                    callbacks->latencyQueued = false;
                    owner = callbacks->owner;
                }
                // The context and owner are constructed and destroyed on this
                // thread. Detach invalidates the owner before context teardown.
                if (owner) owner->updateLatency();
            }, Qt::QueuedConnection);
            if (!queued) callbacks->latencyQueued = false;
        }
    }
    return callbacks->callback
        ? callbacks->callback(bus, message, callbacks->userData) : GST_BUS_PASS;
}

void ProgramAudioOutput::updateLatency()
{
    latencyUpdatePending_ = true;
    if (shutdownRequested_ || !programOutput_ || !programOutputReady_
        || programDeviceChange_) {
        return;
    }
    latencyUpdatePending_ = false;
    if (!gst_bin_recalculate_latency(GST_BIN(programOutput_))) {
        qWarning() << "Could not update PROGRAM audio latency";
    }
}

GstElement *ProgramAudioOutput::itemLevel(int deckIndex) const
{
    return deckIndex >= 0 && deckIndex < static_cast<int>(programItemAudioLevels_.size())
        ? programItemAudioLevels_[deckIndex] : nullptr;
}

void ProgramAudioOutput::requestDevice(const QString &id)
{
    if (shutdownRequested_) {
        return;
    }
    requestedProgramDevice_ = id;
    programDeviceFailed_ = false;
}

void ProgramAudioOutput::reportFailure()
{
    // The audio graph alone failed. Producers/video keep their system clock.
    programOutputReady_ = false;
    programDeviceFailed_ = true;
}

QString ProgramAudioOutput::deviceStatusKey() const
{
    if (programDeviceChange_) {
        return QStringLiteral("settings.output.applying");
    }
    if (programDeviceFailed_) {
        return QStringLiteral("settings.output.failed");
    }
    if (requestedProgramDevice_ != appliedProgramDevice_) {
        return QStringLiteral("settings.output.pending");
    }
    return QStringLiteral("settings.output.applied");
}

bool ProgramAudioOutput::deviceChanging() const
{
    return static_cast<bool>(programDeviceChange_);
}

bool ProgramAudioOutput::deviceOperationPending() const
{
    return programDeviceChange_
        && !programDeviceChange_->done.load(std::memory_order_acquire);
}

void ProgramAudioOutput::updateDevice(bool mayChange)
{
    if (programDeviceChange_) {
        if (!programDeviceChange_->done.load(std::memory_order_acquire)) {
            return;
        }
        // A failed open belongs to this explicit device-setting operation;
        // do not turn its stale bus error into an ON AIR media failure.
        drainBus();
        if (programDeviceChange_->succeeded) {
            appliedProgramDevice_ = programDeviceChange_->id;
        } else {
            programDeviceFailed_ = true;
        }
        programOutputReady_ = programDeviceChange_->outputReady;
        programDeviceChange_.reset();
        if (latencyUpdatePending_) updateLatency();
    }
    if (shutdownRequested_ || programDeviceFailed_ || !programOutput_
        || (programOutputReady_ && requestedProgramDevice_ == appliedProgramDevice_)
        || !mayChange) {
        return;
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(programOutput_),
                                          "program-audio-output");
    const bool hasDeviceProperty = sink
        && g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "device");
    if (!sink || (!hasDeviceProperty && !requestedProgramDevice_.isEmpty())) {
        if (sink) {
            gst_object_unref(sink);
        }
        programDeviceFailed_ = true;
        return;
    }
    auto change = std::make_shared<DeviceChange>();
    change->id = requestedProgramDevice_;
    programDeviceChange_ = change;
    auto *pipeline = GST_ELEMENT(gst_object_ref(programOutput_));
    const auto previous = appliedProgramDevice_.toUtf8();
    const bool restorePrevious = programOutputReady_;
    // Healthy output changes wait for idle. A failed output may recover while
    // independent video/producer graphs continue on the system clock. The worker
    // owns its references and never accesses the player or this object.
    try {
    std::thread([change, pipeline, sink, previous, restorePrevious, hasDeviceProperty] {
        GstState current = GST_STATE_VOID_PENDING;
        const auto stopped = gst_element_set_state(pipeline, GST_STATE_NULL);
        const auto settled = stopped == GST_STATE_CHANGE_FAILURE ? GST_STATE_CHANGE_FAILURE
            : gst_element_get_state(pipeline, &current, nullptr, 2 * GST_SECOND);
        if (settled != GST_STATE_CHANGE_FAILURE && current == GST_STATE_NULL) {
            const auto id = change->id.toUtf8();
            if (hasDeviceProperty) {
                g_object_set(sink, "device", id.isEmpty() ? nullptr : id.constData(), nullptr);
            }
            const auto started = gst_element_set_state(pipeline, GST_STATE_PLAYING);
            current = GST_STATE_VOID_PENDING;
            change->succeeded = started != GST_STATE_CHANGE_FAILURE
                && gst_element_get_state(pipeline, &current, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_FAILURE
                && current == GST_STATE_PLAYING;
            change->outputReady = change->succeeded;
            if (!change->succeeded && restorePrevious) {
                if (gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE
                    && gst_element_get_state(pipeline, &current, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_FAILURE
                    && current == GST_STATE_NULL) {
                    if (hasDeviceProperty) {
                        g_object_set(sink, "device", previous.isEmpty() ? nullptr : previous.constData(), nullptr);
                    }
                    current = GST_STATE_VOID_PENDING;
                    change->outputReady = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE
                        && gst_element_get_state(pipeline, &current, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_FAILURE
                        && current == GST_STATE_PLAYING;
                }
            }
        }
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        change->done.store(true, std::memory_order_release);
    }).detach();
    } catch (const std::system_error &error) {
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        change->outputReady = programOutputReady_;
        change->done.store(true, std::memory_order_release);
        qWarning() << "Could not start program audio device worker:" << error.what();
    }
}

void ProgramAudioOutput::setDeckAudible(int deckIndex, bool audible)
{
    if (deckIndex >= 0 && deckIndex < static_cast<int>(takeVolumes_.size())
        && takeVolumes_[deckIndex]) {
        vtrpon::setAudioEffectAudible(deckEffects_[deckIndex], audible);
        if (!audible) vtrpon::clearAudioDelay(programOutput_,
            deckIndex == 0 ? "deck-a-item-delay" : "deck-b-item-delay");
        g_object_set(takeVolumes_[deckIndex], "volume", audible ? 1.0 : 0.0,
                     nullptr);
    }
}

void ProgramAudioOutput::setMasterDelayFrames(int frames)
{
    vtrpon::setAudioDelayFrames(programOutput_, "program-audio-delay", frames);
}

void ProgramAudioOutput::setDeckDelayFrames(int deck, int frames)
{
    if (deck < 0 || deck > 1) return;
    vtrpon::setAudioDelayFrames(programOutput_, deck == 0 ? "deck-a-item-delay" : "deck-b-item-delay", frames);
}

void ProgramAudioOutput::setMasterVolume(double volume)
{
    masterVolume_ = std::clamp(volume, 0.0, 2.0);
    applyProgramVolume();
}

void ProgramAudioOutput::setDeckEq(int deck, double lowDb, double midDb, double highDb)
{
    if (deck < 0 || deck >= 2 || !deckEffects_[deck]) return;
    vtrpon::setAudioEffectEqualizer(deckEffects_[deck], lowDb, midDb, highDb);
}

void ProgramAudioOutput::setMasterEq(double lowDb, double midDb, double highDb)
{
    masterEqDb_ = {vtrpon::safeEqGain(lowDb), vtrpon::safeEqGain(midDb), vtrpon::safeEqGain(highDb)};
    if (programAudioEffects_) {
        vtrpon::setAudioEffectEqualizer(programAudioEffects_, masterEqDb_[0], masterEqDb_[1], masterEqDb_[2]);
    }
}

void ProgramAudioOutput::setMasterMuted(bool muted)
{
    masterMuted_ = muted;
    applyProgramVolume();
}

void ProgramAudioOutput::setFtbCoverage(double coverage)
{
    ftbCoverage_ = qBound(0.0, coverage, 1.0);
    applyProgramVolume();
}

void ProgramAudioOutput::setBgmGain(double gain)
{
    if (bgmControlVolume_) {
        g_object_set(bgmControlVolume_, "volume", std::clamp(gain, 0.0, 2.0),
                     nullptr);
    }
}

void ProgramAudioOutput::setBgmManualFadeGain(double gain)
{
    if (bgmManualFadeVolume_) {
        g_object_set(bgmManualFadeVolume_, "volume", std::clamp(gain, 0.0, 1.0),
                     nullptr);
    }
}

void ProgramAudioOutput::setBgmTransportGain(double gain)
{
    if (bgmTransportVolume_) {
        g_object_set(bgmTransportVolume_, "volume", std::clamp(gain, 0.0, 1.0),
                     nullptr);
    }
}

void ProgramAudioOutput::applyProgramVolume()
{
    if (programAudioVolume_) {
        const gdouble ftbGain = 1.0 - ftbCoverage_;
        const gdouble muteGain = masterMuted_ ? 0.0 : 1.0;
        g_object_set(programAudioVolume_, "volume",
                     masterVolume_ * ftbGain * muteGain, nullptr);
    }
}

void ProgramAudioOutput::drainBus()
{
    if (!programBus_) {
        return;
    }
    while (GstMessage *message = gst_bus_pop(programBus_)) {
        gst_message_unref(message);
    }
}

void ProgramAudioOutput::detachCallbacks()
{
    if (busCallbacks_) {
        // Serialize context invalidation with the posting thread's invokeMethod.
        // GStreamer retains the shared callback data for any in-flight handler.
        std::lock_guard<std::mutex> lock(busCallbacks_->mutex);
        busCallbacks_->context = nullptr;
        busCallbacks_->owner = nullptr;
    }
    if (programBus_) {
        gst_bus_set_sync_handler(programBus_, nullptr, nullptr, nullptr);
    }
}

bool ProgramAudioOutput::stop()
{
    shutdownRequested_ = true;
    detachCallbacks();
    if (shutdownConfirmed_) {
        return true;
    }
    if (deviceOperationPending()) {
        return false;
    }
    if (!programOutput_) {
        shutdownConfirmed_ = true;
        return true;
    }
    if (gst_element_set_state(programOutput_, GST_STATE_NULL)
        == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "program output" << "rejected GST_STATE_NULL";
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn result = gst_element_get_state(
        programOutput_, &current, &pending, kShutdownStateTimeout);
    if (result == GST_STATE_CHANGE_FAILURE || current != GST_STATE_NULL) {
        qWarning() << "program output" << "did not reach GST_STATE_NULL; current="
                   << gst_element_state_get_name(current) << "pending="
                   << gst_element_state_get_name(pending);
        return false;
    }
    shutdownConfirmed_ = true;
    return true;
}
