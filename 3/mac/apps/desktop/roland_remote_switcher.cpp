#include "roland_remote_switcher.h"
#include <QHostAddress>
#include <QRegularExpression>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace vtrpon {
RolandRemoteSwitcher::RolandRemoteSwitcher(RolandSwitcherModel model, RolandModelSettings settings,
                                         bool receive, QObject *parent)
    : SwitcherDevice(parent), model_(model), profile_(rolandSwitcherProfile(model)),
      settings_(normalizeRolandSettings(model, std::move(settings))), receive_(receive), socket_(new QTcpSocket(this)) {
  socket_->setReadBufferSize(4096);
  deadline_.setSingleShot(true); deadline_.setInterval(4000);
  reconnect_.setSingleShot(true); reconnect_.setInterval(3000);
  pollTimer_.setInterval(receive_ && profile_.receive ? 250 : 2000);
  serialTimer_.setInterval(10);
  connect(socket_, &QTcpSocket::connected, this, [this] {
    if (profile_.passwordLength) { authenticating_ = true; deadline_.start(); }
    else enqueue({"VER;", Reply::Version});
  });
  connect(socket_, &QTcpSocket::readyRead, this, [this] { this->receive(socket_->readAll()); });
  connect(socket_, &QTcpSocket::errorOccurred, this, [this] { fail(socket_->errorString()); });
  connect(socket_, &QTcpSocket::disconnected, this, [this] { fail(QStringLiteral("Transport disconnected.")); });
  connect(&deadline_, &QTimer::timeout, this, [this] {
    fail(authenticating_ ? QStringLiteral("Network authentication timed out.")
                        : QStringLiteral("No response to %1").arg(QString::fromLatin1(pending_.body)));
  });
  connect(&reconnect_, &QTimer::timeout, this, &RolandRemoteSwitcher::connectTransport);
  connect(&pollTimer_, &QTimer::timeout, this, &RolandRemoteSwitcher::poll);
  connect(&serialTimer_, &QTimer::timeout, this, &RolandRemoteSwitcher::readSerial);
}
RolandRemoteSwitcher::~RolandRemoteSwitcher() { close(); }
bool RolandRemoteSwitcher::open(QString *error) {
  close();
  if (!rolandSwitcherSupported(model_)) {
    if (error) *error = QStringLiteral("Selected switcher is unavailable on this platform.");
    return false;
  }
  const QHostAddress host(settings_.host);
  QString reason;
  if (profile_.transport == RolandTransport::Lan &&
      (host.protocol() != QAbstractSocket::IPv4Protocol || host.isNull() || host.isMulticast() ||
       host == QHostAddress(QHostAddress::AnyIPv4) || host == QHostAddress(QHostAddress::Broadcast)))
    reason = QStringLiteral("Enter a valid IPv4 address.");
  else if (profile_.passwordLength &&
           (settings_.password.size() != profile_.passwordLength ||
            !QRegularExpression(QStringLiteral("^[ -~]+$")).match(settings_.password).hasMatch()))
    reason = QStringLiteral("Enter the %1-character network password configured on the switcher.").arg(profile_.passwordLength);
  else if (profile_.transport == RolandTransport::Serial &&
           !QRegularExpression(QStringLiteral("^COM[1-9][0-9]{0,3}$")).match(settings_.serialPort).hasMatch())
    reason = QStringLiteral("Enter the RS-232 adapter's COM port (for example COM3).");
  if (!reason.isEmpty()) { if (error) *error = reason; return false; }
  active_ = true; connectTransport(); return true;
}
void RolandRemoteSwitcher::abortTransport() {
  socket_->abort(); serialTimer_.stop();
#ifdef _WIN32
  if (serialHandle_) { CloseHandle(static_cast<HANDLE>(serialHandle_)); serialHandle_ = nullptr; }
#endif
}
void RolandRemoteSwitcher::close() {
  active_ = ready_ = authenticating_ = passwordSent_ = flowPaused_ = false;
  deadline_.stop(); pollTimer_.stop(); reconnect_.stop(); abortTransport();
  queue_.clear(); pending_ = {}; frame_.clear(); loginText_.clear(); lastInput_ = localTarget_ = -1; telnetState_ = 0;
}
void RolandRemoteSwitcher::connectTransport() {
  if (!active_) return;
  reconnect_.stop(); deadline_.start();
  if (profile_.transport == RolandTransport::Lan) {
    socket_->connectToHost(QHostAddress(settings_.host), quint16(settings_.port)); return;
  }
#ifdef _WIN32
  const auto path = QStringLiteral("\\\\.\\%1").arg(settings_.serialPort);
  auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) { fail(QStringLiteral("Could not open RS-232 port (%1).").arg(GetLastError())); return; }
  serialHandle_ = handle;
  DCB dcb{}; dcb.DCBlength = sizeof(dcb);
  COMMTIMEOUTS timeouts{}; timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.WriteTotalTimeoutConstant = 100;
  if (!GetCommState(handle, &dcb)) { fail(QStringLiteral("Could not read RS-232 configuration.")); return; }
  dcb.BaudRate = DWORD(settings_.baudRate); dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fRtsControl = RTS_CONTROL_DISABLE;
  dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fDsrSensitivity = FALSE; dcb.fAbortOnError = FALSE;
  if (!SetCommState(handle, &dcb) || !SetCommTimeouts(handle, &timeouts) ||
      !PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR)) { fail(QStringLiteral("Could not configure RS-232 (8N1).")); return; }
  serialTimer_.start(); enqueue({"VER;", Reply::Version});
