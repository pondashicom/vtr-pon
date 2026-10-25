#include "media_import_queue.h"
#include "playlist_model.h"
#include <QFileInfo>
#include <QTimer>
#include <atomic>
#include <thread>
#include <utility>

struct PendingMediaImport {
  quint64 itemId = 0;
  QString path;
  std::optional<qint64> imageDurationMs;
  MediaDurationProbeResult result;
  std::atomic_bool cancelled{false};
  std::atomic_bool finished{false};
};

MediaImportQueue::MediaImportQueue(PlaylistModel *model, QObject *parent,
                                 Probe probe)
    : QObject(parent), model_(model), probe_(std::move(probe)),
      timer_(new QTimer(this)) {
  timer_->setInterval(25);
  connect(timer_, &QTimer::timeout, this, &MediaImportQueue::poll);
  connect(model, &PlaylistModel::itemRemoved, this, [this](quint64 id) {
    if (active_ && active_->itemId == id)
      active_->cancelled.store(true, std::memory_order_relaxed);
    for (auto &job : queued_) {
      if (job->itemId == id)
        job->cancelled.store(true, std::memory_order_relaxed);
    }
  });
  connect(model, &PlaylistModel::modelReset, this, [this] {
    // A restored model may reuse IDs. Never apply results to that generation.
    if (active_) active_->cancelled.store(true, std::memory_order_relaxed);
    queued_.clear();
    failures_.clear();
  });
}

MediaImportQueue::~MediaImportQueue() {
  cancelAll();
  // No join: filesystem/discoverer calls may be blocked. The worker owns all
  // of its values and never calls a QObject, even when finishing after close.
}

quint64 MediaImportQueue::enqueue(const QString &path, int playlistNumber,
                                 std::optional<qint64> imageDurationMs) {
  if (shuttingDown_ || !model_ || (imageDurationMs && *imageDurationMs <= 0)) return 0;
  const quint64 id = model_->addPendingMediaItem(path, playlistNumber);
  if (!id) return 0;
  const auto item = model_->itemState(id);
  if (!item) return 0;
  auto job = std::make_shared<PendingMediaImport>();
  job->itemId = id;
  job->path = item->filePath;
  job->imageDurationMs = imageDurationMs;
  queued_.enqueue(std::move(job));
  timer_->start();
  return id;
}

bool MediaImportQueue::hasPendingImports() const {
  if (!model_) return false;
  const auto pending = [this](const auto &job) {
    return !job->cancelled.load(std::memory_order_relaxed) &&
           model_->itemConversionInProgress(job->itemId);
  };
  if (active_ && pending(active_)) return true;
  for (const auto &job : queued_) if (pending(job)) return true;
  return false;
}

void MediaImportQueue::cancelAll() {
  QList<quint64> ids;
  if (active_) {
    if (!active_->cancelled.exchange(true)) ids.append(active_->itemId);
  }
  for (auto &job : queued_) {
    if (!job->cancelled.exchange(true)) ids.append(job->itemId);
  }
  queued_.clear();
  failures_.clear();
  if (model_) for (quint64 id : ids) model_->removeItem(id);
}

void MediaImportQueue::poll() {
  if (!model_) { timer_->stop(); return; }
  if (active_) {
    if (!active_->finished.load(std::memory_order_acquire)) return;
    const auto job = std::exchange(active_, {});
    if (!job->cancelled.load(std::memory_order_relaxed) &&
        model_->itemConversionInProgress(job->itemId)) {
      const auto &result = job->result;
      if (!result.succeeded || !model_->completePendingMediaItem(
              job->itemId, result.mediaType, result.durationMs)) {
        model_->removeItem(job->itemId);
        failures_.append(QStringLiteral("%1: %2")
            .arg(QFileInfo(job->path).fileName(), result.errorDetail.isEmpty()
                ? QStringLiteral("The media item could not be registered.")
                : result.errorDetail));
      }
    }
  }
  while (!queued_.isEmpty()) {
    auto job = queued_.dequeue();
    if (job->cancelled.load(std::memory_order_relaxed) ||
        !model_->itemConversionInProgress(job->itemId)) continue;
    active_ = job;
    model_->setItemConversionProgress(job->itemId,
                                      PlaylistItemConversionStage::Analyzing, 0);
    try {
      std::thread([job, probe = probe_] {
        try {
          const QFileInfo file(job->path);
          if (!file.exists() || !file.isFile()) {
            job->result.errorDetail = QStringLiteral("The media file does not exist.");
          } else if (!job->cancelled.load(std::memory_order_relaxed)) {
            job->result = probe(job->path);
            if (job->result.succeeded && job->imageDurationMs) {
              if (job->result.mediaType != PlaylistMediaType::StillImage) {
                job->result.succeeded = false;
                job->result.errorDetail = QStringLiteral(
                    "The selected PNG/JPEG file is not a playable still image.");
              } else {
                job->result.durationMs = job->imageDurationMs;
              }
            }
          }
        } catch (...) {
          job->result.succeeded = false;
          job->result.errorDetail = QStringLiteral("Media analysis failed unexpectedly.");
        }
        job->finished.store(true, std::memory_order_release);
      }).detach();
    } catch (...) {
      job->result.errorDetail = QStringLiteral("The media analysis worker could not start.");
      job->finished.store(true, std::memory_order_release);
    }
    return;
  }
  timer_->stop();
  if (!failures_.isEmpty()) emit importsFailed(std::exchange(failures_, {}));
}

void MediaImportQueue::requestShutdown() {
  shuttingDown_ = true;
  cancelAll();
}
bool MediaImportQueue::shutdownComplete() const {
  return !active_ || active_->finished.load(std::memory_order_acquire);
}
