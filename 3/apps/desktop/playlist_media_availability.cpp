#include "playlist_media_availability.h"

#include <QDebug>
#include <QTimer>

#include <atomic>
#include <exception>
#include <thread>
#include <utility>

namespace {
constexpr int kMediaAvailabilityCheckIntervalMs = 5000;
}

// Worker-owned values only. No QObject or model pointer crosses the thread
// boundary, so a blocked filesystem call cannot delay model destruction.
struct MediaAvailabilityScan {
  struct Item { quint64 id; QString filePath; };
  quint64 generation = 0;
  QList<Item> items;
  QSet<quint64> offlineItemIds;
  std::atomic_bool cancelled{false};
  std::atomic_bool finished{false};
  bool succeeded = false;
};

PlaylistMediaAvailability::PlaylistMediaAvailability(
    PlaylistModel *model, PlaylistModel::MediaAvailabilityProbe probe)
    : model_(model), probe_(std::move(probe)) {
  auto *timer = new QTimer(model);
  timer->setInterval(kMediaAvailabilityCheckIntervalMs);
  timer->setTimerType(Qt::CoarseTimer);
  QObject::connect(timer, &QTimer::timeout, model,
                   [this] { refresh(); });
  timer->start();

  resultTimer_ = new QTimer(model);
  resultTimer_->setInterval(25);
  QObject::connect(resultTimer_, &QTimer::timeout, model,
                   [this] { finishScan(); });
  // Restore/Undo may reuse IDs. Reject old worker results even when the
  // replacement path happens to be identical.
  const auto invalidate = [this] { invalidateScan(); };
  QObject::connect(model, &PlaylistModel::itemAdded, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemRemoved, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemReplaced, model, invalidate);
  QObject::connect(model, &PlaylistModel::modelReset, model, invalidate);
}

PlaylistMediaAvailability::~PlaylistMediaAvailability() {
  if (scan_) scan_->cancelled.store(true, std::memory_order_relaxed);
}

bool PlaylistMediaAvailability::isOffline(quint64 itemId) const {
  return offlineItemIds_.contains(itemId);
}

void PlaylistMediaAvailability::setOffline(quint64 itemId, bool offline) {
  if (offline) offlineItemIds_.insert(itemId);
  else offlineItemIds_.remove(itemId);
}

void PlaylistMediaAvailability::replaceOffline(QSet<quint64> offlineItemIds) {
  offlineItemIds_ = std::move(offlineItemIds);
}

void PlaylistMediaAvailability::invalidateScan() {
  ++generation_;
  if (scan_) {
    scan_->cancelled.store(true, std::memory_order_relaxed);
    refreshRequested_ = true;
  }
}

void PlaylistMediaAvailability::requestShutdown() {
  shuttingDown_ = true;
  refreshRequested_ = false;
  if (scan_) scan_->cancelled.store(true);
}

bool PlaylistMediaAvailability::shutdownComplete() const {
  return !scan_ || scan_->finished.load(std::memory_order_acquire);
}

void PlaylistMediaAvailability::refresh() {
  if (shuttingDown_) return;
  emit model_->mediaAvailabilityCheckRequested();
  if (scan_) {
    // At most one pending refresh, irrespective of timer/End Mode requests.
    refreshRequested_ = true;
    return;
  }
  auto scan = std::make_shared<MediaAvailabilityScan>();
  scan->generation = generation_;
  for (const auto &item : std::as_const(model_->itemsById_)) {
    if (item.sourceType != PlaylistSourceType::Uvc &&
        !model_->itemConversionInProgress(item.id)) {
      scan->items.append({item.id, item.filePath});
    }
  }
  if (scan->items.isEmpty()) return;
  refreshRequested_ = false;
  scan_ = scan;
  try {
    std::thread([scan, probe = probe_] {
      try {
        QHash<QString, bool> availabilityByPath;
        for (const auto &item : std::as_const(scan->items)) {
          if (scan->cancelled.load(std::memory_order_relaxed)) break;
          auto found = availabilityByPath.constFind(item.filePath);
          bool available;
          if (found == availabilityByPath.cend()) {
            available = probe(item.filePath);
            availabilityByPath.insert(item.filePath, available);
          } else {
            available = found.value();
          }
          if (!available) scan->offlineItemIds.insert(item.id);
        }
        scan->succeeded = !scan->cancelled.load(std::memory_order_relaxed);
      } catch (...) {
        // Keep the last known state if the probe itself cannot complete.
        scan->succeeded = false;
      }
      scan->finished.store(true, std::memory_order_release);
    }).detach();
    resultTimer_->start();
  } catch (const std::exception &error) {
    scan_.reset();
    qWarning() << "Could not start media availability scan:" << error.what();
  }
}

void PlaylistMediaAvailability::finishScan() {
  if (!scan_ ||
      !scan_->finished.load(std::memory_order_acquire)) return;
  auto scan = std::move(scan_);
  resultTimer_->stop();
  const bool refreshRequested = std::exchange(refreshRequested_, false);
  if (!scan->succeeded || scan->generation != generation_) {
    if (refreshRequested) refresh();
    return;
  }

  auto detectedOfflineItemIds = std::move(scan->offlineItemIds);
  // File probes cannot restore (or otherwise overwrite) live-input health.
  for (const auto id : std::as_const(offlineItemIds_)) {
    const auto item = model_->itemState(id);
    if (item && item->sourceType == PlaylistSourceType::Uvc)
      detectedOfflineItemIds.insert(id);
  }
  QList<quint64> changedItemIds;
  changedItemIds.reserve(offlineItemIds_.size() +
                         detectedOfflineItemIds.size());
  for (const quint64 itemId : offlineItemIds_) {
    if (!detectedOfflineItemIds.contains(itemId)) {
      changedItemIds.append(itemId);
    }
  }
  for (const quint64 itemId : detectedOfflineItemIds) {
    if (!offlineItemIds_.contains(itemId)) {
      changedItemIds.append(itemId);
    }
  }

  offlineItemIds_ = std::move(detectedOfflineItemIds);
  for (const quint64 itemId : std::as_const(changedItemIds)) {
    if (scan->generation != generation_) break;
    emit model_->itemMediaAvailabilityChanged(itemId,
                                               offlineItemIds_.contains(itemId));
  }
  if (refreshRequested) refresh();
}
