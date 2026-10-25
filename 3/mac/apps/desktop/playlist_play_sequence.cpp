#include "playlist_play_sequence.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <limits>

PlaylistPlaySequence::PlaylistPlaySequence(PlaylistModel *model) : model_(model) {
  const auto invalidate = [this] { invalidateFuture(); };
  QObject::connect(model, &PlaylistModel::itemAdded, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemRemoved, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemMoved, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemMovedToPlaylist, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemChanged, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemMediaAvailabilityChanged, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemConversionStateChanged, model, invalidate);
  QObject::connect(model, &PlaylistModel::itemReplaced, model, invalidate);
  QObject::connect(model, &PlaylistModel::playlistModeChanged, model, invalidate);
  QObject::connect(model, &PlaylistModel::playlistRemoved, model, invalidate);
  QObject::connect(model, &PlaylistModel::modelReset, model, invalidate);
}

void PlaylistPlaySequence::invalidateFuture() { futureValid_ = false; }

quint64 PlaylistPlaySequence::nextItemId(quint64 currentItemId,
                                          quint64 excludedItemId) const {
  const auto current = model_->itemState(currentItemId);
  if (!current) {
    return 0;
  }

  const QVector<quint64> &itemOrder =
      model_->itemOrdersByPlaylist_.value(current->playlistNumber);
  int currentRow = current->itemNumber - 1;
  if (currentRow < 0 || currentRow >= itemOrder.size() ||
      itemOrder.at(currentRow) != currentItemId) {
    currentRow = itemOrder.indexOf(currentItemId);
  }
  if (currentRow < 0 || itemOrder.isEmpty()) {
    return 0;
  }

  for (int offset = 1; offset <= itemOrder.size(); ++offset) {
    const quint64 candidate =
        itemOrder.at((currentRow + offset) % itemOrder.size());
    if (candidate != 0 && candidate != excludedItemId &&
        !model_->itemMediaOffline(candidate) &&
        !model_->itemConversionInProgress(candidate)) {
      return candidate;
    }
  }
  return 0;
}

quint64 PlaylistPlaySequence::automaticEndTargetItemId(
    quint64 currentItemId, quint64 excludedItemId) const {
  const auto current = model_->itemState(currentItemId);
  if (!current) {
    return 0;
  }
  return endTargetItemId(currentItemId, current->settings.endMode,
                         excludedItemId);
}

quint64 PlaylistPlaySequence::endTargetItemId(
    quint64 currentItemId, PlaylistEndMode endMode, quint64 excludedItemId) const {
  const auto current = model_->itemState(currentItemId);
  if (!current) {
    return 0;
  }
  if (endMode == PlaylistEndMode::Next) {
    return nextItemId(currentItemId, excludedItemId);
  }
  if (endMode != PlaylistEndMode::Goto) {
    return 0;
  }
  const quint64 targetItemId = current->settings.gotoTargetItemId;
  return targetItemId != 0 && targetItemId != excludedItemId &&
                 model_->itemsById_.contains(targetItemId) &&
                 !model_->itemMediaOffline(targetItemId) &&
                 !model_->itemConversionInProgress(targetItemId)
             ? targetItemId
             : 0;
}

