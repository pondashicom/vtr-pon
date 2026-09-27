#include "playlist_model.h"
#include "playlist_play_sequence.h"
#include "playlist_media_availability.h"

#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

void setError(QString *errorDetail, const QString &message) {
  if (errorDetail) {
    *errorDetail = message;
  }
}

bool reserveItemId(quint64 referencedId, quint64 *nextItemId, QString *errorDetail) {
  if (referencedId == (std::numeric_limits<quint64>::max)()) {
    setError(errorDetail, QStringLiteral("The playlist item ID range is exhausted."));
    return false;
  }
  *nextItemId = (std::max)(*nextItemId, referencedId + 1);
  return true;
}

bool reserveMacroItemIds(const QList<MacroDefinition> &macros,
                         const QString &documentId, quint64 *nextItemId,
                         QString *errorDetail) {
  for (const auto &macro : macros) {
    for (const auto &step : macro.steps) {
      // Disabled and incomplete steps still retain identity. A different
      // document's numbers must never exhaust this document's ID allocator.
      if (step.documentId != documentId) continue;
      if (!reserveItemId(step.itemId, nextItemId, errorDetail) ||
          !reserveItemId(step.settingsPatch.value(QStringLiteral("gotoTargetItemId"))
                             .toString().toULongLong(), nextItemId, errorDetail)) return false;
    }
  }
  return true;
}

bool validItemSettings(const PlaylistItemSettings &settings) {
  const bool validRepeatEndMode = [&settings] {
    switch (settings.repeatEndMode) {
    case PlaylistRepeatEndMode::Stop:
    case PlaylistRepeatEndMode::Pause:
    case PlaylistRepeatEndMode::Next:
    case PlaylistRepeatEndMode::Goto:
      return true;
    }
    return false;
  }();
  return std::all_of(settings.nominalEqTenthsDb.begin(), settings.nominalEqTenthsDb.end(),
                     [](int gain) { return gain >= -vtrpon::kEqMaximumTenthsDb && gain <= vtrpon::kEqMaximumTenthsDb; }) &&
         settings.nominalAudioDelayFrames >= 0 && settings.nominalAudioDelayFrames <= 6 &&
         settings.inPointMs >= 0 &&
         (!settings.outPointMs ||
          *settings.outPointMs > settings.inPointMs) &&
         settings.fadeInDurationMs >= kMinimumItemFadeDurationMs &&
         settings.fadeInDurationMs <= kMaximumItemFadeDurationMs &&
         settings.fadeOutDurationMs >= kMinimumItemFadeDurationMs &&
         settings.fadeOutDurationMs <= kMaximumItemFadeDurationMs &&
         settings.endCrossfadeDurationMs >= kMinimumItemFadeDurationMs &&
         settings.endCrossfadeDurationMs <= kMaximumItemFadeDurationMs &&
         settings.repeatCount >= kInfiniteItemRepeatCount &&
         settings.repeatCount <= kMaximumItemRepeatCount &&
         validRepeatEndMode &&
         settings.nominalVolumeTenthsDb >= kMinimumItemVolumeTenthsDb &&
         settings.nominalVolumeTenthsDb <= kMaximumItemVolumeTenthsDb;
}

bool validItemColor(PlaylistItemColor color) {
  switch (color) {
  case PlaylistItemColor::Default:
  case PlaylistItemColor::Red:
  case PlaylistItemColor::Yellow:
  case PlaylistItemColor::Blue:
  case PlaylistItemColor::Green:
    return true;
  }
  return false;
}

bool validMediaType(PlaylistMediaType mediaType) {
  switch (mediaType) {
  case PlaylistMediaType::Unknown:
  case PlaylistMediaType::Video:
  case PlaylistMediaType::Audio:
  case PlaylistMediaType::StillImage:
  case PlaylistMediaType::Presentation:
  case PlaylistMediaType::Pdf:
    return true;
  }
  return false;
}

bool validPowerPointMode(PlaylistPowerPointMode mode,
                         PlaylistSourceType sourceType,
                         PlaylistMediaType mediaType) {
  switch (mode) {
  case PlaylistPowerPointMode::None:
    if (sourceType == PlaylistSourceType::Pdf) {
      return mediaType == PlaylistMediaType::Pdf;
    }
    return mediaType != PlaylistMediaType::Pdf;
  case PlaylistPowerPointMode::NativeSlideShow:
    return sourceType == PlaylistSourceType::PowerPoint &&
           mediaType == PlaylistMediaType::Presentation;
  case PlaylistPowerPointMode::VideoConversion:
    return sourceType == PlaylistSourceType::File &&
           mediaType == PlaylistMediaType::Video;
  case PlaylistPowerPointMode::PngSequence:
    return sourceType == PlaylistSourceType::File &&
           mediaType == PlaylistMediaType::StillImage;
  }
  return false;
}

bool validRegistrationSource(const PlaylistItemRegistration &item,
                             const QFileInfo &fileInfo) {
  switch (item.sourceType) {
  case PlaylistSourceType::File:
  case PlaylistSourceType::PowerPoint:
  case PlaylistSourceType::Pdf:
    return fileInfo.exists() && fileInfo.isFile();
  case PlaylistSourceType::Uvc:
    return !item.filePath.isEmpty() && !item.displayName.isEmpty();
  }
  return false;
}

bool mediaFileAvailable(const PlaylistItemState &item) {
  if (item.sourceType == PlaylistSourceType::Uvc) {
    return true;
  }
  const QFileInfo fileInfo(item.filePath);
  return fileInfo.exists() && fileInfo.isFile();
}

} // namespace

PlaylistModel::PlaylistModel(QObject *parent)
    : PlaylistModel(parent, [](const QString &path) {
        const QFileInfo fileInfo(path);
        return fileInfo.exists() && fileInfo.isFile();
      }) {}

PlaylistModel::PlaylistModel(QObject *parent,
                             MediaAvailabilityProbe availabilityProbe)
    : QObject(parent), playSequence_(std::make_unique<PlaylistPlaySequence>(this)),
      mediaAvailability_(std::make_unique<PlaylistMediaAvailability>(
          this, std::move(availabilityProbe))) {
  PlaylistState initialPlaylist;
  initialPlaylist.number = 1;
  playlistsByNumber_.insert(initialPlaylist.number, initialPlaylist);
  playlistOrder_.append(initialPlaylist.number);
  itemOrdersByPlaylist_.insert(initialPlaylist.number, {});
}

PlaylistModel::~PlaylistModel() = default;

quint64 PlaylistModel::addPendingMediaItem(const QString &path,
                                         int playlistNumber, PlaylistSourceType sourceType) {
  if (sourceType != PlaylistSourceType::File && sourceType != PlaylistSourceType::Pdf &&
      sourceType != PlaylistSourceType::PowerPoint) return 0;
  if (path.isEmpty() || !playlistsByNumber_.contains(playlistNumber) ||
      nextItemId_ == (std::numeric_limits<quint64>::max)()) return 0;
  const QFileInfo file(path); // Path manipulation only; no stat on the GUI thread.
  PlaylistItemState state;
  state.id = nextItemId_++;
  state.playlistNumber = playlistNumber;
  state.filePath = file.absoluteFilePath();
  state.displayName = file.fileName();
  state.sourceType = sourceType;
  if (sourceType != PlaylistSourceType::File) {
    state.mediaType = sourceType == PlaylistSourceType::Pdf
        ? PlaylistMediaType::Pdf : PlaylistMediaType::Presentation;
    state.powerPointMode = sourceType == PlaylistSourceType::PowerPoint
        ? PlaylistPowerPointMode::NativeSlideShow : PlaylistPowerPointMode::None;
    if (sourceType == PlaylistSourceType::Pdf) state.displayName = file.completeBaseName();
    state.settings.startMode = PlaylistStartMode::Play;
    state.settings.endMode = PlaylistEndMode::Pause;
  }
  auto &order = itemOrdersByPlaylist_[playlistNumber];
  state.itemNumber = order.size() + 1;
  itemsById_.insert(state.id, state);
  conversionStatesById_.insert(state.id,
      {PlaylistItemConversionStage::AnalysisWaiting, 0});
  order.append(state.id);
  emit itemAdded(state.id);
  if (playlistNumber == activePlaylistNumber_) emit countChanged(itemCount());
  return state.id;
}

bool PlaylistModel::completePendingNativeDocument(quint64 itemId) {
  auto item = itemsById_.find(itemId);
  const auto conversion = itemConversionState(itemId);
  if (item == itemsById_.end() || !conversion ||
      (conversion->stage != PlaylistItemConversionStage::AnalysisWaiting &&
       conversion->stage != PlaylistItemConversionStage::Analyzing) ||
      (item->sourceType != PlaylistSourceType::Pdf &&
       item->sourceType != PlaylistSourceType::PowerPoint)) return false;
  conversionStatesById_.remove(itemId);
  mediaAvailability_->setOffline(itemId, false);
  emit itemConversionStateChanged(itemId);
  emit itemReplaced(itemId);
  emit itemChanged(itemId);
  return true;
}

