#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

template<class Device> class AsyncDeviceMonitor;

struct UvcDevice {
  QString displayName;
  QString devicePath;

  bool operator==(const UvcDevice &) const = default;
};

class UvcDeviceMonitor final : public QObject {
  Q_OBJECT

public:
  explicit UvcDeviceMonitor(QObject *parent = nullptr);
  ~UvcDeviceMonitor() override;

  UvcDeviceMonitor(const UvcDeviceMonitor &) = delete;
  UvcDeviceMonitor &operator=(const UvcDeviceMonitor &) = delete;

  QList<UvcDevice> devices() const;
  bool isAvailable() const;
  QString errorDetail() const;
  void requestShutdown();
  bool shutdownComplete() const;

signals:
  void devicesChanged();

private:
  void refreshDevices();

  std::shared_ptr<AsyncDeviceMonitor<UvcDevice>> worker_;
  bool shutdownRequested_ = false;
  QTimer refreshTimer_;
  QList<UvcDevice> devices_;
  QString errorDetail_;
  bool started_ = false;
};
