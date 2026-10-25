#pragma once
#include "gpu_configuration.h"
#ifdef __OBJC__
#import <Metal/Metal.h>
namespace vtrpon { GpuAdapter describeMetalGpu(id<MTLDevice> device); }
#endif
namespace vtrpon { QVector<GpuAdapter> availableMetalGpuAdapters(); }
