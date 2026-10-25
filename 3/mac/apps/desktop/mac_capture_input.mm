#include "mac_capture_input.h"
#include "uvc_input_guard.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreVideo/CoreVideo.h>
#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace {
QString fromNative(NSString *value) { return value ? QString::fromUtf8(value.UTF8String) : QString{}; }

QString authorizationError(AVMediaType media, bool request) {
    const auto status = [AVCaptureDevice authorizationStatusForMediaType:media];
    if (status == AVAuthorizationStatusAuthorized) return {};
    const bool audio = [media isEqualToString:AVMediaTypeAudio];
    const auto label = audio ? QStringLiteral("microphone") : QStringLiteral("camera");
    if (status == AVAuthorizationStatusNotDetermined) {
        if (!request) return {};
        // Command-line diagnostics have no usage strings. Fail with a useful
        // error instead of letting macOS terminate an unbundled process.
        const auto key = audio ? @"NSMicrophoneUsageDescription" : @"NSCameraUsageDescription";
        if (![[NSBundle mainBundle] objectForInfoDictionaryKey:key])
            return QStringLiteral("Open the VTR-PON app to grant %1 access.").arg(label);
        static std::atomic_bool cameraPending{false}, audioPending{false};
        auto *pending = audio ? &audioPending : &cameraPending;
        if (!pending->exchange(true)) {
            [AVCaptureDevice requestAccessForMediaType:media completionHandler:^(BOOL) {
                pending->store(false);
            }];
        }
        return QStringLiteral("Waiting for %1 access. Allow access, then retry the input.").arg(label);
    }
    return QStringLiteral("%1 access is denied or restricted. Enable VTR-PON in System Settings > Privacy & Security, then retry.")
        .arg(audio ? QStringLiteral("Microphone") : QStringLiteral("Camera"));
}

NSArray<AVCaptureDevice *> *cameras() {
    NSMutableArray<AVCaptureDeviceType> *types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    if (@available(macOS 14.0, *)) {
        [types addObject:AVCaptureDeviceTypeExternal];
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
    } else {
        [types addObject:AVCaptureDeviceTypeExternalUnknown];
    }
    return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
        mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
}

QString deviceString(AudioDeviceID device, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || !value) return {};
    const auto result = fromNative((__bridge NSString *)value);
    CFRelease(value);
    return result;
}

bool hasAudioInput(AudioDeviceID device) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList)) return false;
    std::vector<unsigned char> storage(size);
    auto *buffers = reinterpret_cast<AudioBufferList *>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, buffers) != noErr) return false;
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
        if (buffers->mBuffers[i].mNumberChannels) return true;
    return false;
}

void postCaptureError(GstElement *element, const QString &detail) {
    const auto message = detail.toUtf8();
    GST_ELEMENT_ERROR(element, RESOURCE, OPEN_READ, ("%s", message.constData()), (nullptr));
}
}

// Each delegate owns its appsrc reference. Disabling, removing the delegate,
// and draining its serial queue precede destroying capture storage.
@interface VtrPonCaptureDelegate : NSObject<AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    GstElement *source;
    std::atomic_bool enabled;
    int width;
    int height;
}
- (instancetype)initWithSource:(GstElement *)appSource;
@end
@implementation VtrPonCaptureDelegate
- (instancetype)initWithSource:(GstElement *)appSource {
    self = [super init];
    if (self) { source = GST_ELEMENT(gst_object_ref(appSource)); enabled.store(true); width = height = 0; }
    return self;
}
- (void)dealloc { gst_object_unref(source); }
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection *)connection {
    (void)output; (void)connection;
    if (!enabled.load(std::memory_order_acquire)) return;
    auto pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA ||
        CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const int w = static_cast<int>(CVPixelBufferGetWidth(pixels));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pixels));
    const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
    const auto *data = static_cast<const unsigned char *>(CVPixelBufferGetBaseAddress(pixels));
    if (!data || w <= 0 || h <= 0 || stride < static_cast<size_t>(w) * 4) {
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly); return;
    }
    if (w != width || h != height) {
        auto *caps = vtrpon::macCameraFrameCaps(w, h);
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        gst_caps_unref(caps);
        width = w; height = h;
    }
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, static_cast<size_t>(w) * h * 4, nullptr);
    GstMapInfo map{};
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        for (int row = 0; row < h; ++row)
            std::memcpy(map.data + static_cast<size_t>(row) * w * 4, data + row * stride, static_cast<size_t>(w) * 4);
        gst_buffer_unmap(buffer, &map);
    } else { if (buffer) gst_buffer_unref(buffer); buffer = nullptr; }
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    // appsrc timestamps in the shared pipeline clock and drops old buffers.
    if (buffer) (void)gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
}
@end

