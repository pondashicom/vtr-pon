#include "program_audio_bridge.h"

#include <gst/base/gstbasesink.h>
#include <gst/base/gstpushsrc.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

GST_DEBUG_CATEGORY_STATIC(program_audio_bridge_debug);
#define GST_CAT_DEFAULT program_audio_bridge_debug

constexpr guint kRate = 48000;
constexpr guint kFrameBytes = 2 * sizeof(float);
constexpr guint64 kBufferTime = 200 * GST_MSECOND;
constexpr guint64 kLatencyTime = 10 * GST_MSECOND;
constexpr gsize kOutputChunkBytes = (kRate / 50) * kFrameBytes;
constexpr const char *kCaps =
    "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved";

struct BufferUnref {
    void operator()(GstBuffer *buffer) const { gst_buffer_unref(buffer); }
};
using Buffer = std::unique_ptr<GstBuffer, BufferUnref>;

struct Channel {
    Channel() { g_weak_ref_init(&source, nullptr); }
    ~Channel() { g_weak_ref_clear(&source); }

    void clearLocked()
    {
        buffers.clear();
        queuedBytes = 0;
        frontOffset = 0;
        discontinuity = true;
        ++generation;
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Buffer> buffers;
    guint64 queuedBytes = 0;
    gsize frontOffset = 0;
    guint64 bufferTime = kBufferTime;
    GstClockTime producerLatency = 0;
    const void *producer = nullptr;
    bool connected = false;
    bool discontinuity = true;
    guint64 generation = 0;
    GWeakRef source;
};

std::shared_ptr<Channel> findChannel(const std::string &name)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<Channel>> channels;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = channels.begin(); it != channels.end();) {
        if (it->second.expired()) it = channels.erase(it);
        else ++it;
    }
    auto &entry = channels[name];
    auto channel = entry.lock();
    if (!channel) {
        channel = std::make_shared<Channel>();
        entry = channel;
    }
    return channel;
}

struct SinkState {
    std::atomic_bool detached = false;
    std::string name = "default";
    std::shared_ptr<Channel> channel;
};

struct SourceState {
    std::string name = "default";
    std::shared_ptr<Channel> channel;
    guint64 bufferTime = kBufferTime;
    guint64 latencyTime = kLatencyTime;
    std::atomic_bool interrupted = false;
    // Accessed by the source streaming task, reset before that task starts.
    guint64 deliveredGeneration = 0;
    bool hasDelivered = false;
};

typedef struct _VtrponAudioSink {
    GstBaseSink parent;
    SinkState *state;
} VtrponAudioSink;
typedef struct _VtrponAudioSinkClass {
    GstBaseSinkClass parent;
} VtrponAudioSinkClass;
typedef struct _VtrponAudioSource {
    GstPushSrc parent;
    SourceState *state;
} VtrponAudioSource;
typedef struct _VtrponAudioSourceClass {
    GstPushSrcClass parent;
} VtrponAudioSourceClass;

G_DEFINE_TYPE(VtrponAudioSink, vtrpon_audio_sink, GST_TYPE_BASE_SINK)
G_DEFINE_TYPE(VtrponAudioSource, vtrpon_audio_source, GST_TYPE_PUSH_SRC)

enum Property : int { PropertyChannel = 1, PropertyBufferTime, PropertyLatencyTime };

void reportError(GstElement *element, GQuark domain, gint code,
                 const char *message, const char *detail = nullptr)
{
    // GST_ELEMENT_ERROR's debug macros import global data, which MSVC cannot
    // delay-load. This API posts the same error while retaining normal startup.
    gst_element_message_full(element, GST_MESSAGE_ERROR, domain, code,
        g_strdup(message), g_strdup(detail), __FILE__, G_STRFUNC, __LINE__);
}

template<class Element>
std::shared_ptr<Channel> elementChannel(Element *element)
{
    GST_OBJECT_LOCK(element);
    auto channel = element->state->channel;
    GST_OBJECT_UNLOCK(element);
    return channel;
}

