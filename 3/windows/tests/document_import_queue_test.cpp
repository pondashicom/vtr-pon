#include "background_io_queue.h"
#include "document_import_queue.h"
#include "document_import_test_hooks.h"
#include "file_management_settings.h"
#include "pdf_import_service.h"
#include "pdf_windows_test_support.h"
#include "playlist_model.h"
#include "user_info.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPdfWriter>
#include <QRect>
#include <QTemporaryDir>
#include <QThread>
#include <QSemaphore>
#include <QTimer>

#include <atomic>
#include <algorithm>
#include <iostream>
#include <memory>

namespace {
bool checkDocumentQueue(bool condition, const char *message, int line) {
  if (!condition)
    std::cerr << "Document import queue failure at line " << line << ": "
              << message << '\n';
  return condition;
}

#define CHECK_DOCUMENT_QUEUE(condition, message) \
  do { \
    if (!checkDocumentQueue(condition, message, __LINE__)) return false; \
  } while (false)

template <typename Predicate>
bool awaitDocumentQueue(Predicate completed, int timeoutMs = 5000) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (completed()) return true;
    QThread::msleep(1);
  } while (timer.elapsed() < timeoutMs);
  return false;
}

bool writeQueuePdf(const QString &path) {
  {
    QPdfWriter writer(path);
    writer.setResolution(72);
    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    painter.fillRect(QRect(0, 0, writer.width(), writer.height()), Qt::white);
    painter.drawText(QRect(20, 20, 300, 100), Qt::AlignLeft,
                     QStringLiteral("Document queue regression"));
    if (!painter.end()) return false;
  }
  return QFileInfo(path).isFile() && QFileInfo(path).size() > 0;
}

struct PdfProgressGate {
  QSemaphore reached;
  QSemaphore resume;
  std::atomic_bool timedOut = false;
  std::atomic_bool seen = false;
};

struct DocumentQueueFixture {
  explicit DocumentQueueFixture(ProgramPlayoutController *program)
      : guiThread(QCoreApplication::instance()->thread()),
        queue(std::make_unique<DocumentImportQueue>(&model, program, info.get(), &fileIo)) {
    const auto recordMutation = [this] {
      if (QThread::currentThread() != guiThread) wrongThread.store(true);
    };
    QObject::connect(&model, &PlaylistModel::itemAdded, &model,
                     recordMutation, Qt::DirectConnection);
    QObject::connect(&model, &PlaylistModel::itemRemoved, &model,
                     recordMutation, Qt::DirectConnection);
    QObject::connect(&model, &PlaylistModel::itemChanged, &model,
                     recordMutation, Qt::DirectConnection);
    QObject::connect(&model, &PlaylistModel::itemConversionStateChanged,
                     &model, recordMutation, Qt::DirectConnection);
    QObject::connect(&model, &PlaylistModel::itemReplaced, &model,
                     [this] { ++replacements; }, Qt::DirectConnection);
  }

  ~DocumentQueueFixture() {
    // Also drain on an assertion failure; the queue dies before its dependencies.
    if (gate) gate->resume.release();
    if (backgroundGate) backgroundGate->resume.release();
    if (queue) {
      queue->requestShutdown();
      (void)awaitDocumentQueue([this] { return queue->shutdownComplete(); }, 15000);
      queue.reset();
    }
    vtrpon::tests::pdfImportProgress = {};
    fileIo.requestShutdown();
    (void)awaitDocumentQueue([this] { return fileIo.shutdownComplete(); }, 15000);
  }

  void gateFirstPage() {
    gate = std::make_shared<PdfProgressGate>();
    vtrpon::tests::pdfImportProgress = [state = gate](PdfImportPhase phase, int) {
      if (phase != PdfImportPhase::Rendering || state->seen.exchange(true)) return;
      state->reached.release();
      if (!state->resume.tryAcquire(1, 10000)) state->timedOut = true;
    };
  }

  QThread *guiThread;
  std::atomic_bool wrongThread = false;
  std::atomic_int replacements = 0;
  PlaylistModel model;
  std::shared_ptr<PdfProgressGate> gate;
  std::shared_ptr<PdfProgressGate> backgroundGate;
  std::unique_ptr<UserInfoCenter> info = std::make_unique<UserInfoCenter>();
  BackgroundIoQueue fileIo;
  std::unique_ptr<DocumentImportQueue> queue;
};

