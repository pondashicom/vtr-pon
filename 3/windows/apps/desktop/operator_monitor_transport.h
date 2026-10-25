#pragma once
#include <windows.h>
#include <cstdint>

namespace vtrpon {
struct OperatorMonitorPresentation {
  std::uint64_t serial;
  unsigned size;
  char json[16384];
};
constexpr unsigned operatorMonitorMaxWidth = 1920;
constexpr unsigned operatorMonitorMaxHeight = 1080;
constexpr unsigned operatorMonitorFramesPerSecond = 24;
// A fixed latest-frame mailbox. Both processes acquire the mutex with timeout
// zero. A stopped/slow receiver can only lose its own frames, never stall Program.
struct OperatorMonitorFrame {
  std::uint64_t serial;
  std::uint64_t publishedAt;
  unsigned width;
  unsigned height;
  unsigned char pixels[operatorMonitorMaxWidth * operatorMonitorMaxHeight * 4];
};
}