bool PlaylistModel::completePendingMediaItem(
    quint64 itemId, PlaylistMediaType mediaType,
    std::optional<qint64> durationMs) {
  const auto conversion = itemConversionState(itemId);
  auto item = itemsById_.find(itemId);
  if (item == itemsById_.end() || !conversion ||
      (conversion->stage != PlaylistItemConversionStage::AnalysisWaiting &&
       conversion->stage != PlaylistItemConversionStage::Analyzing) ||
      item->sourceType != PlaylistSourceType::File ||
      (mediaType != PlaylistMediaType::Video &&
       mediaType != PlaylistMediaType::Audio &&
       mediaType != PlaylistMediaType::StillImage) ||
      (durationMs && *durationMs <= item->settings.inPointMs)) return false;
  // File existence and discoverer validation belong to the worker. Preserve
  // identity, current position, and any display customization made meanwhile.
  item->mediaType = mediaType;
  item->settings.outPointMs = durationMs;
  setKnownMediaDuration(itemId, durationMs.value_or(-1));
  conversionStatesById_.remove(itemId);
  mediaAvailability_->setOffline(itemId, false);
  emit itemConversionStateChanged(itemId);
  emit itemReplaced(itemId);
  emit itemChanged(itemId);
  return true;
}

QList<quint64>
PlaylistModel::addItems(const QList<PlaylistItemRegistration> &items) {
  int addedCount = 0;
  QList<quint64> addedItemIds;
  for (const PlaylistItemRegistration &item : items) {
    const QFileInfo fileInfo(item.filePath);
    if (nextItemId_ == (std::numeric_limits<quint64>::max)() ||
        !validRegistrationSource(item, fileInfo) ||
        !validItemSettings(item.settings) || !validItemColor(item.color) ||
        !validMediaType(item.mediaType) ||
        !validPowerPointMode(item.powerPointMode, item.sourceType,
                             item.mediaType) ||
        (item.settings.gotoTargetItemId != 0 &&
         !itemsById_.contains(item.settings.gotoTargetItemId))) {
      continue;
    }

    PlaylistItemState state;
    state.id = nextItemId_++;
    state.playlistNumber = playlistsByNumber_.contains(item.playlistNumber)
                               ? item.playlistNumber
                               : activePlaylistNumber_;
    QVector<quint64> &itemOrder =
        itemOrdersByPlaylist_[state.playlistNumber];
    state.itemNumber = itemOrder.size() + 1;
    state.filePath = item.sourceType == PlaylistSourceType::Uvc
                         ? item.filePath
                         : fileInfo.absoluteFilePath();
    state.displayName = item.sourceType == PlaylistSourceType::Uvc
                            ? item.displayName
                            : item.displayName.isEmpty()
                                  ? fileInfo.fileName()
                                  : item.displayName;
    state.sourceType = item.sourceType;
    state.mediaType = item.mediaType;
    state.powerPointMode = item.powerPointMode;
    state.color = item.color;
    state.separatorBelow = item.separatorBelow;
    state.settings = item.settings;
    if (state.sourceType == PlaylistSourceType::Uvc)
      state.settings.startMode = PlaylistStartMode::Play;
    itemsById_.insert(state.id, state);
    if (item.conversionState) {
      PlaylistItemConversionState conversion = *item.conversionState;
      conversion.progressPermille =
          std::clamp(conversion.progressPermille, 0, 1000);
      conversionStatesById_.insert(state.id, conversion);
    }
    itemOrder.append(state.id);
    addedItemIds.append(state.id);
    if (state.playlistNumber == activePlaylistNumber_) {
      ++addedCount;
    }
    emit itemAdded(state.id);
  }
  if (addedCount > 0) {
    emit countChanged(itemCount());
  }
  return addedItemIds;
}

QList<quint64> PlaylistModel::addItemsAtomically(
    const QList<PlaylistItemRegistration> &items) {
  if (items.isEmpty()) {
    return {};
  }
  const quint64 availableIds =
      (std::numeric_limits<quint64>::max)() - nextItemId_;
  if (static_cast<quint64>(items.size()) > availableIds) {
    return {};
  }
  QList<PlaylistItemState> states;
  states.reserve(items.size());
  QHash<quint64, PlaylistItemConversionState> conversionStates;
  QHash<int, int> appendedCounts;
  quint64 candidateId = nextItemId_;
  for (const PlaylistItemRegistration &item : items) {
    const QFileInfo fileInfo(item.filePath);
    if (!validRegistrationSource(item, fileInfo) ||
        !validItemSettings(item.settings) || !validItemColor(item.color) ||
        !validMediaType(item.mediaType) ||
        !validPowerPointMode(item.powerPointMode, item.sourceType,
                             item.mediaType) ||
        (item.settings.gotoTargetItemId != 0 &&
         !itemsById_.contains(item.settings.gotoTargetItemId))) {
      return {};
    }

    PlaylistItemState state;
    state.id = candidateId++;
    state.playlistNumber = playlistsByNumber_.contains(item.playlistNumber)
                               ? item.playlistNumber
                               : activePlaylistNumber_;
    state.itemNumber = itemOrdersByPlaylist_[state.playlistNumber].size() +
                       appendedCounts.value(state.playlistNumber) + 1;
    state.filePath = item.sourceType == PlaylistSourceType::Uvc
                         ? item.filePath
                         : fileInfo.absoluteFilePath();
    state.displayName = item.sourceType == PlaylistSourceType::Uvc
                            ? item.displayName
                            : item.displayName.isEmpty()
                                  ? fileInfo.fileName()
                                  : item.displayName;
    state.sourceType = item.sourceType;
    state.mediaType = item.mediaType;
    state.powerPointMode = item.powerPointMode;
    state.color = item.color;
    state.separatorBelow = item.separatorBelow;
    state.settings = item.settings;
    if (state.sourceType == PlaylistSourceType::Uvc)
      state.settings.startMode = PlaylistStartMode::Play;
    states.append(state);
    if (item.conversionState) {
      PlaylistItemConversionState conversion = *item.conversionState;
      conversion.progressPermille =
          std::clamp(conversion.progressPermille, 0, 1000);
      conversionStates.insert(state.id, conversion);
    }
    ++appendedCounts[state.playlistNumber];
  }

  QList<quint64> added;
  added.reserve(states.size());
  int activeAddedCount = 0;
  nextItemId_ = candidateId;
  for (const PlaylistItemState &state : states) {
    itemsById_.insert(state.id, state);
    if (conversionStates.contains(state.id)) {
      conversionStatesById_.insert(state.id,
                                   conversionStates.value(state.id));
    }
    itemOrdersByPlaylist_[state.playlistNumber].append(state.id);
    added.append(state.id);
    activeAddedCount += state.playlistNumber == activePlaylistNumber_ ? 1 : 0;
  }
  for (quint64 itemId : added) {
    emit itemAdded(itemId);
  }
  if (activeAddedCount > 0) {
    emit countChanged(itemCount());
  }
  return added;
}

