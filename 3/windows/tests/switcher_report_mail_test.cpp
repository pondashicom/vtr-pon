#include "switcher_check_window.h"
#include "switcher_control.h"
#include "language_text.h"
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>
#include <QUrlQuery>
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL report-mail line %d: %s\n", __LINE__, #x); return 1; } } while (false)

namespace {
template<class F> bool until(F f) {
  QElapsedTimer timer; timer.start();
  while (!f() && timer.elapsed() < 4000) {
    QCoreApplication::processEvents(); QThread::msleep(2);
  }
  return f();
}
class UrlCapture final : public QObject {
  Q_OBJECT
public:
  QList<QUrl> urls;
public slots:
  void handle(const QUrl &url) { urls.append(url); }
};
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  CHECK(argc == 2);
  QTranslator translator; CHECK(translator.load(QString::fromLocal8Bit(argv[1])));
  app.installTranslator(&translator);
  QTemporaryDir temporary; CHECK(temporary.isValid());
  app.setProperty("vtrpon.settingsFile", temporary.filePath("settings.ini"));
  qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", temporary.path().toUtf8());
  qputenv("VTRPON_TEST_SWITCHER_COMPOSE", "unavailable");
  CHECK(vtrpon::saveSwitcherControlSettings({}));
  // No hardware is opened: only an already recorded report is used here.
  vtrpon::SwitcherController controller(nullptr);
  controller.checkSession().start(vtrpon::RolandSwitcherModel::V80Hd, {});
  controller.checkSession().setAnswers(QStringLiteral("2.10 + 日本語 & #"),
      {{"connection", "failed"}, {"fade", "unavailable"}}, QString(4096, 'X'));
  controller.stopCheck();
  QWidget owner;
  vtrpon::showSwitcherCheckWindow(&controller, nullptr, &owner);
  QPointer<QWidget> window = owner.findChild<QWidget *>("switcherCheckWindow");
  CHECK(window && !window->isModal());
  auto *email = window->findChild<QPushButton *>("switcherCheckEmail");
  auto *folder = window->findChild<QPushButton *>("switcherCheckReportFolder");
  auto *openEmail = window->findChild<QPushButton *>("switcherCheckOpenEmail");
  auto *help = window->findChild<QLabel *>("switcherCheckManualAttachment");
  auto *comment = window->findChild<QPlainTextEdit *>("switcherCheckComment");
  CHECK(email && folder && openEmail && help && comment);
  CHECK(openEmail->isHidden() && help->isHidden());
  CHECK(email->text() == vtrpon::uiText("switcher.check.email"));
  UrlCapture capture;
  QDesktopServices::setUrlHandler("mailto", &capture, "handle");
  QDesktopServices::setUrlHandler("file", &capture, "handle");

  const auto observed = controller.checkSession().report();
  email->click(); CHECK(!email->isEnabled() && !comment->isEnabled());
  CHECK(until([&] { return email->isEnabled() && !openEmail->isHidden(); }));
  CHECK(capture.urls.isEmpty() && help->isVisible() && folder->isEnabled());
  const QDir directory(temporary.filePath("VTR-PON/SwitcherReports"));
  auto files = directory.entryList({"*.json"}, QDir::Files); CHECK(files.size() == 1);
  auto reportPath = directory.filePath(files.front());
  QFile saved(reportPath); CHECK(saved.open(QIODevice::ReadOnly));
  CHECK(QJsonDocument::fromJson(saved.readAll()).object() == observed); saved.close();
  CHECK(help->text().contains(QDir::toNativeSeparators(reportPath)));
  folder->click(); CHECK(capture.urls.size() == 1 && capture.urls.last().toLocalFile() == directory.path());
  openEmail->click(); CHECK(capture.urls.size() == 2 && capture.urls.last().scheme() == "mailto");
  QUrlQuery query(capture.urls.last());
  CHECK(query.queryItems().size() == 2 && capture.urls.last().path() == "info@pondashi.com");
  const auto body = query.queryItemValue("body", QUrl::FullyDecoded);
  CHECK(body.startsWith(vtrpon::uiText("switcher.check.mail_attachment").arg(files.front()).replace("\n", "\r\n")));
  CHECK(body.contains(QStringLiteral("2.10 + 日本語 & #")));
  CHECK(!body.contains(temporary.path()) && !body.contains(QString(4096, 'X')));
  CHECK(help->isVisible()); // instructions remain available after the composer opens
  const auto captures = qEnvironmentVariable("VTRPON_TEST_SWITCHER_CAPTURE_DIR");
  if (!captures.isEmpty()) {
    CHECK(QDir().mkpath(captures));
    const auto language = QFileInfo(QString::fromLocal8Bit(argv[1])).baseName();
    CHECK(window->grab().save(QDir(captures).filePath(language + "-attachment.png")));
    window->resize(500, 640); QCoreApplication::processEvents();
    CHECK(window->rect().contains(openEmail->geometry()) && window->rect().contains(folder->geometry()));
    CHECK(window->grab().save(QDir(captures).filePath(language + "-attachment-narrow.png")));
  }

