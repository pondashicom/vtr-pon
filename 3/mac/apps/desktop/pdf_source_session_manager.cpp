#include "pdf_source_session_manager.h"

#include "pdf_document_backend.h"

#include <QDebug>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <memory>
#include <mutex>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int kDeckCount = 2;
constexpr int kCaptureFramesPerSecond = 30;
constexpr int kCanvasWidth = PdfDocumentBackend::kDefaultCanvasWidth;
constexpr int kCanvasHeight = PdfDocumentBackend::kDefaultCanvasHeight;
constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;

quint64 monotonicMilliseconds() {
  return static_cast<quint64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

QString frameValidationError(const QImage &frame) {
  if (frame.isNull()) {
    return QStringLiteral("The PDF page could not be rendered.");
  }
  if (frame.size() != QSize(kCanvasWidth, kCanvasHeight)) {
    return QStringLiteral(
               "The PDF renderer returned an unexpected frame size: %1 x %2.")
        .arg(frame.width())
        .arg(frame.height());
  }
  if (frame.format() != QImage::Format_ARGB32) {
    return QStringLiteral(
        "The PDF renderer returned an unsupported pixel format.");
  }
  constexpr qsizetype expectedBytesPerLine = kCanvasWidth * 4;
  constexpr qsizetype expectedByteCount =
      expectedBytesPerLine * kCanvasHeight;
  if (frame.bytesPerLine() != expectedBytesPerLine ||
      frame.sizeInBytes() != expectedByteCount) {
    return QStringLiteral(
        "The PDF renderer returned an unsupported frame layout.");
  }
  return {};
}

bool stopPipeline(GstElement *pipeline, GstClockTime timeout) {
  if (!pipeline) {
    return true;
  }
  if (gst_element_set_state(pipeline, GST_STATE_NULL) ==
      GST_STATE_CHANGE_FAILURE) {
    return false;
  }
  GstState current = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn result = gst_element_get_state(
      pipeline, &current, &pending, timeout);
  return result != GST_STATE_CHANGE_FAILURE && current == GST_STATE_NULL &&
         pending == GST_STATE_VOID_PENDING;
}

} // namespace

class PdfSourceSessionManager::Impl final {
public:
  struct CachedPage {
    int pageIndex = -1;
    QImage frame;
  };

  struct Session {
    int deckIndex = -1;
    QString filePath;
    std::unique_ptr<PdfDocumentBackend> document;
    int currentPageIndex = 0;
    QString currentPageLabel;
    QString navigationErrorDetail;
    std::vector<CachedPage> pageCache;
    mutable std::mutex frameMutex;
    QImage committedFrame;
    GstElement *pipeline = nullptr;
    GstElement *appSource = nullptr;
    GstElement *appSink = nullptr;
    GstBus *bus = nullptr;
    std::jthread captureThread;
    quint64 captureStartedTickMs = 0;
    std::atomic<quint64> lastFrameTickMs{0};
    mutable std::mutex errorMutex;
    quint64 errorGeneration = 0;
    QString errorDetail;
    std::mutex navigationMutex;
    PdfNavigationState navigationState;
    int navigationCommand = 0;
    quint64 navigationGeneration = 0;
    std::optional<PdfCommandResponse> navigationResponse;
    std::atomic_bool released{false};
    std::atomic_bool suspended{false};
    std::atomic_bool inputReady{false};
    bool initializationAttempted = false;
    GstContext *context = nullptr;
  };

  Impl(int outputWidth, int outputHeight)
      : outputWidth_((std::max)(1, outputWidth)),
        outputHeight_((std::max)(1, outputHeight)),
        worker_([this](std::stop_token stop) { runWorker(stop); }) {}

  ~Impl() {
    shutdown();
    gst_clear_context(&programContext_);
  }

  void setProgramContext(GstContext *context) {
    if (shutDown_ || programContext_ == context) {
      return;
    }
    gst_clear_context(&programContext_);
    programContext_ = context ? gst_context_ref(context) : nullptr;
  }