QList<quint64> PlaylistModel::replaceItemAtomically(
    quint64 itemId, const QList<PlaylistItemRegistration> &items) {
  const auto replaced = itemState(itemId);
  const int replacedRow = indexOf(itemId);
  if (!replaced || replacedRow < 0 || items.isEmpty() ||
      itemId == onAirItemId_ || itemId == cueItemId_ || itemId == dskItemId_ ||
      itemId == editItemId_) {
    return {};
  }

  const quint64 additionalItemCount =
      static_cast<quint64>(items.size() - 1);
  const quint64 availableIds =
      (std::numeric_limits<quint64>::max)() - nextItemId_;
  if (additionalItemCount > availableIds) {
    return {};
  }

  QList<PlaylistItemState> replacementStates;
  replacementStates.reserve(items.size());
  const bool replacingConversion = itemConversionInProgress(itemId);
  const QFileInfo originalFileInfo(replaced->filePath);
  const QString defaultPendingName =
      replaced->sourceType == PlaylistSourceType::Pdf
          ? originalFileInfo.completeBaseName()
          : originalFileInfo.fileName();
  const bool renamedDuringConversion =
      replacingConversion && replaced->displayName != defaultPendingName;
  quint64 candidateId = nextItemId_;
  for (qsizetype index = 0; index < items.size(); ++index) {
    const PlaylistItemRegistration &registration = items.at(index);
    const QFileInfo fileInfo(registration.filePath);
    if (registration.conversionState ||
        !validRegistrationSource(registration, fileInfo) ||
        !validItemSettings(registration.settings) ||
        !validItemColor(registration.color) ||
        !validMediaType(registration.mediaType) ||
        !validPowerPointMode(registration.powerPointMode,
                             registration.sourceType,
                             registration.mediaType) ||
        (registration.settings.gotoTargetItemId != 0 &&
         !itemsById_.contains(registration.settings.gotoTargetItemId))) {
      return {};
    }

    PlaylistItemState state;
    state.id = index == 0 ? itemId : candidateId++;
    state.itemNumber = replacedRow + static_cast<int>(index) + 1;
    state.playlistNumber = replaced->playlistNumber;
    state.filePath = registration.sourceType == PlaylistSourceType::Uvc
                         ? registration.filePath
                         : fileInfo.absoluteFilePath();
    state.displayName = registration.sourceType == PlaylistSourceType::Uvc
                            ? registration.displayName
                            : registration.displayName.isEmpty()
                                  ? fileInfo.fileName()
                                  : registration.displayName;
    state.sourceType = registration.sourceType;
    state.mediaType = registration.mediaType;
    state.powerPointMode = registration.powerPointMode;
    state.color = registration.color;
    state.separatorBelow = registration.separatorBelow;
    state.settings = registration.settings;
    if (state.sourceType == PlaylistSourceType::Uvc)
      state.settings.startMode = PlaylistStartMode::Play;
    if (replacingConversion) {
      // A converted document may expand into several items. Keep an edited
      // name on the first result, color on every result, and the separator
      // after the final result. Native documents complete in place instead.
      if (index == 0 && renamedDuringConversion) {
        state.displayName = replaced->displayName;
      }
      state.color = replaced->color;
      state.separatorBelow =
          index == items.size() - 1 && replaced->separatorBelow;
    }
    replacementStates.append(state);
  }

  QVector<quint64> &itemOrder =
      itemOrdersByPlaylist_[replaced->playlistNumber];
  itemOrder.removeAt(replacedRow);
  itemsById_.remove(itemId);
  knownMediaDurationsById_.remove(itemId);
  mediaAvailability_->setOffline(itemId, false);
  conversionStatesById_.remove(itemId);
  currentPlayCountsById_.remove(itemId);

  QList<quint64> replacementIds;
  replacementIds.reserve(replacementStates.size());
  for (qsizetype index = 0; index < replacementStates.size(); ++index) {
    const PlaylistItemState &state = replacementStates.at(index);
    itemsById_.insert(state.id, state);
    if (replacingConversion && state.sourceType == PlaylistSourceType::File &&
        (state.mediaType == PlaylistMediaType::Video ||
         state.mediaType == PlaylistMediaType::Audio) &&
        state.settings.outPointMs) {
      setKnownMediaDuration(state.id, *state.settings.outPointMs);
    }
    itemOrder.insert(replacedRow + static_cast<int>(index), state.id);
    replacementIds.append(state.id);
  }
  nextItemId_ = candidateId;
  updateItemNumbers(replaced->playlistNumber, replacedRow);

  if (bulkUndo_) {
    for (const auto &change : bulkUndo_->settings) {
      if (change.itemId == itemId) bulkUndo_->replacedItemIds.insert(itemId);
    }
    for (const auto &change : bulkUndo_->playlists) {
      if (change.beforeOrder != change.afterOrder &&
          change.beforeOrder.contains(itemId)) {
        bulkUndo_->replacedItemIds.insert(itemId);
      }
    }
  }
  emit itemConversionStateChanged(itemId);
  emit itemReplaced(itemId);
  emit itemChanged(itemId);
  for (qsizetype index = 1; index < replacementIds.size(); ++index) {
    emit itemAdded(replacementIds.at(index));
  }
  if (replaced->playlistNumber == activePlaylistNumber_ &&
      replacementIds.size() != 1) {
    emit countChanged(itemCount());
  }
  return replacementIds;
}

bool PlaylistModel::moveItem(quint64 itemId, int offset) {
  if (offset != -1 && offset != 1) {
    return false;
  }
  const int previousRow = indexOf(itemId);
  if (previousRow < 0) {
    return false;
  }
  return moveItemTo(itemId, previousRow + offset);
}

bool PlaylistModel::moveItemTo(quint64 itemId, int targetRow) {
  const int previousRow = indexOf(itemId);
  const auto state = itemState(itemId);
  if (!state) {
    return false;
  }
  QVector<quint64> &itemOrder =
      itemOrdersByPlaylist_[state->playlistNumber];
  if (previousRow < 0 || targetRow < 0 || targetRow >= itemOrder.size() ||
      previousRow == targetRow) {
    return false;
  }
  itemOrder.move(previousRow, targetRow);
  updateItemNumbers(state->playlistNumber,
                    (std::min)(previousRow, targetRow),
                    (std::max)(previousRow, targetRow));
  emit itemMoved(itemId, previousRow, targetRow);
  return true;
}

bool PlaylistModel::moveItemToPlaylist(quint64 itemId,
                                       int targetPlaylistNumber,
                                       int targetInsertionRow) {
  const auto state = itemState(itemId);
  if (!state || !playlistsByNumber_.contains(targetPlaylistNumber)) {
    return false;
  }

  const int previousPlaylistNumber = state->playlistNumber;
  const int previousRow = indexOf(itemId);
  const int targetItemCount = itemCount(targetPlaylistNumber);
  if (previousRow < 0 || targetInsertionRow < 0 ||
      targetInsertionRow > targetItemCount) {
    return false;
  }

  if (previousPlaylistNumber == targetPlaylistNumber) {
    int targetRow = targetInsertionRow;
    if (targetRow > previousRow) {
      --targetRow;
    }
    return moveItemTo(itemId, targetRow);
  }

  QVector<quint64> &previousOrder =
      itemOrdersByPlaylist_[previousPlaylistNumber];
  QVector<quint64> &currentOrder =
      itemOrdersByPlaylist_[targetPlaylistNumber];
  previousOrder.removeAt(previousRow);
  currentOrder.insert(targetInsertionRow, itemId);

  auto item = itemsById_.find(itemId);
  item->playlistNumber = targetPlaylistNumber;
  updateItemNumbers(previousPlaylistNumber, previousRow);
  updateItemNumbers(targetPlaylistNumber, targetInsertionRow);

  emit itemMovedToPlaylist(itemId, previousPlaylistNumber, previousRow,
                           targetPlaylistNumber, targetInsertionRow);
  if (activePlaylistNumber_ == previousPlaylistNumber ||
      activePlaylistNumber_ == targetPlaylistNumber) {
    emit countChanged(itemCount());
  }
  return true;
}

quint64 PlaylistModel::copyItemToPlaylist(quint64 itemId,
                                          int targetPlaylistNumber,
                                          int targetInsertionRow) {
  const auto source = itemState(itemId);
  const int targetItemCount = itemCount(targetPlaylistNumber);
  if (!source || itemConversionInProgress(itemId) ||
      !playlistsByNumber_.contains(targetPlaylistNumber) ||
      targetInsertionRow < 0 || targetInsertionRow > targetItemCount ||
      nextItemId_ == (std::numeric_limits<quint64>::max)()) {
    return 0;
  }

  PlaylistItemState copied = *source;
  copied.id = nextItemId_++;
  copied.playlistNumber = targetPlaylistNumber;
  copied.itemNumber = targetInsertionRow + 1;
  itemsById_.insert(copied.id, copied);
  if (knownMediaDurationsById_.contains(itemId)) {
    knownMediaDurationsById_.insert(
        copied.id, knownMediaDurationsById_.value(itemId));
  }
  if (itemMediaOffline(itemId)) {
    mediaAvailability_->setOffline(copied.id, true);
  }
  itemOrdersByPlaylist_[targetPlaylistNumber].insert(targetInsertionRow,
                                                      copied.id);
  updateItemNumbers(targetPlaylistNumber, targetInsertionRow);

  emit itemAdded(copied.id);
  emit itemCopiedToPlaylist(itemId, copied.id, targetPlaylistNumber,
                            targetInsertionRow);
  if (activePlaylistNumber_ == targetPlaylistNumber) {
    emit countChanged(itemCount());
  }
  return copied.id;
}