void clearProducerChannel(const std::shared_ptr<Channel> &channel,
                          const void *producer, bool disconnect = false)
{
    if (!channel) return;
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (channel->producer != producer) return;
        channel->clearLocked();
        if (disconnect) channel->producer = nullptr;
    }
    channel->ready.notify_all();
}

void updateProducerLatency(const std::shared_ptr<Channel> &channel,
                           GstClockTime latency, const void *producer)
{
    if (!channel) return;
    if (!GST_CLOCK_TIME_IS_VALID(latency)) latency = 0;
    GstElement *source = nullptr;
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (channel->producer != producer) return;
        if (channel->producerLatency == latency) return;
        channel->producerLatency = latency;
        source = static_cast<GstElement *>(g_weak_ref_get(&channel->source));
    }
    if (source) {
        gst_element_post_message(source, gst_message_new_latency(GST_OBJECT(source)));
        gst_object_unref(source);
    }
}

gboolean sinkStart(GstBaseSink *base)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(base);
    GST_OBJECT_LOCK(sink);
    // A retired producer must never reconnect if its pending preroll reaches
    // start() after retirement. A replacement uses a new sink instance.
    if (sink->state->detached.load()) {
        GST_OBJECT_UNLOCK(sink);
        return TRUE;
    }
    sink->state->channel = findChannel(sink->state->name);
    auto channel = sink->state->channel;
    {
        std::lock_guard lock(channel->mutex);
        channel->producer = sink->state;
        channel->clearLocked();
    }
    GST_OBJECT_UNLOCK(sink);
    updateProducerLatency(channel, 0, sink->state);
    return TRUE;
}

gboolean sinkStop(GstBaseSink *base)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(base);
    GST_OBJECT_LOCK(sink);
    auto channel = std::move(sink->state->channel);
    GST_OBJECT_UNLOCK(sink);
    updateProducerLatency(channel, 0, sink->state);
    clearProducerChannel(channel, sink->state, true);
    return TRUE;
}

gboolean sinkEvent(GstBaseSink *base, GstEvent *event)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(base);
    auto channel = elementChannel(sink);
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_STREAM_START:
    case GST_EVENT_SEGMENT:
        clearProducerChannel(channel, sink->state);
        break;
    default:
        break;
    }
    const gboolean result = GST_BASE_SINK_CLASS(vtrpon_audio_sink_parent_class)
        ->event(base, event);
    updateProducerLatency(channel, gst_base_sink_get_latency(base), sink->state);
    return result;
}

gboolean sinkSendEvent(GstElement *element, GstEvent *event)
{
    const bool latencyChanged = GST_EVENT_TYPE(event) == GST_EVENT_LATENCY;
    const gboolean result = GST_ELEMENT_CLASS(vtrpon_audio_sink_parent_class)
        ->send_event(element, event);
    if (latencyChanged) {
        // Pipeline LATENCY events travel upstream through GstElement, before
        // live PCM reaches render; GstBaseSink::event cannot observe them.
        auto *sink = reinterpret_cast<VtrponAudioSink *>(element);
        if (auto channel = elementChannel(sink)) {
            updateProducerLatency(channel, gst_base_sink_get_latency(GST_BASE_SINK(sink)), sink->state);
        }
    }
    return result;
}