PlaylistMapAnalysis PlaylistPlaySequence::analyze(
    quint64 startItemId,
    std::optional<PlaylistMapPlaybackProgress> playbackProgress,
    bool currentItemOnly) const {
  PlaylistMapAnalysis analysis;
  if (startItemId == 0 || !model_->itemsById_.contains(startItemId)) {
    analysis.stopReason = PlaylistMapStopReason::DestinationUnavailable;
    return analysis;
  }

  const auto markInfinite = [&](PlaylistMapStopReason reason) {
    analysis.totalDurationMs.reset();
    analysis.infinite = true;
    analysis.stopReason = reason;
  };
  const auto appendDuration = [&](qint64 durationMs) {
    if (durationMs < 0 ||
        analysis.totalDurationMs.value_or(0) >
            (std::numeric_limits<qint64>::max)() - durationMs) {
      return false;
    }
    analysis.totalDurationMs =
        analysis.totalDurationMs.value_or(0) + durationMs;
    return true;
  };
  const auto multiplyDuration = [](qint64 durationMs,
                                   int playCount) -> std::optional<qint64> {
    if (durationMs < 0 || playCount < 0 ||
        (playCount > 0 &&
         durationMs >
             (std::numeric_limits<qint64>::max)() / playCount)) {
      return std::nullopt;
    }
    return durationMs * playCount;
  };
  const auto terminalMode = [](const PlaylistItemSettings &settings) {
    if (settings.endMode != PlaylistEndMode::Repeat) {
      return settings.endMode;
    }
    switch (settings.repeatEndMode) {
    case PlaylistRepeatEndMode::Stop:
      return PlaylistEndMode::Stop;
    case PlaylistRepeatEndMode::Pause:
      return PlaylistEndMode::Pause;
    case PlaylistRepeatEndMode::Next:
      return PlaylistEndMode::Next;
    case PlaylistRepeatEndMode::Goto:
      return PlaylistEndMode::Goto;
    }
    return PlaylistEndMode::Pause;
  };

  analysis.totalDurationMs = 0;
  QSet<quint64> visitedItemIds;
  quint64 currentItemId = startItemId;
  bool startingItem = true;

  while (currentItemId != 0) {
    if (visitedItemIds.contains(currentItemId)) {
      markInfinite(PlaylistMapStopReason::Loop);
      return analysis;
    }
    visitedItemIds.insert(currentItemId);

    const auto item = model_->itemState(currentItemId);
    if (!item) {
      analysis.stopReason = PlaylistMapStopReason::DestinationUnavailable;
      return analysis;
    }

    PlaylistMapRow row;
    row.sequenceNumber = analysis.rows.size() + 1;
    row.itemId = item->id;
    row.playlistNumber = item->playlistNumber;
    row.itemNumber = item->itemNumber;
    row.itemName = item->displayName.isEmpty()
                       ? QFileInfo(item->filePath).fileName()
                       : item->displayName;
    row.settings = item->settings;

    const std::optional<qint64> fullDurationMs =
        item->sourceType == PlaylistSourceType::File &&
                item->settings.outPointMs &&
                *item->settings.outPointMs >= item->settings.inPointMs
            ? std::optional<qint64>(*item->settings.outPointMs -
                                    item->settings.inPointMs)
            : std::nullopt;
    if (!fullDurationMs) {
      row.infiniteDuration = true;
      analysis.rows.append(row);
      markInfinite(PlaylistMapStopReason::LiveOrUnknownDuration);
      return analysis;
    }

    qint64 contributionMs = *fullDurationMs;
    const double repeatRate = startingItem && playbackProgress &&
                                      std::isfinite(playbackProgress->playbackRate) &&
                                      playbackProgress->playbackRate > 0.0
                                  ? playbackProgress->playbackRate : 1.0;
    const qint64 repeatOverlapMs = static_cast<qint64>(
        playlistRepeatCrossfadeDurationMs(*item, *item->settings.outPointMs,
                                         repeatRate) * repeatRate);
    if (item->settings.endMode == PlaylistEndMode::Repeat) {
      if (item->settings.repeatCount == kInfiniteItemRepeatCount) {
        row.infiniteDuration = true;
        analysis.rows.append(row);
        markInfinite(PlaylistMapStopReason::InfiniteRepeat);
        return analysis;
      }
      if (startingItem && playbackProgress) {
        const int currentPlayCount =
            std::clamp(playbackProgress->currentPlayCount, 1,
                       item->settings.repeatCount);
        const int remainingPlayCount =
            item->settings.repeatCount - currentPlayCount;
        qint64 currentRemainingMs =
            std::clamp(playbackProgress->currentRemainingMs, qint64{0},
                       *fullDurationMs);
        if (remainingPlayCount > 0) {
          currentRemainingMs = (std::max)(currentRemainingMs, repeatOverlapMs);
        }
        const auto remainingRepeats =
            multiplyDuration(*fullDurationMs - repeatOverlapMs, remainingPlayCount);
        if (!remainingRepeats ||
            currentRemainingMs >
                (std::numeric_limits<qint64>::max)() -
                    *remainingRepeats) {
          row.infiniteDuration = true;
          analysis.rows.append(row);
          markInfinite(PlaylistMapStopReason::LiveOrUnknownDuration);
          return analysis;
        }
        contributionMs = currentRemainingMs + *remainingRepeats;
      } else {
        const auto repeatedDuration =
            multiplyDuration(*fullDurationMs,
                             item->settings.repeatCount);
        if (!repeatedDuration) {
          row.infiniteDuration = true;
          analysis.rows.append(row);
          markInfinite(PlaylistMapStopReason::LiveOrUnknownDuration);
          return analysis;
        }
        contributionMs = *repeatedDuration -
                         repeatOverlapMs * (item->settings.repeatCount - 1);
      }
    } else if (startingItem && playbackProgress) {
      contributionMs =
          std::clamp(playbackProgress->currentRemainingMs, qint64{0},
                     *fullDurationMs);
    }

    row.durationMs = contributionMs;
    analysis.rows.append(row);
    if (startingItem) {
      analysis.startingItemContributionMs = contributionMs;
    }
    if (!appendDuration(contributionMs)) {
      analysis.rows.last().infiniteDuration = true;
      markInfinite(PlaylistMapStopReason::LiveOrUnknownDuration);
      return analysis;
    }
    if (currentItemOnly) return analysis;

    if (startingItem &&
        item->settings.startMode == PlaylistStartMode::Pause) {
      analysis.stopReason = PlaylistMapStopReason::StartModePause;
      return analysis;
    }

    const PlaylistEndMode endMode = terminalMode(item->settings);
    if (endMode == PlaylistEndMode::Stop ||
        endMode == PlaylistEndMode::Pause) {
      return analysis;
    }

    const quint64 nextItemId = endTargetItemId(currentItemId, endMode);
    if (nextItemId == 0) {
      analysis.stopReason = PlaylistMapStopReason::DestinationUnavailable;
      return analysis;
    }
    const auto nextItem = model_->itemState(nextItemId);
    if (!nextItem) {
      analysis.stopReason = PlaylistMapStopReason::DestinationUnavailable;
      return analysis;
    }
    if (model_->playlistMode(nextItem->playlistNumber) == PlaylistMode::Bgm) {
      analysis.stopReason = PlaylistMapStopReason::DestinationBgm;
      return analysis;
    }
    if (nextItem->settings.startMode == PlaylistStartMode::Pause) {
      analysis.stopReason =
          PlaylistMapStopReason::DestinationStartModePause;
      return analysis;
    }

    currentItemId = nextItemId;
    startingItem = false;
  }

  return analysis;
}

