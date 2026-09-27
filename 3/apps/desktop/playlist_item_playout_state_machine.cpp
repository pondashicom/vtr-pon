#include "playlist_item_playout_state_machine.h"

#include "playlist_model.h"

#include <algorithm>

namespace {

PlaylistEndMode endModeAfterRepeat(PlaylistRepeatEndMode mode) {
  switch (mode) {
  case PlaylistRepeatEndMode::Pause:
    return PlaylistEndMode::Pause;
  case PlaylistRepeatEndMode::Stop:
    return PlaylistEndMode::Stop;
  case PlaylistRepeatEndMode::Next:
    return PlaylistEndMode::Next;
  case PlaylistRepeatEndMode::Goto:
    return PlaylistEndMode::Goto;
  }
  return PlaylistEndMode::Pause;
}

} // namespace

PlaylistItemPlayoutStateMachine::PlaylistItemPlayoutStateMachine(
    PlaylistModel *model)
    : model_(model) {}

bool PlaylistItemPlayoutStateMachine::prepare(quint64 itemId) {
  if (!model_ || itemId == 0) {
    preparedMedia_.reset();
    markMissingItem();
    return false;
  }
  const auto item = model_->itemState(itemId);
  if (!item || item->filePath.isEmpty()) {
    preparedMedia_.reset();
    markMissingItem();
    return false;
  }

  preparedMedia_ = PlaylistPreparedMedia{
      item->id,
      item->filePath,
      item->settings.inPointMs,
      item->settings.outPointMs,
  };
  completedPlayCount_ = 0;
  phase_ = PlaylistItemPlayoutPhase::Prepared;
  return true;
}

std::optional<PlaylistStartMode> PlaylistItemPlayoutStateMachine::start() {
  if (phase_ != PlaylistItemPlayoutPhase::Prepared) {
    return std::nullopt;
  }
  const auto item = currentItem();
  if (!item) {
    markMissingItem();
    return std::nullopt;
  }
  phase_ = PlaylistItemPlayoutPhase::Active;
  return item->settings.startMode;
}

std::optional<PlaylistEndMode>
PlaylistItemPlayoutStateMachine::endModeForCurrentIteration() const {
  if (phase_ != PlaylistItemPlayoutPhase::Active) {
    return std::nullopt;
  }
  const auto item = currentItem();
  if (!item) {
    return std::nullopt;
  }
  if (item->settings.endMode != PlaylistEndMode::Repeat) {
    return item->settings.endMode;
  }
  if (item->settings.repeatCount == kInfiniteItemRepeatCount ||
      completedPlayCount_ + 1 < item->settings.repeatCount) {
    return PlaylistEndMode::Repeat;
  }
  return endModeAfterRepeat(item->settings.repeatEndMode);
}

std::optional<PlaylistEndMode> PlaylistItemPlayoutStateMachine::finish() {
  if (phase_ != PlaylistItemPlayoutPhase::Active) {
    return std::nullopt;
  }
  const auto item = currentItem();
  if (!item) {
    markMissingItem();
    return std::nullopt;
  }
  const auto endMode = endModeForCurrentIteration();
  if (!endMode) {
    return std::nullopt;
  }
  if (item->settings.endMode == PlaylistEndMode::Repeat &&
      completedPlayCount_ < kMaximumItemRepeatCount) {
    ++completedPlayCount_;
  }
  phase_ = *endMode == PlaylistEndMode::Repeat
               ? PlaylistItemPlayoutPhase::Active
               : PlaylistItemPlayoutPhase::Ended;
  return endMode;
}

bool PlaylistItemPlayoutStateMachine::markActive() {
  if (!preparedMedia_ || phase_ == PlaylistItemPlayoutPhase::Empty ||
      phase_ == PlaylistItemPlayoutPhase::MissingItem) {
    return false;
  }
  if (phase_ == PlaylistItemPlayoutPhase::Ended) {
    completedPlayCount_ = 0;
  }
  phase_ = PlaylistItemPlayoutPhase::Active;
  return true;
}

bool PlaylistItemPlayoutStateMachine::markEnded() {
  if (!preparedMedia_ || phase_ == PlaylistItemPlayoutPhase::Empty ||
      phase_ == PlaylistItemPlayoutPhase::MissingItem) {
    return false;
  }
  phase_ = PlaylistItemPlayoutPhase::Ended;
  return true;
}

bool PlaylistItemPlayoutStateMachine::deactivate() {
  if (!preparedMedia_ || phase_ == PlaylistItemPlayoutPhase::Empty ||
      phase_ == PlaylistItemPlayoutPhase::MissingItem) {
    return false;
  }
  phase_ = PlaylistItemPlayoutPhase::Prepared;
  return true;
}

void PlaylistItemPlayoutStateMachine::fail() {
  if (preparedMedia_) {
    phase_ = PlaylistItemPlayoutPhase::Error;
  }
}

void PlaylistItemPlayoutStateMachine::reset() {
  preparedMedia_.reset();
  completedPlayCount_ = 0;
  phase_ = PlaylistItemPlayoutPhase::Empty;
}

PlaylistItemPlayoutPhase PlaylistItemPlayoutStateMachine::phase() const {
  return phase_;
}

const PlaylistPreparedMedia *
PlaylistItemPlayoutStateMachine::preparedMedia() const {
  return preparedMedia_ ? &*preparedMedia_ : nullptr;
}

int PlaylistItemPlayoutStateMachine::currentPlayCount() const {
  if (!preparedMedia_ || phase_ == PlaylistItemPlayoutPhase::Empty ||
      phase_ == PlaylistItemPlayoutPhase::Prepared ||
      phase_ == PlaylistItemPlayoutPhase::MissingItem) {
    return 0;
  }
  if (phase_ == PlaylistItemPlayoutPhase::Active) {
    return (std::min)(completedPlayCount_ + 1,
                      kMaximumItemRepeatCount);
  }
  return completedPlayCount_;
}

std::optional<PlaylistItemState>
PlaylistItemPlayoutStateMachine::currentItem() const {
  if (!model_ || !preparedMedia_) {
    return std::nullopt;
  }
  return model_->itemState(preparedMedia_->itemId);
}

void PlaylistItemPlayoutStateMachine::markMissingItem() {
  phase_ = PlaylistItemPlayoutPhase::MissingItem;
}
