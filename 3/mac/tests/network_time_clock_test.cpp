#include "network_time_clock.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>

#include <cstdio>
#include <functional>

namespace {

constexpr qint64 kNtpToUnixEpochSeconds = 2208988800LL;

bool check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "line %d: check failed: %s\n", line, expression);
  return false;
}

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!check((expression), #expression, __LINE__)) {                          \
      return 1;                                                                 \
    }                                                                           \
  } while (false)

void writeBigEndian32(QByteArray &data, int offset, quint32 value) {
  data[offset] = static_cast<char>((value >> 24) & 0xff);
  data[offset + 1] = static_cast<char>((value >> 16) & 0xff);
  data[offset + 2] = static_cast<char>((value >> 8) & 0xff);
  data[offset + 3] = static_cast<char>(value & 0xff);
}

void writeNtpTimestamp(QByteArray &data, int offset, qint64 unixMs) {
  const quint32 seconds = static_cast<quint32>(
      unixMs / 1000 + kNtpToUnixEpochSeconds);
  const quint32 fraction = static_cast<quint32>(
      (static_cast<quint64>(unixMs % 1000) << 32) / 1000);
  writeBigEndian32(data, offset, seconds);
  writeBigEndian32(data, offset + 4, fraction);
}

bool waitUntil(const std::function<bool()> &condition) {
  QEventLoop eventLoop;
  QTimer deadline;
  QTimer poll;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
  QObject::connect(&poll, &QTimer::timeout, &eventLoop, [&] {
    if (condition()) {
      eventLoop.quit();
    }
  });
  deadline.start(3000);
  poll.start(10);
  if (!condition()) {
    eventLoop.exec();
  }
  return condition();
}

