/* Test-process replacement for mfvideosrc. Its first read returns the same
 * diagnostics and flow error as GStreamer 1.28.5's MF Source Reader.
 * The real selected-camera input and Windows camera services are left in place. */
#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>

#define PACKAGE "vtrpon-uvc-test"

typedef struct { GstPushSrc parent; gchar *path; } TestReader;
typedef struct { GstPushSrcClass parent; } TestReaderClass;
G_DEFINE_TYPE(TestReader, test_reader, GST_TYPE_PUSH_SRC)
GST_DEBUG_CATEGORY_STATIC(reader_category);

static GstFlowReturn first_read(GstPushSrc *self, GstBuffer **buffer)
{
    const gchar *code = g_getenv("VTRPON_TEST_READ_HRESULT");
    const gchar *denied_device = g_getenv("VTRPON_TEST_DENIED_DEVICE");
    if (denied_device && ((TestReader *)self)->path &&
        g_strrstr(((TestReader *)self)->path, denied_device))
        code = "0x80070005";
    gst_debug_log(reader_category, GST_LEVEL_WARNING, __FILE__,
        "gst_mf_source_reader_read_sample", __LINE__, NULL,
        "MediaFoundation call failed: %s, injected first read", code ? code : "0xc00d36ff");
    return GST_FLOW_ERROR;
}

static void set_path(GObject *obj, guint id, const GValue *value, GParamSpec *spec)
{
    TestReader *self = (TestReader *)obj;
    g_free(self->path);
    self->path = g_value_dup_string(value);
}

static void get_path(GObject *obj, guint id, GValue *value, GParamSpec *spec)
{
    g_value_set_string(value, ((TestReader *)obj)->path);
}

static void finalize(GObject *obj)
{
    g_free(((TestReader *)obj)->path);
    G_OBJECT_CLASS(test_reader_parent_class)->finalize(obj);
}

static void test_reader_class_init(TestReaderClass *klass)
{
    static GstStaticPadTemplate pad = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC,
        GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw,format=YUY2,width=640,height=480,framerate=30/1"));
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = set_path;
    object->get_property = get_path;
    object->finalize = finalize;
    g_object_class_install_property(object, 1, g_param_spec_string("device-path",
        "Path", "Selected device interface", NULL, G_PARAM_READWRITE));
    gst_element_class_add_static_pad_template(GST_ELEMENT_CLASS(klass), &pad);
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "First-read fault fixture",
        "Source/Video", "Returns a controlled MF read failure", "VTR-PON tests");
    GST_PUSH_SRC_CLASS(klass)->create = first_read;
}

static void test_reader_init(TestReader *self)
{
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(reader_category, "mfsourceobject", 0, "Test MF source reader");
    return gst_element_register(plugin, "mfvideosrc", GST_RANK_PRIMARY, test_reader_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mediafoundation,
    "VTR-PON first-read test fixture", plugin_init, "1.0", "LGPL", PACKAGE, "https://gstreamer.freedesktop.org")
