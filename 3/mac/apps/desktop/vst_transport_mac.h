#pragma once
// macOS protocol-v2 transport. Only anonymous descriptors are inherited;
// no filesystem socket or persistent shared-memory name survives a crash.
#include "vst_protocol.h"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <initializer_list>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <cstddef>

namespace vtrpon::vst {
class MacTransport final {
public:
    // Deadlines and cross-process stamps must use the same absolute clock.
    // Converting two separately sampled clocks can invent an expired deadline
    // if the caller is descheduled between the samples.
    struct Clock {
        using duration = std::chrono::nanoseconds;
        using rep = duration::rep;
        using period = duration::period;
        using time_point = std::chrono::time_point<Clock>;
        static constexpr bool is_steady = true;
        static time_point now() noexcept {
            timespec time{}; clock_gettime(CLOCK_MONOTONIC_RAW, &time);
            return time_point(duration(int64_t(time.tv_sec) * 1000000000LL + time.tv_nsec));
        }
    };
    // Optional diagnostics follow the unchanged protocol-v2 payload. Older
    // peers ignore them; a magic tag prevents using an old mapping's padding.
    struct Timing {
        uint32_t magic = 0x56535454, version = 1;
        alignas(8) int64_t requestNs = 0, startedNs = 0, finishedNs = 0;
    };
    static_assert(__atomic_always_lock_free(sizeof(int64_t), nullptr));
    struct Arena { Shared audio; Timing timing; };
    static_assert(offsetof(Arena, timing) >= sizeof(Shared));
    struct Snapshot { int64_t requestNs=0, startedNs=0, finishedNs=0; };
    Shared *shared = nullptr;
    int memory = -1, socket = -1, childSocket = -1;
    MacTransport() = default;
    MacTransport(const MacTransport &) = delete;
    MacTransport &operator=(const MacTransport &) = delete;
    ~MacTransport() {
        if (shared) munmap(shared, mappedBytes_);
        for (int fd : {memory, socket, childSocket}) if (fd >= 0) close(fd);
    }
    bool create(const char *name) {
        memory = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (memory < 0) return false;
        if (shm_unlink(name) != 0 || fcntl(memory, F_SETFD, FD_CLOEXEC) != 0 ||
            ftruncate(memory, sizeof(Arena)) != 0 || !map(sizeof(Arena))) return false;
        new (shared) Shared;
        timing_ = new (reinterpret_cast<unsigned char *>(shared) + offsetof(Arena, timing)) Timing;
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) return false;
        socket = pair[0]; childSocket = pair[1];
        for (int fd : pair) {
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fcntl(fd, F_SETFL, O_NONBLOCK) != 0) return false;
        }
        return true;
    }
    bool inherit() {
        // QProcess reserves stdin for the mapping and stdout for signaling.
        // Move the socket before loading vendor code, which may write stdout.
        memory = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
        socket = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (memory < 0 || socket < 0 || null < 0) { if (null >= 0) close(null); return false; }
        const bool redirected = dup2(null, STDIN_FILENO) >= 0 && dup2(null, STDOUT_FILENO) >= 0;
        close(null);
        // Darwin rounds a POSIX shared-memory object up to a VM page.
        struct stat info{};
        return redirected && fstat(memory, &info) == 0 && info.st_size >= sizeof(Shared) &&
            map(info.st_size >= sizeof(Arena) ? sizeof(Arena) : sizeof(Shared)) && shared->protocol == kProtocol && discoverTiming();
    }
    static int64_t nowNs() { return Clock::now().time_since_epoch().count(); }
    void markRequest() const {
        if (!timing_) return;
        __atomic_store_n(&timing_->startedNs, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&timing_->finishedNs, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&timing_->requestNs, nowNs(), __ATOMIC_SEQ_CST);
    }
    void markStarted() const { if (timing_) __atomic_store_n(&timing_->startedNs, nowNs(), __ATOMIC_SEQ_CST); }
    void markResponse() const { if (timing_) __atomic_store_n(&timing_->finishedNs, nowNs(), __ATOMIC_SEQ_CST); }
    Snapshot timing() const {
        if (!timing_) return {};
        return {__atomic_load_n(&timing_->requestNs, __ATOMIC_SEQ_CST),
                __atomic_load_n(&timing_->startedNs, __ATOMIC_SEQ_CST),
                __atomic_load_n(&timing_->finishedNs, __ATOMIC_SEQ_CST)};
    }
    bool signal() const {
        const unsigned char byte = 1;
        std::atomic_thread_fence(std::memory_order_release);
        ssize_t n;
        do { n = send(socket, &byte, 1, MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
        return n == 1;
    }
    enum class Result { Ready, ReadyAfterCallerDelay, Timeout, Failed };
    Result waitForResponse(Clock::time_point deadline) const {
        // A descheduled caller can enter/leave poll after its deadline even
        // though DSP completed on time. Do not retire a healthy plug-in for
        // that delay. Require a currently queued reply and proof that this
        // request's worker completed within the original buffer deadline.
        const auto rawDeadline = deadline.time_since_epoch().count();
        const auto result = wait(deadline);
        if (result != Result::Ready && result != Result::Timeout) return result;
        const auto stamp = timing();
        const bool measured = stamp.requestNs && stamp.startedNs >= stamp.requestNs && stamp.finishedNs >= stamp.startedNs;
        // poll can itself resume late with POLLIN; a measured late completion
        // must still fail, even when poll reports a readable socket.
        if (result == Result::Ready)
            return measured && stamp.finishedNs > rawDeadline ? Result::Timeout : result;
        if (!measured || stamp.finishedNs > rawDeadline) return result;
        unsigned char byte = 0;
        const auto received = recv(socket, &byte, 1, MSG_DONTWAIT);
        if (received < 0 && (errno == EAGAIN || errno == EINTR)) return result;
        if (received != 1 || byte != 1) return Result::Failed;
        std::atomic_thread_fence(std::memory_order_acquire);
        return Result::ReadyAfterCallerDelay;
    }
    Result wait(Clock::time_point deadline) const {
        for (;;) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return Result::Timeout;
            pollfd fd{socket, POLLIN, 0};
            const int result = poll(&fd, 1, int(remaining > INT_MAX ? INT_MAX : remaining));
            if (result < 0 && errno == EINTR) continue;
            if (result == 0) return Result::Timeout;
            if (result < 0 || !(fd.revents & POLLIN) || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) return Result::Failed;
            unsigned char byte = 0;
            const auto n = recv(socket, &byte, 1, MSG_DONTWAIT);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n != 1 || byte != 1) return Result::Failed;
            std::atomic_thread_fence(std::memory_order_acquire);
            return Result::Ready;
        }
    }
private:
    Timing *timing_ = nullptr;
    size_t mappedBytes_ = sizeof(Shared);
    bool discoverTiming() {
        if (mappedBytes_ >= sizeof(Arena)) {
            auto *candidate = reinterpret_cast<Timing *>(reinterpret_cast<unsigned char *>(shared) + offsetof(Arena, timing));
            if (candidate->magic == 0x56535454 && candidate->version == 1) timing_ = candidate;
        }
        return true;
    }
    bool map(size_t bytes) {
        void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
        if (p == MAP_FAILED) return false;
        shared = static_cast<Shared *>(p); mappedBytes_ = bytes;
        return true;
    }
};
} // namespace vtrpon::vst
