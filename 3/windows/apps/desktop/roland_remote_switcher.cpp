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
      settings_(normalizeRolandSettings(model, std::move(settings))), receive_(receive), programQuery_(model), socket_(new QTcpSocket(this)) {
  socket_->setReadBufferSize(4096);
  deadline_.setSingleShot(true); deadline_.setInterval(4000);
  reconnect_.setSingleShot(true); reconnect_.setInterval(3000);
  pollTimer_.setInterval(receive_ && profile_.receive ? 250 : 2000);
  serialTimer_.setInterval(10);
  connect(socket_, &QTcpSocket::connected, this, [this] {
    connecting_ = false;
    if (profile_.passwordLength) { authenticating_ = true; deadline_.start(6000); }
    else enqueue({"VER;", Reply::Version});
  });
  connect(socket_, &QTcpSocket::readyRead, this, [this] { this->receive(socket_->readAll()); });
  connect(socket_, &QTcpSocket::errorOccurred, this, [this] { fail(socket_->errorString()); });
  connect(socket_, &QTcpSocket::disconnected, this, [this] { fail(QStringLiteral("Transport disconnected.")); });
  connect(&deadline_, &QTimer::timeout, this, &RolandRemoteSwitcher::responseTimeout);
  connect(&reconnect_, &QTimer::timeout, this, &RolandRemoteSwitcher::connectTransport);
  connect(&pollTimer_, &QTimer::timeout, this, &RolandRemoteSwitcher::poll);
  connect(&serialTimer_, &QTimer::timeout, this, &RolandRemoteSwitcher::readSerial);
}
RolandRemoteSwitcher::~RolandRemoteSwitcher() { close(); }
bool RolandRemoteSwitcher::open(QString *error) {
  close();
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
  connecting_ = baselineKnown_ = false; programQuery_.reset(); firmwareVersion_.clear();
  deadline_.stop(); pollTimer_.stop(); reconnect_.stop(); abortTransport();
  queue_.clear(); pending_ = {}; frame_.clear(); loginText_.clear(); lastInput_ = localTarget_ = -1; telnetState_ = 0;
}
void RolandRemoteSwitcher::connectTransport() {
  if (!active_) return;
  reconnect_.stop(); connecting_ = profile_.transport == RolandTransport::Lan; deadline_.start(4000);
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
void RolandRemoteSwitcher::fail(const QString &detail, bool retry) {
  if (!active_ || reconnect_.isActive()) return;
  if (retry) reconnect_.start();
  else { active_ = false; reconnect_.stop(); }
  ready_ = authenticating_ = passwordSent_ = flowPaused_ = false;
  connecting_ = baselineKnown_ = false; programQuery_.reset(); firmwareVersion_.clear();
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
  queue_.enqueue(std::move(command));
  QMetaObject::invokeMethod(this, &RolandRemoteSwitcher::sendNext, Qt::QueuedConnection);
  return true;
}
void RolandRemoteSwitcher::sendNext() {
  if (!active_ || authenticating_ || flowPaused_ || pending_.reply != Reply::None || queue_.isEmpty() || reconnect_.isActive()) return;
  pending_ = queue_.dequeue();
  QByteArray wire = pending_.body;
  if (profile_.transport == RolandTransport::Serial) wire.prepend(char(0x02));
  else wire += lanLineEnding(false);
  if (!writeBytes(wire)) { fail(QStringLiteral("Command send failed.")); return; }
  emit diagnosticEvent(QStringLiteral("TX"), QString::fromLatin1(pending_.body)); deadline_.start(4000);
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
  const auto minimum = rolandMinimumFadeVersion(model_);
  if (!minimum.isEmpty()) {
    const auto versionCode = [](const QString &s) {
      const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\.(\\d+)")).match(s);
      if (!match.hasMatch()) return -1;
      return match.captured(1).toInt()*100 + match.captured(2).leftJustified(2, '0').toInt();
    };
    if (versionCode(firmwareVersion_) < versionCode(minimum)) {
      if (error) *error = QStringLiteral("LAN FTB requires firmware %1 or later (reported: %2).").arg(minimum, firmwareVersion_);
      return false;
    }
  }
  if (rolandHasUsbMidiChoice(model_))
    return enqueue({enabled ? QByteArrayLiteral("FTB:ON;") : QByteArrayLiteral("FTB:OFF;"), Reply::Ack}, error);
  return enqueue({model_ == RolandSwitcherModel::V1HdPlus
      ? (enabled ? QByteArrayLiteral("OFD:-63;") : QByteArrayLiteral("OFD:0;"))
      : (enabled ? QByteArrayLiteral("FDE:1;") : QByteArrayLiteral("FDE:0;")), Reply::Ack}, error);
}
void RolandRemoteSwitcher::poll() {
  if (!ready_ || pending_.reply != Reply::None || !queue_.isEmpty()) return;
  if (!profile_.receive) enqueue({"VER;", Reply::Version});
  else { programQuery_.reset(); enqueue({statusQuery(), Reply::Program}); }
}
QByteArray RolandRemoteSwitcher::statusQuery() const {
  // The hardware-backed V-80HD client found that only single-byte reads work.
  return *profile_.programAddress ? "RQH:" + programQuery_.address().toHex().toUpper() + ",000001;" : QByteArrayLiteral("TLY;");
}
QByteArray RolandRemoteSwitcher::lanLineEnding(bool password) const {
  // Match the model-specific, hardware-backed clients; retain VR-120HD's existing framing.
  if (model_ == RolandSwitcherModel::V80Hd || model_ == RolandSwitcherModel::Vr120Hd ||
      (model_ == RolandSwitcherModel::V600Uhd && !password)) return "\r\n";
  return "\n";
}
void RolandRemoteSwitcher::responseTimeout() {
  if (connecting_) fail(QStringLiteral("TCP connection timed out."));
  else if (authenticating_ && passwordSent_)
    fail(QStringLiteral("No authentication result after sending the password. Check the password and reconnect manually."), false);
  else if (authenticating_)
    fail(QStringLiteral("No authentication prompt received. Another controller may hold the LAN session."));
  else if (model_ == RolandSwitcherModel::V80Hd && !flowPaused_ &&
           (isParameterRead() || pending_.reply == Reply::Version) &&
           pending_.response.isEmpty() && !pending_.ackReceived && pending_.readRetries++ == 0) {
    // Packet-capture-backed Companion reports dropped read requests. Retry only
    // a wholly unanswered read, preserving the same pending command/ACK pair.
    if (!writeBytes(pending_.body + lanLineEnding(false))) { fail(QStringLiteral("Read retry send failed.")); return; }
    emit diagnosticEvent(QStringLiteral("RETRY"), QString::fromLatin1(pending_.body)); deadline_.start(4000);
  }
  else fail(pending_.body.isEmpty() ? QStringLiteral("Transport response timed out.") :
      QStringLiteral("No response to %1").arg(QString::fromLatin1(pending_.body)));
}
void RolandRemoteSwitcher::authenticationAccepted() {
  if (!authenticating_ || !passwordSent_) return;
  authenticating_ = false; loginText_.clear(); deadline_.stop();
  emit diagnosticEvent(QStringLiteral("AUTH"), QStringLiteral("Authentication accepted."));
  enqueue({"VER;", Reply::Version});
}
void RolandRemoteSwitcher::consumeAuthentication(unsigned char byte) {
  // Only fixed status messages leave this parser. Password echoes/banners stay private.
  if (byte < 0x20 && byte != '\r' && byte != '\n') return;
  loginText_.append(char(byte));
  if (loginText_.size() > 1024) { fail(QStringLiteral("Authentication response exceeded the limit."), false); return; }
  const auto text = loginText_.toLower();
  if (text.contains("authentication error")) {
    fail(QStringLiteral("Network password was rejected. Check the password and reconnect manually."), false); return;
  }
  if (text.contains("wait a moment")) {
    fail(QStringLiteral("Device authentication lockout is active. Wait for it to clear, then reconnect manually."), false); return;
  }
  static const QRegularExpression prompt(QStringLiteral("(?:^|[\\r\\n])(?:enter[ \t]+)?password[ \t]*:"), QRegularExpression::CaseInsensitiveOption);
  if (prompt.match(QString::fromLatin1(loginText_)).hasMatch()) {
    if (passwordSent_) { fail(QStringLiteral("Network password was rejected. Check the password and reconnect manually."), false); return; }
    passwordSent_ = true; loginText_.clear();
    if (!writeBytes(settings_.password.toLatin1() + lanLineEnding(true))) { fail(QStringLiteral("Authentication send failed.")); return; }
    emit diagnosticEvent(QStringLiteral("AUTH"), QStringLiteral("Password sent (omitted)."));
    deadline_.start(6000); return;
  }
  if (!passwordSent_) return;
  const auto model = QByteArray(profile_.name).toLower();
  if (text.contains("welcome to " + model + '.') || text.contains("welcome to roland " + model + '.')) {
    authenticationAccepted(); return;
  }
  if (byte == '\n' || byte == '\r' || byte == ';') {
    auto line = loginText_.trimmed();
    if (line.endsWith(';')) line.chop(1);
    // Valid passwords such as VER:ABCD or "Welcome " can also be echoed.
    if (line == settings_.password.toLatin1()) { loginText_.clear(); return; }
    // A login version must identify this model, not merely contain the word VER.
    const auto fields = line.mid(4).split(',');
    if (line.startsWith("VER:") && fields.size() >= 2 && fields[0].trimmed() == profile_.name && !fields[1].trimmed().isEmpty()) {
      authenticationAccepted(); return;
    }
    loginText_.clear();
  }
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
  if (authenticating_) { consumeAuthentication(b); return; }
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
bool RolandRemoteSwitcher::isParameterRead() const {
  return *profile_.programAddress && pending_.reply == Reply::Program;
}
bool RolandRemoteSwitcher::needsQueryAck() const {
  // These models document a received-command ACK in addition to query data.
  // The other models' query tables document data only; do not require an extra ACK.
  return (model_ == RolandSwitcherModel::V80Hd || model_ == RolandSwitcherModel::V1_4K) &&
      (isParameterRead() || pending_.reply == Reply::Version);
}
void RolandRemoteSwitcher::acknowledge() {
  if (needsQueryAck()) {
    pending_.ackReceived = true;
    if (!pending_.response.isEmpty()) finishResponse();
    return;
  }
  if (pending_.reply != Reply::Ack) return;
  emit diagnosticEvent(QStringLiteral("RX"), QStringLiteral("ACK"));
  if (pending_.localInput >= 0) {
    lastInput_ = pending_.localInput;
    localTarget_ = pending_.localInput == settings_.sourceNumber - 1 ? pending_.localInput : -1;
    localTargetDeadline_.setRemainingTime(5000);
  }
  complete();
}
void RolandRemoteSwitcher::markReady() {
  if (!ready_) { ready_ = true; pollTimer_.start(); emit connectionOpened(true, connectionDetail()); }
}
void RolandRemoteSwitcher::handleParameterValue(int value) {
  const auto result = programQuery_.accept(value);
  complete();
  if (result == RolandProgramQuery::Result::Invalid) { fail(QStringLiteral("Invalid PGM source or INPUT assignment response.")); return; }
  if (result == RolandProgramQuery::Result::More) { enqueue({statusQuery(), Reply::Program}); return; }
  observe(programQuery_.input() == settings_.sourceNumber - 1 ? programQuery_.input() : -1);
}
void RolandRemoteSwitcher::finishResponse() {
  if (needsQueryAck() && !pending_.ackReceived) return;
  if (isParameterRead()) {
    bool valid = false;
    const int value = pending_.response.toInt(&valid, 16);
    if (!valid) { fail(QStringLiteral("Invalid parameter response.")); return; }
    handleParameterValue(value); return;
  }
  if (pending_.reply == Reply::Version) {
    emit diagnosticEvent(QStringLiteral("VERSION"), QString::fromLatin1(pending_.response.left(100)));
    complete();
    if (!profile_.receive) markReady();
    else { programQuery_.reset(); enqueue({statusQuery(), Reply::Program}); }
  }
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
  const bool hadBaseline = baselineKnown_; baselineKnown_ = true;
  const int previous = lastInput_; lastInput_ = input;
  if (pending_.reply == Reply::Program) complete();
  markReady();
  if (hadBaseline && receive_ && previous != input) emit programInputChanged(input + 1);
}
void RolandRemoteSwitcher::handleFrame(const QByteArray &frame) {
  if (frame == "ACK") { acknowledge(); return; }
  if (frame.startsWith("ERR:")) { fail(QStringLiteral("Device rejected command: %1").arg(QString::fromLatin1(frame.left(16)))); return; }
  if (pending_.reply == Reply::Version && frame.startsWith("VER:")) {
    const QByteArray name = frame.mid(4).split(',').value(0).trimmed();
    const QByteArray expected = model_ == RolandSwitcherModel::V1HdPlus ? QByteArrayLiteral("V-1HD PLUS") : QByteArray(profile_.name);
    if (name != expected) { fail(QStringLiteral("Connected device does not match the selected model.")); return; }
    firmwareVersion_ = QString::fromLatin1(frame.mid(4).split(',').value(1)).trimmed();
    pending_.response = frame;
    finishResponse(); return;
  }
  if (isParameterRead()) {
    const auto prefix = "DTH:" + pending_.body.mid(4, 6).toUpper() + ',';
    if (!frame.toUpper().startsWith(prefix)) return;
    const auto value = frame.mid(prefix.size());
    static const QRegularExpression hexByte(QStringLiteral("^[0-9a-fA-F]{2}$"));
    if (!hexByte.match(QString::fromLatin1(value)).hasMatch()) { fail(QStringLiteral("Invalid parameter response.")); return; }
    pending_.response = value;
    emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(frame));
    finishResponse(); return;
  }
  if (pending_.reply != Reply::Program) return;
  if (frame.startsWith("TLY:")) {
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
