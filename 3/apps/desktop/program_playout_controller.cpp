#include "decklink.h"
#include "program_playout_controller.h"
#include "live_audio_source.h"
#include "network_video_source.h"
#include <QDebug>

#include "bgm_playout_controller.h"
#include "language_text.h"
#include "playlist_model.h"
#include "playlist_panel.h"
#include "program_panel.h"
#include "user_info.h"
#include "uvc_audio_mapping.h"
#include "uvc_source_session_manager.h"

#include <QFileInfo>
#include <QScopedValueRollback>
#include <QWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace {
bool macroOperationError(QString *error, const QString &detail) {
  if (error) *error = detail;
  return false;
}


constexpr int kFtbFadeDurationMs = 1000;
constexpr qint64 kNextPreloadLeadMs = 1000;
constexpr int kPowerPointNavigationRetryIntervalMs = 100;
constexpr int kMaximumPowerPointNavigationRetries = 300;
constexpr auto kFtbBlinkInterval = std::chrono::milliseconds{200};

std::optional<qint64>
timedStillEndPosition(const PlaylistItemState &item) {
  if (item.mediaType != PlaylistMediaType::StillImage ||
      item.settings.inPointMs < 0 || !item.settings.outPointMs ||
      *item.settings.outPointMs <= item.settings.inPointMs) {
    return std::nullopt;
  }
  return item.settings.outPointMs;
}

} // namespace

ProgramPlayoutController::ProgramPlayoutController(
    QWidget *videoWindow, ProgramPanel *programPanel,
    PlaylistPanel *playlistPanel, PlaylistModel *playlistModel,
    UserInfoCenter *userInfoCenter, bool use4kProgramOutput,
    UvcSourceSessionManager *uvcSourceSessions,
    UvcAudioMappingStore *uvcAudioMappingStore, QObject *parent)
    : QObject(parent), videoWindow_(videoWindow),
      programPanel_(programPanel), playlistPanel_(playlistPanel),
      playlistModel_(playlistModel), userInfoCenter_(userInfoCenter),
      uvcAudioMappingStore_(uvcAudioMappingStore),
      uvcSourceSessions_(uvcSourceSessions),
      onAirStateMachine_(playlistModel), dskStateMachine_(playlistModel),
      player_(videoWindow, programPanel->monitorWindow(), use4kProgramOutput,
              uvcSourceSessions),
      audioController_(&player_, programPanel) {
  bgmController_ = std::make_unique<BgmPlayoutController>(
      &player_, programPanel, playlistPanel, playlistModel, &audioController_,
      userInfoCenter, this);
  auto *operations = programPanel->operations();
  using A = ControllerOperations::Action;
  operations->bind<qint64>(A::ProgramSeek, this, [this](qint64 positionMs) {
    if (positionMs < 0 || !onAirStateMachine_.preparedMedia()) return false;
    if (nextTransition_.mode == PlaylistEndMode::Repeat) {
      nextTransition_.due = false;
      nextTransition_.paused = false;
    }
    pendingSeekMs_ = positionMs;
    return true;
  });
  operations->connectInput(programPanel, &ProgramPanel::seekRequested, A::ProgramSeek);
  operations->bindMember(A::ProgramPlaybackRate, this, &ProgramPlayoutController::setPlaybackRate);
  operations->connectInput(programPanel, &ProgramPanel::playbackRateRequested, A::ProgramPlaybackRate);
  connect(playlistPanel, &PlaylistPanel::selectionChanged, this,
          [this] {
            if (!internalAutoSelection_) {
              const QString reason = QStringLiteral("Playlist selection changed the standby item.");
              if (macroCueOwner_ || macroRequest_ || macroTransportRequest_)
                interruptMacroReservation(reason);
              else emit macroIntervention(reason);
            }
            scheduleSelectedItemCue();
          });
  connect(playlistPanel, &PlaylistPanel::selectionChanged, this,
          &ProgramPlayoutController::updateDskSelectionAvailability);
  connect(playlistModel, &PlaylistModel::itemMediaAvailabilityChanged, this,
          [this](quint64 itemId, bool offline) {
    if (shutdown_) return;
    if (offline && macroCueOwner_ && macroCueOwner_->itemId == itemId) {
      failMacroReservation(QStringLiteral("The macro standby item became unavailable."));
    }
    const auto item = playlistModel_->itemState(itemId);
    if (!item || item->sourceType != PlaylistSourceType::Uvc) return;
    if (offline) {
      if (playlistModel_->onAirItemId() == itemId) {
        clearOnAir();
        return;
      }
      if (crossfadeOutgoingItemId_ == itemId && player_.crossfadeActive()) {
        crossfadeAnimation_.stop();
        (void)player_.setCrossfadeProgress(1.0F);
        finishOnAirHighlightCrossfade();
      }
      if (nextTransition_.itemId == itemId) cancelNextTransition(false);
      if ((cue_ && cue_->itemId == itemId) ||
          (pendingCue_ && pendingCue_->itemId == itemId))
        scheduleCueRequest(std::nullopt);
      if (pendingTakeItemId_ == itemId) pendingTakeItemId_.reset();
    } else if (playlistPanel_->selectedItemId() == itemId) {
      scheduleSelectedItemCue();
    }
  });
  const auto warmUpNativePowerPoint = [this](quint64 itemId) {
    if (shutdown_) return;
    const auto item = playlistModel_->itemState(itemId);
    if (item && item->sourceType == PlaylistSourceType::PowerPoint &&
        item->powerPointMode == PlaylistPowerPointMode::NativeSlideShow &&
        !playlistModel_->itemConversionInProgress(itemId)) {
      player_.warmUpPowerPointApplication();
    }
  };
  connect(playlistModel, &PlaylistModel::itemAdded, this, warmUpNativePowerPoint);
  connect(playlistModel, &PlaylistModel::itemChanged, this, warmUpNativePowerPoint);
  const auto warmUpRestoredPowerPoint = [this, warmUpNativePowerPoint] {
    for (const auto &item : playlistModel_->snapshot().items) {
      warmUpNativePowerPoint(item.id);
    }
  };
  connect(playlistModel, &PlaylistModel::modelReset, this, warmUpRestoredPowerPoint);
  warmUpRestoredPowerPoint();
  connect(playlistModel, &PlaylistModel::itemSettingsChanged, this,
          [this](quint64 itemId) {
            if (macroCueOwner_ && macroCueOwner_->itemId == itemId) {
              const auto item = playlistModel_->itemState(itemId);
              if (!item || cueRequestFor(*item) != *macroCueOwner_) {
                if (macroRequest_) {
                  interruptMacroReservation(QStringLiteral("The macro standby item settings changed."));
                } else {
                  // A settings step may intentionally change In/Out after CUE.
                  // Drop stale readiness; the next TAKE must prepare again.
                  // The runner independently observes external item edits.
                  cancelMacroRequests();
                }
              }
            }
            if (playlistPanel_ && itemId == playlistPanel_->selectedItemId()) {
              scheduleSelectedItemCue();
            }
          });
  connect(playlistModel, &PlaylistModel::playlistModeChanged, this,
          [this](int) {
            scheduleSelectedItemCue();
            updateDskSelectionAvailability();
          });
  operations->bindMember(A::DocumentNext, this, &ProgramPlayoutController::nextPowerPointSlide);
  operations->connectInput(programPanel, &ProgramPanel::powerPointNextRequested, A::DocumentNext);
  operations->bindMember(A::DocumentPrevious, this, &ProgramPlayoutController::previousPowerPointSlide);
  operations->connectInput(programPanel, &ProgramPanel::powerPointPreviousRequested, A::DocumentPrevious);
  connect(&powerPointNavigationRetryTimer_, &QTimer::timeout, this,
          [this] {
            if (pendingPowerPointNavigation_) {
              attemptPowerPointNavigation(*pendingPowerPointNavigation_);
            } else {
              powerPointNavigationRetryTimer_.stop();
            }
          });
  if (uvcAudioMappingStore_) {
    connect(uvcAudioMappingStore_, &UvcAudioMappingStore::mappingChanged,
            this, [this](const QString &uvcDevicePath) {
              const auto selected = playlistPanel_
                                        ? playlistPanel_->selectedItemState()
                                        : std::nullopt;
              if (selected &&
                  selected->sourceType == PlaylistSourceType::Uvc &&
                  selected->filePath == uvcDevicePath) {
                scheduleSelectedItemCue();
              }
            });
  }
  connect(&busTimer_, &QTimer::timeout, this,
          &ProgramPlayoutController::updatePlaybackUi);
  connect(&ftbAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) { applyFtbCoverage(value.toReal()); });
  connect(&ftbAnimation_, &QVariantAnimation::finished, this, [this] {
    applyFtbCoverage(ftbTargetCovered_ ? 1.0 : 0.0);
    ftbBlinkTimer_.stop();
    if (programPanel_) {
      programPanel_->setFtbButtonActive(ftbTargetCovered_);
    }
  });
  connect(&ftbBlinkTimer_, &QTimer::timeout, this, [this] {
    ftbBlinkRed_ = !ftbBlinkRed_;
    if (programPanel_) {
      programPanel_->setFtbButtonActive(ftbBlinkRed_);
    }
  });
  connect(&crossfadeAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            crossfadeProgress_ = std::clamp(value.toReal(), 0.0, 1.0);
            applyOnAirHighlightProgress(std::clamp(
                itemFadeInProgress_ * itemFadeOutProgress_, 0.0, 1.0));
            if (!player_.setCrossfadeProgress(
                    static_cast<float>(crossfadeProgress_))) {
              crossfadeAnimation_.stop();
              finishOnAirHighlightCrossfade();
            }
          });
  connect(&crossfadeAnimation_, &QVariantAnimation::finished, this, [this] {
    if (player_.crossfadeActive()) {
      (void)player_.setCrossfadeProgress(1.0F);
    }
    finishOnAirHighlightCrossfade();
  });
  connect(&itemFadeInAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            itemFadeInProgress_ = value.toReal();
            applyItemFadeProgress();
          });
  connect(&itemFadeOutAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            itemFadeOutProgress_ = value.toReal();
            applyItemFadeProgress();
          });
  connect(&dskCrossfadeAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            if (!player_.setDskCrossfadeProgress(
                    static_cast<float>(value.toReal()))) {
              dskCrossfadeAnimation_.stop();
            }
          });
  connect(&dskCrossfadeAnimation_, &QVariantAnimation::finished, this,
          [this] {
            if (player_.dskCrossfadeActive()) {
              (void)player_.setDskCrossfadeProgress(1.0F);
            }
          });
  connect(&dskFadeInAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            dskFadeInProgress_ = value.toReal();
            applyDskFadeProgress();
          });
  connect(&dskFadeOutAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            dskFadeOutProgress_ = value.toReal();
            applyDskFadeProgress();
          });
  connect(&dskFadeOutAnimation_, &QVariantAnimation::finished, this,
          [this] {
            if (pendingDskOff_) {
              finishDskOff();
            }
          });
  ftbAnimation_.setDuration(kFtbFadeDurationMs);
  crossfadeAnimation_.setDuration(
      static_cast<int>(kDefaultItemFadeDurationMs));
  crossfadeAnimation_.setEasingCurve(QEasingCurve::Linear);
  itemFadeInAnimation_.setEasingCurve(QEasingCurve::Linear);
  itemFadeOutAnimation_.setEasingCurve(QEasingCurve::Linear);
  dskCrossfadeAnimation_.setEasingCurve(QEasingCurve::Linear);
  dskFadeInAnimation_.setEasingCurve(QEasingCurve::Linear);
  dskFadeOutAnimation_.setEasingCurve(QEasingCurve::Linear);
  ftbBlinkTimer_.setInterval(kFtbBlinkInterval);
  powerPointNavigationRetryTimer_.setInterval(
      kPowerPointNavigationRetryIntervalMs);
  applyFtbCoverage(0.0);
  programPanel->setDskCueState(ProgramPanel::DskCueState::Empty);
  programPanel->setDskOn(false);
  programPanel->setPlaybackState(false, false);
  updateDskSelectionAvailability();
  if (player_.isAvailable()) {
    onAirTimedStillElapsedTimer_.start();
    dskTimedStillElapsedTimer_.start();
    busTimer_.start(100);
  }
}

ProgramPlayoutController::~ProgramPlayoutController() { shutdown(); }

ProgramPlayoutController::CueRequest
ProgramPlayoutController::cueRequestFor(
    const PlaylistItemState &item) const {
  const bool timedStill = timedStillEndPosition(item).has_value();
  return CueRequest{
      item.id,
      item.filePath,
      item.displayName,
      item.sourceType,
      vtrpon::isLiveAudioSource(item.filePath) ? vtrpon::liveAudioDeviceId(item.filePath)
      : (vtrpon::isNetworkVideoSource(item.filePath) || vtrpon::isDeckLinkSource(item.filePath)) ? item.filePath
      : item.sourceType == PlaylistSourceType::Uvc && uvcAudioMappingStore_
          ? uvcAudioMappingStore_->audioDeviceIdFor(item.filePath)
          : QString(),
      timedStill ? qint64{0} : item.settings.inPointMs,
      timedStill ? std::nullopt : item.settings.outPointMs,
  };
}

bool ProgramPlayoutController::isAvailable() const {
  return player_.isAvailable();
}

DualDeckPlayer::FullscreenOutputStats
ProgramPlayoutController::fullscreenOutputStats() const {
  return player_.fullscreenOutputStats();
}

bool ProgramPlayoutController::canRemoveItem(quint64 itemId) {
  if (!playlistModel_ || itemId == 0) {
    return false;
  }
  if (playlistModel_->onAirItemId() == itemId) {
    showWarning(QStringLiteral("An item currently ON AIR cannot be deleted."));
    return false;
  }
  if (playlistModel_->bgmItemId() == itemId) {
    showWarning(vtrpon::uiText("program.warning.delete_bgm"));
    return false;
  }

  const PlaylistPreparedMedia *dskMedia = dskStateMachine_.preparedMedia();
  const DualDeckPlayer::DskStatus dskCueStatus = player_.dskCueStatus();
  if ((dskMedia && dskMedia->itemId == itemId &&
       player_.dskStatus() == DualDeckPlayer::DskStatus::On) ||
      (dskCue_ && dskCue_->itemId == itemId &&
       (pendingDskOn_ || dskCueStatus == DualDeckPlayer::DskStatus::Loading))) {
    showWarning(QStringLiteral("An item currently in DSK cannot be deleted."));
    return false;
  }
  return true;
}

bool ProgramPlayoutController::prepareForItemRemoval(quint64 itemId) {
  if (!canRemoveItem(itemId)) {
    return false;
  }

  const DualDeckPlayer::DskStatus dskCueStatus = player_.dskCueStatus();
  if (dskCue_ && dskCue_->itemId == itemId) {
    if (dskCueStatus == DualDeckPlayer::DskStatus::Ready
        || dskCueStatus == DualDeckPlayer::DskStatus::Error) {
      if (!player_.clearDskCue()) {
        showWarning(QStringLiteral("The item cannot be deleted because DSK could not be turned off."));
        return false;
      }
      dskCue_.reset();
      pendingDskOn_ = false;
      lastDskStatus_ = player_.dskStatus();
    }
  }
  if (nextDskTransition_.itemId && *nextDskTransition_.itemId == itemId) {
    resetNextDskTransitionState(true);
  }

  const bool cueMatches =
      (pendingCue_ && pendingCue_->itemId == itemId)
      || (cue_ && cue_->itemId == itemId)
      || playlistModel_->cueItemId() == itemId;
  if (cueMatches) {
    if (cueReplacementMustWait()) {
      scheduleCueRequest(std::nullopt);
      if (nextTransition_.itemId && *nextTransition_.itemId == itemId) {
        resetNextTransitionState();
      }
      return true;
    }
    if (!player_.clearCue()) {
      showWarning(QStringLiteral("The item cannot be deleted because its preparation could not be released."));
      return false;
    }
    pendingCue_.reset();
    cueReplacementPending_ = false;
    cue_.reset();
    if (pendingTakeItemId_ && *pendingTakeItemId_ == itemId) {
      pendingTakeItemId_.reset();
    }
    lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
    playlistModel_->clearCueItem();
  }
  if (nextTransition_.itemId && *nextTransition_.itemId == itemId) {
    resetNextTransitionState();
  }
  return true;
}

