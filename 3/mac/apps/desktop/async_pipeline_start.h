#pragma once

#include <gst/gst.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// Opening a capture driver must not run on the GUI thread. Retirement waits
// for this exact opener before stopping its graph; it never races set_state.
class AsyncPipelineStart final {
public:
    static std::shared_ptr<AsyncPipelineStart> start(GstElement *pipeline) {
        auto job = std::make_shared<AsyncPipelineStart>();
        auto graph = std::shared_ptr<GstElement>(GST_ELEMENT(gst_object_ref(pipeline)),
            [](GstElement *value) { gst_object_unref(value); });
        try {
            std::thread([job, graph] {
                const bool ok = gst_element_set_state(graph.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
                {
                    std::lock_guard lock(job->mutex_);
                    job->ok_ = ok;
                    job->done_.store(true, std::memory_order_release);
                }
                job->wake_.notify_all();
            }).detach();
        } catch (...) {
            job->done_.store(true, std::memory_order_release);
        }
        return job;
    }
    bool done() const { return done_.load(std::memory_order_acquire); }
    bool succeeded() const { return done() && ok_; }
    static std::function<void()> retire(std::shared_ptr<AsyncPipelineStart> &job) {
        auto held = std::exchange(job, {});
        if (!held) return {};
        return [held] {
            std::unique_lock lock(held->mutex_);
            held->wake_.wait(lock, [&] { return held->done(); });
        };
    }
private:
    std::atomic_bool done_{false};
    bool ok_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
};