  bool bindProgramDeck(int deckIndex, const QString &filePath,
                       ProgramSource &source, QString *errorDetail) {
    source = {};
    if (errorDetail) {
      errorDetail->clear();
    }
    if (shutDown_ || deckIndex < 0 || deckIndex >= kDeckCount ||
        filePath.isEmpty()) {
      if (errorDetail) {
        *errorDetail =
            QStringLiteral("The PDF ON AIR source request is invalid.");
      }
      return false;
    }
    releaseProgramDeck(deckIndex);
    auto session = std::make_shared<Session>();
    session->deckIndex = deckIndex;
    session->filePath = filePath;
    session->context = programContext_ ? gst_context_ref(programContext_) : nullptr;
    session->navigationState.active = true;
    session->navigationState.busy = true;
    sessions_[deckIndex] = session;
    {
      std::scoped_lock lock(workerMutex_);
      workerSessions_.push_back(std::move(session));
    }
    workerWake_.notify_one();
    return true;
  }

  ProgramSource programSource(int deckIndex) const {
    const Session *session = sessionAt(deckIndex);
    if (!session || !session->inputReady.load(std::memory_order_acquire)) return {};
    return {session->pipeline, session->appSink};
  }

  // Called only on the document worker. The QPdfDocument is created, used,
  // and destroyed there; the UI reads published snapshots only.
  bool initializeSession(const std::shared_ptr<Session> &session, QString *errorDetail) {
    const auto &filePath = session->filePath;
    const int deckIndex = session->deckIndex;
    session->document = std::make_unique<PdfDocumentBackend>();
    if (!session->document->load(filePath)) {
      if (errorDetail) {
        *errorDetail = session->document->errorDetail();
      }
      return false;
    }
    session->filePath = session->document->filePath();
    if (session->document->pageCount() < 1 ||
        session->document->pageCount() >
            PdfDocumentBackend::kMaximumPageCount) {
      if (errorDetail) {
        *errorDetail = QStringLiteral(
            "The PDF page count is outside the supported range of 1 to %1.")
                           .arg(PdfDocumentBackend::kMaximumPageCount);
      }
      return false;
    }

    if (session->released.load(std::memory_order_acquire)) return true;
    QImage firstFrame = session->document->renderPage(
        0, QSize(kCanvasWidth, kCanvasHeight));
    QString detail = frameValidationError(firstFrame);
    if (!detail.isEmpty()) {
      if (!session->document->errorDetail().isEmpty()) {
        detail = session->document->errorDetail();
      }
      if (errorDetail) {
        *errorDetail = detail;
      }
      return false;
    }
    session->currentPageLabel = session->document->pageLabel(0);
    session->pageCache.reserve(3);
    session->pageCache.push_back(CachedPage{0, firstFrame});
    session->committedFrame = firstFrame;
    session->pageCache =
        cacheCurrentAndAdjacent(*session, 0, firstFrame);

    if (session->released.load(std::memory_order_acquire)) return true;
    const QString prefix = QStringLiteral("pdf-deck-%1").arg(deckIndex);
#ifdef __APPLE__
    const QString description = QStringLiteral(
        "appsrc name=%1-source is-live=true format=time do-timestamp=true "
        "block=false max-buffers=2 max-bytes=0 max-time=0 leaky-type=downstream "
        "caps=video/x-raw,format=BGRA,width=%2,height=%3,framerate=%4/1 ! "
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "videoscale add-borders=true ! "
        "video/x-raw,format=BGRA,width=%5,height=%6,pixel-aspect-ratio=1/1 ! "
        "appsink name=%1-sink max-buffers=2 leaky-type=downstream "
        "wait-on-eos=false sync=true qos=false async=false enable-last-sample=false")
        .arg(prefix).arg(kCanvasWidth).arg(kCanvasHeight)
        .arg(kCaptureFramesPerSecond).arg(outputWidth_).arg(outputHeight_);
#else
    const QString description = QStringLiteral(
        "appsrc name=%1-source is-live=true do-timestamp=true "
        "format=time block=false "
        "caps=video/x-raw,format=BGRA,width=%2,height=%3,"
        "pixel-aspect-ratio=1/1,framerate=%4/1 ! "
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 "
        "leaky=downstream ! videoconvert ! d3d11upload ! "
        "video/x-raw(memory:D3D11Memory) ! "
        "d3d11convert add-borders=true border-color=0 ! "
        "video/x-raw(memory:D3D11Memory),format=RGBA,width=%5,"
        "height=%6,pixel-aspect-ratio=1/1 ! "
        "appsink name=%1-sink max-buffers=2 leaky-type=downstream "
        "wait-on-eos=false sync=true qos=false async=false "
        "enable-last-sample=false")
                                    .arg(prefix)
                                    .arg(kCanvasWidth)
                                    .arg(kCanvasHeight)
                                    .arg(kCaptureFramesPerSecond)
                                    .arg(outputWidth_)
                                    .arg(outputHeight_);
#endif
    GError *parseError = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (parseError) {
      detail = QString::fromUtf8(parseError->message);
      g_error_free(parseError);
    }
    if (!pipeline || !GST_IS_PIPELINE(pipeline)) {
      if (pipeline) {
        gst_object_unref(pipeline);
      }
      if (errorDetail) {
        *errorDetail =
            detail.isEmpty()
                ? QStringLiteral("Could not create the PDF video input.")
                : detail;
      }
      return false;
    }

    session->captureStartedTickMs = monotonicMilliseconds();
    session->pipeline = pipeline;
    session->appSource = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (prefix + QStringLiteral("-source")).toUtf8().constData());
    session->appSink = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (prefix + QStringLiteral("-sink")).toUtf8().constData());
    session->bus = gst_element_get_bus(pipeline);
    if (!session->appSource || !GST_IS_APP_SRC(session->appSource) ||
        !session->appSink || !GST_IS_APP_SINK(session->appSink) ||
        !session->bus) {
      if (errorDetail) {
        *errorDetail =
            QStringLiteral("Could not create the PDF source elements.");
      }
      return false;
    }
    if (session->context) {
      gst_element_set_context(session->pipeline, session->context);
    }
    if (gst_element_set_state(session->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      pollBus(*session);
      if (errorDetail) {
        std::scoped_lock lock(session->errorMutex);
        *errorDetail =
            session->errorDetail.isEmpty()
                ? QStringLiteral("Could not start the PDF video input.")
                : session->errorDetail;
      }
      return false;
    }

    if (session->released.load(std::memory_order_acquire)) return true;
    startCapture(*session);
    publishNavigation(*session);
    session->inputReady.store(true, std::memory_order_release);
    return true;
  }

  bool releaseProgramDeck(int deckIndex) {
    if (deckIndex < 0 || deckIndex >= kDeckCount) return false;
    auto session = std::exchange(sessions_[deckIndex], {});
    if (session) session->released.store(true, std::memory_order_release);
    workerWake_.notify_one();
    return true;
  }

  void suspendProgramDeck(int deckIndex) {
    if (auto *session = sessionAt(deckIndex)) {
      session->suspended.store(true, std::memory_order_release);
    }
  }

  bool tryReleaseProgramDeck(int deckIndex) { return releaseProgramDeck(deckIndex); }

  bool sourceIsPlaying(int deckIndex) const {
    return programSource(deckIndex).isValid();
  }

  bool sourceErrorAfter(int deckIndex, quint64 previousGeneration,
                        quint64 &currentGeneration,
                        QString &errorDetail) {
    Session *session = sessionAt(deckIndex);
    if (!session) {
      return false;
    }
    std::scoped_lock lock(session->errorMutex);
    currentGeneration = session->errorGeneration;
    if (currentGeneration <= previousGeneration) {
      return false;
    }
    errorDetail = session->errorDetail;
    return true;
  }

  PdfNavigationState navigationState(int deckIndex) {
    Session *session = sessionAt(deckIndex);
    if (!session) return {};
    std::scoped_lock lock(session->navigationMutex);
    auto state = session->navigationState;
    state.busy = state.busy || session->navigationCommand != 0;
    state.canGoPrevious = state.canGoPrevious && !state.busy;
    state.canGoNext = state.canGoNext && !state.busy;
    return state;
  }

  PdfCommandResponse next(int deckIndex) {
    return navigate(deckIndex, 1);
  }

  PdfCommandResponse previous(int deckIndex) {
    return navigate(deckIndex, -1);
  }

  void cancelNavigation(int deckIndex) {
    auto *session = sessionAt(deckIndex);
    if (!session) return;
    std::scoped_lock lock(session->navigationMutex);
    ++session->navigationGeneration;
    session->navigationCommand = 0;
    session->navigationResponse.reset();
  }

  void requestShutdown() {
    shutDown_ = true;
    for (int index = 0; index < kDeckCount; ++index) releaseProgramDeck(index);
    worker_.request_stop();
    workerWake_.notify_one();
  }

  bool shutdownComplete() const { return workerFinished_.load(std::memory_order_acquire); }

  bool shutdown() {
    const bool alreadyRequested = shutDown_;
    requestShutdown();
    if (!alreadyRequested) {
      std::unique_lock lock(workerMutex_);
      workerWake_.wait_for(lock, 20s, [this] { return shutdownComplete(); });
    }
    if (!shutdownComplete()) return false;
    if (worker_.joinable()) worker_.join();
    return true;
  }

