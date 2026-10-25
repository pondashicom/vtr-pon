#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace vtrpon {

struct MacLoadSnapshot {
  double systemCpu = -1;
  double systemGpu = -1;
  double processMetal = -1;
  double processCpu = -1;
  double systemRam = -1;
  double processRam = -1;
};

// Use on one sampling thread. No child-process or dedicated-VRAM counters.
class MacSystemLoadSampler final {
public:
  MacSystemLoadSampler();
  ~MacSystemLoadSampler();
  MacSystemLoadSampler(const MacSystemLoadSampler &) = delete;
  MacSystemLoadSampler &operator=(const MacSystemLoadSampler &) = delete;
  MacLoadSnapshot sample();

private:
  std::uint32_t host_ = 0;
  std::uint64_t totalMemory_ = 0;
  int processors_ = 0;
  std::array<std::uint32_t, 4> previousCpu_{};
  bool haveSystemCpu_ = false;
  bool haveProcessCpu_ = false;
  std::uint64_t previousProcessUs_ = 0;
  std::chrono::steady_clock::time_point previousTime_{};
};

} // namespace vtrpon
