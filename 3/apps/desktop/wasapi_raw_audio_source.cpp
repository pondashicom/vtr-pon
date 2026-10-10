#include "wasapi_raw_audio_source.h"

#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include "wasapi_capture_endpoint.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
constexpr GstClockTime kQueueLimit = 200 * GST_MSECOND;

struct CaptureState {
    GstElement *appSource = nullptr; // Owned by the bin, until after stop().
    std::string device;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    std::atomic_bool rawActive{false};

    void stop() {
        if (stopEvent) SetEvent(stopEvent);
        if (worker.joinable()) worker.join();
        rawActive.store(false);
    }
    ~CaptureState() {
        stop();
        if (stopEvent) CloseHandle(stopEvent);
    }
};
struct VtrponWasapiRawSource { GstBin parent; CaptureState *capture; };
struct VtrponWasapiRawSourceClass { GstBinClass parent; };
G_DEFINE_TYPE(VtrponWasapiRawSource, vtrpon_wasapi_raw_source, GST_TYPE_BIN)

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct CaptureResources {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient2> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX *format = nullptr;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool started = false;
    ~CaptureResources() {
        if (started) client->Stop();
        // All COM interfaces and buffers are released on the capture thread.
        capture.Reset();
        client.Reset();
        device.Reset();
        enumerator.Reset();
        CoTaskMemFree(format);
        if (ready) CloseHandle(ready);
    }
};

bool check(VtrponWasapiRawSource *self, HRESULT result, const char *operation) {
    if (SUCCEEDED(result)) return true;
    // Use the function directly: GST_ELEMENT_ERROR imports debug globals that
    // cannot be linked with the application's MSVC delay-loaded GStreamer DLL.
    gst_element_message_full(GST_ELEMENT(self), GST_MESSAGE_ERROR,
        GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ,
        g_strdup_printf("Could not capture the selected audio input in RAW mode (%s, HRESULT 0x%08lx). "
                        "Check device availability and RAW support.", operation, static_cast<unsigned long>(result)),
        g_strdup("Shared RAW capture has no processed-input fallback."), __FILE__, G_STRFUNC, __LINE__);
    return false;
}

GstCaps *captureCaps(const WAVEFORMATEX *format) {
    if (!format || !format->nSamplesPerSec || format->nSamplesPerSec > G_MAXINT
        || !format->nChannels || format->nChannels > 18) return nullptr;
    WORD tag = format->wFormatTag;
    DWORD mask = 0;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return nullptr;
        const auto *extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
        else return nullptr;
        if (!extended->Samples.wValidBitsPerSample
            || extended->Samples.wValidBitsPerSample > format->wBitsPerSample) return nullptr;
        mask = extended->dwChannelMask;
    }
    GstAudioFormat sample = GST_AUDIO_FORMAT_UNKNOWN;
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (format->wBitsPerSample == 32) sample = GST_AUDIO_FORMAT_F32LE;
        else if (format->wBitsPerSample == 64) sample = GST_AUDIO_FORMAT_F64LE;
    } else if (tag == WAVE_FORMAT_PCM) {
        switch (format->wBitsPerSample) {
        case 8: sample = GST_AUDIO_FORMAT_U8; break;
        case 16: sample = GST_AUDIO_FORMAT_S16LE; break;
        case 24: sample = GST_AUDIO_FORMAT_S24LE; break;
        case 32: sample = GST_AUDIO_FORMAT_S32LE; break; // Valid bits are left aligned by WASAPI.
        }
    }
    if (sample == GST_AUDIO_FORMAT_UNKNOWN
        || format->nBlockAlign != format->nChannels * format->wBitsPerSample / 8) return nullptr;

    // Windows speaker-bit order differs from GStreamer's above REAR_CENTER.
    constexpr GstAudioChannelPosition speakers[] = {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_LFE1,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
        GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
        GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_TOP_CENTER,
        GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER,
        GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT
    };
    GstAudioChannelPosition positions[18]{};
    if (format->nChannels == 1) positions[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
    else if (!mask && format->nChannels == 2) {
        positions[0] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
        positions[1] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
    } else {
        unsigned count = 0;
        if (mask >> 18) return nullptr;
        for (unsigned bit = 0; bit < 18; ++bit)
            if (mask & (1u << bit)) positions[count++] = speakers[bit];
        if (count != format->nChannels) return nullptr;
        // Fail ambiguous/noncanonical layouts instead of silently relabeling PCM.
        if (!gst_audio_check_valid_channel_positions(positions, count, TRUE)) return nullptr;
    }
    GstAudioInfo info;
    gst_audio_info_init(&info);
    gst_audio_info_set_format(&info, sample, format->nSamplesPerSec, format->nChannels, positions);
    return gst_audio_info_to_caps(&info);
}