  // Changing the report invalidates the saved snapshot and its email action.
  comment->setPlainText("Updated observation");
  CHECK(openEmail->isHidden() && help->isHidden());
  openEmail->click(); CHECK(capture.urls.size() == 2);
  email->click(); CHECK(until([&] { return email->isEnabled() && !openEmail->isHidden(); }));
  CHECK(capture.urls.size() == 2);
  files = directory.entryList({"*.json"}, QDir::Files); CHECK(files.size() == 2);
  for (const auto &file : files) CHECK(QFile::remove(directory.filePath(file)));
  openEmail->click(); CHECK(capture.urls.size() == 2 && openEmail->isHidden());

  // Save failure cannot open mail, and a cancelled native composer must not fall back to mailto.
  QFile blocked(temporary.filePath("blocked")); CHECK(blocked.open(QIODevice::WriteOnly)); blocked.close();
  qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", blocked.fileName().toUtf8());
  email->click(); CHECK(until([&] { return email->isEnabled(); }));
  CHECK(openEmail->isHidden() && capture.urls.size() == 2);
  qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", temporary.path().toUtf8());
  for (const auto *result : {"cancelled", "completed"}) {
    qputenv("VTRPON_TEST_SWITCHER_COMPOSE", result);
    email->click(); CHECK(until([&] { return email->isEnabled(); }));
    CHECK(openEmail->isHidden() && capture.urls.size() == 2);
  }
  qputenv("VTRPON_TEST_SWITCHER_COMPOSE", "unavailable");
  email->click(); window->close();
  CHECK(until([&] { return !window; }));
  for (bool usb : {true, false}) {
    vtrpon::RolandModelSettings settings; settings.useUsbMidi = usb; settings.ftbFollowsProgram = true;
    controller.checkSession().start(vtrpon::RolandSwitcherModel::Vr120Hd, settings);
    controller.stopCheck();
    vtrpon::showSwitcherCheckWindow(&controller, nullptr, &owner);
    window = owner.findChild<QWidget *>("switcherCheckWindow"); CHECK(window);
    auto *fade = window->findChild<QComboBox *>("switcherCheckResult3");
    auto *preview = window->findChild<QPlainTextEdit *>("switcherCheckPreview");
    CHECK(fade && preview && fade->isEnabled() == !usb);
    const auto snapshot = QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object();
    CHECK(snapshot["transport"] == (usb ? "USB MIDI" : "LAN"));
    CHECK(snapshot["capabilities"].toObject()["fade"].toBool() == !usb);
    if (!captures.isEmpty()) CHECK(window->grab().save(QDir(captures).filePath(
        QFileInfo(QString::fromLocal8Bit(argv[1])).baseName() + (usb ? "-vr120-usb.png" : "-vr120-lan.png"))));
    window->close(); CHECK(until([&] { return !window; }));
  }
  QDesktopServices::unsetUrlHandler("mailto"); QDesktopServices::unsetUrlHandler("file");
  std::puts("PASS: attachment steps before mail, saved snapshot, mail body, edits, missing file, save failure, native completion/cancellation and close");
  return 0;
}

#include "switcher_report_mail_test.moc"
