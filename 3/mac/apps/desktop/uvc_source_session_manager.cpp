#include "decklink.h"
#include "uvc_source_session_manager.h"
#include "uvc_input_guard.h"
#include "uvc_thumbnail_mailbox.h"
#include "network_video.h"
#ifdef __APPLE__
#include "mac_capture_input.h"
#endif

#include <QDebug>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kThumbnailWidth = 120;
constexpr int kThumbnailHeight = 68;
constexpr int kThumbnailFps = 10;
constexpr int kEditPreviewWidth = 640;
constexpr int kEditPreviewHeight = 360;
constexpr int kEditPreviewFps = 15;
constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;
constexpr qint64 kFrameTimeoutMs = 5000;
constexpr qint64 kRetryIntervalMs = 3000;
constexpr qint64 kShutdownWorkerGraceMs = 3000;

qint64 monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

QImage imageFromSample(GstSample *sample)
{
    if (!sample) {
        return {};
    }
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info = {};
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)
        || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA) {
        return {};
    }

    GstVideoFrame frame = {};
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        return {};
    }
    const int width = static_cast<int>(GST_VIDEO_FRAME_WIDTH(&frame));
    const int height = static_cast<int>(GST_VIDEO_FRAME_HEIGHT(&frame));
    const auto *source = static_cast<const uchar *>(
        GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int sourceStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    QImage image(width, height, QImage::Format_RGBA8888);
    if (!source || sourceStride <= 0 || image.isNull()) {
        gst_video_frame_unmap(&frame);
        return {};
    }
    const qsizetype rowBytes = static_cast<qsizetype>(width) * 4;
    for (int row = 0; row < height; ++row) {
        std::memcpy(image.scanLine(row), source + row * sourceStride,
                    static_cast<std::size_t>(rowBytes));
    }
    gst_video_frame_unmap(&frame);
    return image;
}

bool stopPipeline(GstElement *pipeline)
{
    if (!pipeline) {
        return true;
    }
    if (gst_element_set_state(pipeline, GST_STATE_NULL)
        == GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn result = gst_element_get_state(
        pipeline, &current, &pending, kShutdownStateTimeout);
    return result != GST_STATE_CHANGE_FAILURE
        && current == GST_STATE_NULL
        && pending == GST_STATE_VOID_PENDING;
}

} // namespace

class UvcSourceSessionManager::Impl final
{
public:
    struct Session {
        QPointer<UvcSourceSessionManager> owner;
        QString devicePath;
        GstElement *pipeline = nullptr;
        GstElement *source = nullptr;
        std::shared_ptr<vtrpon::NetworkVideoReceiver> networkReceiver;
        std::shared_ptr<vtrpon::DeckLinkReceiver> deckLinkReceiver;
        std::array<GstElement *, 2> programValves = {};
        std::array<GstElement *, 2> programSinks = {};
        GstElement *thumbnailSink = nullptr;
        GstElement *editValve = nullptr;
        GstElement *editSink = nullptr;
        GstBus *bus = nullptr;
        std::shared_ptr<UvcThumbnailMailbox> thumbnails =
            std::make_shared<UvcThumbnailMailbox>();
        std::shared_ptr<UvcThumbnailMailbox> editFrames =
            std::make_shared<UvcThumbnailMailbox>();
        mutable std::mutex callbackMutex;
        std::condition_variable callbackCondition;
        unsigned int activeCallbacks = 0;
        bool acceptingCallbacks = false;
        quint64 errorGeneration = 0;
        QString errorDetail;
        std::atomic<qint64> lastFrameMs{0};
        qint64 startedMs = monotonicMs();
    };

    struct Startup {
        std::shared_ptr<Session> session;
        std::mutex completionMutex;
        std::atomic_bool done{false};
        bool succeeded = false;
        bool invalidated = false; // GUI thread only.
        bool abandoned = false; // Protected by completionMutex.
    };

    class CallbackScope final
    {
    public:
        explicit CallbackScope(Session &session) : session_(session)
        {
            std::scoped_lock lock(session_.callbackMutex);
            if (session_.acceptingCallbacks) {
                ++session_.activeCallbacks;
                active_ = true;
            }
        }

        ~CallbackScope()
        {
            if (!active_) {
                return;
            }
            std::scoped_lock lock(session_.callbackMutex);
            --session_.activeCallbacks;
            session_.callbackCondition.notify_all();
        }

        explicit operator bool() const { return active_; }

    private:
        Session &session_;
        bool active_ = false;
    };

    Impl(UvcSourceSessionManager *owner, bool use4kProgramOutput)
        : owner_(owner)
        , outputWidth_(use4kProgramOutput ? 3840 : 1920)
        , outputHeight_(use4kProgramOutput ? 2160 : 1080)
    {
        QObject::connect(&busTimer_, &QTimer::timeout, owner_, [this] {
            pollBuses();
        });
        busTimer_.setInterval(100);
        busTimer_.start();
    }

    ~Impl()
    {
        shutdown();
        abandonStartups();
        gst_clear_context(&programContext_);
    }

    void setProgramContext(GstContext *context)
    {
        if (shutDown_ || programContext_ == context) {
            return;
        }
        gst_clear_context(&programContext_);
        programContext_ = context ? gst_context_ref(context) : nullptr;
    }

    static QString deckLinkPhysicalKey(const QString &path)
    {
        VtrPonDeckLinkStreamOptions options{};
        if (!vtrpon::deckLinkSourceOptions(path, &options)) return {};
        return QStringLiteral("%1/%2").arg(options.persistentId).arg(options.subDeviceIndex);
    }

    bool deckLinkSuppressed(const QString &path) const
    {
        const auto key = deckLinkPhysicalKey(path);
        const auto selected = preferredDeckLinkPaths_.value(key);
        return !key.isEmpty() && !selected.isEmpty() && selected != path;
    }