namespace {
struct VideoState {
    AVCaptureSession *session = nil;
    AVCaptureVideoDataOutput *output = nil;
    VtrPonCaptureDelegate *delegate = nil;
    dispatch_queue_t queue = nullptr;
    void clear() {
        if (delegate) delegate->enabled.store(false, std::memory_order_release);
        [output setSampleBufferDelegate:nil queue:nullptr];
        [session stopRunning];
        if (queue) dispatch_sync(queue, ^{});
        delegate = nil; output = nil; session = nil; queue = nullptr;
    }
};
struct MacVideoSource { GstBin parent; GstElement *appSource; gchar *device; VideoState *capture; };
struct MacVideoSourceClass { GstBinClass parent; };
G_DEFINE_TYPE(MacVideoSource, mac_video_source, GST_TYPE_BIN)

bool openVideo(MacVideoSource *self) {
    @autoreleasepool {
        const auto path = QString::fromUtf8(self->device ? self->device : "");
        AVCaptureDevice *selected = nil;
        if (path.startsWith(QLatin1StringView("avfoundation:"))) {
            const auto uid = path.mid(13);
            for (AVCaptureDevice *device in cameras())
                if (fromNative(device.uniqueID) == uid && device.connected) { selected = device; break; }
        }
        QString error;
        if (!selected) error = QStringLiteral("The selected Mac camera is missing or disconnected. No other camera was opened.");
        else if (!vtrpon::uvcInputAllowed(path, &error)) {}
        else error = authorizationError(AVMediaTypeVideo, true);
        if (!error.isEmpty()) { postCaptureError(GST_ELEMENT(self), error); return false; }
        NSError *nativeError = nil;
        auto *input = [AVCaptureDeviceInput deviceInputWithDevice:selected error:&nativeError];
        if (!input) { postCaptureError(GST_ELEMENT(self), fromNative(nativeError.localizedDescription)); return false; }
        auto &state = *self->capture;
        state.session = [[AVCaptureSession alloc] init];
        state.output = [[AVCaptureVideoDataOutput alloc] init];
        state.output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        state.output.alwaysDiscardsLateVideoFrames = YES;
        [state.session beginConfiguration];
        if ([state.session canSetSessionPreset:AVCaptureSessionPreset1920x1080])
            state.session.sessionPreset = AVCaptureSessionPreset1920x1080;
        if (![state.session canAddInput:input] || ![state.session canAddOutput:state.output]) {
            [state.session commitConfiguration]; state.clear();
            postCaptureError(GST_ELEMENT(self), QStringLiteral("Could not configure the selected camera input and BGRA output.")); return false;
        }
        [state.session addInput:input]; [state.session addOutput:state.output];
        [state.session commitConfiguration];
        state.queue = dispatch_queue_create("com.pondashi.vtrpon.capture", DISPATCH_QUEUE_SERIAL);
        state.delegate = [[VtrPonCaptureDelegate alloc] initWithSource:self->appSource];
        [state.output setSampleBufferDelegate:state.delegate queue:state.queue];
        return true;
    }
}
GstStateChangeReturn videoChangeState(GstElement *element, GstStateChange change) {
    auto *self = reinterpret_cast<MacVideoSource *>(element);
    @autoreleasepool {
        if (change == GST_STATE_CHANGE_NULL_TO_READY && !openVideo(self)) return GST_STATE_CHANGE_FAILURE;
        if (change == GST_STATE_CHANGE_PAUSED_TO_READY) {
            if (self->capture->delegate) self->capture->delegate->enabled.store(false);
            [self->capture->session stopRunning];
        }
        const auto result = GST_ELEMENT_CLASS(mac_video_source_parent_class)->change_state(element, change);
        if (result == GST_STATE_CHANGE_FAILURE) return result;
        if (change == GST_STATE_CHANGE_READY_TO_PAUSED) {
            if (self->capture->delegate) self->capture->delegate->enabled.store(true);
            [self->capture->session startRunning];
            if (!self->capture->session.running) {
                postCaptureError(element, QStringLiteral("The selected camera could not start capture."));
                return GST_STATE_CHANGE_FAILURE;
            }
        }
        if (change == GST_STATE_CHANGE_READY_TO_NULL) self->capture->clear();
        return result;
    }
}
void mac_video_source_class_init(MacVideoSourceClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = [](GObject *value, guint, const GValue *data, GParamSpec *) {
        auto *self = reinterpret_cast<MacVideoSource *>(value);
        g_free(self->device); self->device = g_value_dup_string(data);
    };
    object->get_property = [](GObject *value, guint, GValue *data, GParamSpec *) {
        g_value_set_string(data, reinterpret_cast<MacVideoSource *>(value)->device);
    };
    object->finalize = [](GObject *value) {
        auto *self = reinterpret_cast<MacVideoSource *>(value);
        @autoreleasepool { self->capture->clear(); }
        delete self->capture; g_free(self->device);
        G_OBJECT_CLASS(mac_video_source_parent_class)->finalize(value);
    };
    g_object_class_install_property(object, 1, g_param_spec_string("device", "Device", "AVFoundation unique identity", "", G_PARAM_READWRITE));
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "VTR-PON Mac camera", "Source/Video", "Camera capture by persistent ID", "VTR-PON");
    GST_ELEMENT_CLASS(klass)->change_state = videoChangeState;
}
void mac_video_source_init(MacVideoSource *self) {
    self->capture = new VideoState;
    self->appSource = gst_element_factory_make("appsrc", nullptr);
    g_object_set(self->appSource, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE,
        "block", FALSE, "max-buffers", guint64(2), "max-bytes", guint64(0), "leaky-type", 2, nullptr);
    gst_bin_add(GST_BIN(self), self->appSource);
    auto *pad = gst_element_get_static_pad(self->appSource, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}

struct MacAudioSource { GstBin parent; GstElement *input; gchar *device; };
struct MacAudioSourceClass { GstBinClass parent; };
G_DEFINE_TYPE(MacAudioSource, mac_audio_source, GST_TYPE_BIN)
GstStateChangeReturn audioChangeState(GstElement *element, GstStateChange change) {
    auto *self = reinterpret_cast<MacAudioSource *>(element);
    if (change == GST_STATE_CHANGE_NULL_TO_READY) {
        @autoreleasepool {
            const auto id = QString::fromUtf8(self->device ? self->device : "");
            QString error;
            const auto devices = vtrpon::macAudioInputDevices(&error);
            const bool found = std::any_of(devices.begin(), devices.end(), [&](const auto &device) { return device.deviceId == id; });
            if (!found) error = QStringLiteral("The selected Mac audio input is missing or disconnected. No other input was opened.");
            if (error.isEmpty()) error = authorizationError(AVMediaTypeAudio, true);
            if (!self->input || !g_object_class_find_property(G_OBJECT_GET_CLASS(self->input), "unique-id"))
                error = QStringLiteral("Mac audio input requires osxaudiosrc with unique-id support (GStreamer 1.26 or newer).");
            if (!error.isEmpty()) { postCaptureError(element, error); return GST_STATE_CHANGE_FAILURE; }
            const auto uid = id.mid(16).toUtf8();
            g_object_set(self->input, "unique-id", uid.constData(), "provide-clock", FALSE, nullptr);
        }
    }
    return GST_ELEMENT_CLASS(mac_audio_source_parent_class)->change_state(element, change);
}
void mac_audio_source_class_init(MacAudioSourceClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = [](GObject *value, guint, const GValue *data, GParamSpec *) {
        auto *self = reinterpret_cast<MacAudioSource *>(value);
        g_free(self->device); self->device = g_value_dup_string(data);
    };
    object->get_property = [](GObject *value, guint, GValue *data, GParamSpec *) { g_value_set_string(data, reinterpret_cast<MacAudioSource *>(value)->device); };
    object->finalize = [](GObject *value) {
        g_free(reinterpret_cast<MacAudioSource *>(value)->device);
        G_OBJECT_CLASS(mac_audio_source_parent_class)->finalize(value);
    };
    g_object_class_install_property(object, 1, g_param_spec_string("device", "Device", "CoreAudio input unique identity", "", G_PARAM_READWRITE));
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "VTR-PON Mac audio input", "Source/Audio", "CoreAudio input by persistent ID", "VTR-PON");
    GST_ELEMENT_CLASS(klass)->change_state = audioChangeState;
}
void mac_audio_source_init(MacAudioSource *self) {
    self->input = gst_element_factory_make("osxaudiosrc", nullptr);
    if (!self->input) return;
    gst_bin_add(GST_BIN(self), self->input);
    vtrpon::installMacAudioClockMapping(self->input);
    auto *pad = gst_element_get_static_pad(self->input, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}
}

