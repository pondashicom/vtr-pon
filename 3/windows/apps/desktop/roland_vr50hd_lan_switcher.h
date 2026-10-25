#pragma once

#include "switcher_control.h"

#include <QQueue>
#include <QTcpSocket>
#include <QTimer>

namespace vtrpon {

// VR-50HD MK II Reference Manual v2, pp. 60-62. One command in flight;
// setters wait for ACK, queries wait for their documented response.
class RolandVr50HdLanSwitcher final : public SwitcherDevice {
public:
  RolandVr50HdLanSwitcher(QString host, int port, bool receiveInputChanges,
                         QObject *parent = nullptr);
  ~RolandVr50HdLanSwitcher() override;
  bool open(QString *errorDetail = nullptr) override;
  bool opensAsynchronously() const override { return true; }
  void close() override;
  bool setProgramInput(int sourceNumber, QString *errorDetail = nullptr) override;
  bool setFadeToBlack(bool enabled, QString *errorDetail = nullptr) override;
  QString connectionDetail() const override;

private:
  enum class Reply { None, Version, Status, Ack };
  struct Command {
    QByteArray body;
    Reply reply = Reply::None;
    int programIndex = -1;
  };
  void connectSocket();
  void fail(const QString &detail);
  bool enqueue(Command command, QString *errorDetail = nullptr);
  void sendNext();
  void receive();
  void consumeByte(unsigned char byte);
  void handleFrame(const QByteArray &frame);
  void completeCommand();
  void poll();

  QString host_;
  quint16 port_;
  bool validPort_;
  bool receiveInputChanges_;
  QTcpSocket *socket_;
  QTimer *deadline_;
  QTimer *pollTimer_;
  QTimer *reconnectTimer_;
  QQueue<Command> commands_;
  Command pending_;
  QByteArray frame_;
  bool inFrame_ = false;
  // Minimal Telnet option negotiation. Application bytes are parsed separately.
  int telnetState_ = 0;
  unsigned char telnetVerb_ = 0;
  bool active_ = false;
  bool ready_ = false;
  int lastInput_ = -1;
};

} // namespace vtrpon
