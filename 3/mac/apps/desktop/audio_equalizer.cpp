#include "audio_equalizer.h"

#include <gst/base/gstbasetransform.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {
// All consumers negotiate interleaved stereo float PCM at 48 kHz.
constexpr int kChannels = 2;
constexpr double kSampleRate = 48000.0;
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    std::array<double, kChannels> z1{}, z2{};
    void clear() { z1.fill(0); z2.fill(0); }
    double process(double input, int channel) {
        const double output = b0 * input + z1[channel];
        z1[channel] = b1 * input - a1 * output + z2[channel];
        z2[channel] = b2 * input - a2 * output;
        // Avoid denormal processing after a long silent tail.
        if (std::abs(z1[channel]) < 1e-30) z1[channel] = 0;
        if (std::abs(z2[channel]) < 1e-30) z2[channel] = 0;
        return output;
    }
    void configure(int band, double gain) {
        if (gain == 0) {
            b0 = 1; b1 = b2 = a1 = a2 = 0;
            clear();
            return;
        }
        // RBJ bilinear-transform biquads, normalized by a0:
        // https://www.w3.org/TR/audio-eq-cookbook/
        // LOW/HIGH: monotonic shelves with S=1. MID: broad peak with Q=0.7.
        const double a = std::pow(10.0, gain / 40.0);
        const double w = 2 * std::acos(-1.0) * vtrpon::kEqFrequencies[band] / kSampleRate;
        const double c = std::cos(w), s = std::sin(w);
        double a0;
        if (band == 1) {
            const double alpha = s / (2 * vtrpon::kEqMidQ);
            b0 = 1 + alpha * a; b1 = -2 * c; b2 = 1 - alpha * a;
            a0 = 1 + alpha / a; a1 = -2 * c; a2 = 1 - alpha / a;
        } else {
            const double beta = s * std::sqrt(2 * a); // 2 sqrt(A) alpha, S=1
            if (band == 0) {
                b0 = a * ((a + 1) - (a - 1) * c + beta);
                b1 = 2 * a * ((a - 1) - (a + 1) * c);
                b2 = a * ((a + 1) - (a - 1) * c - beta);
                a0 = (a + 1) + (a - 1) * c + beta;
                a1 = -2 * ((a - 1) + (a + 1) * c);
                a2 = (a + 1) + (a - 1) * c - beta;
            } else {
                b0 = a * ((a + 1) + (a - 1) * c + beta);
                b1 = -2 * a * ((a - 1) + (a + 1) * c);
                b2 = a * ((a + 1) + (a - 1) * c - beta);
                a0 = (a + 1) - (a - 1) * c + beta;
                a1 = 2 * ((a - 1) - (a + 1) * c);
                a2 = (a + 1) - (a - 1) * c - beta;
            }
        }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
};
} // namespace

struct vtrpon::AudioEqualizerProcessor::State {
    std::mutex mutex;
    std::array<double, 3> gains{};
    std::array<Biquad, 3> filters{};
    void clear() { for (auto &filter : filters) filter.clear(); }
};

vtrpon::AudioEqualizerProcessor::AudioEqualizerProcessor() : state_(std::make_unique<State>()) {}
vtrpon::AudioEqualizerProcessor::~AudioEqualizerProcessor() = default;

void vtrpon::AudioEqualizerProcessor::setGain(unsigned band, double gain) {
    if (band >= state_->gains.size()) return;
    std::lock_guard lock(state_->mutex);
    gain = safeEqGain(gain);
    if (state_->gains[band] == gain) return;
    state_->gains[band] = gain;
    state_->filters[band].configure(band, gain);
}

double vtrpon::AudioEqualizerProcessor::gain(unsigned band) const {
    if (band >= state_->gains.size()) return 0;
    std::lock_guard lock(state_->mutex);
    return state_->gains[band];
}

void vtrpon::AudioEqualizerProcessor::reset() {
    std::lock_guard lock(state_->mutex);
    state_->clear();
}

