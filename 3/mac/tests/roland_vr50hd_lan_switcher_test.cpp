#include "roland_vr50hd_lan_switcher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTcpServer>
#include <QThread>
#include <functional>
#include <iostream>

bool runRolandVr50HdLanRegression() {
  bool ok = true;
  const auto check = [&](bool result, const char *message) {
    if (!result) std::cerr << "FAIL: VR-50HD LAN: " << message << '\n';
    ok &= result;
    return result;
  };
  const auto waitFor = [](const std::function<bool()> &condition, int limit = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < limit) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    return condition();
  };
  const auto settle = [&](int ms) {
    QElapsedTimer timer;
    timer.start();
    waitFor([&] { return timer.elapsed() >= ms; }, ms + 100);
  };
  const auto frame = [](const QByteArray &body) { return QByteArray(1, '\x02') + body + ';'; };

  QTcpServer server;
  if (!check(server.listen(QHostAddress::LocalHost), "loopback server listens")) return false;
  QPointer<QTcpSocket> peer;
  QByteArray incoming;
  QList<QByteArray> received;
  int input = 0;
  int connections = 0;
  bool holdAck = false, ignoreStatus = false, wrongModel = false, rejectFade = false;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
    peer = server.nextPendingConnection();
    ++connections;
    incoming.clear();
    auto *client = peer.data();
    QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
    QObject::connect(client, &QTcpSocket::readyRead, &server, [&, client] {
      incoming += client->readAll();
      while (incoming.contains(';')) {
        const qsizetype end = incoming.indexOf(';');
        QByteArray command = incoming.left(end + 1);
        incoming.remove(0, end + 1);
        const qsizetype start = command.indexOf('\x02');
        if (start < 0) continue;
        command = command.mid(start + 1);
        received.append(command);
        if (command == "VER;") {
          // Deliberately split a response and a Telnet negotiation over TCP reads.
          client->write(QByteArray::fromHex("fffb01") + frame(wrongModel ? "VER:OTHER,1.0" : "VER:VR-50HDMK2,2.03").left(7));
          const QByteArray rest = frame(wrongModel ? "VER:OTHER,1.0" : "VER:VR-50HDMK2,2.03").mid(7);
          QTimer::singleShot(15, client, [client, rest] { client->write(rest); });
        } else if (command == "QPG;") {
          if (!ignoreStatus) client->write(frame("QPG:" + QByteArray::number(input)));
        } else if (command.startsWith("PGM:")) {
          input = command.mid(4, 1).toInt();
          if (!holdAck) client->write(QByteArray(1, '\x06'));
        } else if (command.startsWith("FDE:")) {
          if (rejectFade) client->write(frame("ERR:4"));
          else if (!holdAck) client->write(QByteArray(1, '\x06'));
        }
      }
    });
  });
  vtrpon::RolandVr50HdLanSwitcher device("127.0.0.1", server.serverPort(), true);
  bool ready = false;
  QString detail;
  QList<int> notifications;
  int failures = 0;
  QObject::connect(&device, &vtrpon::SwitcherDevice::connectionOpened, &server,
                   [&](bool value, const QString &message) { ready = value; detail = message; });
  QObject::connect(&device, &vtrpon::SwitcherDevice::errorOccurred, &server,
                   [&](const QString &) { ++failures; });
  QObject::connect(&device, &vtrpon::SwitcherDevice::programInputChanged, &server,
                   [&](int source) { notifications.append(source); });
  check(device.open(), "open starts without blocking");
  if (!check(waitFor([&] { return ready; }), "model and baseline responses establish readiness")) return false;
  check(notifications.isEmpty(), "connection baseline never starts ON AIR");
  check(received.mid(0, 2) == QList<QByteArray>{"VER;", "QPG;"}, "documented query framing");
  input = 3;
  check(waitFor([&] { return notifications == QList<int>{4}; }), "input 4 detected by polling");
  settle(550);
  check(notifications == QList<int>{4}, "unchanged input does not retrigger");
  check(!device.setProgramInput(5), "STILL is not a selectable PC input");

  holdAck = true;
  check(device.setProgramInput(2), "input command queues");
  check(device.setFadeToBlack(true), "FTB ON queues behind input");
  check(device.setFadeToBlack(false), "FTB OFF queues behind ON");
  check(waitFor([&] { return received.contains("PGM:1;"); }), "input is zero-based");
  settle(100);
  check(!received.contains("FDE:1;"), "no setter is sent before ACK");
  peer->write(QByteArray(1, '\x06'));
  check(waitFor([&] { return received.contains("FDE:1;"); }), "ACK releases FTB ON");
  check(!received.contains("FDE:0;"), "FTB OFF still waits for ACK");
  peer->write(QByteArray(1, '\x06'));
  check(waitFor([&] { return received.contains("FDE:0;"); }), "ACK releases FTB OFF");
  peer->write(QByteArray(1, '\x06'));
  holdAck = false;
  settle(600);
  check(notifications == QList<int>{4}, "local PGM echo cannot retake ON AIR");
  input = 4;
  check(waitFor([&] { return notifications.size() == 2; }), "STILL is observed");
  input = 1;
  check(waitFor([&] { return notifications.size() == 3; }), "return from STILL detected");
  check(notifications.last() == 2, "returned input is one-based");

  rejectFade = true;
  device.setFadeToBlack(true);
  device.setProgramInput(4); // Must be discarded when the first command fails.
  check(waitFor([&] { return failures > 0 && !ready; }), "ERR clears readiness");
  check(detail.contains("ERR:4"), "device rejection is visible");
  const int notificationCount = notifications.size();
  const int commandCount = received.count("PGM:3;");
  rejectFade = false;
  check(waitFor([&] { return connections >= 2 && ready; }, 5000), "connection automatically recovers");
  check(notifications.size() == notificationCount, "reconnect baseline never takes");
  check(received.count("PGM:3;") == commandCount, "failed-session commands never replay");

  const int oldFailures = failures;
  ignoreStatus = true;
  check(waitFor([&] { return failures > oldFailures && !ready; }, 4000), "missing response times out");
  check(detail.contains("timed out"), "timeout diagnostic");
  device.close();
  const int oldConnections = connections;
  settle(3200);
  check(connections == oldConnections, "close cancels reconnect");

  ignoreStatus = false;
  wrongModel = true;
  const int failuresBeforeModel = failures;
  check(device.open(), "wrong-model test connects");
  check(waitFor([&] { return failures > failuresBeforeModel; }), "wrong model rejected");
  check(!ready && detail.contains("identify"), "TCP alone is not ready");
  device.close();
  wrongModel = false;
  check(device.open(), "malformed-response test connects");
  check(waitFor([&] { return ready; }), "reopened");
  peer->write(frame("QPG:99"));
  check(waitFor([&] { return !ready; }), "out-of-range response rejected");
  device.close();

  vtrpon::RolandVr50HdLanSwitcher invalid("", 8023, false);
  check(!invalid.open(&detail), "blank host rejected without network access");
  vtrpon::RolandVr50HdLanSwitcher sendOnly("127.0.0.1", server.serverPort(), false);
  bool sendOnlyReady = false;
  int sendOnlyNotifications = 0;
  QObject::connect(&sendOnly, &vtrpon::SwitcherDevice::connectionOpened, &server,
                   [&](bool value, const QString &) { sendOnlyReady = value; });
  QObject::connect(&sendOnly, &vtrpon::SwitcherDevice::programInputChanged, &server,
                   [&](int) { ++sendOnlyNotifications; });
  check(sendOnly.open() && waitFor([&] { return sendOnlyReady; }), "send-only mode connects");
  peer->write(frame("QPG:3") + frame("QPG:2"));
  settle(80);
  check(sendOnlyNotifications == 0, "disabled receive mode ignores input notifications");
  sendOnly.close();
  if (ok) std::cout << "PASS: VR-50HD MK II LAN protocol regression\n";
  return ok;
}
