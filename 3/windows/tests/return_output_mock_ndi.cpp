#include "network_video_addon_api.h"
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>

// This fixture lives only beside return-output-routing-test, never beside VTR-PON.
namespace {
std::mutex mutex;
std::array<std::array<uint64_t, 10>, 2> records{};
std::atomic_bool discoveryFailure{false};
std::atomic_uint discoveryChecks{0};
struct Sender { std::string name; unsigned route; };
void *create(char *, uint32_t) { return new int(1); }
void destroy(void *context) { delete static_cast<int *>(context); }
int find(void *, VtrPonNetworkSourceCallback callback, void *user) {
    ++discoveryChecks;
    if (discoveryFailure.load()) return 0;
    callback(user, "TEST HOST (Return-test)");
    callback(user, "TEST HOST (Return-test Return Monitor)");
    return 1;
}
void *createSender(void *, const char *name) {
    auto *sender = new Sender{std::string("TEST HOST (") + name + ')',
        std::string(name).ends_with(" Return Monitor") ? 1u : 0u};
    return sender;
}
void destroySender(void *, void *sender) { delete static_cast<Sender *>(sender); }
const char *senderName(void *, void *sender) { return static_cast<Sender *>(sender)->name.c_str(); }
void sendVideo(void *, void *handle, const VtrPonNetworkVideoFrame *frame) {
    std::lock_guard lock(mutex);
    auto &record = records[static_cast<Sender *>(handle)->route];
    ++record[0]; record[1] = frame->width; record[2] = frame->height; record[3] = frame->fpsNumerator;
    const auto pixel = [&](int x, int y) { uint32_t value; std::memcpy(&value, frame->data + y * frame->stride + x * 4, 4); return value; };
    record[4] = pixel(0, 0); record[5] = pixel(frame->width / 2, frame->height / 4);
    record[6] = 0;
    for (int y = frame->height * 4 / 5; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x)
            if ((pixel(x, y) & 0xffffffu) == 0xffffffu) ++record[6];
    record[7] = frame->timecode; record[9] = frame->hasAlpha;
}
void sendAudio(void *, void *handle, const VtrPonNetworkAudioFrame *) {
    std::lock_guard lock(mutex); ++records[static_cast<Sender *>(handle)->route][8];
}
void *createReceiver(void *, const char *, int) { return nullptr; }
void destroyReceiver(void *, void *) {}
int receive(void *, void *, int, VtrPonNetworkVideoCallback, VtrPonNetworkAudioCallback, void *, uint32_t timeout) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout)); return 0;
}
const VtrPonNetworkVideoApi api{2, sizeof(api), "vtr-pon-network-video-addon", create, destroy, find,
    createSender, destroySender, senderName, sendVideo, sendAudio, createReceiver, destroyReceiver, receive};
}
extern "C" VTRPON_NETWORK_VIDEO_EXPORT const VtrPonNetworkVideoApi *vtrpon_network_video_get_api(uint32_t version) {
    return version == 2 ? &api : nullptr;
}
extern "C" __declspec(dllexport) void vtrpon_return_test_stats(uint64_t *values) {
    std::lock_guard lock(mutex); std::memcpy(values, records.data(), sizeof(records));
}
extern "C" __declspec(dllexport) void vtrpon_return_test_discovery_failure(int failed) {
    discoveryFailure.store(failed != 0);
}
extern "C" __declspec(dllexport) unsigned vtrpon_return_test_discovery_checks() {
    return discoveryChecks.load();
}
