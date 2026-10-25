#pragma once
#include "operator_monitor_transport.h"
#include <atomic>
#include <mach/mach_time.h>
#include <pthread.h>

namespace vtrpon {
constexpr std::uint32_t operatorMonitorMacMagic = 0x5654524d;
constexpr std::uint32_t operatorMonitorMacVersion = 1;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
// Video and metadata have independent, process-shared try-locks. A dead reader
// may leave a lock owned; the parent drops frames and creates a fresh mapping
// on the next start. It never waits on or repairs a receiver-owned mutex.
struct MacOperatorMonitorShared {
  std::uint32_t magic = operatorMonitorMacMagic;
  std::uint32_t version = operatorMonitorMacVersion;
  pthread_mutex_t videoMutex{}, infoMutex{};
  OperatorMonitorFrame frame{};
  OperatorMonitorPresentation presentation{};
  std::uint64_t sourceSequence = 0;
  std::atomic<std::uint64_t> receiverFrameSerial{0}, receiverInfoSerial{0};
};
inline std::uint64_t operatorMonitorNowMs() {
  static const auto scale = [] { mach_timebase_info_data_t v{}; mach_timebase_info(&v); return v; }();
  const auto ticks = mach_absolute_time();
  return ((ticks / scale.denom) * scale.numer +
          (ticks % scale.denom) * scale.numer / scale.denom) / 1000000;
}
}