bool PlaylistModel::applyBulkPlaybackOperation(
    int playlistNumber, PlaylistBulkPlaybackOperation operation) {
  if (!playlistsByNumber_.contains(playlistNumber)) {
    return false;
  }
  switch (operation) {
  case PlaylistBulkPlaybackOperation::PlayOnce:
  case PlaylistBulkPlaybackOperation::RepeatAll:
  case PlaylistBulkPlaybackOperation::Shuffle:
  case PlaylistBulkPlaybackOperation::BgmMode:
  case PlaylistBulkPlaybackOperation::StandardMode:
    break;
  default:
    return false;
  }

  const QVector<quint64> originalOrder =
      itemOrdersByPlaylist_.value(playlistNumber);
  for (const quint64 itemId : originalOrder) {
    if (!itemsById_.contains(itemId) ||
        settingsLockedItemIds_.contains(itemId) ||
        itemConversionInProgress(itemId)) {
      return false;
    }
  }

  QVector<quint64> desiredOrder = originalOrder;
  if (operation == PlaylistBulkPlaybackOperation::Shuffle) {
    for (int index = desiredOrder.size() - 1; index > 0; --index) {
      const int randomIndex = QRandomGenerator::global()->bounded(index + 1);
      desiredOrder.swapItemsAt(index, randomIndex);
    }
    if (desiredOrder.size() > 1 && desiredOrder == originalOrder) {
      std::rotate(desiredOrder.begin(), desiredOrder.begin() + 1,
                  desiredOrder.end());
    }
  }

  if (operation == PlaylistBulkPlaybackOperation::StandardMode) {
    return setPlaylistMode(playlistNumber, PlaylistMode::Standard);
  }

  if (operation == PlaylistBulkPlaybackOperation::BgmMode &&
      !setPlaylistMode(playlistNumber, PlaylistMode::Bgm)) {
    return false;
  }

  const bool pauseLast =
      operation == PlaylistBulkPlaybackOperation::PlayOnce;
  for (int row = 0; row < desiredOrder.size(); ++row) {
    const quint64 itemId = desiredOrder.at(row);
    auto item = itemsById_.find(itemId);
    PlaylistItemSettings settings = item->settings;
    settings.startMode = PlaylistStartMode::Play;
    settings.endMode = pauseLast && row + 1 == desiredOrder.size()
                           ? PlaylistEndMode::Pause
                           : PlaylistEndMode::Next;
    if (item->settings == settings) {
      continue;
    }
    item->settings = settings;
    emit itemChanged(itemId);
    emit itemSettingsChanged(itemId, settings);
  }

  for (int targetRow = 0; targetRow < desiredOrder.size(); ++targetRow) {
    const quint64 itemId = desiredOrder.at(targetRow);
    const int currentRow = indexOf(itemId);
    if (currentRow != targetRow && !moveItemTo(itemId, targetRow)) {
      return false;
    }
  }
  return true;
}

void PlaylistModel::rememberBulkOperation(const PlaylistModelSnapshot &before,
                                          int playlistNumber) {
  BulkUndo undo;
  QHash<int, QVector<quint64>> beforeOrders;
  QSet<quint64> removedIds;
  for (const auto &item : before.items) {
    beforeOrders[item.playlistNumber].append(item.id);
    if (item.playlistNumber == playlistNumber && !itemsById_.contains(item.id)) {
      undo.removedItems.append(item);
      removedIds.insert(item.id);
    }
  }
  for (const auto &item : before.items) {
    const auto current = itemState(item.id);
    if (!current) continue;
    auto previous = current->settings;
    if (item.playlistNumber == playlistNumber) {
      previous.startMode = item.settings.startMode;
      previous.endMode = item.settings.endMode;
    }
    if (removedIds.contains(item.settings.gotoTargetItemId) &&
        current->settings.gotoTargetItemId == 0) {
      previous.gotoTargetItemId = item.settings.gotoTargetItemId;
    }
    if (previous != current->settings) {
      undo.settings.append({item.id, previous, current->settings});
    }
  }
  for (const auto &playlist : before.playlists) {
    if (playlist.number != playlistNumber) continue;
    if (!playlistsByNumber_.contains(playlist.number)) {
      undo.removedPlaylist = playlist;
      undo.removedPlaylistIndex = before.playlists.indexOf(playlist);
    }
    const auto beforeOrder = beforeOrders.value(playlist.number);
    const auto afterOrder = itemOrdersByPlaylist_.value(playlist.number);
    const auto afterMode = undo.removedPlaylist ? playlist.mode : playlistMode(playlist.number);
    if (beforeOrder != afterOrder || playlist.mode != afterMode) {
      undo.playlists.append({playlist.number, playlist.mode, afterMode,
                             beforeOrder, afterOrder});
    }
  }
  // A no-op must not replace the last operation that can actually be undone.
  if (undo.settings.isEmpty() && undo.playlists.isEmpty() &&
      undo.removedItems.isEmpty() && !undo.removedPlaylist) {
    return;
  }
  bulkUndo_ = std::move(undo);
  emit bulkUndoAvailableChanged(true);
}

bool PlaylistModel::hasBulkUndo() const { return bulkUndo_.has_value(); }

bool PlaylistModel::undoBulkOperation(QString *errorDetail,
                                       bool *keptLaterChanges) {
  if (keptLaterChanges) *keptLaterChanges = false;
  if (!bulkUndo_) {
    setError(errorDetail, QStringLiteral("There is no playlist bulk operation to undo."));
    return false;
  }
  if (onAirItemId_ != 0 || dskItemId_ != 0 || bgmItemId_ != 0 ||
      !conversionStatesById_.isEmpty()) {
    setError(errorDetail, QStringLiteral("Stop playout and wait for document imports before undoing."));
    return false;
  }
  // Build and validate the entire result before mutating anything. This is a
  // three-way comparison of only the fields owned by the bulk operation; the
  // current model supplies all other fields, playlists and runtime state.
  const BulkUndo undo = *bulkUndo_;
  auto desiredItems = itemsById_;
  auto desiredOrders = itemOrdersByPlaylist_;
  auto desiredPlaylists = playlistsByNumber_;
  auto desiredPlaylistOrder = playlistOrder_;
  if (undo.removedPlaylist) {
    const auto &playlist = *undo.removedPlaylist;
    if (desiredPlaylists.contains(playlist.number)) {
      setError(errorDetail, QStringLiteral("The deleted playlist number is already in use."));
      return false;
    }
    desiredPlaylists.insert(playlist.number, playlist);
    desiredOrders.insert(playlist.number, {});
    desiredPlaylistOrder.insert(
        std::min(undo.removedPlaylistIndex, int(desiredPlaylistOrder.size())),
        playlist.number);
  }
  bool keptChanges = false;
  QList<PlaylistItemState> restoredItems;
  QList<quint64> changedSettings;
  QList<int> changedModes;
  for (const auto &item : undo.removedItems) {
    if (desiredItems.contains(item.id) ||
        !desiredPlaylists.contains(item.playlistNumber)) {
      keptChanges = true;
      continue;
    }
    desiredItems.insert(item.id, item);
    restoredItems.append(item);
  }
  for (const auto &item : restoredItems) {
    auto &settings = desiredItems[item.id].settings;
    if (settings.gotoTargetItemId != 0 &&
        !desiredItems.contains(settings.gotoTargetItemId)) {
      // Keep later deletion of a GOTO destination when restoring a cleared row.
      settings.gotoTargetItemId = 0;
      keptChanges = true;
    }
  }
  for (const auto &change : undo.settings) {
    auto current = desiredItems.find(change.itemId);
    // Native sources restrict individual editing, but participate in bulk
    // playback operations. Match their explicit lock guard when undoing.
    if (current == desiredItems.end() ||
        settingsLockedItemIds_.contains(change.itemId) ||
        undo.replacedItemIds.contains(change.itemId)) {
      keptChanges = true;
      continue;
    }
    auto &settings = current->settings;
    const auto previousSettings = settings;
    const auto restoreField = [&keptChanges](auto &value, const auto &before,
                                             const auto &after) {
      if (before == after) return;
      if (value == after) {
        value = before;
      } else {
        keptChanges = true;
      }
    };
    restoreField(settings.startMode, change.before.startMode,
                   change.after.startMode);
    restoreField(settings.endMode, change.before.endMode, change.after.endMode);
    if (change.before.gotoTargetItemId == 0 ||
        desiredItems.contains(change.before.gotoTargetItemId)) {
      restoreField(settings.gotoTargetItemId, change.before.gotoTargetItemId,
                     change.after.gotoTargetItemId);
    } else if (change.before.gotoTargetItemId != change.after.gotoTargetItemId) {
      keptChanges = true;
    }
    if (settings != previousSettings) changedSettings.append(change.itemId);
  }
  for (const auto &change : undo.playlists) {
    bool locked = false;
    for (const auto id : itemOrdersByPlaylist_.value(change.number)) {
      locked |= settingsLockedItemIds_.contains(id);
    }
    auto playlist = desiredPlaylists.find(change.number);
    if (playlist == desiredPlaylists.end()) {
      keptChanges = true;
      continue;
    }
    if (change.beforeMode != change.afterMode) {
      if (!locked && playlist->mode == change.afterMode) {
        playlist->mode = change.beforeMode;
        changedModes.append(change.number);
      } else {
        keptChanges = true;
      }
    }
    if (change.beforeOrder == change.afterOrder) continue;
    auto &order = desiredOrders[change.number];
    if (change.afterOrder.isEmpty()) {
      // Clear: put the removed items back before items added afterwards.
      QVector<quint64> restoredOrder;
      for (const auto &item : restoredItems) {
        if (item.playlistNumber == change.number) restoredOrder.append(item.id);
      }
      order = restoredOrder + order;
      continue;
    }
    const QSet<quint64> originalIds(change.beforeOrder.cbegin(),
                                   change.beforeOrder.cend());
    QVector<quint64> survivingOrder;
    bool replaced = false;
    for (const quint64 id : std::as_const(order)) {
      if (originalIds.contains(id)) survivingOrder.append(id);
      replaced |= originalIds.contains(id) && undo.replacedItemIds.contains(id);
    }
    if (locked || replaced || survivingOrder != change.afterOrder) {
      // Ordering is one change: retain the later order, but still undo settings.
      keptChanges = true;
      continue;
    }
    int originalIndex = 0;
    for (auto &id : order) {
      if (originalIds.contains(id)) {
        id = change.beforeOrder.at(originalIndex++);
      }
    }
  }
  PlaylistModelSnapshot desired;
  desired.activePlaylistNumber = activePlaylistNumber_;
  for (const int number : desiredPlaylistOrder) {
    desired.playlists.append(desiredPlaylists.value(number));
    const auto order = desiredOrders.value(number);
    for (int row = 0; row < order.size(); ++row) {
      auto &item = desiredItems[order.at(row)];
      item.itemNumber = row + 1;
      desired.items.append(item);
    }
  }
  if (!canRestoreSnapshot(desired, errorDetail)) return false;

  struct ItemMove { quint64 id; int from; int to; };
  QList<ItemMove> moves;
  for (const auto &change : undo.playlists) {
    auto order = itemOrdersByPlaylist_.value(change.number);
    int restoredRow = 0;
    for (const auto &item : restoredItems) {
      if (item.playlistNumber == change.number) {
        order.insert(restoredRow++, item.id);
      }
    }
    const auto desiredOrder = desiredOrders.value(change.number);
    for (int row = 0; row < desiredOrder.size(); ++row) {
      const quint64 id = desiredOrder.at(row);
      const int from = order.indexOf(id);
      if (from != row) {
        order.move(from, row);
        moves.append({id, from, row});
      }
    }
  }

  // Commit before notifying observers. Do not reset the model: selection,
  // CUE/EDIT references, thumbnails and play counts belong to later operations.
  for (auto it = knownMediaDurationsById_.begin();
       it != knownMediaDurationsById_.end();) {
    const auto current = itemsById_.constFind(it.key());
    const auto desired = desiredItems.constFind(it.key());
    if (current == itemsById_.cend() || desired == desiredItems.cend() ||
        current->filePath != desired->filePath) {
      it = knownMediaDurationsById_.erase(it);
    } else {
      ++it;
    }
  }
  itemsById_ = std::move(desiredItems);
  itemOrdersByPlaylist_ = std::move(desiredOrders);
  playlistsByNumber_ = std::move(desiredPlaylists);
  playlistOrder_ = std::move(desiredPlaylistOrder);
  bulkUndo_.reset();
  for (const auto &item : restoredItems) {
    // Removed rows have no current availability result. Keep them unavailable
    // until the background file probe or live-source registration validates
    // them; a synchronous stat here can block Undo on an unresponsive share.
    mediaAvailability_->setOffline(item.id, true);
  }
  // Emit existing per-row notifications in their usual order so widgets can
  // update incrementally. All validation above is complete at this point.
  if (undo.removedPlaylist) emit playlistAdded(undo.removedPlaylist->number);
  for (const auto &item : restoredItems) emit itemAdded(item.id);
  for (const auto &move : moves) {
    emit itemMoved(move.id, move.from, move.to);
  }
  for (const auto id : changedSettings) {
    emit itemChanged(id);
    emit itemSettingsChanged(id, itemsById_.value(id).settings);
  }
  for (const auto number : changedModes) {
    emit playlistModeChanged(number, playlistsByNumber_.value(number).mode);
  }
  if (!restoredItems.isEmpty()) emit countChanged(itemCount());
  emit bulkUndoAvailableChanged(false);
  // itemAdded invalidates any older scan first, including reused IDs/paths.
  if (!restoredItems.isEmpty()) refreshMediaAvailability();
  if (keptLaterChanges) *keptLaterChanges = keptChanges;
  if (errorDetail) errorDetail->clear();
  return true;
}

