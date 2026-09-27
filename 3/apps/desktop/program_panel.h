#pragma once

#include "playlist_item.h"

#include <QWidget>
#include <QColor>
#include <QJsonObject>
#include "controller_operations.h"

#include <array>
#include <optional>

class QAction;
class QMenu;
class AudioMeterWidget;
class QDoubleSpinBox;
class QSpinBox;
class QDialog;
class QDial;
namespace vtrpon { class EqualizerEditor; }
class QLabel;
class QPaintEvent;
class AudioFaderWidget;
class QPushButton;
class RangeMarkerSlider;
class QEvent;
class QToolButton;
class QTimer;
class QVariantAnimation;

class ProgramPanel final : public QWidget {
  Q_OBJECT

public:
  enum class DskCueState { Empty, Loading, Ready, Error };
  enum class BgmPresentationState {
    Stopped,
    Loading,
    Playing,
    Paused,
    Error,
  };
  enum class BgmFadeState { Idle, FadeIn, FadeOut };

  explicit ProgramPanel(QWidget *parent = nullptr,
                        ControllerOperations *operations = nullptr);
  ControllerOperations *operations() const { return operations_; }

  void setPlayerBackground(const QColor &color);
  QWidget *monitorWindow() const;
  void setFile(const QString &fileName, const QString &absolutePath);
  QStringList operatorMonitorStatus() const;
  void setOperatorTimerPresentation(const QJsonObject &presentation);
  void clearFile();
  void setProgramFault(const QString &detail);
  void clearProgramFault();
  void setTimelineDuration(qint64 durationMs);
  void setTimelinePosition(qint64 positionMs);
  qint64 timelinePosition() const;
  bool isTimelineSliderDown() const;
  void setRangeMarkers(qint64 inPointMs, std::optional<qint64> outPointMs);
  void clearRangeMarkers();
  void setRemainingTime(qint64 remainingMs);
  void setRemainingLive();
  void setTotalRemainingTime(qint64 remainingMs);
  void setTotalRemainingInfinite();
  void clearTotalRemainingTime();
  bool totalModeEnabled() const;
  void applyTimeSettings();
  void setPowerPointNavigationState(bool visible, bool canGoPrevious,
                                    bool canGoNext, int currentSlide = 0,
                                    int slideCount = 0, bool busy = false);
  void setPdfNavigationState(bool visible, bool canGoPrevious,
                             bool canGoNext, int currentPage = 0,
                             int pageCount = 0, bool busy = false);
  void setEndMode(const PlaylistItemSettings &settings,
                  int currentPlayCount);
  void clearEndMode();
  void setOffAirButtonVisible(bool visible);
  void setFtbControlsVisible(bool visible);
  void setPlaybackRateControlsVisible(bool visible);
  void setPlaybackRateControlsEnabled(bool enabled);
  void setPlaybackRate(double rate);
  void setBgmControlsVisible(bool visible);
  bool bgmControlsVisible() const;
  bool configuredBgmFadeAvailable() const;
  qint64 configuredBgmFadeInDurationMs() const;
  qint64 configuredBgmFadeOutDurationMs() const;
  void setBgmPresentation(BgmPresentationState state,
                          const QString &playlistName = {},
                          const QString &itemName = {},
                          const QString &detail = {},
                          bool volumeAtSilence = false);
  void setBgmFadePresentation(BgmFadeState state,
                              bool volumeAtSilence = false);
  void setBgmSignalAppearance(bool active, bool clipped);
  void setItemFaderValue(int value);
  void setItemFadeProgress(qreal progress);
  double itemGain() const;
  int itemDelayFrames() const;
  std::array<int, 3> itemEqGains() const { return itemEqTenthsDb_; }
  double bgmGain() const;
  void setBgmFaderGain(double gain);
  void setBgmMeter(double leftPeakDb, double rightPeakDb, bool clipped);
  double masterGain() const;
  std::array<double, 3> masterEqGains() const { return masterEqDb_; }
  void setItemDelayFrames(int frames);
  void setItemEqGains(const std::array<int, 3> &gains);
  bool masterFaderAtSilence() const;
  void setPlaybackState(bool hasOnAirMedia, bool playing);
  void setFtbButtonActive(bool active);
  void setDskSelectionAvailable(bool available);
  void setDskCueState(DskCueState state, const QString &fileName = {},
                      const QString &detail = {});
  void setDskPendingOn(bool pending);
  void setDskOn(bool on);
  void setDskPlaybackState(bool playing, bool enabled = true);
  void setMuteAppearance(bool explicitlyMuted, bool effectivelyMuted);
  void setInputSignalAppearance(bool active, bool clipped);
  bool setItemMeter(double leftPeakDb, double rightPeakDb, bool clipped);
  bool setMasterMeter(double leftPeakDb, double rightPeakDb, bool clipped);
  void resetAudioPresentation();
  void setPlayPauseShortcutLabel(const QString &shortcut);
  void triggerShortcutAction(ControllerOperations::Action action,
                             const char *inputSource = "Shortcut");

signals:
  void operatorTimerStartRequested();
  void operatorTimerPauseRequested();
  void operatorTimerResetRequested();
  void operatorTimerSettingsRequested();
  void playerBackgroundRequested(const QColor &color);
  void offAirRequested();
  void playRequested();
  void pauseRequested();
  void ftbRequested();
  void dskToggleRequested();
  void dskPlayRequested();
  void dskPauseRequested();
  void seekRequested(qint64 positionMs);
  void itemVolumeChanged(double linearGain);
  void bgmVolumeChanged(double linearGain);
  void masterVolumeChanged(double linearGain);
  void masterEqChanged(double lowDb, double midDb, double highDb);
  void itemDelayChanged(int frames);
  void masterDelayChanged(int frames);
  void itemEqChanged(double lowDb, double midDb, double highDb);
  void masterMuteChanged(bool muted);
  void powerPointPreviousRequested();
  void powerPointNextRequested();
  void playbackRateRequested(double rate);
  void bgmStopRequested();
  void bgmPlayRequested();
  void bgmPauseRequested();

protected:
  void changeEvent(QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  ControllerOperations *operations_ = nullptr;
  QColor playerBackgroundColor_{Qt::black};
  QToolButton *backgroundColorButton_ = nullptr;
  QMenu *backgroundColorMenu_ = nullptr;
  std::array<QAction *, 4> backgroundColorActions_{};
  QAction *backgroundColorTitleAction_ = nullptr;
  QAction *otherBackgroundColorAction_ = nullptr;
  struct FadeControlWidgets {
    QWidget *control = nullptr;
    QPushButton *button = nullptr;
    QDoubleSpinBox *durationSpinBox = nullptr;
    QToolButton *increaseButton = nullptr;
    QToolButton *decreaseButton = nullptr;
    double durationSeconds = 0.0;
  };

  enum class ManualFadeTarget { None, Item, Master };
  enum class DocumentNavigationKind { PowerPoint, Pdf };

  FadeControlWidgets createFadeControl(const QString &objectNamePrefix,
                                       const QString &buttonText,
                                       QWidget *parent);
  void retranslateFadeControl(FadeControlWidgets &control,
                              const QString &channelName,
                              const QString &fadeName);
  void startManualFade(AudioFaderWidget *fader, QPushButton *button,
                       double durationSeconds, double targetGain,
                       ManualFadeTarget target);
  void cancelManualFade();
  void handleUserFaderOperation(AudioFaderWidget *fader);
  void retranslateUi();
  void showItemEqWindow();
  void showMasterEqWindow();
  void updateMasterEqPresentation();
  void updateBackgroundPresentation();
  void updateFilePresentation();
  void updateRemainingPresentation();
  void updateEndModePresentation();
  void updateBgmPresentation();
  void updateBgmFadeButtonAppearance();
  void updateDskControls();
  void applyDocumentNavigationState(bool visible, bool canGoPrevious,
                                    bool canGoNext, int currentPosition,
                                    int positionCount, bool busy);

  QWidget *monitorWindow_ = nullptr;
  QLabel *inputSignalIndicator_ = nullptr;
  QLabel *fileLabel_ = nullptr;
  QLabel *remainingTimeLabel_ = nullptr;
  QPushButton *totalModeButton_ = nullptr;
  QLabel *endModeLabel_ = nullptr;
  QWidget *playbackRatePanel_ = nullptr;
  QWidget *ftbPanel_ = nullptr;
  QLabel *playbackRateTitle_ = nullptr;
  std::array<QPushButton *, 7> playbackRateButtons_{};
  QWidget *bgmControlPanel_ = nullptr;
  QLabel *bgmControlTitle_ = nullptr;
  QLabel *bgmItemLabel_ = nullptr;
  QLabel *bgmSignalIndicator_ = nullptr;
  QPushButton *bgmStopButton_ = nullptr;
  QPushButton *bgmPlayButton_ = nullptr;
  QPushButton *bgmPauseButton_ = nullptr;
  FadeControlWidgets bgmFadeInControl_;
  FadeControlWidgets bgmFadeOutControl_;
  QTimer *bgmFadeBlinkTimer_ = nullptr;
  QWidget *powerPointNavigationPanel_ = nullptr;
  QLabel *powerPointNavigationTitle_ = nullptr;
  QLabel *powerPointPositionLabel_ = nullptr;
  QPushButton *powerPointPreviousButton_ = nullptr;
  QPushButton *powerPointNextButton_ = nullptr;
  AudioMeterWidget *itemMeter_ = nullptr;
  QWidget *audioStrip_ = nullptr;
  QWidget *operatorTimerPanel_ = nullptr;
  QLabel *operatorTimerTitle_ = nullptr;
  QLabel *operatorTimerRemaining_ = nullptr;
  QPushButton *operatorTimerStart_ = nullptr;
  QPushButton *operatorTimerPause_ = nullptr;
  QPushButton *operatorTimerReset_ = nullptr;
  QPushButton *operatorTimerSettings_ = nullptr;
  QJsonObject operatorTimerPresentation_;
  AudioMeterWidget *masterMeter_ = nullptr;
  AudioFaderWidget *itemFader_ = nullptr;
  AudioFaderWidget *bgmFader_ = nullptr;
  AudioMeterWidget *bgmMeter_ = nullptr;
  QPushButton *bgmFaderButton_ = nullptr;
  AudioFaderWidget *masterFader_ = nullptr;
  QPushButton *masterEqButton_ = nullptr;
  QSpinBox *itemDelaySpin_ = nullptr;
  QSpinBox *masterDelaySpin_ = nullptr;
  QPushButton *itemEqButton_ = nullptr;
  vtrpon::EqualizerEditor *itemEqEditor_ = nullptr;
  std::array<int, 3> itemEqTenthsDb_{};
  QDialog *masterEqWindow_ = nullptr;
  QPushButton *masterEqResetButton_ = nullptr;
  std::array<QDial *, 3> masterEqDials_{};
  std::array<QLabel *, 3> masterEqValueLabels_{};
  std::array<double, 3> masterEqDb_{};
  FadeControlWidgets itemFadeInControl_;
  FadeControlWidgets itemFadeOutControl_;
  FadeControlWidgets masterFadeInControl_;
  FadeControlWidgets masterFadeOutControl_;
  QVariantAnimation *manualFadeAnimation_ = nullptr;
  QPushButton *activeFadeButton_ = nullptr;
  RangeMarkerSlider *seekBar_ = nullptr;
  QPushButton *offAirButton_ = nullptr;
  QPushButton *ftbButton_ = nullptr;
  QPushButton *dskOnButton_ = nullptr;
  QPushButton *dskPlayButton_ = nullptr;
  QPushButton *dskPauseButton_ = nullptr;
  QPushButton *pauseButton_ = nullptr;
  QPushButton *muteButton_ = nullptr;
  QPushButton *playButton_ = nullptr;
  DskCueState dskCueState_ = DskCueState::Empty;
  bool dskSelectionAvailable_ = false;
  bool dskPendingOn_ = false;
  bool dskOn_ = false;
  bool dskPlaying_ = false;
  bool dskTransportEnabled_ = true;
  QString dskFileName_;
  QString dskDetail_;
  QString fileName_;
  QString fileAbsolutePath_;
  QString programFaultDetail_;
  QString playPauseShortcut_;
  qint64 remainingMs_ = 0;
  std::optional<qint64> totalRemainingMs_ = std::nullopt;
  double playbackRate_ = 1.0;
  BgmPresentationState bgmPresentationState_ =
      BgmPresentationState::Stopped;
  BgmFadeState bgmFadeState_ = BgmFadeState::Idle;
  QString bgmPlaylistName_;
  QString bgmItemName_;
  QString bgmDetail_;
  bool bgmVolumeAtSilence_ = false;
  bool bgmFadeBlinkOn_ = false;
  bool bgmSignalActive_ = false;
  bool bgmSignalClipped_ = false;
  std::optional<PlaylistItemSettings> endModeSettings_ = std::nullopt;
  int currentPlayCount_ = 0;
  bool hasOnAirMedia_ = false;
  bool ftbControlsEnabled_ = true;
  bool bgmControlsEnabled_ = false;
  bool bgmFaderSelected_ = false;
  bool documentNavigationVisible_ = false;
  bool documentNavigationBusy_ = false;
  bool documentCanGoPrevious_ = false;
  bool documentCanGoNext_ = false;
  int documentCurrentPage_ = 0;
  int documentPageCount_ = 0;
  bool programFault_ = false;
  bool remainingLive_ = false;
  bool totalRemainingInfinite_ = false;
  bool inputSignalActive_ = false;
  bool inputSignalClipped_ = false;
  double itemNominalGain_ = 1.0;
  ManualFadeTarget manualFadeTarget_ = ManualFadeTarget::None;
  bool itemAudioAutomationOverridden_ = false;
  DocumentNavigationKind documentNavigationKind_ =
      DocumentNavigationKind::PowerPoint;
};