GstFlowReturn vtrpon::AudioEqualizerProcessor::process(GstBuffer *buffer) {
    auto &state = *state_;
    std::lock_guard lock(state.mutex);
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) state.clear();
    // Exact unity, including PCM bits, when all controls are centered.
    if (state.gains == std::array<double, 3>{}) return GST_FLOW_OK;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_FLOW_ERROR;
    if (map.size % (kChannels * sizeof(float))) {
        gst_buffer_unmap(buffer, &map);
        return GST_FLOW_ERROR;
    }
    const bool gap = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP);
    bool silent = true;
    for (gsize i = 0; i < map.size / sizeof(float); ++i) {
        float sample = 0;
        if (!gap) std::memcpy(&sample, map.data + i * sizeof(float), sizeof(float));
        double output = sample;
        for (int band = 0; band < 3; ++band) {
            if (state.gains[band] != 0)
                output = state.filters[band].process(output, i % kChannels);
        }
        sample = static_cast<float>(output);
        std::memcpy(map.data + i * sizeof(float), &sample, sizeof(float));
        silent = silent && sample == 0;
    }
    gst_buffer_unmap(buffer, &map);
    if (silent) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_GAP);
    else GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_GAP);
    return GST_FLOW_OK;
}

namespace {
struct VtrponAudioEqualizer { GstBaseTransform parent; vtrpon::AudioEqualizerProcessor *processor; };
struct VtrponAudioEqualizerClass { GstBaseTransformClass parent; };
G_DEFINE_TYPE(VtrponAudioEqualizer, vtrpon_audio_equalizer, GST_TYPE_BASE_TRANSFORM)

void setProperty(GObject *object, guint id, const GValue *value, GParamSpec *spec) {
    if (id < 1 || id > 3) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); return; }
    reinterpret_cast<VtrponAudioEqualizer *>(object)->processor->setGain(id - 1, g_value_get_double(value));
}
void getProperty(GObject *object, guint id, GValue *value, GParamSpec *spec) {
    if (id < 1 || id > 3) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); return; }
    g_value_set_double(value, reinterpret_cast<VtrponAudioEqualizer *>(object)->processor->gain(id - 1));
}
gboolean reset(GstBaseTransform *base) {
    reinterpret_cast<VtrponAudioEqualizer *>(base)->processor->reset();
    return TRUE;
}
gboolean sinkEvent(GstBaseTransform *base, GstEvent *event) {
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_FLUSH_START: case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_STREAM_START: case GST_EVENT_SEGMENT: case GST_EVENT_EOS:
        reset(base);
        break;
    default: break;
    }
    return GST_BASE_TRANSFORM_CLASS(vtrpon_audio_equalizer_parent_class)->sink_event(base, event);
}
GstFlowReturn transform(GstBaseTransform *base, GstBuffer *buffer) {
    return reinterpret_cast<VtrponAudioEqualizer *>(base)->processor->process(buffer);
}
void finalize(GObject *object) {
    delete reinterpret_cast<VtrponAudioEqualizer *>(object)->processor;
    G_OBJECT_CLASS(vtrpon_audio_equalizer_parent_class)->finalize(object);
}
void vtrpon_audio_equalizer_class_init(VtrponAudioEqualizerClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = setProperty;
    object->get_property = getProperty;
    object->finalize = finalize;
    constexpr std::array<const char *, 3> names{"band0", "band1", "band2"};
    for (guint i = 0; i < names.size(); ++i) {
        g_object_class_install_property(object, i + 1,
            g_param_spec_double(names[i], names[i], "Band gain in dB",
                -vtrpon::kEqMaximumDb, vtrpon::kEqMaximumDb, 0,
                static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));
    }
    auto *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "MG-style three-band EQ", "Filter/Effect/Audio",
        "100 Hz low shelf, 2.5 kHz peak, 10 kHz high shelf, +/-15 dB", "VTR-PON");
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
void vtrpon_audio_equalizer_init(VtrponAudioEqualizer *eq) {
    eq->processor = new vtrpon::AudioEqualizerProcessor;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(eq), TRUE);
    gst_base_transform_set_gap_aware(GST_BASE_TRANSFORM(eq), TRUE);
}
} // namespace

bool vtrpon::registerAudioEqualizer() {
    if (!gst_is_initialized()) return false;
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        registered = gst_element_register(nullptr, "vtrponaudioequalizer", GST_RANK_NONE,
                                          vtrpon_audio_equalizer_get_type());
    });
    return registered;
}
double vtrpon::safeEqGain(double gain) {
    return std::isfinite(gain) ? std::clamp(gain, -kEqMaximumDb, kEqMaximumDb) : 0.0;
}
void vtrpon::setAudioEqualizer(GstElement *eq, double lowDb, double midDb, double highDb) {
    if (!eq) return;
    g_object_set(eq, "band0", safeEqGain(lowDb), "band1", safeEqGain(midDb),
                 "band2", safeEqGain(highDb), nullptr);
}
