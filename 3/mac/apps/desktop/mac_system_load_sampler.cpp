#include "mac_system_load_sampler.h"
#include "mac_gpu_activity.h"

#include <cstddef>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/resource.h>
#include <sys/sysctl.h>

namespace vtrpon {
namespace {
template<typename T> bool readSysctl(const char *name, T &value) {
  size_t size = sizeof(value);
  return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && size == sizeof(value);
}

std::uint64_t microseconds(const timeval &value) {
  return std::uint64_t(value.tv_sec) * 1000000 + value.tv_usec;
}
} // namespace

MacSystemLoadSampler::MacSystemLoadSampler() : host_(mach_host_self()) {
  readSysctl("hw.memsize", totalMemory_);
  readSysctl("hw.logicalcpu", processors_);
  sample(); // Prime interval counters; never report lifetime CPU as current load.
}

MacSystemLoadSampler::~MacSystemLoadSampler() {
  if (host_ != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), host_);
}

MacLoadSnapshot MacSystemLoadSampler::sample() {
  MacLoadSnapshot result;
  result.systemGpu = macGpuDeviceUtilization();
  result.processMetal = sampleMetalActivity();
  host_cpu_load_info_data_t cpu{};
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(host_, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count)
      == KERN_SUCCESS && count >= HOST_CPU_LOAD_INFO_COUNT) {
    static_assert(CPU_STATE_MAX == 4);
    std::uint64_t total = 0, idle = 0;
    for (unsigned i = 0; i < CPU_STATE_MAX; ++i) {
      // The OS counters are 32-bit and can wrap during a long-running session.
      const auto delta = std::uint32_t(cpu.cpu_ticks[i] - previousCpu_[i]);
      total += delta;
      if (i == CPU_STATE_IDLE) idle = delta;
      previousCpu_[i] = cpu.cpu_ticks[i];
    }
    if (haveSystemCpu_ && total) result.systemCpu = 100.0 * (total - idle) / total;
    haveSystemCpu_ = true;
  } else {
    haveSystemCpu_ = false;
  }

  rusage usage{};
  const auto now = std::chrono::steady_clock::now();
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    // RUSAGE_SELF includes all threads, including threads that have exited.
    const auto current = microseconds(usage.ru_utime) + microseconds(usage.ru_stime);
    const double seconds = std::chrono::duration<double>(now - previousTime_).count();
    if (haveProcessCpu_ && processors_ > 0 && seconds > 0 && current >= previousProcessUs_)
      result.processCpu = 100.0 * (current - previousProcessUs_) / 1.0e6 / seconds / processors_;
    previousProcessUs_ = current;
    previousTime_ = now;
    haveProcessCpu_ = true;
  } else {
    haveProcessCpu_ = false;
  }

  if (totalMemory_ > 0) {
    vm_statistics64_data_t memory{};
    count = HOST_VM_INFO64_COUNT;
    // Some kernels return the fields without the latest SDK's trailing padding.
    // Validate the final field we use, not sizeof(the padded SDK structure).
    vm_size_t pageSize = 0;
    if (host_page_size(host_, &pageSize) == KERN_SUCCESS && pageSize > 0 &&
        host_statistics64(host_, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&memory), &count)
            == KERN_SUCCESS && count * sizeof(integer_t) >=
                offsetof(vm_statistics64_data_t, internal_page_count) + sizeof(memory.internal_page_count)) {
      // Anonymous + wired + actual compressor storage, excluding purgeable pages.
      // File-backed cache is excluded. This is an estimate, not memory pressure.
      const std::uint64_t pages = std::uint64_t(memory.internal_page_count) + memory.wire_count
          + memory.compressor_page_count;
      if (pages >= memory.purgeable_count)
        result.systemRam = 100.0 * (pages - memory.purgeable_count) * pageSize / totalMemory_;
    }
    task_vm_info_data_t task{};
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&task), &count)
        == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT)
      result.processRam = 100.0 * task.phys_footprint / totalMemory_;
  }
  return result;
}

} // namespace vtrpon
