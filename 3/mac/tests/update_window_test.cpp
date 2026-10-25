#include "update_notification.h"
#include "controller_settings.h"

#include <QApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; return 1; } } while (false)

class UrlRecorder final : public QObject {
  Q_OBJECT
public:
  QList<QUrl> urls;
public slots:
  void openUrl(const QUrl &url) { urls.append(url); }
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTemporaryDir settingsDir;
  CHECK(settingsDir.isValid());
  app.setProperty("vtrpon.settingsFile", settingsDir.path() + QStringLiteral("/user.ini"));
  using namespace vtrpon;
  const UpdateTarget windows{QStringLiteral("windows-x64"),
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/updates.json")),
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/download/windows/"))};
  const UpdateTarget mac{QStringLiteral("macos-arm64"), windows.catalogUrl,
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/download/mac/index.html"))};
  bool japanese = true;
  UpdateWindow window(QStringLiteral("3.0.0-beta.1"), [&] { return japanese; }, windows);
  CHECK(window.windowModality() == Qt::NonModal);
  auto *notes = window.findChild<QPlainTextEdit *>("updateReleaseNotes");
  auto *download = window.findChild<QPushButton *>("updateDownloadButton");
  CHECK(notes && download);
  UpdateRelease release{QStringLiteral("3.0.0-beta.2"), QStringLiteral("2026-10-19"),
      {QStringLiteral("日本語の変更点")}, {QStringLiteral("English release note")},
      windows.platform, windows.downloadPage};
  window.setResult(UpdateCheckStatus::Available, release);
  CHECK(window.windowTitle().contains(QStringLiteral("更新")));
  CHECK(notes->toPlainText().contains(QStringLiteral("日本語の変更点")));
  CHECK(!download->isHidden() && download->isEnabled());
  UrlRecorder recorder;
  QDesktopServices::setUrlHandler(QStringLiteral("https"), &recorder, "openUrl");
  download->click();
  CHECK(recorder.urls.size() == 1 && recorder.urls.back() == windows.downloadPage);
  japanese = false; window.retranslate();
  CHECK(notes->toPlainText().contains(QStringLiteral("English release note")));
  japanese = true; window.setResult(UpdateCheckStatus::Current, std::nullopt);
  CHECK(window.findChild<QLabel *>("updateHeading")->text() == QStringLiteral("最新のバージョンです"));
  CHECK(download->isHidden() && notes->isHidden());
  japanese = false; window.setResult(UpdateCheckStatus::Failed, std::nullopt);
  CHECK(!notes->isHidden() && notes->toPlainText().contains(QStringLiteral("network")));

  UpdateWindow macWindow(QStringLiteral("3.0.0-beta.1"), [&] { return japanese; }, mac);
  auto *macDownload = macWindow.findChild<QPushButton *>("updateDownloadButton");
  auto *macHeading = macWindow.findChild<QLabel *>("updateHeading");
  auto *macNotes = macWindow.findChild<QPlainTextEdit *>("updateReleaseNotes");
  // A caller cannot bypass OS/CPU and destination checks by supplying an Available result.
  macWindow.setResult(UpdateCheckStatus::Available, release);
  CHECK(macDownload->isHidden() && !macDownload->isEnabled());
  CHECK(macHeading->text().contains(QStringLiteral("not available")));
  macDownload->setEnabled(true); macDownload->click();
  CHECK(recorder.urls.size() == 1);
  release.platform = mac.platform; release.downloadPage = mac.downloadPage;
  macWindow.setResult(UpdateCheckStatus::Available, release);
  CHECK(!macDownload->isHidden() && macDownload->isEnabled());
  macDownload->click();
  CHECK(recorder.urls.size() == 2 && recorder.urls.back() == mac.downloadPage);
  for (const auto &wrong : {QStringLiteral("http://pondashi.com/vtr-pon/download/mac/index.html"),
      QStringLiteral("https://example.com/vtr-pon/download/mac/index.html"),
      QStringLiteral("https://pondashi.com/vtr-pon/download/windows/"),
      QStringLiteral("https://name@pondashi.com/vtr-pon/download/mac/index.html"),
      QStringLiteral("https://pondashi.com/vtr-pon/download/mac/index.html?next=windows"),
      QStringLiteral("https://pondashi.com/vtr-pon/download/mac/index.html../windows/")}) {
    auto invalid = release; invalid.downloadPage = QUrl(wrong);
    macWindow.setResult(UpdateCheckStatus::Available, invalid);
    CHECK(macDownload->isHidden() && !macDownload->isEnabled());
  }
  auto intel = release; intel.platform = QStringLiteral("macos-x64");
  macWindow.setResult(UpdateCheckStatus::Available, intel);
  CHECK(macDownload->isHidden());
  japanese = true;
  macWindow.setResult(UpdateCheckStatus::Unavailable, std::nullopt);
  CHECK(macHeading->text().contains(QStringLiteral("まだ提供")));
  CHECK(macNotes->toPlainText().contains(QStringLiteral("引き続き使用")));
  japanese = false; macWindow.retranslate();
  CHECK(macNotes->toPlainText().contains(QStringLiteral("continue using")));
#ifdef Q_OS_MACOS
  UpdateWindow nativeTarget(QStringLiteral("3.0.0-beta.1"), [&] { return japanese; });
  nativeTarget.setResult(UpdateCheckStatus::Current, std::nullopt);
  CHECK(nativeTarget.findChild<QPushButton *>("updateDownloadButton")->isHidden());
  CHECK(nativeTarget.findChild<QLabel *>("updateHeading")->text().contains(QStringLiteral("up to date")));
  CHECK(defaultUpdateTarget().downloadPage == mac.downloadPage);
  CHECK(updateTargetConfigured(defaultUpdateTarget()));
#endif
  macWindow.setResult(UpdateCheckStatus::Current, std::nullopt);
  CHECK(macHeading->text().contains(QStringLiteral("up to date")) && macDownload->isHidden());
  QDesktopServices::unsetUrlHandler(QStringLiteral("https"));

  QTcpServer server;
  CHECK(server.listen(QHostAddress::LocalHost));
  const QUrl localUrl(QStringLiteral("http://127.0.0.1:%1/updates.json").arg(server.serverPort()));
  UpdateCheckStatus checkedStatus = UpdateCheckStatus::Failed;
  bool wasManual = false;
  int completions = 0, requests = 0;
  std::optional<UpdateRelease> checkedRelease;
  QEventLoop responseLoop;
  const auto complete = [&](bool manual, UpdateCheckStatus status, std::optional<UpdateRelease> value) {
    wasManual = manual; checkedStatus = status; checkedRelease = std::move(value);
    ++completions; responseLoop.quit();
  };
  auto localWindows = windows; localWindows.catalogUrl = localUrl;
  auto localMac = mac; localMac.catalogUrl = localUrl;
  UpdateChecker checker(QStringLiteral("3.0.0-beta.1"), complete, localWindows);
  UpdateChecker macChecker(QStringLiteral("3.0.0-beta.1"), complete, localMac);
  const QByteArray legacy = R"({"schemaVersion":1,"preview":"3.0.0-beta.1","releases":[{"version":"3.0.0-beta.1","publishedAt":"2026-09-27","notes":{"ja":["確認"],"en":["Check"]}}]})";
  auto mixed = QJsonDocument::fromJson(legacy).object();
  const auto channel = QJsonDocument::fromJson(R"({"preview":"3.0.0-beta.2","releases":[{"version":"3.0.0-beta.2","publishedAt":"2026-10-19","notes":{"ja":["Mac向け"],"en":["For Mac"]}}]})").object();
  mixed.insert(QStringLiteral("platforms"), QJsonObject{{mac.platform, channel}});
  const auto serve = [&](UpdateChecker &subject, const QByteArray &body, int httpStatus,
                         bool pendManual, bool manual = false) {
    const auto connection = QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
      auto *socket = server.nextPendingConnection(); ++requests;
      if (pendManual) subject.check(true);
      QTimer::singleShot(40, socket, [socket, body, httpStatus] {
        socket->write("HTTP/1.1 " + QByteArray::number(httpStatus) + " Response\r\nContent-Type: application/json\r\n" +
            (httpStatus == 302 ? QByteArray("Location: /redirected\r\n") : QByteArray()) +
            "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
    QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &responseLoop, &QEventLoop::quit);
    deadline.start(3000); subject.check(manual); responseLoop.exec();
    QObject::disconnect(connection);
  };
  serve(checker, legacy, 200, true);
  CHECK(completions == 1 && wasManual && checkedStatus == UpdateCheckStatus::Current);
  serve(checker, QByteArrayLiteral("not JSON"), 200, false);
  CHECK(completions == 2 && !wasManual && checkedStatus == UpdateCheckStatus::Failed);
  serve(checker, legacy, 500, false);
  CHECK(completions == 3 && checkedStatus == UpdateCheckStatus::Failed);
  serve(macChecker, legacy, 200, false, true);
  CHECK(completions == 4 && wasManual && checkedStatus == UpdateCheckStatus::Unavailable && !checkedRelease);
  serve(macChecker, QJsonDocument(mixed).toJson(), 200, false);
  CHECK(completions == 5 && checkedStatus == UpdateCheckStatus::Available && checkedRelease);
  CHECK(checkedRelease->platform == mac.platform && checkedRelease->downloadPage == mac.downloadPage);
  serve(macChecker, QByteArrayLiteral("{\"schemaVersion\":1,\"platforms\":{\"macos-arm64\":null}}"), 200, false);
  CHECK(completions == 6 && checkedStatus == UpdateCheckStatus::Failed);
  serve(macChecker, legacy, 302, false);
  CHECK(completions == 7 && checkedStatus == UpdateCheckStatus::Failed && requests == 7);
  serve(macChecker, QByteArray(256 * 1024 + 1, 'x'), 200, false);
  CHECK(completions == 8 && checkedStatus == UpdateCheckStatus::Failed);
  auto noPage = localMac; noPage.downloadPage = QUrl{};
  UpdateChecker unavailable(QStringLiteral("3.0.0-beta.1"), complete, noPage);
  const int beforeRequests = requests;
  QTimer::singleShot(2000, &responseLoop, &QEventLoop::quit);
  unavailable.check(true); responseLoop.exec();
  CHECK(completions == 9 && wasManual && checkedStatus == UpdateCheckStatus::Unavailable && requests == beforeRequests);
  auto settings = makeControllerSettings(); settings.setValue(QStringLiteral("updates/checkAutomatically"), false); settings.sync();
  unavailable.check(false);
  QTimer::singleShot(30, &responseLoop, &QEventLoop::quit); responseLoop.exec();
  CHECK(completions == 9 && requests == beforeRequests);
  UpdateChecker macCurrent(QStringLiteral("3.0.0-beta.2"), complete, localMac);
  serve(macCurrent, QJsonDocument(mixed).toJson(), 200, false, true);
  CHECK(completions == 10 && wasManual && checkedStatus == UpdateCheckStatus::Current && !checkedRelease);
  std::cout << "PASS: Windows compatibility, OS/CPU isolation, Mac unavailable/current/available, guarded download URLs, network errors and preferences\n";
  return 0;
}

#include "update_window_test.moc"