bool PlaylistModel::removeItem(quint64 itemId) {
  const int previousRow = indexOf(itemId);
  const auto state = itemState(itemId);
  if (previousRow < 0 || !state || itemId == onAirItemId_ ||
      itemId == bgmItemId_) {
    return false;
  }
  QVector<quint64> &itemOrder =
      itemOrdersByPlaylist_[state->playlistNumber];
  itemOrder.removeAt(previousRow);
  itemsById_.remove(itemId);
  knownMediaDurationsById_.remove(itemId);
  mediaAvailability_->setOffline(itemId, false);
  conversionStatesById_.remove(itemId);
  currentPlayCountsById_.remove(itemId);
  updateItemNumbers(state->playlistNumber, previousRow);
  for (auto item = itemsById_.begin(); item != itemsById_.end(); ++item) {
    if (item->settings.gotoTargetItemId != itemId) {
      continue;
    }
    item->settings.gotoTargetItemId = 0;
    emit itemChanged(item->id);
    emit itemSettingsChanged(item->id, item->settings);
  }
  if (cueItemId_ == itemId) {
    clearCueItem();
  }
  if (dskItemId_ == itemId) {
    clearDskItem();
  }
  if (editItemId_ == itemId) {
    clearEditItem();
  }
  emit itemRemoved(itemId, previousRow);
  if (state->playlistNumber == activePlaylistNumber_) {
    emit countChanged(itemCount());
  }
  return true;
}

int PlaylistModel::itemCount() const {
  return itemCount(activePlaylistNumber_);
}

int PlaylistModel::itemCount(int playlistNumber) const {
  return itemOrdersByPlaylist_.value(playlistNumber).size();
}

quint64 PlaylistModel::itemIdAt(int row) const {
  return itemIdAt(activePlaylistNumber_, row);
}

quint64 PlaylistModel::itemIdAt(int playlistNumber, int row) const {
  const QVector<quint64> itemOrder =
      itemOrdersByPlaylist_.value(playlistNumber);
  return row >= 0 && row < itemOrder.size() ? itemOrder.at(row) : 0;
}

quint64 PlaylistModel::nextItemIdForPlayout(quint64 currentItemId,
                                            quint64 excludedItemId) const {
  return playSequence_->nextItemId(currentItemId, excludedItemId);
}

quint64 PlaylistModel::automaticEndTargetItemIdForPlayout(
    quint64 currentItemId, quint64 excludedItemId) const {
  return playSequence_->automaticEndTargetItemId(currentItemId, excludedItemId);
}

quint64 PlaylistModel::endTargetItemIdForPlayout(
    quint64 currentItemId, PlaylistEndMode endMode, quint64 excludedItemId) const {
  return playSequence_->endTargetItemId(currentItemId, endMode, excludedItemId);
}

PlaylistMapAnalysis PlaylistModel::analyzePlaySequence(
    quint64 startItemId,
    std::optional<PlaylistMapPlaybackProgress> playbackProgress,
    bool currentItemOnly) const {
  return playSequence_->analyze(startItemId, playbackProgress, currentItemOnly);
}

PlaylistTotalRemaining PlaylistModel::totalRemainingForPlayout(
    quint64 onAirItemId, PlaylistMapPlaybackProgress progress) const {
  return playSequence_->totalRemaining(onAirItemId, progress);
}

int PlaylistModel::indexOf(quint64 itemId) const {
  const auto state = itemState(itemId);
  return state ? itemOrdersByPlaylist_.value(state->playlistNumber)
                     .indexOf(itemId)
               : -1;
}

std::optional<PlaylistItemState>
PlaylistModel::itemState(quint64 itemId) const {
  const auto iterator = itemsById_.constFind(itemId);
  if (iterator == itemsById_.cend()) {
    return std::nullopt;
  }
  return iterator.value();
}

bool PlaylistModel::itemMediaOffline(quint64 itemId) const {
  return mediaAvailability_->isOffline(itemId);
}

void PlaylistModel::setUvcDeviceAvailable(const QString &devicePath, bool available) {
  QList<quint64> changed;
  for (const auto &item : std::as_const(itemsById_)) {
    if (item.sourceType != PlaylistSourceType::Uvc || item.filePath != devicePath ||
        itemMediaOffline(item.id) == !available) continue;
    if (available) mediaAvailability_->setOffline(item.id, false);
    else mediaAvailability_->setOffline(item.id, true);
    changed.append(item.id);
  }
  // Publish the entire device's state before observers may change ON AIR/CUE.
  for (const auto id : changed) emit itemMediaAvailabilityChanged(id, !available);
}

void PlaylistModel::refreshMediaAvailability() { mediaAvailability_->refresh(); }

std::optional<PlaylistItemConversionState>
PlaylistModel::itemConversionState(quint64 itemId) const {
  const auto conversion = conversionStatesById_.constFind(itemId);
  return conversion == conversionStatesById_.cend()
             ? std::nullopt
             : std::optional<PlaylistItemConversionState>{conversion.value()};
}

bool PlaylistModel::itemConversionInProgress(quint64 itemId) const {
  return conversionStatesById_.contains(itemId);
}

bool PlaylistModel::setItemConversionProgress(
    quint64 itemId, PlaylistItemConversionStage stage, int progressPermille) {
  auto conversion = conversionStatesById_.find(itemId);
  if (conversion == conversionStatesById_.end()) {
    return false;
  }
  const PlaylistItemConversionState updated{
      stage, std::clamp(progressPermille, 0, 1000)};
  if (conversion.value() == updated) {
    return true;
  }
  conversion.value() = updated;
  emit itemConversionStateChanged(itemId);
  return true;
}

