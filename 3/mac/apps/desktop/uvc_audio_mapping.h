#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

template<class Device> class AsyncDeviceMonitor;

struct AudioInputDevice {
  QString displayName;
  QString deviceId;
  bool isDefault = false;

  bool operator==(const AudioInputDevice &) const = default;
};

class AudioInputDeviceMonitor final : public QObject {
  Q_OBJECT

public:
  explicit AudioInputDeviceMonitor(QObject *parent = nullptr);
  ~AudioInputDeviceMonitor() override;

  AudioInputDeviceMonitor(const AudioInputDeviceMonitor &) = delete;
  AudioInputDeviceMonitor &operator=(const AudioInputDeviceMonitor &) = delete;

  QList<AudioInputDevice> devices() const;
  bool isAvailable() const;
  QString errorDetail() const;
  void requestShutdown();
  bool shutdownComplete() const;

signals:
  void devicesChanged();

private:
  void refreshDevices();

  std::shared_ptr<AsyncDeviceMonitor<AudioInputDevice>> worker_;
  bool shutdownRequested_ = false;
  QTimer refreshTimer_;
  QList<AudioInputDevice> devices_;
  QString errorDetail_;
  bool started_ = false;
};

class UvcAudioMappingStore final : public QObject {
  Q_OBJECT

public:
  explicit UvcAudioMappingStore(QObject *parent = nullptr);

  QString audioDeviceIdFor(const QString &uvcDevicePath) const;
  bool setAudioDeviceIdFor(const QString &uvcDevicePath,
                           const QString &audioDeviceId,
                           QString *errorDetail = nullptr);

signals:
  void mappingChanged(const QString &uvcDevicePath,
                      const QString &audioDeviceId);

private:
  static QString settingKey(const QString &uvcDevicePath);
};
