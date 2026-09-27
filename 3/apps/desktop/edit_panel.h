#pragma once

#include "playlist_item.h"
#include "language_text.h"

#include <QList>
#include <QImage>
#include <QSet>
#include <QWidget>
#include "controller_operations.h"

#include <optional>

class AudioMeterWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPaintEvent;
class QSpinBox;
class AudioFaderWidget;
namespace vtrpon { class EqualizerEditor; }
class QPushButton;
class QToolButton;
class RangeMarkerSlider;
class EditLivePreviewWidget;
class UserInfoCenter;
class QEvent;

class EditPanel final : public QWidget {
  Q_OBJECT

public:
  explicit EditPanel(UserInfoCenter *userInfoCenter,
                     QWidget *parent = nullptr,
                     ControllerOperations *operations = nullptr);

  QWidget *videoSurface() const;
  void setFile(const QString &fileName, const QString &absolutePath);
  void clearFile();
  void setLiveFrame(const QImage &frame);
  void clearLiveFrame();
  void setStatus(const QString &text, bool error = false);
  void setStatus(const vtrpon::LocalizedText &text, bool error = false);
  void setStatusToolTip(const QString &text);
  void setTransportEnabled(bool enabled);
  void setPlaybackState(bool playbackActive);
  void setEditorState(const std::optional<PlaylistItemSettings> &settings,
                      bool editPointsEnabled, bool otherSettingsEnabled,
                      const QList<PlaylistItemState> &gotoTargets = {},
                      const QSet<quint64> &offlineGotoTargets = {},
                      bool liveSource = false);
  void setGotoTargetOffline(quint64 itemId, bool offline);
  void resetForLoadedMedia();
  void setTimelineDuration(qint64 durationMs);
  void setTimelinePosition(qint64 positionMs);
  bool isTimelineSliderDown() const;
  void setAudioLevel(double leftPeakDb, double rightPeakDb, bool clipped);
  void setInputSignalAppearance(bool active, bool clipped);
  void resetAudioPresentation();
  void setPflState(bool enabled, bool canEnable, const QString &status);

signals:
  void playRequested();
  void pflRequested(bool enabled);
  void pauseRequested();
  void stopRequested();
  void jumpToStartRequested();
  void jumpToInRequested();
  void jumpToOutRequested();
  void jumpToEndRequested();
  void scanBackwardRequested();
  void scanForwardRequested();
  void scanStopRequested();
  void setInRequested();
  void setOutRequested();
  void inPointTimeRequested(qint64 positionMs);
  void outPointTimeRequested(qint64 positionMs);
  void startModeRequested(PlaylistStartMode mode);
  void endModeRequested(PlaylistEndMode mode);
  void gotoTargetRequested(quint64 itemId);
  void repeatOptionsRequested(int count, PlaylistRepeatEndMode endMode);
  void fadeInRequested(bool enabled, qint64 durationMs);
  void fadeOutRequested(bool enabled, qint64 durationMs);
  void endCrossfadeRequested(bool enabled, qint64 durationMs);
  void nominalVolumeRequested(int volumeTenthsDb);
  void nominalDelayRequested(int frames);
  void nominalEqRequested(int lowTenthsDb, int midTenthsDb, int highTenthsDb);
  void seekRequested(qint64 positionMs);
  void frameStepRequested(int frameCount);

protected:
  void changeEvent(QEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
    ControllerOperations *operations_ = nullptr;
  void retranslateUi();
  void commitEditPointTime(QLineEdit *editor, bool inPoint);
  void updateEditPointTimeDisplays();
  void updateEditPointButtonAppearance();
  void updateFilePresentation();
  void updateGotoTargetItems(quint64 selectedTargetItemId);
  void updateGotoTargetAvailability(int index, const PlaylistItemState &target);
  void updateRepeatEndModeItems(PlaylistRepeatEndMode selectedMode);
  void openNominalEq();
  bool nominalSettingsEnabled_ = false;
  bool pflRequestedEnabled_ = false;
  bool pflAvailable_ = false;

  QWidget *videoMonitor_ = nullptr;
  QWidget *videoSurface_ = nullptr;
  EditLivePreviewWidget *livePreviewWidget_ = nullptr;
  AudioMeterWidget *meter_ = nullptr;
  QLabel *inputSignalIndicator_ = nullptr;
  QLabel *fileLabel_ = nullptr;
  AudioFaderWidget *nominalVolumeFader_ = nullptr;
  QSpinBox *nominalDelaySpin_ = nullptr;
  QPushButton *nominalEqButton_ = nullptr;
  vtrpon::EqualizerEditor *nominalEqEditor_ = nullptr;
  std::array<int, 3> nominalEqTenthsDb_{};
  QPushButton *pflButton_ = nullptr;
  UserInfoCenter *userInfoCenter_ = nullptr;
  QString pendingStatusDetail_;
  QString fileName_;
  QString fileAbsolutePath_;
  RangeMarkerSlider *seekBar_ = nullptr;
  QPushButton *jumpToStartButton_ = nullptr;
  QPushButton *jumpToInButton_ = nullptr;
  QPushButton *rewindButton_ = nullptr;
  QPushButton *playButton_ = nullptr;
  QPushButton *pauseButton_ = nullptr;
  QPushButton *forwardButton_ = nullptr;
  QPushButton *jumpToOutButton_ = nullptr;
  QPushButton *jumpToEndButton_ = nullptr;
  QPushButton *inButton_ = nullptr;
  QPushButton *outButton_ = nullptr;
  QLineEdit *inPointTimeEdit_ = nullptr;
  QLineEdit *outPointTimeEdit_ = nullptr;
  QPushButton *startPauseButton_ = nullptr;
  QPushButton *startPlayButton_ = nullptr;
  QPushButton *endStopButton_ = nullptr;
  QPushButton *endPauseButton_ = nullptr;
  QPushButton *endRepeatButton_ = nullptr;
  QPushButton *endNextButton_ = nullptr;
  QPushButton *endGotoButton_ = nullptr;
  QCheckBox *fadeInCheckBox_ = nullptr;
  QWidget *fadeInDurationControl_ = nullptr;
  QDoubleSpinBox *fadeInDurationSpinBox_ = nullptr;
  QToolButton *fadeInDurationIncreaseButton_ = nullptr;
  QToolButton *fadeInDurationDecreaseButton_ = nullptr;
  QCheckBox *fadeOutCheckBox_ = nullptr;
  QWidget *fadeOutDurationControl_ = nullptr;
  QDoubleSpinBox *fadeOutDurationSpinBox_ = nullptr;
  QToolButton *fadeOutDurationIncreaseButton_ = nullptr;
  QToolButton *fadeOutDurationDecreaseButton_ = nullptr;
  QCheckBox *endCrossfadeCheckBox_ = nullptr;
  QWidget *endCrossfadeDurationControl_ = nullptr;
  QDoubleSpinBox *endCrossfadeDurationSpinBox_ = nullptr;
  QToolButton *endCrossfadeDurationIncreaseButton_ = nullptr;
  QToolButton *endCrossfadeDurationDecreaseButton_ = nullptr;
  QLabel *gotoTargetLabel_ = nullptr;
  QComboBox *gotoTargetComboBox_ = nullptr;
  QLabel *repeatOptionLabel_ = nullptr;
  QLabel *repeatCountLabel_ = nullptr;
  QSpinBox *repeatCountSpinBox_ = nullptr;
  QWidget *repeatCountControl_ = nullptr;
  QToolButton *repeatCountIncreaseButton_ = nullptr;
  QToolButton *repeatCountDecreaseButton_ = nullptr;
  QLabel *repeatEndModeLabel_ = nullptr;
  QComboBox *repeatEndModeComboBox_ = nullptr;
  QList<PlaylistItemState> gotoTargets_;
  QSet<quint64> offlineGotoTargets_;
  std::optional<PlaylistItemSettings> displayedSettings_ = std::nullopt;
  qint64 timelineDurationMs_ = -1;
  double audioLeftPeakDb_ = -60.0;
  double audioRightPeakDb_ = -60.0;
  bool audioClipped_ = false;
  bool inputSignalActive_ = false;
  bool inputSignalClipped_ = false;
  int wheelAngleRemainder_ = 0;
};