#else
  fail(QStringLiteral("RS-232 control is supported on Windows."));
#endif
}
bool RolandRemoteSwitcher::writeBytes(const QByteArray &bytes) {
  if (writeForTest_) return writeForTest_(bytes);
  if (profile_.transport == RolandTransport::Lan) return socket_->write(bytes) == bytes.size();
#ifdef _WIN32
  DWORD sent = 0;
  return serialHandle_ && WriteFile(static_cast<HANDLE>(serialHandle_), bytes.constData(), DWORD(bytes.size()), &sent, nullptr) && sent == DWORD(bytes.size());
#else
  return false;
#endif
}
void RolandRemoteSwitcher::readSerial() {
#ifdef _WIN32
  char buffer[1024]; DWORD count = 0;
  if (!serialHandle_) return;
  if (!ReadFile(static_cast<HANDLE>(serialHandle_), buffer, sizeof(buffer), &count, nullptr)) {
    fail(QStringLiteral("RS-232 read failed (%1).").arg(GetLastError())); return;
  }
  if (count) receive(QByteArray(buffer, int(count)));
#endif
}
void RolandRemoteSwitcher::fail(const QString &detail) {
  if (!active_ || reconnect_.isActive()) return;
  reconnect_.start(); ready_ = authenticating_ = passwordSent_ = flowPaused_ = false;
  deadline_.stop(); pollTimer_.stop(); queue_.clear(); pending_ = {};
  frame_.clear(); loginText_.clear(); lastInput_ = localTarget_ = -1; telnetState_ = 0; abortTransport();
  const QString message = QString::fromLatin1(profile_.name) + QStringLiteral(": ") + detail;
  emit connectionOpened(false, message); emit errorOccurred(message);
}
QString RolandRemoteSwitcher::connectionDetail() const {
  return QString::fromLatin1(profile_.name) + QStringLiteral(" / ") + rolandTransportName(profile_.transport);
}
bool RolandRemoteSwitcher::enqueue(Command command, QString *error) {
  if (queue_.size() >= 32) { if (error) *error = QStringLiteral("Command queue full."); fail(QStringLiteral("Command queue full.")); return false; }
  queue_.enqueue(std::move(command)); sendNext(); return true;
}
void RolandRemoteSwitcher::sendNext() {
  if (!active_ || authenticating_ || flowPaused_ || pending_.reply != Reply::None || queue_.isEmpty() || reconnect_.isActive()) return;
  pending_ = queue_.dequeue();
  QByteArray wire = pending_.body;
  if (profile_.transport == RolandTransport::Serial) wire.prepend(char(0x02));
  if (!writeBytes(wire)) { fail(QStringLiteral("Command send failed.")); return; }
  emit diagnosticEvent(QStringLiteral("TX"), QString::fromLatin1(pending_.body)); deadline_.start();
}
void RolandRemoteSwitcher::complete() {
  deadline_.stop(); pending_ = {};
  QMetaObject::invokeMethod(this, &RolandRemoteSwitcher::sendNext, Qt::QueuedConnection);
}
bool RolandRemoteSwitcher::setProgramInput(int source, QString *error) {
  if (!ready_ || source < 1 || source > profile_.inputs) { if (error) *error = QStringLiteral("Switcher is not ready or input is out of range."); return false; }
  const QByteArray body = *profile_.programAddress
      ? "DTH:" + QByteArray(profile_.programAddress).toUpper() + ',' + QByteArray::number(source-1, 16).rightJustified(2, '0').toUpper() + ';'
      : "PGM:" + QByteArray::number(source-1) + ';';
  return enqueue({body, Reply::Ack, source-1}, error);
}
bool RolandRemoteSwitcher::setFadeToBlack(bool enabled, QString *error) {
  if (!ready_ || !profile_.fade) { if (error) *error = QStringLiteral("FTB is unavailable for this model or connection."); return false; }
  // The basic command guide defines explicit states; never send the FTB toggle.
  if (model_ == RolandSwitcherModel::Vr120Hd)
    return enqueue({enabled ? QByteArrayLiteral("FTB:ON;") : QByteArrayLiteral("FTB:OFF;"), Reply::Ack}, error);
  return enqueue({model_ == RolandSwitcherModel::V1HdPlus
      ? (enabled ? QByteArrayLiteral("OFD:-63;") : QByteArrayLiteral("OFD:0;"))
      : (enabled ? QByteArrayLiteral("FDE:1;") : QByteArrayLiteral("FDE:0;")), Reply::Ack}, error);
}
void RolandRemoteSwitcher::poll() {
  if (!ready_ || pending_.reply != Reply::None || !queue_.isEmpty()) return;
  if (!profile_.receive) enqueue({"VER;", Reply::Version});
  else enqueue({statusQuery(), Reply::Program});
}
QByteArray RolandRemoteSwitcher::statusQuery() const {
  // Include STILL/player/virtual tallies to distinguish a source change from
  // fading all sources out. The selected physical input remains the test target.
  return *profile_.programAddress ? "RQH:0C0000," + QByteArray::number(tallySize(), 16).rightJustified(6, '0').toUpper() + ';'
                                 : QByteArrayLiteral("TLY;");
}
int RolandRemoteSwitcher::tallySize() const {
  // V-1-4K's BLACK selection has no tally entry; its table ends at INPUT 8.
  return profile_.maximumProgramValue + (model_ == RolandSwitcherModel::V1_4K ? 0 : 1);
}
void RolandRemoteSwitcher::receive(const QByteArray &bytes) {
  for (unsigned char b : bytes) {
    if (!active_ || reconnect_.isActive()) break;
    if (telnetState_ == 1) {
      if (b == 0xff) { telnetState_ = 0; consume(b); }
      else if (b >= 0xfb && b <= 0xfe) { telnetVerb_ = b; telnetState_ = 2; }
      else if (b == 0xfa) telnetState_ = 3;
      else telnetState_ = 0;
    } else if (telnetState_ == 2) {
      if (telnetVerb_ == 0xfb || telnetVerb_ == 0xfd) {
        QByteArray refusal; refusal.append(char(0xff)); refusal.append(char(telnetVerb_ == 0xfb ? 0xfe : 0xfc)); refusal.append(char(b)); writeBytes(refusal);
      }
      telnetState_ = 0;
    } else if (telnetState_ == 3) { if (b == 0xff) telnetState_ = 4; }
    else if (telnetState_ == 4) telnetState_ = b == 0xf0 ? 0 : 3;
    else if (b == 0xff && profile_.transport == RolandTransport::Lan) telnetState_ = 1;
    else consume(b);
  }
}
void RolandRemoteSwitcher::consume(unsigned char b) {
  if (authenticating_) {
    // Login banners and password echoes are never included in diagnostics.
    loginText_.append(char(b)); if (loginText_.size() > 1024) loginText_.remove(0, 512);
    if (loginText_.toLower().contains("password")) {
      if (passwordSent_) { fail(QStringLiteral("Network password was rejected.")); return; }
      passwordSent_ = true; loginText_.clear();
      if (!writeBytes(settings_.password.toLatin1() + "\r\n")) { fail(QStringLiteral("Authentication send failed.")); return; }
      emit diagnosticEvent(QStringLiteral("AUTH"), QStringLiteral("Password sent (omitted)."));
      QTimer::singleShot(150, this, [this] {
        if (!active_ || !authenticating_ || reconnect_.isActive()) return;
        authenticating_ = false; loginText_.clear(); frame_.clear(); enqueue({"VER;", Reply::Version});
      });
    }
    return;
  }
  if (b == 0x13) { flowPaused_ = true; deadline_.start(); return; }
  if (b == 0x11) { flowPaused_ = false; if (pending_.reply == Reply::None) deadline_.stop(); sendNext(); return; }
  if (b == 0x06 && frame_.isEmpty()) { acknowledge(); return; }
  if (b == 0x02) { frame_.clear(); return; }
  if (b == ';' || b == '\n' || b == '\r') {
    if (!frame_.isEmpty()) handleFrame(std::exchange(frame_, {}).trimmed()); return;
  }
  if (b < 0x20) return;
  if (frame_.size() >= 1024) { fail(QStringLiteral("Response frame exceeded the limit.")); return; }
  frame_.append(char(b));
}
void RolandRemoteSwitcher::acknowledge() {
  if (pending_.reply != Reply::Ack) return;
  emit diagnosticEvent(QStringLiteral("RX"), QStringLiteral("ACK"));
  if (pending_.localInput >= 0) {
    lastInput_ = pending_.localInput;
    localTarget_ = pending_.localInput == settings_.sourceNumber - 1 ? pending_.localInput : -1;
    localTargetDeadline_.setRemainingTime(5000);
  }
  complete();
}
void RolandRemoteSwitcher::observe(int input) {
  if (input < -1 || input > profile_.maximumProgramValue) { fail(QStringLiteral("Invalid input response.")); return; }
  if (localTarget_ >= 0) {
    if (input == localTarget_) localTarget_ = -1;
    else if (localTargetDeadline_.hasExpired()) {
      localTarget_ = -1;
      emit diagnosticEvent(QStringLiteral("OBSERVATION"), QStringLiteral("Requested input was not observed before another source became active."));
    }
    if (localTarget_ >= 0 || input == lastInput_) {
      if (pending_.reply == Reply::Program) complete();
      return;
    }
  }
  const int previous = lastInput_; lastInput_ = input;
  if (pending_.reply == Reply::Program) complete();
  if (!ready_) { ready_ = true; pollTimer_.start(); emit connectionOpened(true, connectionDetail()); }
  else if (receive_ && previous >= -1 && previous != input) emit programInputChanged(input + 1);
}
void RolandRemoteSwitcher::handleFrame(const QByteArray &frame) {
  if (frame == "ACK") { acknowledge(); return; }
  if (frame.startsWith("ERR:")) { fail(QStringLiteral("Device rejected command: %1").arg(QString::fromLatin1(frame.left(16)))); return; }
  if (pending_.reply == Reply::Version && frame.startsWith("VER:")) {
    const QByteArray name = frame.mid(4).split(',').value(0).trimmed();
    const QByteArray expected = model_ == RolandSwitcherModel::V1HdPlus ? QByteArrayLiteral("V-1HD PLUS") : QByteArray(profile_.name);
    if (name != expected) { fail(QStringLiteral("Connected device does not match the selected model.")); return; }
    emit diagnosticEvent(QStringLiteral("VERSION"), QString::fromLatin1(frame.left(100)));
    complete();
    if (!profile_.receive) { ready_ = true; pollTimer_.start(); emit connectionOpened(true, connectionDetail()); }
    else enqueue({statusQuery(), Reply::Program});
    return;
  }
  if (pending_.reply != Reply::Program) return;
  if (*profile_.programAddress) {
    const QByteArray prefix = QByteArrayLiteral("DTH:0C0000,");
    if (!frame.toUpper().startsWith(prefix)) return;
    const QByteArray value = frame.mid(prefix.size());
    if (value.size() != tallySize() * 2 || !QRegularExpression(QStringLiteral("^[0-2]+$")).match(QString::fromLatin1(value)).hasMatch()) {
      fail(QStringLiteral("Invalid parameter response.")); return;
    }
    const auto tallies = QByteArray::fromHex(value);
    for (unsigned char tally : tallies) if (tally > 2) { fail(QStringLiteral("Invalid tally value.")); return; }
    emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(frame));
    if (ready_ && !tallies.contains(char(1))) { complete(); return; }
    observe(tallies[settings_.sourceNumber - 1] == char(1) ? settings_.sourceNumber - 1 : -1);
  } else if (frame.startsWith("TLY:")) {
    const auto values = frame.mid(4).split(','); int selected = -1;
    if (values.size() != 4) { fail(QStringLiteral("Invalid tally response.")); return; }
    for (int i = 0; i < 4; ++i) {
      if (values[i].size() != 1 || values[i][0] < '0' || values[i][0] > '2') { fail(QStringLiteral("Invalid tally value.")); return; }
      if (values[i] == "1" && i == settings_.sourceNumber - 1) selected = i;
    }
    emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(frame));
    if (ready_ && !values.contains(QByteArrayLiteral("1"))) { complete(); return; }
    observe(selected);
  }
}
} // namespace vtrpon