bool hasOffset(const vtrpon::NetworkTimeClock &clock, qint64 expectedMs) {
  const qint64 offsetMs = clock.currentLocalDateTime().toMSecsSinceEpoch()
                         - QDateTime::currentMSecsSinceEpoch();
  return qAbs(offsetMs - expectedMs) < 500;
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);

  QUdpSocket server;
  CHECK(server.bind(QHostAddress(QHostAddress::LocalHost), 0));
  const quint16 serverPort = server.localPort();
  int requestCount = 0;
  int responseCount = 0;
  bool respondToRequests = true;
  qint64 serverOffsetMs = 2000;

  QObject::connect(&server, &QUdpSocket::readyRead, &server,
                   [&] {
    while (server.hasPendingDatagrams()) {
      QByteArray request;
      request.resize(static_cast<qsizetype>(server.pendingDatagramSize()));
      QHostAddress clientAddress;
      quint16 clientPort = 0;
      if (server.readDatagram(request.data(), request.size(), &clientAddress,
                              &clientPort) < 0
          || request.size() < 48) {
        continue;
      }
      ++requestCount;
      if (!respondToRequests) {
        continue;
      }

      QByteArray response(48, '\0');
      response[0] = static_cast<char>(0x24); // NTP v4, server mode.
      response[1] = static_cast<char>(1);
      response.replace(24, 8, request.mid(40, 8));
      const qint64 serverUnixMs =
          QDateTime::currentMSecsSinceEpoch() + serverOffsetMs;
      writeNtpTimestamp(response, 32, serverUnixMs);
      writeNtpTimestamp(response, 40, serverUnixMs);
      if (server.writeDatagram(response, clientAddress, clientPort)
          == response.size()) {
        ++responseCount;
      }
    }
  });

  QUdpSocket silentServer;
  CHECK(silentServer.bind(QHostAddress(QHostAddress::LocalHost), 0));
  vtrpon::NetworkTimeClock fallbackClock(
      QStringLiteral("127.0.0.1"), silentServer.localPort());
  QList<bool> fallbackResults;
  QObject::connect(
      &fallbackClock, &vtrpon::NetworkTimeClock::synchronizationFinished,
      &application, [&](bool succeeded) { fallbackResults.append(succeeded); });
  CHECK(!fallbackClock.isNetworkSynchronized());
  CHECK(qAbs(fallbackClock.currentLocalDateTime().toMSecsSinceEpoch()
             - QDateTime::currentMSecsSinceEpoch()) < 250);

  vtrpon::NetworkTimeClock networkClock(
      QStringLiteral("127.0.0.1"), serverPort);
  int startedCount = 0;
  int synchronizationStateChanges = 0;
  QList<bool> results;
  QObject::connect(
      &networkClock, &vtrpon::NetworkTimeClock::synchronizationStarted,
      &application, [&] { ++startedCount; });
  QObject::connect(
      &networkClock, &vtrpon::NetworkTimeClock::synchronizationFinished,
      &application, [&](bool succeeded) { results.append(succeeded); });
  QObject::connect(
      &networkClock, &vtrpon::NetworkTimeClock::networkSynchronizationChanged,
      &application, [&](bool) { ++synchronizationStateChanges; });

  // Startup synchronization and fallback require no manual request.
  CHECK(waitUntil([&] { return results.size() == 1; }));
  CHECK(results.constLast());
  CHECK(startedCount == 1);
  CHECK(!networkClock.isSynchronizationInProgress());
  CHECK(networkClock.isNetworkSynchronized());
  CHECK(hasOffset(networkClock, 2000));
  networkClock.setTimeZone(QTimeZone("Asia/Tokyo"));
  CHECK(networkClock.currentLocalDateTime().offsetFromUtc() == 9 * 3600);
  CHECK(hasOffset(networkClock, 2000));
  networkClock.setTimeZone(QTimeZone("UTC"));
  CHECK(networkClock.currentLocalDateTime().offsetFromUtc() == 0);
  CHECK(hasOffset(networkClock, 2000));
  fallbackClock.setTimeZone(QTimeZone("Asia/Tokyo"));
  CHECK(fallbackClock.currentLocalDateTime().offsetFromUtc() == 9 * 3600);
  CHECK(hasOffset(fallbackClock, 0));
  networkClock.setTimeZone(QTimeZone());
  CHECK(networkClock.currentLocalDateTime().timeZone() == QTimeZone::systemTimeZone());
  CHECK(waitUntil([&] { return fallbackResults.size() == 1; }));
  CHECK(!fallbackResults.constLast());
  CHECK(!fallbackClock.isSynchronizationInProgress());
  CHECK(!fallbackClock.isNetworkSynchronized());
  CHECK(hasOffset(fallbackClock, 0));

  // A manual request corrects an already synchronized clock and completes
  // even though the persistent synchronization state does not change.
  serverOffsetMs = 5000;
  networkClock.synchronize();
  CHECK(networkClock.isSynchronizationInProgress());
  networkClock.synchronize(); // Duplicate requests must not create more work.
  CHECK(startedCount == 2);
  CHECK(waitUntil([&] { return results.size() == 2; }));
  CHECK(results.constLast());
  CHECK(requestCount == 2);
  CHECK(responseCount == 2);
  CHECK(synchronizationStateChanges == 1);
  CHECK(hasOffset(networkClock, 5000));

  // Timeout leaves the last corrected clock advancing and the event loop
  // responsive. A subsequent request remains possible.
  respondToRequests = false;
  int heartbeatCount = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, &application,
                   [&] { ++heartbeatCount; });
  heartbeat.start(20);
  const QDateTime beforeFailure = networkClock.currentLocalDateTime();
  networkClock.synchronize();
  CHECK(hasOffset(networkClock, 5000));
  CHECK(waitUntil([&] { return results.size() == 3; }));
  heartbeat.stop();
  CHECK(!results.constLast());
  CHECK(heartbeatCount > 5);
  CHECK(!networkClock.isSynchronizationInProgress());
  CHECK(networkClock.isNetworkSynchronized());
  CHECK(networkClock.currentLocalDateTime() > beforeFailure);
  CHECK(hasOffset(networkClock, 5000));

  respondToRequests = true;
  serverOffsetMs = -3000;
  networkClock.synchronize();
  CHECK(waitUntil([&] { return results.size() == 4; }));
  CHECK(results.constLast());
  CHECK(!networkClock.isSynchronizationInProgress());
  CHECK(hasOffset(networkClock, -3000));
  CHECK(startedCount == 4);
  CHECK(requestCount == 4);
  CHECK(responseCount == 3);

  // Manual failure before any successful sync still uses the PC clock.
  fallbackClock.synchronize();
  fallbackClock.synchronize();
  CHECK(waitUntil([&] { return fallbackResults.size() == 2; }));
  CHECK(!fallbackResults.constLast());
  CHECK(!fallbackClock.isSynchronizationInProgress());
  CHECK(!fallbackClock.isNetworkSynchronized());
  CHECK(hasOffset(fallbackClock, 0));
  return 0;
}
