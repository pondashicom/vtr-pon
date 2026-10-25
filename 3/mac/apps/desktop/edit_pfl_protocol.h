#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

// Private, one-way EDIT PCM transport. No Program objects, channels, clocks,
// handles or commands are part of this protocol. Lock-free atomics in shared
// memory keep a stalled/dead consumer off the producer's path (Windows x64 and
// macOS arm64; other targets must satisfy the assertions below).
namespace vtrpon::pfl {
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kFrames = 480;
constexpr std::uint32_t kSlots = 64;
constexpr std::int64_t kLatencyNs = 80000000;
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);

enum class State : std::uint32_t { Closed, Opening, Ready, Failed };
struct Packet {
    std::uint32_t generation = 0;
    std::uint32_t frames = 0;
    std::int64_t timeNs = 0;
    float samples[kFrames * kChannels]{};
};
struct SharedAudio {
    std::uint32_t version = kVersion;
    alignas(64) std::atomic<std::uint32_t> write{0};
    alignas(64) std::atomic<std::uint32_t> read{0};
    alignas(64) std::atomic<std::uint32_t> generation{1};
    std::atomic<std::uint32_t> enabled{0};
    std::atomic<std::uint32_t> playing{0};
    std::atomic<std::uint32_t> quit{0};
    std::atomic<std::uint32_t> state{0};
    std::atomic<std::uint32_t> error{0};
    std::atomic<std::uint32_t> dropped{0};
    std::atomic<std::int64_t> heartbeatNs{0};
    Packet packets[kSlots];

    // Called only by the EDIT audio streaming thread. Full means drop, never
    // wait, allocate, move the reader's cursor, or touch an output device.
    bool push(const float *samples, std::uint32_t frames,
              std::int64_t timeNs, std::uint32_t epoch) {
        if (frames == 0 || frames > kFrames) return false;
        const auto next = write.load(std::memory_order_relaxed);
        if (next - read.load(std::memory_order_acquire) >= kSlots) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto &packet = packets[next % kSlots];
        packet.generation = epoch;
        packet.frames = frames;
        packet.timeNs = timeNs;
        std::memcpy(packet.samples, samples, frames * kChannels * sizeof(float));
        write.store(next + 1, std::memory_order_release);
        return true;
    }
    bool pop(Packet &packet) {
        const auto next = read.load(std::memory_order_relaxed);
        if (next == write.load(std::memory_order_acquire)) return false;
        packet = packets[next % kSlots];
        read.store(next + 1, std::memory_order_release);
        return true;
    }
};
} // namespace vtrpon::pfl
