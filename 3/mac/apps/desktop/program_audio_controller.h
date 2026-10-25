#pragma once

#include "dual_deck_player.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

class ProgramPanel;

class ProgramAudioController final : public QObject {
  Q_OBJECT

public:
  ProgramAudioController(DualDeckPlayer *player, ProgramPanel *panel,
                         QObject *parent = nullptr);

  void setActive(bool active);
  void setBgmActive(bool active);
  void reset();

private:
  void updateMuteAppearance();
  bool updateInputSignalIndicator(const DualDeckPlayer::AudioLevel &inputLevel,
                                  bool forceOff = false);
  void updateAudioMeter();

  DualDeckPlayer *player_ = nullptr;
  QPointer<ProgramPanel> panel_;
  QTimer meterTimer_;
  QElapsedTimer inputSignalClock_;
  qint64 inputSignalUntilMs_ = 0;
  qint64 inputClipUntilMs_ = 0;
  DualDeckPlayer::AudioLevels lastAudioLevels_;
  bool haveLastAudioLevels_ = false;
  bool inputSignalNeedsRefresh_ = false;
  bool itemMeterNeedsRefresh_ = false;
  bool masterMeterNeedsRefresh_ = false;
  bool inputSignalVisualInitialized_ = false;
  bool inputSignalClipped_ = false;
  bool inputSignalActive_ = false;
  bool masterMuted_ = false;
  bool onAirActive_ = false;
  bool bgmActive_ = false;
};