bool ProgramPlayoutController::beginPowerPointConversion(
    std::atomic_bool *cancelRequested, QString *errorDetail,
    vtrpon::LocalizedText *errorMessage) {
  if (errorMessage) {
    *errorMessage = vtrpon::LocalizedText("playlist.conversion.failed");
  }
  if (errorDetail) {
    errorDetail->clear();
  }
  if (!playlistModel_ || !cancelRequested ||
      powerPointConversionInProgress_) {
    if (errorDetail) {
      *errorDetail = QStringLiteral(
          "PowerPoint conversion preparation is unavailable.");
    }
    return false;
  }
  if (player_.powerPointSourceIsOnAir()) {
    if (errorMessage) {
      *errorMessage = vtrpon::LocalizedText("powerpoint.import.native_on_air");
    }
    if (errorDetail) {
      *errorDetail =
          vtrpon::uiText("powerpoint.import.native_on_air");
    }
    return false;
  }

  cancelRequested->store(false, std::memory_order_release);
  powerPointConversionInProgress_ = true;
  powerPointNextTransitionDeferred_ = false;
  powerPointConversionCancelRequested_ = cancelRequested;
  bool powerPointCueReleased = false;
  QString releaseError;
  const bool preparationSucceeded = player_.preparePowerPointConversion(
      &powerPointCueReleased, &releaseError);
  if (powerPointCueReleased) {
    pendingCue_.reset();
    cueReplacementPending_ = false;
    cue_.reset();
    pendingTakeItemId_.reset();
    lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
    playlistModel_->clearCueItem();
    if (nextTransition_.itemId) {
      const bool transitionWasDue = nextTransition_.due;
      resetNextTransitionState();
      if (preparationSucceeded) {
        nextTransition_.due = transitionWasDue;
        powerPointNextTransitionDeferred_ = true;
      }
    }
  }
  if (!preparationSucceeded) {
    powerPointConversionInProgress_ = false;
    powerPointNextTransitionDeferred_ = false;
    powerPointConversionCancelRequested_ = nullptr;
    if (errorDetail) {
      *errorDetail =
          vtrpon::uiText("powerpoint.import.release_cue_failed");
      if (!releaseError.isEmpty()) {
        *errorDetail += QLatin1Char('\n') + releaseError;
      }
    }
    return false;
  }
  return true;
}

void ProgramPlayoutController::finishPowerPointConversion() {
  if (!powerPointConversionInProgress_) {
    return;
  }
  powerPointConversionInProgress_ = false;
  powerPointConversionCancelRequested_ = nullptr;
  const bool restoreDeferredTransition =
      powerPointNextTransitionDeferred_;
  powerPointNextTransitionDeferred_ = false;
  if (shutdown_ || !player_.isAvailable()) {
    return;
  }
  if (restoreDeferredTransition) {
    const qint64 endPosition = playbackEnd(programDurationMs());
    maybePrepareNextItem(endPosition, endPosition);
    if (nextTransition_.itemId) {
      return;
    }
  }
  scheduleSelectedItemCue();
}

void ProgramPlayoutController::refreshWindowHandles() {
  player_.refreshWindowHandles();
}

bool ProgramPlayoutController::prepareNativeDocumentShutdown() {
  busTimer_.stop();
  ftbBlinkTimer_.stop();
  ftbAnimation_.stop();
  crossfadeAnimation_.stop();
  itemFadeInAnimation_.stop();
  itemFadeOutAnimation_.stop();
  dskCrossfadeAnimation_.stop();
  dskFadeInAnimation_.stop();
  dskFadeOutAnimation_.stop();
  clearPendingPowerPointNavigation();
  return player_.prepareNativeDocumentShutdown();
}

void ProgramPlayoutController::requestShutdown() {
  if (shutdown_) {
    player_.requestShutdown();
    return;
  }
  shutdown_ = true;
  cancelMacroRequests();
  if (powerPointConversionCancelRequested_) {
    powerPointConversionCancelRequested_->store(
        true, std::memory_order_release);
  }
  busTimer_.stop();
  ftbBlinkTimer_.stop();
  ftbAnimation_.stop();
  crossfadeAnimation_.stop();
  itemFadeInAnimation_.stop();
  itemFadeOutAnimation_.stop();
  dskCrossfadeAnimation_.stop();
  dskFadeInAnimation_.stop();
  dskFadeOutAnimation_.stop();
  clearPendingPowerPointNavigation();
  if (bgmController_) {
    bgmController_->requestShutdown();
  }
  audioController_.setBgmActive(false);
  audioController_.setActive(false);
  if (playlistModel_) {
    playlistModel_->clearDskItem();
  }
  player_.requestShutdown();
}

bool ProgramPlayoutController::shutdownComplete() { return player_.shutdownComplete(); }

void ProgramPlayoutController::shutdown() { requestShutdown(); }

void ProgramPlayoutController::setAutoSelectNextItemAfterOnAir(bool enabled) {
  autoSelectNextItemAfterOnAir_ = enabled;
}

void ProgramPlayoutController::setAllowSameItemOnAirRetake(bool enabled) {
  if (allowSameItemOnAirRetake_ == enabled) {
    return;
  }
  allowSameItemOnAirRetake_ = enabled;
  scheduleSelectedItemCue();
}

void ProgramPlayoutController::setBgmControlsVisible(bool visible) {
  if (bgmController_) {
    bgmController_->setControlsVisibleRequested(visible);
  }
}

bool ProgramPlayoutController::bgmIsActive() const {
  return bgmController_ && bgmController_->isActive();
}

bool ProgramPlayoutController::validateMacroTarget(quint64 itemId,
                                                   QString *error) const {
  if (shutdown_ || !playlistModel_ || !programPanel_ || !isAvailable()) {
    return macroOperationError(error, QStringLiteral("ON AIR is unavailable."));
  }
  const auto item = playlistModel_->itemState(itemId);
  if (!item || itemId == 0) {
    return macroOperationError(error, QStringLiteral("The macro target no longer exists."));
  }
  if (item->sourceType != PlaylistSourceType::File ||
      (item->mediaType != PlaylistMediaType::Video &&
       item->mediaType != PlaylistMediaType::Audio &&
       item->mediaType != PlaylistMediaType::StillImage) ||
      playlistModel_->playlistMode(item->playlistNumber) != PlaylistMode::Standard) {
    return macroOperationError(error, QStringLiteral("Macro preparation and ON AIR support video, audio and still-image files in standard playlists only."));
  }
  if (playlistModel_->itemConversionInProgress(itemId)) {
    return macroOperationError(error, QStringLiteral("The macro target is still being imported or converted."));
  }
  if (item->filePath.isEmpty() || playlistModel_->itemMediaOffline(itemId) ||
      !QFileInfo::exists(item->filePath)) {
    return macroOperationError(error, QStringLiteral("The macro target file is unavailable."));
  }
  if (playlistModel_->dskItemId() == itemId) {
    return macroOperationError(error, QStringLiteral("The macro target is currently used by DSK."));
  }
  if (!allowSameItemOnAirRetake_ && playlistModel_->onAirItemId() == itemId) {
    return macroOperationError(error, QStringLiteral("Same-item ON AIR retake is disabled."));
  }
  if (error) error->clear();
  return true;
}

bool ProgramPlayoutController::requestMacroCue(quint64 requestId, quint64 itemId,
                                               QString *error) {
  if (requestId == 0 || macroRequest_ || macroTransportRequest_) {
    return macroOperationError(error, QStringLiteral("A macro preparation or ON AIR request is already pending, or its ID is invalid."));
  }
  if (!validateMacroTarget(itemId, error)) return false;
  maybePrepareNextItem(programPositionMs(), playbackEnd(programDurationMs()));
  if (pendingTakeItemId_ || nextTransition_.itemId ||
      nextTransition_.preparationAttempted || nextTransition_.due) {
    return macroOperationError(error, QStringLiteral("Manual ON AIR or automatic NEXT/GOTO/REPEAT is already using the standby player."));
  }
  macroCueOwner_ = cueRequestFor(*playlistModel_->itemState(itemId));
  macroRequest_ = MacroRequest{requestId};
  scheduleCueRequest(macroCueOwner_, true);
  updateMacroRequest();
  return true;
}

bool ProgramPlayoutController::requestMacroTake(
    quint64 requestId, quint64 itemId, bool requireReady, bool crossfade,
    qint64 crossfadeDurationMs, QString *error) {
  if (requestId == 0 || macroRequest_ || macroTransportRequest_) {
    return macroOperationError(error, QStringLiteral("A macro preparation or ON AIR request is already pending, or its ID is invalid."));
  }
  if (!validateMacroTarget(itemId, error)) return false;
  if (crossfade && (crossfadeDurationMs < kMinimumItemFadeDurationMs ||
                    crossfadeDurationMs > kMaximumItemFadeDurationMs)) {
    return macroOperationError(error, QStringLiteral("The macro crossfade duration is outside the supported range."));
  }
  maybePrepareNextItem(programPositionMs(), playbackEnd(programDurationMs()));
  if (pendingTakeItemId_ || nextTransition_.itemId ||
      nextTransition_.preparationAttempted || nextTransition_.due) {
    return macroOperationError(error, QStringLiteral("Manual ON AIR or automatic NEXT/GOTO/REPEAT is already using the standby player."));
  }
  const CueRequest request = cueRequestFor(*playlistModel_->itemState(itemId));
  if (requireReady && (cueReplacementPending_ || !cue_ || *cue_ != request ||
                       player_.cueStatus() != DualDeckPlayer::CueStatus::Ready)) {
    return macroOperationError(error, QStringLiteral("Timed ON AIR requires this exact item to be ready."));
  }
  macroCueOwner_ = request;
  macroRequest_ = MacroRequest{requestId, true, crossfade, crossfadeDurationMs};
  if (!requireReady) scheduleCueRequest(request, true);
  updateMacroRequest();
  return true;
}

void ProgramPlayoutController::cancelMacroRequests() {
  // A cancelled preroll may still finish, but only this request grants TAKE.
  // Manual pendingTakeItemId_ and existing automatic transitions are untouched.
  macroRequest_.reset();
  macroCueOwner_.reset();
  if (macroTransportRequest_) {
    macroTransportRequest_.reset();
    onAirTimedStillRestorePending_ = false;
  }
}

void ProgramPlayoutController::finishMacroRequest(bool success,
                                                  const QString &detail) {
  if (!macroRequest_) return;
  const auto request = *macroRequest_;
  macroRequest_.reset();
  if (!success || request.take) macroCueOwner_.reset();
  emit macroRequestFinished(request.requestId, success, detail);
}

void ProgramPlayoutController::interruptMacroReservation(const QString &reason) {
  if (!macroCueOwner_ && !macroRequest_ && !macroTransportRequest_) return;
  cancelMacroRequests();
  // Synchronous notification lets the runner pause before selection replaces CUE.
  emit macroIntervention(reason);
}

void ProgramPlayoutController::failMacroReservation(const QString &detail) {
  if (!macroCueOwner_ && !macroRequest_ && !macroTransportRequest_) return;
  const auto request = macroRequest_;
  const auto transport = macroTransportRequest_;
  cancelMacroRequests();
  emit macroReservationFailed(detail);
  if (request) emit macroRequestFinished(request->requestId, false, detail);
  if (transport) emit macroRequestFinished(transport->requestId, false, detail);
}

void ProgramPlayoutController::updateMacroRequest() {
  if (!macroCueOwner_) return;
  if (shutdown_ || !playlistModel_) {
    failMacroReservation(QStringLiteral("ON AIR is unavailable."));
    return;
  }
  const auto item = playlistModel_->itemState(macroCueOwner_->itemId);
  if (!item || cueRequestFor(*item) != *macroCueOwner_) {
    interruptMacroReservation(QStringLiteral("The macro standby item changed."));
    return;
  }
  if (cueReplacementPending_) return;
  if (playlistModel_->cueItemId() == item->id &&
      playlistModel_->cueState() == PlaylistCueState::Error) {
    failMacroReservation(playlistModel_->cueDetail());
    return;
  }
  if (!cue_ || *cue_ != *macroCueOwner_) {
    failMacroReservation(QStringLiteral("The requested macro item could not be prepared."));
    return;
  }
  if (player_.cueStatus() == DualDeckPlayer::CueStatus::Error) {
    failMacroReservation(player_.cueErrorDetail());
    return;
  }
  if (player_.cueStatus() != DualDeckPlayer::CueStatus::Ready || !macroRequest_) return;
  // Bus polling resolves CUE before normal end-preload polling. Give an END
  // transition that became due during this preroll its ordinary priority before
  // a ready macro TAKE can change the active item and hide that boundary.
  maybePrepareNextItem(programPositionMs(), playbackEnd(programDurationMs()));
  if (!macroRequest_ || !macroCueOwner_) return;
  // Recheck availability once at the completion/TAKE boundary. A long timed
  // wait must not repeatedly stat a potentially remote file on the UI thread.
  QString error;
  if (!validateMacroTarget(item->id, &error)) {
    failMacroReservation(error);
    return;
  }
  if (!macroRequest_->take) {
    finishMacroRequest(true);
    return;
  }
  // Clear ownership before the existing TAKE emits synchronous model signals.
  const auto request = *macroRequest_;
  cancelMacroRequests();
  const bool success = takeCueOnAir(*item, request.crossfade,
                                  request.crossfadeDurationMs);
  emit macroRequestFinished(request.requestId, success,
      success ? QString{} : QStringLiteral("The prepared macro item could not be taken ON AIR."));
}

bool ProgramPlayoutController::executeMacroTransport(const QString &operation,
                                                     QString *error) {
  if (shutdown_ || !playlistModel_ || !isAvailable()) {
    return macroOperationError(error, QStringLiteral("ON AIR is unavailable."));
  }
  if (operation == QStringLiteral("offAir")) {
    cancelMacroRequests();
    clearOnAir();
    if (playlistModel_->onAirItemId() != 0) {
      return macroOperationError(error, QStringLiteral("ON AIR could not switch OFF AIR."));
    }
  } else {
    const auto *media = onAirStateMachine_.preparedMedia();
    const auto item = media ? playlistModel_->itemState(media->itemId) : std::nullopt;
    if (!item || item->sourceType != PlaylistSourceType::File ||
        (item->mediaType != PlaylistMediaType::Video &&
         item->mediaType != PlaylistMediaType::Audio &&
         item->mediaType != PlaylistMediaType::StillImage)) {
      return macroOperationError(error, QStringLiteral("Macro transport requires a supported file ON AIR."));
    }
    if (operation == QStringLiteral("play")) {
      playOnAir();
      if (!programIsPlaying()) {
        // This synchronous operation cannot promise completion of a deferred
        // still-frame restore. Do not leave a later Play queued after failure.
        onAirTimedStillRestorePending_ = false;
        return macroOperationError(error, QStringLiteral("ON AIR playback did not start."));
      }
    } else if (operation == QStringLiteral("pause")) {
      pauseOnAir();
      if (programIsPlaying()) {
        return macroOperationError(error, QStringLiteral("ON AIR did not pause."));
      }
    } else {
      return macroOperationError(error, QStringLiteral("Unknown macro transport operation."));
    }
  }
  if (error) error->clear();
  return true;
}

bool ProgramPlayoutController::requestMacroTransport(
    quint64 requestId, const QString &operation, QString *error) {
  if (requestId == 0 || macroRequest_ || macroTransportRequest_) {
    return macroOperationError(error, QStringLiteral("A macro operation is already pending, or its ID is invalid."));
  }
  if (operation != QStringLiteral("play")) {
    if (!executeMacroTransport(operation, error)) return false;
    emit macroRequestFinished(requestId, true, {});
    return true;
  }
  if (shutdown_ || !playlistModel_ || !isAvailable()) {
    return macroOperationError(error, QStringLiteral("ON AIR is unavailable."));
  }
  const auto *media = onAirStateMachine_.preparedMedia();
  const auto item = media ? playlistModel_->itemState(media->itemId) : std::nullopt;
  if (!item || item->sourceType != PlaylistSourceType::File ||
      (item->mediaType != PlaylistMediaType::Video &&
       item->mediaType != PlaylistMediaType::Audio &&
       item->mediaType != PlaylistMediaType::StillImage)) {
    return macroOperationError(error, QStringLiteral("Macro transport requires a supported file ON AIR."));
  }
  macroTransportRequest_ = MacroTransportRequest{requestId, item->id};
  playOnAir();
  updateMacroTransportRequest();
  if (error) error->clear();
  return true;
}

void ProgramPlayoutController::finishMacroTransportRequest(
    bool success, const QString &detail) {
  if (!macroTransportRequest_) return;
  const quint64 requestId = macroTransportRequest_->requestId;
  macroTransportRequest_.reset();
  if (!success) onAirTimedStillRestorePending_ = false;
  emit macroRequestFinished(requestId, success, detail);
}

void ProgramPlayoutController::updateMacroTransportRequest() {
  if (!macroTransportRequest_) return;
  if (!playlistModel_ || playlistModel_->onAirItemId() != macroTransportRequest_->itemId) {
    finishMacroTransportRequest(false, QStringLiteral("The ON AIR item changed while macro playback was starting."));
  } else if (programIsPlaying()) {
    finishMacroTransportRequest(true);
  } else if (!onAirTimedStillRestorePending_) {
    finishMacroTransportRequest(false, QStringLiteral("ON AIR playback did not start."));
  }
}