private:
  static void publishNavigation(Session &session) {
    PdfNavigationState state;
    state.active = true;
    state.currentPage = session.currentPageIndex + 1;
    state.pageCount = session.document->pageCount();
    state.pageLabel = session.currentPageLabel;
    state.canGoPrevious = session.currentPageIndex > 0;
    state.canGoNext = session.currentPageIndex + 1 < state.pageCount;
    state.errorDetail = session.navigationErrorDetail;
    std::scoped_lock lock(session.navigationMutex);
    session.navigationState = state;
  }

  void runWorker(std::stop_token stop) {
    for (;;) {
      std::vector<std::shared_ptr<Session>> sessions;
      {
        std::unique_lock lock(workerMutex_);
        workerWake_.wait_for(lock, 10ms);
        sessions = workerSessions_;
      }
      for (const auto &session : sessions) {
        if (session->released.load(std::memory_order_acquire)) {
          if (releaseSession(*session, kShutdownStateTimeout)) {
            std::scoped_lock lock(workerMutex_);
            std::erase(workerSessions_, session);
          }
          continue;
        }
        if (!session->initializationAttempted) {
          session->initializationAttempted = true;
          QString detail;
          try {
            if (!initializeSession(session, &detail)) storeError(*session, detail);
          } catch (const std::exception &error) {
            storeError(*session, QString::fromUtf8(error.what()));
          }
          continue;
        }
        pollBus(*session);
        if (!session->inputReady.load(std::memory_order_acquire)) continue;
        int command = 0;
        quint64 generation = 0;
        {
          std::scoped_lock lock(session->navigationMutex);
          command = session->navigationCommand;
          generation = session->navigationGeneration;
        }
        if (command != 0) {
          const auto response = renderNavigation(*session, command, generation);
          publishNavigation(*session);
          std::scoped_lock lock(session->navigationMutex);
          if (generation == session->navigationGeneration) {
            session->navigationResponse = response;
            session->navigationCommand = 0;
          }
        }
        const auto last = session->lastFrameTickMs.load(std::memory_order_relaxed);
        const auto reference = last == 0 ? session->captureStartedTickMs : last;
        if (!session->suspended.load(std::memory_order_acquire) && reference != 0 &&
            monotonicMilliseconds() > reference + 5000) {
          storeError(*session, QStringLiteral(
              "PDF frame output stopped updating. Switch OFF AIR and prepare the item again."));
        }
      }
      if (stop.stop_requested()) {
        std::scoped_lock lock(workerMutex_);
        if (workerSessions_.empty()) break;
      }
    }
    workerFinished_.store(true, std::memory_order_release);
    workerWake_.notify_all();
  }

  Session *sessionAt(int deckIndex) {
    return deckIndex >= 0 && deckIndex < kDeckCount
               ? sessions_[deckIndex].get()
               : nullptr;
  }

  const Session *sessionAt(int deckIndex) const {
    return deckIndex >= 0 && deckIndex < kDeckCount
               ? sessions_[deckIndex].get()
               : nullptr;
  }

  static void storeError(Session &session, const QString &detail) {
    if (detail.isEmpty()) {
      return;
    }
    std::scoped_lock lock(session.errorMutex);
    if (detail == session.errorDetail && session.errorGeneration != 0) {
      return;
    }
    session.errorDetail = detail;
    ++session.errorGeneration;
  }

  static QImage cachedFrame(const Session &session, int pageIndex) {
    const auto found = std::find_if(
        session.pageCache.cbegin(), session.pageCache.cend(),
        [pageIndex](const CachedPage &page) {
          return page.pageIndex == pageIndex;
        });
    return found == session.pageCache.cend() ? QImage() : found->frame;
  }

  static std::vector<CachedPage>
  cacheCurrentAndAdjacent(Session &session, int pageIndex,
                          const QImage &candidate) {
    std::vector<CachedPage> cache;
    cache.reserve(3);
    const int pageCount = session.document->pageCount();
    for (int candidateIndex = pageIndex - 1;
         candidateIndex <= pageIndex + 1; ++candidateIndex) {
      if (session.released.load(std::memory_order_acquire)) break;
      if (candidateIndex < 0 || candidateIndex >= pageCount) {
        continue;
      }
      QImage frame = candidateIndex == pageIndex
                         ? candidate
                         : cachedFrame(session, candidateIndex);
      if (frame.isNull()) {
        frame = session.document->renderPage(
            candidateIndex, QSize(kCanvasWidth, kCanvasHeight));
      }
      if (frameValidationError(frame).isEmpty()) {
        cache.push_back(CachedPage{candidateIndex, frame});
      }
    }
    return cache;
  }

  PdfCommandResponse navigate(int deckIndex, int direction) {
    auto *session = sessionAt(deckIndex);
    if (!session) return {PdfCommandResult::Failed, QStringLiteral("No PDF item is ON AIR.")};
    std::scoped_lock lock(session->navigationMutex);
    if (session->navigationResponse) {
      const auto response = *session->navigationResponse;
      session->navigationResponse.reset();
      return response;
    }
    if (session->navigationCommand == 0) {
      session->navigationCommand = direction;
      workerWake_.notify_one();
    }
    return {PdfCommandResult::Busy, {}};
  }

  static PdfCommandResponse renderNavigation(Session &value, int direction, quint64 generation) {
    Session *session = &value;
    const int targetPageIndex = session->currentPageIndex + direction;
    if (targetPageIndex < 0 ||
        targetPageIndex >= session->document->pageCount()) {
      return {PdfCommandResult::Boundary, {}};
    }

    QImage candidate = cachedFrame(*session, targetPageIndex);
    if (candidate.isNull()) {
      candidate = session->document->renderPage(
          targetPageIndex, QSize(kCanvasWidth, kCanvasHeight));
    }
    QString detail = frameValidationError(candidate);
    if (!detail.isEmpty()) {
      if (!session->document->errorDetail().isEmpty()) {
        detail = session->document->errorDetail();
      }
      session->navigationErrorDetail = detail;
      return {PdfCommandResult::Failed, detail};
    }

    const QString pageLabel =
        session->document->pageLabel(targetPageIndex);

    // Cancellation invalidates a late render before it can change PROGRAM.
    {
      std::scoped_lock navigationLock(session->navigationMutex);
      if (session->released.load(std::memory_order_acquire) ||
          generation != session->navigationGeneration) return {PdfCommandResult::Failed, {}};
      std::scoped_lock frameLock(session->frameMutex);
      session->committedFrame = candidate;
      session->currentPageIndex = targetPageIndex;
      session->currentPageLabel = pageLabel;
      session->navigationErrorDetail.clear();
    }
    session->pageCache = cacheCurrentAndAdjacent(*session, targetPageIndex, candidate);
    return {PdfCommandResult::Completed, {}};
  }

  void startCapture(Session &session) {
    Session *sessionPointer = &session;
    try {
    session.captureThread =
        std::jthread([sessionPointer](std::stop_token stopToken) {
          const auto interval =
              std::chrono::milliseconds(1000 / kCaptureFramesPerSecond);
          auto nextCapture = std::chrono::steady_clock::now();
          while (!stopToken.stop_requested() &&
                 !sessionPointer->suspended.load(std::memory_order_acquire)) {
            QImage frame;
            {
              std::scoped_lock lock(sessionPointer->frameMutex);
              frame = sessionPointer->committedFrame;
            }
            const QString validationError = frameValidationError(frame);
            if (!validationError.isEmpty()) {
              storeError(*sessionPointer, validationError);
              break;
            }

            GstBuffer *buffer = gst_buffer_new_allocate(
                nullptr, static_cast<gsize>(frame.sizeInBytes()), nullptr);
            if (!buffer ||
                gst_buffer_fill(buffer, 0, frame.constBits(),
                                static_cast<gsize>(frame.sizeInBytes())) !=
                    static_cast<gsize>(frame.sizeInBytes())) {
              if (buffer) {
                gst_buffer_unref(buffer);
              }
              storeError(
                  *sessionPointer,
                  QStringLiteral("Could not allocate a PDF video frame."));
              break;
            }
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(
                GST_SECOND, 1, kCaptureFramesPerSecond);
            const GstFlowReturn flow = gst_app_src_push_buffer(
                GST_APP_SRC(sessionPointer->appSource), buffer);
            if (flow == GST_FLOW_OK) {
              sessionPointer->lastFrameTickMs.store(
                  monotonicMilliseconds(), std::memory_order_relaxed);
            } else if (flow != GST_FLOW_FLUSHING) {
              storeError(
                  *sessionPointer,
                  QStringLiteral(
                      "The PDF video input stopped accepting frames."));
              break;
            } else if (stopToken.stop_requested()) {
              break;
            }

            nextCapture += interval;
            std::this_thread::sleep_until(nextCapture);
            if (nextCapture < std::chrono::steady_clock::now() - interval) {
              nextCapture = std::chrono::steady_clock::now();
            }
          }
        });
    } catch (const std::system_error &error) {
      storeError(session, QStringLiteral("Could not start PDF capture worker: %1")
                              .arg(QString::fromLocal8Bit(error.what())));
    }
  }

  static void pollBus(Session &session) {
    if (!session.bus) {
      return;
    }
    while (GstMessage *message = gst_bus_pop(session.bus)) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        qWarning().noquote()
            << "GST_BUS" << (error ? error->message : "Unknown error")
            << (debug ? debug : "");
        const QString detail = QString::fromUtf8(
            error ? error->message : "Unknown PDF input error");
        g_clear_error(&error);
        g_free(debug);
        storeError(session, detail);
      }
      gst_message_unref(message);
    }
  }

  static bool releaseSession(Session &session, GstClockTime timeout) {
    if (session.captureThread.joinable()) {
      session.captureThread.request_stop();
      session.captureThread.join();
    }
    const bool stopped = stopPipeline(session.pipeline, timeout);
    if (!stopped) {
      qWarning() << "PDF source did not confirm NULL state:"
                 << session.filePath;
      return false;
    }
    if (session.bus) {
      gst_object_unref(session.bus);
      session.bus = nullptr;
    }
    if (session.appSink) {
      gst_object_unref(session.appSink);
      session.appSink = nullptr;
    }
    if (session.appSource) {
      gst_object_unref(session.appSource);
      session.appSource = nullptr;
    }
    if (session.pipeline) {
      gst_object_unref(session.pipeline);
      session.pipeline = nullptr;
    }
    session.pageCache.clear();
    {
      std::scoped_lock lock(session.frameMutex);
      session.committedFrame = {};
    }
    session.document.reset();
    gst_clear_context(&session.context);
    return stopped;
  }

  int outputWidth_ = kCanvasWidth;
  int outputHeight_ = kCanvasHeight;
  GstContext *programContext_ = nullptr;
  std::array<std::shared_ptr<Session>, kDeckCount> sessions_;
  std::mutex workerMutex_;
  std::condition_variable workerWake_;
  std::vector<std::shared_ptr<Session>> workerSessions_;
  std::atomic_bool workerFinished_{false};
  bool shutDown_ = false;
  std::jthread worker_;
};

