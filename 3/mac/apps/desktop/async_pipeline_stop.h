#pragma once

#include <gst/gst.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <system_error>

// The worker owns its pipeline reference and never captures the UI owner.
// Completion includes cleanup and reference release. A failed stop retains the
// worker reference: callers must not reuse or destroy the unconfirmed backend.
class AsyncPipelineStop final {
public:
    static std::shared_ptr<AsyncPipelineStop> start(
        GstElement *pipeline, std::function<void()> cleanup = {},
        std::function<void()> beforeStop = {})
    {
        return startWithLauncher(pipeline, std::move(cleanup), [](auto worker) {
            std::thread(std::move(worker)).detach();
        }, std::move(beforeStop));
    }

    template <typename Launcher>
    static std::shared_ptr<AsyncPipelineStop> startWithLauncher(
        GstElement *pipeline, std::function<void()> cleanup, Launcher launch,
        std::function<void()> beforeStop = {})
    {
        auto job = std::make_shared<AsyncPipelineStop>();
        job->pipeline_ = pipeline;
        job->cleanup_ = std::move(cleanup);
        if (pipeline) gst_object_ref(pipeline);
        try {
        launch([job, beforeStop = std::move(beforeStop)] {
            if (beforeStop) beforeStop();
            GstElement *pipeline = job->pipeline_;
            GstState state = GST_STATE_NULL;
            const bool stopped = !pipeline
                || (gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE
                    && gst_element_get_state(pipeline, &state, nullptr, 2 * GST_SECOND)
                        != GST_STATE_CHANGE_FAILURE && state == GST_STATE_NULL);
            if (stopped) {
                if (job->cleanup_) job->cleanup_();
                if (pipeline) gst_object_unref(pipeline);
                job->pipeline_ = nullptr;
                job->cleanup_ = {};
            }
            job->succeeded_ = stopped;
            job->done_.store(true, std::memory_order_release);
        });
        } catch (const std::system_error &error) {
            // The job keeps the pipeline reference and cleanup captures. Its
            // owner must treat this exactly like an unconfirmed NULL state.
            g_warning("Could not start pipeline stop worker: %s", error.what());
            job->done_.store(true, std::memory_order_release);
        }
        return job;
    }
    bool done() const { return done_.load(std::memory_order_acquire); }
    bool succeeded() const { return done() && succeeded_; }
private:
    std::atomic_bool done_ = false;
    bool succeeded_ = false;
    GstElement *pipeline_ = nullptr; // Retained on an unconfirmed stop.
    std::function<void()> cleanup_;
};