    QString conflictingDeckLinkInput(const QString &path) const
    {
        const auto key = deckLinkPhysicalKey(path);
        if (key.isEmpty()) return {};
        QSet<QString> reserved = registeredDevicePaths_;
        for (const auto &active : deckDevicePaths_) reserved.insert(active);
        for (const auto &pending : deckRequests_) reserved.insert(pending);
        for (const auto &opening : startups_.keys()) reserved.insert(opening);
        for (const auto &active : sessions_.keys()) reserved.insert(active);
        if (!editDevicePath_.isEmpty()) reserved.insert(editDevicePath_);
        auto candidates = reserved.values();
        std::sort(candidates.begin(), candidates.end());
        for (const auto &candidate : candidates) {
            if (candidate != path && deckLinkPhysicalKey(candidate) == key)
                return candidate;
        }
        return {};
    }

    bool selectDeckLinkInput(const QString &path, int priority)
    {
        const auto key = deckLinkPhysicalKey(path);
        if (key.isEmpty()) return true;
        auto previous = preferredDeckLinkPaths_.value(key);
        if (!previous.isEmpty() && !sessions_.contains(previous) && !startups_.contains(previous) &&
            !deckDevicePaths_.values().contains(previous) && !deckRequests_.values().contains(previous) && previous != editDevicePath_) previous.clear();
        if (previous == path) return true;
        if (!previous.isEmpty()) {
            const bool bound = deckDevicePaths_.values().contains(previous);
            const int previousPriority = bound || deckRequests_.values().contains(previous) ? 2 : previous == editDevicePath_ ? 1 : 0;
            if (bound || priority <= previousPriority) {
                // An empty detail keeps standby pending too: its caller treats
                // a nonempty error as device failure and marks items OFFLINE.
                return false;
            }
            if (const auto job = startups_.value(previous)) job->invalidated = true;
            if (const auto session = sessions_.take(previous)) retireSession(session);
            healthyDevicePaths_.remove(previous);
            preferredDeckLinkPaths_.insert(key, path);
            emit owner_->sourceSuspended(previous);
            emit owner_->thumbnailReady(previous, {});
            if (previous == editDevicePath_) emit owner_->editPreviewReady(previous, {});
        }
        preferredDeckLinkPaths_.insert(key, path);
        return true;
    }

    bool ensureSource(const QString &devicePath, QString *errorDetail, int priority = 0)
    {
        if (errorDetail) {
            errorDetail->clear();
        }
        if (!(vtrpon::isDeckLinkSource(devicePath)
              ? vtrpon::deckLinkInputAllowed(devicePath, errorDetail)
              : vtrpon::isNetworkVideoSource(devicePath)
                ? vtrpon::networkVideoInputAllowed(devicePath, errorDetail)
                : vtrpon::uvcInputAllowed(devicePath, errorDetail))) {
            return false;
        }
        if (shutDown_ || devicePath.isEmpty()) {
            if (errorDetail) {
                *errorDetail = QStringLiteral(
                    "UVC device identification is missing.");
            }
            return false;
        }
#if !defined(_WIN32) && !defined(__APPLE__)
        if (errorDetail) {
            *errorDetail = QStringLiteral(
                "UVC input is available only on Windows.");
        }
        return false;
#else
        if (discoveryReady_ && !availableDevicePaths_.contains(devicePath)) {
            if (errorDetail) *errorDetail = QStringLiteral("The UVC device is disconnected or disabled.");
            return false;
        }
        if (retryAfterMs_.value(devicePath) > monotonicMs()) {
            if (errorDetail) *errorDetail = QStringLiteral("The UVC input is offline. Waiting to retry.");
            return false;
        }
        if (!selectDeckLinkInput(devicePath, std::max(priority, devicePath == editDevicePath_ ? 1 : 0))) return false;
        const auto physical = deckLinkPhysicalKey(devicePath);
        if (!physical.isEmpty()) {
            for (auto it = startups_.cbegin(); it != startups_.cend(); ++it)
                if (it.key() != devicePath && it.value()->invalidated && deckLinkPhysicalKey(it.key()) == physical) return false;
            for (auto it = retirements_.cbegin(); it != retirements_.cend(); ++it) {
                if (deckLinkPhysicalKey(it.key()) != physical) continue;
                if (!it.value()->done.load(std::memory_order_acquire)) return false;
                if (!it.value()->succeeded) {
                    if (errorDetail) *errorDetail = QStringLiteral("The previous DeckLink input could not be stopped safely.");
                    return false;
                }
            }
        }
        if (auto retiring = retirements_.find(devicePath); retiring != retirements_.end()) {
            if (!retiring.value()->done.load(std::memory_order_acquire)) return false;
            if (!retiring.value()->succeeded) {
                if (errorDetail) *errorDetail = QStringLiteral("The previous UVC session could not be stopped safely.");
                return false;
            }
            retirements_.erase(retiring);
        }
        if (sessions_.contains(devicePath)) {
            const auto session = sessions_.value(devicePath);
            if (session->errorGeneration != 0) {
                if (errorDetail) *errorDetail = session->errorDetail;
                return false;
            }
            return true;
        }
        if (startups_.contains(devicePath)) return true;
        auto job = std::make_shared<Startup>();
        job->session = std::make_shared<Session>();
        job->session->owner = owner_;
        job->session->devicePath = devicePath;
        const auto context = std::shared_ptr<GstContext>(
            programContext_ ? gst_context_ref(programContext_) : nullptr,
            [](GstContext *value) { if (value) gst_context_unref(value); });
        const auto name = QStringLiteral("uvc-session-%1").arg(nextSessionId_++);
        startups_.insert(devicePath, job);
        try {
            std::thread([job, context, name, width = outputWidth_, height = outputHeight_] {
                try {
                    job->succeeded = createSession(job->session, name, width, height, context.get());
                } catch (...) {
                    job->session->errorDetail = QStringLiteral("The UVC input worker failed while opening the device.");
                }
                {
                    std::lock_guard lock(job->completionMutex);
                    if (!job->abandoned) {
                        job->done.store(true, std::memory_order_release);
                        return;
                    }
                }
                // Main has stopped waiting. This worker owns everything it
                // needs to dispose a late result without touching a QObject.
                (void)releaseSession(job->session);
                job->done.store(true, std::memory_order_release);
            }).detach();
        } catch (...) {
            startups_.remove(devicePath);
            if (errorDetail) *errorDetail = QStringLiteral("Could not start the UVC input worker.");
            return false;
        }
        return true;
#endif
    }

