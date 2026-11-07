#include "update_notification.h"
#include "controller_settings.h"
#include "product_identity.h"
#include <QApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <memory>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; return 1; } } while (false)

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTemporaryDir settingsDirectory;
  CHECK(settingsDirectory.isValid());
  app.setProperty("vtrpon.settingsFile", settingsDirectory.path() + "/user.ini");
  CHECK(vtrpon::isDistributionDirectory(QCoreApplication::applicationDirPath()));
  QTcpServer server;
  CHECK(server.listen(QHostAddress::LocalHost));
  const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
  QList<QJsonObject> notices;
  int responseStatus = 204;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (server.hasPendingConnections()) {
      auto *socket = server.nextPendingConnection();
      auto buffer = std::make_shared<QByteArray>();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
        buffer->append(socket->readAll());
        const auto end = buffer->indexOf("\r\n\r\n");
        if (end < 0) return;
        int length = 0;
        for (const auto &line : buffer->left(end).split('\n'))
          if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
        if (buffer->size() < end + 4 + length) return;
        int status = 200;
        QByteArray body = R"({"schemaVersion":1,"releases":[]})";
        if (buffer->startsWith("POST /activity.php ")) {
          notices.append(QJsonDocument::fromJson(buffer->mid(end + 4, length)).object());
          status = responseStatus;
          body.clear();
        }
        socket->write("HTTP/1.1 " + QByteArray::number(status) +
            " Response\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
      QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
  });
  auto settings = vtrpon::makeControllerSettings();
  const QString lastSent = QStringLiteral("privacy/activityLastSentUtcDay");
  const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
  auto check = [&](const QString &platform, bool twice = false) {
    QEventLoop loop;
    vtrpon::UpdateTarget target{platform, QUrl(base + "/updates.json"),
        QUrl(platform.startsWith("macos-") ? "https://pondashi.com/vtr-pon/download/mac/index.html"
                                           : "https://pondashi.com/vtr-pon/download/windows/")};
    vtrpon::UpdateChecker checker("3.0.0-beta.3",
        [&](bool, auto, auto) { QTimer::singleShot(150, &loop, &QEventLoop::quit); },
        target, QUrl(base + "/activity.php"));
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    checker.check(true);
    if (twice) checker.check(true);
    loop.exec();
    settings.sync();
  };
  check("macos-arm64");
  CHECK(notices.isEmpty()); // Consent stays off by default.
  settings.setValue("privacy/sendActivityNotice", true); settings.sync();
  check("macos-arm64", true);
  CHECK(notices.size() == 1 && notices.back().size() == 2);
  CHECK(notices.back().value("platform").toString() == "macos-arm64");
  CHECK(notices.back().value("version").toString() == "3.0.0-beta.3");
  CHECK(settings.value(lastSent).toString() == today);
  check("macos-arm64");
  CHECK(notices.size() == 1); // At most one successful notice per UTC day.
  settings.remove(lastSent); settings.sync();
  check("windows-x64");
  CHECK(notices.size() == 2 && notices.back().value("platform").toString() == "windows-x64");
  settings.remove(lastSent); settings.sync(); responseStatus = 503;
  check("macos-arm64");
  CHECK(notices.size() == 3 && !settings.contains(lastSent));
  responseStatus = 204;
  check("macos-arm64");
  CHECK(notices.size() == 4 && settings.value(lastSent).toString() == today);
  std::cout << "PASS: opt-in, explicit OS/CPU payload, pending and daily deduplication, failed-send retry\n";
  return 0;
}
