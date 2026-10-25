#include "bgm_playout_controller.h"
#include <QDebug>

#include "dual_deck_player.h"
#include "playlist_model.h"
#include "playlist_panel.h"
#include "program_audio_controller.h"
#include "program_panel.h"
#include "user_info.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSignalHoldMs = 250;
constexpr int kClipHoldMs = 500;
constexpr int kTransportRampMs = 150;
constexpr double kSignalFloorDb = -60.0;
constexpr qreal kSilenceGainEpsilon = 0.0001;

double nominalGain(int volumeTenthsDb) {
  if (volumeTenthsDb <= kMinimumItemVolumeTenthsDb) {
    return 0.0;
  }
  return std::pow(10.0, static_cast<double>(volumeTenthsDb) / 200.0);
}

} // namespace

BgmPlayoutController::BgmPlayoutController(
    DualDeckPlayer *player, ProgramPanel *programPanel,
    PlaylistPanel *playlistPanel, PlaylistModel *playlistModel,
    ProgramAudioController *audioController, UserInfoCenter *userInfoCenter,
    QObject *parent)
    : QObject(parent), player_(player), programPanel_(programPanel),
      playlistPanel_(playlistPanel), playlistModel_(playlistModel),
      audioController_(audioController), userInfoCenter_(userInfoCenter),
      stateMachine_(playlistModel) {
  auto *operations = programPanel->operations();
  using A = ControllerOperations::Action;
  operations->bindMember(A::BgmStop, this, &BgmPlayoutController::stop);
  operations->connectInput(programPanel, &ProgramPanel::bgmStopRequested, A::BgmStop);
  operations->bindMember(A::BgmPlay, this, &BgmPlayoutController::play);
  operations->connectInput(programPanel, &ProgramPanel::bgmPlayRequested, A::BgmPlay);
  operations->bindMember(A::BgmPause, this, &BgmPlayoutController::pause);
  operations->connectInput(programPanel, &ProgramPanel::bgmPauseRequested, A::BgmPause);
  operations->bindMember(A::BgmFadeIn, this, &BgmPlayoutController::fadeIn);
  operations->setGuard<qint64>(A::BgmFadeIn, this,
      [this](qint64) { return canManualFade(true); });
  operations->bindMember(A::BgmFadeOut, this, &BgmPlayoutController::fadeOut);
  operations->setGuard<qint64>(A::BgmFadeOut, this,
      [this](qint64) { return canManualFade(false); });
  operations->setGuard<>(A::BgmFadeInConfigured, this, [this] {
    return programPanel_ && programPanel_->configuredBgmFadeAvailable()
        && canManualFade(true);
  });
  operations->setGuard<>(A::BgmFadeOutConfigured, this, [this] {
    return programPanel_ && programPanel_->configuredBgmFadeAvailable()
        && canManualFade(false);
  });
  operations->bind<>(A::BgmFadeInConfigured, this, [this] {
    fadeIn(programPanel_->configuredBgmFadeInDurationMs());
  });
  operations->bind<>(A::BgmFadeOutConfigured, this, [this] {
    fadeOut(programPanel_->configuredBgmFadeOutDurationMs());
  });
  manualFadeAnimation_.setEasingCurve(QEasingCurve::Linear);
  itemFadeAnimation_.setEasingCurve(QEasingCurve::Linear);
  transportFadeAnimation_.setEasingCurve(QEasingCurve::InOutSine);
  connect(&manualFadeAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            manualFadeGain_ = std::clamp(value.toReal(), qreal{0.0},
                                         qreal{2.0});
            if (programPanel_) programPanel_->setBgmFaderGain(manualFadeGain_);
            if (player_) {
              player_->setBgmGain(manualFadeGain_);
            }
          });
  connect(&manualFadeAnimation_, &QVariantAnimation::finished, this, [this] {
    if (programPanel_) {
      programPanel_->setBgmFadePresentation(
          ProgramPanel::BgmFadeState::Idle,
          manualFadeGain_ <= kSilenceGainEpsilon);
    }
  });
  connect(&itemFadeAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            itemFadeGain_ = std::clamp(value.toReal(), qreal{0.0},
                                       qreal{1.0});
            applyItemVolume();
          });
  connect(&transportFadeAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            setTransportGain(value.toReal());
          });
  connect(&transportFadeAnimation_, &QVariantAnimation::finished, this,
          [this] {
            const PendingTransportAction action = pendingTransportAction_;
            pendingTransportAction_ = PendingTransportAction::None;
            if (action == PendingTransportAction::Pause) {
              const bool paused = player_ && player_->pauseBgm();
              setPresentation(
                  paused ? ProgramPanel::BgmPresentationState::Paused
                         : ProgramPanel::BgmPresentationState::Playing);
              if (!paused) {
                startTransportFade(1.0, PendingTransportAction::None);
              }
            } else if (action == PendingTransportAction::Stop) {
              completeStop();
            }
          });
  if (playlistModel_) {
    connect(playlistModel_, &PlaylistModel::itemSettingsChanged, this,
            [this](quint64 itemId) {
              const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
              if (media && media->itemId == itemId) {
                applyItemVolume();
              }
            });
  }
  manualFadeGain_ = programPanel_->bgmGain();
  signalClock_.start();
  if (player_) {
    player_->setBgmGain(programPanel_->bgmGain());
    player_->setBgmManualFadeGain(1.0);
    player_->setBgmTransportGain(1.0);
  }
  setPresentation(ProgramPanel::BgmPresentationState::Stopped);
  connect(programPanel, &ProgramPanel::bgmVolumeChanged, this,
          [this](double gain) {
            manualFadeGain_ = std::clamp(gain, 0.0, 2.0);
            cancelManualFade();
            if (player_) player_->setBgmGain(manualFadeGain_);
          });
}

