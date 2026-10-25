#include "program_audio_controller.h"

#include "program_panel.h"

#include <algorithm>
#include <chrono>

namespace {

constexpr auto kAudioMeterRefreshInterval = std::chrono::milliseconds{8};
constexpr int kInputSignalHoldMs = 250;
constexpr int kInputClipHoldMs = 500;
constexpr double kMeterFloorDb = -60.0;

bool sameAudioLevel(const DualDeckPlayer::AudioLevel &left,
                    const DualDeckPlayer::AudioLevel &right) {
  return left.leftPeakDb == right.leftPeakDb &&
         left.rightPeakDb == right.rightPeakDb && left.clipped == right.clipped;
}

} // namespace

ProgramAudioController::ProgramAudioController(DualDeckPlayer *player,
                                               ProgramPanel *panel,
                                               QObject *parent)
    : QObject(parent), player_(player), panel_(panel) {
  connect(panel, &ProgramPanel::itemDelayChanged, this,
          [this](int frames) { player_->setItemDelayFrames(frames); });
  connect(panel, &ProgramPanel::masterDelayChanged, this,
          [this](int frames) { player_->setMasterDelayFrames(frames); });
  connect(panel, &ProgramPanel::itemEqChanged, this,
          [this](double low, double mid, double high) { player_->setItemEq(low, mid, high); });
  connect(panel, &ProgramPanel::masterEqChanged, this,
          [this](double lowDb, double midDb, double highDb) {
            player_->setMasterEq(lowDb, midDb, highDb);
          });
  connect(panel, &ProgramPanel::masterVolumeChanged, this, [this](double gain) {
    player_->setMasterVolume(gain);
    updateMuteAppearance();
  });
  connect(panel, &ProgramPanel::itemVolumeChanged, this,
          [this](double gain) { player_->setItemVolume(gain); });
  connect(panel, &ProgramPanel::masterMuteChanged, this, [this](bool muted) {
    masterMuted_ = muted;
    player_->setMasterMuted(masterMuted_);
    updateMuteAppearance();
  });
  connect(&meterTimer_, &QTimer::timeout, this,
          &ProgramAudioController::updateAudioMeter);
  meterTimer_.setTimerType(Qt::PreciseTimer);
  meterTimer_.setInterval(kAudioMeterRefreshInterval);

  player_->setItemVolume(panel_->itemGain());
  player_->setMasterVolume(panel_->masterGain());
  const auto eq = panel_->masterEqGains();
  player_->setMasterEq(eq[0], eq[1], eq[2]);
  player_->setMasterMuted(false);
  updateMuteAppearance();
  reset();
}

void ProgramAudioController::setActive(bool active) {
  onAirActive_ = active;
  if (onAirActive_ || bgmActive_) {
    if (!meterTimer_.isActive()) {
      meterTimer_.start();
    }
  } else {
    reset();
  }
}

void ProgramAudioController::setBgmActive(bool active) {
  if (player_) {
    player_->setBgmMeterActive(active);
  }
  bgmActive_ = active;
  if (onAirActive_ || bgmActive_) {
    if (!meterTimer_.isActive()) {
      meterTimer_.start();
    }
  } else {
    reset();
  }
}

void ProgramAudioController::reset() {
  meterTimer_.stop();
  inputSignalUntilMs_ = 0;
  inputClipUntilMs_ = 0;
  inputSignalNeedsRefresh_ = false;
  itemMeterNeedsRefresh_ = false;
  masterMeterNeedsRefresh_ = false;
  inputSignalVisualInitialized_ = false;
  inputSignalClipped_ = false;
  inputSignalActive_ = false;
  lastAudioLevels_ = {};
  haveLastAudioLevels_ = false;
  if (panel_) {
    panel_->resetAudioPresentation();
  }
}

void ProgramAudioController::updateMuteAppearance() {
  if (!panel_) {
    return;
  }
  panel_->setMuteAppearance(masterMuted_,
                            masterMuted_ || panel_->masterFaderAtSilence());
}

bool ProgramAudioController::updateInputSignalIndicator(
    const DualDeckPlayer::AudioLevel &inputLevel, bool forceOff) {
  if (!panel_) {
    return false;
  }
  if (!inputSignalClock_.isValid()) {
    inputSignalClock_.start();
  }
  const qint64 now = inputSignalClock_.elapsed();
  if (forceOff) {
    inputSignalUntilMs_ = 0;
    inputClipUntilMs_ = 0;
  } else {
    if ((std::max)(inputLevel.leftPeakDb, inputLevel.rightPeakDb) >
        kMeterFloorDb) {
      inputSignalUntilMs_ = now + kInputSignalHoldMs;
    }
    if (inputLevel.clipped) {
      inputClipUntilMs_ = now + kInputClipHoldMs;
    }
  }
  const bool clipped = now < inputClipUntilMs_;
  const bool active = now < inputSignalUntilMs_;
  if (inputSignalVisualInitialized_ && clipped == inputSignalClipped_ &&
      active == inputSignalActive_) {
    return clipped || active;
  }
  inputSignalVisualInitialized_ = true;
  inputSignalClipped_ = clipped;
  inputSignalActive_ = active;
  panel_->setInputSignalAppearance(active, clipped);
  return clipped || active;
}

void ProgramAudioController::updateAudioMeter() {
  if (!panel_) {
    meterTimer_.stop();
    return;
  }
  const DualDeckPlayer::AudioLevels levels = player_->audioLevels();
  panel_->setBgmMeter(levels.bgmPostFader.leftPeakDb,
                     levels.bgmPostFader.rightPeakDb, levels.bgmPostFader.clipped);
  if (!haveLastAudioLevels_ || inputSignalNeedsRefresh_ ||
      !sameAudioLevel(levels.input, lastAudioLevels_.input)) {
    inputSignalNeedsRefresh_ = updateInputSignalIndicator(levels.input);
  }
  if (!haveLastAudioLevels_ || itemMeterNeedsRefresh_ ||
      !sameAudioLevel(levels.itemPostFader, lastAudioLevels_.itemPostFader)) {
    itemMeterNeedsRefresh_ = panel_->setItemMeter(
        levels.itemPostFader.leftPeakDb, levels.itemPostFader.rightPeakDb,
        levels.itemPostFader.clipped);
  }
  if (!haveLastAudioLevels_ || masterMeterNeedsRefresh_ ||
      !sameAudioLevel(levels.masterOutput, lastAudioLevels_.masterOutput)) {
    masterMeterNeedsRefresh_ = panel_->setMasterMeter(
        levels.masterOutput.leftPeakDb, levels.masterOutput.rightPeakDb,
        levels.masterOutput.clipped);
  }
  lastAudioLevels_ = levels;
  haveLastAudioLevels_ = true;
}
