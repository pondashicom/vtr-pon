#include "roland_vr50hd_lan_switcher.h"

#include <QHostAddress>
#include <utility>

namespace vtrpon {

RolandVr50HdLanSwitcher::RolandVr50HdLanSwitcher(
    QString host, int port, bool receiveInputChanges, QObject *parent)
    : SwitcherDevice(parent), host_(host.trimmed()),
      port_(static_cast<quint16>(port)), validPort_(port > 0 && port <= 65535),
      receiveInputChanges_(receiveInputChanges), socket_(new QTcpSocket(this)),
      deadline_(new QTimer(this)), pollTimer_(new QTimer(this)),
      reconnectTimer_(new QTimer(this)) {
  socket_->setReadBufferSize(4096);
  deadline_->setSingleShot(true);
  deadline_->setInterval(3000);
  reconnectTimer_->setSingleShot(true);
  reconnectTimer_->setInterval(3000);
  pollTimer_->setInterval(receiveInputChanges_ ? 250 : 2000);
  connect(socket_, &QTcpSocket::connected, this, [this] {
    deadline_->stop();
    enqueue({QByteArrayLiteral("VER;"), Reply::Version});
  });
  connect(socket_, &QTcpSocket::readyRead, this, &RolandVr50HdLanSwitcher::receive);
  connect(socket_, &QTcpSocket::errorOccurred, this, [this] {
    fail(QStringLiteral("VR-50HD MK II LAN: %1").arg(socket_->errorString()));
  });
  connect(socket_, &QTcpSocket::disconnected, this, [this] {
    fail(QStringLiteral("VR-50HD MK II LAN disconnected."));
  });
  connect(deadline_, &QTimer::timeout, this, [this] {
    fail(QStringLiteral("VR-50HD MK II LAN response timed out (%1).")
             .arg(QString::fromLatin1(pending_.body)));
  });
  connect(reconnectTimer_, &QTimer::timeout, this,
          &RolandVr50HdLanSwitcher::connectSocket);
  connect(pollTimer_, &QTimer::timeout, this, &RolandVr50HdLanSwitcher::poll);
}

RolandVr50HdLanSwitcher::~RolandVr50HdLanSwitcher() { close(); }

bool RolandVr50HdLanSwitcher::open(QString *errorDetail) {
  close();
  const QHostAddress address(host_);
  if (!validPort_ || address.protocol() != QAbstractSocket::IPv4Protocol ||
      address.isNull() || address.isMulticast() ||
      address == QHostAddress(QHostAddress::AnyIPv4) ||
      address == QHostAddress(QHostAddress::Broadcast)) {
    if (errorDetail)
      *errorDetail = QStringLiteral("Enter the VR-50HD MK II IPv4 address and TCP port (default 8023).");
    return false;
  }
  active_ = true;
  connectSocket();
  return true;
}

void RolandVr50HdLanSwitcher::close() {
  active_ = false;
  ready_ = false;
  deadline_->stop();
  pollTimer_->stop();
  reconnectTimer_->stop();
  socket_->abort();
  commands_.clear();
  pending_ = {};
  frame_.clear();
  inFrame_ = false;
  telnetState_ = 0;
  lastInput_ = -1;
}

void RolandVr50HdLanSwitcher::connectSocket() {
  if (!active_) return;
  reconnectTimer_->stop();
  deadline_->start();
  socket_->connectToHost(QHostAddress(host_), port_);
}

void RolandVr50HdLanSwitcher::fail(const QString &detail) {
  if (!active_ || reconnectTimer_->isActive()) return;
  // Start before abort(), which can synchronously emit disconnected().
  reconnectTimer_->start();
  ready_ = false;
  deadline_->stop();
  pollTimer_->stop();
  commands_.clear(); // Never replay old ON AIR/FTB operations after reconnect.
  pending_ = {};
  frame_.clear();
  inFrame_ = false;
  telnetState_ = 0;
  lastInput_ = -1;
  socket_->abort();
  emit connectionOpened(false, detail);
  emit errorOccurred(detail);
}

QString RolandVr50HdLanSwitcher::connectionDetail() const {
  return QStringLiteral("VR-50HD MK II LAN %1:%2").arg(host_).arg(port_);
}

bool RolandVr50HdLanSwitcher::setProgramInput(int sourceNumber, QString *errorDetail) {
  if (sourceNumber < 1 || sourceNumber > 4 || !ready_) {
    if (errorDetail)
      *errorDetail = QStringLiteral("VR-50HD MK II requires a ready LAN connection and INPUT 1-4.");
    return false;
  }
  return enqueue({QByteArrayLiteral("PGM:") + QByteArray::number(sourceNumber - 1) + ';',
                  Reply::Ack, sourceNumber - 1}, errorDetail);
}

bool RolandVr50HdLanSwitcher::setFadeToBlack(bool enabled, QString *errorDetail) {
  if (!ready_) {
    if (errorDetail) *errorDetail = QStringLiteral("VR-50HD MK II LAN is not ready.");
    return false;
  }
  // The device's Output Fade Color must be Black (no documented color command).
  return enqueue({enabled ? QByteArrayLiteral("FDE:1;") : QByteArrayLiteral("FDE:0;"),
                  Reply::Ack}, errorDetail);
}

bool RolandVr50HdLanSwitcher::enqueue(Command command, QString *errorDetail) {
  if (commands_.size() >= 32) {
    const QString detail = QStringLiteral("VR-50HD MK II LAN command queue is full.");
    if (errorDetail) *errorDetail = detail;
    fail(detail);
    return false;
  }
  commands_.enqueue(std::move(command));
  sendNext();
  return true;
}

void RolandVr50HdLanSwitcher::sendNext() {
  if (!active_ || pending_.reply != Reply::None || commands_.isEmpty() ||
      socket_->state() != QAbstractSocket::ConnectedState) return;
  pending_ = commands_.dequeue();
  const QByteArray bytes = QByteArray(1, '\x02') + pending_.body;
  if (socket_->write(bytes) != bytes.size()) {
    fail(QStringLiteral("Could not send VR-50HD MK II LAN command."));
    return;
  }
  emit diagnosticEvent(QStringLiteral("TX"), QString::fromLatin1(pending_.body));
  deadline_->start();
}

void RolandVr50HdLanSwitcher::completeCommand() {
  deadline_->stop();
  pending_ = {};
  // Defer until all bytes in this read are parsed; a duplicate ACK in the same
  // packet must not acknowledge a command that has not been sent yet.
  QMetaObject::invokeMethod(this, &RolandVr50HdLanSwitcher::sendNext, Qt::QueuedConnection);
}

void RolandVr50HdLanSwitcher::poll() {
  if (!ready_ || pending_.reply != Reply::None || !commands_.isEmpty()) return;
  enqueue({QByteArrayLiteral("QPG;"), Reply::Status});
}

void RolandVr50HdLanSwitcher::receive() {
  const QByteArray bytes = socket_->readAll();
  for (unsigned char byte : bytes) {
    if (!active_ || reconnectTimer_->isActive()) break;
    // Refuse optional Telnet features; retain framing across split TCP reads.
    if (telnetState_ == 1) {
      if (byte == 0xff) { telnetState_ = 0; consumeByte(byte); }
      else if (byte >= 0xfb && byte <= 0xfe) { telnetVerb_ = byte; telnetState_ = 2; }
      else if (byte == 0xfa) telnetState_ = 3;
      else telnetState_ = 0;
    } else if (telnetState_ == 2) {
      if (telnetVerb_ == 0xfb || telnetVerb_ == 0xfd) {
        QByteArray refusal;
        refusal.append(char(0xff));
        refusal.append(char(telnetVerb_ == 0xfb ? 0xfe : 0xfc));
        refusal.append(char(byte));
        socket_->write(refusal);
      }
      telnetState_ = 0;
    } else if (telnetState_ == 3) {
      if (byte == 0xff) telnetState_ = 4;
    } else if (telnetState_ == 4) {
      telnetState_ = byte == 0xf0 ? 0 : 3;
    } else if (byte == 0xff) telnetState_ = 1;
    else consumeByte(byte);
  }
}

void RolandVr50HdLanSwitcher::consumeByte(unsigned char byte) {
  if (byte == 0x02) {
    frame_.clear();
    inFrame_ = true;
  } else if (byte == 0x06 && !inFrame_) {
    if (pending_.reply == Reply::Ack) {
      emit diagnosticEvent(QStringLiteral("RX"), QStringLiteral("ACK"));
      // A local input command must not be echoed back as another ON AIR take.
      if (pending_.programIndex >= 0) lastInput_ = pending_.programIndex;
      completeCommand();
    }
  } else if (inFrame_) {
    if (byte == ';') {
      inFrame_ = false;
      const QByteArray frame = std::exchange(frame_, {});
      handleFrame(frame);
    } else if (frame_.size() >= 1024) {
      fail(QStringLiteral("VR-50HD MK II LAN response exceeded the frame limit."));
    } else frame_.append(char(byte));
  }
}

void RolandVr50HdLanSwitcher::handleFrame(const QByteArray &frame) {
  if (frame.startsWith("ERR:")) {
    fail(QStringLiteral("VR-50HD MK II rejected %1: ERR:%2")
             .arg(QString::fromLatin1(pending_.body),
                  QString::fromLatin1(frame.mid(4))));
    return;
  }
  if (pending_.reply == Reply::Version && frame.startsWith("VER:")) {
    if (frame.mid(4).split(',').value(0).trimmed() != "VR-50HDMK2") {
      fail(QStringLiteral("The LAN device did not identify itself as VR-50HDMK2."));
      return;
    }
    emit diagnosticEvent(QStringLiteral("VERSION"), QString::fromLatin1(frame.left(100)));
    completeCommand();
    enqueue({QByteArrayLiteral("QPG;"), Reply::Status});
    return;
  }
  if (!frame.startsWith("QPG:")) return;
  const QByteArray value = frame.mid(4).trimmed();
  if (value.size() != 1 || value[0] < '0' || value[0] > '4') {
    fail(QStringLiteral("Invalid VR-50HD MK II input status."));
    return;
  }
  if (pending_.reply != Reply::Status && !ready_) return;
  emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(frame));
  const int input = value[0] - '0';
  const int previous = lastInput_;
  lastInput_ = input; // STILL is a valid observed state, never a selectable PC input.
  if (pending_.reply == Reply::Status) completeCommand();
  if (!ready_) {
    ready_ = true;
    pollTimer_->start();
    emit connectionOpened(true, connectionDetail());
  } else if (receiveInputChanges_ && previous >= 0 && previous != input &&
             !(pending_.reply == Reply::Ack && pending_.programIndex == input)) {
    emit programInputChanged(input + 1);
  }
}

} // namespace vtrpon