GstFlowReturn sinkRender(GstBaseSink *base, GstBuffer *input)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(base);
    auto channel = elementChannel(sink);
    if (!channel) return GST_FLOW_FLUSHING;
    updateProducerLatency(channel, gst_base_sink_get_latency(base), sink->state);
    guint64 generation = 0;
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (sink->state->detached.load() || channel->producer != sink->state || !channel->connected) return GST_FLOW_OK;
        generation = channel->generation;
    }
    const gsize bytes = gst_buffer_get_size(input);
    if (bytes == 0) return GST_FLOW_OK;
    if (bytes % kFrameBytes != 0 || !GST_BUFFER_PTS_IS_VALID(input)) {
        reportError(GST_ELEMENT(sink), GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
            "PROGRAM audio requires timestamped 48 kHz stereo float PCM");
        return GST_FLOW_ERROR;
    }
    const GstClockTime running = gst_segment_to_running_time(
        &base->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(input));
    const GstClockTime baseTime = gst_element_get_base_time(GST_ELEMENT(sink));
    if (!GST_CLOCK_TIME_IS_VALID(running) || !GST_CLOCK_TIME_IS_VALID(baseTime)
        || running >= GST_CLOCK_TIME_NONE - baseTime) {
        reportError(GST_ELEMENT(sink), GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
            "PROGRAM audio has an invalid playback timestamp");
        return GST_FLOW_ERROR;
    }
    Buffer buffer(gst_buffer_copy(input));
    if (!buffer) return GST_FLOW_ERROR;
    GST_BUFFER_PTS(buffer.get()) = baseTime + running;
    GST_BUFFER_DTS(buffer.get()) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer.get()) = gst_util_uint64_scale(
        bytes / kFrameBytes, GST_SECOND, kRate);
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (sink->state->detached.load() || channel->producer != sink->state || !channel->connected || generation != channel->generation) return GST_FLOW_OK;
        const guint64 limit = gst_util_uint64_scale(
            channel->bufferTime, kRate * kFrameBytes, GST_SECOND);
        if (channel->queuedBytes && (bytes > limit || channel->queuedBytes > limit - bytes)) {
            // An unavailable/stalled output must not make the media decoder
            // fail. Keep storage bounded and resume with current PCM rather
            // than replaying an old backlog when the receiver recovers.
            channel->clearLocked();
        }
        // Demuxers may deliver a single PCM block larger than buffer-time
        // (e.g. 500.5 ms from a PCM MOV). Keep that block intact: the bound is
        // the configured backlog or one input block, never an unbounded queue.
        channel->queuedBytes += bytes;
        channel->buffers.push_back(std::move(buffer));
    }
    channel->ready.notify_one();
    return GST_FLOW_OK;
}

void sinkSetProperty(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(object);
    if (id != PropertyChannel) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        return;
    }
    GST_OBJECT_LOCK(sink);
    if (!sink->state->channel) {
        const char *name = g_value_get_string(value);
        sink->state->name = name ? name : "default";
    }
    GST_OBJECT_UNLOCK(sink);
}

void sinkGetProperty(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(object);
    if (id != PropertyChannel) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        return;
    }
    GST_OBJECT_LOCK(sink);
    g_value_set_string(value, sink->state->name.c_str());
    GST_OBJECT_UNLOCK(sink);
}

void sinkFinalize(GObject *object)
{
    auto *sink = reinterpret_cast<VtrponAudioSink *>(object);
    delete sink->state;
    G_OBJECT_CLASS(vtrpon_audio_sink_parent_class)->finalize(object);
}

gboolean sourceStart(GstBaseSrc *base)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(base);
    GST_OBJECT_LOCK(source);
    auto channel = findChannel(source->state->name);
    const guint64 bufferTime = source->state->bufferTime;
    GST_OBJECT_UNLOCK(source);
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (!channel->connected) {
            channel->clearLocked();
            channel->bufferTime = bufferTime;
            channel->connected = true;
            g_weak_ref_set(&channel->source, G_OBJECT(source));
            claimed = true;
        }
    }
    if (!claimed) {
        reportError(GST_ELEMENT(source), GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_BUSY,
            "PROGRAM audio channel already has a receiver");
        return FALSE;
    }
    GstCaps *caps = gst_caps_from_string(kCaps);
    const gboolean result = gst_base_src_set_caps(base, caps);
    gst_caps_unref(caps);
    if (!result) {
        std::lock_guard<std::mutex> lock(channel->mutex);
        channel->connected = false;
        g_weak_ref_set(&channel->source, nullptr);
        channel->clearLocked();
        return FALSE;
    }
    GST_OBJECT_LOCK(source);
    source->state->channel = channel;
    source->state->interrupted.store(false);
    source->state->hasDelivered = false;
    GST_OBJECT_UNLOCK(source);
    return result;
}