bool BgmPlayoutController::isActive() const {
  return playlistModel_ && playlistModel_->bgmItemId() != 0;
}

int BgmPlayoutController::activePlaylistNumber() const {
  return activePlaylistNumber_;
}

void BgmPlayoutController::setControlsVisibleRequested(bool visible) {
  controlsVisibleRequested_ = visible;
  if (!programPanel_) {
    return;
  }
  if (visible) {
    hideWhenStopped_ = false;
    programPanel_->setBgmControlsVisible(true);
    return;
  }
  if (isActive()) {
    hideWhenStopped_ = true;
    showWarning(vtrpon::uiText("program.bgm.hide_after_stop"));
    return;
  }
  programPanel_->setBgmControlsVisible(false);
}

bool BgmPlayoutController::itemIsEligible(
    const PlaylistItemState &item) const {
  return playlistModel_ && item.sourceType == PlaylistSourceType::File &&
         (item.mediaType == PlaylistMediaType::Audio ||
          item.mediaType == PlaylistMediaType::Video ||
          item.mediaType == PlaylistMediaType::Unknown) &&
         playlistModel_->playlistMode(item.playlistNumber) ==
             PlaylistMode::Bgm &&
         !playlistModel_->itemMediaOffline(item.id) &&
         !playlistModel_->itemConversionInProgress(item.id);
}

quint64 BgmPlayoutController::startingItemId() const {
  if (!playlistModel_ || !playlistPanel_) {
    return 0;
  }
  const auto selected = playlistPanel_->selectedItemState();
  if (selected && itemIsEligible(*selected)) {
    return selected->id;
  }
  // Prefer the visible BGM list, then search the other BGM lists in order.
  auto playlists = playlistModel_->playlists();
  const int visible = playlistModel_->activePlaylistNumber();
  std::stable_sort(playlists.begin(), playlists.end(),
                   [visible](const auto &a, const auto &b) {
    return a.number == visible && b.number != visible;
  });
  for (const auto &playlist : playlists) {
    if (playlist.mode != PlaylistMode::Bgm) continue;
    for (int row = 0; row < playlistModel_->itemCount(playlist.number); ++row) {
      const quint64 itemId = playlistModel_->itemIdAt(playlist.number, row);
      const auto item = playlistModel_->itemState(itemId);
      if (item && itemIsEligible(*item)) return itemId;
    }
  }
  return 0;
}

