#include "audio_effects.h"
#include "audio_equalizer.h"
#include "vst_service.h"

#include <gst/base/gstbasetransform.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {
constexpr int kUnassignedSlot = -1;
enum Property { Low = 1, Mid, High, Slot, VstAudible };

GType slotType() {
    static const GEnumValue values[] = {
        {kUnassignedSlot, "Unassigned", "none"},
        {int(vtrpon::VstSlot::Edit), "Edit", "edit"},
        {int(vtrpon::VstSlot::Item), "Item", "item"},
        {int(vtrpon::VstSlot::Master), "Master", "master"},
        {0, nullptr, nullptr}
    };
    static const GType type = g_enum_register_static("VtrponAudioEffectsSlot", values);
    return type;
}

struct EffectsState {
    vtrpon::AudioEqualizerProcessor equalizer;
    std::atomic_int slot{kUnassignedSlot};
    std::atomic_bool vstAudible{false};
};
struct VtrponAudioEffects { GstBaseTransform parent; EffectsState *state; };
struct VtrponAudioEffectsClass { GstBaseTransformClass parent; };
G_DEFINE_TYPE(VtrponAudioEffects, vtrpon_audio_effects, GST_TYPE_BASE_TRANSFORM)

void setProperty(GObject *object, guint id, const GValue *value, GParamSpec *spec) {
    auto &state = *reinterpret_cast<VtrponAudioEffects *>(object)->state;
    switch (id) {
    case Low: case Mid: case High:
        state.equalizer.setGain(id - Low, g_value_get_double(value));
        break;
    case Slot: state.slot = g_value_get_enum(value); break;
    case VstAudible: state.vstAudible = g_value_get_boolean(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
}

void getProperty(GObject *object, guint id, GValue *value, GParamSpec *spec) {
    auto &state = *reinterpret_cast<VtrponAudioEffects *>(object)->state;
    switch (id) {
    case Low: case Mid: case High:
        g_value_set_double(value, state.equalizer.gain(id - Low));
        break;
    case Slot: g_value_set_enum(value, state.slot.load()); break;
    case VstAudible: g_value_set_boolean(value, state.vstAudible.load()); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
}

gboolean reset(GstBaseTransform *base) {
    reinterpret_cast<VtrponAudioEffects *>(base)->state->equalizer.reset();
    return TRUE;
}

gboolean start(GstBaseTransform *base) {
    if (reinterpret_cast<VtrponAudioEffects *>(base)->state->slot.load() == kUnassignedSlot) {
        gst_element_message_full(GST_ELEMENT(base), GST_MESSAGE_ERROR,
            GST_CORE_ERROR, GST_CORE_ERROR_FAILED,
            g_strdup("Audio effects require an explicit slot"), nullptr,
            __FILE__, G_STRFUNC, __LINE__);
        return FALSE;
    }
    return reset(base);
}

gboolean sinkEvent(GstBaseTransform *base, GstEvent *event) {
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_START: case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_STREAM_START: case GST_EVENT_SEGMENT: case GST_EVENT_EOS:
        reset(base);
        break;
    default: break;
    }
    return GST_BASE_TRANSFORM_CLASS(vtrpon_audio_effects_parent_class)->sink_event(base, event);
}

GstFlowReturn transform(GstBaseTransform *base, GstBuffer *buffer) {
    auto &state = *reinterpret_cast<VtrponAudioEffects *>(base)->state;
    // Selection belongs to the application strip, never to the generic EQ.
    // A single decision prevents applying both effects to the same buffer.
    if (!vtrpon::vstModeEnabled()) return state.equalizer.process(buffer);

    const int slot = state.slot.load();
    if (slot == kUnassignedSlot) return GST_FLOW_ERROR;
    // CUE must not advance the shared ITEM plug-in's delay/history. It also
    // bypasses the replacement EQ while VST mode is selected.
    if (slot == int(vtrpon::VstSlot::Item) && !state.vstAudible.load()) return GST_FLOW_OK;

    GstMapInfo map{};
    if (gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) {
        if (map.size % (2 * sizeof(float)) == 0) {
            if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP)) std::memset(map.data, 0, map.size);
            vtrpon::processVst(vtrpon::VstSlot(slot), reinterpret_cast<float *>(map.data),
                              map.size / (2 * sizeof(float)));
            GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_GAP);
        }
        gst_buffer_unmap(buffer, &map);
    }
    return GST_FLOW_OK;
}

void finalize(GObject *object) {
    delete reinterpret_cast<VtrponAudioEffects *>(object)->state;
    G_OBJECT_CLASS(vtrpon_audio_effects_parent_class)->finalize(object);
}

void vtrpon_audio_effects_class_init(VtrponAudioEffectsClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = setProperty;
    object->get_property = getProperty;
    object->finalize = finalize;
    constexpr auto flags = static_cast<GParamFlags>(
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING);
    constexpr std::array<const char *, 3> names{"band0", "band1", "band2"};
    for (guint i = 0; i < names.size(); ++i) {
        g_object_class_install_property(object, i + Low,
            g_param_spec_double(names[i], names[i], "Built-in EQ band gain in dB",
                -vtrpon::kEqMaximumDb, vtrpon::kEqMaximumDb, 0, flags));
    }
    g_object_class_install_property(object, Slot,
        g_param_spec_enum("slot", "VST slot", "Explicit application audio route",
            slotType(), kUnassignedSlot, flags));
    g_object_class_install_property(object, VstAudible,
        g_param_spec_boolean("vst-audible", "VST audible",
            "ON AIR deck owns the ITEM VST instance", FALSE, flags));

    auto *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Application audio effects", "Filter/Effect/Audio",
        "Built-in EQ or explicitly routed VST processing", "VTR-PON");
    GstCaps *caps = gst_caps_from_string(
        "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    auto *base = GST_BASE_TRANSFORM_CLASS(klass);
    base->start = start;
    base->stop = reset;
    base->sink_event = sinkEvent;
    base->transform_ip = transform;
}

void vtrpon_audio_effects_init(VtrponAudioEffects *effects) {
    effects->state = new EffectsState;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(effects), TRUE);
    gst_base_transform_set_gap_aware(GST_BASE_TRANSFORM(effects), TRUE);
}
} // namespace

bool vtrpon::registerAudioEffects() {
    if (!gst_is_initialized()) return false;
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        registered = gst_element_register(nullptr, "vtrponaudioeffects", GST_RANK_NONE,
                                          vtrpon_audio_effects_get_type());
    });
    return registered;
}

void vtrpon::setAudioEffectEqualizer(GstElement *effects, double lowDb, double midDb, double highDb) {
    setAudioEqualizer(effects, lowDb, midDb, highDb);
}

void vtrpon::setAudioEffectAudible(GstElement *effects, bool audible) {
    if (effects) g_object_set(effects, "vst-audible", audible, nullptr);
}