gboolean sourceUnlock(GstBaseSrc *base)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(base);
    if (auto channel = elementChannel(source)) {
        {
            // Serialize the cancellation with the CV predicate to avoid losing
            // an unlock notification immediately before create starts waiting.
            std::lock_guard<std::mutex> lock(channel->mutex);
            source->state->interrupted.store(true);
        }
        channel->ready.notify_all();
    } else {
        source->state->interrupted.store(true);
    }
    return TRUE;
}

gboolean sourceUnlockStop(GstBaseSrc *base)
{
    reinterpret_cast<VtrponAudioSource *>(base)->state->interrupted.store(false);
    return TRUE;
}

gboolean sourceStop(GstBaseSrc *base)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(base);
    source->state->interrupted.store(true);
    GST_OBJECT_LOCK(source);
    auto channel = std::move(source->state->channel);
    GST_OBJECT_UNLOCK(source);
    if (channel) {
        {
            std::lock_guard<std::mutex> lock(channel->mutex);
            channel->connected = false;
            g_weak_ref_set(&channel->source, nullptr);
            channel->clearLocked();
        }
        channel->ready.notify_all();
    }
    return TRUE;
}

GstFlowReturn sourceCreate(GstPushSrc *base, GstBuffer **output)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(base);
    auto channel = elementChannel(source);
    if (!channel) return GST_FLOW_FLUSHING;
    bool skippedOldAudio = false;
    for (;;) {
        Buffer buffer;
        bool discontinuity = false;
        guint64 generation = 0;
        {
            std::unique_lock<std::mutex> lock(channel->mutex);
            channel->ready.wait(lock, [&] {
                return source->state->interrupted.load() || !channel->buffers.empty();
            });
            if (source->state->interrupted.load()) return GST_FLOW_FLUSHING;
            auto &front = channel->buffers.front();
            const gsize size = gst_buffer_get_size(front.get());
            const gsize offset = channel->frontOffset;
            const gsize bytes = (std::min)(size - offset, kOutputChunkBytes);
            if (!offset && bytes == size) {
                buffer = std::move(front);
            } else {
                // Feed short spans to the mixer so a large decoder block does
                // not commit half a second of old fader/transport state ahead.
                // Retain the remainder in the generation-checked bridge queue.
                buffer.reset(gst_buffer_copy_region(front.get(), GST_BUFFER_COPY_MEMORY, offset, bytes));
                if (!buffer) return GST_FLOW_ERROR;
                GST_BUFFER_FLAGS(buffer.get()) = GST_BUFFER_FLAGS(front.get());
                if (offset) GST_BUFFER_FLAG_UNSET(buffer.get(), GST_BUFFER_FLAG_DISCONT);
                const auto start = gst_util_uint64_scale(offset / kFrameBytes, GST_SECOND, kRate);
                const auto end = gst_util_uint64_scale((offset + bytes) / kFrameBytes, GST_SECOND, kRate);
                GST_BUFFER_PTS(buffer.get()) = GST_BUFFER_PTS(front.get()) + start;
                GST_BUFFER_DURATION(buffer.get()) = end - start;
            }
            channel->frontOffset += bytes;
            if (channel->frontOffset == size) {
                channel->buffers.pop_front();
                channel->frontOffset = 0;
            }
            channel->queuedBytes -= bytes;
            discontinuity = channel->discontinuity;
            channel->discontinuity = false;
            generation = channel->generation;
        }
        const GstClockTime baseTime = gst_element_get_base_time(GST_ELEMENT(source));
        if (!GST_CLOCK_TIME_IS_VALID(baseTime)) {
            reportError(GST_ELEMENT(source), GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
                "PROGRAM audio output has no valid timeline");
            return GST_FLOW_ERROR;
        }
        if (GST_BUFFER_PTS(buffer.get()) < baseTime) {
            // The producer keeps playing while this output restarts. PCM
            // rendered just before the new output base time is stale, not a
            // media error. Wait for current PCM without stopping the producer.
            skippedOldAudio = true;
            continue;
        }
        GST_BUFFER_PTS(buffer.get()) -= baseTime;
        if (source->state->hasDelivered
            && generation != source->state->deliveredGeneration) {
            // Clearing the bridge queue alone leaves the previous segment's
            // buffer inside audiomixer. Flush this input on its own streaming
            // task, so the first new frame is not clipped against that buffer.
            // A distinct seqnum prevents this becoming an all-input PROGRAM
            // flush. Keep the PROGRAM clock and its other inputs running.
            const guint32 seqnum = gst_util_seqnum_next();
            GstEvent *start = gst_event_new_flush_start();
            gst_event_set_seqnum(start, seqnum);
            GstEvent *stop = gst_event_new_flush_stop(FALSE);
            gst_event_set_seqnum(stop, seqnum);
            const gboolean started = gst_pad_push_event(GST_BASE_SRC_PAD(source), start);
            const gboolean stopped = gst_pad_push_event(GST_BASE_SRC_PAD(source), stop);
            GstSegment segment;
            gst_segment_init(&segment, GST_FORMAT_TIME);
            segment.position = GST_BUFFER_PTS(buffer.get());
            const gboolean segmented = gst_base_src_new_segment(GST_BASE_SRC(source), &segment);
            if (!started || !stopped || !segmented) {
                if (source->state->interrupted.load()) return GST_FLOW_FLUSHING;
                reportError(GST_ELEMENT(source), GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                    "Could not reset the PROGRAM audio input after a playback segment change");
                return GST_FLOW_ERROR;
            }
            discontinuity = true;
        }
        {
            std::lock_guard<std::mutex> lock(channel->mutex);
            if (source->state->interrupted.load()) return GST_FLOW_FLUSHING;
            if (generation != channel->generation) continue;
        }
        source->state->hasDelivered = true;
        source->state->deliveredGeneration = generation;
        if (discontinuity || skippedOldAudio)
            GST_BUFFER_FLAG_SET(buffer.get(), GST_BUFFER_FLAG_DISCONT);
        *output = buffer.release();
        return GST_FLOW_OK;
    }
}