bool ProgramPlayoutController::setMacroPlaybackRate(double rate, QString *error) {
  const auto *media = onAirStateMachine_.preparedMedia();
  const auto item = media && playlistModel_ ? playlistModel_->itemState(media->itemId)
                                           : std::nullopt;
  if (shutdown_ || !item || item->sourceType != PlaylistSourceType::File ||
      (item->mediaType != PlaylistMediaType::Video && item->mediaType != PlaylistMediaType::Audio) ||
      (rate != 0.5 && rate != 0.75 && rate != 1.0 && rate != 1.25 &&
       rate != 1.5 && rate != 2.0 && rate != 3.0)) {
    return macroOperationError(error, QStringLiteral("The macro playback rate or current media is unsupported."));
  }
  setPlaybackRate(rate);
  if (!qFuzzyCompare(playbackRate_, rate)) {
    return macroOperationError(error, QStringLiteral("The macro playback rate was not applied."));
  }
  if (error) error->clear();
  return true;
}

int ProgramPlayoutController::bgmPlaylistNumber() const {
  return bgmController_ ? bgmController_->activePlaylistNumber() : 0;
}

void ProgramPlayoutController::clearOnAir() {
  failMacroReservation(QStringLiteral("OFF AIR released the macro standby reservation."));
  const quint64 previousOnAirItemId =
      playlistModel_ ? playlistModel_->onAirItemId() : 0;
  const DualDeckPlayer::ClearMediaResult result = player_.clearMedia();
  if (result != DualDeckPlayer::ClearMediaResult::Cleared) {
    showWarning(QStringLiteral(
        "Failed to switch OFF AIR. The playback state was not changed."));
    return;
  }
  resetFtb();
  resetItemFadeState();
  onAirTimedStillRestorePending_ = false;
  onAirTimedStillClock_.reset();
  onAirTimedStillCompletionDue_ = false;
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  seekLockTicks_ = 0;
  pendingCue_.reset();
  cueReplacementPending_ = false;
  cue_.reset();
  pendingTakeItemId_.reset();
  clearPendingPowerPointNavigation();
  resetNextTransitionState();
  onAirStateMachine_.reset();
  lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
  if (programPanel_) {
    programPanel_->setTimelineDuration(-1);
    programPanel_->setTimelinePosition(0);
    programPanel_->clearRangeMarkers();
    programPanel_->setRemainingTime(0);
    programPanel_->setItemFaderValue(0);
    programPanel_->setItemDelayFrames(0);
    programPanel_->setItemEqGains({});
    programPanel_->clearFile();
    programPanel_->clearEndMode();
    programPanel_->setPlaybackRate(1.0);
    programPanel_->setPlaybackRateControlsEnabled(false);
    programPanel_->setPowerPointNavigationState(false, false, false);
    programPanel_->setPlaybackState(false, false);
    programPanel_->monitorWindow()->repaint();
  }
  if (playlistModel_) {
    playlistModel_->clearCueItem();
    playlistModel_->clearOnAirItem();
  }
  if (previousOnAirItemId != 0) {
    emit onAirChanged(false);
  }
  audioController_.setActive(false);
  playbackRate_ = 1.0;
  if (videoWindow_) {
    videoWindow_->repaint();
  }
  selectNextItemAfterOnAir(previousOnAirItemId);
  scheduleSelectedItemCue();
}

void ProgramPlayoutController::takeSelectedPlaylistItemOnAir() {
  interruptMacroReservation(QStringLiteral("Manual ON AIR replaced the macro standby reservation."));
  if (!playlistPanel_ || !playlistModel_) {
    return;
  }
  const auto selected = playlistPanel_->selectedItemState();
  if (!selected || selected->id == 0 || selected->filePath.isEmpty()) {
    showWarning(QStringLiteral("Select an item to take ON AIR."));
    return;
  }
  if (playlistModel_->playlistMode(selected->playlistNumber) ==
      PlaylistMode::Bgm) {
    pendingTakeItemId_.reset();
    showWarning(vtrpon::uiText("program.warning.bgm_mode_on_air"));
    return;
  }
  if (!playlistPanel_->allowItemAction(selected->id,
                                       PlaylistItemAction::OnAir)) {
    pendingTakeItemId_.reset();
    return;
  }
  if (!allowSameItemOnAirRetake_ &&
      selected->id == playlistModel_->onAirItemId()) {
    pendingTakeItemId_.reset();
    return;
  }
  if (nextTransition_.itemId) {
    cancelNextTransition(false);
  }
  const CueRequest request = cueRequestFor(*selected);
  if (!cue_ || *cue_ != request ||
      player_.cueStatus() == DualDeckPlayer::CueStatus::Error) {
    if (cue_ && *cue_ == request && request.sourceType == PlaylistSourceType::File
        && player_.cueStatus() == DualDeckPlayer::CueStatus::Error) {
      scheduleCueRequest(request, true);
    } else {
      scheduleSelectedItemCue();
    }
  }
  if (!cue_ || *cue_ != request) {
    if (cueReplacementPending_ && pendingCue_ && *pendingCue_ == request) {
      pendingTakeItemId_ = selected->id;
      return;
    }
    pendingTakeItemId_.reset();
    return;
  }
  const DualDeckPlayer::CueStatus status = player_.cueStatus();
  if (status == DualDeckPlayer::CueStatus::Ready) {
    takeCueOnAir(*selected);
    return;
  }
  if (status == DualDeckPlayer::CueStatus::Error) {
    pendingTakeItemId_.reset();
    showWarning(player_.cueErrorDetail());
    return;
  }
  pendingTakeItemId_ = selected->id;
}

bool ProgramPlayoutController::takeCueOnAir(const PlaylistItemState &item) {
  return takeCueOnAir(
      item, playlistPanel_ && playlistPanel_->crossfadeEnabled(),
      playlistPanel_ ? playlistPanel_->crossfadeDurationMs()
                     : kDefaultItemFadeDurationMs);
}

bool ProgramPlayoutController::takeCueOnAir(const PlaylistItemState &item,
                                            bool crossfade,
                                            qint64 crossfadeDurationMs) {
  if (!programPanel_ || !playlistModel_) {
    return false;
  }
  if (item.sourceType == PlaylistSourceType::Uvc && playlistModel_->itemMediaOffline(item.id)) {
    pendingTakeItemId_.reset();
    return false;
  }
  if (playlistModel_->playlistMode(item.playlistNumber) ==
      PlaylistMode::Bgm) {
    pendingTakeItemId_.reset();
    showWarning(vtrpon::uiText("program.warning.bgm_mode_on_air"));
    return false;
  }
  const CueRequest request = cueRequestFor(item);
  if (!cue_ || *cue_ != request) {
    showWarning(
        QStringLiteral("Could not take the selected item ON AIR."));
    pendingTakeItemId_.reset();
    return false;
  }
  const quint64 outgoingItemId = playlistModel_->onAirItemId();
  const qreal outgoingHighlightOpacity = std::clamp(
      itemFadeInProgress_ * itemFadeOutProgress_, 0.0, 1.0);
  const bool live = item.sourceType != PlaylistSourceType::File;
  const qreal initialFadeProgress = item.settings.fadeInEnabled ? 0.0 : 1.0;
  player_.setCueDelayFrames(item.settings.nominalAudioDelayFrames);
  const auto itemEq = item.settings.nominalEqTenthsDb;
  player_.setCueEq(itemEq[0] / 10.0, itemEq[1] / 10.0, itemEq[2] / 10.0);
  DualDeckPlayer::TakeCueFailure takeFailure;
  if (!player_.takeCue(crossfade,
                       static_cast<float>(initialFadeProgress), &takeFailure)) {
    if (takeFailure == DualDeckPlayer::TakeCueFailure::AudioOutputUnavailable) {
      if (userInfoCenter_) {
        userInfoCenter_->showAudioOutputWarning(
            vtrpon::LocalizedText("program.warning.audio_output_unavailable"));
      }
    } else if (takeFailure == DualDeckPlayer::TakeCueFailure::AudioOutputChanging) {
      showWarning(vtrpon::uiText("program.warning.audio_output_changing"));
    } else {
      showWarning(QStringLiteral("Could not take the selected item ON AIR."));
    }
    pendingTakeItemId_.reset();
    return false;
  }
  clearPendingPowerPointNavigation();
  if (!onAirStateMachine_.prepare(item.id)) {
    if (player_.crossfadeActive()) {
      (void)player_.setCrossfadeProgress(1.0F);
    }
    const QString detail =
        QStringLiteral("Could not retrieve the playlist item to take ON AIR.");
    setFault(detail);
    showWarning(detail);
    pendingTakeItemId_.reset();
    return false;
  }

  const auto startMode = onAirStateMachine_.start();
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (!startMode || !onAirMedia) {
    if (player_.crossfadeActive()) {
      (void)player_.setCrossfadeProgress(1.0F);
    }
    onAirStateMachine_.fail();
    const QString detail =
        QStringLiteral("Could not retrieve the latest item information when ON AIR started.");
    setFault(detail);
    showWarning(detail);
    return false;
  }
  onAirTimedStillRestorePending_ = false;
  onAirTimedStillClock_.configure(
      item.settings.inPointMs, timedStillEndPosition(item),
      *startMode == PlaylistStartMode::Play);
  onAirTimedStillElapsedTimer_.restart();
  onAirTimedStillCompletionDue_ = false;

  programPanel_->setItemFaderValue(item.settings.nominalVolumeTenthsDb);
  programPanel_->setItemDelayFrames(item.settings.nominalAudioDelayFrames);
  programPanel_->setItemEqGains(item.settings.nominalEqTenthsDb);
  const QFileInfo fileInfo(onAirMedia->filePath);
  const QString sourceName = live
      ? item.displayName : fileInfo.fileName();
  const QString sourceDetail = live
      ? item.filePath : fileInfo.absoluteFilePath();
  programPanel_->setFile(sourceName, sourceDetail);
  playbackRate_ = 1.0;
  programPanel_->setPlaybackRate(playbackRate_);
  programPanel_->setPlaybackRateControlsEnabled(
      item.sourceType == PlaylistSourceType::File &&
      (item.mediaType == PlaylistMediaType::Video ||
       item.mediaType == PlaylistMediaType::Audio ||
       item.mediaType == PlaylistMediaType::Unknown));
  const qint64 duration = programDurationMs();
  if (live) {
    programPanel_->setTimelineDuration(-1);
    programPanel_->setTimelinePosition(0);
    programPanel_->clearRangeMarkers();
    programPanel_->setRemainingLive();
  } else {
    programPanel_->setTimelineDuration(duration);
    programPanel_->setTimelinePosition(onAirMedia->inPointMs);
    programPanel_->setRangeMarkers(onAirMedia->inPointMs,
                                    onAirMedia->outPointMs);
    programPanel_->setRemainingTime(onAirMedia->outPointMs
                                        ? *onAirMedia->outPointMs -
                                              onAirMedia->inPointMs
                                        : 0);
  }
  pendingCue_.reset();
  cueReplacementPending_ = false;
  cue_.reset();
  pendingTakeItemId_.reset();
  lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
  playlistModel_->clearCueItem();
  playlistModel_->setOnAirItemId(item.id);
  emit onAirChanged(true);
  refreshOnAirEndModePresentation();
  crossfadeOutgoingItemId_ =
      player_.crossfadeActive() && outgoingItemId != item.id
          ? outgoingItemId
          : 0;
  crossfadeProgress_ = crossfadeOutgoingItemId_ != 0 ? 0.0 : 1.0;
  crossfadeOutgoingStartOpacity_ = crossfadeOutgoingItemId_ != 0
                                       ? outgoingHighlightOpacity
                                       : 0.0;
  itemFadeInProgress_ = initialFadeProgress;
  itemFadeOutProgress_ = 1.0;
  itemFadeOutStarted_ = false;
  applyItemFadeProgress();
  bool playbackStarted = true;
  if (onAirTimedStillClock_.active()) {
    audioController_.setActive(false);
    clearFault();
  } else if (live || *startMode == PlaylistStartMode::Play) {
    if (!player_.play()) {
      playbackStarted = false;
      if (player_.crossfadeActive()) {
        (void)player_.setCrossfadeProgress(1.0F);
      }
      finishOnAirHighlightCrossfade();
      onAirStateMachine_.fail();
      const QString detail =
          QStringLiteral("The item was taken ON AIR, but Start Mode \"Play\" could not be started.");
      audioController_.setActive(false);
      setFault(detail);
      showWarning(detail);
    } else {
      audioController_.setActive(player_.currentSourceHasAudio());
      clearFault();
    }
  } else {
    audioController_.setActive(false);
    clearFault();
  }
  if (playbackStarted) {
    startCrossfade(crossfadeDurationMs);
    startItemFadeIn(item.settings);
  }
  programPanel_->setPlaybackState(true, programIsPlaying());
  resetNextTransitionState();
  return true;
}

void ProgramPlayoutController::startCrossfade(qint64 durationMs) {
  if (!player_.crossfadeActive()) {
    return;
  }
  crossfadeAnimation_.stop();
  crossfadeAnimation_.setDuration(static_cast<int>(std::clamp<qint64>(
      durationMs, kMinimumItemFadeDurationMs,
      kMaximumItemFadeDurationMs)));
  crossfadeAnimation_.setStartValue(0.0F);
  crossfadeAnimation_.setEndValue(1.0F);
  crossfadeAnimation_.start();
}

void ProgramPlayoutController::startItemFadeIn(
    const PlaylistItemSettings &settings) {
  itemFadeInAnimation_.stop();
  itemFadeOutAnimation_.stop();
  itemFadeOutProgress_ = 1.0;
  itemFadeOutStarted_ = false;
  if (!settings.fadeInEnabled) {
    itemFadeInProgress_ = 1.0;
    applyItemFadeProgress();
    return;
  }
  itemFadeInProgress_ = 0.0;
  applyItemFadeProgress();
  itemFadeInAnimation_.setDuration(static_cast<int>(std::clamp<qint64>(
      settings.fadeInDurationMs, kMinimumItemFadeDurationMs,
      kMaximumItemFadeDurationMs)));
  itemFadeInAnimation_.setStartValue(0.0);
  itemFadeInAnimation_.setEndValue(1.0);
  itemFadeInAnimation_.start();
}

