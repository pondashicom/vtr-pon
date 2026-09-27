#include "audio_delay.h"

#include <gst/base/gstbasetransform.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace {
constexpr int kChannels = 2;
constexpr int kCapacity = vtrpon::kMaximumAudioDelayFrames
    * vtrpon::kAudioDelaySamplesPerFrame * kChannels;
struct DelayState {
    std::mutex mutex;
    std::array<float, kCapacity> samples{};
    int frames = 0;
    int cursor = 0;
    void clear() { samples.fill(0.0F); cursor = 0; }
};
struct VtrponAudioDelay { GstBaseTransform parent; DelayState *state; };
struct VtrponAudioDelayClass { GstBaseTransformClass parent; };
G_DEFINE_TYPE(VtrponAudioDelay, vtrpon_audio_delay, GST_TYPE_BASE_TRANSFORM)

void setProperty(GObject *object, guint id, const GValue *value, GParamSpec *spec) {
    if (id != 1) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); return; }
    auto &state = *reinterpret_cast<VtrponAudioDelay *>(object)->state;
    std::lock_guard lock(state.mutex);
    const int frames = g_value_get_int(value);
    if (state.frames != frames) { state.frames = frames; state.clear(); }
}
void getProperty(GObject *object, guint id, GValue *value, GParamSpec *spec) {
    if (id != 1) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); return; }
    auto &state = *reinterpret_cast<VtrponAudioDelay *>(object)->state;
    std::lock_guard lock(state.mutex);
    g_value_set_int(value, state.frames);
}
gboolean reset(GstBaseTransform *base) {
    auto &state = *reinterpret_cast<VtrponAudioDelay *>(base)->state;
    std::lock_guard lock(state.mutex);
    state.clear();
    return TRUE;
}
gboolean sinkEvent(GstBaseTransform *base, GstEvent *event) {
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_STREAM_START:
    case GST_EVENT_SEGMENT:
    case GST_EVENT_EOS:
        reset(base);
        break;
    default: break;
    }
    return GST_BASE_TRANSFORM_CLASS(vtrpon_audio_delay_parent_class)->sink_event(base, event);
}
GstFlowReturn transform(GstBaseTransform *base, GstBuffer *buffer) {
    auto &state = *reinterpret_cast<VtrponAudioDelay *>(base)->state;
    std::lock_guard lock(state.mutex);
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) state.clear();
    if (!state.frames) return GST_FLOW_OK;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_FLOW_ERROR;
    if (map.size % (kChannels * sizeof(float))) {
        gst_buffer_unmap(buffer, &map);
        return GST_FLOW_ERROR;
    }
    const int length = state.frames * vtrpon::kAudioDelaySamplesPerFrame * kChannels;
    const bool gap = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP);
    bool silent = true;
    for (gsize i = 0; i < map.size; i += sizeof(float)) {
        float input = 0.0F;
        if (!gap) std::memcpy(&input, map.data + i, sizeof(float));
        const float output = state.samples[state.cursor];
        state.samples[state.cursor] = input;
        if (++state.cursor == length) state.cursor = 0;
        std::memcpy(map.data + i, &output, sizeof(float));
        silent = silent && output == 0.0F;
    }
    gst_buffer_unmap(buffer, &map);
    if (silent) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_GAP);
    else GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_GAP);
    // Keep timestamps/segments intact: this is intentional signal delay, not
    // scheduling latency. Transport stop/seek discards the buffered tail.
    return GST_FLOW_OK;
}
void finalize(GObject *object) {
    delete reinterpret_cast<VtrponAudioDelay *>(object)->state;
    G_OBJECT_CLASS(vtrpon_audio_delay_parent_class)->finalize(object);
}
void vtrpon_audio_delay_class_init(VtrponAudioDelayClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = setProperty;
    object->get_property = getProperty;
    object->finalize = finalize;
    g_object_class_install_property(object, 1,
        g_param_spec_int("frames", "Frames", "Audio delay in 30 fps video frames",
            0, vtrpon::kMaximumAudioDelayFrames, 0,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
    auto *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Frame audio delay", "Filter/Effect/Audio",
        "Delays stereo PCM by 0 to 6 video frames at 30 fps", "VTR-PON");
    GstCaps *caps = gst_caps_from_string(
        "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    auto *base = GST_BASE_TRANSFORM_CLASS(klass);
    base->start = reset;
    base->stop = reset;
    base->sink_event = sinkEvent;
    base->transform_ip = transform;
}
void vtrpon_audio_delay_init(VtrponAudioDelay *delay) {
    delay->state = new DelayState;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(delay), TRUE);
    gst_base_transform_set_gap_aware(GST_BASE_TRANSFORM(delay), TRUE);
}
}

bool vtrpon::registerAudioDelay() {
    if (!gst_is_initialized()) return false;
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        registered = gst_element_register(nullptr, "vtrponaudiodelay", GST_RANK_NONE,
                                           vtrpon_audio_delay_get_type());
    });
    return registered;
}
bool vtrpon::setAudioDelayFrames(GstElement *bin, const char *name, int frames) {
    if (!bin || !GST_IS_BIN(bin) || frames < 0 || frames > kMaximumAudioDelayFrames) return false;
    GstElement *delay = gst_bin_get_by_name(GST_BIN(bin), name);
    if (!delay) return false;
    g_object_set(delay, "frames", frames, nullptr);
    gst_object_unref(delay);
    return true;
}
void vtrpon::clearAudioDelay(GstElement *bin, const char *name) {
    if (!bin || !GST_IS_BIN(bin)) return;
    GstElement *delay = gst_bin_get_by_name(GST_BIN(bin), name);
    if (!delay) return;
    reset(GST_BASE_TRANSFORM(delay));
    gst_object_unref(delay);
}