gboolean sourceQuery(GstBaseSrc *base, GstQuery *query)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(base);
    if (GST_QUERY_TYPE(query) != GST_QUERY_LATENCY) {
        return GST_BASE_SRC_CLASS(vtrpon_audio_source_parent_class)->query(base, query);
    }
    GST_OBJECT_LOCK(source);
    guint64 minimum = source->state->latencyTime;
    guint64 maximum = (std::max)(minimum, source->state->bufferTime);
    auto channel = source->state->channel;
    GST_OBJECT_UNLOCK(source);
    if (channel) {
        std::lock_guard<std::mutex> lock(channel->mutex);
        const auto addLatency = [channel](guint64 value) {
            const guint64 room = GST_CLOCK_TIME_NONE - 1 - value;
            return value + (std::min)(room, channel->producerLatency);
        };
        minimum = addLatency(minimum);
        maximum = addLatency(maximum);
    }
    gst_query_set_latency(query, TRUE, minimum, maximum);
    return TRUE;
}

void sourceSetProperty(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(object);
    GST_OBJECT_LOCK(source);
    switch (id) {
    case PropertyChannel:
        if (!source->state->channel) {
            const char *name = g_value_get_string(value);
            source->state->name = name ? name : "default";
        }
        break;
    case PropertyBufferTime:
        source->state->bufferTime = g_value_get_uint64(value);
        break;
    case PropertyLatencyTime:
        source->state->latencyTime = g_value_get_uint64(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        break;
    }
    GST_OBJECT_UNLOCK(source);
}

void sourceGetProperty(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(object);
    GST_OBJECT_LOCK(source);
    switch (id) {
    case PropertyChannel: g_value_set_string(value, source->state->name.c_str()); break;
    case PropertyBufferTime: g_value_set_uint64(value, source->state->bufferTime); break;
    case PropertyLatencyTime: g_value_set_uint64(value, source->state->latencyTime); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
    GST_OBJECT_UNLOCK(source);
}

void sourceFinalize(GObject *object)
{
    auto *source = reinterpret_cast<VtrponAudioSource *>(object);
    delete source->state;
    G_OBJECT_CLASS(vtrpon_audio_source_parent_class)->finalize(object);
}

void addPadTemplate(GstElementClass *element, const char *name, GstPadDirection direction)
{
    GstCaps *caps = gst_caps_from_string(kCaps);
    gst_element_class_add_pad_template(element,
        gst_pad_template_new(name, direction, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
}

void vtrpon_audio_sink_class_init(VtrponAudioSinkClass *klass)
{
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = sinkSetProperty;
    object->get_property = sinkGetProperty;
    object->finalize = sinkFinalize;
    g_object_class_install_property(object, PropertyChannel,
        g_param_spec_string("channel", "Channel", "PROGRAM PCM channel", "default",
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                                    | GST_PARAM_MUTABLE_READY)));
    auto *element = GST_ELEMENT_CLASS(klass);
    element->send_event = sinkSendEvent;
    gst_element_class_set_static_metadata(element, "PROGRAM PCM sender", "Sink/Audio",
        "Preserves PCM and playback time across the PROGRAM pipeline boundary", "VTR-PON");
    addPadTemplate(element, "sink", GST_PAD_SINK);
    auto *base = GST_BASE_SINK_CLASS(klass);
    base->start = sinkStart;
    base->stop = sinkStop;
    base->event = sinkEvent;
    base->render = sinkRender;
}

void vtrpon_audio_sink_init(VtrponAudioSink *sink)
{
    sink->state = new SinkState;
    gst_base_sink_set_sync(GST_BASE_SINK(sink), TRUE);
}

void vtrpon_audio_source_class_init(VtrponAudioSourceClass *klass)
{
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = sourceSetProperty;
    object->get_property = sourceGetProperty;
    object->finalize = sourceFinalize;
    const auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                                              | GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object, PropertyChannel,
        g_param_spec_string("channel", "Channel", "PROGRAM PCM channel", "default", flags));
    g_object_class_install_property(object, PropertyBufferTime,
        g_param_spec_uint64("buffer-time", "Buffer time", "Maximum queued PCM time in nanoseconds",
            1, GST_CLOCK_TIME_NONE - 1, kBufferTime, flags));
    g_object_class_install_property(object, PropertyLatencyTime,
        g_param_spec_uint64("latency-time", "Latency time", "Delivery latency in nanoseconds",
            0, GST_CLOCK_TIME_NONE - 1, kLatencyTime, flags));
    auto *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "PROGRAM PCM receiver", "Source/Audio",
        "Waits for real PCM without fabricating silence or discarding queued audio", "VTR-PON");
    addPadTemplate(element, "src", GST_PAD_SRC);
    auto *base = GST_BASE_SRC_CLASS(klass);
    base->start = sourceStart;
    base->stop = sourceStop;
    base->unlock = sourceUnlock;
    base->unlock_stop = sourceUnlockStop;
    base->query = sourceQuery;
    GST_PUSH_SRC_CLASS(klass)->create = sourceCreate;
}

void vtrpon_audio_source_init(VtrponAudioSource *source)
{
    source->state = new SourceState;
    gst_base_src_set_live(GST_BASE_SRC(source), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(source), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(source), FALSE);
}

} // namespace

bool vtrpon::registerProgramAudioBridge()
{
    if (!gst_is_initialized()) return false;
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        GST_DEBUG_CATEGORY_INIT(program_audio_bridge_debug, "vtrpon-audio-bridge", 0,
                                "PROGRAM timestamp-preserving PCM transport");
        registered = gst_element_register(nullptr, "vtrponaudiosink", GST_RANK_NONE,
                                          vtrpon_audio_sink_get_type())
            && gst_element_register(nullptr, "vtrponaudiosrc", GST_RANK_NONE,
                                    vtrpon_audio_source_get_type());
    });
    return registered;
}

void vtrpon::disconnectProgramAudioProducer(GstElement *element)
{
    if (!element || !G_TYPE_CHECK_INSTANCE_TYPE(element, vtrpon_audio_sink_get_type())) return;
    auto *sink = reinterpret_cast<VtrponAudioSink *>(element);
    sink->state->detached.store(true);
    GST_OBJECT_LOCK(sink);
    auto channel = std::move(sink->state->channel);
    GST_OBJECT_UNLOCK(sink);
    clearProducerChannel(channel, sink->state, true);
}
