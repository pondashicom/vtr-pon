#pragma once

#include "midi_control_settings.h"
#include <QObject>
#include <functional>
#include <memory>

class ControllerOperations;
namespace vtrpon {
class MidiControlInput final : public QObject {
  Q_OBJECT
public:
  struct Device { QString id; QString name; unsigned int index; };
  explicit MidiControlInput(ControllerOperations *operations,
                           std::function<void(const QString &)> buttonAction,
                           QObject *parent = nullptr);
  void setVolumeValueProvider(std::function<int(const QString &)> provider);
  ~MidiControlInput() override;
  static QList<Device> devices();
  void reload();
  void setConfigurationMode(bool active);
  void monitorDevice(const QString &id);
  QString statusKey() const;
  QString errorDetail() const;
  void stop();
signals:
  void received(const QString &type, int channel, int number, int value, bool pressed);
  void connectionChanged();
private:
  friend class MidiControlTestAccess;
  void processMessage(const midiControl::Message &message);
  void enqueueMessage(quint32 packed);
  void pollMessages();
  class Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace vtrpon
