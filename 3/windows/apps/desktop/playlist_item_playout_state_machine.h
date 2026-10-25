#pragma once

#include "playlist_item.h"

#include <QPointer>

#include <optional>

class PlaylistModel;

enum class PlaylistItemPlayoutPhase {
  Empty,
  Prepared,
  Active,
  Ended,
  Error,
  MissingItem,
};

struct PlaylistPreparedMedia {
  quint64 itemId = 0;
  QString filePath;
  qint64 inPointMs = 0;
  std::optional<qint64> outPointMs = std::nullopt;

  bool operator==(const PlaylistPreparedMedia &) const = default;
};

class PlaylistItemPlayoutStateMachine final {
public:
  explicit PlaylistItemPlayoutStateMachine(PlaylistModel *model);

  bool prepare(quint64 itemId);
  std::optional<PlaylistStartMode> start();
  std::optional<PlaylistEndMode> endModeForCurrentIteration() const;
  std::optional<PlaylistEndMode> finish();
  bool markActive();
  bool markEnded();
  bool deactivate();
  void fail();
  void reset();

  PlaylistItemPlayoutPhase phase() const;
  const PlaylistPreparedMedia *preparedMedia() const;
  int currentPlayCount() const;

private:
  Q_DISABLE_COPY_MOVE(PlaylistItemPlayoutStateMachine)

  std::optional<PlaylistItemState> currentItem() const;
  void markMissingItem();

  QPointer<PlaylistModel> model_;
  PlaylistItemPlayoutPhase phase_ = PlaylistItemPlayoutPhase::Empty;
  std::optional<PlaylistPreparedMedia> preparedMedia_ = std::nullopt;
  int completedPlayCount_ = 0;
};
