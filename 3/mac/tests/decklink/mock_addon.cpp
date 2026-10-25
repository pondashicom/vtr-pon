#include "decklink_addon_api.h"
#include "decklink_media_api.h"
#include "decklink_output_api.h"
#include <array>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <new>
#include <thread>

namespace {
std::atomic<uint32_t> inputOpens{0}, inputs{0}, videoSends{0}, audioSends{0}, outputPumps{0};
struct OutputStats {
    std::atomic<uint64_t> video{0}, audio{0}, time{0}, color{0}, active{0}, group{0}, opens{0};
    std::atomic<uint32_t> expected{0xffc8280a};
};
std::array<OutputStats, 3> outputs;
struct Context {
    std::thread::id owner = std::this_thread::get_id(); bool snapshot = false, input = false, output = false;
    uint64_t frame = 0; unsigned route = 0; int64_t group = 0, epoch = 0; uint32_t mode = 0; bool running = false;
    std::vector<uint8_t> video;
    std::vector<int32_t> audio;
    ~Context() { if (input) --inputs; if (output) --outputs[route].active; }
};
void message(char *out, uint32_t capacity, const char *text) noexcept {
    if (!out || !capacity) return;
    const auto length = std::min<std::size_t>(std::strlen(text), capacity - 1);
    std::memcpy(out, text, length); out[length] = 0;
}
int32_t check(void *context, char *error, uint32_t capacity) noexcept {
    message(error, capacity, "");
    if (!context) { message(error, capacity, "Context is required."); return VtrPonDeckLinkInvalidArgument; }
    if (static_cast<Context *>(context)->owner != std::this_thread::get_id()) {
        message(error, capacity, "Use the creating thread."); return VtrPonDeckLinkWrongThread;
    }
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL create(void **context, char *error, uint32_t capacity) noexcept {
    message(error, capacity, "");
    if (!context) return VtrPonDeckLinkInvalidArgument;
    *context = new (std::nothrow) Context;
    return *context ? VtrPonDeckLinkOk : VtrPonDeckLinkBackendError;
}
int32_t VTRPON_DECKLINK_CALL destroy(void *context, char *error, uint32_t capacity) noexcept {
    if (!context) { message(error, capacity, ""); return VtrPonDeckLinkOk; }
    const auto status = check(context, error, capacity);
    if (status == VtrPonDeckLinkOk) delete static_cast<Context *>(context);
    return status;
}
int32_t VTRPON_DECKLINK_CALL devices(void *context, VtrPonDeckLinkDevice *out,
    uint32_t capacity, uint32_t *count, char *error, uint32_t errorCapacity) noexcept {
    if (count) *count = 0;
    const auto status = check(context, error, errorCapacity);
    if (status != VtrPonDeckLinkOk) return status;
    if (!count || (!out && capacity)) return VtrPonDeckLinkInvalidArgument;
    static_cast<Context *>(context)->snapshot = true;
    if (std::getenv("VTRPON_DECKLINK_TEST_NO_DEVICES")) { *count = 0; return VtrPonDeckLinkOk; }
    *count = 2;
    if (capacity < *count) return VtrPonDeckLinkBufferTooSmall;
    VtrPonDeckLinkDevice records[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        records[i].structSize = sizeof(records[i]); records[i].index = i;
        records[i].directions = i == 0 ? 3u : 1u; records[i].hasPersistentId = 1;
        records[i].persistentId = 100 + i;
        message(records[i].nameUtf8, sizeof(records[i].nameUtf8),
            i == 0 ? "TEST ONLY: simulated DeckLink I/O" : "TEST ONLY: input device");
    }
    std::memcpy(out, records, sizeof(records));
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL modes(void *context, uint32_t index, uint32_t direction,
    VtrPonDeckLinkMode *out, uint32_t capacity, uint32_t *count,
    char *error, uint32_t errorCapacity) noexcept {
    if (count) *count = 0;
    const auto status = check(context, error, errorCapacity);
    if (status != VtrPonDeckLinkOk) return status;
    if (!count || (!out && capacity) || !static_cast<Context *>(context)->snapshot ||
        index > 1 || (direction != 1 && direction != 2) || (index == 1 && direction == 2))
        return VtrPonDeckLinkInvalidArgument;
    *count = 2;
    if (capacity < *count) return VtrPonDeckLinkBufferTooSmall;
    VtrPonDeckLinkMode records[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        records[i].structSize = sizeof(records[i]); records[i].modeId = 1000 + i;
        records[i].width = 1920; records[i].height = 1080;
        records[i].frameDuration = 1001; records[i].timeScale = i ? 60000 : 30000;
        message(records[i].nameUtf8, sizeof(records[i].nameUtf8), i ? "1080p59.94 (test)" : "1080p29.97 (test)");
    }
    std::memcpy(out, records, sizeof(records));
    return VtrPonDeckLinkOk;
}
#include "mock_media.inc"
const VtrPonDeckLinkApi api{VTRPON_DECKLINK_ABI, sizeof(VtrPonDeckLinkApi),
    VtrPonDeckLinkTestOnly, "vtr-pon-decklink-test-addon",
    create, destroy, devices, modes};
}
const VtrPonDeckLinkMediaApi *VTRPON_DECKLINK_CALL vtrpon_decklink_get_media_api(uint32_t version) {
    return version == 1 ? &mediaApi : nullptr;
}
const VtrPonDeckLinkOutputApi *VTRPON_DECKLINK_CALL vtrpon_decklink_get_output_api(uint32_t version) { return version == 1 ? &outputApi : nullptr; }
extern "C" __declspec(dllexport) void __cdecl vtrpon_decklink_test_expect(unsigned route, uint32_t color) { if (route < 3) outputs[route].expected = color; }
extern "C" __declspec(dllexport) void __cdecl vtrpon_decklink_test_outputs(uint64_t *out) {
    for (unsigned i = 0; i < 3; ++i) {
        out[i*6] = outputs[i].video; out[i*6+1] = outputs[i].audio; out[i*6+2] = outputs[i].time;
        out[i*6+3] = outputs[i].color; out[i*6+4] = outputs[i].active; out[i*6+5] = outputs[i].group;
        out[18+i] = outputs[i].opens;
    }
}
extern "C" __declspec(dllexport) void __cdecl vtrpon_decklink_test_stats(uint32_t *out) {
    out[0] = inputOpens; out[1] = inputs; out[2] = videoSends; out[3] = audioSends; out[4] = outputPumps;
}
const VtrPonDeckLinkApi *VTRPON_DECKLINK_CALL vtrpon_decklink_get_api(uint32_t version) {
    return version == VTRPON_DECKLINK_ABI ? &api : nullptr;
}
