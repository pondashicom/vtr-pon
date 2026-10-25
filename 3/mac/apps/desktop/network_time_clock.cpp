#include "network_time_clock.h"

#include <QHostInfo>
#include <QTimer>
#include <QUdpSocket>

#include <cmath>

namespace {

constexpr auto kPublicNtpServer = "time.cloudflare.com";
constexpr quint16 kNtpPort = 123;
constexpr int kNtpPacketSize = 48;
constexpr int kNtpResponseTimeoutMs = 1600;
constexpr int kNtpAttemptTimeoutMs = 8000;
constexpr int kNtpResynchronizeIntervalMs = 30 * 60 * 1000;
constexpr qint64 kNtpToUnixEpochSeconds = 2208988800LL;
constexpr double kNtpFractionScale = 4294967296.0;

quint32 readBigEndian32(const QByteArray &data, int offset) {
  return (static_cast<quint32>(static_cast<quint8>(data.at(offset))) << 24)
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1)))
            << 16)
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2)))
            << 8)
         | static_cast<quint32>(static_cast<quint8>(data.at(offset + 3)));
}

void writeBigEndian32(QByteArray &data, int offset, quint32 value) {
  data[offset] = static_cast<char>((value >> 24) & 0xff);
  data[offset + 1] = static_cast<char>((value >> 16) & 0xff);
  data[offset + 2] = static_cast<char>((value >> 8) & 0xff);
  data[offset + 3] = static_cast<char>(value & 0xff);
}

void writeNtpTimestamp(QByteArray &data, int offset, qint64 unixMs) {
  const qint64 unixSeconds = unixMs / 1000;
  const qint64 millisecondPart = unixMs % 1000;
  const quint32 ntpSeconds = static_cast<quint32>(
      unixSeconds + kNtpToUnixEpochSeconds);
  const quint32 ntpFraction = static_cast<quint32>(
      (static_cast<quint64>(millisecondPart) << 32) / 1000);
  writeBigEndian32(data, offset, ntpSeconds);
  writeBigEndian32(data, offset + 4, ntpFraction);
}

double readNtpTimestampMs(const QByteArray &data, int offset) {
  const quint32 ntpSeconds = readBigEndian32(data, offset);
  const quint32 ntpFraction = readBigEndian32(data, offset + 4);
  return (static_cast<double>(ntpSeconds) - kNtpToUnixEpochSeconds) * 1000.0
         + (static_cast<double>(ntpFraction) * 1000.0
            / kNtpFractionScale);
}

} // namespace

