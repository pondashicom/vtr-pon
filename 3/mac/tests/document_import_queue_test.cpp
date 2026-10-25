#include "background_io_queue.h"
#include "document_import_queue.h"
#include "pdf_import_service.h"
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

#include <atomic>
#include <iostream>

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

struct DocumentQueueFixture {
  explicit DocumentQueueFixture(ProgramPlayoutController *program)
      : guiThread(QCoreApplication::instance()->thread()),
        queue(&model, program, &info, &fileIo) {
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
    queue.requestShutdown();
    (void)awaitDocumentQueue([this] { return queue.shutdownComplete(); });
    fileIo.requestShutdown();
    (void)awaitDocumentQueue([this] { return fileIo.shutdownComplete(); });
  }

  QThread *guiThread;
  std::atomic_bool wrongThread = false;
  std::atomic_int replacements = 0;
  PlaylistModel model;
  UserInfoCenter info;
  BackgroundIoQueue fileIo;
  DocumentImportQueue queue;
};

bool queuedCancellation(ProgramPlayoutController *program, const QString &pdf) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(pdf, {}, 1, &error),
                       "native PDF can be queued before starting a batch");
  const quint64 id = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(id && fixture.queue.contains(id) &&
                           fixture.model.itemConversionInProgress(id),
                       "queued document owns a pending placeholder");
  fixture.queue.cancel(id, true);
  fixture.queue.startNext();
  CHECK_DOCUMENT_QUEUE(!fixture.queue.hasPendingImports() &&
                           !fixture.queue.contains(id) &&
                           !fixture.model.itemState(id) &&
                           fixture.queue.shutdownComplete(),
                       "cancelling before start removes both job and placeholder");
  QCoreApplication::processEvents();
  CHECK_DOCUMENT_QUEUE(fixture.replacements == 0 && !fixture.wrongThread,
                       "cancelled queued document never completes in the model");
  return true;
}

bool nativeCompletion(ProgramPlayoutController *program, const QString &pdf) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(pdf, {}, 1, &error),
                       "first native PDF is accepted");
  const quint64 first = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(pdf, {}, 1, &error),
                       "second native PDF is accepted");
  const quint64 second = fixture.model.itemIdAt(1);
  CHECK_DOCUMENT_QUEUE(first && second && first != second,
                       "batch placeholders have distinct stable identities");
  CHECK_DOCUMENT_QUEUE(fixture.model.setItemDisplayName(first, "renamed while pending"),
                       "pending native document can retain an edited label");
  const int otherPlaylist = fixture.model.createPlaylist();
  CHECK_DOCUMENT_QUEUE(fixture.model.setActivePlaylistNumber(otherPlaylist),
                       "operator can change playlists while imports are pending");
  fixture.queue.startNext();
  fixture.queue.startNext();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return !fixture.queue.hasPendingImports();
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
  CHECK_DOCUMENT_QUEUE(fixture.info.severity() == UserInfoSeverity::Success,
                       "successful completion reports success");
  return true;
}

bool failedCompletion(ProgramPlayoutController *program, const QString &corrupt,
                      const QString &missing) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(corrupt, {}, 1, &error),
                       "corrupt PDF is accepted for background validation");
  const quint64 first = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(missing, {}, 1, &error),
                       "missing PDF is accepted for background validation");
  const quint64 second = fixture.model.itemIdAt(1);
  fixture.queue.startNext();
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return !fixture.queue.hasPendingImports();
                       }),
                       "validation failure advances to the next queued document");
  CHECK_DOCUMENT_QUEUE(!fixture.model.itemState(first) &&
                           !fixture.model.itemState(second) &&
                           fixture.model.itemCount() == 0 &&
                           fixture.replacements == 0 && !fixture.wrongThread,
                       "failed jobs remove placeholders without committing replacements");
  CHECK_DOCUMENT_QUEUE(fixture.info.severity() == UserInfoSeverity::Warning &&
                           !fixture.info.detail().isEmpty(),
                       "failed document validation reports a diagnostic");
  return true;
}

bool activeShutdown(ProgramPlayoutController *program, const QString &pdf) {
  DocumentQueueFixture fixture(program);
  QString error;
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(pdf, {}, 1, &error),
                       "shutdown fixture accepts the active document");
  const quint64 active = fixture.model.itemIdAt(0);
  CHECK_DOCUMENT_QUEUE(fixture.queue.enqueuePdf(pdf, {}, 1, &error),
                       "shutdown fixture accepts the queued document");
  const quint64 queued = fixture.model.itemIdAt(1);
  fixture.queue.startNext();
  // Do not pump events here: even a fast worker still needs GUI finalization.
  CHECK_DOCUMENT_QUEUE(!fixture.queue.shutdownComplete(),
                       "worker startup has not completed GUI finalization");
  fixture.queue.requestShutdown();
  fixture.queue.requestShutdown();
  fixture.queue.startNext();
  CHECK_DOCUMENT_QUEUE(fixture.queue.contains(active) &&
                           !fixture.queue.contains(queued) &&
                           !fixture.queue.shutdownComplete() &&
                           fixture.model.itemCount() == 0,
                       "shutdown cancels queued work and awaits active finalization");
  CHECK_DOCUMENT_QUEUE(awaitDocumentQueue([&] {
                         return fixture.queue.shutdownComplete();
                       }),
                       "cancelled active document reaches complete shutdown");
  CHECK_DOCUMENT_QUEUE(!fixture.queue.hasPendingImports() &&
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
  const bool passed = queuedCancellation(program, pdf) &&
      nativeCompletion(program, pdf) &&
      failedCompletion(program, corrupt,
                       directory.filePath(QStringLiteral("missing.pdf"))) &&
      activeShutdown(program, pdf);
  if (passed) std::cout << "Document import queue regression: PASS\n";
  return passed;
}

#undef CHECK_DOCUMENT_QUEUE