PlaylistTotalRemaining PlaylistPlaySequence::totalRemaining(
    quint64 onAirItemId, PlaylistMapPlaybackProgress progress) const {
  if (!futureValid_ || futureItemId_ != onAirItemId ||
      futurePlaybackRate_ != progress.playbackRate) {
    const auto item = model_->itemState(onAirItemId);
    const int terminalPlayCount = item && item->settings.endMode == PlaylistEndMode::Repeat
                                      ? (std::max)(item->settings.repeatCount, 1)
                                      : 1;
    const auto future = analyze(
        onAirItemId, PlaylistMapPlaybackProgress{0, terminalPlayCount,
                                                 progress.playbackRate});
    futureItemId_ = onAirItemId;
    futurePlaybackRate_ = progress.playbackRate;
    futureDurationMs_ = future.totalDurationMs;
    futureInfinite_ = future.infinite;
    futureValid_ = true;
  }

  const auto current = analyze(onAirItemId, progress, true);
  if (current.infinite || futureInfinite_) return {std::nullopt, true};
  if (!current.totalDurationMs || !futureDurationMs_) return {};
  const qint64 futureItemsMs = *futureDurationMs_;
  if (current.startingItemContributionMs >
      (std::numeric_limits<qint64>::max)() - futureItemsMs) {
    return {std::nullopt, true};
  }
  const qint64 startingItemWallClockMs = static_cast<qint64>(std::ceil(
      static_cast<double>(current.startingItemContributionMs) /
      (std::max)(progress.playbackRate, 0.01)));
  if (startingItemWallClockMs >
      (std::numeric_limits<qint64>::max)() - futureItemsMs) {
    return {std::nullopt, true};
  }
  return {startingItemWallClockMs + futureItemsMs, false};
}