int PlaylistModel::itemCurrentPlayCount(quint64 itemId) const {
  return currentPlayCountsById_.value(itemId, 0);
}

void PlaylistModel::setItemCurrentPlayCount(quint64 itemId,
                                            int currentPlayCount) {
  if (!itemsById_.contains(itemId) || currentPlayCount < 0 ||
      currentPlayCount > kMaximumItemRepeatCount ||
      itemCurrentPlayCount(itemId) == currentPlayCount) {
    return;
  }
  if (currentPlayCount == 0) {
    currentPlayCountsById_.remove(itemId);
  } else {
    currentPlayCountsById_.insert(itemId, currentPlayCount);
  }
  emit itemChanged(itemId);
}

bool PlaylistModel::setItemDisplayName(quint64 itemId,
                                       const QString &displayName) {
  auto item = itemsById_.find(itemId);
  const QString trimmedName = displayName.trimmed();
  if (item == itemsById_.end() || trimmedName.isEmpty()) {
    return false;
  }
  if (item->displayName == trimmedName) {
    return true;
  }
  item->displayName = trimmedName;
  emit itemChanged(itemId);
  return true;
}

bool PlaylistModel::setItemColor(quint64 itemId, PlaylistItemColor color) {
  auto item = itemsById_.find(itemId);
  if (item == itemsById_.end() || !validItemColor(color)) {
    return false;
  }
  if (item->color == color) {
    return true;
  }
  item->color = color;
  emit itemChanged(itemId);
  return true;
}

bool PlaylistModel::setItemSeparatorBelow(quint64 itemId,
                                          bool separatorBelow) {
  auto item = itemsById_.find(itemId);
  if (item == itemsById_.end()) {
    return false;
  }
  if (item->separatorBelow == separatorBelow) {
    return true;
  }
  item->separatorBelow = separatorBelow;
  emit itemChanged(itemId);
  return true;
}

void PlaylistModel::updateItemNumbers(int playlistNumber, int firstRow,
                                      int lastRow) {
  const QVector<quint64> &itemOrder =
      itemOrdersByPlaylist_[playlistNumber];
  if (itemOrder.isEmpty()) {
    return;
  }
  const int itemCount = static_cast<int>(itemOrder.size());
  firstRow = std::clamp(firstRow, 0, itemCount - 1);
  if (lastRow < 0) {
    lastRow = itemCount - 1;
  } else {
    lastRow = std::clamp(lastRow, firstRow, itemCount - 1);
  }

  for (int row = firstRow; row <= lastRow; ++row) {
    auto item = itemsById_.find(itemOrder.at(row));
    if (item != itemsById_.end()) {
      item->itemNumber = row + 1;
    }
  }
}

bool PlaylistModel::updateItemSettings(quint64 itemId,
                                       const PlaylistItemSettings &settings) {
  if (!validItemSettings(settings) ||
      (settings.gotoTargetItemId != 0 &&
       !itemsById_.contains(settings.gotoTargetItemId))) {
    return false;
  }
  auto iterator = itemsById_.find(itemId);
  if (iterator == itemsById_.end() || itemConversionInProgress(itemId) ||
      itemSettingsLocked(itemId) ||
      !itemSettingsWithinKnownDuration(itemId, settings)) {
    return false;
  }
  if (iterator->sourceType == PlaylistSourceType::Uvc) {
    const auto &old = iterator->settings;
    if (settings.startMode != PlaylistStartMode::Play ||
        settings.inPointMs != old.inPointMs || settings.outPointMs != old.outPointMs ||
        settings.nominalAudioDelayFrames != old.nominalAudioDelayFrames ||
        settings.endMode != old.endMode || settings.fadeOutEnabled != old.fadeOutEnabled ||
        settings.fadeOutDurationMs != old.fadeOutDurationMs ||
        settings.endCrossfadeEnabled != old.endCrossfadeEnabled ||
        settings.endCrossfadeDurationMs != old.endCrossfadeDurationMs ||
        settings.gotoTargetItemId != old.gotoTargetItemId ||
        settings.repeatCount != old.repeatCount ||
        settings.repeatEndMode != old.repeatEndMode) return false;
  } else if (iterator->sourceType != PlaylistSourceType::File) {
    return false;
  }
  if (itemEditPointsLocked(itemId) &&
      (iterator->settings.inPointMs != settings.inPointMs ||
       iterator->settings.outPointMs != settings.outPointMs)) {
    return false;
  }
  if (iterator->settings == settings) {
    return true;
  }
  iterator->settings = settings;
  emit itemChanged(itemId);
  emit itemSettingsChanged(itemId, settings);
  return true;
}

void PlaylistModel::setKnownMediaDuration(quint64 itemId, qint64 durationMs) {
  const auto item = itemsById_.constFind(itemId);
  if (item == itemsById_.cend()) return;
  if (item->sourceType != PlaylistSourceType::File ||
      item->mediaType == PlaylistMediaType::StillImage) {
    knownMediaDurationsById_.remove(itemId);
    return;
  }
  if (durationMs > 0) {
    if (knownMediaDurationsById_.value(itemId) != durationMs)
      knownMediaDurationsById_.insert(itemId, durationMs);
  } else {
    knownMediaDurationsById_.remove(itemId);
  }
}

bool PlaylistModel::itemSettingsWithinKnownDuration(
    quint64 itemId, const PlaylistItemSettings &settings) const {
  if (!itemsById_.contains(itemId)) return false;
  const auto known = knownMediaDurationsById_.constFind(itemId);
  return known == knownMediaDurationsById_.cend() ||
         (settings.inPointMs < *known &&
          (!settings.outPointMs || *settings.outPointMs <= *known));
}

bool PlaylistModel::itemEditPointsLocked(quint64 itemId) const {
  const auto state = itemState(itemId);
  return itemId != 0 &&
         (itemId == onAirItemId_ || itemId == bgmItemId_ ||
          itemSettingsLocked(itemId) ||
          (state && state->sourceType != PlaylistSourceType::File));
}

bool PlaylistModel::itemSettingsLocked(quint64 itemId) const {
  const auto state = itemState(itemId);
  return itemId != 0 &&
         (settingsLockedItemIds_.contains(itemId) ||
          (state && state->sourceType != PlaylistSourceType::File &&
           state->sourceType != PlaylistSourceType::Uvc));
}

void PlaylistModel::setItemSettingsLocked(quint64 itemId, bool locked) {
  if (itemId == 0 || !itemsById_.contains(itemId) ||
      settingsLockedItemIds_.contains(itemId) == locked) {
    return;
  }
  if (locked) {
    settingsLockedItemIds_.insert(itemId);
  } else {
    settingsLockedItemIds_.remove(itemId);
  }
  emit itemSettingsLockChanged(itemId, locked);
}

quint64 PlaylistModel::onAirItemId() const { return onAirItemId_; }

QString PlaylistModel::onAirErrorDetail() const { return onAirErrorDetail_; }

void PlaylistModel::setOnAirItemId(quint64 itemId) {
  if ((itemId != 0 &&
       (!itemsById_.contains(itemId) || itemConversionInProgress(itemId))) ||
      (onAirItemId_ == itemId && onAirErrorDetail_.isEmpty())) {
    return;
  }
  const quint64 previousItemId = onAirItemId_;
  if (previousItemId != 0 && previousItemId != itemId) {
    setItemCurrentPlayCount(previousItemId, 0);
  }
  onAirItemId_ = itemId;
  onAirErrorDetail_.clear();
  emit onAirStateChanged(previousItemId, onAirItemId_);
}

void PlaylistModel::clearOnAirItem() { setOnAirItemId(0); }

void PlaylistModel::setOnAirError(const QString &detail) {
  if (onAirErrorDetail_ == detail) {
    return;
  }
  const quint64 itemId = onAirItemId_;
  onAirErrorDetail_ = detail;
  emit onAirStateChanged(itemId, itemId);
}

quint64 PlaylistModel::cueItemId() const { return cueItemId_; }

PlaylistCueState PlaylistModel::cueState() const { return cueState_; }

QString PlaylistModel::cueDetail() const { return cueDetail_; }

void PlaylistModel::setCueItemState(quint64 itemId, PlaylistCueState state,
                                    const QString &detail) {
  if ((itemId != 0 &&
       (!itemsById_.contains(itemId) || itemConversionInProgress(itemId))) ||
      (cueItemId_ == itemId && cueState_ == state && cueDetail_ == detail)) {
    return;
  }
  const quint64 previousItemId = cueItemId_;
  cueItemId_ = itemId;
  cueState_ = state;
  cueDetail_ = detail;
  emit cueStateChanged(previousItemId, cueItemId_);
}

void PlaylistModel::clearCueItem() {
  setCueItemState(0, PlaylistCueState::None);
}

quint64 PlaylistModel::dskItemId() const { return dskItemId_; }