namespace vtrpon {

NetworkTimeClock::NetworkTimeClock(QObject *parent)
    : NetworkTimeClock(QString::fromLatin1(kPublicNtpServer), kNtpPort,
                       parent) {}

NetworkTimeClock::NetworkTimeClock(const QString &serverName,
                                   quint16 serverPort, QObject *parent)
    : QObject(parent), serverName_(serverName), serverPort_(serverPort),
      socket_(new QUdpSocket(this)),
      resynchronizeTimer_(new QTimer(this)),
      responseTimer_(new QTimer(this)), attemptTimer_(new QTimer(this)) {
  resynchronizeTimer_->setInterval(kNtpResynchronizeIntervalMs);
  responseTimer_->setSingleShot(true);
  attemptTimer_->setSingleShot(true);

  connect(resynchronizeTimer_, &QTimer::timeout, this,
          &NetworkTimeClock::synchronize);
  connect(responseTimer_, &QTimer::timeout, this,
          &NetworkTimeClock::handleResponseTimeout);
  connect(attemptTimer_, &QTimer::timeout, this,
          &NetworkTimeClock::finishAttemptWithoutNetworkTime);
  connect(socket_, &QUdpSocket::readyRead, this,
          &NetworkTimeClock::readPendingDatagrams);

  resynchronizeTimer_->start();
  QTimer::singleShot(0, this, &NetworkTimeClock::synchronize);
}

NetworkTimeClock::~NetworkTimeClock() {
  if (hostLookupId_ >= 0) {
    QHostInfo::abortHostLookup(hostLookupId_);
  }
}

QDateTime NetworkTimeClock::currentLocalDateTime() const {
  if (!networkSynchronized_ || !elapsedSinceReference_.isValid()) {
    return QDateTime::currentDateTimeUtc().toTimeZone(timeZone_);
  }
  return networkUtcAtReference_
      .addMSecs(elapsedSinceReference_.elapsed())
      .toTimeZone(timeZone_);
}

void NetworkTimeClock::setTimeZone(const QTimeZone &zone) {
  timeZone_ = zone.isValid() ? zone : QTimeZone::systemTimeZone();
}

bool NetworkTimeClock::isNetworkSynchronized() const {
  return networkSynchronized_;
}

bool NetworkTimeClock::isSynchronizationInProgress() const {
  return synchronizationInProgress_;
}

void NetworkTimeClock::synchronize() {
  if (synchronizationInProgress_) {
    return;
  }

  synchronizationInProgress_ = true;
  serverAddresses_.clear();
  currentAddressIndex_ = -1;
  currentAddress_.clear();
  requestTransmitTimestamp_.clear();
  socket_->close();
  attemptTimer_->start(kNtpAttemptTimeoutMs);
  emit synchronizationStarted();

  QHostAddress directAddress;
  if (directAddress.setAddress(serverName_)) {
    serverAddresses_.append(directAddress);
    requestNextAddress();
    return;
  }

  hostLookupId_ = QHostInfo::lookupHost(
      serverName_, this,
      [this](const QHostInfo &hostInfo) { handleHostLookup(hostInfo); });
}

void NetworkTimeClock::handleHostLookup(const QHostInfo &hostInfo) {
  hostLookupId_ = -1;
  if (!synchronizationInProgress_
      || hostInfo.error() != QHostInfo::NoError) {
    finishAttemptWithoutNetworkTime();
    return;
  }

  const QList<QHostAddress> resolved = hostInfo.addresses();
  for (QAbstractSocket::NetworkLayerProtocol protocol :
       {QAbstractSocket::IPv4Protocol, QAbstractSocket::IPv6Protocol}) {
    for (const QHostAddress &address : resolved) {
      if (address.protocol() == protocol
          && !serverAddresses_.contains(address)) {
        serverAddresses_.append(address);
      }
    }
  }

  if (serverAddresses_.isEmpty()) {
    finishAttemptWithoutNetworkTime();
    return;
  }
  requestNextAddress();
}

void NetworkTimeClock::requestNextAddress() {
  responseTimer_->stop();
  ++currentAddressIndex_;
  if (!synchronizationInProgress_
      || currentAddressIndex_ >= serverAddresses_.size()) {
    finishAttemptWithoutNetworkTime();
    return;
  }

  currentAddress_ = serverAddresses_.at(currentAddressIndex_);
  QByteArray request(kNtpPacketSize, '\0');
  request[0] = static_cast<char>(0x23); // NTP v4, client mode.
  requestSentUnixMs_ = QDateTime::currentMSecsSinceEpoch();
  writeNtpTimestamp(request, 40, requestSentUnixMs_);
  requestTransmitTimestamp_ = request.mid(40, 8);

  if (socket_->writeDatagram(request, currentAddress_, serverPort_)
      != request.size()) {
    requestNextAddress();
    return;
  }
  responseTimer_->start(kNtpResponseTimeoutMs);
}

void NetworkTimeClock::readPendingDatagrams() {
  while (socket_->hasPendingDatagrams()) {
    QByteArray response;
    response.resize(static_cast<qsizetype>(socket_->pendingDatagramSize()));
    QHostAddress senderAddress;
    quint16 senderPort = 0;
    if (socket_->readDatagram(response.data(), response.size(),
                              &senderAddress, &senderPort) < 0) {
      continue;
    }
    if (!synchronizationInProgress_
        || !senderAddress.isEqual(currentAddress_)
        || senderPort != serverPort_ || response.size() < kNtpPacketSize) {
      continue;
    }

    const quint8 header = static_cast<quint8>(response.at(0));
    const int leapIndicator = (header >> 6) & 0x03;
    const int version = (header >> 3) & 0x07;
    const int mode = header & 0x07;
    const int stratum = static_cast<quint8>(response.at(1));
    if (leapIndicator == 3 || (version != 3 && version != 4) || mode != 4
        || stratum < 1 || stratum > 15
        || response.mid(24, 8) != requestTransmitTimestamp_) {
      continue;
    }

    const qint64 receivedUnixMs = QDateTime::currentMSecsSinceEpoch();
    const double serverReceiveMs = readNtpTimestampMs(response, 32);
    const double serverTransmitMs = readNtpTimestampMs(response, 40);
    if (!std::isfinite(serverReceiveMs) || !std::isfinite(serverTransmitMs)
        || serverReceiveMs <= 0.0 || serverTransmitMs <= 0.0
        || serverTransmitMs < serverReceiveMs) {
      continue;
    }

    const double networkRoundTripMs =
        static_cast<double>(receivedUnixMs - requestSentUnixMs_)
        - (serverTransmitMs - serverReceiveMs);
    if (networkRoundTripMs < -10.0 || networkRoundTripMs > 5000.0) {
      continue;
    }

    const double offsetMs =
        ((serverReceiveMs - static_cast<double>(requestSentUnixMs_))
         + (serverTransmitMs - static_cast<double>(receivedUnixMs)))
        / 2.0;
    const qint64 correctedReceiptMs =
        std::llround(static_cast<double>(receivedUnixMs) + offsetMs);
    finishSynchronization(
        QDateTime::fromMSecsSinceEpoch(correctedReceiptMs, Qt::UTC));
    return;
  }
}

void NetworkTimeClock::handleResponseTimeout() {
  if (synchronizationInProgress_) {
    requestNextAddress();
  }
}

void NetworkTimeClock::finishSynchronization(
    const QDateTime &networkUtcAtReceipt) {
  responseTimer_->stop();
  attemptTimer_->stop();
  socket_->close();
  synchronizationInProgress_ = false;
  networkUtcAtReference_ = networkUtcAtReceipt;
  elapsedSinceReference_.restart();
  const bool synchronizationStateChanged = !networkSynchronized_;
  networkSynchronized_ = true;
  if (synchronizationStateChanged) {
    emit networkSynchronizationChanged(true);
  }
  emit synchronizationFinished(true);
}

void NetworkTimeClock::finishAttemptWithoutNetworkTime() {
  if (!synchronizationInProgress_) {
    return;
  }
  if (hostLookupId_ >= 0) {
    QHostInfo::abortHostLookup(hostLookupId_);
    hostLookupId_ = -1;
  }
  responseTimer_->stop();
  attemptTimer_->stop();
  socket_->close();
  synchronizationInProgress_ = false;
  emit synchronizationFinished(false);
}

} // namespace vtrpon
