#include <cstddef>
#include <Processing.NDI.Lib.h>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {
unsigned initialized = 0, destroyed = 0, media = 0;
template <class> struct Stub;
template <class R, class... Args> struct Stub<R (*)(Args...)> {
    static R call(Args...) { ++media; if constexpr (!std::is_void_v<R>) return {}; }
};
bool initialize() { ++initialized; const auto *mode = std::getenv("VTRPON_NDI_TEST_MODE"); return !mode || std::strcmp(mode, "initialization_failed"); }
void destroy() { ++destroyed; }
const char *version() { return "VTR-PON test NDI Runtime v6"; }
NDIlib_v6 api() {
    NDIlib_v6 value{};
    value.initialize = initialize; value.destroy = destroy; value.version = version;
#define STUB(name) value.name = &Stub<decltype(value.name)>::call
    STUB(find_create_v2); STUB(find_destroy); STUB(find_get_current_sources);
    STUB(send_create); STUB(send_destroy); STUB(send_get_source_name);
    STUB(send_send_video_v2); STUB(send_send_audio_v2);
    STUB(recv_create_v3); STUB(recv_destroy); STUB(recv_capture_v2);
    STUB(recv_free_video_v2); STUB(recv_free_audio_v2);
#undef STUB
    return value;
}
}
#ifndef VTRPON_NDI_NO_V6
extern "C" const NDIlib_v6 *NDIlib_v6_load() {
    static auto value = api();
    const auto *mode = std::getenv("VTRPON_NDI_TEST_MODE");
    return mode && !std::strcmp(mode, "invalid_api") ? nullptr : &value;
}
#endif
extern "C" unsigned vtrpon_runtime_fixture_count(unsigned kind) {
    return kind == 0 ? initialized : kind == 1 ? destroyed : media;
}
