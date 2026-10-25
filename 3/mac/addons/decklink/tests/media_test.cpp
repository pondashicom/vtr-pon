#include "media_contract.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#define CHECK(expression) do { if (!(expression)) { std::cerr << "Failed line " << __LINE__ << ": " #expression << '\n'; std::exit(1); } } while (false)
using namespace vtrpon::decklink;
VideoFrame frame(std::int64_t pts) {
    return {2, 2, 8, PixelFormat::Bgra8, pts, std::vector<std::uint8_t>(16, 42)};
}
extern "C" int c_header_test(void);
int main() {
    CHECK(c_header_test());
    auto f = frame(0); CHECK(valid(f));
    f.bytes.pop_back(); CHECK(!valid(f));
    f = frame(0); f.stride = 7; CHECK(!valid(f));
    f = frame(0); f.width = 4097; CHECK(!valid(f));
    f = frame(-1); CHECK(!valid(f));
    f = {2, 2, 4, PixelFormat::Uyvy8, 0, std::vector<std::uint8_t>(8)}; CHECK(valid(f));
    f.width = 1; CHECK(!valid(f)); // Packed 4:2:2 cannot represent odd width.
    f = frame(0); f.format = static_cast<PixelFormat>(99); CHECK(!valid(f));
    CHECK(frameTime100ns(30000, 1001, 30000) == 10010000000LL); // 1001 s, zero accumulated drift.
    CHECK(frameTime100ns(60000, 1001, 60000) == 10010000000LL);
    CHECK(frameTime100ns(1, 1001, 30000) == 333666);
    // Both output streams use the same 3-frame delay; rounding stays below one
    // audio sample after six hours at 29.97 and 59.94 fps.
    for (const auto scale : {30000LL, 60000LL}) {
        const auto index = scale * 21600 / 1001;
        const auto video = frameTime100ns(index + 3, 1001, scale);
        const auto source = frameTime100ns(index, 1001, scale);
        const auto delay = frameTime100ns(3, 1001, scale);
        CHECK(video && source && delay);
        CHECK(std::abs(*audioTime48k(*video) - (*audioTime48k(*source) + *audioTime48k(*delay))) <= 1);
    }
    CHECK(audioTime48k(10000000) == 48000 && audioTime48k(-10000000) == -48000);
    CHECK(!frameTime100ns(-1, 1001, 30000)); CHECK(!frameTime100ns(1, 1, 0));
    CHECK(!frameTime100ns(std::numeric_limits<std::int64_t>::max(), 1001, 30000));
    AudioFrame audio{48000, 2, 480, 0, std::vector<float>(960, 0.5f)}; CHECK(valid(audio));
    audio.sampleRate = 44100; CHECK(!valid(audio)); audio.sampleRate = 48000;
    audio.interleaved[0] = std::numeric_limits<float>::quiet_NaN(); CHECK(!valid(audio));
    audio.interleaved[0] = 0.5f; audio.channels = 3; CHECK(!valid(audio));
    bool threw = false; try { VideoQueue invalid(0, FullPolicy::DropOldest); } catch (const std::invalid_argument &) { threw = true; } CHECK(threw);
    VideoQueue input(2, FullPolicy::DropOldest);
    CHECK(input.push(frame(0)) == PushResult::Accepted);
    CHECK(input.push(frame(10)) == PushResult::Accepted);
    CHECK(input.push(frame(20)) == PushResult::DroppedOldest);
    CHECK(input.size() == 2 && input.dropped() == 1);
    auto received = input.pop(); CHECK(received && received->pts100ns == 10 && received->bytes[0] == 42);
    CHECK(input.push(frame(20)) == PushResult::Invalid); // Reject repeated/backwards PTS.
    input.stop(); CHECK(!input.pop() && input.size() == 0);
    CHECK(input.push(frame(30)) == PushResult::Stopped);
    VideoQueue output(1, FullPolicy::RejectNewest);
    CHECK(output.push(frame(0)) == PushResult::Accepted);
    CHECK(output.push(frame(10)) == PushResult::Full); CHECK(output.dropped() == 0);
    CHECK(output.pop()->pts100ns == 0);
    CHECK(output.push(frame(10)) == PushResult::Accepted); // Retry after backpressure.
    VideoQueue sinkA(1, FullPolicy::RejectNewest), sinkB(1, FullPolicy::DropOldest);
    auto sharedSource = frame(0);
    CHECK(sinkA.push(sharedSource) == PushResult::Accepted);
    CHECK(sinkB.push(sharedSource) == PushResult::Accepted);
    sharedSource.bytes[0] = 99; // Each output owns a copy, with separate lifetime.
    sinkA.stop();
    CHECK(sinkA.push(frame(10)) == PushResult::Stopped);
    auto independent = sinkB.pop();
    CHECK(independent && independent->bytes[0] == 42);
    CHECK(sinkB.push(frame(10)) == PushResult::Accepted);
    std::cout << "Media contract passed.\n";
}