quint64 BgmPlayoutController::nextEligibleItemId(
    quint64 currentItemId) const {
  if (!playlistModel_ || activePlaylistNumber_ <= 0) {
    return 0;
  }
  const int count = playlistModel_->itemCount(activePlaylistNumber_);
  int currentRow = -1;
  for (int row = 0; row < count; ++row) {
    if (playlistModel_->itemIdAt(activePlaylistNumber_, row) == currentItemId) {
      currentRow = row;
      break;
    }
  }
  for (int offset = 1; offset <= count; ++offset) {
    const int row = (currentRow + offset + count) % count;
    const quint64 candidateId =
        playlistModel_->itemIdAt(activePlaylistNumber_, row);
    if (failedItemIds_.contains(candidateId)) {
      continue;
    }
    const auto candidate = playlistModel_->itemState(candidateId);
    if (candidate && itemIsEligible(*candidate)) {
      return candidateId;
    }
  }
  return 0;
}

bool BgmPlayoutController::loadItem(quint64 itemId,
                                    PendingStart pendingStart) {
  if (shutdown_ || !player_ || !playlistModel_) {
    return false;
  }
  const auto item = playlistModel_->itemState(itemId);
  if (!item || !itemIsEligible(*item) || !stateMachine_.prepare(itemId)) {
    return false;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  if (!media || !player_->loadBgm(
                    media->filePath, media->inPointMs,
                    media->outPointMs.value_or(-1))) {
    stateMachine_.fail();
    return false;
  }
  activePlaylistNumber_ = item->playlistNumber;
  cancelTransportFade();
  setTransportGain(0.0);
  pendingStart_ = pendingStart;
  firstPlayPending_ = true;
  playlistModel_->setBgmItemId(itemId);
  resetItemFade();
  applyItemVolume();
  const QString playlistName =
      playlistModel_->playlistName(activePlaylistNumber_).isEmpty()
          ? vtrpon::uiText("playlist.default_name")
                .arg(activePlaylistNumber_)
          : playlistModel_->playlistName(activePlaylistNumber_);
  programPanel_->setBgmPresentation(
      ProgramPanel::BgmPresentationState::Loading, playlistName,
      item->displayName.isEmpty() ? QFileInfo(item->filePath).fileName()
                                  : item->displayName,
      item->filePath, manualFadeGain_ <= kSilenceGainEpsilon);
  return true;
}

bool BgmPlayoutController::startLoadedItem(PendingStart pendingStart) {
  if (!player_ || !playlistModel_) {
    return false;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const auto item = media ? playlistModel_->itemState(media->itemId)
                          : std::nullopt;
  const auto startMode = stateMachine_.start();
  if (!item || !startMode) {
    return false;
  }
  applyItemVolume();
  if (*startMode == PlaylistStartMode::Pause ||
      pendingStart == PendingStart::Pause) {
    cancelTransportFade();
    setTransportGain(0.0);
    pendingStart_ = PendingStart::None;
    audioController_->setBgmActive(true);
    setPresentation(ProgramPanel::BgmPresentationState::Paused);
    return true;
  }
  player_->setBgmGain(manualFadeGain_);
  cancelTransportFade();
  setTransportGain(0.0);
  bool outputBlocked = false;
  if (!tryPlayBgm(outputBlocked)) {
    if (outputBlocked) {
      // Retain this valid item for an explicit PLAY retry, without marking it
      // failed or silently advancing through the entire playlist.
      pendingStart_ = PendingStart::None;
      audioController_->setBgmActive(true);
      setPresentation(ProgramPanel::BgmPresentationState::Paused);
      return true;
    }
    stateMachine_.fail();
    return false;
  }
  audioController_->setBgmActive(true);
  startItemFadeIn(item->settings);
  firstPlayPending_ = false;
  pendingStart_ = PendingStart::None;
  setPresentation(ProgramPanel::BgmPresentationState::Playing);
  startTransportFade(1.0, PendingTransportAction::None);
  return true;
}

bool BgmPlayoutController::tryPlayBgm(bool &outputBlocked) {
  DualDeckPlayer::BgmPlayFailure failure = DualDeckPlayer::BgmPlayFailure::Other;
  const bool played = player_ && player_->playBgm(&failure);
  outputBlocked = !played &&
      (failure == DualDeckPlayer::BgmPlayFailure::AudioOutputChanging ||
       failure == DualDeckPlayer::BgmPlayFailure::AudioOutputUnavailable);
  if (outputBlocked && userInfoCenter_) {
    userInfoCenter_->showAudioOutputWarning(vtrpon::LocalizedText(
        failure == DualDeckPlayer::BgmPlayFailure::AudioOutputChanging
            ? "program.bgm.audio_output_changing" : "program.bgm.audio_output_unavailable"));
  }
  return played;
}

void BgmPlayoutController::play() {
  if (stopFailed_) {
    showWarning(vtrpon::uiText("program.bgm.stop_failed"));
    return;
  }
  if (shutdown_ || !controlsVisibleRequested_ || !player_ ||
      !playlistModel_) {
    return;
  }
  if (isActive()) {
    const auto status = player_->bgmStatus();
    if (status == DualDeckPlayer::BgmStatus::Loading) {
      pendingStart_ = PendingStart::Play;
      return;
    }
    cancelManualFade();
    const bool rampTransport =
        status != DualDeckPlayer::BgmStatus::Playing ||
        transportFadeAnimation_.state() == QAbstractAnimation::Running ||
        transportGain_ < 0.999;
    cancelTransportFade();
    if (status != DualDeckPlayer::BgmStatus::Playing) {
      setTransportGain(0.0);
    }

    bool outputBlocked = false;
    if (tryPlayBgm(outputBlocked)) {
      if (firstPlayPending_) {
        const auto item = playlistModel_->itemState(playlistModel_->bgmItemId());
        if (item) {
          startItemFadeIn(item->settings);
          firstPlayPending_ = false;
        }
      }
      (void)stateMachine_.markActive();
      audioController_->setBgmActive(true);
      setPresentation(ProgramPanel::BgmPresentationState::Playing);
      if (rampTransport) {
        startTransportFade(1.0, PendingTransportAction::None);
      }
    } else if (!outputBlocked) {
      showWarning(vtrpon::uiText("program.bgm.play_failed"), player_->bgmErrorDetail());
    }
    return;
  }
  failedItemIds_.clear();
  const quint64 itemId = startingItemId();
  if (itemId == 0 || !loadItem(itemId, PendingStart::Play)) {
    showWarning(vtrpon::uiText("program.bgm.no_playable_item"));
  }
}

void BgmPlayoutController::pause() {
  if (stopFailed_) return;
  if (!isActive() || !player_) {
    return;
  }
  if (player_->bgmStatus() == DualDeckPlayer::BgmStatus::Loading) {
    pendingStart_ = PendingStart::Pause;
    return;
  }
  cancelManualFade();
  const auto status = player_->bgmStatus();
  if (status == DualDeckPlayer::BgmStatus::Playing) {
    audioController_->setBgmActive(true);
    startTransportFade(0.0, PendingTransportAction::Pause);
  } else if (player_->pauseBgm()) {
    cancelTransportFade();
    setTransportGain(0.0);
    audioController_->setBgmActive(true);
    setPresentation(ProgramPanel::BgmPresentationState::Paused);
  }
}

void BgmPlayoutController::stop() {
  cancelManualFade();
  cancelTransportFade();
  itemFadeAnimation_.stop();
  itemFadeGain_ = 1.0;
  if (!shutdown_ && player_ &&
      player_->bgmStatus() == DualDeckPlayer::BgmStatus::Playing &&
      transportGain_ > kSilenceGainEpsilon) {
    startTransportFade(0.0, PendingTransportAction::Stop);
    return;
  }
  completeStop();
}

void BgmPlayoutController::completeStop() {
  cancelTransportFade();
  if (player_) {
    setTransportGain(0.0);
    const auto status = player_->bgmStatus();
    bool stopped = false;
    if (stopFailed_ || status == DualDeckPlayer::BgmStatus::Loading ||
        status == DualDeckPlayer::BgmStatus::Error) {
      stopped = player_->clearBgm();
    } else {
      stopped = player_->stopBgm();
    }
    if (!stopped) {
      stopFailed_ = true;
      pendingStart_ = PendingStart::None;
      const QString message = vtrpon::uiText("program.bgm.stop_failed");
      setPresentation(ProgramPanel::BgmPresentationState::Error, message);
      showWarning(message, player_->bgmErrorDetail());
      return;
    }
  }
  stopFailed_ = false;
  firstPlayPending_ = false;
  pendingStart_ = PendingStart::None;
  failedItemIds_.clear();
  (void)stateMachine_.deactivate();
  if (playlistModel_) {
    playlistModel_->clearBgmItem();
  }
  audioController_->setBgmActive(false);

  setPresentation(ProgramPanel::BgmPresentationState::Stopped);
  if (hideWhenStopped_ && programPanel_) {
    hideWhenStopped_ = false;
    programPanel_->setBgmControlsVisible(false);
  }
  activePlaylistNumber_ = 0;
}

bool BgmPlayoutController::canManualFade(bool fadeIn) const {
  if (stopFailed_ || !isActive() || !player_) return false;
  if (fadeIn && (shutdown_ || !controlsVisibleRequested_)) return false;
  const auto status = player_->bgmStatus();
  return status == DualDeckPlayer::BgmStatus::Playing
      || status == DualDeckPlayer::BgmStatus::Paused;
}

void BgmPlayoutController::fadeIn(qint64 durationMs) {
  if (!canManualFade(true)) return;
  foldTransportGainIntoManualFade();
  startManualFade(1.0, durationMs);
}

void BgmPlayoutController::fadeOut(qint64 durationMs) {
  if (!canManualFade(false)) return;
  foldTransportGainIntoManualFade();
  startManualFade(0.0, durationMs);
}

void BgmPlayoutController::startManualFade(qreal target,
                                           qint64 durationMs) {
  manualFadeAnimation_.stop();
  manualFadeAnimation_.setStartValue(manualFadeGain_);
  manualFadeAnimation_.setEndValue(std::clamp(target, qreal{0.0}, qreal{1.0}));
  manualFadeAnimation_.setDuration(static_cast<int>(std::clamp<qint64>(
      durationMs, kMinimumItemFadeDurationMs, kMaximumItemFadeDurationMs)));
  if (programPanel_) {
    programPanel_->setBgmFadePresentation(
        target <= kSilenceGainEpsilon
            ? ProgramPanel::BgmFadeState::FadeOut
            : ProgramPanel::BgmFadeState::FadeIn,
        manualFadeGain_ <= kSilenceGainEpsilon);
  }
  manualFadeAnimation_.start();
}

void BgmPlayoutController::cancelManualFade() {
  manualFadeAnimation_.stop();
  if (programPanel_) {
    programPanel_->setBgmFadePresentation(
        ProgramPanel::BgmFadeState::Idle,
        manualFadeGain_ <= kSilenceGainEpsilon);
  }
}

void BgmPlayoutController::startTransportFade(
    qreal target, PendingTransportAction action) {
  transportFadeAnimation_.stop();
  pendingTransportAction_ = action;
  transportFadeAnimation_.setStartValue(transportGain_);
  transportFadeAnimation_.setEndValue(
      std::clamp(target, qreal{0.0}, qreal{1.0}));
  transportFadeAnimation_.setDuration(kTransportRampMs);
  transportFadeAnimation_.start();
}

void BgmPlayoutController::cancelTransportFade() {
  transportFadeAnimation_.stop();
  pendingTransportAction_ = PendingTransportAction::None;
}

void BgmPlayoutController::setTransportGain(qreal gain) {
  transportGain_ = std::clamp(gain, qreal{0.0}, qreal{1.0});
  if (player_) {
    player_->setBgmTransportGain(transportGain_);
  }
}

void BgmPlayoutController::foldTransportGainIntoManualFade() {
  if (transportFadeAnimation_.state() != QAbstractAnimation::Running &&
      transportGain_ >= 0.999) {
    return;
  }
  cancelTransportFade();
  manualFadeGain_ = std::clamp(manualFadeGain_ * transportGain_,
                               qreal{0.0}, qreal{2.0});
  if (programPanel_) programPanel_->setBgmFaderGain(manualFadeGain_);
  if (player_) {
    player_->setBgmGain(manualFadeGain_);
  }
  setTransportGain(1.0);
}

void BgmPlayoutController::resetItemFade() {
  itemFadeAnimation_.stop();
  itemFadeGain_ = 1.0;
  itemFadeOutStarted_ = false;
  applyItemVolume();
}

void BgmPlayoutController::startItemFadeIn(
    const PlaylistItemSettings &settings) {
  itemFadeAnimation_.stop();
  itemFadeOutStarted_ = false;
  if (!settings.fadeInEnabled) {
    itemFadeGain_ = 1.0;
    applyItemVolume();
    return;
  }
  itemFadeGain_ = 0.0;
  applyItemVolume();
  itemFadeAnimation_.setStartValue(0.0);
  itemFadeAnimation_.setEndValue(1.0);
  itemFadeAnimation_.setDuration(static_cast<int>(settings.fadeInDurationMs));
  itemFadeAnimation_.start();
}

void BgmPlayoutController::maybeStartItemFadeOut(qint64 positionMs,
                                                  qint64 endPositionMs) {
  if (itemFadeOutStarted_ || !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const auto item = media ? playlistModel_->itemState(media->itemId)
                          : std::nullopt;
  if (!item || !item->settings.fadeOutEnabled || positionMs < 0 ||
      endPositionMs < positionMs ||
      endPositionMs - positionMs > item->settings.fadeOutDurationMs) {
    return;
  }
  itemFadeOutStarted_ = true;
  itemFadeAnimation_.stop();
  itemFadeAnimation_.setStartValue(itemFadeGain_);
  itemFadeAnimation_.setEndValue(0.0);
  itemFadeAnimation_.setDuration(static_cast<int>(
      (std::max)(qint64{1}, endPositionMs - positionMs)));
  itemFadeAnimation_.start();
}

void BgmPlayoutController::applyItemVolume() {
  if (!player_ || !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const auto item = media ? playlistModel_->itemState(media->itemId)
                          : std::nullopt;
  if (item) {
    player_->setBgmItemVolume(
        nominalGain(item->settings.nominalVolumeTenthsDb) * itemFadeGain_);
    player_->setBgmItemDelayFrames(item->settings.nominalAudioDelayFrames);
    const auto eq = item->settings.nominalEqTenthsDb;
    player_->setBgmItemEq(eq[0] / 10.0, eq[1] / 10.0, eq[2] / 10.0);
  }
}

void BgmPlayoutController::handleLoadFailure(const QString &detail) {
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const quint64 failedId = media ? media->itemId : 0;
  if (failedId != 0) {
    failedItemIds_.insert(failedId);
  }
  const auto failedItem = playlistModel_ ? playlistModel_->itemState(failedId)
                                         : std::nullopt;
  showWarning(vtrpon::uiText("program.bgm.item_skipped")
                  .arg(failedItem ? failedItem->displayName : QString()),
              detail);
  const quint64 nextId = nextEligibleItemId(failedId);
  const PendingStart pending = pendingStart_ == PendingStart::None
                                   ? PendingStart::Play
                                   : pendingStart_;
  if (nextId == 0 || !loadItem(nextId, pending)) {
    stop();
    showWarning(vtrpon::uiText("program.bgm.no_playable_item"), detail);
  }
}

void BgmPlayoutController::handleCompletion() {
  if (!player_ || !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const auto item = media ? playlistModel_->itemState(media->itemId)
                          : std::nullopt;
  const auto endMode = stateMachine_.finish();
  qInfo() << "AUTO BGM completion item=" << (media ? media->itemId : 0)
          << "endMode=" << (endMode ? static_cast<int>(*endMode) : -1);
  if (!media || !item || !endMode) {
    stop();
    return;
  }
  if (*endMode == PlaylistEndMode::Repeat) {
    resetItemFade();
    bool outputBlocked = false;
    if (player_->seekBgm(media->inPointMs) && tryPlayBgm(outputBlocked)) {
      startItemFadeIn(item->settings);
      setPresentation(ProgramPanel::BgmPresentationState::Playing);
      return;
    }
    if (outputBlocked) {
      cancelTransportFade();
      setTransportGain(0.0);
      if (player_->pauseBgm()) {
        setPresentation(ProgramPanel::BgmPresentationState::Paused);
        return;
      }
      stop();
      return;
    }
    stop();
    showWarning(vtrpon::uiText("program.bgm.transition_failed"));
    return;
  }
  if (*endMode == PlaylistEndMode::Stop) {
    stop();
    return;
  }
  if (*endMode == PlaylistEndMode::Pause) {
    itemFadeAnimation_.stop();
    (void)player_->pauseBgm();
    audioController_->setBgmActive(true);
    setPresentation(ProgramPanel::BgmPresentationState::Paused);
    return;
  }

  quint64 targetId = 0;
  failedItemIds_.clear();
  if (*endMode == PlaylistEndMode::Goto &&
      item->settings.gotoTargetItemId != 0) {
    const auto target = playlistModel_->itemState(
        item->settings.gotoTargetItemId);
    if (target && itemIsEligible(*target)) {
      targetId = target->id;
    }
  } else if (*endMode == PlaylistEndMode::Next) {
    targetId = nextEligibleItemId(media->itemId);
  }
  if (targetId != 0 && loadItem(targetId, PendingStart::Play)) {
    return;
  }
  stop();
  showWarning(vtrpon::uiText("program.bgm.transition_failed"));
}

void BgmPlayoutController::updateSignalIndicator() {
  if (!player_ || !programPanel_) {
    return;
  }
  const DualDeckPlayer::AudioLevel level = player_->bgmSignalLevel();
  const qint64 now = signalClock_.elapsed();
  if ((std::max)(level.leftPeakDb, level.rightPeakDb) > kSignalFloorDb) {
    signalUntilMs_ = now + kSignalHoldMs;
  }
  if (level.clipped) {
    clipUntilMs_ = now + kClipHoldMs;
  }
  programPanel_->setBgmSignalAppearance(now < signalUntilMs_,
                                        now < clipUntilMs_);
}

void BgmPlayoutController::update() {
  if (shutdown_ || !player_) {
    return;
  }
  updateSignalIndicator();
  if (stopFailed_) return;
  if (pendingTransportAction_ == PendingTransportAction::Stop) return;
  const auto status = player_->bgmStatus();
  if (status == DualDeckPlayer::BgmStatus::Error) {
    if (playlistModel_ && player_->bgmMediaOffline())
      playlistModel_->setFileMediaOffline(playlistModel_->bgmItemId());
    handleLoadFailure(player_->bgmErrorDetail());
    return;
  }
  if (pendingTransportAction_ != PendingTransportAction::None) {
    return;
  }
  if (status == DualDeckPlayer::BgmStatus::Ready &&
      pendingStart_ != PendingStart::None) {
    const PendingStart pending = pendingStart_;
    if (!startLoadedItem(pending)) {
      handleLoadFailure(player_->bgmErrorDetail());
    }
    return;
  }
  if (!isActive() || status != DualDeckPlayer::BgmStatus::Playing) {
    return;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const qint64 duration = player_->bgmDurationMs();
  const qint64 endPosition = media && media->outPointMs
      ? *media->outPointMs : duration;
  const qint64 position = player_->bgmPositionMs();
  if (position >= 0 && endPosition >= position) {
    maybeStartItemFadeOut(position, endPosition);
  }
  QString detail;
  const auto event = player_->takeBgmEvent(detail);
  if (event == DualDeckPlayer::BgmEvent::Error) {
    handleLoadFailure(detail);
  } else if (event == DualDeckPlayer::BgmEvent::EndOfStream ||
             (endPosition >= 0 && position >= endPosition)) {
    handleCompletion();
  }
}

void BgmPlayoutController::setPresentation(
    ProgramPanel::BgmPresentationState state, const QString &detail) {
  if (!programPanel_ || !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *media = stateMachine_.preparedMedia();
  const auto item = media ? playlistModel_->itemState(media->itemId)
                          : std::nullopt;
  QString playlistName;
  QString itemName;
  QString itemDetail = detail;
  if (item) {
    playlistName = playlistModel_->playlistName(item->playlistNumber);
    if (playlistName.isEmpty()) {
      playlistName = vtrpon::uiText("playlist.default_name")
                         .arg(item->playlistNumber);
    }
    itemName = item->displayName.isEmpty()
        ? QFileInfo(item->filePath).fileName() : item->displayName;
    if (itemDetail.isEmpty()) {
      itemDetail = item->filePath;
    }
  }
  programPanel_->setBgmPresentation(state, playlistName, itemName,
                                    itemDetail,
                                    manualFadeGain_ <= kSilenceGainEpsilon);
}

void BgmPlayoutController::showWarning(const QString &text,
                                       const QString &detail) {
  if (userInfoCenter_) {
    userInfoCenter_->showWarning(QStringLiteral("BGM"), text, detail);
  }
}

void BgmPlayoutController::requestShutdown() {
  shutdown_ = true;
  manualFadeAnimation_.stop();
  itemFadeAnimation_.stop();
  transportFadeAnimation_.stop();
  pendingStart_ = PendingStart::None;
  pendingTransportAction_ = PendingTransportAction::None;
}

void BgmPlayoutController::shutdown() {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  stop();
  if (player_) {
    (void)player_->clearBgm();
  }
}
