#include "update_notification.h"

#include <QApplication>
#include <QLabel>
#include <QEventLoop>
#include <QHostAddress>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; \
    return 1; \
  } \
} while (false)

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  bool japanese = true;
  vtrpon::UpdateWindow window(QStringLiteral("3.0.0-beta.1"),
                              [&japanese] { return japanese; });
  CHECK(window.windowModality() == Qt::NonModal);
  auto *notes = window.findChild<QPlainTextEdit *>(QStringLiteral("updateReleaseNotes"));
  auto *download = window.findChild<QPushButton *>(QStringLiteral("updateDownloadButton"));
  CHECK(notes && download);
  const vtrpon::UpdateRelease release{QStringLiteral("3.0.0-beta.2"),
                                     QStringLiteral("2026-09-25"),
                                     {QStringLiteral("日本語の変更点")},
                                     {QStringLiteral("English release note")}};
  window.setResult(vtrpon::UpdateCheckStatus::Available, release);
  CHECK(window.windowTitle().contains(QStringLiteral("更新")));
  CHECK(notes->toPlainText().contains(QStringLiteral("日本語の変更点")));
  CHECK(!download->isHidden());
  japanese = false;
  window.retranslate();
  CHECK(notes->toPlainText().contains(QStringLiteral("English release note")));
  japanese = true;
  window.setResult(vtrpon::UpdateCheckStatus::Current, std::nullopt);
  CHECK(window.findChild<QLabel *>(QStringLiteral("updateHeading"))->text() ==
        QStringLiteral("最新のバージョンです"));
  CHECK(download->isHidden());
  CHECK(notes->isHidden());
  japanese = false;
  window.setResult(vtrpon::UpdateCheckStatus::Failed, std::nullopt);
  CHECK(!notes->isHidden());
  CHECK(notes->toPlainText().contains(QStringLiteral("network")));

  QTcpServer server;
  CHECK(server.listen(QHostAddress::LocalHost));
  QTemporaryDir settingsDir;
  CHECK(settingsDir.isValid());
  app.setProperty("vtrpon.settingsFile", settingsDir.path() + QStringLiteral("/user.ini"));
  const QUrl localUrl(QStringLiteral("http://127.0.0.1:%1/updates.json").arg(server.serverPort()));
  vtrpon::UpdateCheckStatus checkedStatus = vtrpon::UpdateCheckStatus::Failed;
  bool wasManual = false;
  int completions = 0;
  QEventLoop responseLoop;
  vtrpon::UpdateChecker checker(QStringLiteral("3.0.0-beta.1"),
      [&](bool manual, vtrpon::UpdateCheckStatus status,
          std::optional<vtrpon::UpdateRelease>) {
        wasManual = manual;
        checkedStatus = status;
        ++completions;
        responseLoop.quit();
      }, localUrl);
  const QByteArray validCatalog = R"({"schemaVersion":1,"preview":"3.0.0-beta.1","releases":[{"version":"3.0.0-beta.1","publishedAt":"2026-09-27","notes":{"ja":["確認"],"en":["Check"]}}]})";
  const auto serve = [&](const QByteArray &body, int httpStatus, bool pendManual) {
    const QMetaObject::Connection connection = QObject::connect(&server, &QTcpServer::newConnection,
        &server, [&] {
          auto *socket = server.nextPendingConnection();
          if (pendManual) checker.check(true);
          QTimer::singleShot(100, socket, [socket, body, httpStatus] {
            const QByteArray status = httpStatus == 200 ? "200 OK" : "500 Server Error";
            socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
          });
        });
    QTimer::singleShot(3000, &responseLoop, &QEventLoop::quit);
    checker.check(false);
    responseLoop.exec();
    QObject::disconnect(connection);
  };
  serve(validCatalog, 200, true);
  CHECK(completions == 1 && wasManual && checkedStatus == vtrpon::UpdateCheckStatus::Current);
  serve(QByteArrayLiteral("not JSON"), 200, false);
  CHECK(completions == 2 && !wasManual && checkedStatus == vtrpon::UpdateCheckStatus::Failed);
  serve(validCatalog, 500, false);
  CHECK(completions == 3 && checkedStatus == vtrpon::UpdateCheckStatus::Failed);
  return 0;
}