    bool bindProgramDeck(int deckIndex, const QString &devicePath,
                         ProgramSource &source, QString *errorDetail)
    {
        source = {};
        if (deckIndex < 0 || deckIndex > 1) return false;
        deckRequests_.insert(deckIndex, devicePath);
        QString detail;
        if (!ensureSource(devicePath, &detail, 2)) {
            if (!detail.isEmpty()) deckRequests_.remove(deckIndex);
            if (errorDetail) *errorDetail = detail;
            return false;
        }
        const std::shared_ptr<Session> session = sessions_.value(devicePath);
        // Opening a camera runs on its own worker; the deck remains Loading.
        if (!session && startups_.contains(devicePath)) return false;
        if (!session || !session->pipeline
            || !session->programSinks[deckIndex]) {
            deckRequests_.remove(deckIndex);
            if (errorDetail) {
                *errorDetail = QStringLiteral(
                    "Could not connect the shared UVC session to ON AIR.");
            }
            return false;
        }
        const QString previousDevicePath =
            deckDevicePaths_.value(deckIndex);
        if (!previousDevicePath.isEmpty()
            && previousDevicePath != devicePath) {
            deckDevicePaths_.remove(deckIndex);
            updateProgramValves(previousDevicePath);
        }
        deckDevicePaths_.insert(deckIndex, devicePath);
        deckRequests_.remove(deckIndex);
        updateProgramValves(devicePath);
        releaseUnusedSessions();
        source.pipeline = session->pipeline;
        source.appSink = session->programSinks[deckIndex];
        return true;
    }

    void releaseProgramDeck(int deckIndex)
    {
        deckRequests_.remove(deckIndex);
        const QString devicePath = deckDevicePaths_.take(deckIndex);
        if (devicePath.isEmpty()) {
            return;
        }
        updateProgramValves(devicePath);
        releaseUnusedSessions();
    }

    void setPreviewDevicePaths(const QSet<QString> &devicePaths)
    {
        if (shutDown_) {
            return;
        }
        QSet<QString> normalized;
        for (const QString &devicePath : devicePaths) {
            if (!devicePath.isEmpty()) {
                normalized.insert(devicePath);
            }
        }
        previewDevicePaths_ = normalized;
        for (const QString &devicePath : previewDevicePaths_) {
            if ((discoveryReady_ && !availableDevicePaths_.contains(devicePath)) ||
                retryAfterMs_.value(devicePath) > monotonicMs()) continue;
            QString detail;
            if (!ensureSource(devicePath, &detail)) {
                if (!detail.isEmpty()) reportSourceFailure(devicePath, detail);
            }
        }
        releaseUnusedSessions();
    }

    void setEditPreviewPath(const QString &devicePath)
    {
        if (shutDown_ || editDevicePath_ == devicePath) return;
        const QString previous = editDevicePath_;
        editDevicePath_ = devicePath;
        updateEditValve(previous);
        if (!devicePath.isEmpty()) {
            QString detail;
            if (!ensureSource(devicePath, &detail) && !detail.isEmpty())
                reportSourceFailure(devicePath, detail);
            updateEditValve(devicePath);
        }
        releaseUnusedSessions();
    }

    bool sourceAvailable(const QString &path) const { return healthyDevicePaths_.contains(path); }

    bool sourceOffline(const QString &path) const
    {
        return failedDevicePaths_.contains(path) ||
            (discoveryReady_ && !availableDevicePaths_.contains(path));
    }

    void setRegisteredDevicePaths(const QSet<QString> &paths)
    {
        if (shutDown_) return;
        for (const auto &path : registeredDevicePaths_) {
            if (paths.contains(path)) continue;
            if (const auto job = startups_.value(path)) job->invalidated = true;
            healthyDevicePaths_.remove(path);
            failedDevicePaths_.remove(path);
            retryAfterMs_.remove(path);
        }
        registeredDevicePaths_ = paths;
        for (auto it = preferredDeckLinkPaths_.begin(); it != preferredDeckLinkPaths_.end();) {
            if (!paths.contains(it.value()) && !previewDevicePaths_.contains(it.value()) &&
                it.value() != editDevicePath_ && !deckDevicePaths_.values().contains(it.value()) &&
                !deckRequests_.values().contains(it.value()))
                it = preferredDeckLinkPaths_.erase(it);
            else ++it;
        }
    }

    void setAvailableDevicePaths(const QSet<QString> &paths)
    {
        if (shutDown_) return;
        discoveryReady_ = true;
        const auto previous = availableDevicePaths_;
        availableDevicePaths_ = paths;
        QSet<QString> tracked = registeredDevicePaths_;
        for (const auto &path : sessions_.keys()) tracked.insert(path);
        for (const auto &path : startups_.keys()) tracked.insert(path);
        for (const auto &path : tracked) {
            if (!paths.contains(path))
                reportSourceFailure(path, QStringLiteral("The UVC device is disconnected or disabled."));
            else if (!previous.contains(path)) retryAfterMs_.remove(path);
        }
    }