PdfSourceSessionManager::PdfSourceSessionManager(int outputWidth,
                                                 int outputHeight)
    : impl_(std::make_unique<Impl>(outputWidth, outputHeight)) {}

PdfSourceSessionManager::~PdfSourceSessionManager() {
  if (!impl_->shutdown()) {
    qWarning() << "Retaining PDF worker after unconfirmed shutdown";
    (void)impl_.release();
  }
}

void PdfSourceSessionManager::setProgramContext(GstContext *context) {
  impl_->setProgramContext(context);
}

bool PdfSourceSessionManager::bindProgramDeck(
    int deckIndex, const QString &filePath, ProgramSource &source,
    QString *errorDetail) {
  return impl_->bindProgramDeck(deckIndex, filePath, source, errorDetail);
}

bool PdfSourceSessionManager::releaseProgramDeck(int deckIndex) {
  return impl_->releaseProgramDeck(deckIndex);
}

void PdfSourceSessionManager::suspendProgramDeck(int deckIndex) {
  impl_->suspendProgramDeck(deckIndex);
}

bool PdfSourceSessionManager::tryReleaseProgramDeck(int deckIndex) {
  return impl_->tryReleaseProgramDeck(deckIndex);
}

bool PdfSourceSessionManager::sourceIsPlaying(int deckIndex) const {
  return impl_->sourceIsPlaying(deckIndex);
}

