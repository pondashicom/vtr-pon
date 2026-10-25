#pragma once

#include <QDeadlineTimer>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>
#include <gst/gst.h>
#include <atomic>
#include <chrono>
#include <utility>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Filesystem calls, state changes and flushing seeks can block inside the OS.
// This job owns only a pipeline reference and copied request values, never a UI
// owner. A timeout rejects the result; it does not pretend to stop an OS call.
class AsyncFilePreparation final {
public:
    static constexpr int timeoutMs = 10000;
    enum class State { Loading, Ready, Offline, Error, Cancelled };
    struct Result {
        State state = State::Error;
        QString error;
        bool hasVideo = false;
        bool hasAudio = false;
        bool stillImage = false;
    };
    struct Request {
        QString path;
        qint64 inMs = 0;
        qint64 outMs = -1;
        double rate = 1.0;
        bool imageFreeze = false;
    };
    using Work = std::function<Result(GstElement *, const AsyncFilePreparation &)>;

    static std::shared_ptr<AsyncFilePreparation> start(GstElement *pipeline, Request request, int deadlineMs = timeoutMs) {
        return startWithWork(pipeline, [request = std::move(request)](GstElement *p, const AsyncFilePreparation &job) {
            return prepare(p, request, job);
        }, deadlineMs);
    }
    static std::shared_ptr<AsyncFilePreparation> startWithWork(
        GstElement *pipeline, Work work, int deadlineMs = timeoutMs) {
        auto job = std::shared_ptr<AsyncFilePreparation>(new AsyncFilePreparation(deadlineMs));
        if (pipeline) gst_object_ref(pipeline);
        try {
            std::thread([job, pipeline, work = std::move(work)] {
                try { job->result_ = work(pipeline, *job); }
                catch (...) { job->result_.error = QStringLiteral("Media preparation failed."); }
                if (pipeline) gst_object_unref(pipeline);
                job->finishedLate_ = job->deadline_.hasExpired();
                {
                    std::lock_guard lock(job->mutex_);
                    job->done_.store(true, std::memory_order_release);
                }
                job->finished_.notify_all();
            }).detach();
        } catch (...) {
            if (pipeline) gst_object_unref(pipeline);
            job->result_.error = QStringLiteral("Could not start the media preparation worker.");
            job->done_.store(true, std::memory_order_release);
        }
        return job;
    }
    State state() const {
        if (cancelled_.load()) return State::Cancelled;
        if (done()) return finishedLate_ ? State::Offline : result_.state;
        return deadline_.hasExpired() ? State::Offline : State::Loading;
    }
    bool done() const { return done_.load(std::memory_order_acquire); }
    bool cancelled() const { return cancelled_.load() || deadline_.hasExpired(); }
    void cancel() { cancelled_.store(true); }
    const Result &result() const { return result_; } // Read only after done().
    static QString offlineError() { return QStringLiteral("Media offline: the file is missing or did not respond in time."); }
    static bool isOfflineError(const GError *error) {
        return error && error->domain == GST_RESOURCE_ERROR
            && (error->code == GST_RESOURCE_ERROR_NOT_FOUND
                || error->code == GST_RESOURCE_ERROR_OPEN_READ
                || error->code == GST_RESOURCE_ERROR_READ);
    }
    // Only a background retirement worker may invoke the returned barrier.
    static std::function<void()> retire(std::shared_ptr<AsyncFilePreparation> job) {
        if (!job) return {};
        job->cancel();
        return [job = std::move(job)] {
            std::unique_lock lock(job->mutex_);
            job->finished_.wait(lock, [&] { return job->done(); });
        };
    }
private:
    explicit AsyncFilePreparation(int deadlineMs) : deadline_(deadlineMs) {}
    static Result failure(GstElement *pipeline, QString fallback) {
        Result result;
        result.error = std::move(fallback);
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = bus ? gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR) : nullptr;
        if (message) {
            GError *error = nullptr;
            gst_message_parse_error(message, &error, nullptr);
            if (error) {
                result.error = QString::fromUtf8(error->message);
                if (isOfflineError(error)) result.state = State::Offline;
                g_error_free(error);
            }
            gst_message_unref(message);
        }
        if (bus) gst_object_unref(bus);
        return result;
    }
    static Result prepare(GstElement *pipeline, Request request, const AsyncFilePreparation &job) {
        Result result;
        const QFileInfo file(request.path);
        if (!file.exists() || !file.isFile()) {
            result.state = State::Offline;
            return result;
        }
        if (job.cancelled()) return result;
        if (request.imageFreeze) {
            result.stillImage = QMimeDatabase().mimeTypeForFile(request.path,
                QMimeDatabase::MatchContent).name().startsWith(QStringLiteral("image/"));
            if (job.cancelled()) return result;
            GstElement *filter = result.stillImage ? gst_element_factory_make("imagefreeze", nullptr) : nullptr;
            if (result.stillImage && !filter) {
                result.error = QStringLiteral("Could not create the still image filter.");
                return result;
            }
            if (filter) gst_object_ref_sink(filter);
            g_object_set(pipeline, "video-filter", filter, nullptr);
            if (filter) gst_object_unref(filter);
            if (result.stillImage) { request.inMs = 0; request.outMs = -1; }
        }
        const auto uri = QUrl::fromLocalFile(request.path).toEncoded();
        g_object_set(pipeline, "uri", uri.constData(), nullptr);
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
            return failure(pipeline, QStringLiteral("Could not prepare the media pipeline."));
        }
        bool seekIssued = request.inMs == 0 && request.outMs < 0 && request.rate == 1.0;
        while (!job.cancelled()) {
            GstState state = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
            const auto change = gst_element_get_state(pipeline, &state, &pending, 50 * GST_MSECOND);
            if (change == GST_STATE_CHANGE_FAILURE) {
                return failure(pipeline, QStringLiteral("Could not prepare the media pipeline."));
            }
            if (change != GST_STATE_CHANGE_SUCCESS || state != GST_STATE_PAUSED || pending != GST_STATE_VOID_PENDING)
                continue;
            gint video = 0, audio = 0;
            g_object_get(pipeline, "n-video", &video, "n-audio", &audio, nullptr);
            result.hasVideo = video > 0;
            result.hasAudio = audio > 0;
            if (!result.hasVideo && !result.hasAudio) {
                result.error = QStringLiteral("No playable video or audio streams were found.");
                return result;
            }
            if (!seekIssued) {
                gint64 duration = GST_CLOCK_TIME_NONE;
                if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration) && duration > 0
                    && (request.inMs >= duration / GST_MSECOND
                        || (request.outMs >= 0 && request.outMs > duration / GST_MSECOND))) {
                    result.error = QStringLiteral("The In or Out point exceeds the media duration.");
                    return result;
                }
                if (!gst_element_seek(pipeline, request.rate, GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                    GST_SEEK_TYPE_SET, request.inMs * GST_MSECOND,
                    request.outMs >= 0 ? GST_SEEK_TYPE_SET : GST_SEEK_TYPE_NONE,
                    request.outMs >= 0 ? request.outMs * GST_MSECOND : GST_CLOCK_TIME_NONE)) {
                    result.error = QStringLiteral("Could not prepare the In/Out points.");
                    return result;
                }
                seekIssued = true;
                continue;
            }
            if (request.inMs != 0 || request.outMs >= 0 || request.rate != 1.0) {
                gint64 position = GST_CLOCK_TIME_NONE;
                if (!gst_element_query_position(pipeline, GST_FORMAT_TIME, &position)
                    || qAbs(position / GST_MSECOND - request.inMs) > 250) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
            }
            result.state = State::Ready;
            return result;
        }
        return result;
    }
    QDeadlineTimer deadline_;
    std::atomic_bool done_{false}, cancelled_{false};
    Result result_;
    bool finishedLate_ = false;
    std::mutex mutex_;
    std::condition_variable finished_;
};