    void reportSourceFailure(const QString &path, const QString &detail)
    {
        if (shutDown_) return;
        const auto session = sessions_.value(path);
        if (const auto job = startups_.value(path)) job->invalidated = true;
        if (session && session->errorGeneration == 0) {
            ++session->errorGeneration;
            session->errorDetail = detail;
        }
        retryAfterMs_.insert(path, monotonicMs() + kRetryIntervalMs);
        const bool wasHealthy = healthyDevicePaths_.remove(path);
        const bool firstFailure = !failedDevicePaths_.contains(path);
        failedDevicePaths_.insert(path);
        const auto key = deckLinkPhysicalKey(path);
        if (!key.isEmpty() && preferredDeckLinkPaths_.value(key) == path &&
            !deckDevicePaths_.values().contains(path)) preferredDeckLinkPaths_.remove(key);
        // Synchronous observers detach only the affected Program/CUE input
        // before its producer is handed to the retirement worker.
        // Opening can fail before the first healthy frame has ever arrived.
        if (wasHealthy || firstFailure) emit owner_->sourceAvailabilityChanged(path, false);
        if (firstFailure) {
            emit owner_->sourceError(path, detail);
        }
        releaseUnusedSessions();
    }

    QImage latestThumbnail(const QString &devicePath) const
    {
        const std::shared_ptr<Session> session = sessions_.value(devicePath);
        if (!session) {
            return {};
        }
        return session->thumbnails->latest();
    }

    bool sourceIsPlaying(const QString &devicePath) const
    {
        const std::shared_ptr<Session> session = sessions_.value(devicePath);
        if (!session || !session->pipeline) {
            return false;
        }
        GstState current = GST_STATE_NULL;
        GstState pending = GST_STATE_VOID_PENDING;
        if (gst_element_get_state(session->pipeline, &current, &pending, 0)
            == GST_STATE_CHANGE_FAILURE) {
            return false;
        }
        return pending != GST_STATE_VOID_PENDING
            ? pending == GST_STATE_PLAYING
            : current == GST_STATE_PLAYING;
    }

    bool sourceErrorAfter(const QString &devicePath,
                          quint64 previousGeneration,
                          quint64 &currentGeneration,
                          QString &errorDetail) const
    {
        const std::shared_ptr<Session> session = sessions_.value(devicePath);
        if (!session || session->errorGeneration <= previousGeneration) {
            return false;
        }
        currentGeneration = session->errorGeneration;
        errorDetail = session->errorDetail;
        return true;
    }

    void shutdown()
    {
        requestShutdown();
    }

    bool shutdownComplete()
    {
        if (!shutDown_) return false;
        if (monotonicMs() - shutdownStartedMs_ >= kShutdownWorkerGraceMs) {
            abandonStartups();
            for (const auto &path : retirements_.keys()) {
                const auto job = retirements_.value(path);
                if (!job->done.load(std::memory_order_acquire) && !job->reported) {
                    job->reported = true;
                    qWarning() << "UVC shutdown stopped waiting for the isolated capture worker:" << path;
                }
            }
            // Each detached retirement worker retains its Session and job.
            // It has no manager/widget pointers, and consumers were detached
            // before requestShutdown. Never free or kill a blocked driver.
            return true;
        }
        if (!startups_.isEmpty()) return false;
        for (const auto &job : retirements_)
            if (!job->done.load(std::memory_order_acquire)) return false;
        return true;
    }

    bool sourceIsRetiring(const QString &devicePath) const
    {
        const auto job = retirements_.value(devicePath);
        return job && !job->done.load(std::memory_order_acquire);
    }

