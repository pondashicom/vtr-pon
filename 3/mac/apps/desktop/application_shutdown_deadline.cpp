#include "application_shutdown_deadline.h"

#include <atomic>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vtrpon {
namespace {

[[noreturn]] void exitAfterShutdownTimeout() noexcept
{
    // Do not log, invoke Qt, or run destructors here: any of their workers or
    // locks may be the reason shutdown stalled. Never destroy live worker state.
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), kApplicationShutdownTimeoutExitCode);
#endif
    std::_Exit(kApplicationShutdownTimeoutExitCode);
}

} // namespace

void armApplicationShutdownDeadline(std::chrono::milliseconds timeout) noexcept
{
    static std::atomic_bool armed = false;
    if (armed.exchange(true, std::memory_order_relaxed)) return;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    try {
        // No Controller/Qt ownership and no event loop dependency. Intentionally
        // outlives all application objects so their final waits are bounded too.
        std::thread([deadline] {
            std::this_thread::sleep_until(deadline);
            exitAfterShutdownTimeout();
        }).detach();
    } catch (...) {
        // If the guard cannot start, an unbounded terminal wait is still unsafe.
        exitAfterShutdownTimeout();
    }
}

} // namespace vtrpon