void ProgramPlayoutController::maybeStartItemFadeOut(qint64 positionMs,
                                                      qint64 endPositionMs) {
  if (itemFadeOutStarted_ || positionMs < 0 || endPositionMs < positionMs ||
      !programIsPlaying() || !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (!onAirMedia) {
    return;
  }
  const auto state = playlistModel_->itemState(onAirMedia->itemId);
  if (!state || !state->settings.fadeOutEnabled) {
    return;
  }
  // A loop boundary is a deck handoff, not a fade through the background.
  if (usesAlternatingRepeat()) {
    return;
  }
  const qint64 configuredDuration = std::clamp<qint64>(
      state->settings.fadeOutDurationMs, kMinimumItemFadeDurationMs,
      kMaximumItemFadeDurationMs);
  const qint64 remainingMs = endPositionMs - positionMs;
  if (remainingMs > configuredDuration) {
    return;
  }
  itemFadeOutStarted_ = true;
  itemFadeOutAnimation_.stop();
  itemFadeOutProgress_ = 1.0;
  applyItemFadeProgress();
  itemFadeOutAnimation_.setDuration(
      static_cast<int>((std::max)(qint64{1}, remainingMs)));
  itemFadeOutAnimation_.setStartValue(1.0);
  itemFadeOutAnimation_.setEndValue(0.0);
  itemFadeOutAnimation_.start();
}

void ProgramPlayoutController::finishItemFadeForEnd(bool fadeOutEnabled) {
  itemFadeInAnimation_.stop();
  itemFadeOutAnimation_.stop();
  itemFadeInProgress_ = 1.0;
  itemFadeOutProgress_ = fadeOutEnabled ? 0.0 : 1.0;
  itemFadeOutStarted_ = fadeOutEnabled;
  applyItemFadeProgress();
}

void ProgramPlayoutController::revealItemAfterRepeat() {
  itemFadeInAnimation_.stop();
  itemFadeOutAnimation_.stop();
  itemFadeInProgress_ = 1.0;
  itemFadeOutProgress_ = 1.0;
  itemFadeOutStarted_ = false;
  applyItemFadeProgress();
}

void ProgramPlayoutController::applyItemFadeProgress() {
  const qreal progress =
      std::clamp(itemFadeInProgress_ * itemFadeOutProgress_, 0.0, 1.0);
  (void)player_.setItemVideoFadeProgress(static_cast<float>(progress));
  if (programPanel_) {
    programPanel_->setItemFadeProgress(progress);
  }
  applyOnAirHighlightProgress(progress);
}

void ProgramPlayoutController::applyOnAirHighlightProgress(qreal itemProgress) {
  if (!playlistPanel_ || !playlistModel_) {
    return;
  }
  const quint64 incomingItemId = playlistModel_->onAirItemId();
  if (crossfadeOutgoingItemId_ != 0) {
    playlistPanel_->setOnAirHighlightOpacity(
        crossfadeOutgoingItemId_, crossfadeOutgoingStartOpacity_
                                      * (1.0 - crossfadeProgress_));
    itemProgress *= crossfadeProgress_;
  }
  playlistPanel_->setOnAirHighlightOpacity(incomingItemId, itemProgress);
}

void ProgramPlayoutController::finishOnAirHighlightCrossfade() {
  if (playlistPanel_ && crossfadeOutgoingItemId_ != 0) {
    playlistPanel_->setOnAirHighlightOpacity(crossfadeOutgoingItemId_, 0.0);
  }
  crossfadeOutgoingItemId_ = 0;
  crossfadeProgress_ = 1.0;
  crossfadeOutgoingStartOpacity_ = 0.0;
  applyOnAirHighlightProgress(std::clamp(
      itemFadeInProgress_ * itemFadeOutProgress_, 0.0, 1.0));
}

void ProgramPlayoutController::resetItemFadeState() {
  itemFadeInAnimation_.stop();
  itemFadeOutAnimation_.stop();
  itemFadeInProgress_ = 1.0;
  itemFadeOutProgress_ = 1.0;
  itemFadeOutStarted_ = false;
}

void ProgramPlayoutController::scheduleSelectedItemCue() {
  if (!playlistPanel_ || !playlistModel_) {
    return;
  }
  if (nextTransition_.itemId || macroCueOwner_) {
    return;
  }
  const auto selected = playlistPanel_->selectedItemState();
  const std::optional<CueRequest> requestedCue =
      selected && !playlistModel_->itemConversionInProgress(selected->id) &&
              (selected->sourceType != PlaylistSourceType::Uvc ||
               !playlistModel_->itemMediaOffline(selected->id)) &&
              playlistModel_->playlistMode(selected->playlistNumber) !=
                  PlaylistMode::Bgm &&
              (allowSameItemOnAirRetake_ ||
               selected->id != playlistModel_->onAirItemId())
          ? std::optional<CueRequest>{cueRequestFor(*selected)}
          : std::nullopt;
  scheduleCueRequest(requestedCue);
}

void ProgramPlayoutController::scheduleCueRequest(
    std::optional<CueRequest> requestedCue, bool retryFailed) {
  if (!playlistModel_) {
    return;
  }
  if (macroCueOwner_ && requestedCue != macroCueOwner_) {
    interruptMacroReservation(QStringLiteral("The macro standby reservation was replaced."));
  }
  if (powerPointConversionInProgress_ && requestedCue &&
      requestedCue->sourceType == PlaylistSourceType::PowerPoint) {
    // The conversion and native CUE share PowerPoint. Keep the conversion
    // item alive; finishPowerPointConversion() prepares the current selection.
    if (pendingTakeItemId_ &&
        *pendingTakeItemId_ == requestedCue->itemId) {
      pendingTakeItemId_.reset();
    }
    return;
  }
  if (requestedCue && cue_ && *requestedCue == *cue_ &&
      !((retryFailed || requestedCue->sourceType == PlaylistSourceType::PowerPoint) &&
        player_.cueStatus() == DualDeckPlayer::CueStatus::Error)) {
    pendingCue_.reset();
    if (cueReplacementPending_) {
      lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
    }
    cueReplacementPending_ = false;
    if (pendingTakeItemId_ && *pendingTakeItemId_ != requestedCue->itemId) {
      pendingTakeItemId_.reset();
    }
    return;
  }
  pendingCue_ = requestedCue;
  if (pendingTakeItemId_ &&
      (!pendingCue_ || *pendingTakeItemId_ != pendingCue_->itemId)) {
    pendingTakeItemId_.reset();
  }
  cueReplacementPending_ = true;
  if (cueReplacementMustWait()) {
    if (pendingCue_) {
      playlistModel_->setCueItemState(pendingCue_->itemId,
                                      PlaylistCueState::Pending);
    } else {
      playlistModel_->clearCueItem();
    }
    return;
  }
  applyPendingCue();
}

bool ProgramPlayoutController::cueReplacementMustWait() {
  // Loading/failed File graphs are isolated and replaced by clearCue(), never
  // rapidly NULL/reused. A crossfade still owns both physical deck layers.
  return player_.crossfadeActive();
}

void ProgramPlayoutController::applyPendingCue() {
  cueReplacementPending_ = false;
  if (!player_.clearCue()) {
    pendingTakeItemId_.reset();
    lastCueStatus_ = DualDeckPlayer::CueStatus::Error;
    const quint64 failedItemId = cue_          ? cue_->itemId
                                 : pendingCue_ ? pendingCue_->itemId
                                               : 0;
    if (failedItemId != 0) {
      playlistModel_->setCueItemState(failedItemId, PlaylistCueState::Error,
                                      player_.cueErrorDetail());
    }
    return;
  }
  cue_.reset();
  lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
  if (!pendingCue_ ||
      (!allowSameItemOnAirRetake_ &&
       pendingCue_->itemId == playlistModel_->onAirItemId() &&
       !ownsAutomaticTransitionCue(*pendingCue_))) {
    pendingCue_.reset();
    playlistModel_->clearCueItem();
    return;
  }
  playlistModel_->setCueItemState(pendingCue_->itemId,
                                  PlaylistCueState::Pending);
  beginPendingCue();
}

void ProgramPlayoutController::beginPendingCue() {
  if (!pendingCue_ || !playlistModel_) {
    return;
  }
  if (pendingCue_->sourceType == PlaylistSourceType::File &&
      !QFileInfo::exists(pendingCue_->path)) {
    pendingTakeItemId_.reset();
    playlistModel_->setCueItemState(
        pendingCue_->itemId, PlaylistCueState::Error,
        QStringLiteral("The registered file could not be found.\n") +
            pendingCue_->path);
    return;
  }
  std::optional<DualDeckPlayer::DeckSource::Type> deckSourceType;
  switch (pendingCue_->sourceType) {
  case PlaylistSourceType::File:
    deckSourceType = DualDeckPlayer::DeckSource::Type::File;
    break;
  case PlaylistSourceType::Uvc:
    deckSourceType = DualDeckPlayer::DeckSource::Type::Uvc;
    break;
  case PlaylistSourceType::PowerPoint:
    deckSourceType = DualDeckPlayer::DeckSource::Type::PowerPoint;
    break;
  case PlaylistSourceType::Pdf:
    deckSourceType = DualDeckPlayer::DeckSource::Type::Pdf;
    break;
  }
  if (!deckSourceType) {
    pendingTakeItemId_.reset();
    playlistModel_->setCueItemState(
        pendingCue_->itemId, PlaylistCueState::Error,
        QStringLiteral("This input type is not yet supported for ON AIR playout."));
    return;
  }
  const DualDeckPlayer::DeckSource source{
      *deckSourceType,
      pendingCue_->path,
      pendingCue_->audioDeviceId,
      pendingCue_->inPointMs,
      pendingCue_->outPointMs.value_or(-1),
      pendingCue_->playbackRate,
  };
  const bool cueSucceeded = player_.cue(source);
  if (!cueSucceeded) {
    pendingTakeItemId_.reset();
    playlistModel_->setCueItemState(
        pendingCue_->itemId, PlaylistCueState::Error, player_.cueErrorDetail());
    return;
  }
  cue_ = pendingCue_;
  lastCueStatus_ = DualDeckPlayer::CueStatus::Loading;
  playlistModel_->setCueItemState(cue_->itemId, PlaylistCueState::Loading);
}

void ProgramPlayoutController::updateCueUi() {
  if (cueReplacementPending_) {
    if (cueReplacementMustWait()) {
      return;
    }
    applyPendingCue();
  }
  if (!cue_ || !playlistModel_) {
    updateMacroRequest();
    return;
  }
  const DualDeckPlayer::CueStatus status = player_.cueStatus();
  if (status == lastCueStatus_) {
    updateMacroRequest();
    return;
  }
  lastCueStatus_ = status;
  if (status == DualDeckPlayer::CueStatus::Ready) {
    playlistModel_->setCueItemState(cue_->itemId, PlaylistCueState::Ready);
    if (nextTransition_.due && nextTransition_.itemId &&
        *nextTransition_.itemId == cue_->itemId) {
      (void)takePreparedNextItem();
      return;
    }
    if (macroRequest_) {
      updateMacroRequest();
      return;
    }
    const auto selected =
        playlistPanel_ ? playlistPanel_->selectedItemState() : std::nullopt;
    const CueRequest selectedRequest =
        selected ? cueRequestFor(*selected) : CueRequest{};
    if (pendingTakeItemId_ && *pendingTakeItemId_ == cue_->itemId && selected &&
        selectedRequest == *cue_) {
      takeCueOnAir(*selected);
    }
  } else if (status == DualDeckPlayer::CueStatus::Error) {
    if (cue_->sourceType == PlaylistSourceType::Uvc && uvcSourceSessions_
        && !vtrpon::isLiveAudioSource(cue_->path)) {
      const auto path = cue_->path;
      const auto detail = player_.cueErrorDetail();
      pendingTakeItemId_.reset();
      uvcSourceSessions_->reportSourceFailure(path, detail);
      return;
    }
    playlistModel_->setCueItemState(cue_->itemId, PlaylistCueState::Error,
                                    player_.cueErrorDetail());
    pendingTakeItemId_.reset();
    if (nextTransition_.due && nextTransition_.itemId &&
        *nextTransition_.itemId == cue_->itemId) {
      (void)takePreparedNextItem();
      return;
    }
  } else if (status == DualDeckPlayer::CueStatus::Loading) {
    playlistModel_->setCueItemState(cue_->itemId, PlaylistCueState::Loading);
  }
  updateMacroRequest();
}

void ProgramPlayoutController::maybePrepareNextItem(qint64 positionMs,
                                                     qint64 endPositionMs) {
  maybePrepareNextItem(PlayoutTarget::Program, positionMs, endPositionMs);
}

void ProgramPlayoutController::maybePrepareNextDskItem(
    qint64 positionMs, qint64 endPositionMs) {
  maybePrepareNextItem(PlayoutTarget::Dsk, positionMs, endPositionMs);
}

bool ProgramPlayoutController::maybePrepareNextItem(
    PlayoutTarget target, qint64 positionMs, qint64 endPositionMs) {
  const bool dsk = target == PlayoutTarget::Dsk;
  auto &transition = dsk ? nextDskTransition_ : nextTransition_;
  const auto &stateMachine = dsk ? dskStateMachine_ : onAirStateMachine_;
  const bool repeat = !dsk && usesAlternatingRepeat();
  if (!playlistModel_ ||
      (!repeat && !transition.due && (positionMs < 0 || endPositionMs < positionMs)) ||
      (dsk && (player_.dskCrossfadeActive() ||
               player_.dskCueStatus() == DualDeckPlayer::DskStatus::Loading))) {
    // Keep both DSK decks intact through crossfade/preroll. The caller must
    // not TAKE until this refresh can validate the latest target and request.
    return false;
  }
  const auto *current = stateMachine.preparedMedia();
  const auto currentItem = current ? playlistModel_->itemState(current->itemId)
                                   : std::nullopt;
  auto mode = stateMachine.endModeForCurrentIteration();
  if (!mode && transition.due) {
    mode = transition.mode;
  }
  if (!currentItem || !mode ||
      (*mode != PlaylistEndMode::Next && *mode != PlaylistEndMode::Goto && !repeat)) {
    if (transition.itemId || transition.preparationAttempted || transition.due) {
      cancelNextTransition(!dsk, target);
    }
    return false;
  }
  // An explicit ON AIR request owns the spare deck until that request settles.
  if (!dsk && pendingTakeItemId_) {
    return false;
  }

  const quint64 excludedItemId = dsk ? playlistModel_->onAirItemId()
                                     : playlistModel_->dskItemId();
  auto candidate = repeat ? currentItem : playlistModel_->itemState(
      playlistModel_->endTargetItemIdForPlayout(
          current->itemId, *mode, excludedItemId));
  // DSK accepts File sources only. NEXT can skip unsupported sources;
  // an explicit GOTO never silently selects a different destination.
  if (dsk && *mode == PlaylistEndMode::Next) {
    const int limit = playlistModel_->itemCount(currentItem->playlistNumber);
    for (int attempt = 0; attempt < limit && candidate &&
         candidate->sourceType != PlaylistSourceType::File; ++attempt) {
      candidate = playlistModel_->itemState(
          playlistModel_->nextItemIdForPlayout(candidate->id, excludedItemId));
    }
  }
  if (candidate &&
      (candidate->filePath.isEmpty() ||
       playlistModel_->playlistMode(candidate->playlistNumber) == PlaylistMode::Bgm ||
       (dsk && candidate->sourceType != PlaylistSourceType::File))) {
    candidate.reset();
  }
  std::optional<CueRequest> request = candidate
      ? std::optional<CueRequest>{dsk ? dskRequestFor(*candidate)
                                      : cueRequestFor(*candidate)}
      : std::nullopt;
  if (repeat && request) {
    request->playbackRate = onAirTimedStillClock_.active() ? 1.0 : playbackRate_;
  }
  if (transition.preparationAttempted && transition.mode == mode &&
      transition.request == request) {
    return true;
  }
  if (transition.preparationAttempted) {
    // Replacing stale preparation must not lose a transition already waiting
    // at the outgoing item's end (including after Repeat -> NEXT/GOTO).
    const bool wasDue = transition.due;
    const bool wasPaused = transition.paused;
    cancelNextTransition(false, target);
    transition.due = wasDue;
    transition.paused = wasPaused;
  }
  transition.mode = mode;
  if (!repeat && !transition.due && endPositionMs - positionMs > kNextPreloadLeadMs) {
    return false;
  }
  if (!dsk && powerPointConversionInProgress_ && candidate &&
      candidate->sourceType == PlaylistSourceType::PowerPoint) {
    if (powerPointConversionCancelRequested_) {
      powerPointConversionCancelRequested_->store(true, std::memory_order_release);
    }
    powerPointNextTransitionDeferred_ = true;
    return false;
  }
  transition.request = request;
  transition.itemId = candidate ? std::optional<quint64>{candidate->id}
                                : std::nullopt;
  transition.preparationAttempted = true;
  if (!candidate) {
    return false;
  }
  if (dsk) {
    (void)cuePlaylistItemToDsk(*candidate, false);
  } else {
    failMacroReservation(QStringLiteral("Automatic NEXT/GOTO/REPEAT preparation has priority over the macro standby reservation."));
    scheduleCueRequest(request);
  }
  return true;
}

std::optional<PlaylistItemState>
ProgramPlayoutController::preparedNextItem(PlayoutTarget target) const {
  const bool dsk = target == PlayoutTarget::Dsk;
  const auto &transition = dsk ? nextDskTransition_ : nextTransition_;
  const auto &cue = dsk ? dskCue_ : cue_;
  if (!playlistModel_ || !transition.due || !transition.itemId ||
      !transition.request || !cue || *cue != *transition.request ||
      (!dsk && cueReplacementPending_)) {
    return std::nullopt;
  }
  const auto item = playlistModel_->itemState(*transition.itemId);
  if (!item) {
    return std::nullopt;
  }
  auto request = dsk ? dskRequestFor(*item) : cueRequestFor(*item);
  if (!dsk && transition.mode == PlaylistEndMode::Repeat) {
    request.playbackRate = onAirTimedStillClock_.active() ? 1.0 : playbackRate_;
  }
  if (request != *cue) {
    return std::nullopt;
  }
  return item;
}

bool ProgramPlayoutController::takePreparedNextItem() {
  if (nextTransition_.paused || pendingTakeItemId_) {
    return false;
  }
  if (nextTransition_.due &&
      !maybePrepareNextItem(PlayoutTarget::Program, 0, 0)) {
    return false;
  }
  if (!nextTransition_.due || !nextTransition_.itemId || !nextTransition_.mode ||
      !playlistModel_) {
    return false;
  }
  if (!cue_ || cue_->itemId != *nextTransition_.itemId) {
    if (playlistModel_->cueItemId() == *nextTransition_.itemId &&
        playlistModel_->cueState() == PlaylistCueState::Error) {
      failNextTransition(
          playlistModel_->cueDetail().isEmpty()
              ? QStringLiteral("The next playlist item could not be prepared.")
              : playlistModel_->cueDetail());
    }
    return false;
  }
  const DualDeckPlayer::CueStatus status = player_.cueStatus();
  if (status == DualDeckPlayer::CueStatus::Error) {
    const QString detail = player_.cueErrorDetail().isEmpty()
                               ? QStringLiteral(
                                     "The next playlist item could not be prepared.")
                               : player_.cueErrorDetail();
    failNextTransition(detail);
    return false;
  }
  if (status != DualDeckPlayer::CueStatus::Ready) {
    return false;
  }

  const auto nextItem = preparedNextItem(PlayoutTarget::Program);
  if (!nextItem) {
    return false;
  }
  if (nextTransition_.mode == PlaylistEndMode::Repeat) {
    return takePreparedRepeatItem(*nextItem);
  }
  bool crossfade = false;
  qint64 crossfadeDurationMs = kDefaultItemFadeDurationMs;
  if (const PlaylistPreparedMedia *onAirMedia =
          onAirStateMachine_.preparedMedia()) {
    const auto outgoingItem = playlistModel_->itemState(onAirMedia->itemId);
    if (outgoingItem &&
        (*nextTransition_.mode == PlaylistEndMode::Next ||
         *nextTransition_.mode == PlaylistEndMode::Goto)) {
      crossfade = outgoingItem->settings.endCrossfadeEnabled;
      crossfadeDurationMs =
          outgoingItem->settings.endCrossfadeDurationMs;
    }
  }
  qInfo() << "AUTO PROGRAM transition target=" << nextItem->id
          << "mode=" << static_cast<int>(*nextTransition_.mode)
          << "crossfade=" << crossfade << "durationMs=" << crossfadeDurationMs;
  if (takeCueOnAir(*nextItem, crossfade, crossfadeDurationMs)) {
    return true;
  }
  failNextTransition(
      QStringLiteral("The prepared next playlist item could not be taken ON AIR."));
  return false;
}

bool ProgramPlayoutController::usesAlternatingRepeat() const {
  const auto *media = onAirStateMachine_.preparedMedia();
  const auto item = media && playlistModel_
                        ? playlistModel_->itemState(media->itemId) : std::nullopt;
  return item && item->sourceType == PlaylistSourceType::File &&
         onAirStateMachine_.endModeForCurrentIteration() == PlaylistEndMode::Repeat;
}

bool ProgramPlayoutController::ownsAutomaticTransitionCue(
    const CueRequest &request) const {
  if (!nextTransition_.preparationAttempted ||
      nextTransition_.itemId != request.itemId ||
      nextTransition_.request != request || !nextTransition_.mode) {
    return false;
  }
  // Manual retake remains opt-in; an exact automatic reservation may preload
  // the ON AIR item on the other deck regardless of that setting.
  return *nextTransition_.mode == PlaylistEndMode::Next ||
         *nextTransition_.mode == PlaylistEndMode::Goto ||
         (*nextTransition_.mode == PlaylistEndMode::Repeat &&
          usesAlternatingRepeat());
}

qint64 ProgramPlayoutController::repeatCrossfadeDurationMs(qint64 endPositionMs) const {
  const auto *media = onAirStateMachine_.preparedMedia();
  const auto item = media && playlistModel_
                        ? playlistModel_->itemState(media->itemId) : std::nullopt;
  return item ? playlistRepeatCrossfadeDurationMs(*item, endPositionMs, playbackRate_) : 0;
}

bool ProgramPlayoutController::takePreparedRepeatItem(const PlaylistItemState &item) {
  if (!programPanel_ || !usesAlternatingRepeat() || player_.crossfadeActive()) {
    return false;
  }
  const qint64 endPosition = playbackEnd(programDurationMs());
  const qint64 crossfadeDuration = repeatCrossfadeDurationMs(endPosition);
  player_.setCueDelayFrames(programPanel_->itemDelayFrames());
  const auto eq = programPanel_->itemEqGains();
  player_.setCueEq(eq[0] / 10.0, eq[1] / 10.0, eq[2] / 10.0);
  if (!player_.takeCue(crossfadeDuration > 0, 1.0F)) {
    failNextTransition(QStringLiteral("The prepared repeat could not be taken ON AIR."));
    return false;
  }
  // Preserve the logical item, live fader/EQ/delay, rate and completed count.
  // prepare()/start() would reset the count and reapply Start Pause.
  if (onAirStateMachine_.finish() != PlaylistEndMode::Repeat) {
    failNextTransition(QStringLiteral("The repeat state could not be advanced."));
    return false;
  }
  revealItemAfterRepeat();
  player_.setItemVolume(programPanel_->itemGain());
  if (onAirTimedStillClock_.active()) {
    onAirTimedStillClock_.restart(true);
    onAirTimedStillElapsedTimer_.restart();
  } else if (!player_.play()) {
    failNextTransition(QStringLiteral("The prepared repeat could not start playback."));
    return false;
  }
  onAirTimedStillCompletionDue_ = false;
  pendingCue_.reset();
  cueReplacementPending_ = false;
  cue_.reset();
  lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
  playlistModel_->clearCueItem();
  resetNextTransitionState();
  pendingSeekMs_.reset();
  seekLockMs_ = item.settings.inPointMs;
  seekLockTicks_ = 20;
  programPanel_->setTimelinePosition(item.settings.inPointMs);
  programPanel_->setRemainingTime((std::max)(qint64{0}, endPosition - item.settings.inPointMs));
  programPanel_->setPlaybackState(true, true);
  refreshOnAirEndModePresentation();
  audioController_.setActive(!onAirTimedStillClock_.active() && player_.currentSourceHasAudio());
  startCrossfade(crossfadeDuration);
  clearFault();
  qInfo() << "AUTO PROGRAM repeat item=" << item.id
          << "deck=" << player_.currentDeckIndex()
          << "count=" << onAirStateMachine_.currentPlayCount()
          << "rate=" << playbackRate_ << "crossfadeMs=" << crossfadeDuration;
  return true;
}

void ProgramPlayoutController::cancelNextTransition(
    bool restoreSelectedCue, PlayoutTarget target) {
  if (target == PlayoutTarget::Dsk) {
    resetNextDskTransitionState(true);
    return;
  }
  const quint64 previousNextItemId = nextTransition_.itemId.value_or(0);
  const bool ownsCue =
      previousNextItemId != 0 &&
      ((pendingCue_ && pendingCue_->itemId == previousNextItemId) ||
       (cue_ && cue_->itemId == previousNextItemId));
  resetNextTransitionState();
  if (ownsCue) {
    scheduleCueRequest(std::nullopt);
  }
  if (restoreSelectedCue) {
    scheduleSelectedItemCue();
  }
}

void ProgramPlayoutController::resetNextTransitionState() {
  nextTransition_ = {};
}

void ProgramPlayoutController::failNextTransition(const QString &detail) {
  const PlaylistEndMode mode =
      nextTransition_.mode.value_or(PlaylistEndMode::Next);
  if (mode == PlaylistEndMode::Repeat) {
    (void)player_.pause();
    revealItemAfterRepeat();
  }
  onAirStateMachine_.fail();
  audioController_.setActive(false);
  setFault(detail);
  const QString modeName = mode == PlaylistEndMode::Repeat ? QStringLiteral("Repeat")
                           : mode == PlaylistEndMode::Goto
                               ? QStringLiteral("GOTO")
                               : QStringLiteral("Next");
  showWarning(QStringLiteral("Could not execute End Mode \"%1\".")
                  .arg(modeName));
  cancelNextTransition(false);
}

ProgramPlayoutController::CueRequest
ProgramPlayoutController::dskRequestFor(const PlaylistItemState &item) const {
  const bool timedStill = timedStillEndPosition(item).has_value();
  return CueRequest{item.id, item.filePath, {}, PlaylistSourceType::File, {},
                    timedStill ? qint64{0} : item.settings.inPointMs,
                    timedStill ? std::nullopt : item.settings.outPointMs};
}

bool ProgramPlayoutController::cueSelectedPlaylistItemToDsk() {
  const auto selected = playlistPanel_ ? playlistPanel_->selectedItemState()
                                       : std::nullopt;
  if (!selected) {
    showWarning(QStringLiteral("Select a valid playlist item to send to DSK."));
    return false;
  }
  if (!playlistPanel_->allowItemAction(selected->id,
                                       PlaylistItemAction::Dsk)) {
    return false;
  }
  return cuePlaylistItemToDsk(*selected, true);
}

bool ProgramPlayoutController::cuePlaylistItemToDsk(
    const PlaylistItemState &item, bool pendingOn) {
  if (!programPanel_ || !playlistModel_) {
    return false;
  }
  if (playlistModel_->playlistMode(item.playlistNumber) ==
      PlaylistMode::Bgm) {
    setPendingDskOn(false);
    showWarning(vtrpon::uiText("program.warning.bgm_mode_dsk"));
    updateDskSelectionAvailability();
    return false;
  }
  if (item.sourceType != PlaylistSourceType::File ||
      item.filePath.isEmpty() || !QFileInfo::exists(item.filePath)) {
    setPendingDskOn(false);
    showWarning(QStringLiteral("Select a valid playlist item to send to DSK."));
    updateDskSelectionAvailability();
    return false;
  }

  const CueRequest request = dskRequestFor(item);
  setPendingDskOn(pendingOn);
  if (player_.dskStatus() != DualDeckPlayer::DskStatus::On) {
    dskFileName_ = QFileInfo(item.filePath).fileName();
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Loading,
                                  dskFileName_);
  }
  if (!player_.cueDsk(item.filePath, item.settings.inPointMs,
                      item.settings.outPointMs.value_or(-1))) {
    dskCue_.reset();
    setPendingDskOn(false);
    const QString detail = takeDskFailureDetail(
        QStringLiteral("Could not prepare the DSK item."));
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Error,
                                  dskFileName_, detail);
    showWarning(detail);
    return false;
  }
  dskCue_ = request;
  lastDskStatus_ = player_.dskStatus();
  return true;
}