    void requestShutdown()
    {
        if (shutDown_) {
            return;
        }
        shutDown_ = true;
        shutdownStartedMs_ = monotonicMs();
        // Keep polling until any in-flight opener can retire its own result.
        previewDevicePaths_.clear();
        deckDevicePaths_.clear();
        deckRequests_.clear();
        const auto sessions = sessions_;
        sessions_.clear();
        for (const auto &session : sessions) retireSession(session);
    }

private:
    void abandonStartups()
    {
        const auto jobs = std::exchange(startups_, {});
        for (const auto &job : jobs) {
            {
                std::scoped_lock lock(job->session->callbackMutex);
                job->session->acceptingCallbacks = false;
                job->session->owner.clear();
            }
            bool completed;
            {
                std::lock_guard lock(job->completionMutex);
                completed = job->done.load(std::memory_order_acquire);
                job->abandoned = true;
            }
            if (completed) retireSession(job->session);
            else qWarning() << "UVC shutdown transferred late input cleanup to its opener:"
                            << job->session->devicePath;
        }
    }

#if defined(_WIN32) || defined(__APPLE__)
    static bool createSession(const std::shared_ptr<Session> &session,
                              const QString &sessionName, int outputWidth,
                              int outputHeight, GstContext *programContext)
    {
        QString *errorDetail = &session->errorDetail;
        const bool deckLink = vtrpon::isDeckLinkSource(session->devicePath);
        const bool network = deckLink || vtrpon::isNetworkVideoSource(session->devicePath);
#ifdef __APPLE__
        if (deckLink || (!network && !vtrpon::registerMacCaptureSources())) {
            *errorDetail = QStringLiteral("The selected input backend is unavailable on Mac.");
            return false;
        }
        const QString input = network
            ? QStringLiteral("appsrc name=%1-source is-live=true format=time do-timestamp=true block=false max-buffers=2 max-bytes=0 max-time=0 leaky-type=downstream ! ")
            : QStringLiteral("vtrponavfvideosrc name=%1-source ! ");
        const QString programConversion = QStringLiteral(
            "videoscale add-borders=true ! videoconvert ! "
            "video/x-raw,format=BGRA,width=%2,height=%3,pixel-aspect-ratio=1/1 ! ");
#else
        const QString programConversion = QStringLiteral(
            "d3d11upload ! video/x-raw(memory:D3D11Memory) ! "
            "d3d11convert add-borders=true border-color=0 ! "
            "video/x-raw(memory:D3D11Memory),format=RGBA,width=%2,"
            "height=%3,pixel-aspect-ratio=1/1 ! ");
        const QString input = deckLink
            ? QStringLiteral("appsrc name=%1-source is-live=true format=time do-timestamp=true block=false max-buffers=2 max-bytes=0 max-time=0 leaky-type=downstream ! videoconvert ! deinterlace mode=auto ! ")
            : network
            ? QStringLiteral("appsrc name=%1-source is-live=true format=time do-timestamp=true block=false max-buffers=2 max-bytes=0 max-time=0 leaky-type=downstream ! ")
            : QStringLiteral("mfvideosrc name=%1-source do-timestamp=true ! decodebin ! ");
#endif
        const QString description = (input + QStringLiteral(
            "tee name=%1-tee "
            "%1-tee. ! valve name=%1-program-a-valve drop=true ! "
            "queue max-size-buffers=2 max-size-bytes=0 "
            "max-size-time=0 leaky=downstream ! videoconvert ! ") + programConversion + QStringLiteral(
            "appsink name=%1-program-a-sink max-buffers=2 "
            "leaky-type=downstream wait-on-eos=false sync=true qos=false "
            "async=false enable-last-sample=false "
            "%1-tee. ! valve name=%1-program-b-valve drop=true ! "
            "queue max-size-buffers=2 max-size-bytes=0 "
            "max-size-time=0 leaky=downstream ! videoconvert ! ") + programConversion + QStringLiteral(
            "appsink name=%1-program-b-sink max-buffers=2 "
            "leaky-type=downstream wait-on-eos=false sync=true qos=false "
            "async=false enable-last-sample=false "
            "%1-tee. ! queue max-size-buffers=1 max-size-bytes=0 "
            "max-size-time=0 leaky=downstream ! videorate drop-only=true ! "
            "videoscale add-borders=true ! videoconvert ! "
            "video/x-raw,format=RGBA,width=%4,height=%5,"
            "pixel-aspect-ratio=1/1,framerate=%6/1 ! "
            "appsink name=%1-thumbnail-sink max-buffers=1 drop=true "
            "wait-on-eos=false sync=false enable-last-sample=false "
            "%1-tee. ! valve name=%1-edit-valve drop=true ! "
            "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
            "videorate drop-only=true ! videoscale add-borders=true ! videoconvert ! "
            "video/x-raw,format=RGBA,width=%7,height=%8,"
            "pixel-aspect-ratio=1/1,framerate=%9/1 ! "
            "appsink name=%1-edit-sink max-buffers=1 drop=true async=false "
            "wait-on-eos=false sync=false enable-last-sample=false"))
                .arg(sessionName)
                .arg(outputWidth)
                .arg(outputHeight)
                .arg(kThumbnailWidth)
                .arg(kThumbnailHeight)
                 .arg(kThumbnailFps)
                 .arg(kEditPreviewWidth)
                 .arg(kEditPreviewHeight)
                 .arg(kEditPreviewFps);

        GError *parseError = nullptr;
        GstElement *pipeline = gst_parse_launch(
            description.toUtf8().constData(), &parseError);
        if (parseError) {
            if (errorDetail) {
                *errorDetail = QString::fromUtf8(parseError->message);
            }
            qWarning() << "Failed to create shared UVC session:"
                       << parseError->message;
            g_error_free(parseError);
        }
        if (!pipeline || !GST_IS_PIPELINE(pipeline)) {
            if (pipeline) {
                gst_object_unref(pipeline);
            }
            if (errorDetail && errorDetail->isEmpty()) {
                *errorDetail = QStringLiteral(
                    "Could not create the shared UVC session.");
            }
            return false;
        }

        session->pipeline = pipeline;
        session->source = gst_bin_get_by_name(
            GST_BIN(pipeline),
            (sessionName + QStringLiteral("-source")).toUtf8().constData());
        for (int deckIndex = 0; deckIndex < 2; ++deckIndex) {
            const QString deckName = deckIndex == 0
                ? QStringLiteral("a") : QStringLiteral("b");
            session->programValves[deckIndex] = gst_bin_get_by_name(
                GST_BIN(pipeline),
                (sessionName + QStringLiteral("-program-") + deckName
                 + QStringLiteral("-valve")).toUtf8().constData());
            session->programSinks[deckIndex] = gst_bin_get_by_name(
                GST_BIN(pipeline),
                (sessionName + QStringLiteral("-program-") + deckName
                 + QStringLiteral("-sink")).toUtf8().constData());
        }
        session->thumbnailSink = gst_bin_get_by_name(
            GST_BIN(pipeline),
            (sessionName + QStringLiteral("-thumbnail-sink"))
                .toUtf8().constData());
        session->editValve = gst_bin_get_by_name(
            GST_BIN(pipeline), (sessionName + QStringLiteral("-edit-valve")).toUtf8().constData());
        session->editSink = gst_bin_get_by_name(
            GST_BIN(pipeline), (sessionName + QStringLiteral("-edit-sink")).toUtf8().constData());
        session->bus = gst_element_get_bus(pipeline);
        if (!session->source || !session->programValves[0]
            || !session->programValves[1] || !session->programSinks[0]
            || !GST_IS_APP_SINK(session->programSinks[0])
            || !session->programSinks[1]
            || !GST_IS_APP_SINK(session->programSinks[1])
            || !session->thumbnailSink
            || !GST_IS_APP_SINK(session->thumbnailSink) || !session->editValve
            || !session->editSink || !GST_IS_APP_SINK(session->editSink) || !session->bus) {
            if (errorDetail && errorDetail->isEmpty()) {
                *errorDetail = QStringLiteral(
                    "Could not create the shared UVC session elements.");
            }
            return false;
        }

        const QByteArray encodedPath = session->devicePath.toUtf8();
        if (!network)
            g_object_set(session->source,
#ifdef __APPLE__
                "device",
#else
                "device-path",
#endif
                encodedPath.constData(), nullptr);
        if (programContext) {
            gst_element_set_context(session->pipeline, programContext);
        }
        {
            std::scoped_lock lock(session->callbackMutex);
            session->acceptingCallbacks = true;
        }
        GstAppSinkCallbacks callbacks = {};
        callbacks.new_preroll = &Impl::onNewThumbnailPreroll;
        callbacks.new_sample = &Impl::onNewThumbnailSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(session->thumbnailSink),
                                   &callbacks, session.get(), nullptr);
        GstAppSinkCallbacks editCallbacks = {};
        editCallbacks.new_preroll = &Impl::onNewEditPreroll;
        editCallbacks.new_sample = &Impl::onNewEditSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(session->editSink),
                                   &editCallbacks, session.get(), nullptr);
        if (gst_element_set_state(session->pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
            if (errorDetail && errorDetail->isEmpty()) {
                *errorDetail = session->errorDetail.isEmpty()
                    ? QStringLiteral("Could not start the UVC device.")
                    : session->errorDetail;
            }
            return false;
        }
        if (deckLink) session->deckLinkReceiver = std::make_shared<vtrpon::DeckLinkReceiver>(session->devicePath, session->source, false);
        else if (network) session->networkReceiver = std::make_shared<vtrpon::NetworkVideoReceiver>(
            session->devicePath, session->source, false);
        return true;
    }
