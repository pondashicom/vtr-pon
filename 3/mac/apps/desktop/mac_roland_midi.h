#pragma once
#include <CoreMIDI/CoreMIDI.h>
#include <QByteArray>
#include <QList>
#include <QString>
#include <memory>

namespace vtrpon {
// Called only on the switcher worker. CoreMIDI callbacks retain value state,
// never a QObject. The injectable sender exercises driver completion failures.
class MacRolandMidi final {
public:
  using SysExSender = OSStatus (*)(MIDISysexSendRequest *);
  explicit MacRolandMidi(SysExSender sender = MIDISendSysex);
  ~MacRolandMidi();
  bool open(bool receive, QString *error, const QString &modelName = QStringLiteral("V-8HD"), int sendIntervalMs = 0);
  void close();
  bool isOpen() const;
  bool send(const QByteArray &message, QString *error);
  bool poll(QList<QByteArray> &messages, QString *error);
  struct Diagnostic { QString kind, detail; };
  QList<Diagnostic> takeDiagnostics();
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