void PlaylistModel::setDskItemId(quint64 itemId) {
  if ((itemId != 0 &&
       (!itemsById_.contains(itemId) || itemConversionInProgress(itemId))) ||
      dskItemId_ == itemId) {
    return;
  }
  const quint64 previousItemId = dskItemId_;
  dskItemId_ = itemId;
  emit dskStateChanged(previousItemId, dskItemId_);
}

void PlaylistModel::clearDskItem() { setDskItemId(0); }

quint64 PlaylistModel::editItemId() const { return editItemId_; }

void PlaylistModel::setEditItemId(quint64 itemId) {
  if ((itemId != 0 &&
       (!itemsById_.contains(itemId) || itemConversionInProgress(itemId))) ||
      editItemId_ == itemId) {
    return;
  }
  const quint64 previousItemId = editItemId_;
  editItemId_ = itemId;
  emit editStateChanged(previousItemId, editItemId_);
}

void PlaylistModel::clearEditItem() { setEditItemId(0); }

quint64 PlaylistModel::bgmItemId() const { return bgmItemId_; }

void PlaylistModel::setBgmItemId(quint64 itemId) {
  if ((itemId != 0 && !itemsById_.contains(itemId)) || bgmItemId_ == itemId) {
    return;
  }
  const quint64 previousItemId = bgmItemId_;
  bgmItemId_ = itemId;
  emit bgmStateChanged(previousItemId, bgmItemId_);
}

void PlaylistModel::clearBgmItem() { setBgmItemId(0); }

QList<PlaylistState> PlaylistModel::playlists() const {
  QList<PlaylistState> result;
  result.reserve(playlistOrder_.size());
  for (const int playlistNumber : playlistOrder_) {
    result.append(playlistsByNumber_.value(playlistNumber));
  }
  return result;
}

bool PlaylistModel::movePlaylist(int playlistNumber, int destinationIndex) {
  const int sourceIndex = playlistOrder_.indexOf(playlistNumber);
  if (sourceIndex < 0 || destinationIndex < 0 || destinationIndex >= playlistOrder_.size()) {
    return false;
  }
  if (sourceIndex != destinationIndex) {
    playlistOrder_.move(sourceIndex, destinationIndex);
    emit playlistOrderChanged();
  }
  return true;
}

int PlaylistModel::createPlaylist() {
  if (playlistOrder_.isEmpty()) {
    return 0;
  }
  int largestNumber =
      *std::max_element(playlistOrder_.cbegin(), playlistOrder_.cend());
  // Keep the deleted number free until the operation can no longer be undone.
  if (bulkUndo_ && bulkUndo_->removedPlaylist) {
    largestNumber = std::max(largestNumber, bulkUndo_->removedPlaylist->number);
  }
  if (largestNumber == (std::numeric_limits<int>::max)()) {
    return 0;
  }
  const int playlistNumber = largestNumber + 1;
  PlaylistState state;
  state.number = playlistNumber;
  playlistsByNumber_.insert(playlistNumber, state);
  playlistOrder_.append(playlistNumber);
  itemOrdersByPlaylist_.insert(playlistNumber, {});
  emit playlistAdded(playlistNumber);
  return playlistNumber;
}

bool PlaylistModel::removeEmptyPlaylist(int playlistNumber) {
  if (playlistOrder_.size() <= 1 || !playlistsByNumber_.contains(playlistNumber)
      || itemCount(playlistNumber) != 0) {
    return false;
  }
  const int removedIndex = playlistOrder_.indexOf(playlistNumber);
  playlistOrder_.removeAt(removedIndex);
  playlistsByNumber_.remove(playlistNumber);
  itemOrdersByPlaylist_.remove(playlistNumber);
  if (activePlaylistNumber_ == playlistNumber) {
    setActivePlaylistNumber(playlistOrder_.at(
        std::min(removedIndex, int(playlistOrder_.size()) - 1)));
  }
  emit playlistRemoved(playlistNumber);
  return true;
}

int PlaylistModel::activePlaylistNumber() const {
  return activePlaylistNumber_;
}

bool PlaylistModel::setActivePlaylistNumber(int playlistNumber) {
  if (!playlistsByNumber_.contains(playlistNumber)) {
    return false;
  }
  if (activePlaylistNumber_ == playlistNumber) {
    return true;
  }
  const int previousPlaylistNumber = activePlaylistNumber_;
  activePlaylistNumber_ = playlistNumber;
  emit activePlaylistChanged(previousPlaylistNumber, activePlaylistNumber_);
  emit countChanged(itemCount());
  return true;
}

QString PlaylistModel::playlistName(int playlistNumber) const {
  return playlistsByNumber_.value(playlistNumber).name;
}

bool PlaylistModel::setPlaylistName(int playlistNumber, const QString &name) {
  auto playlist = playlistsByNumber_.find(playlistNumber);
  if (playlist == playlistsByNumber_.end()) {
    return false;
  }
  if (playlist->name == name) {
    return true;
  }
  playlist->name = name;
  emit playlistNameChanged(playlistNumber, name);
  return true;
}

PlaylistMode PlaylistModel::playlistMode(int playlistNumber) const {
  return playlistsByNumber_.value(playlistNumber).mode;
}

bool PlaylistModel::setPlaylistMode(int playlistNumber, PlaylistMode mode) {
  if (mode != PlaylistMode::Standard && mode != PlaylistMode::Bgm) {
    return false;
  }
  auto playlist = playlistsByNumber_.find(playlistNumber);
  if (playlist == playlistsByNumber_.end()) {
    return false;
  }
  if (playlist->mode == mode) {
    return true;
  }
  playlist->mode = mode;
  emit playlistModeChanged(playlistNumber, mode);
  return true;
}

bool PlaylistModel::manualCrossfadeEnabled(int playlistNumber) const {
  return playlistsByNumber_.value(playlistNumber).manualCrossfadeEnabled;
}

void PlaylistModel::setManualCrossfadeEnabled(int playlistNumber, bool enabled) {
  auto it = playlistsByNumber_.find(playlistNumber);
  if (it != playlistsByNumber_.end()) it->manualCrossfadeEnabled = enabled;
}

qint64 PlaylistModel::manualCrossfadeDurationMs() const {
  return manualCrossfadeDurationMs_;
}

void PlaylistModel::setManualCrossfadeDurationMs(qint64 durationMs) {
  manualCrossfadeDurationMs_ = std::clamp(durationMs,
      kMinimumItemFadeDurationMs, kMaximumItemFadeDurationMs);
}

PlaylistModelSnapshot PlaylistModel::snapshot() const {
  PlaylistModelSnapshot result;
  result.documentId = documentId_;
  result.macros = macros_;
  result.nextItemId = nextItemId_;
  result.playlists = playlists();
  result.activePlaylistNumber = activePlaylistNumber_;
  result.manualCrossfadeDurationMs = manualCrossfadeDurationMs_;
  for (const int playlistNumber : playlistOrder_) {
    const QVector<quint64> &itemOrder =
        itemOrdersByPlaylist_[playlistNumber];
    for (const quint64 itemId : itemOrder) {
      const auto item = itemState(itemId);
      if (item) {
        result.items.append(*item);
      }
    }
  }
  return result;
}

QString PlaylistModel::documentId() const { return documentId_; }

QList<MacroDefinition> PlaylistModel::macros() const { return macros_; }

bool PlaylistModel::setMacros(const QList<MacroDefinition> &macros,
                              QString *errorDetail) {
  if (!MacroDocumentIo::validateDefinitions(macros, errorDetail)) return false;
  quint64 reservedNextId = nextItemId_;
  if (!reserveMacroItemIds(macros, documentId_, &reservedNextId, errorDetail)) return false;
  if (macros_ == macros) return true;
  nextItemId_ = reservedNextId;
  macros_ = macros;
  emit macrosChanged();
  return true;
}

QStringList PlaylistModel::macroReferenceErrors() const {
  QSet<quint64> itemIds;
  for (auto item = itemsById_.constBegin(); item != itemsById_.constEnd(); ++item)
    itemIds.insert(item.key());
  return MacroDocumentIo::referenceErrors(macros_, documentId_, itemIds);
}

std::optional<quint64> PlaylistModel::nextItemIdForSnapshot(
    const PlaylistModelSnapshot &snapshot, QString *errorDetail) {
  if (snapshot.nextItemId == 0) {
    setError(errorDetail, QStringLiteral("The next playlist item ID must be positive."));
    return std::nullopt;
  }
  quint64 nextId = snapshot.nextItemId;
  for (const auto &item : snapshot.items)
    if (!reserveItemId(item.id, &nextId, errorDetail)) return std::nullopt;
  if (!reserveMacroItemIds(snapshot.macros, snapshot.documentId, &nextId, errorDetail))
    return std::nullopt;
  if (errorDetail) errorDetail->clear();
  return nextId;
}