QList<UvcDevice> vtrpon::macCameraDevices(QString *error) {
    @autoreleasepool {
        if (error) *error = authorizationError(AVMediaTypeVideo, false);
        QList<UvcDevice> result;
        for (AVCaptureDevice *device in cameras()) {
            const auto path = macCameraPath(fromNative(device.uniqueID));
            if (device.connected && vtrpon::uvcInputAllowed(path)) result.append({fromNative(device.localizedName), path});
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            const int order = QString::localeAwareCompare(a.displayName, b.displayName);
            return order ? order < 0 : a.devicePath < b.devicePath;
        });
        return result;
    }
}
QList<AudioInputDevice> vtrpon::macAudioInputDevices(QString *error) {
    @autoreleasepool {
        if (error) *error = authorizationError(AVMediaTypeAudio, false);
        AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
            if (error) *error = QStringLiteral("Could not enumerate CoreAudio input devices."); return {};
        }
        std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) {
            if (error) *error = QStringLiteral("Could not read CoreAudio input devices."); return {};
        }
        QList<AudioInputDevice> result;
        for (auto device : devices) {
            if (!hasAudioInput(device)) continue;
            const auto uid = deviceString(device, kAudioDevicePropertyDeviceUID);
            const auto name = deviceString(device, kAudioObjectPropertyName);
            if (!uid.isEmpty() && !name.isEmpty()) result.append({name, macAudioInputId(uid), false});
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            const int order = QString::localeAwareCompare(a.displayName, b.displayName);
            return order ? order < 0 : a.deviceId < b.deviceId;
        });
        return result;
    }
}
bool vtrpon::registerMacCaptureSources() {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const auto add = [](const char *name, GType type) {
        if (auto *factory = gst_element_factory_find(name)) { gst_object_unref(factory); return true; }
        return gst_element_register(nullptr, name, GST_RANK_NONE, type) != FALSE;
    };
    return add("vtrponavfvideosrc", mac_video_source_get_type()) && add("vtrponcoreaudiosrc", mac_audio_source_get_type());
}

