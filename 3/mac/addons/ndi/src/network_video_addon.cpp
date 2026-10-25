#include "network_video_addon_api.h"
#include "addon_runtime_api.h"
#include <cstddef>
#include <Processing.NDI.Lib.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#include <cstdlib>
#include <filesystem>
#endif
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct Runtime {
#ifdef _WIN32
    HMODULE module = nullptr;
#else
    void *module = nullptr;
#endif
    const NDIlib_v6 *api = nullptr;
    ~Runtime() {
        if (api) api->destroy();
#ifdef _WIN32
        if (module) FreeLibrary(module);
#else
        if (module) dlclose(module);
#endif
    }
};
std::mutex runtimeMutex;
std::weak_ptr<Runtime> sharedRuntime;
#ifdef _WIN32
std::wstring environment(const wchar_t *name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring value(size, L'\0');
    const DWORD count = GetEnvironmentVariableW(name, value.data(), size);
    if (!count || count >= size) return {};
    value.resize(count);
    return value;
}
#endif
std::shared_ptr<Runtime> loadRuntime(int *status = nullptr, std::string *detail = nullptr) {
    if (status) *status = VtrPonAddonRuntimeReady;
    std::lock_guard lock(runtimeMutex);
    if (auto runtime = sharedRuntime.lock()) return runtime;
    auto runtime = std::make_shared<Runtime>();
    bool candidateFound = false;
#ifdef _WIN32
    std::vector<std::wstring> directories;
    const auto configured = environment(L"NDI_RUNTIME_DIR_V6");
    if (!configured.empty()) directories.push_back(configured);
    const auto programFiles = environment(L"ProgramFiles");
    if (!programFiles.empty())
        directories.push_back(programFiles + L"\\NDI\\NDI 6 Runtime\\v6");
    for (const auto &directory : directories) {
        const auto file = directory + L"\\Processing.NDI.Lib.x64.dll";
        const auto attributes = GetFileAttributesW(file.c_str());
        const auto error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        candidateFound = candidateFound || attributes != INVALID_FILE_ATTRIBUTES
            || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND);
        runtime->module = LoadLibraryExW(file.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (runtime->module) break;
    }
#else
    std::vector<std::filesystem::path> directories;
    if (const auto *configured = std::getenv("NDI_RUNTIME_DIR_V6"); configured && *configured) {
        const std::filesystem::path path(configured);
        if (path.is_absolute()) directories.push_back(path);
    }
    // Official macOS redistributable location. Never search the working
    // directory, another app's private runtime, or implicit dylib search paths.
    directories.emplace_back("/usr/local/lib");
    for (const auto &directory : directories) {
        const auto path = directory / "libndi.dylib";
        std::error_code error;
        candidateFound = std::filesystem::exists(path, error) || bool(error) || candidateFound;
        runtime->module = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (runtime->module) break;
    }
#endif
    if (!runtime->module) {
        if (status) *status = candidateFound ? VtrPonAddonRuntimeIncompatible : VtrPonAddonRuntimeMissing;
#ifdef _WIN32
        if (detail) *detail = candidateFound
            ? "The NDI Runtime DLL could not be loaded. Check the x64 installation and its dependencies."
            : "NDI Runtime 6 (x64) was not found in its registered or standard installation directory.";
#else
        if (detail) *detail = candidateFound
            ? "The NDI Runtime could not be loaded. Check this Mac's CPU architecture and runtime dependencies."
            : "NDI Runtime 6 for macOS was not found. Install the official macOS redistributable and restart VTR-PON.";
#endif
        return {};
    }
    const auto load = reinterpret_cast<const NDIlib_v6 *(*)()>(
#ifdef _WIN32
        GetProcAddress(runtime->module, "NDIlib_v6_load")
#else
        dlsym(runtime->module, "NDIlib_v6_load")
#endif
    );
    const auto *api = load ? load() : nullptr;
    if (!api || !api->initialize || !api->destroy || !api->version
        || !api->find_create_v2 || !api->find_destroy || !api->find_get_current_sources
        || !api->send_create || !api->send_destroy || !api->send_get_source_name
        || !api->send_send_video_v2 || !api->send_send_audio_v2
        || !api->recv_create_v3 || !api->recv_destroy || !api->recv_capture_v2
        || !api->recv_free_video_v2 || !api->recv_free_audio_v2) {
        if (status) *status = VtrPonAddonRuntimeIncompatible;
        if (detail) *detail = "The installed NDI Runtime does not provide the required v6 interface.";
        return {};
    }
    if (!api->initialize()) {
        if (status) *status = VtrPonAddonRuntimeError;
        if (detail) *detail = "NDI Runtime initialization failed. Check runtime and CPU compatibility.";
        return {};
    }
    runtime->api = api;
    if (detail) *detail = api->version();
    sharedRuntime = runtime;
    return runtime;
}
struct Context {
    std::shared_ptr<Runtime> runtime;
    NDIlib_find_instance_t finder = nullptr;
    ~Context() { if (finder) runtime->api->find_destroy(finder); }
};
void *create(char *error, uint32_t capacity) {
    std::string detail;
    try {
        auto runtime = loadRuntime(nullptr, &detail);
        if (runtime) return new Context{std::move(runtime)};
    } catch (...) { detail = "NDI Runtime initialization failed."; }
    if (error && capacity) {
        const auto count = std::min<size_t>(capacity - 1, detail.size());
        std::memcpy(error, detail.data(), count); error[count] = 0;
    }
    return nullptr;
}
void destroy(void *value) { delete static_cast<Context *>(value); }
int findSources(void *value, VtrPonNetworkSourceCallback callback, void *user) {
    auto &ctx = *static_cast<Context *>(value);
    const auto *api = ctx.runtime->api;
    if (!ctx.finder) {
        NDIlib_find_create_t settings;
        settings.show_local_sources = true;
        ctx.finder = api->find_create_v2(&settings);
    }
    if (!ctx.finder) return 0;
    uint32_t count = 0;
    const auto *sources = api->find_get_current_sources(ctx.finder, &count);
    for (uint32_t i = 0; sources && i < count; ++i)
        if (sources[i].p_ndi_name) callback(user, sources[i].p_ndi_name);
    return 1;
}
void *createSender(void *value, const char *name) {
    NDIlib_send_create_t settings;
    settings.p_ndi_name = name;
    // The compositor and PROGRAM audio clock already pace these streams.
    settings.clock_video = false;
    settings.clock_audio = false;
    return static_cast<Context *>(value)->runtime->api->send_create(&settings);
}
void destroySender(void *value, void *sender) {
    static_cast<Context *>(value)->runtime->api->send_destroy(static_cast<NDIlib_send_instance_t>(sender));
}
const char *senderName(void *value, void *sender) {
    const auto *source = static_cast<Context *>(value)->runtime->api->send_get_source_name(
        static_cast<NDIlib_send_instance_t>(sender));
    return source ? source->p_ndi_name : nullptr;
}
void sendVideo(void *value, void *sender, const VtrPonNetworkVideoFrame *frame) {
    if (!frame || !frame->data || frame->width <= 0 || frame->width > 8192
        || frame->height <= 0 || frame->height > 4320 || frame->stride < frame->width * 4
        || frame->fpsNumerator <= 0 || frame->fpsDenominator <= 0) return;
    NDIlib_video_frame_v2_t video;
    video.xres = frame->width; video.yres = frame->height;
    video.FourCC = frame->hasAlpha ? NDIlib_FourCC_video_type_BGRA : NDIlib_FourCC_video_type_BGRX;
    video.frame_rate_N = frame->fpsNumerator; video.frame_rate_D = frame->fpsDenominator;
    video.picture_aspect_ratio = float(frame->width) / frame->height;
    video.frame_format_type = NDIlib_frame_format_type_progressive;
    video.p_data = const_cast<uint8_t *>(frame->data);
    video.line_stride_in_bytes = frame->stride;
    video.timecode = frame->timecode;
    static_cast<Context *>(value)->runtime->api->send_send_video_v2(
        static_cast<NDIlib_send_instance_t>(sender), &video);
}
void sendAudio(void *value, void *sender, const VtrPonNetworkAudioFrame *frame) {
    if (!frame || !frame->data || frame->sampleRate <= 0 || frame->samples <= 0
        || frame->samples > 192000 || frame->channels <= 0 || frame->channels > 16) return;
    try {
        std::vector<float> planar(size_t(frame->samples) * frame->channels);
        for (int c = 0; c < frame->channels; ++c)
            for (int s = 0; s < frame->samples; ++s)
                planar[size_t(c) * frame->samples + s] = frame->data[size_t(s) * frame->channels + c];
        NDIlib_audio_frame_v2_t audio;
        audio.sample_rate = frame->sampleRate; audio.no_channels = frame->channels;
        audio.no_samples = frame->samples; audio.p_data = planar.data();
        audio.channel_stride_in_bytes = frame->samples * sizeof(float);
        audio.timecode = frame->timecode;
        static_cast<Context *>(value)->runtime->api->send_send_audio_v2(
            static_cast<NDIlib_send_instance_t>(sender), &audio);
    } catch (...) {} // Never unwind through the C ABI.
}
void *createReceiver(void *value, const char *name, int audioOnly) {
    NDIlib_recv_create_v3_t settings;
    settings.source_to_connect_to.p_ndi_name = name;
    settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    settings.bandwidth = audioOnly ? NDIlib_recv_bandwidth_audio_only : NDIlib_recv_bandwidth_highest;
    settings.allow_video_fields = false;
    settings.p_ndi_recv_name = "VTR-PON network video input";
    return static_cast<Context *>(value)->runtime->api->recv_create_v3(&settings);
}
void destroyReceiver(void *value, void *receiver) {
    static_cast<Context *>(value)->runtime->api->recv_destroy(static_cast<NDIlib_recv_instance_t>(receiver));
}
int receive(void *value, void *receiver, int audioOnly, VtrPonNetworkVideoCallback videoCallback,
            VtrPonNetworkAudioCallback audioCallback, void *user, uint32_t timeout) {
    const auto *api = static_cast<Context *>(value)->runtime->api;
    const auto recv = static_cast<NDIlib_recv_instance_t>(receiver);
    NDIlib_video_frame_v2_t video;
    NDIlib_audio_frame_v2_t audio;
    const auto type = api->recv_capture_v2(recv, audioOnly ? nullptr : &video,
        audioOnly ? &audio : nullptr, nullptr, std::min(timeout, 100u));
    int delivered = 0;
    if (type == NDIlib_frame_type_video) {
        if ((video.FourCC == NDIlib_FourCC_video_type_BGRX || video.FourCC == NDIlib_FourCC_video_type_BGRA)
            && videoCallback) {
            const VtrPonNetworkVideoFrame frame{video.p_data, video.xres, video.yres,
                video.line_stride_in_bytes, video.frame_rate_N, video.frame_rate_D,
                video.FourCC == NDIlib_FourCC_video_type_BGRA, video.timecode};
            videoCallback(user, &frame); delivered = 1;
        }
        api->recv_free_video_v2(recv, &video);
    } else if (type == NDIlib_frame_type_audio) {
        try {
            if (audio.p_data && audio.no_samples > 0 && audio.no_samples <= 192000
                && audio.no_channels > 0 && audio.no_channels <= 16 && audioCallback
                && audio.channel_stride_in_bytes >= audio.no_samples * int(sizeof(float))) {
                std::vector<float> pcm(size_t(audio.no_samples) * audio.no_channels);
                for (int c = 0; c < audio.no_channels; ++c) {
                    const auto *channel = reinterpret_cast<const float *>(
                        reinterpret_cast<const uint8_t *>(audio.p_data) + size_t(c) * audio.channel_stride_in_bytes);
                    for (int s = 0; s < audio.no_samples; ++s)
                        pcm[size_t(s) * audio.no_channels + c] = channel[s];
                }
                const VtrPonNetworkAudioFrame frame{pcm.data(), audio.sample_rate, audio.no_channels, audio.no_samples, audio.timecode};
                audioCallback(user, &frame); delivered = 1;
            }
        } catch (...) {}
        api->recv_free_audio_v2(recv, &audio);
    } else if (type == NDIlib_frame_type_error) return -1;
    return delivered;
}
const VtrPonNetworkVideoApi api{VTRPON_NETWORK_VIDEO_ABI, sizeof(VtrPonNetworkVideoApi),
    "vtr-pon-network-video-addon", create, destroy, findSources, createSender, destroySender,
    senderName, sendVideo, sendAudio, createReceiver, destroyReceiver, receive};
}
extern "C" VTRPON_NETWORK_VIDEO_EXPORT const VtrPonNetworkVideoApi *
vtrpon_network_video_get_api(uint32_t version) {
    return version == VTRPON_NETWORK_VIDEO_ABI ? &api : nullptr;
}

extern "C" VTRPON_ADDON_RUNTIME_EXPORT int32_t
vtrpon_addon_runtime_status_v1(char *detail, uint32_t capacity) {
    if (detail && capacity) *detail = '\0';
    try {
        int status = VtrPonAddonRuntimeError;
        std::string message;
        const auto runtime = loadRuntime(&status, &message);
        if (detail && capacity) {
            const auto count = std::min<size_t>(capacity - 1, message.size());
            std::memcpy(detail, message.data(), count); detail[count] = '\0';
        }
        return status;
    } catch (...) { return VtrPonAddonRuntimeError; }
}
