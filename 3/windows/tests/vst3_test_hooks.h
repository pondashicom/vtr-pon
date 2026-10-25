#pragma once
#include "vst_deadline.h"
// Compiled only into the test bridge, never the normal application.
namespace vtrpon::vst::test {
inline void (*acquired)(const void *) = nullptr;
inline void (*requested)(Shared *, Timing *) = nullptr;
inline void (*destroyed)(const void *) = nullptr;
inline const char *addonOverride = nullptr;
}