bool PlaylistModel::canRestoreSnapshot(
    const PlaylistModelSnapshot &snapshot, QString *errorDetail) {
  if (QUuid(snapshot.documentId).isNull()) {
    setError(errorDetail, QStringLiteral("Invalid playlist document ID."));
    return false;
  }
  if (!MacroDocumentIo::validateDefinitions(snapshot.macros, errorDetail)) return false;
  if (!nextItemIdForSnapshot(snapshot, errorDetail)) return false;
  if (snapshot.manualCrossfadeDurationMs < kMinimumItemFadeDurationMs ||
      snapshot.manualCrossfadeDurationMs > kMaximumItemFadeDurationMs) {
    setError(errorDetail, QStringLiteral("Invalid manual crossfade duration."));
    return false;
  }
  if (snapshot.playlists.isEmpty()) {
    setError(errorDetail,
             QStringLiteral("The playlist document contains no playlists."));
    return false;
  }

  QSet<int> playlistNumbers;
  for (const PlaylistState &playlist : snapshot.playlists) {
    if (playlist.number <= 0) {
      setError(errorDetail,
               QStringLiteral("Playlist numbers must be positive."));
      return false;
    }
    if (playlistNumbers.contains(playlist.number)) {
      setError(errorDetail,
               QStringLiteral("Playlist number %1 is duplicated.")
                   .arg(playlist.number));
      return false;
    }
    if (playlist.mode != PlaylistMode::Standard &&
        playlist.mode != PlaylistMode::Bgm) {
      setError(errorDetail,
               QStringLiteral("Playlist %1 has an invalid mode.")
                   .arg(playlist.number));
      return false;
    }
    playlistNumbers.insert(playlist.number);
  }
  if (!playlistNumbers.contains(snapshot.activePlaylistNumber)) {
    setError(errorDetail,
             QStringLiteral("The active playlist number does not exist."));
    return false;
  }

  QHash<int, int> itemCounts;
  QSet<quint64> itemIds;
  quint64 largestItemId = 0;
  for (const PlaylistItemState &item : snapshot.items) {
    if (item.id == 0 || itemIds.contains(item.id)) {
      setError(errorDetail,
               item.id == 0
                   ? QStringLiteral("Playlist item ID 0 is invalid.")
                   : QStringLiteral("Playlist item ID %1 is duplicated.")
                         .arg(item.id));
      return false;
    }
    if (!playlistNumbers.contains(item.playlistNumber)) {
      setError(errorDetail,
               QStringLiteral("Item ID %1 refers to missing playlist %2.")
                   .arg(item.id)
                   .arg(item.playlistNumber));
      return false;
    }
    const int expectedItemNumber = itemCounts.value(item.playlistNumber) + 1;
    if (item.itemNumber != expectedItemNumber) {
      setError(errorDetail,
               QStringLiteral("Playlist %1 has an invalid item order at ID %2.")
                   .arg(item.playlistNumber)
                   .arg(item.id));
      return false;
    }
    if (!validItemSettings(item.settings)) {
      setError(errorDetail,
               QStringLiteral("Item ID %1 contains invalid settings.")
                   .arg(item.id));
      return false;
    }
    if (!validItemColor(item.color)) {
      setError(errorDetail,
               QStringLiteral("Item ID %1 has an invalid display color.")
                   .arg(item.id));
      return false;
    }
    if (!validPowerPointMode(item.powerPointMode, item.sourceType,
                             item.mediaType)) {
      setError(errorDetail,
               QStringLiteral("Item ID %1 has an invalid PowerPoint mode.")
                   .arg(item.id));
      return false;
    }

    switch (item.mediaType) {
    case PlaylistMediaType::Unknown:
    case PlaylistMediaType::Video:
    case PlaylistMediaType::Audio:
    case PlaylistMediaType::StillImage:
    case PlaylistMediaType::Presentation:
    case PlaylistMediaType::Pdf:
      break;
    default:
      setError(errorDetail,
               QStringLiteral("Item ID %1 has an invalid media type.")
                   .arg(item.id));
      return false;
    }

    if (item.sourceType == PlaylistSourceType::File ||
        item.sourceType == PlaylistSourceType::PowerPoint ||
        item.sourceType == PlaylistSourceType::Pdf) {
      if (item.filePath.trimmed().isEmpty()) {
        setError(errorDetail,
                 QStringLiteral("File item ID %1 has no media file path.")
                     .arg(item.id));
        return false;
      }
    } else if (item.sourceType == PlaylistSourceType::Uvc) {
      if (item.filePath.isEmpty() || item.displayName.isEmpty()) {
        setError(errorDetail,
                 QStringLiteral("UVC item ID %1 has no device path or display name.")
                     .arg(item.id));
        return false;
      }
    } else {
      setError(errorDetail,
               QStringLiteral("Item ID %1 has an invalid source type.")
                   .arg(item.id));
      return false;
    }

    itemCounts[item.playlistNumber] = expectedItemNumber;
    itemIds.insert(item.id);
    largestItemId = (std::max)(largestItemId, item.id);
  }
  if (largestItemId == (std::numeric_limits<quint64>::max)()) {
    setError(errorDetail,
             QStringLiteral("The playlist item ID range is exhausted."));
    return false;
  }
  for (const PlaylistItemState &item : snapshot.items) {
    if (item.settings.gotoTargetItemId != 0 &&
        !itemIds.contains(item.settings.gotoTargetItemId)) {
      setError(errorDetail,
               QStringLiteral("Item ID %1 refers to missing GOTO target %2.")
                   .arg(item.id)
                   .arg(item.settings.gotoTargetItemId));
      return false;
    }
  }
  if (errorDetail) {
    errorDetail->clear();
  }
  return true;
}

bool PlaylistModel::restoreSnapshot(const PlaylistModelSnapshot &snapshot,
                                    QString *errorDetail, const QSet<quint64> *knownOfflineItems) {
  if (onAirItemId_ != 0 || cueItemId_ != 0 || dskItemId_ != 0 ||
      editItemId_ != 0 || bgmItemId_ != 0 ||
      !settingsLockedItemIds_.isEmpty() ||
      !conversionStatesById_.isEmpty()) {
    setError(errorDetail,
             QStringLiteral("Playback references must be released before importing a playlist."));
    return false;
  }
  if (!canRestoreSnapshot(snapshot, errorDetail)) {
    return false;
  }

  QHash<quint64, PlaylistItemState> restoredItems;
  QSet<quint64> restoredOfflineItemIds;
  QHash<int, QVector<quint64>> restoredOrders;
  QHash<int, PlaylistState> restoredPlaylists;
  QVector<int> restoredPlaylistOrder;

  for (const PlaylistState &playlist : snapshot.playlists) {
    restoredPlaylists.insert(playlist.number, playlist);
    restoredPlaylistOrder.append(playlist.number);
    restoredOrders.insert(playlist.number, {});
  }
  for (PlaylistItemState item : snapshot.items) {
    if (item.sourceType == PlaylistSourceType::Uvc)
      item.settings.startMode = PlaylistStartMode::Play;
    if (item.sourceType == PlaylistSourceType::File ||
        item.sourceType == PlaylistSourceType::PowerPoint ||
        item.sourceType == PlaylistSourceType::Pdf) {
      const QFileInfo fileInfo(item.filePath);
      item.filePath = fileInfo.absoluteFilePath();
      if (item.displayName.isEmpty()) {
        item.displayName = fileInfo.fileName();
      }
      if (knownOfflineItems ? knownOfflineItems->contains(item.id) : !mediaFileAvailable(item)) {
        restoredOfflineItemIds.insert(item.id);
      }
    }
    restoredItems.insert(item.id, item);
    restoredOrders[item.playlistNumber].append(item.id);
  }

  itemsById_ = std::move(restoredItems);
  knownMediaDurationsById_.clear();
  documentId_ = snapshot.documentId;
  macros_ = snapshot.macros;
  mediaAvailability_->replaceOffline(std::move(restoredOfflineItemIds));
  conversionStatesById_.clear();
  currentPlayCountsById_.clear();
  itemOrdersByPlaylist_ = std::move(restoredOrders);
  playlistsByNumber_ = std::move(restoredPlaylists);
  playlistOrder_ = std::move(restoredPlaylistOrder);
  nextItemId_ = *nextItemIdForSnapshot(snapshot);
  activePlaylistNumber_ = snapshot.activePlaylistNumber;
  manualCrossfadeDurationMs_ = snapshot.manualCrossfadeDurationMs;
  onAirItemId_ = 0;
  onAirErrorDetail_.clear();
  cueItemId_ = 0;
  cueState_ = PlaylistCueState::None;
  cueDetail_.clear();
  dskItemId_ = 0;
  editItemId_ = 0;
  bgmItemId_ = 0;
  settingsLockedItemIds_.clear();
  bulkUndo_.reset();
  emit bulkUndoAvailableChanged(false);
  emit modelReset();
  emit macrosChanged();
  if (errorDetail) {
    errorDetail->clear();
  }
  return true;
}

void PlaylistModel::requestShutdown() { mediaAvailability_->requestShutdown(); }
bool PlaylistModel::shutdownComplete() const {
  return mediaAvailability_->shutdownComplete();
}