bool queuedCancellation(ProgramPlayoutController *program, const QString &pdf) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, {}, 1, &error),
                       "native PDF can be queued before starting a batch");
  const quint64 id = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(id && fixture.queue->contains(id) &&
                           fixture.model.itemConversionInProgress(id),
                       "queued document owns a pending placeholder");
  fixture.queue->cancel(id, true);
  fixture.queue->startNext();
  CHECK_DOCUMENT_QUEUE(!fixture.queue->hasPendingImports() &&
                           !fixture.queue->contains(id) &&
                           !fixture.model.itemState(id) &&
                           fixture.queue->shutdownComplete(),
                       "cancelling before start removes both job and placeholder");
  QCoreApplication::processEvents();
  CHECK_DOCUMENT_QUEUE(fixture.replacements == 0 && !fixture.wrongThread,
                       "cancelled queued document never completes in the model");
  return true;
}

bool nativeCompletion(ProgramPlayoutController *program, const QString &pdf) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, {}, 1, &error),
                       "first native PDF is accepted");
  const quint64 first = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, {}, 1, &error),
                       "second native PDF is accepted");
  const quint64 second = fixture.model.itemIdAt(1);
  CHECK_DOCUMENT_QUEUE(first && second && first != second,
                       "batch placeholders have distinct stable identities");
  CHECK_DOCUMENT_QUEUE(fixture.model.setItemDisplayName(first, "renamed while pending"),
                       "pending native document can retain an edited label");
  const int otherPlaylist = fixture.model.createPlaylist();
  CHECK_DOCUMENT_QUEUE(fixture.model.setActivePlaylistNumber(otherPlaylist),
                       "operator can change playlists while imports are pending");
  fixture.queue->startNext();
  fixture.queue->startNext();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return !fixture.queue->hasPendingImports();
                       }),
                       "both native PDF jobs finish through the GUI event loop");
  const auto firstState = fixture.model.itemState(first);
  const auto secondState = fixture.model.itemState(second);
  CHECK_DOCUMENT_QUEUE(firstState && secondState &&
                           firstState->sourceType == PlaylistSourceType::Pdf &&
                           firstState->playlistNumber == 1 &&
                           secondState->playlistNumber == 1 &&
                           firstState->displayName == "renamed while pending" &&
                           fixture.model.itemIdAt(1, 0) == first &&
                           fixture.model.itemIdAt(1, 1) == second,
                       "native completion preserves IDs, order, target playlist and edited label");
  CHECK_DOCUMENT_QUEUE(!fixture.model.itemConversionInProgress(first) &&
                           !fixture.model.itemConversionInProgress(second) &&
                           fixture.replacements == 2 && !fixture.wrongThread,
                       "each native job commits exactly once on the GUI thread");
  CHECK_DOCUMENT_QUEUE(fixture.info->severity() == UserInfoSeverity::Success,
                       "successful completion reports success");
  return true;
}

bool failedCompletion(ProgramPlayoutController *program, const QString &corrupt,
                      const QString &missing) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(corrupt, {}, 1, &error),
                       "corrupt PDF is accepted for background validation");
  const quint64 first = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(missing, {}, 1, &error),
                       "missing PDF is accepted for background validation");
  const quint64 second = fixture.model.itemIdAt(1);
  fixture.queue->startNext();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return !fixture.queue->hasPendingImports();
                       }),
                       "validation failure advances to the next queued document");
  CHECK_DOCUMENT_QUEUE(!fixture.model.itemState(first) &&
                           !fixture.model.itemState(second) &&
                           fixture.model.itemCount() == 0 &&
                           fixture.replacements == 0 && !fixture.wrongThread,
                       "failed jobs remove placeholders without committing replacements");
  CHECK_DOCUMENT_QUEUE(fixture.info->severity() == UserInfoSeverity::Warning &&
                           !fixture.info->detail().isEmpty(),
                       "failed document validation reports a diagnostic");
  return true;
}

bool activeShutdown(ProgramPlayoutController *program, const QString &pdf) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, {}, 1, &error),
                       "shutdown fixture accepts the active document");
  const quint64 active = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, {}, 1, &error),
                       "shutdown fixture accepts the queued document");
  const quint64 queued = fixture.model.itemIdAt(1);
  fixture.queue->startNext();
  // Do not pump events here: even a fast worker still needs GUI finalization.
  CHECK_DOCUMENT_QUEUE(!fixture.queue->shutdownComplete(),
                       "worker startup has not completed GUI finalization");
  fixture.queue->requestShutdown();
  fixture.queue->requestShutdown();
  fixture.queue->startNext();
  CHECK_DOCUMENT_QUEUE(fixture.queue->contains(active) &&
                           !fixture.queue->contains(queued) &&
                           !fixture.queue->shutdownComplete() &&
                           fixture.model.itemCount() == 0,
                       "shutdown cancels queued work and awaits active finalization");
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return fixture.queue->shutdownComplete();
                       }),
                       "cancelled active document reaches complete shutdown");
  CHECK_DOCUMENT_QUEUE(!fixture.queue->hasPendingImports() &&
                           !fixture.model.itemState(active) &&
                           !fixture.model.itemState(queued) &&
                           fixture.replacements == 0 && !fixture.wrongThread,
                       "late worker completion cannot restore cancelled placeholders");
  fixture.fileIo.requestShutdown();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return fixture.fileIo.shutdownComplete();
                       }),
                       "file cleanup drains after document finalization");
  return true;
}