GstCaps *vtrpon::macCameraFrameCaps(int width, int height) {
    return gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA",
        "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
        // AVFoundation delivers timestamped frames and may change capture rate.
        // Declare variable rate; omitting framerate makes videorate reject caps.
        "framerate", GST_TYPE_FRACTION, 0, 1,
        "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
}

// osxaudiosrc's ring-buffer timestamps use the CoreAudio sample clock. In
// GStreamer 1.28.1 GstAudioBaseSrc subtracts the external system base-time from
// those timestamps and clamps every block to zero. Map at this native boundary:
// anchor capture to the pipeline running-time, then keep sample-exact cadence.
// AudioStreamAlign tolerates delivery jitter and reanchors after real clock
// drift/discontinuity; never change file/other producers in the PCM bridge.
bool vtrpon::installMacAudioClockMapping(GstElement *source) {
    auto *pad = gst_element_get_static_pad(source, "src");
    if (!pad) return false;
    struct CaptureTiming {
        GstAudioInfo format{};
        GstAudioStreamAlign *align = gst_audio_stream_align_new(48000, 40 * GST_MSECOND, GST_SECOND);
        ~CaptureTiming() { gst_audio_stream_align_free(align); }
    };
    gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
        [](GstPad *pad, GstPadProbeInfo *probe, gpointer data) {
            auto &timing = *static_cast<CaptureTiming *>(data);
            if (GST_PAD_PROBE_INFO_TYPE(probe) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
                auto *event = GST_PAD_PROBE_INFO_EVENT(probe);
                switch (GST_EVENT_TYPE(event)) {
                case GST_EVENT_CAPS: {
                    GstCaps *caps = nullptr;
                    gst_event_parse_caps(event, &caps);
                    if (gst_audio_info_from_caps(&timing.format, caps))
                        gst_audio_stream_align_set_rate(timing.align, GST_AUDIO_INFO_RATE(&timing.format));
                    gst_audio_stream_align_mark_discont(timing.align);
                    break;
                }
                case GST_EVENT_STREAM_START:
                case GST_EVENT_SEGMENT:
                    gst_audio_stream_align_mark_discont(timing.align);
                    break;
                default: break;
                }
                return GST_PAD_PROBE_OK;
            }
            if (!GST_AUDIO_INFO_IS_VALID(&timing.format)) return GST_PAD_PROBE_OK;
            auto *buffer = GST_PAD_PROBE_INFO_BUFFER(probe);
            const auto frames = gst_buffer_get_size(buffer) / GST_AUDIO_INFO_BPF(&timing.format);
            if (!frames || frames > G_MAXUINT) return GST_PAD_PROBE_OK;
            auto *element = gst_pad_get_parent_element(pad);
            auto *clock = element ? gst_element_get_clock(element) : nullptr;
            if (!clock) {
                if (element) gst_object_unref(element);
                return GST_PAD_PROBE_DROP;
            }
            const auto now = gst_clock_get_time(clock);
            const auto base = gst_element_get_base_time(element);
            gst_object_unref(clock); gst_object_unref(element);
            if (!GST_CLOCK_TIME_IS_VALID(base)) return GST_PAD_PROBE_DROP;
            const auto duration = gst_util_uint64_scale(frames, GST_SECOND, GST_AUDIO_INFO_RATE(&timing.format));
            const auto running = now > base ? now - base : 0;
            const auto captured = running > duration ? running - duration : 0;
            GstClockTime pts, alignedDuration;
            const bool discont = gst_audio_stream_align_process(timing.align,
                GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT), captured,
                guint(frames), &pts, &alignedDuration, nullptr);
            buffer = gst_buffer_make_writable(buffer);
            GST_PAD_PROBE_INFO_DATA(probe) = buffer;
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = alignedDuration;
            if (discont) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
            return GST_PAD_PROBE_OK;
        }, new CaptureTiming, [](gpointer data) { delete static_cast<CaptureTiming *>(data); });
    gst_object_unref(pad);
    return true;
}
