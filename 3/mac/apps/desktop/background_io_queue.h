#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

// UI owns queue/completions. A worker sees only a copied work function and its
// result; callers capture values in work, never widgets or their owner.
class BackgroundIoQueue final : public QObject {
public:
    using Work = std::function<void()>;
    using Completion = std::function<void(const QString &)>;
    explicit BackgroundIoQueue(QObject *parent = nullptr) : QObject(parent) {
        timer_.setInterval(20);
        connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    }
    bool enqueue(Work work, Completion completion = {}) {
        if (closing_) return false;
        pending_.push_back({std::move(work), std::move(completion)});
        timer_.start();
        return true;
    }
    void requestShutdown() { closing_ = true; }
    bool shutdownComplete() const { return pending_.empty() && !active_; }
private:
    struct Task { Work work; Completion completion; };
    struct Result { std::atomic_bool done = false; QString error; };
    void poll() {
        if (active_) {
            if (!active_->done.load(std::memory_order_acquire)) return;
            const auto result = std::exchange(active_, {});
            auto completion = std::move(completion_);
            completion_ = {};
            if (completion) completion(result->error);
        }
        if (pending_.empty()) { timer_.stop(); return; }
        auto task = std::move(pending_.front());
        pending_.pop_front();
        active_ = std::make_shared<Result>();
        completion_ = std::move(task.completion);
        const auto result = active_;
        try {
            std::thread([result, work = std::move(task.work)] {
                try { work(); }
                catch (const std::exception &error) { result->error = QString::fromUtf8(error.what()); }
                catch (...) { result->error = QStringLiteral("Background file operation failed."); }
                result->done.store(true, std::memory_order_release);
            }).detach();
        } catch (...) {
            result->error = QStringLiteral("Could not start the file operation worker.");
            result->done.store(true, std::memory_order_release);
        }
    }
    QTimer timer_;
    std::deque<Task> pending_;
    std::shared_ptr<Result> active_;
    Completion completion_;
    bool closing_ = false;
};