bool PdfSourceSessionManager::sourceErrorAfter(
    int deckIndex, quint64 previousGeneration, quint64 &currentGeneration,
    QString &errorDetail) {
  return impl_->sourceErrorAfter(deckIndex, previousGeneration,
                                 currentGeneration, errorDetail);
}

PdfNavigationState
PdfSourceSessionManager::navigationState(int deckIndex) {
  return impl_->navigationState(deckIndex);
}

PdfCommandResponse PdfSourceSessionManager::next(int deckIndex) {
  return impl_->next(deckIndex);
}

PdfCommandResponse PdfSourceSessionManager::previous(int deckIndex) {
  return impl_->previous(deckIndex);
}

PdfSourceSessionManager::ProgramSource PdfSourceSessionManager::programSource(int deckIndex) const {
  return impl_->programSource(deckIndex);
}
void PdfSourceSessionManager::requestShutdown() { impl_->requestShutdown(); }
bool PdfSourceSessionManager::shutdownComplete() const { return impl_->shutdownComplete(); }
void PdfSourceSessionManager::cancelNavigation(int deckIndex) {
  impl_->cancelNavigation(deckIndex);
}

bool PdfSourceSessionManager::shutdown() { return impl_->shutdown(); }

QString validatePdfSource(const QString &filePath) {
  PdfDocumentBackend document;
  return document.load(filePath) ? QString() : document.errorDetail();
}

QImage generatePdfThumbnail(const QString &filePath,
                            const QSize &targetSize,
                            QString *errorDetail) {
  if (errorDetail) {
    errorDetail->clear();
  }
  if (targetSize.width() <= 0 || targetSize.height() <= 0) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("The PDF thumbnail size is invalid.");
    }
    return {};
  }

  PdfDocumentBackend document;
  if (!document.load(filePath)) {
    if (errorDetail) {
      *errorDetail = document.errorDetail();
    }
    return {};
  }
  QImage thumbnail = document.renderPage(0, targetSize);
  if (thumbnail.isNull() && errorDetail) {
    *errorDetail = document.errorDetail().isEmpty()
                       ? QStringLiteral(
                             "The first PDF page could not be rendered.")
                       : document.errorDetail();
  }
  return thumbnail;
}