bool ProgramPlayoutController::activatePendingDsk() {
  if (!pendingDskOn_ || !dskCue_ || !programPanel_ || !playlistModel_) {
    return false;
  }
  const auto item = playlistModel_->itemState(dskCue_->itemId);
  if (item && dskRequestFor(*item) != *dskCue_) {
    return cuePlaylistItemToDsk(*item, true);
  }
  if (!item || !dskStateMachine_.prepare(item->id)) {
    finishDskOff();
    return false;
  }
  const auto startMode = dskStateMachine_.start();
  if (!startMode) {
    finishDskOff();
    return false;
  }
  dskTimedStillClock_.configure(
      item->settings.inPointMs, timedStillEndPosition(*item),
      *startMode == PlaylistStartMode::Play);
  dskTimedStillElapsedTimer_.restart();
  dskTimedStillCompletionDue_ = false;
  const qreal initialFade = item->settings.fadeInEnabled ? 0.0 : 1.0;
  if (!player_.takeDskCue(*startMode == PlaylistStartMode::Play, false,
                          static_cast<float>(initialFade))) {
    dskTimedStillClock_.reset();
    dskTimedStillCompletionDue_ = false;
    dskStateMachine_.fail();
    const QString detail = takeDskFailureDetail(
        QStringLiteral("Could not display DSK."));
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Error,
                                  dskFileName_, detail);
    showWarning(detail);
    return false;
  }
  dskCue_.reset();
  setPendingDskOn(false);
  dskManuallyPaused_ = false;
  dskEndOfStreamPending_ = false;
  playlistModel_->setDskItemId(item->id);
  dskFileName_ = QFileInfo(item->filePath).fileName();
  resetDskFadeState();
  dskFadeInProgress_ = initialFade;
  applyDskFadeProgress();
  startDskFadeIn(item->settings);
  programPanel_->setDskCueState(ProgramPanel::DskCueState::Ready,
                                dskFileName_);
  programPanel_->setDskOn(true);
  lastDskStatus_ = DualDeckPlayer::DskStatus::On;
  updateDskTransportControls();
  return true;
}

bool ProgramPlayoutController::takePreparedNextDskItem() {
  if (dskManuallyPaused_ || pendingDskOff_) {
    return false;
  }
  if (nextDskTransition_.due &&
      !maybePrepareNextItem(PlayoutTarget::Dsk, 0, 0)) {
    return false;
  }
  if (!nextDskTransition_.due || !nextDskTransition_.itemId ||
      !nextDskTransition_.mode || !dskCue_ || !playlistModel_) {
    return false;
  }
  if (player_.dskCueStatus() != DualDeckPlayer::DskStatus::Ready) {
    return false;
  }
  const auto nextItem = preparedNextItem(PlayoutTarget::Dsk);
  const PlaylistPreparedMedia *outgoing = dskStateMachine_.preparedMedia();
  const auto outgoingItem = outgoing
      ? playlistModel_->itemState(outgoing->itemId) : std::nullopt;
  if (!nextItem || !outgoingItem) {
    return false;
  }
  const bool crossfade = outgoingItem->settings.endCrossfadeEnabled;
  const qint64 crossfadeDuration =
      outgoingItem->settings.endCrossfadeDurationMs;
  const qreal initialFade = nextItem->settings.fadeInEnabled ? 0.0 : 1.0;
  qInfo() << "AUTO DSK transition target=" << nextItem->id
          << "crossfade=" << crossfade;
  if (!player_.takeDskCue(
          nextItem->settings.startMode == PlaylistStartMode::Play,
          crossfade, static_cast<float>(initialFade))) {
    return false;
  }
  if (!dskStateMachine_.prepare(nextItem->id)) {
    dskStateMachine_.fail();
    return false;
  }
  const auto startMode = dskStateMachine_.start();
  if (!startMode) {
    dskStateMachine_.fail();
    return false;
  }
  dskTimedStillClock_.configure(
      nextItem->settings.inPointMs, timedStillEndPosition(*nextItem),
      *startMode == PlaylistStartMode::Play);
  dskTimedStillElapsedTimer_.restart();
  dskTimedStillCompletionDue_ = false;
  playlistModel_->setDskItemId(nextItem->id);
  dskEndOfStreamPending_ = false;
  dskFileName_ = QFileInfo(nextItem->filePath).fileName();
  dskCue_.reset();
  resetNextDskTransitionState(false);
  resetDskFadeState();
  dskFadeInProgress_ = initialFade;
  applyDskFadeProgress();
  startDskFadeIn(nextItem->settings);
  startDskCrossfade(crossfadeDuration);
  if (programPanel_) {
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Ready,
                                  dskFileName_);
    programPanel_->setDskOn(true);
  }
  updateDskTransportControls();
  return true;
}

void ProgramPlayoutController::resetNextDskTransitionState(bool clearCue) {
  if (clearCue && dskCue_) {
    (void)player_.clearDskCue();
  }
  dskCue_.reset();
  nextDskTransition_ = {};
}

void ProgramPlayoutController::handleDskCompletion() {
  if (!programPanel_ || !playlistModel_ || dskManuallyPaused_ || pendingDskOff_) {
    return;
  }
  const auto endMode = dskStateMachine_.finish();
  qInfo() << "AUTO DSK completion item=" << playlistModel_->dskItemId()
          << "endMode=" << (endMode ? static_cast<int>(*endMode) : -1);
  const PlaylistPreparedMedia *current = dskStateMachine_.preparedMedia();
  const auto item = current ? playlistModel_->itemState(current->itemId)
                            : std::nullopt;
  if (!endMode || !item) {
    return;
  }
  dskFadeInAnimation_.stop();
  dskFadeOutAnimation_.stop();
  dskFadeInProgress_ = 1.0;
  dskFadeOutProgress_ = item->settings.fadeOutEnabled ? 0.0 : 1.0;
  dskFadeOutStarted_ = item->settings.fadeOutEnabled;
  applyDskFadeProgress();

  bool succeeded = false;
  QString failure;
  switch (*endMode) {
  case PlaylistEndMode::Stop:
    succeeded = player_.restartDskFromIn(false);
    if (succeeded && dskTimedStillClock_.active()) {
      dskTimedStillClock_.stopAtStart();
    }
    failure = QStringLiteral("Could not execute DSK End Mode \"Stop\".");
    break;
  case PlaylistEndMode::Pause:
    succeeded = player_.pauseDskAtEnd();
    if (succeeded && dskTimedStillClock_.active()) {
      dskTimedStillClock_.holdAtEnd();
    }
    failure = QStringLiteral("Could not execute DSK End Mode \"Pause\".");
    break;
  case PlaylistEndMode::Repeat:
    succeeded = player_.restartDskFromIn(true);
    failure = QStringLiteral("Could not execute DSK End Mode \"Repeat\".");
    if (succeeded) {
      if (dskTimedStillClock_.active()) {
        dskTimedStillClock_.restart(true);
        dskTimedStillElapsedTimer_.restart();
      }
      resetDskFadeState();
      startDskFadeIn(item->settings);
    }
    break;
  case PlaylistEndMode::Next:
  case PlaylistEndMode::Goto:
    nextDskTransition_.mode = *endMode;
    nextDskTransition_.due = true;
    {
      const qint64 endPosition = dskTimedStillClock_.active()
                                     ? dskTimedStillClock_.durationMs()
                                     : player_.dskPositionMs();
      maybePrepareNextDskItem(endPosition, endPosition);
    }
    if (takePreparedNextDskItem()) {
      return;
    }
    succeeded = player_.pauseDskAtEnd();
    failure = *endMode == PlaylistEndMode::Goto
        ? QStringLiteral("Could not execute DSK End Mode \"GOTO\".")
        : QStringLiteral("Could not execute DSK End Mode \"Next\".");
    if (succeeded && !nextDskTransition_.itemId) {
      showWarning(*endMode == PlaylistEndMode::Goto
                      ? QStringLiteral(
                            "No eligible DSK GOTO destination is selected or available.")
                      : QStringLiteral(
                            "No eligible next DSK playlist item is available."));
    }
    break;
  }
  if (succeeded) {
    programPanel_->setDskOn(true);
    updateDskTransportControls();
    return;
  }
  failDsk(takeDskFailureDetail(failure));
}