GstClockTime captureTime(GstElement *element, UINT64 qpc100ns, DWORD flags,
                         GstClockTime duration) {
    GstClock *clock = gst_element_get_clock(element);
    if (!clock) return GST_CLOCK_TIME_NONE;
    const auto now = gst_clock_get_time(clock);
    const auto base = gst_element_get_base_time(element);
    gst_object_unref(clock);
    if (!GST_CLOCK_TIME_IS_VALID(base) || now < base) return GST_CLOCK_TIME_NONE;
    GstClockTime age = duration;
    LARGE_INTEGER counter{}, frequency{};
    if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc100ns
        && QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency)) {
        const auto qpcNow = gst_util_uint64_scale(counter.QuadPart, GST_SECOND, frequency.QuadPart);
        const auto packet = qpc100ns * 100;
        age = qpcNow > packet ? qpcNow - packet : 0;
    }
    // Discard old/early packets instead of replaying stale audio after a pause.
    return age > kQueueLimit || now - base < age ? GST_CLOCK_TIME_NONE : now - base - age;
}

void captureLoop(VtrponWasapiRawSource *self, const std::string &endpoint) {
    auto &state = *self->capture;
    ComScope com;
    if (!check(self, com.result, "CoInitializeEx")) return;
    CaptureResources resources;
    if (!resources.ready) { check(self, HRESULT_FROM_WIN32(GetLastError()), "CreateEvent"); return; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint.c_str(), -1, nullptr, 0);
    if (!length) { check(self, E_INVALIDARG, "Endpoint ID"); return; }
    std::wstring wide(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint.c_str(), -1, wide.data(), length)) {
        check(self, HRESULT_FROM_WIN32(GetLastError()), "Endpoint ID"); return;
    }
    if (!check(self, CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
               IID_PPV_ARGS(&resources.enumerator)), "MMDeviceEnumerator")
        || !check(self, vtrpon::resolveWasapiCaptureEndpoint(resources.enumerator.Get(),
               wide.c_str(), resources.device.GetAddressOf()), "Resolve selected capture endpoint")
        || !check(self, resources.device->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr,
               reinterpret_cast<void **>(resources.client.GetAddressOf())), "IAudioClient2")) return;
    ComPtr<IMMEndpoint> captureEndpoint;
    EDataFlow flow = eAll;
    if (!check(self, resources.device.As(&captureEndpoint), "IMMEndpoint")
        || !check(self, captureEndpoint->GetDataFlow(&flow), "GetDataFlow")) return;
    if (flow != eCapture) { check(self, E_INVALIDARG, "Capture endpoint required"); return; }

    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.eCategory = AudioCategory_Other;
    properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
    if (!check(self, resources.client->SetClientProperties(&properties), "SetClientProperties(RAW)")
        || !check(self, resources.client->GetMixFormat(&resources.format), "GetMixFormat")) return;
    GstCaps *caps = captureCaps(resources.format);
    if (!caps) { check(self, AUDCLNT_E_UNSUPPORTED_FORMAT, "Capture PCM format/channel layout"); return; }
    gst_app_src_set_caps(GST_APP_SRC(state.appSource), caps);
    gst_caps_unref(caps);
    REFERENCE_TIME period = 0;
    if (!check(self, resources.client->GetDevicePeriod(&period, nullptr), "GetDevicePeriod")
        || !check(self, resources.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, resources.format, nullptr), "Initialize(RAW)")
        || !check(self, resources.client->SetEventHandle(resources.ready), "SetEventHandle")
        || !check(self, resources.client->GetService(IID_PPV_ARGS(&resources.capture)), "IAudioCaptureClient")
        || !check(self, resources.client->Start(), "Start")) return;
    resources.started = true;
    gst_app_src_set_latency(GST_APP_SRC(state.appSource), period * 100, kQueueLimit);
    state.rawActive.store(true);
    bool first = true;
    GstClockTime previousPts = GST_CLOCK_TIME_NONE;
    HANDLE events[] = {state.stopEvent, resources.ready};
    while (true) {
        const auto wait = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) {
            check(self, wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                            : HRESULT_FROM_WIN32(GetLastError()), "Capture event");
            break;
        }
        UINT32 pending = 0;
        if (!check(self, resources.capture->GetNextPacketSize(&pending), "GetNextPacketSize")) break;
        while (pending && WaitForSingleObject(state.stopEvent, 0) != WAIT_OBJECT_0) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            if (!check(self, resources.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc), "GetBuffer")) return;
            if (!frames) break;
            const auto bytes = static_cast<gsize>(frames) * resources.format->nBlockAlign;
            GstBuffer *buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
            if (buffer) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // PCM unsigned 8-bit silence is 128, all other supported formats use zero.
                    gst_buffer_memset(buffer, 0, resources.format->wBitsPerSample == 8 ? 128 : 0, bytes);
                } else if (data) gst_buffer_fill(buffer, 0, data, bytes);
                else { gst_buffer_unref(buffer); buffer = nullptr; }
            }
            if (!check(self, resources.capture->ReleaseBuffer(frames), "ReleaseBuffer")) {
                if (buffer) gst_buffer_unref(buffer);
                return;
            }
            if (!buffer) { check(self, E_OUTOFMEMORY, "Copy capture packet"); return; }
            const auto duration = gst_util_uint64_scale(frames, GST_SECOND, resources.format->nSamplesPerSec);
            const auto pts = captureTime(GST_ELEMENT(self), qpc, flags, duration);
            const bool discontinuity = first || (flags & (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
                                                        | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR));
            if (!GST_CLOCK_TIME_IS_VALID(pts)
                || (GST_CLOCK_TIME_IS_VALID(previousPts) && pts < previousPts)) {
                gst_buffer_unref(buffer);
                first = true;
            } else {
                // Timestamp every packet against the pipeline clock so the device's
                // nominal sample rate cannot accumulate a wall-clock drift.
                GST_BUFFER_PTS(buffer) = pts;
                previousPts = pts;
                GST_BUFFER_DURATION(buffer) = duration;
                if (discontinuity) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
                first = false;
                const auto pushed = gst_app_src_push_buffer(GST_APP_SRC(state.appSource), buffer);
                if (pushed == GST_FLOW_FLUSHING) return;
                if (pushed != GST_FLOW_OK) { check(self, E_FAIL, "Publish capture PCM"); return; }
            }
            if (!check(self, resources.capture->GetNextPacketSize(&pending), "GetNextPacketSize")) return;
        }
    }
}

