#pragma once
#include "switcher_control.h"
#include "roland_switcher_profiles.h"
#include <QQueue>
#include <QDeadlineTimer>
#include <QTcpSocket>
#include <QTimer>

namespace vtrpon {
// LAN DTH/RQH, V-600UHD ASCII, and V-1HD+ RS-232. Runs on the switcher worker.
class RolandRemoteSwitcher final : public SwitcherDevice {
public:
  RolandRemoteSwitcher(RolandSwitcherModel model, RolandModelSettings settings,
                       bool receive, QObject *parent = nullptr);
  ~RolandRemoteSwitcher() override;
  bool open(QString *error = nullptr) override;
  bool opensAsynchronously() const override { return true; }
  void close() override;
  bool setProgramInput(int source, QString *error = nullptr) override;
  bool setFadeToBlack(bool enabled, QString *error = nullptr) override;
  QString connectionDetail() const override;
private:
  friend class RolandRemoteProtocolTest;
  std::function<bool(const QByteArray &)> writeForTest_;
  enum class Reply { None, Version, Program, Ack };
  struct Command { QByteArray body; Reply reply = Reply::None; int localInput = -1; };
  void connectTransport();
  bool writeBytes(const QByteArray &bytes);
  void readSerial();
  void receive(const QByteArray &bytes);
  void consume(unsigned char byte);
  void handleFrame(const QByteArray &frame);
  bool enqueue(Command command, QString *error = nullptr);
  void sendNext();
  void complete();
  void acknowledge();
  void observe(int input);
  void poll();
  QByteArray statusQuery() const;
  int tallySize() const;
  void fail(const QString &detail);
  void abortTransport();
  RolandSwitcherModel model_;
  const RolandSwitcherProfile &profile_;
  RolandModelSettings settings_;
  bool receive_, active_ = false, ready_ = false, authenticating_ = false;
  bool passwordSent_ = false, flowPaused_ = false;
  int lastInput_ = -1, telnetState_ = 0;
  int localTarget_ = -1;
  QDeadlineTimer localTargetDeadline_;
  unsigned char telnetVerb_ = 0;
  QByteArray frame_, loginText_;
  Command pending_;
  QQueue<Command> queue_;
  QTcpSocket *socket_;
  QTimer deadline_, pollTimer_, reconnect_, serialTimer_;
  void *serialHandle_ = nullptr;
};
} // namespace vtrpon