void ProgramPlayoutController::startDskCrossfade(qint64 durationMs) {
  if (!player_.dskCrossfadeActive()) {
    return;
  }
  dskCrossfadeAnimation_.stop();
  dskCrossfadeAnimation_.setDuration(static_cast<int>(std::clamp<qint64>(
      durationMs, kMinimumItemFadeDurationMs, kMaximumItemFadeDurationMs)));
  dskCrossfadeAnimation_.setStartValue(0.0);
  dskCrossfadeAnimation_.setEndValue(1.0);
  dskCrossfadeAnimation_.start();
}

void ProgramPlayoutController::startDskFadeIn(
    const PlaylistItemSettings &settings) {
  dskFadeInAnimation_.stop();
  dskFadeOutAnimation_.stop();
  dskFadeOutProgress_ = 1.0;
  dskFadeOutStarted_ = false;
  if (!settings.fadeInEnabled) {
    dskFadeInProgress_ = 1.0;
    applyDskFadeProgress();
    return;
  }
  dskFadeInProgress_ = 0.0;
  applyDskFadeProgress();
  dskFadeInAnimation_.setDuration(static_cast<int>(std::clamp<qint64>(
      settings.fadeInDurationMs, kMinimumItemFadeDurationMs,
      kMaximumItemFadeDurationMs)));
  dskFadeInAnimation_.setStartValue(0.0);
  dskFadeInAnimation_.setEndValue(1.0);
  dskFadeInAnimation_.start();
}

void ProgramPlayoutController::maybeStartDskFadeOut(
    qint64 positionMs, qint64 endPositionMs) {
  if (pendingDskOff_ || dskManuallyPaused_ ||
      (!dskTimedStillClock_.active() && !player_.dskIsPlaying()) ||
      dskFadeOutStarted_ || positionMs < 0 || endPositionMs < positionMs ||
      (dskTimedStillClock_.active() &&
       !dskTimedStillClock_.playing() &&
       !dskTimedStillCompletionDue_) ||
      !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *current = dskStateMachine_.preparedMedia();
  const auto item = current ? playlistModel_->itemState(current->itemId)
                            : std::nullopt;
  if (!item || !item->settings.fadeOutEnabled) {
    return;
  }
  const qint64 duration = std::clamp<qint64>(
      item->settings.fadeOutDurationMs, kMinimumItemFadeDurationMs,
      kMaximumItemFadeDurationMs);
  const qint64 remaining = endPositionMs - positionMs;
  if (remaining > duration) {
    return;
  }
  dskFadeOutStarted_ = true;
  dskFadeOutAnimation_.setDuration(
      static_cast<int>((std::max)(qint64{1}, remaining)));
  dskFadeOutAnimation_.setStartValue(dskFadeOutProgress_);
  dskFadeOutAnimation_.setEndValue(0.0);
  dskFadeOutAnimation_.start();
}

void ProgramPlayoutController::applyDskFadeProgress() {
  if (player_.dskStatus() == DualDeckPlayer::DskStatus::On) {
    (void)player_.setDskVideoOpacity(static_cast<float>(std::clamp(
        dskFadeInProgress_ * dskFadeOutProgress_, 0.0, 1.0)));
  }
}

void ProgramPlayoutController::resetDskFadeState() {
  dskFadeInAnimation_.stop();
  dskFadeOutAnimation_.stop();
  dskFadeInProgress_ = 1.0;
  dskFadeOutProgress_ = 1.0;
  dskFadeOutStarted_ = false;
}

void ProgramPlayoutController::beginDskOff() {
  resetNextDskTransitionState(true);
  setPendingDskOn(false);
  pendingDskOff_ = true;
  updateDskTransportControls();
  dskCrossfadeAnimation_.stop();
  if (player_.dskCrossfadeActive()) {
    (void)player_.setDskCrossfadeProgress(1.0F);
  }
  if (programPanel_) {
    programPanel_->setDskOn(true);
  }
  const PlaylistPreparedMedia *current = dskStateMachine_.preparedMedia();
  const auto item = current && playlistModel_
      ? playlistModel_->itemState(current->itemId) : std::nullopt;
  const qreal currentOpacity = std::clamp(
      dskFadeInProgress_ * dskFadeOutProgress_, 0.0, 1.0);
  if (!item || !item->settings.fadeOutEnabled ||
      currentOpacity <= 0.0) {
    finishDskOff();
    return;
  }
  dskFadeInAnimation_.stop();
  dskFadeOutAnimation_.stop();
  dskFadeInProgress_ = 1.0;
  dskFadeOutProgress_ = currentOpacity;
  dskFadeOutStarted_ = true;
  dskFadeOutAnimation_.setDuration(static_cast<int>(std::clamp<qint64>(
      item->settings.fadeOutDurationMs, kMinimumItemFadeDurationMs,
      kMaximumItemFadeDurationMs)));
  dskFadeOutAnimation_.setStartValue(currentOpacity);
  dskFadeOutAnimation_.setEndValue(0.0);
  dskFadeOutAnimation_.start();
}

void ProgramPlayoutController::finishDskOff() {
  pendingDskOff_ = false;
  dskManuallyPaused_ = false;
  dskEndOfStreamPending_ = false;
  setPendingDskOn(false);
  dskCrossfadeAnimation_.stop();
  resetDskFadeState();
  if (!player_.clearDsk()) {
    const QString detail = takeDskFailureDetail(
        QStringLiteral("Could not turn DSK off."));
    if (programPanel_) {
      programPanel_->setDskCueState(ProgramPanel::DskCueState::Error,
                                    dskFileName_, detail);
    }
    showWarning(detail);
    return;
  }
  dskTimedStillClock_.reset();
  dskTimedStillCompletionDue_ = false;
  dskStateMachine_.reset();
  dskCue_.reset();
  nextDskTransition_ = {};
  if (playlistModel_) {
    playlistModel_->clearDskItem();
  }
  dskFileName_.clear();
  lastDskStatus_ = DualDeckPlayer::DskStatus::Empty;
  if (programPanel_) {
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Empty);
  }
}

void ProgramPlayoutController::toggleDsk() {
  if (!programPanel_) {
    return;
  }
  if (pendingDskOff_) {
    programPanel_->setDskOn(true);
    return;
  }
  if (player_.dskStatus() == DualDeckPlayer::DskStatus::On) {
    beginDskOff();
    return;
  }
  const DualDeckPlayer::DskStatus cueStatus = player_.dskCueStatus();
  if (pendingDskOn_ &&
      (cueStatus == DualDeckPlayer::DskStatus::Loading ||
       cueStatus == DualDeckPlayer::DskStatus::Ready)) {
    finishDskOff();
    return;
  }
  const auto selected = playlistPanel_ ? playlistPanel_->selectedItemState()
                                       : std::nullopt;
  if (selected && dskCue_ && dskRequestFor(*selected) == *dskCue_ &&
      cueStatus == DualDeckPlayer::DskStatus::Ready) {
    setPendingDskOn(true);
    (void)activatePendingDsk();
    return;
  }
  (void)cueSelectedPlaylistItemToDsk();
}

void ProgramPlayoutController::setDskAnimationsPaused(bool paused) {
  for (auto *animation : {&dskFadeInAnimation_, &dskFadeOutAnimation_,
                          &dskCrossfadeAnimation_}) {
    if (paused && animation->state() == QAbstractAnimation::Running) {
      animation->pause();
    } else if (!paused && animation->state() == QAbstractAnimation::Paused) {
      animation->resume();
    }
  }
}

void ProgramPlayoutController::updateDskTransportControls() {
  if (programPanel_) {
    const bool playing = dskTimedStillClock_.active()
        ? dskTimedStillClock_.playing() : player_.dskIsPlaying();
    programPanel_->setDskPlaybackState(
        playing && !dskManuallyPaused_, !pendingDskOff_);
  }
}

void ProgramPlayoutController::failDsk(const QString &detail) {
  // Both synchronous transport failures and bus errors use the same cleanup.
  // Consuming an error for its text must not bypass playlist/state cleanup.
  pendingDskOff_ = false;
  dskManuallyPaused_ = false;
  dskEndOfStreamPending_ = false;
  setPendingDskOn(false);
  dskCrossfadeAnimation_.stop();
  resetDskFadeState();
  resetNextDskTransitionState(true);
  (void)player_.clearDsk();
  dskTimedStillClock_.reset();
  dskTimedStillCompletionDue_ = false;
  dskStateMachine_.fail();
  if (playlistModel_) {
    playlistModel_->clearDskItem();
  }
  lastDskStatus_ = DualDeckPlayer::DskStatus::Error;
  if (programPanel_) {
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Error,
                                 dskFileName_, detail);
  }
  showWarning(detail);
}

void ProgramPlayoutController::playDsk() {
  if (!programPanel_ || !playlistModel_ || pendingDskOff_) {
    return;
  }
  // Observe EOS/errors before deciding between resume and restart. A paused
  // operator transport keeps completion pending in updateDskUi().
  const quint64 previousItemId = playlistModel_->dskItemId();
  player_.processBusMessages();
  updateDskUi();
  if (player_.dskStatus() != DualDeckPlayer::DskStatus::On ||
      playlistModel_->dskItemId() != previousItemId) {
    // An automatic TAKE already performed the action; retain its Start Mode.
    return;
  }
  if (nextDskTransition_.due) {
    dskManuallyPaused_ = false;
    setDskAnimationsPaused(false);
    updateDskUi();
    return;
  }
  const auto *current = dskStateMachine_.preparedMedia();
  const auto item = current ? playlistModel_->itemState(current->itemId)
                            : std::nullopt;
  if (!item) {
    return;
  }
  const bool restarting =
      dskStateMachine_.phase() == PlaylistItemPlayoutPhase::Ended ||
      dskEndOfStreamPending_ || dskTimedStillCompletionDue_ ||
      (dskTimedStillClock_.active() &&
       dskTimedStillClock_.positionMs() >= dskTimedStillClock_.durationMs());
  if (!restarting && !dskManuallyPaused_ &&
      (dskTimedStillClock_.active() ? dskTimedStillClock_.playing()
                                    : player_.dskIsPlaying())) {
    return; // Repeated Play must not reset the timed-still elapsed anchor.
  }
  if (!(restarting ? player_.restartDskFromIn(true) : player_.playDsk())) {
    failDsk(takeDskFailureDetail(QStringLiteral("Could not play the DSK media.")));
    return;
  }
  dskManuallyPaused_ = false;
  if (dskTimedStillClock_.active()) {
    if (restarting) {
      dskTimedStillClock_.restart(true);
    } else {
      dskTimedStillClock_.play();
    }
    dskTimedStillElapsedTimer_.restart();
  }
  if (restarting) {
    dskEndOfStreamPending_ = false;
    dskTimedStillCompletionDue_ = false;
    resetNextDskTransitionState(true);
    resetDskFadeState();
    startDskFadeIn(item->settings);
  }
  (void)dskStateMachine_.markActive();
  setDskAnimationsPaused(false);
  updateDskTransportControls();
}

void ProgramPlayoutController::pauseDsk() {
  if (!programPanel_ || pendingDskOff_ ||
      player_.dskStatus() != DualDeckPlayer::DskStatus::On ||
      !dskStateMachine_.preparedMedia()) {
    return;
  }
  if (!player_.pauseDsk()) {
    failDsk(takeDskFailureDetail(QStringLiteral("Could not pause the DSK media.")));
    return;
  }
  dskManuallyPaused_ = true;
  if (dskTimedStillClock_.active()) {
    dskTimedStillCompletionDue_ =
        dskTimedStillClock_.advance((std::max)(
            qint64{0}, dskTimedStillElapsedTimer_.restart())) ||
        dskTimedStillCompletionDue_;
    dskTimedStillClock_.pause();
  }
  setDskAnimationsPaused(true);
  updateDskTransportControls();
}

void ProgramPlayoutController::updateDskUi() {
  if (!programPanel_) {
    return;
  }
  QString eventDetail;
  const DualDeckPlayer::DskEvent event = player_.takeDskEvent(eventDetail);
  if (event == DualDeckPlayer::DskEvent::CueError) {
    if (player_.dskStatus() == DualDeckPlayer::DskStatus::On) {
      resetNextDskTransitionState(true);
      showWarning(eventDetail.isEmpty() ? player_.dskErrorDetail()
                                        : eventDetail);
    } else {
      setPendingDskOn(false);
      programPanel_->setDskOn(false);
      programPanel_->setDskCueState(ProgramPanel::DskCueState::Error,
                                    dskFileName_, eventDetail);
      showWarning(eventDetail);
    }
  } else if (event == DualDeckPlayer::DskEvent::Error) {
    failDsk(eventDetail.isEmpty() ? player_.dskErrorDetail() : eventDetail);
    return;
  } else if (event == DualDeckPlayer::DskEvent::EndOfStream &&
             !dskTimedStillClock_.active()) {
    dskEndOfStreamPending_ = true;
  }
  if (player_.dskStatus() == DualDeckPlayer::DskStatus::Error &&
      dskStateMachine_.preparedMedia() &&
      dskStateMachine_.phase() != PlaylistItemPlayoutPhase::Error) {
    failDsk(takeDskFailureDetail(QStringLiteral("DSK playback failed.")));
    return;
  }

  if (player_.dskStatus() == DualDeckPlayer::DskStatus::On) {
    if (pendingDskOff_ || dskManuallyPaused_) {
      updateDskTransportControls();
      return;
    }
    const PlaylistPreparedMedia *current = dskStateMachine_.preparedMedia();
    const qint64 position = dskTimedStillClock_.active()
                                 ? dskTimedStillClock_.positionMs()
                                 : player_.dskPositionMs();
    const qint64 duration = dskTimedStillClock_.active()
                                 ? dskTimedStillClock_.durationMs()
                                 : player_.dskDurationMs();
    const qint64 endPosition = current && current->outPointMs
        ? *current->outPointMs : duration;
    maybeStartDskFadeOut(position, endPosition);
    maybePrepareNextDskItem(position, endPosition);
    if (dskTimedStillCompletionDue_ || dskEndOfStreamPending_) {
      dskTimedStillCompletionDue_ = false;
      dskEndOfStreamPending_ = false;
      handleDskCompletion();
      return;
    }
    if (player_.dskCueStatus() == DualDeckPlayer::DskStatus::Ready &&
        nextDskTransition_.due) {
      (void)takePreparedNextDskItem();
    }
    programPanel_->setDskOn(true);
    updateDskTransportControls();
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Ready,
                                  dskFileName_);
    lastDskStatus_ = DualDeckPlayer::DskStatus::On;
    return;
  }

  const DualDeckPlayer::DskStatus cueStatus = player_.dskCueStatus();
  if (cueStatus == DualDeckPlayer::DskStatus::Ready && pendingDskOn_) {
    (void)activatePendingDsk();
    return;
  }
  if (cueStatus == DualDeckPlayer::DskStatus::Empty &&
      lastDskStatus_ == DualDeckPlayer::DskStatus::Error) {
    return; // Keep the failure visible until a new CUE or explicit OFF.
  }
  lastDskStatus_ = cueStatus;
  switch (cueStatus) {
  case DualDeckPlayer::DskStatus::Empty:
    programPanel_->setDskOn(false);
    if (!pendingDskOn_) {
      programPanel_->setDskCueState(ProgramPanel::DskCueState::Empty);
    }
    break;
  case DualDeckPlayer::DskStatus::Loading:
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Loading,
                                  dskFileName_);
    break;
  case DualDeckPlayer::DskStatus::Ready:
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Ready,
                                  dskFileName_);
    break;
  case DualDeckPlayer::DskStatus::On:
    break;
  case DualDeckPlayer::DskStatus::Error:
    setPendingDskOn(false);
    programPanel_->setDskOn(false);
    programPanel_->setDskCueState(ProgramPanel::DskCueState::Error,
                                  dskFileName_, player_.dskErrorDetail());
    break;
  }
}

void ProgramPlayoutController::setPendingDskOn(bool pending) {
  pendingDskOn_ = pending;
  if (programPanel_) {
    programPanel_->setDskPendingOn(pending);
  }
}

QString ProgramPlayoutController::takeDskFailureDetail(
    const QString &fallback) {
  QString detail = player_.dskErrorDetail();
  QString eventDetail;
  const DualDeckPlayer::DskEvent event = player_.takeDskEvent(eventDetail);
  if ((event == DualDeckPlayer::DskEvent::Error ||
       event == DualDeckPlayer::DskEvent::CueError) &&
      !eventDetail.isEmpty()) {
    detail = eventDetail;
  }
  return detail.isEmpty() ? fallback : detail;
}

