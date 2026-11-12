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
inline std::atomic_int createdSources{0};
inline std::atomic_int thumbnailOpens{0}, fullOpens{0};
inline std::atomic_int thumbnailFrameWidth{0}, fullFrameWidth{0};
struct Source { GstBin parent; GstElement *modeFilter; gboolean thumbnailOnly; };
struct SourceClass { GstBinClass parent; };
G_DEFINE_TYPE(Source, fault_uvc_source, GST_TYPE_BIN)

inline GstStateChangeReturn changeState(GstElement *element, GstStateChange change) {
    if (change == GST_STATE_CHANGE_NULL_TO_READY) {
        auto *self = reinterpret_cast<Source *>(element);
        if (self->thumbnailOnly) ++thumbnailOpens;
        else ++fullOpens;
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
    object->set_property = [](GObject *object, guint id, const GValue *value, GParamSpec *) {
        if (id != 4) return;
        auto *self = reinterpret_cast<Source *>(object);
        self->thumbnailOnly = g_value_get_boolean(value);
        auto *caps = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, self->thumbnailOnly ? 160 : 1920,
            "height", G_TYPE_INT, self->thumbnailOnly ? 120 : 1080,
            "framerate", GST_TYPE_FRACTION, self->thumbnailOnly ? 10 : 30, 1, nullptr);
        g_object_set(self->modeFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
    };
    object->get_property = [](GObject *object, guint id, GValue *value, GParamSpec *) {
        if (id == 4) g_value_set_boolean(value, reinterpret_cast<Source *>(object)->thumbnailOnly);
        else if (id == 1) g_value_set_boolean(value, TRUE);
        else g_value_set_string(value, "fault-test-camera");
    };
    g_object_class_install_property(object, 1, g_param_spec_boolean(
        "do-timestamp", "Timestamp", "Timestamp", TRUE, G_PARAM_READWRITE));
    g_object_class_install_property(object, 2, g_param_spec_string(
        "device-path", "Device", "Device", "", G_PARAM_READWRITE));
    g_object_class_install_property(object, 3, g_param_spec_string(
        "device", "Device", "Persistent device", "", G_PARAM_READWRITE));
    g_object_class_install_property(object, 4, g_param_spec_boolean(
        "thumbnail-only", "Thumbnail", "Capture role before first frame", FALSE, G_PARAM_READWRITE));
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
        "Fault test camera", "Source/Video", "UVC failure regression", "VTR-PON tests");
    GST_ELEMENT_CLASS(klass)->change_state = changeState;
}
inline void fault_uvc_source_init(Source *self) {
    ++liveSources;
    ++createdSources;
    auto *source = gst_element_factory_make("videotestsrc", nullptr);
    g_object_set(source, "is-live", TRUE, "do-timestamp", TRUE, "pattern", pattern.load(), nullptr);
    self->modeFilter = gst_element_factory_make("capsfilter", nullptr);
    // Normal capture is the default; retain thumbnail-only to catch regressions
    // that switch to a smaller native mode instead of scaling the shared input.
    g_object_set(self, "thumbnail-only", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(self), source, self->modeFilter, nullptr);
    gst_element_link(source, self->modeFilter);
    auto *pad = gst_element_get_static_pad(self->modeFilter, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *pad, GstPadProbeInfo *, gpointer data) {
            auto *self = static_cast<Source *>(data);
            auto *caps = gst_pad_get_current_caps(pad);
            int width = 0;
            if (caps && gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &width))
                (self->thumbnailOnly ? thumbnailFrameWidth : fullFrameWidth).store(width);
            if (caps) gst_caps_unref(caps);
            return dropFrames.load() ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
        }, self, nullptr);
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
