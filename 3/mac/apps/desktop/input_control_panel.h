#pragma once

#include "uvc_device_monitor.h"
#include "uvc_audio_mapping.h"
#include "network_video.h"

#include <QList>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QHBoxLayout;
class QLabel;
class QPushButton;
class ControllerOperations;

class InputControlPanel final : public QWidget {
  Q_OBJECT

public:
  explicit InputControlPanel(QWidget *parent = nullptr,
                             ControllerOperations *operations = nullptr);

  void setUvcDevices(const QList<UvcDevice> &devices);
  void setAudioInputDevices(const QList<AudioInputDevice> &devices);
  void setNetworkVideoSources(const QList<vtrpon::NetworkVideoSource> &sources, bool enabled);
  void setDeckLinkSources(const QList<vtrpon::NetworkVideoSource> &sources, bool enabled);
  void setUvcCaptureBusy(bool busy);
  void setStillCaptureBusy(bool busy);

signals:
  void stillCaptureRequested(bool program, qint64 durationMs);
  void uvcAddRequested(const QString &displayName,
                       const QString &devicePath);

private:
  void changeEvent(QEvent *event) override;
  void retranslateUi();
  void refreshDevices();
  void updateCaptureButtonWidths();
  QList<UvcDevice> uvcDevices_;
  QList<AudioInputDevice> audioDevices_;
  QList<vtrpon::NetworkVideoSource> networkSources_;
  bool networkInputEnabled_ = false, deckLinkInputEnabled_ = false;
  QList<vtrpon::NetworkVideoSource> deckLinkSources_;

  QLabel *sectionTitle_ = nullptr;
  QLabel *moduleTitle_ = nullptr;
  QWidget *uvcModule_ = nullptr;
  QComboBox *uvcDeviceComboBox_ = nullptr;
  QPushButton *uvcAddButton_ = nullptr;
  QLabel *stillTitle_ = nullptr;
  QLabel *stillDurationLabel_ = nullptr;
  QPushButton *previewCaptureButton_ = nullptr;
  QPushButton *programCaptureButton_ = nullptr;
  QHBoxLayout *captureRow_ = nullptr;
  QDoubleSpinBox *stillDuration_ = nullptr;
  bool uvcCaptureBusy_ = false;
  bool stillCaptureBusy_ = false;
  ControllerOperations *operations_ = nullptr;
};
