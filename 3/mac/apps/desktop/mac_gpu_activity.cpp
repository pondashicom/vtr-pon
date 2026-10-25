#include "mac_gpu_activity.h"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace vtrpon {
namespace {
std::mutex intervalMutex;
std::vector<MetalInterval> recent;
bool haveTimestamps = false;
double previousSample = 0;
double nowSeconds() {
  static const auto scale = [] { mach_timebase_info_data_t t{}; mach_timebase_info(&t); return t; }();
  return double(mach_absolute_time()) * scale.numer / scale.denom / 1.0e9;
}
}
double metalBusyPercent(std::vector<MetalInterval> values, double begin, double end) {
  if (!std::isfinite(begin) || !std::isfinite(end) || end <= begin) return -1;
  std::sort(values.begin(), values.end(), [](auto a, auto b) { return a.begin < b.begin; });
  double busy = 0, cursor = begin;
  for (const auto &v : values) {
    if (!std::isfinite(v.begin) || !std::isfinite(v.end) || v.end <= v.begin) continue;
    const auto first = std::max({begin, cursor, v.begin});
    const auto last = std::min(end, v.end);
    if (last > first) { busy += last - first; cursor = last; }
  }
  return std::clamp(100.0 * busy / (end - begin), 0.0, 100.0);
}
void recordMetalActivity(double begin, double end) {
  if (!std::isfinite(begin) || !std::isfinite(end) || begin <= 0 || end <= begin) return;
  // Instrumentation must never stall the render/presentation workers.
  std::unique_lock lock(intervalMutex, std::try_to_lock);
  if (!lock) return;
  haveTimestamps = true;
  if (recent.size() >= 4096) recent.erase(recent.begin(), recent.begin() + 2048);
  recent.push_back({begin, end});
}
double sampleMetalActivity() {
  const auto now = nowSeconds();
  std::vector<MetalInterval> values;
  double begin;
  {
    std::lock_guard lock(intervalMutex);
    begin = previousSample; previousSample = now;
    if (!begin || !haveTimestamps) return -1;
    values = recent;
    std::erase_if(recent, [begin](auto v) { return v.end < begin; });
  }
  return metalBusyPercent(std::move(values), begin, now);
}
double macGpuDeviceUtilization() {
  io_iterator_t entries = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &entries) != KERN_SUCCESS) return -1;
  double busiest = -1;
  while (const auto entry = IOIteratorNext(entries)) {
    auto property = IORegistryEntryCreateCFProperty(entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
    if (property && CFGetTypeID(property) == CFDictionaryGetTypeID()) {
      const auto number = CFDictionaryGetValue(static_cast<CFDictionaryRef>(property), CFSTR("Device Utilization %"));
      double value = -1;
      if (number && CFGetTypeID(number) == CFNumberGetTypeID() &&
          CFNumberGetValue(static_cast<CFNumberRef>(number), kCFNumberDoubleType, &value) &&
          std::isfinite(value) && value >= 0 && value <= 100) busiest = std::max(busiest, value);
    }
    if (property) CFRelease(property);
    IOObjectRelease(entry);
  }
  IOObjectRelease(entries);
  return busiest;
}
}
