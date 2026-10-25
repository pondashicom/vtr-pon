#pragma once

#include <gst/gst.h>

namespace {
// Process-local capture substitute: no microphone or physical output is opened.
struct TestLiveAudioSource { GstBin parent; };
struct TestLiveAudioSourceClass { GstBinClass parent; };
G_DEFINE_TYPE(TestLiveAudioSource, test_live_audio_source, GST_TYPE_BIN)

GstStateChangeReturn testLiveAudioChangeState(GstElement *element, GstStateChange transition) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(element), "endpoint-id"));
    if (transition == GST_STATE_CHANGE_NULL_TO_READY && id && g_str_equal(id, "missing-input"))
        return GST_STATE_CHANGE_FAILURE;
    return GST_ELEMENT_CLASS(test_live_audio_source_parent_class)->change_state(element, transition);
}

void test_live_audio_source_class_init(TestLiveAudioSourceClass *klass) {
    auto *objects = G_OBJECT_CLASS(klass);
    objects->set_property = [](GObject *object, guint id, const GValue *value, GParamSpec *) {
        if (id == 1) g_object_set_data_full(object, "endpoint-id", g_value_dup_string(value), g_free);
    };
    objects->get_property = [](GObject *object, guint id, GValue *value, GParamSpec *) {
        if (id == 1) g_value_set_string(value, static_cast<const char *>(g_object_get_data(object, "endpoint-id")));
        else g_value_set_boolean(value, FALSE);
    };
    g_object_class_install_property(objects, 1, g_param_spec_string(
        "device", "Device", "Capture endpoint ID", "", G_PARAM_READWRITE));
    for (guint i = 0; i < 3; ++i) {
        const char *name = i == 0 ? "do-timestamp" : i == 1 ? "provide-clock" : "low-latency";
        g_object_class_install_property(objects, i + 2, g_param_spec_boolean(
            name, name, name, FALSE, G_PARAM_READWRITE));
    }
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
        "Test live audio", "Source/Audio", "Synthetic live capture", "VTR-PON tests");
    GST_ELEMENT_CLASS(klass)->change_state = testLiveAudioChangeState;
}

void test_live_audio_source_init(TestLiveAudioSource *self) {
    auto *source = gst_element_factory_make("audiotestsrc", nullptr);
    g_object_set(source, "is-live", TRUE, "volume", 0.25, nullptr);
    gst_bin_add(GST_BIN(self), source);
    auto *pad = gst_element_get_static_pad(source, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}

bool substituteLiveAudioSource() {
    auto *registry = gst_registry_get();
    if (auto *feature = gst_registry_find_feature(registry, "vtrponwasapirawsrc", GST_TYPE_ELEMENT_FACTORY)) {
        gst_registry_remove_feature(registry, feature);
        gst_object_unref(feature);
    }
    return gst_element_register(nullptr, "vtrponwasapirawsrc", GST_RANK_NONE, test_live_audio_source_get_type());
}
}
