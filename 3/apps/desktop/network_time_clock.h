#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimeZone>

class QTimer;
class QUdpSocket;
class QHostInfo;

namespace vtrpon {

class NetworkTimeClock final : public QObject {
  Q_OBJECT

public:
  explicit NetworkTimeClock(QObject *parent = nullptr);
  NetworkTimeClock(const QString &serverName, quint16 serverPort,
                   QObject *parent = nullptr);
  ~NetworkTimeClock() override;

  QDateTime currentLocalDateTime() const;
  bool isNetworkSynchronized() const;
  bool isSynchronizationInProgress() const;
  void synchronize();
  void setTimeZone(const QTimeZone &zone);

signals:
  void networkSynchronizationChanged(bool synchronized);
  void synchronizationStarted();
  void synchronizationFinished(bool succeeded);

private:
  void handleHostLookup(const QHostInfo &hostInfo);
  void requestNextAddress();
  void readPendingDatagrams();
  void handleResponseTimeout();
  void finishSynchronization(const QDateTime &networkUtcAtReceipt);
  void finishAttemptWithoutNetworkTime();

  QTimeZone timeZone_ = QTimeZone::systemTimeZone();
  QString serverName_;
  quint16 serverPort_ = 123;
  QUdpSocket *socket_ = nullptr;
  QTimer *resynchronizeTimer_ = nullptr;
  QTimer *responseTimer_ = nullptr;
  QTimer *attemptTimer_ = nullptr;
  QList<QHostAddress> serverAddresses_;
  qsizetype currentAddressIndex_ = -1;
  QHostAddress currentAddress_;
  QByteArray requestTransmitTimestamp_;
  qint64 requestSentUnixMs_ = 0;
  int hostLookupId_ = -1;
  bool synchronizationInProgress_ = false;
  bool networkSynchronized_ = false;
  QDateTime networkUtcAtReference_;
  QElapsedTimer elapsedSinceReference_;
};

} // namespace vtrpon
