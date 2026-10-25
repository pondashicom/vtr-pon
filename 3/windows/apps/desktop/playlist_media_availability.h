#pragma once

#include "playlist_model.h"

#include <memory>

class QTimer;
struct MediaAvailabilityScan;

// Owns the offline cache and worker lifecycle. Only snapshots cross into the
// worker; all model updates and notifications stay on the model's thread.
class PlaylistMediaAvailability final {
public:
  PlaylistMediaAvailability(PlaylistModel *model,
                            PlaylistModel::MediaAvailabilityProbe probe, int timeoutMs);
  ~PlaylistMediaAvailability();

  bool isOffline(quint64 itemId) const;
  void setOffline(quint64 itemId, bool offline);
  void replaceOffline(QSet<quint64> offlineItemIds);
  void refresh();
  void invalidateScan();
  void requestShutdown();
  bool shutdownComplete() const;

private:
  void finishScan();

  PlaylistModel *model_;
  int timeoutMs_;
  PlaylistModel::MediaAvailabilityProbe probe_;
  QSet<quint64> offlineItemIds_;
  std::shared_ptr<MediaAvailabilityScan> scan_;
  QTimer *resultTimer_ = nullptr;
  quint64 generation_ = 0;
  bool refreshRequested_ = false;
  bool shuttingDown_ = false;
};