void ProgramPlayoutController::updateDskSelectionAvailability() {
  if (!programPanel_ || !playlistPanel_) {
    return;
  }
  const auto selected = playlistPanel_->selectedItemState();
  const bool conversionInProgress =
      selected && playlistModel_->itemConversionInProgress(selected->id);
  programPanel_->setDskSelectionAvailable(
      selected &&
      playlistModel_->playlistMode(selected->playlistNumber) !=
          PlaylistMode::Bgm &&
      (conversionInProgress ||
       (selected->sourceType == PlaylistSourceType::File &&
        !selected->filePath.isEmpty() &&
        QFileInfo::exists(selected->filePath))));
}

void ProgramPlayoutController::nextPowerPointSlide() {
  if (pendingPowerPointNavigation_) {
    return;
  }
  pendingPowerPointNavigation_ = PowerPointNavigationCommand::Next;
  powerPointNavigationRetryCount_ = 0;
  attemptPowerPointNavigation(*pendingPowerPointNavigation_);
}

void ProgramPlayoutController::previousPowerPointSlide() {
  if (pendingPowerPointNavigation_) {
    return;
  }
  pendingPowerPointNavigation_ = PowerPointNavigationCommand::Previous;
  powerPointNavigationRetryCount_ = 0;
  attemptPowerPointNavigation(*pendingPowerPointNavigation_);
}

void ProgramPlayoutController::attemptPowerPointNavigation(
    PowerPointNavigationCommand command) {
  const bool pdfNavigation = player_.pdfNavigationState().active;
  bool busy = false;
  bool failed = false;
  QString failureDetail;
  if (pdfNavigation) {
    const PdfCommandResponse response =
        command == PowerPointNavigationCommand::Next
            ? player_.nextPdfPage()
            : player_.previousPdfPage();
    busy = response.result == PdfCommandResult::Busy;
    failed = response.result == PdfCommandResult::Failed;
    failureDetail = response.errorDetail;
  } else {
    const PowerPointCommandResponse response =
        command == PowerPointNavigationCommand::Next
            ? player_.nextPowerPointSlide()
            : player_.previousPowerPointSlide();
    busy = response.result == PowerPointCommandResult::Busy;
    failed = response.result == PowerPointCommandResult::Failed;
    failureDetail = response.errorDetail;
  }
  if (busy &&
      powerPointNavigationRetryCount_ <
          kMaximumPowerPointNavigationRetries) {
    ++powerPointNavigationRetryCount_;
    pendingPowerPointNavigation_ = command;
    powerPointNavigationRetryTimer_.start();
    updatePowerPointNavigationUi();
    return;
  }

  failed = failed ||
           (busy && powerPointNavigationRetryCount_ >=
                        kMaximumPowerPointNavigationRetries);
  clearPendingPowerPointNavigation();
  updatePowerPointNavigationUi();
  if (failed) {
    showWarning(
        failureDetail.isEmpty()
            ? QStringLiteral("Document navigation failed.")
            : failureDetail);
  }
}

void ProgramPlayoutController::clearPendingPowerPointNavigation() {
  if (pendingPowerPointNavigation_) player_.cancelDocumentNavigation();
  powerPointNavigationRetryTimer_.stop();
  pendingPowerPointNavigation_.reset();
  powerPointNavigationRetryCount_ = 0;
}

void ProgramPlayoutController::updatePowerPointNavigationUi() {
  if (!programPanel_) {
    return;
  }
  const PowerPointNavigationState state =
      player_.powerPointNavigationState();
  if (state.active) {
    programPanel_->setPowerPointNavigationState(
        true, state.canGoPrevious, state.canGoNext,
        state.currentSlide, state.slideCount,
        state.busy || pendingPowerPointNavigation_.has_value());
    return;
  }
  const PdfNavigationState pdfState = player_.pdfNavigationState();
  programPanel_->setPdfNavigationState(
      pdfState.active, pdfState.canGoPrevious, pdfState.canGoNext,
      pdfState.currentPage, pdfState.pageCount,
      pdfState.busy || pendingPowerPointNavigation_.has_value());
}

void ProgramPlayoutController::playOnAir() {
  if (nextTransition_.mode == PlaylistEndMode::Repeat) {
    nextTransition_.paused = false;
    if (nextTransition_.due) {
      (void)takePreparedNextItem();
      return;
    }
  }
  if (player_.powerPointNavigationState().active ||
      player_.pdfNavigationState().active) {
    return;
  }
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (onAirMedia && onAirTimedStillClock_.active()) {
    const DualDeckPlayer::ProgramFrameRestoreStatus restoreStatus =
        player_.restoreCurrentProgramFrame();
    if (restoreStatus ==
        DualDeckPlayer::ProgramFrameRestoreStatus::Error) {
      onAirTimedStillRestorePending_ = false;
      const QString detail =
          QStringLiteral("Could not restore the still image for playback.");
      setFault(detail);
      showWarning(detail);
      return;
    }
    if (restoreStatus ==
        DualDeckPlayer::ProgramFrameRestoreStatus::Loading) {
      onAirTimedStillRestorePending_ = true;
      if (programPanel_) {
        programPanel_->setPlaybackState(true, false);
      }
      return;
    }
    onAirTimedStillRestorePending_ = false;
    const bool restarting =
        onAirStateMachine_.phase() == PlaylistItemPlayoutPhase::Ended ||
        onAirTimedStillClock_.positionMs() >=
            onAirTimedStillClock_.durationMs();
    if (restarting) {
      onAirTimedStillClock_.restart(true);
      onAirTimedStillElapsedTimer_.restart();
      onAirTimedStillCompletionDue_ = false;
      if (playlistModel_) {
        const auto state = playlistModel_->itemState(onAirMedia->itemId);
        if (state) {
          startItemFadeIn(state->settings);
        }
      }
    } else {
      onAirTimedStillClock_.play();
      onAirTimedStillElapsedTimer_.restart();
    }
    (void)onAirStateMachine_.markActive();
    audioController_.setActive(false);
    if (programPanel_) {
      programPanel_->setPlaybackState(true, true);
    }
    clearFault();
    return;
  }
  const bool live = onAirIsLive();
  bool restartingAtInPoint = false;
  if (onAirMedia && !live) {
    const qint64 position = player_.positionMs();
    const bool atOutPoint =
        onAirMedia->outPointMs && position >= *onAirMedia->outPointMs;
    if (onAirStateMachine_.phase() == PlaylistItemPlayoutPhase::Ended ||
        atOutPoint) {
      restartingAtInPoint = true;
      if (!player_.seek(onAirMedia->inPointMs)) {
        const QString detail =
            QStringLiteral("Failed to seek to the In point before playback.");
        setFault(detail);
        showWarning(detail);
        synchronizeProgramPosition();
        return;
      }
      seekLockMs_ = onAirMedia->inPointMs;
      seekLockTicks_ = 20;
      if (programPanel_) {
        programPanel_->setTimelinePosition(*seekLockMs_);
        if (onAirMedia->outPointMs) {
          programPanel_->setRemainingTime(*onAirMedia->outPointMs -
                                          onAirMedia->inPointMs);
        }
      }
      if (playlistModel_) {
        const auto state = playlistModel_->itemState(onAirMedia->itemId);
        if (state) {
          startItemFadeIn(state->settings);
        } else {
          revealItemAfterRepeat();
        }
      }
    }
  }
  if (!player_.play()) {
    if (restartingAtInPoint) {
      revealItemAfterRepeat();
    }
    const QString detail = QStringLiteral("Playback failed.");
    setFault(detail);
    showWarning(detail);
    return;
  }
  if (onAirMedia) {
    (void)onAirStateMachine_.markActive();
  }
  audioController_.setActive(onAirMedia != nullptr &&
                             player_.currentSourceHasAudio());
  if (programPanel_) {
    programPanel_->setPlaybackState(onAirMedia != nullptr, true);
  }
  clearFault();
}

void ProgramPlayoutController::pauseOnAir() {
  if (nextTransition_.mode == PlaylistEndMode::Repeat) {
    nextTransition_.paused = true;
  }
  if (player_.powerPointNavigationState().active ||
      player_.pdfNavigationState().active) {
    return;
  }
  if (onAirTimedStillClock_.active()) {
    onAirTimedStillRestorePending_ = false;
    onAirTimedStillCompletionDue_ =
        onAirTimedStillClock_.advance(
            (std::max)(qint64{0}, onAirTimedStillElapsedTimer_.restart())) ||
        onAirTimedStillCompletionDue_;
    onAirTimedStillClock_.pause();
    audioController_.setActive(false);
    if (programPanel_) {
      programPanel_->setPlaybackState(
          onAirStateMachine_.preparedMedia() != nullptr, false);
    }
    clearFault();
    return;
  }
  if (!player_.pause()) {
    const QString detail = QStringLiteral("Pause failed.");
    setFault(detail);
    showWarning(detail);
    return;
  }
  audioController_.setActive(false);
  if (programPanel_) {
    programPanel_->setPlaybackState(
        onAirStateMachine_.preparedMedia() != nullptr, false);
  }
  clearFault();
}

void ProgramPlayoutController::setPlaybackRate(double rate) {
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  const auto item = onAirMedia && playlistModel_
                        ? playlistModel_->itemState(onAirMedia->itemId)
                        : std::nullopt;
  const bool rateSupported =
      item && item->sourceType == PlaylistSourceType::File &&
      (item->mediaType == PlaylistMediaType::Video ||
       item->mediaType == PlaylistMediaType::Audio ||
       item->mediaType == PlaylistMediaType::Unknown) &&
      rate > 0.0;
  if (!rateSupported) {
    if (programPanel_) {
      programPanel_->setPlaybackRate(playbackRate_);
    }
    return;
  }

  if (!player_.setRate(rate)) {
    if (programPanel_) {
      programPanel_->setPlaybackRate(playbackRate_);
    }
    showWarning(QStringLiteral("Playback rate change failed."));
    return;
  }

  playbackRate_ = rate;
  if (programPanel_) {
    programPanel_->setPlaybackRate(playbackRate_);
  }
  clearFault();
}

void ProgramPlayoutController::togglePlayback() {
  if (programIsPlaying()) {
    pauseOnAir();
  } else {
    playOnAir();
  }
}

void ProgramPlayoutController::handleOnAirCompletion(qint64 endPosition) {
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (!onAirMedia || !programPanel_) {
    return;
  }
  const qint64 inPoint = onAirMedia->inPointMs;
  const qint64 rangeDuration =
      endPosition >= inPoint ? endPosition - inPoint : 0;

  const auto itemState = playlistModel_
                             ? playlistModel_->itemState(onAirMedia->itemId)
                             : std::nullopt;
  if (usesAlternatingRepeat()) {
    // Leave the old deck/frame intact until the other deck is genuinely Ready.
    // In particular, never seek the visible deck back to IN to retry a loop.
    nextTransition_.mode = PlaylistEndMode::Repeat;
    nextTransition_.due = true;
    maybePrepareNextItem(endPosition, endPosition);
    if (takePreparedNextItem()) {
      return;
    }
    if (onAirTimedStillClock_.active()) {
      onAirTimedStillClock_.holdAtEnd();
    } else {
      (void)player_.pause();
    }
    programPanel_->setTimelinePosition(endPosition);
    programPanel_->setRemainingTime(0);
    programPanel_->setPlaybackState(true, false);
    audioController_.setActive(false);
    return;
  }
  const bool fadeOutEnabled =
      itemState && itemState->settings.fadeOutEnabled;
  finishItemFadeForEnd(fadeOutEnabled);

  const auto endMode = onAirStateMachine_.finish();
  qInfo() << "AUTO PROGRAM completion item=" << onAirMedia->itemId
          << "endMode=" << (endMode ? static_cast<int>(*endMode) : -1)
          << "positionMs=" << endPosition;
  if (!endMode) {
    (void)player_.pause();
    audioController_.setActive(false);
    const QString detail =
        QStringLiteral("Could not retrieve the latest playlist item information during end processing.");
    setFault(detail);
    showWarning(detail);
    return;
  }

  if (*endMode == PlaylistEndMode::Repeat) {
    const bool repeated = onAirTimedStillClock_.active()
                              ? (onAirTimedStillClock_.restart(true), true)
                              : player_.seek(inPoint) && player_.play();
    if (repeated) {
      if (onAirTimedStillClock_.active()) {
        onAirTimedStillElapsedTimer_.restart();
      }
      onAirTimedStillCompletionDue_ = false;
      if (itemState) {
        startItemFadeIn(itemState->settings);
      } else {
        revealItemAfterRepeat();
      }
      seekLockMs_ = inPoint;
      seekLockTicks_ = 20;
      programPanel_->setTimelinePosition(inPoint);
      programPanel_->setRemainingTime(rangeDuration);
      refreshOnAirEndModePresentation();
      clearFault();
      return;
    }
    player_.pause();
    onAirStateMachine_.fail();
    refreshOnAirEndModePresentation();
    const QString detail =
        QStringLiteral("Could not execute End Mode \"Repeat\".");
    setFault(detail);
    synchronizeProgramPosition();
    showWarning(detail);
    return;
  }

  selectNextItemAfterOnAir(onAirMedia->itemId);
  refreshOnAirEndModePresentation();

  if (playlistModel_ &&
      (*endMode == PlaylistEndMode::Next ||
       *endMode == PlaylistEndMode::Goto)) {
    playlistModel_->refreshMediaAvailability();
  }
  if (*endMode == PlaylistEndMode::Goto && itemState && playlistModel_ &&
      itemState->settings.gotoTargetItemId != 0 &&
      playlistModel_->itemMediaOffline(
          itemState->settings.gotoTargetItemId)) {
    clearOnAir();
    return;
  }

  if (*endMode == PlaylistEndMode::Stop) {
    clearOnAir();
    return;
  }

  if (*endMode == PlaylistEndMode::Next ||
      *endMode == PlaylistEndMode::Goto) {
    nextTransition_.mode = *endMode;
    nextTransition_.due = true;
    maybePrepareNextItem(endPosition, endPosition);
    if (takePreparedNextItem()) {
      return;
    }
    if (onAirStateMachine_.phase() == PlaylistItemPlayoutPhase::Error) {
      return;
    }
    programPanel_->setTimelinePosition(endPosition);
    programPanel_->setRemainingTime(0);
    audioController_.setActive(false);
    if (!nextTransition_.itemId) {
      if (powerPointConversionInProgress_ &&
          powerPointNextTransitionDeferred_) {
        return;
      }
      failNextTransition(
          *endMode == PlaylistEndMode::Goto
              ? QStringLiteral("No eligible GOTO destination is selected or available.")
              : QStringLiteral("No eligible next playlist item is available."));
    }
    return;
  }

  bool paused = true;
  if (onAirTimedStillClock_.active()) {
    onAirTimedStillClock_.holdAtEnd();
  } else {
    paused = player_.pause();
  }
  if (paused && !onAirTimedStillClock_.active() &&
      !onAirMedia->outPointMs && endPosition > inPoint) {
    const qint64 holdPosition = (std::max)(inPoint, endPosition - qint64{1});
    paused = player_.seek(holdPosition) && player_.pause();
  }
  if (paused) {
    programPanel_->setTimelinePosition(endPosition);
    programPanel_->setRemainingTime(0);
    audioController_.setActive(false);
    clearFault();
    return;
  }
  onAirStateMachine_.fail();
  const QString detail =
      QStringLiteral("Could not execute End Mode \"Pause\".");
  setFault(detail);
  synchronizeProgramPosition();
  showWarning(detail);
}

