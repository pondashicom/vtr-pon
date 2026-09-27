#pragma once
#include "edit_pfl_protocol.h"
#include <cmath>

namespace vtrpon::pfl {
// Timestamp-driven reader, used only in the isolated output process. Silence
// fills gaps; an absent or slow producer never repeats an old audio block.
class Renderer {
public:
    void render(SharedAudio &audio, float *output, std::uint32_t frames,
                std::int64_t bufferTimeNs) {
        const auto epoch = audio.generation.load();
        const bool audible = audio.enabled.load() && audio.playing.load();
        if (epoch != generation_) {
            generation_ = epoch; havePacket_ = false; haveNext_ = false; haveTime_ = false;
            for (int c = 0; c < 2; ++c) tail_[c] = last_[c] * gain_;
            tailGain_ = 1.0F; gain_ = 0.0F;
        }
        // Endpoint-buffer/QPC observations have scheduling jitter. Re-anchoring
        // every WASAPI block skips/repeats samples and produces a buzz. Keep
        // a continuous fractional sample cursor and correct clock drift slowly.
        const auto observedTime = bufferTimeNs - kLatencyNs;
        if (haveTime_ && std::abs(static_cast<long double>(observedTime) - timeNs_) > 100000000.0L) {
            // A real callback gap is not oscillator drift. Rejoin current PCM
            // promptly, with the same short ramp used for a seek.
            for (int c = 0; c < 2; ++c) tail_[c] = last_[c] * gain_;
            tailGain_ = 1.0F; gain_ = 0.0F; haveTime_ = false;
        }
        if (!haveTime_) { timeNs_ = observedTime; haveTime_ = true; }
        const double errorSeconds = static_cast<double>(observedTime - timeNs_) / 1.0e9;
        const double stepNs = (1.0e9 / kRate)
            * (1.0 + std::clamp(errorSeconds * 0.05, -0.002, 0.002));
        if (!audible) {
            havePacket_ = false;
            haveNext_ = false;
            // Only the consumer advances read. Bound work even if a producer
            // is continuously filling; it is never allowed to wait for us.
            audio.read.store(audio.write.load(std::memory_order_acquire), std::memory_order_release);
        }
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const auto time = static_cast<std::int64_t>(timeNs_);
            timeNs_ += stepNs;
            bool valid = false;
            float sample[2]{};
            if (audible) {
                for (std::uint32_t count = 0; count <= kSlots; ++count) {
                    if (!havePacket_) {
                        if (haveNext_) { packet_ = nextPacket_; haveNext_ = false; }
                        else if (!audio.pop(packet_)) break;
                        havePacket_ = packet_.generation == generation_
                            && packet_.frames > 0 && packet_.frames <= kFrames;
                        if (!havePacket_) continue;
                    }
                    const double index = static_cast<double>(time - packet_.timeNs) * kRate / 1.0e9;
                    if (index >= packet_.frames) { havePacket_ = false; continue; }
                    if (index >= 0.0) {
                        const auto first = static_cast<std::uint32_t>(index);
                        const auto second = (std::min)(first + 1, packet_.frames - 1);
                        const auto fraction = static_cast<float>(index - first);
                        if (first + 1 == packet_.frames && !haveNext_) {
                            haveNext_ = audio.pop(nextPacket_);
                        }
                        const auto endTime = packet_.timeNs
                            + static_cast<std::int64_t>(packet_.frames) * 1000000000LL / kRate;
                        const bool contiguous = haveNext_ && nextPacket_.generation == generation_
                            && nextPacket_.frames > 0 && nextPacket_.frames <= kFrames
                            && std::abs(nextPacket_.timeNs - endTime) < 500000LL;
                        for (int c = 0; c < 2; ++c) {
                            const float a = packet_.samples[first * 2 + c];
                            const float b = first + 1 == packet_.frames && contiguous
                                ? nextPacket_.samples[c] : packet_.samples[second * 2 + c];
                            const float value = a + (b - a) * fraction;
                            sample[c] = std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
                        }
                        valid = true;
                    }
                    break;
                }
            }
            constexpr float step = 1.0F / 240.0F; // five milliseconds
            if (valid) {
                gain_ = (std::min)(1.0F, gain_ + step);
                for (int c = 0; c < 2; ++c) {
                    last_[c] = sample[c];
                    output[frame * 2 + c] = sample[c] * gain_;
                }
            } else {
                gain_ = (std::max)(0.0F, gain_ - step);
                for (int c = 0; c < 2; ++c) output[frame * 2 + c] = last_[c] * gain_;
            }
            tailGain_ = (std::max)(0.0F, tailGain_ - step);
            for (int c = 0; c < 2; ++c) output[frame * 2 + c] += tail_[c] * tailGain_;
        }
    }
private:
    Packet packet_;
    Packet nextPacket_;
    bool haveNext_ = false;
    bool havePacket_ = false;
    std::uint32_t generation_ = 0;
    float gain_ = 0.0F;
    float last_[2]{};
    bool haveTime_ = false;
    long double timeNs_ = 0;
    float tail_[2]{};
    float tailGain_ = 0;
};
} // namespace vtrpon::pfl
