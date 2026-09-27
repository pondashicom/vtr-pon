#pragma once

#include "playlist_model.h"

// Owns playout routing, sequence analysis, and the future portion of TOTAL.
// PlaylistModel remains the source of item/order/availability state.
class PlaylistPlaySequence final {
public:
  explicit PlaylistPlaySequence(PlaylistModel *model);

  quint64 nextItemId(quint64 currentItemId, quint64 excludedItemId = 0) const;
  quint64 automaticEndTargetItemId(quint64 currentItemId,
                                   quint64 excludedItemId = 0) const;
  quint64 endTargetItemId(quint64 currentItemId, PlaylistEndMode endMode,
                          quint64 excludedItemId = 0) const;
  PlaylistMapAnalysis analyze(
      quint64 startItemId,
      std::optional<PlaylistMapPlaybackProgress> playbackProgress = std::nullopt,
      bool currentItemOnly = false) const;
  PlaylistTotalRemaining totalRemaining(quint64 onAirItemId,
                                       PlaylistMapPlaybackProgress progress) const;

  void invalidateFuture();

private:
  PlaylistModel *model_;
  mutable bool futureValid_ = false;
  mutable quint64 futureItemId_ = 0;
  mutable double futurePlaybackRate_ = 1.0;
  mutable std::optional<qint64> futureDurationMs_;
  mutable bool futureInfinite_ = false;
};