enum class CleanupCase { Removed, Retained, ClosingIo, Normal, QueueGone, InfoGone };

bool cleanupCompletion(ProgramPlayoutController *program, const QString &pdf,
                       CleanupCase scenario) {
  QTemporaryDir outputs;
  CHECK_DOCUMENT_QUEUE(outputs.isValid(), "cleanup fixture output root exists");
  auto settings = vtrpon::makeControllerSettings();
  const QString key = QStringLiteral("files/documentConversionDirectory");
  const QVariant previous = settings.value(key);
  struct RestoreSetting {
    QString key;
    QVariant value;
    ~RestoreSetting() {
      auto saved = vtrpon::makeControllerSettings();
      if (value.isValid()) saved.setValue(key, value);
      else saved.remove(key);
    }
  } restore{key, previous};
  settings.setValue(key, outputs.path());
  const QString root = outputs.filePath("PDF");
  const QString sentinel = QDir(root).filePath("existing/keep.txt");
  CHECK_DOCUMENT_QUEUE(QDir().mkpath(QFileInfo(sentinel).absolutePath()), "sentinel directory created");
  QFile keep(sentinel);
  CHECK_DOCUMENT_QUEUE(keep.open(QIODevice::WriteOnly) && keep.write("keep") == 4,
                       "unrelated output created");
  keep.close();
  const auto sourceHash = pdf_test::sha256(pdf);
  const auto keepHash = pdf_test::sha256(sentinel);

  // Keep the sharing violation alive through service and background retries,
  // including early assertion exits while the fixture drains.
  pdf_test::DeleteDeny held;
  DocumentQueueFixture fixture(program);
  fixture.gateFirstPage();
  if (scenario == CleanupCase::QueueGone || scenario == CleanupCase::InfoGone) {
    fixture.backgroundGate = std::make_shared<PdfProgressGate>();
    CHECK_DOCUMENT_QUEUE(fixture.fileIo.enqueue([gate = fixture.backgroundGate] {
      gate->reached.release();
      if (!gate->resume.tryAcquire(1, 15000)) gate->timedOut = true;
    }), "background gate precedes the cleanup task");
    CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] { return fixture.backgroundGate->reached.tryAcquire(); }),
                         "background gate is active before import starts");
  }
  QStringList warnings;
  bool wrongNotificationThread = false;
  int heartbeat = 0;
  QObject observer; // Disconnect before the captured local state is destroyed.
  QObject::connect(fixture.info.get(), &UserInfoCenter::messageChanged, &observer,
      [&](UserInfoSeverity severity, const QString &source, const QString &, const QString &detail) {
        if (QThread::currentThread() != fixture.guiThread) wrongNotificationThread = true;
        if (severity == UserInfoSeverity::Warning && source == QStringLiteral("PLAYLIST"))
          warnings.append(detail);
      }, Qt::DirectConnection);
  QTimer pulse;
  pulse.setInterval(5);
  QObject::connect(&pulse, &QTimer::timeout, &observer, [&] { ++heartbeat; });
  pulse.start();
  PdfAddSettings png;
  png.method = PdfAddMethod::PngSequence;
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, png, 1, &error), "cleanup PDF queued");
  const auto id = fixture.model.itemIdAt(0);
  fixture.queue->startNext();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] { return fixture.gate->reached.tryAcquire(); }),
                       "first rendered page reached the synchronization gate");
  const QString page = pdf_test::firstPng(root);
  CHECK_DOCUMENT_QUEUE(!page.isEmpty(), "first PNG exists before cancellation");
  const QString residual = QFileInfo(page).absolutePath();
  const bool normal = scenario == CleanupCase::Normal;
  CHECK_DOCUMENT_QUEUE(normal || held.hold(page), "DeleteFileW confirms the sharing violation");
  if (scenario == CleanupCase::ClosingIo) fixture.fileIo.requestShutdown();
  fixture.queue->cancel(id, scenario != CleanupCase::Retained);
  CHECK_DOCUMENT_QUEUE(fixture.model.itemState(id).has_value() == (scenario == CleanupCase::Retained),
                       "placeholder removal matches the cancellation request");
  const int heartbeatBefore = heartbeat;
  fixture.gate->resume.release();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] { return fixture.queue->shutdownComplete(); }, 15000),
                       "cancelled service finalized without blocking GUI");
  CHECK_DOCUMENT_QUEUE(!fixture.gate->timedOut && !fixture.model.itemState(id) && fixture.replacements == 0,
                       "cancelled placeholder cannot return after finalization");
  if (fixture.backgroundGate) {
    CHECK_DOCUMENT_QUEUE(!fixture.backgroundGate->timedOut && !fixture.fileIo.shutdownComplete() &&
                             warnings.isEmpty(), "cleanup has not completed before target destruction");
    fixture.queue.reset();
  }
  if (scenario == CleanupCase::InfoGone) fixture.info.reset();
  if (fixture.backgroundGate) fixture.backgroundGate->resume.release();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] { return fixture.fileIo.shutdownComplete(); }, 15000),
                       "background cleanup completed while handle is held");
  const bool expectWarning = !normal && scenario != CleanupCase::InfoGone;
  const QString nativeResidual = QDir::toNativeSeparators(residual);
  const bool foundPath = std::any_of(warnings.cbegin(), warnings.cend(),
      [&](const QString &detail) { return detail.contains(nativeResidual); });
  std::cout << "Cleanup case=" << static_cast<int>(scenario)
            << " residual=" << residual.toStdString()
            << " warning=" << foundPath << " heartbeat=" << heartbeat - heartbeatBefore << '\n';
  CHECK_DOCUMENT_QUEUE(foundPath == expectWarning && (expectWarning || warnings.isEmpty()),
                       "residual warning follows deletion result and target lifetime");
  CHECK_DOCUMENT_QUEUE(!wrongNotificationThread && !fixture.wrongThread &&
                           (!fixture.backgroundGate || !fixture.backgroundGate->timedOut) &&
                           (normal || heartbeat - heartbeatBefore >= 10),
                       "notifications and model stay on GUI thread while heartbeat continues");
  CHECK_DOCUMENT_QUEUE(QFileInfo::exists(residual) == !normal,
                       "only failed cleanup retains output");
  held.release();
  CHECK_DOCUMENT_QUEUE(normal || PdfImportService::discardGeneratedArtifacts(residual, root),
                       "residual is removable after CloseHandle");
  CHECK_DOCUMENT_QUEUE(pdf_test::sha256(pdf) == sourceHash && pdf_test::sha256(sentinel) == keepHash,
                       "source and unrelated output hashes remain unchanged");
  if (fixture.queue && scenario != CleanupCase::ClosingIo) {
    CHECK_DOCUMENT_QUEUE(fixture.queue->enqueuePdf(pdf, png, 1, &error), "same PDF can be retried");
    fixture.queue->startNext();
    CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] { return fixture.queue->shutdownComplete(); }),
                         "retry finishes");
    CHECK_DOCUMENT_QUEUE(fixture.model.itemCount() == 1 && fixture.replacements == 1,
                         "retry registers exactly one page");
  }
  CHECK_DOCUMENT_QUEUE(pdf_test::sha256(pdf) == sourceHash && pdf_test::sha256(sentinel) == keepHash,
                       "retry preserves source and unrelated output");
  return true;
}
} // namespace