void ProgramPlayoutController::updatePlaybackUi() {
  // Poll file repeat boundaries at display cadence; the ordinary UI path uses
  // 100 ms. The prepared first frame is retained throughout an EOS handoff.
  const int interval = usesAlternatingRepeat() ? 16 : 100;
  if (busTimer_.interval() != interval) {
    busTimer_.setInterval(interval);
  }
  const qint64 onAirElapsedMs = onAirTimedStillElapsedTimer_.isValid()
                                    ? (std::max)(
                                          qint64{0},
                                          onAirTimedStillElapsedTimer_.restart())
                                    : 0;
  const qint64 dskElapsedMs = dskTimedStillElapsedTimer_.isValid()
                                  ? (std::max)(
                                        qint64{0},
                                        dskTimedStillElapsedTimer_.restart())
                                  : 0;
  onAirTimedStillCompletionDue_ =
      onAirTimedStillClock_.advance(onAirElapsedMs) ||
      onAirTimedStillCompletionDue_;
  dskTimedStillCompletionDue_ =
      dskTimedStillClock_.advance(dskElapsedMs) ||
      dskTimedStillCompletionDue_;
  player_.processBusMessages();
  if (bgmController_) {
    bgmController_->update();
  }
  updateCueUi();
  updateDskUi();
  if (!programPanel_) {
    return;
  }
  programPanel_->setPlaybackState(
      onAirStateMachine_.preparedMedia() != nullptr, programIsPlaying());
  updatePowerPointNavigationUi();
  const qint64 duration = programDurationMs();
  if (duration >= 0) {
    programPanel_->setTimelineDuration(duration);
  }
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (onAirMedia && playlistModel_ && duration > 0) {
    const auto item = playlistModel_->itemState(onAirMedia->itemId);
    if (item && item->sourceType == PlaylistSourceType::File &&
        item->mediaType != PlaylistMediaType::StillImage) {
      playlistModel_->setKnownMediaDuration(item->id, duration);
    }
  }
  if (!onAirMedia) {
    programPanel_->clearTotalRemainingTime();
  }
  refreshOnAirEndModePresentation();
  const bool live = onAirIsLive();
  if (live) {
    pendingSeekMs_.reset();
    seekLockMs_.reset();
    seekLockTicks_ = 0;
    programPanel_->setTimelineDuration(-1);
    programPanel_->setRemainingLive();
    updateTotalRemainingTime(-1);
  }
  const qint64 endPosition = playbackEnd(duration);
  if (pendingSeekMs_) {
    qint64 target = *pendingSeekMs_;
    pendingSeekMs_.reset();
    if (onAirMedia && endPosition >= onAirMedia->inPointMs) {
      target = std::clamp(target, onAirMedia->inPointMs, endPosition);
    }
    const bool timedStillWasPlaying = onAirTimedStillClock_.playing();
    const bool seekSucceeded = onAirTimedStillClock_.active()
                                   ? (onAirTimedStillClock_.seek(target), true)
                                   : player_.seek(target);
    if (seekSucceeded) {
      if (onAirTimedStillClock_.active()) {
        onAirTimedStillElapsedTimer_.restart();
        onAirTimedStillCompletionDue_ =
            timedStillWasPlaying &&
            onAirTimedStillClock_.positionMs() >=
                onAirTimedStillClock_.durationMs();
        if (onAirTimedStillCompletionDue_) {
          onAirTimedStillClock_.pause();
        }
      }
      seekLockMs_ = target;
      seekLockTicks_ = 20;
      if (onAirMedia) {
        (void)onAirStateMachine_.markActive();
      }
      clearFault();
    } else {
      const QString detail = QStringLiteral("ON AIR seek failed.");
      setFault(detail);
      showWarning(detail);
      synchronizeProgramPosition();
    }
  }

  const qint64 position = programPositionMs();
  if (usesAlternatingRepeat()) {
    maybePrepareNextItem(position, endPosition);
  }
  if (!programPanel_->isTimelineSliderDown() && position >= 0) {
    if (seekLockMs_ && qAbs(position - *seekLockMs_) > 1000 &&
        --seekLockTicks_ > 0) {
      programPanel_->setTimelinePosition(*seekLockMs_);
    } else {
      seekLockMs_.reset();
      programPanel_->setTimelinePosition(position);
    }
  }
  if (onAirMedia && endPosition >= 0 && position >= 0) {
    const qint64 displayedPosition =
        std::clamp(programPanel_->timelinePosition(),
                   onAirMedia->inPointMs, endPosition);
    const qint64 currentItemRemainingMs =
        endPosition - displayedPosition;
    programPanel_->setRemainingTime(currentItemRemainingMs);
    updateTotalRemainingTime(currentItemRemainingMs);
    maybeStartItemFadeOut(position, endPosition);
    maybePrepareNextItem(position, endPosition);
  }

  QString detail;
  bool programAudioError = false;
  const DualDeckPlayer::Event event = player_.takeEvent(detail, &programAudioError);
  if (event == DualDeckPlayer::Event::Error) {
    if (programAudioError) {
      finishMacroTransportRequest(false, detail);
      emit programAudioOutputError(detail);
      return;
    }
    const auto item = onAirMedia ? playlistModel_->itemState(onAirMedia->itemId) : std::nullopt;
    if (item && item->sourceType == PlaylistSourceType::Uvc && vtrpon::isLiveAudioSource(item->filePath)) {
      playlistModel_->setUvcDeviceAvailable(item->filePath, false);
      if (playlistModel_->onAirItemId() == item->id) clearOnAir();
      userInfoCenter_->showError(QStringLiteral("Live Audio"),
                                vtrpon::LocalizedText("input.audio.error"), detail);
      return;
    }
    if (!programAudioError && item && item->sourceType == PlaylistSourceType::Uvc && uvcSourceSessions_) {
      uvcSourceSessions_->reportSourceFailure(item->filePath, detail);
      if (playlistModel_->onAirItemId() == item->id) clearOnAir();
      return;
    }
    const bool stopped = player_.stop();
    onAirTimedStillRestorePending_ = false;
    if (onAirTimedStillClock_.active()) {
      if (stopped) {
        onAirTimedStillClock_.pause();
      } else {
        onAirTimedStillClock_.reset();
      }
      onAirTimedStillCompletionDue_ = false;
    }
    if (onAirMedia) {
      if (stopped) {
        (void)onAirStateMachine_.markEnded();
      } else {
        onAirStateMachine_.fail();
      }
    }
    if (stopped) {
      audioController_.setActive(false);
    }
    if (!stopped) {
      detail += QStringLiteral("\nThe playback pipeline also failed to stop.");
    }
    if (nextTransition_.itemId || nextTransition_.preparationAttempted || nextTransition_.due) {
      cancelNextTransition(false);
    }
    synchronizeProgramPosition();
    reportPlaybackError(detail);
    finishMacroTransportRequest(false, detail);
    return;
  }

  if (onAirTimedStillRestorePending_) {
    onAirTimedStillRestorePending_ = false;
    playOnAir();
  }
  updateMacroTransportRequest();

  if (usesAlternatingRepeat() && !nextTransition_.paused) {
    const qint64 crossfadeDuration = repeatCrossfadeDurationMs(endPosition);
    if (!nextTransition_.due && crossfadeDuration > 0 && programIsPlaying() &&
        position >= 0 && endPosition >= position &&
        endPosition - position <= crossfadeDuration * playbackRate_) {
      nextTransition_.mode = PlaylistEndMode::Repeat;
      nextTransition_.due = true;
    }
    if (nextTransition_.due && takePreparedNextItem()) {
      return;
    }
  }

  if (onAirTimedStillCompletionDue_) {
    onAirTimedStillCompletionDue_ = false;
    handleOnAirCompletion(endPosition);
    return;
  }

  if (event == DualDeckPlayer::Event::EndOfStream &&
      !onAirTimedStillClock_.active()) {
    handleOnAirCompletion(endPosition);
  }
}

void ProgramPlayoutController::reportPlaybackError(const QString &detail) {
  setFault(detail);
  if (userInfoCenter_) {
    userInfoCenter_->showError(QStringLiteral("PROGRAM"),
        vtrpon::LocalizedText("program.error.gstreamer_playback"), detail);
  }
}

void ProgramPlayoutController::refreshOnAirEndModePresentation() {
  if (!programPanel_ || !playlistModel_) {
    return;
  }
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  const auto item = onAirMedia
                        ? playlistModel_->itemState(onAirMedia->itemId)
                        : std::nullopt;
  if (!item || item->sourceType == PlaylistSourceType::PowerPoint ||
      item->sourceType == PlaylistSourceType::Pdf) {
    programPanel_->clearEndMode();
    return;
  }
  const int currentPlayCount = onAirStateMachine_.currentPlayCount();
  playlistModel_->setItemCurrentPlayCount(item->id, currentPlayCount);
  programPanel_->setEndMode(item->settings, currentPlayCount);
}

void ProgramPlayoutController::toggleFtb() {
  ftbTargetCovered_ = !ftbTargetCovered_;
  emit ftbChanged(ftbTargetCovered_);
  ftbAnimation_.stop();
  ftbAnimation_.setStartValue(ftbCoverage_);
  ftbAnimation_.setEndValue(ftbTargetCovered_ ? 1.0 : 0.0);
  ftbBlinkRed_ = true;
  if (programPanel_) {
    programPanel_->setFtbButtonActive(true);
  }
  ftbBlinkTimer_.start();
  ftbAnimation_.start();
}

void ProgramPlayoutController::applyFtbCoverage(qreal coverage) {
  ftbCoverage_ = qBound<qreal>(0.0, coverage, 1.0);
  player_.setFtbCoverage(ftbCoverage_);
}

void ProgramPlayoutController::resetFtb() {
  const bool wasCovered = ftbTargetCovered_;
  ftbAnimation_.stop();
  ftbBlinkTimer_.stop();
  ftbTargetCovered_ = false;
  ftbBlinkRed_ = false;
  applyFtbCoverage(0.0);
  if (programPanel_) {
    programPanel_->setFtbButtonActive(false);
  }
  if (wasCovered) {
    emit ftbChanged(false);
  }
}

void ProgramPlayoutController::showWarning(const QString &text) {
  if (userInfoCenter_) {
    static constexpr auto knownWarnings =
        std::to_array<std::pair<const char *, const char *>>({
            {"An item currently ON AIR cannot be deleted.", "program.warning.delete_on_air"},
            {"An item currently in DSK cannot be deleted.", "program.warning.delete_dsk"},
            {"The item cannot be deleted because DSK could not be turned off.", "program.warning.delete_dsk_off_failed"},
            {"The item cannot be deleted because its preparation could not be released.", "program.warning.delete_cue_release_failed"},
            {"Failed to switch OFF AIR. The playback state was not changed.", "program.warning.off_air_failed"},
            {"Select an item to take ON AIR.", "program.warning.select_on_air_item"},
            {"Could not take the selected item ON AIR.", "program.warning.take_cue_failed"},
            {"Could not retrieve the playlist item to take ON AIR.", "program.warning.retrieve_on_air_item"},
            {"Could not retrieve the latest item information when ON AIR started.", "program.warning.retrieve_on_air_latest"},
            {"The item was taken ON AIR, but Start Mode \"Play\" could not be started.", "program.warning.start_play_failed"},
            {"Select a valid playlist item to send to DSK.", "program.warning.select_dsk_item"},
            {"Could not prepare the DSK item.", "program.warning.dsk_cue_failed"},
            {"Could not display DSK.", "program.warning.dsk_display_failed"},
            {"Could not execute DSK End Mode \"Stop\".", "program.warning.dsk_end_stop_failed"},
            {"Could not execute DSK End Mode \"Pause\".", "program.warning.dsk_end_pause_failed"},
            {"Could not execute DSK End Mode \"Repeat\".", "program.warning.dsk_end_repeat_failed"},
            {"Could not execute DSK End Mode \"Next\".", "program.warning.dsk_end_next_failed"},
            {"Could not turn DSK off.", "program.warning.dsk_off_failed"},
            {"A playback error occurred in DSK.", "program.warning.dsk_playback_error"},
            {"Failed to seek to the In point before playback.", "program.warning.seek_in_failed"},
            {"Playback failed.", "program.warning.playback_failed"},
            {"Pause failed.", "program.warning.pause_failed"},
            {"Playback rate change failed.", "program.warning.playback_rate_failed"},
            {"Could not retrieve the latest playlist item information during end processing.", "program.warning.retrieve_end_item"},
            {"Could not execute End Mode \"Repeat\".", "program.warning.end_repeat_failed"},
            {"Could not execute End Mode \"Stop\".", "program.warning.end_stop_failed"},
            {"Could not execute End Mode \"Pause\".", "program.warning.end_pause_failed"},
            {"Could not execute End Mode \"Next\".", "program.warning.end_next_failed"},
            {"ON AIR seek failed.", "program.warning.seek_failed"},
        });
    for (const auto &[english, id] : knownWarnings) {
      if (text == QLatin1StringView(english)) {
        userInfoCenter_->showWarning(QStringLiteral("PROGRAM"),
                                     vtrpon::LocalizedText(id));
        return;
      }
    }
    userInfoCenter_->showWarning(QStringLiteral("PROGRAM"), text);
  }
}

void ProgramPlayoutController::setFault(const QString &detail) {
  if (programPanel_) {
    programPanel_->setProgramFault(detail);
  }
  if (playlistModel_ && playlistModel_->onAirItemId() != 0) {
    playlistModel_->setOnAirError(detail);
  }
}

void ProgramPlayoutController::clearFault() {
  if (programPanel_) {
    programPanel_->clearProgramFault();
  }
  if (playlistModel_ && playlistModel_->onAirItemId() != 0 &&
      !playlistModel_->onAirErrorDetail().isEmpty()) {
    playlistModel_->setOnAirError(QString());
  }
}

void ProgramPlayoutController::synchronizeProgramPosition() {
  if (!programPanel_) {
    return;
  }
  if (onAirIsLive()) {
    programPanel_->setTimelineDuration(-1);
    programPanel_->setRemainingLive();
    updateTotalRemainingTime(-1);
    return;
  }
  const qint64 duration = programDurationMs();
  if (duration >= 0) {
    programPanel_->setTimelineDuration(duration);
  }
  const qint64 position = programPositionMs();
  if (position >= 0) {
    programPanel_->setTimelinePosition(position);
  }
  if (onAirStateMachine_.preparedMedia()) {
    const qint64 endPosition = playbackEnd(duration);
    if (endPosition >= 0 && position >= 0) {
      const qint64 currentItemRemainingMs =
          (std::max)(qint64{0}, endPosition - position);
      programPanel_->setRemainingTime(currentItemRemainingMs);
      updateTotalRemainingTime(currentItemRemainingMs);
    }
  }
}

void ProgramPlayoutController::updateTotalRemainingTime(
    qint64 currentItemRemainingMs) {
  if (!programPanel_ || !programPanel_->totalModeEnabled()) {
    if (programPanel_) {
      programPanel_->clearTotalRemainingTime();
    }
    return;
  }
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (!playlistModel_ || !onAirMedia) {
    programPanel_->clearTotalRemainingTime();
    return;
  }

  const PlaylistTotalRemaining total = playlistModel_->totalRemainingForPlayout(
      onAirMedia->itemId,
      PlaylistMapPlaybackProgress{
          (std::max)(currentItemRemainingMs, qint64{0}),
          (std::max)(onAirStateMachine_.currentPlayCount(), 1), playbackRate_});
  if (total.infinite) {
    programPanel_->setTotalRemainingInfinite();
    return;
  }
  if (!total.durationMs) {
    programPanel_->clearTotalRemainingTime();
    return;
  }
  programPanel_->setTotalRemainingTime(*total.durationMs);
}

qint64 ProgramPlayoutController::playbackEnd(qint64 duration) const {
  const PlaylistPreparedMedia *onAirMedia =
      onAirStateMachine_.preparedMedia();
  if (!onAirMedia) {
    return duration;
  }
  qint64 endPosition = onAirMedia->outPointMs.value_or(duration);
  if (duration >= 0 && endPosition >= 0) {
    endPosition = (std::min)(endPosition, duration);
  }
  return endPosition;
}

qint64 ProgramPlayoutController::programPositionMs() const {
  return onAirTimedStillClock_.active()
             ? onAirTimedStillClock_.positionMs()
             : player_.positionMs();
}

qint64 ProgramPlayoutController::programDurationMs() const {
  return onAirTimedStillClock_.active()
             ? onAirTimedStillClock_.durationMs()
             : player_.durationMs();
}

bool ProgramPlayoutController::programIsPlaying() const {
  return onAirTimedStillClock_.active()
             ? onAirTimedStillClock_.playing()
             : player_.isPlaying();
}

bool ProgramPlayoutController::onAirIsLive() const {
  const PlaylistPreparedMedia *media = onAirStateMachine_.preparedMedia();
  if (!media || !playlistModel_) {
    return false;
  }
  const auto state = playlistModel_->itemState(media->itemId);
  return state && state->sourceType != PlaylistSourceType::File;
}

void ProgramPlayoutController::selectNextItemAfterOnAir(quint64 itemId) {
  if (!autoSelectNextItemAfterOnAir_ || !playlistPanel_ || itemId == 0) {
    return;
  }
  const QPointer<PlaylistPanel> guardedPlaylistPanel = playlistPanel_;
  QTimer::singleShot(0, this, [this, guardedPlaylistPanel, itemId] {
    if (guardedPlaylistPanel) {
      QScopedValueRollback<bool> automaticSelection(internalAutoSelection_, true);
      (void)guardedPlaylistPanel->selectNextItemAfter(itemId);
    }
  });
}
void ProgramPlayoutController::setBackgroundColor(const QColor &color) {
  player_.setBackgroundColor(color);
}

void ProgramPlayoutController::requestProgramOutputDevice(const QString &id) { player_.requestProgramOutputDevice(id); }
QString ProgramPlayoutController::appliedProgramOutputDevice() const { return player_.appliedProgramOutputDevice(); }
QString ProgramPlayoutController::programOutputDeviceStatusKey() const { return player_.programOutputDeviceStatusKey(); }
bool ProgramPlayoutController::programAudioOutputReady() const { return player_.programAudioOutputReady(); }