GstStateChangeReturn changeState(GstElement *element, GstStateChange transition) {
    auto *self = reinterpret_cast<VtrponWasapiRawSource *>(element);
    auto &capture = *self->capture;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY
        && (capture.device.empty() || !capture.appSource || !capture.stopEvent)) {
        check(self, E_INVALIDARG, "Selected capture endpoint required");
        return GST_STATE_CHANGE_FAILURE;
    }
    if (transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED) capture.stop();
    const auto result = GST_ELEMENT_CLASS(vtrpon_wasapi_raw_source_parent_class)->change_state(element, transition);
    if (result == GST_STATE_CHANGE_FAILURE) return result;
    if (transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING) {
        ResetEvent(capture.stopEvent);
        const auto endpoint = capture.device;
        try {
            capture.worker = std::thread([self, endpoint] {
                try { captureLoop(self, endpoint); }
                catch (...) { check(self, E_FAIL, "Capture worker"); }
                self->capture->rawActive.store(false);
            });
        } catch (...) {
            check(self, E_OUTOFMEMORY, "Start capture worker");
            return GST_STATE_CHANGE_FAILURE;
        }
    }
    return result;
}

void vtrpon_wasapi_raw_source_class_init(VtrponWasapiRawSourceClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = [](GObject *object, guint id, const GValue *value, GParamSpec *spec) {
        auto &state = *reinterpret_cast<VtrponWasapiRawSource *>(object)->capture;
        if (id == 1) { const auto *text = g_value_get_string(value); state.device = text ? text : ""; }
        else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    };
    object->get_property = [](GObject *object, guint id, GValue *value, GParamSpec *spec) {
        auto &state = *reinterpret_cast<VtrponWasapiRawSource *>(object)->capture;
        if (id == 1) g_value_set_string(value, state.device.c_str());
        else if (id == 2) g_value_set_boolean(value, state.rawActive.load());
        else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    };
    object->dispose = [](GObject *object) {
        reinterpret_cast<VtrponWasapiRawSource *>(object)->capture->stop();
        G_OBJECT_CLASS(vtrpon_wasapi_raw_source_parent_class)->dispose(object);
    };
    object->finalize = [](GObject *object) {
        delete reinterpret_cast<VtrponWasapiRawSource *>(object)->capture;
        G_OBJECT_CLASS(vtrpon_wasapi_raw_source_parent_class)->finalize(object);
    };
    g_object_class_install_property(object, 1, g_param_spec_string("device", "Device",
        "Selected WASAPI endpoint ID or explicit Default capture alias", "", G_PARAM_READWRITE));
    g_object_class_install_property(object, 2, g_param_spec_boolean("raw-active", "RAW active",
        "Shared RAW capture successfully initialized and running", FALSE, G_PARAM_READABLE));
    auto *element = GST_ELEMENT_CLASS(klass);
    element->change_state = changeState;
    gst_element_class_set_static_metadata(element, "VTR-PON shared RAW audio input",
        "Source/Audio/Hardware", "Selected Windows endpoint without optional signal processing", "VTR-PON");
    GstCaps *caps = gst_caps_from_string("audio/x-raw,layout=interleaved");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
}

void vtrpon_wasapi_raw_source_init(VtrponWasapiRawSource *self) {
    self->capture = new CaptureState;
    auto *source = gst_element_factory_make("appsrc", "raw-capture-pcm");
    if (!source) return;
    self->capture->appSource = source;
    g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
        "max-bytes", guint64{0}, "max-buffers", guint64{0}, "max-time", kQueueLimit,
        "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM, "emit-signals", FALSE, nullptr);
    gst_bin_add(GST_BIN(self), source);
    GstPad *pad = gst_element_get_static_pad(source, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}
} // namespace
#endif

bool vtrpon::registerWasapiRawAudioSource() {
    if (!gst_is_initialized()) return false;
    static std::mutex registration;
    std::lock_guard lock(registration);
    if (auto *factory = gst_element_factory_find("vtrponwasapirawsrc")) {
        gst_object_unref(factory);
        return true;
    }
#ifdef _WIN32
    return gst_element_register(nullptr, "vtrponwasapirawsrc", GST_RANK_NONE,
                                vtrpon_wasapi_raw_source_get_type());
#else
    return false;
#endif
}