#endif

    void releaseUnusedSessions()
    {
        QSet<QString> usedDevicePaths = previewDevicePaths_;
        if (!editDevicePath_.isEmpty()) usedDevicePaths.insert(editDevicePath_);
        for (auto it = deckDevicePaths_.cbegin();
             it != deckDevicePaths_.cend(); ++it) {
            usedDevicePaths.insert(it.value());
        }
        const QList<QString> devicePaths = sessions_.keys();
        for (const QString &devicePath : devicePaths) {
            const auto existing = sessions_.value(devicePath);
            const bool bound = deckDevicePaths_.values().contains(devicePath);
            const bool probing = registeredDevicePaths_.contains(devicePath) &&
                !healthyDevicePaths_.contains(devicePath) && existing->errorGeneration == 0;
            if (bound || (!deckLinkSuppressed(devicePath) && existing->errorGeneration == 0 &&
                          (usedDevicePaths.contains(devicePath) || probing))) {
                continue;
            }
            const std::shared_ptr<Session> session = sessions_.take(devicePath);
            if (session) {
                retireSession(session);
            }
        }
    }

    static void quarantineSession(const std::shared_ptr<Session> &session)
    {
        static auto *const mutex = new std::mutex;
        static auto *const retained = new std::vector<std::shared_ptr<Session>>;
        std::lock_guard lock(*mutex);
        retained->push_back(session);
    }

    struct Retirement {
        std::atomic_bool done{false};
        bool succeeded = false;
        bool reported = false; // GUI thread only.
    };

    void retireSession(const std::shared_ptr<Session> &session)
    {
        {
            std::scoped_lock lock(session->callbackMutex);
            session->acceptingCallbacks = false;
            session->owner.clear();
        }
        auto job = std::make_shared<Retirement>();
        retirements_.insert(session->devicePath, job);
        try {
            std::thread([session, job] {
                job->succeeded = releaseSession(session);
                job->done.store(true, std::memory_order_release);
            }).detach();
        } catch (...) {
            quarantineSession(session);
            qCritical() << "Could not start UVC retirement worker; retaining disabled session";
            job->done.store(true, std::memory_order_release);
        }
    }

    static bool releaseSession(const std::shared_ptr<Session> &sessionHolder)
    {
        Session &session = *sessionHolder;
        session.networkReceiver.reset();
        session.deckLinkReceiver.reset();
        {
            std::scoped_lock lock(session.callbackMutex);
            session.acceptingCallbacks = false;
        }
        if (session.thumbnailSink && GST_IS_APP_SINK(session.thumbnailSink)) {
            GstAppSinkCallbacks disabledCallbacks = {};
            gst_app_sink_set_callbacks(GST_APP_SINK(session.thumbnailSink),
                                       &disabledCallbacks, nullptr, nullptr);
        }
        if (session.editSink && GST_IS_APP_SINK(session.editSink)) {
            GstAppSinkCallbacks disabledCallbacks = {};
            gst_app_sink_set_callbacks(GST_APP_SINK(session.editSink),
                                       &disabledCallbacks, nullptr, nullptr);
        }
        const bool stopped = stopPipeline(session.pipeline);
        {
            std::unique_lock lock(session.callbackMutex);
            session.callbackCondition.wait(lock, [&session] {
                return session.activeCallbacks == 0;
            });
        }
        if (!stopped) {
            qCritical() << "Shared UVC session did not confirm NULL state; "
                           "retaining it to avoid freeing a live GStreamer "
                           "backend:"
                        << session.devicePath;
            // A live pipeline may still use Session as callback storage.
            quarantineSession(sessionHolder);
            return false;
        }
        if (session.bus) {
            gst_object_unref(session.bus);
            session.bus = nullptr;
        }
        if (session.thumbnailSink) {
            gst_object_unref(session.thumbnailSink);
            session.thumbnailSink = nullptr;
        }
        if (session.editSink) {
            gst_object_unref(session.editSink);
            session.editSink = nullptr;
        }
        if (session.editValve) {
            gst_object_unref(session.editValve);
            session.editValve = nullptr;
        }
        for (GstElement *&programSink : session.programSinks) {
            if (programSink) {
                gst_object_unref(programSink);
                programSink = nullptr;
            }
        }
        for (GstElement *&programValve : session.programValves) {
            if (programValve) {
                gst_object_unref(programValve);
                programValve = nullptr;
            }
        }
        if (session.source) {
            gst_object_unref(session.source);
            session.source = nullptr;
        }
        if (session.pipeline) {
            gst_object_unref(session.pipeline);
            session.pipeline = nullptr;
        }
        return true;
    }

    void pollBuses()
    {
        for (const auto &path : startups_.keys()) {
            const auto job = startups_.value(path);
            if (!job->done.load(std::memory_order_acquire)) {
                if (!shutDown_ && !job->invalidated &&
                    monotonicMs() - job->session->startedMs > 2 * kFrameTimeoutMs)
                    reportSourceFailure(path, QStringLiteral("The UVC device did not finish opening."));
                continue;
            }
            startups_.remove(path);
            if (shutDown_ || job->invalidated) {
                retireSession(job->session);
                continue;
            }
            sessions_.insert(path, job->session);
            updateEditValve(path);
            if (!job->succeeded) {
                const QString busError = pollBus(*job->session);
                reportSourceFailure(path, busError.isEmpty() ? job->session->errorDetail : busError);
            }
        }
        if (shutDown_) {
            if (shutdownComplete()) busTimer_.stop();
            return;
        }
        // Reacquire a preview only after its previous capture has fully stopped.
        // Failed sessions remain quarantined and cannot reopen the same device.
        for (const auto &path : retirements_.keys()) {
            const auto job = retirements_.value(path);
            if (!job->done.load(std::memory_order_acquire)) continue;
            if (!job->succeeded) {
                if (!job->reported) {
                    job->reported = true;
                    reportSourceFailure(path, QStringLiteral("The previous UVC session could not be stopped safely."));
                }
                continue;
            }
            retirements_.remove(path);
        }
        // Signals may detach a deck and remove its session. Iterate a snapshot.
        const auto sessions = sessions_;
        for (const auto &session : sessions) {
            if (sessions_.value(session->devicePath) != session || session->errorGeneration) continue;
            QString detail = pollBus(*session);
            const qint64 lastFrame = session->lastFrameMs.load(std::memory_order_acquire);
            // NDI screen capture sources may publish only when pixels change.
            // Once the first frame confirms the connection, the receiver's
            // GStreamer error and source discovery report actual disconnects.
            const bool network = vtrpon::isNetworkVideoSource(session->devicePath);
            if (detail.isEmpty() && (!network || !lastFrame) &&
                monotonicMs() - (lastFrame ? lastFrame : session->startedMs) > kFrameTimeoutMs)
                detail = network
                    ? QStringLiteral("No video frames have arrived from the NDI input for 5 seconds.")
                    : QStringLiteral("No video frames have arrived from the UVC input for 5 seconds.");
            if (!detail.isEmpty()) {
                reportSourceFailure(session->devicePath, detail);
            } else if (lastFrame && !healthyDevicePaths_.contains(session->devicePath)) {
                healthyDevicePaths_.insert(session->devicePath);
                failedDevicePaths_.remove(session->devicePath);
                retryAfterMs_.remove(session->devicePath);
                emit owner_->sourceAvailabilityChanged(session->devicePath, true);
                // A hidden item's recovery probe may retire before its queued
                // thumbnail callback runs. Deliver the confirmed frame first.
                emit owner_->thumbnailReady(session->devicePath, session->thumbnails->latest());
                if (registeredDevicePaths_.contains(session->devicePath) &&
                    !previewDevicePaths_.contains(session->devicePath)) releaseUnusedSessions();
            }
        }
        QSet<QString> retryPaths = registeredDevicePaths_;
        retryPaths.unite(previewDevicePaths_);
        for (const auto &path : retryPaths) {
            if (sessions_.contains(path) || startups_.contains(path) ||
                (healthyDevicePaths_.contains(path) && !previewDevicePaths_.contains(path)) ||
                (discoveryReady_ && !availableDevicePaths_.contains(path)) ||
                retryAfterMs_.value(path) > monotonicMs()) continue;
            QString detail;
            if (!ensureSource(path, &detail) && !detail.isEmpty()) reportSourceFailure(path, detail);
        }
    }

    void updateProgramValves(const QString &devicePath)
    {
        const std::shared_ptr<Session> session = sessions_.value(devicePath);
        if (!session) {
            return;
        }
        for (int deckIndex = 0; deckIndex < 2; ++deckIndex) {
            if (session->programValves[deckIndex]) {
                const bool usedByDeck =
                    deckDevicePaths_.value(deckIndex) == devicePath;
                g_object_set(session->programValves[deckIndex], "drop",
                             !usedByDeck, nullptr);
            }
        }
    }

    void updateEditValve(const QString &devicePath)
    {
        const auto session = sessions_.value(devicePath);
        if (session && session->editValve)
            g_object_set(session->editValve, "drop", devicePath != editDevicePath_, nullptr);
    }

    static QString pollBus(Session &session)
    {
        QString detail;
        if (!session.bus) {
            return detail;
        }
        while (GstMessage *message = gst_bus_pop(session.bus)) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                qWarning().noquote()
                    << "GST_BUS" << (error ? error->message : "Unknown error")
                    << (debug ? debug : "");
                detail = QString::fromUtf8(
                    error ? error->message : "Unknown UVC GStreamer error");
                g_clear_error(&error);
                g_free(debug);
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && detail.isEmpty()) {
                detail = QStringLiteral("The UVC input stream ended.");
            }
            gst_message_unref(message);
        }
        return detail;
    }

    static GstFlowReturn storePreview(GstAppSink *sink, gpointer userData,
                                      bool preroll, bool edit)
    {
        auto &session = *static_cast<Session *>(userData);
        CallbackScope callbackScope(session);
        if (!callbackScope) {
            return GST_FLOW_FLUSHING;
        }
        GstSample *sample = preroll ? gst_app_sink_pull_preroll(sink)
                                    : gst_app_sink_pull_sample(sink);
        if (!sample) {
            return GST_FLOW_EOS;
        }
        QImage image = imageFromSample(sample);
        gst_sample_unref(sample);
        if (image.isNull()) {
            return GST_FLOW_OK;
        }
        if (!edit)
            session.lastFrameMs.store(monotonicMs(), std::memory_order_release);
        const auto mailbox = edit ? session.editFrames : session.thumbnails;
        if (!mailbox->publish(std::move(image))) return GST_FLOW_OK;
        std::scoped_lock ownerLock(session.callbackMutex);
        if (!session.acceptingCallbacks || !session.owner) return GST_FLOW_FLUSHING;
        const QPointer<UvcSourceSessionManager> owner = session.owner;
        const QString devicePath = session.devicePath;
        if (!QMetaObject::invokeMethod(
            owner, [owner, devicePath, mailbox, edit] {
                const QImage latest = mailbox->consume();
                if (!owner || owner->impl_->shutDown_) return;
                const auto current = owner->impl_->sessions_.value(devicePath);
                // A device path may already belong to a newly created session.
                if (!current || current->errorGeneration ||
                    (edit ? current->editFrames != mailbox : current->thumbnails != mailbox)) return;
                if (edit) {
                    if (owner->impl_->editDevicePath_ == devicePath)
                        emit owner->editPreviewReady(devicePath, latest);
                } else {
                    emit owner->thumbnailReady(devicePath, latest);
                }
            }, Qt::QueuedConnection)) {
            (void)mailbox->consume();
        }
        return GST_FLOW_OK;
    }

    static GstFlowReturn onNewThumbnailPreroll(GstAppSink *sink,
                                                gpointer userData)
    {
        return storePreview(sink, userData, true, false);
    }

    static GstFlowReturn onNewThumbnailSample(GstAppSink *sink,
                                               gpointer userData)
    {
        return storePreview(sink, userData, false, false);
    }

    static GstFlowReturn onNewEditPreroll(GstAppSink *sink, gpointer userData)
    {
        return storePreview(sink, userData, true, true);
    }

    static GstFlowReturn onNewEditSample(GstAppSink *sink, gpointer userData)
    {
        return storePreview(sink, userData, false, true);
    }

    UvcSourceSessionManager *owner_ = nullptr;
    QTimer busTimer_;
    QHash<QString, std::shared_ptr<Session>> sessions_;
    QHash<int, QString> deckDevicePaths_;
    QHash<int, QString> deckRequests_;
    QHash<QString, QString> preferredDeckLinkPaths_;
    QString editDevicePath_;
    QSet<QString> previewDevicePaths_;
    QSet<QString> registeredDevicePaths_;
    QSet<QString> availableDevicePaths_;
    QSet<QString> healthyDevicePaths_;
    QSet<QString> failedDevicePaths_;
    QHash<QString, qint64> retryAfterMs_;
    QHash<QString, std::shared_ptr<Startup>> startups_;
    bool discoveryReady_ = false;
    GstContext *programContext_ = nullptr;
    quint64 nextSessionId_ = 1;
    int outputWidth_ = 1920;
    int outputHeight_ = 1080;
    bool shutDown_ = false;
    qint64 shutdownStartedMs_ = 0;
    QHash<QString, std::shared_ptr<Retirement>> retirements_;
};

