#pragma once

#include <chrono>

namespace vtrpon {

inline constexpr int kApplicationShutdownTimeoutExitCode = 124;

// Terminal application shutdown only: the first call fixes the total deadline.
// The process-lifetime watchdog stays armed through QObject, worker, and static
// destruction; subsequent calls neither restart nor cancel it.
void armApplicationShutdownDeadline(
    std::chrono::milliseconds timeout = std::chrono::seconds(30)) noexcept;

} // namespace vtrpon
