#include "wasapi_raw_audio_source.h"
#include "wasapi_capture_endpoint.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
struct EndpointEnumerator final : IMMDeviceEnumerator {
    int defaults = 0, explicitDevices = 0;
    std::wstring selectedId;
    EDataFlow flow = eAll;
    ERole role = eCommunications;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **value) override {
        if (value) *value = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole deviceRole,
                                                       IMMDevice **device) override {
        ++defaults; flow = dataFlow; role = deviceRole; *device = nullptr;
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice **device) override {
        ++explicitDevices; selectedId = id; *device = nullptr;
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *) override { return E_NOTIMPL; }
};

bool endpointResolution() {
    for (const auto *id : {L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}",
                          L"{2eef81be-33fa-4800-9670-1cd474972c3f}"}) {
        EndpointEnumerator enumerator;
        IMMDevice *device = nullptr;
        const auto result = vtrpon::resolveWasapiCaptureEndpoint(&enumerator, id, &device);
        if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || device || enumerator.defaults != 1
            || enumerator.explicitDevices || enumerator.flow != eCapture || enumerator.role != eConsole)
            return false;
    }
    for (const auto *id : {L"{0.0.1.00000000}.{explicit-device}", L"missing-input",
                          L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}"}) {
        EndpointEnumerator enumerator;
        IMMDevice *device = nullptr;
        const auto result = vtrpon::resolveWasapiCaptureEndpoint(&enumerator, id, &device);
        if (result != E_INVALIDARG || device || enumerator.defaults || enumerator.explicitDevices != 1
            || enumerator.selectedId != id) return false;
    }
    EndpointEnumerator enumerator;
    IMMDevice *device = nullptr;
    if (vtrpon::resolveWasapiCaptureEndpoint(&enumerator, L"", &device) != E_INVALIDARG
        || enumerator.defaults || enumerator.explicitDevices) return false;
    std::cout << "Default capture alias, console role, missing default and explicit endpoint isolation: PASS\n";
    return true;
}

struct Pipeline {
    GstElement *pipeline = gst_parse_launch(
        "vtrponwasapirawsrc name=input ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=200000000 ! "
        "audioconvert ! audioresample ! audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
        "appsink name=capture sync=false max-buffers=100 drop=true", nullptr);
    GstElement *input = pipeline ? gst_bin_get_by_name(GST_BIN(pipeline), "input") : nullptr;
    GstElement *sink = pipeline ? gst_bin_get_by_name(GST_BIN(pipeline), "capture") : nullptr;
    GstBus *bus = pipeline ? gst_element_get_bus(pipeline) : nullptr;
    ~Pipeline() {
        if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) gst_object_unref(bus);
        if (input) gst_object_unref(input);
        if (sink) gst_object_unref(sink);
        if (pipeline) gst_object_unref(pipeline);
    }
    bool start(const char *endpoint) {
        if (!input || !sink || !bus) return false;
        g_object_set(input, "device", endpoint, nullptr);
        return gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    }
    bool error() {
        auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (!message) return false;
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::cerr << (error ? error->message : "Input error") << '\n';
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        return true;
    }
};

bool invalidEndpoints() {
    for (const auto *endpoint : {"", "missing-input"}) {
        Pipeline input;
        if (!input.pipeline || !input.input) return false;
        for (int cycle = 0; cycle < 3; ++cycle) {
            (void)input.start(endpoint);
            auto *error = gst_bus_timed_pop_filtered(input.bus, 2 * GST_SECOND, GST_MESSAGE_ERROR);
            if (!error) { std::cerr << "Missing endpoint did not report a failure\n"; return false; }
            gst_message_unref(error);
            gboolean active = TRUE;
            g_object_get(input.input, "raw-active", &active, nullptr);
            auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(input.sink), 0);
            if (sample) gst_sample_unref(sample);
            if (active || sample) { std::cerr << "Invalid endpoint captured audio or claimed RAW success\n"; return false; }
            const auto started = std::chrono::steady_clock::now();
            if (gst_element_set_state(input.pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE
                || std::chrono::steady_clock::now() - started > std::chrono::seconds(1)) return false;
        }
    }
    std::cout << "Missing endpoint, no default/fallback capture, RAW failure status and repeated stop: PASS\n";
    return true;
}

// Explicit endpoint only: this manual probe never opens a default microphone,
// changes endpoint volume, or sends audio to a physical output.
bool capture(const char *endpoint, const std::string &path, int seconds, int copies) {
    struct Recording {
        Pipeline input;
        std::ofstream output;
        std::uint64_t frames = 0, packets = 0, discontinuities = 0;
        GstClockTime previous = GST_CLOCK_TIME_NONE;
        bool valid = true;
    };
    std::vector<std::unique_ptr<Recording>> recordings;
    for (int copy = 0; copy < copies; ++copy) {
        auto item = std::make_unique<Recording>();
        item->output.open(path + "." + std::to_string(copy) + ".f32", std::ios::binary);
        if (!item->output || !item->input.start(endpoint)) return false;
        recordings.push_back(std::move(item));
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
        for (auto &item : recordings) {
            if (item->input.error()) return false;
            auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(item->input.sink), 5 * GST_MSECOND);
            if (!sample) continue;
            auto *buffer = gst_sample_get_buffer(sample);
            const auto pts = GST_BUFFER_PTS(buffer);
            const auto duration = GST_BUFFER_DURATION(buffer);
            GstMapInfo map{};
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                item->valid &= map.size % (2 * sizeof(float)) == 0
                    && GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(duration)
                    && (!GST_CLOCK_TIME_IS_VALID(item->previous) || pts >= item->previous);
                item->output.write(reinterpret_cast<const char *>(map.data), map.size);
                item->frames += map.size / (2 * sizeof(float));
                item->packets++;
                item->discontinuities += GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT);
                item->previous = pts;
                gst_buffer_unmap(buffer, &map);
            } else item->valid = false;
            gst_sample_unref(sample);
        }
    }
    bool ok = true;
    for (std::size_t copy = 0; copy < recordings.size(); ++copy) {
        auto &item = *recordings[copy];
        gboolean raw = FALSE;
        g_object_get(item.input.input, "raw-active", &raw, nullptr);
        ok &= item.valid && raw && item.frames >= std::uint64_t(seconds - 1) * 48000
            && bool(item.output);
        std::cout << "copy=" << copy << " raw=" << raw << " frames=" << item.frames
                  << " packets=" << item.packets << " discontinuities=" << item.discontinuities
                  << " valid-timestamps=" << item.valid << '\n';
    }
    return ok;
}
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (!vtrpon::registerWasapiRawAudioSource()) return 1;
    if (argc >= 5 && std::string(argv[1]) == "--capture") {
        const int seconds = std::stoi(argv[4]);
        const int copies = argc > 5 ? std::stoi(argv[5]) : 1;
        if (seconds < 2 || seconds > 120 || copies < 1 || copies > 3) return 2;
        return capture(argv[2], argv[3], seconds, copies) ? 0 : 1;
    }
    return endpointResolution() && invalidEndpoints() ? 0 : 1;
}
