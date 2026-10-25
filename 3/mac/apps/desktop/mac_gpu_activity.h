#pragma once
#include <cstdint>
#include <vector>
namespace vtrpon {
struct MetalInterval { double begin, end; };
// The union avoids double-counting overlapping work on independent queues.
double metalBusyPercent(std::vector<MetalInterval> intervals, double begin, double end);
void recordMetalActivity(double begin, double end);
double sampleMetalActivity();
double macGpuDeviceUtilization();
}