UvcSourceSessionManager::UvcSourceSessionManager(bool use4kProgramOutput,
                                                 QObject *parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this, use4kProgramOutput))
{
}

UvcSourceSessionManager::~UvcSourceSessionManager() = default;

void UvcSourceSessionManager::setProgramContext(GstContext *context)
{
    impl_->setProgramContext(context);
}

bool UvcSourceSessionManager::ensureSource(const QString &devicePath,
                                           QString *errorDetail)
{
    return impl_->ensureSource(devicePath, errorDetail);
}

bool UvcSourceSessionManager::bindProgramDeck(int deckIndex,
                                              const QString &devicePath,
                                              ProgramSource &source,
                                              QString *errorDetail)
{
    return impl_->bindProgramDeck(deckIndex, devicePath, source,
                                  errorDetail);
}

void UvcSourceSessionManager::releaseProgramDeck(int deckIndex)
{
    impl_->releaseProgramDeck(deckIndex);
}

void UvcSourceSessionManager::setPreviewDevicePaths(
    const QSet<QString> &devicePaths)
{
    impl_->setPreviewDevicePaths(devicePaths);
}

void UvcSourceSessionManager::setEditPreviewPath(const QString &devicePath)
{
    impl_->setEditPreviewPath(devicePath);
}

void UvcSourceSessionManager::setRegisteredDevicePaths(const QSet<QString> &paths)
{
    impl_->setRegisteredDevicePaths(paths);
}