bool runDocumentImportQueueRegression(ProgramPlayoutController *program) {
  QTemporaryDir directory;
  CHECK_DOCUMENT_QUEUE(directory.isValid(), "temporary PDF directory exists");
  const QString pdf = directory.filePath(QStringLiteral("queue.pdf"));
  CHECK_DOCUMENT_QUEUE(writeQueuePdf(pdf), "small real PDF fixture was written");
  const QString corrupt = directory.filePath(QStringLiteral("corrupt.pdf"));
  QFile corruptFile(corrupt);
  CHECK_DOCUMENT_QUEUE(corruptFile.open(QIODevice::WriteOnly),
                       "corrupt PDF fixture can be created");
  const QByteArray invalidPdf("This is not a PDF document.\n");
  CHECK_DOCUMENT_QUEUE(corruptFile.write(invalidPdf) == invalidPdf.size(),
                       "corrupt PDF fixture was written completely");
  corruptFile.close();
  bool passed = queuedCancellation(program, pdf) &&
      nativeCompletion(program, pdf) &&
      failedCompletion(program, corrupt,
                       directory.filePath(QStringLiteral("missing.pdf"))) &&
      activeShutdown(program, pdf);
  for (const auto scenario : {CleanupCase::Removed, CleanupCase::Retained, CleanupCase::ClosingIo,
                              CleanupCase::Normal, CleanupCase::QueueGone, CleanupCase::InfoGone})
    passed = cleanupCompletion(program, pdf, scenario) && passed;
  if (passed) std::cout << "Document import queue regression: PASS\n";
  return passed;
}

#undef CHECK_DOCUMENT_QUEUE
