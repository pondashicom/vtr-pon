#pragma once

#include <gst/gst.h>
#include <atomic>
#include <QThread>

// Registered only inside the regression process; no physical device is opened.
namespace UvcFaultTest {
inline std::atomic_bool holdOpen{false};
inline std::atomic_bool openEntered{false};
inline std::atomic_bool dropFrames{false};
inline std::atomic_bool holdStop{false};
inline std::atomic_bool stopEntered{false};
inline std::atomic_int liveSources{0};
inline std::atomic_int pattern{0};
struct Source { GstBin parent; };
struct SourceClass { GstBinClass parent; };
G_DEFINE_TYPE(Source, fault_uvc_source, GST_TYPE_BIN)

inline GstStateChangeReturn changeState(GstElement *element, GstStateChange change) {
    if (change == GST_STATE_CHANGE_NULL_TO_READY) {
        openEntered.store(true);
        while (holdOpen.load()) QThread::msleep(5);
    }
    if (change == GST_STATE_CHANGE_READY_TO_NULL) {
        stopEntered.store(true);
        while (holdStop.load()) QThread::msleep(5);
    }
    return GST_ELEMENT_CLASS(fault_uvc_source_parent_class)->change_state(element, change);
}
inline void fault_uvc_source_class_init(SourceClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->finalize = [](GObject *instance) {
        --liveSources;
        G_OBJECT_CLASS(fault_uvc_source_parent_class)->finalize(instance);
    };
    object->set_property = [](GObject *, guint, const GValue *, GParamSpec *) {};
    object->get_property = [](GObject *, guint id, GValue *value, GParamSpec *) {
        if (id == 1) g_value_set_boolean(value, TRUE);
        else g_value_set_string(value, "fault-test-camera");
    };
    g_object_class_install_property(object, 1, g_param_spec_boolean(
        "do-timestamp", "Timestamp", "Timestamp", TRUE, G_PARAM_READWRITE));
    g_object_class_install_property(object, 2, g_param_spec_string(
        "device-path", "Device", "Device", "", G_PARAM_READWRITE));
    g_object_class_install_property(object, 3, g_param_spec_string(
        "device", "Device", "Persistent device", "", G_PARAM_READWRITE));
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
        "Fault test camera", "Source/Video", "UVC failure regression", "VTR-PON tests");
    GST_ELEMENT_CLASS(klass)->change_state = changeState;
}
inline void fault_uvc_source_init(Source *self) {
    ++liveSources;
    auto *source = gst_element_factory_make("videotestsrc", nullptr);
    g_object_set(source, "is-live", TRUE, "do-timestamp", TRUE, "pattern", pattern.load(), nullptr);
    gst_bin_add(GST_BIN(self), source);
    auto *pad = gst_element_get_static_pad(source, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer) {
            return dropFrames.load() ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
        }, nullptr, nullptr);
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}
inline bool install(const char *factory = "mfvideosrc") {
    auto *registry = gst_registry_get();
    if (auto *feature = gst_registry_find_feature(registry, factory, GST_TYPE_ELEMENT_FACTORY)) {
        gst_registry_remove_feature(registry, feature);
        gst_object_unref(feature);
    }
    return gst_element_register(nullptr, factory, GST_RANK_NONE, fault_uvc_source_get_type());
}
}