void UvcSourceSessionManager::setAvailableDevicePaths(const QSet<QString> &paths)
{
    impl_->setAvailableDevicePaths(paths);
}

bool UvcSourceSessionManager::sourceAvailable(const QString &path) const
{
    return impl_->sourceAvailable(path);
}

bool UvcSourceSessionManager::sourceOffline(const QString &path) const
{
    return impl_->sourceOffline(path);
}

bool UvcSourceSessionManager::sourceSuspendedForPriority(const QString &path) const
{
    return impl_->deckLinkSuppressed(path);
}

QString UvcSourceSessionManager::conflictingDeckLinkInput(const QString &path) const
{
    return impl_->conflictingDeckLinkInput(path);
}

void UvcSourceSessionManager::reportSourceFailure(const QString &path, const QString &detail)
{
    impl_->reportSourceFailure(path, detail);
}

QImage UvcSourceSessionManager::latestThumbnail(
    const QString &devicePath) const
{
    return impl_->latestThumbnail(devicePath);
}

bool UvcSourceSessionManager::sourceIsPlaying(
    const QString &devicePath) const
{
    return impl_->sourceIsPlaying(devicePath);
}

bool UvcSourceSessionManager::sourceErrorAfter(
    const QString &devicePath, quint64 previousGeneration,
    quint64 &currentGeneration, QString &errorDetail) const
{
    return impl_->sourceErrorAfter(devicePath, previousGeneration,
                                   currentGeneration, errorDetail);
}

void UvcSourceSessionManager::shutdown()
{
    impl_->shutdown();
}

void UvcSourceSessionManager::requestShutdown()
{
    impl_->requestShutdown();
}

bool UvcSourceSessionManager::shutdownComplete() const
{
    return impl_->shutdownComplete();
}

bool UvcSourceSessionManager::sourceIsRetiring(const QString &devicePath) const
{
    return impl_->sourceIsRetiring(devicePath);
}
