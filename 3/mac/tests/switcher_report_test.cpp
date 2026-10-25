#include "switcher_control.h"
#include "switcher_check_window.h"
#include "controller_settings.h"
#include "controller_settings_dialog.h"
#include "controller_settings_io.h"
#include "controller_scaling.h"
#include "atem_addon_switcher.h"
#include "language_text.h"
#include "skins/controller_skin.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>
#include <atomic>
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL switcher-report line %d: %s\n", __LINE__, #x); return false; } } while(false)
namespace {
void pump(int ms = 30) {
  QElapsedTimer t; t.start();
  while (t.elapsed() < ms) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
template<class F> bool until(F f, int limit = 4000) {
  QElapsedTimer t; t.start(); while (!f() && t.elapsed() < limit) pump(5); return f();
}
class MailCapture final : public QObject {
  Q_OBJECT
public:
  QList<QUrl> urls;
public slots:
  void handle(const QUrl &url) { urls.append(url); }
};
struct Counts { std::atomic_int opens{0}, closes{0}, inputs{0}, fades{0}; };
class FakeReportSwitcher final : public vtrpon::SwitcherDevice {
public:
  FakeReportSwitcher(Counts &counts, QObject *parent) : SwitcherDevice(parent), counts_(counts) {}
  bool open(QString *) override { ++counts_.opens; emit diagnosticEvent("DRIVER", "Test transport opened, not a hardware success."); return true; }
  void close() override { ++counts_.closes; }
  bool setProgramInput(int, QString *) override { ++counts_.inputs; emit diagnosticEvent("TX", "Test input request"); return true; }
  bool setFadeToBlack(bool, QString *) override { ++counts_.fades; return true; }
private:
  Counts &counts_;
};
}
namespace vtrpon {
class SwitcherReportTest {
public:
  static bool run() {
    auto &app = *qobject_cast<QApplication *>(QCoreApplication::instance());
    QTemporaryDir temporary; CHECK(temporary.isValid());
    const auto previous = qgetenv("VTRPON_TEST_SWITCHER_REPORT_DIR");
    qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", temporary.path().toUtf8());
    const auto restore = qScopeGuard([&] {
      if (previous.isNull()) qunsetenv("VTRPON_TEST_SWITCHER_REPORT_DIR"); else qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", previous);
      QDesktopServices::unsetUrlHandler("mailto"); QDesktopServices::unsetUrlHandler("file");
    });
    // Validate the shared report contract, redaction, bounded history and explicit observations.
    RolandModelSettings redacted; redacted.password = "SECRET"; redacted.host = "192.168.0.7";
    SwitcherCheckSession session; session.start(RolandSwitcherModel::V8Hd, redacted);
    for (int i = 0; i < 2010; ++i) session.append("RX", "SECRET 192.168.0.7");
    auto report = session.report(); const auto bytes = QJsonDocument(report).toJson();
    CHECK(report["events"].toArray().size() == 2000 && report["droppedEventCount"].toInt() == 11);
    CHECK(!bytes.contains("SECRET") && !bytes.contains("192.168.0.7"));
    CHECK(report["operatorResults"].toObject().isEmpty());
    session.setAnswers("2.10", {{"connection", "failed"}}, "Observed by operator"); session.stop();
    CHECK(!session.report()["endedAt"].toString().isEmpty());
    CHECK(session.report()["operatorResults"].toObject()["connection"] == "failed");

    CHECK(saveSwitcherControlSettings({}));
    Counts counts;
    SwitcherController controller(nullptr); // disabled before installing the fake
    controller.deviceFactory_ = [&](bool, bool, QObject *parent) { return new FakeReportSwitcher(counts, parent); };
    CHECK(!controller.startCheck());
    SwitcherControlSettings settings; settings.rolandV8HdEnabled = true;
    settings.rolandV8HdSourceNumber = 6; settings.takeProgramWhenOnAir = settings.takeOnAirWhenProgramSelected = settings.ftbFollowsProgram = true;
    CHECK(saveSwitcherControlSettings(settings)); controller.applySettings(settings);
    CHECK(until([&] { return controller.switcherReady(); }));
    QWidget owner;
    ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, &controller, nullptr, &owner);
    QObject::connect(&dialog, &ControllerSettingsDialog::switcherControlSettingsChanged, &controller, &SwitcherController::applySettings);
    auto *navigation = dialog.findChild<QListWidget *>("settingsNavigationList");
    CHECK(navigation);
    const auto rolandPages = navigation->findItems(uiText("settings.category.roland"), Qt::MatchExactly);
    CHECK(rolandPages.size() == 1);
    CHECK(navigation->findItems(uiText("settings.category.atem"), Qt::MatchExactly).isEmpty());
    auto *rolandScroll = dialog.findChild<QScrollArea *>("switcherSettingsScroll");
    auto *rolandEnabled = dialog.findChild<QCheckBox *>("rolandV8HdEnabledCheckBox");
    auto *check = dialog.findChild<QPushButton *>("rolandCheckButton");
    CHECK(rolandScroll && rolandEnabled && check && check->isEnabled());
    CHECK(rolandScroll->isAncestorOf(check) && rolandScroll->isAncestorOf(rolandEnabled));
    auto *models = dialog.findChild<QComboBox *>("rolandModelComboBox");
    auto *sources = dialog.findChild<QSpinBox *>("rolandV8HdSourceSpinBox");
    auto *host = dialog.findChild<QLineEdit *>("rolandHostEdit");
    auto *password = dialog.findChild<QLineEdit *>("rolandPasswordEdit");
    auto *receive = dialog.findChild<QCheckBox *>("rolandTakeOnAirWhenProgramSelectedCheckBox");
    auto *fade = dialog.findChild<QCheckBox *>("rolandFtbFollowsProgramCheckBox");
    CHECK(models && sources && host && password && receive && fade && models->count() == 13);
    auto *items = qobject_cast<QStandardItemModel *>(models->model()); CHECK(items);
    int supported = 0;
    for (int i = 0; i < models->count(); ++i) {
      const auto model = static_cast<RolandSwitcherModel>(models->itemData(i).toInt());
      const auto &p = rolandSwitcherProfile(model);
      CHECK(items->item(i)->isEnabled() == rolandSwitcherSupported(model));
      if (!rolandSwitcherSupported(model)) {
        SwitcherControlSettings unavailable; unavailable.rolandModel = model; unavailable.rolandV8HdEnabled = true;
        CHECK(!saveSwitcherControlSettings(unavailable));
        models->setCurrentIndex(i);
        CHECK(!rolandEnabled->isEnabled() && !check->isEnabled());
        CHECK(!dialog.findChild<QLabel *>("rolandConnectionLabel")->property("ready").toBool());
        CHECK(loadSwitcherControlSettings().rolandModel != model);
        models->setCurrentIndex(0);
        continue;
      }
      ++supported; models->setCurrentIndex(i);
      CHECK(sources->maximum() == p.inputs && receive->isEnabled() == p.receive && fade->isEnabled() == p.fade);
      sources->setValue(p.inputs);
      host->setText("127.0.0.1"); QMetaObject::invokeMethod(host, "editingFinished", Qt::DirectConnection);
      if (p.passwordLength) { password->setText(QString(p.passwordLength, 'P')); QMetaObject::invokeMethod(password, "editingFinished", Qt::DirectConnection); }
      models->setCurrentIndex(0); models->setCurrentIndex(i); dialog.reloadSavedSettings();
      CHECK(sources->value() == p.inputs && loadSwitcherControlSettings().rolandModel == model);
      if (p.passwordLength) CHECK(password->text() == QString(p.passwordLength, 'P'));
    }
    CHECK(supported == 11);
    models->setCurrentIndex(0); sources->setValue(6);
    receive->setChecked(true); fade->setChecked(true);
    auto exportStore = makeControllerSettings();
    const auto exportedProfiles = controllerSettingsSnapshot(exportStore).value("switcher/roland/profiles").toObject();
    for (const auto &value : exportedProfiles) CHECK(!value.toObject().contains("password"));
    const auto captures = qEnvironmentVariable("VTRPON_TEST_SWITCHER_CAPTURE_DIR");
    for (const auto &skin : {QStringLiteral("hardware"), QStringLiteral("material-dark"), QStringLiteral("vtr-pon-clasic")}) {
      CHECK(applyControllerSkin(app, skin));
      navigation->setCurrentItem(rolandPages.front()); dialog.show(); pump();
      CHECK(rolandScroll->isVisible() && check->isVisible());
      if (!captures.isEmpty()) {
        CHECK(QDir().mkpath(captures));
        CHECK(dialog.grab().save(QDir(captures).filePath(skin + "-settings.png")));
        for (int i = 0; i < models->count(); ++i) {
          const auto model = static_cast<RolandSwitcherModel>(models->itemData(i).toInt());
          if (!rolandSwitcherSupported(model)) continue;
          models->setCurrentIndex(i); pump();
          CHECK(dialog.grab().save(QDir(captures).filePath(skin + "-" + rolandSwitcherProfile(model).id + ".png")));
        }
        models->setCurrentIndex(0); sources->setValue(6); receive->setChecked(true); fade->setChecked(true);
      }
    }
    setUiTextSize(2);
    {
      ControllerSettingsDialog large(nullptr, nullptr, nullptr, &controller, nullptr, &owner);
      auto *largeNav = large.findChild<QListWidget *>("settingsNavigationList");
      const auto pages = largeNav->findItems(uiText("settings.category.roland"), Qt::MatchExactly);
      CHECK(pages.size() == 1); largeNav->setCurrentItem(pages.front());
      auto *largeScroll = large.findChild<QScrollArea *>("switcherSettingsScroll");
      auto *largeCheck = large.findChild<QPushButton *>("rolandCheckButton");
      CHECK(largeScroll && largeCheck); large.resize(900, 650); large.show(); pump();
      largeScroll->ensureWidgetVisible(largeCheck); pump();
      CHECK(largeCheck->isVisible() && largeScroll->viewport()->rect().contains(largeCheck->mapTo(largeScroll->viewport(), largeCheck->rect().center())));
      if (!captures.isEmpty()) CHECK(large.grab().save(QDir(captures).filePath("large-settings.png")));
      large.close();
    }
    setUiTextSize(0);
    check->click(); CHECK(dialog.result() == QDialog::Accepted && !dialog.isVisible());
    QPointer<QWidget> window = owner.findChild<QWidget *>("switcherCheckWindow");
    CHECK(window && !window->isModal());
    auto *start = window->findChild<QPushButton *>("switcherCheckStart");
    auto *stop = window->findChild<QPushButton *>("switcherCheckStop");
    auto *save = window->findChild<QPushButton *>("switcherCheckSave");
    auto *email = window->findChild<QPushButton *>("switcherCheckEmail");
    auto *folder = window->findChild<QPushButton *>("switcherCheckReportFolder");
    auto *firmware = window->findChild<QLineEdit *>("switcherCheckFirmware");
    auto *answer = window->findChild<QComboBox *>("switcherCheckResult0");
    auto *comment = window->findChild<QPlainTextEdit *>("switcherCheckComment");
    CHECK(start && stop && save && email && folder && firmware && answer && comment);
    CHECK(!email->isEnabled() && !folder->isEnabled());
    start->click(); CHECK(controller.checkSession().active());
    CHECK(until([&] { return controller.switcherReady(); }));
    CHECK(counts.inputs == 0 && counts.fades == 0 && !email->isEnabled());
    CHECK(answer->currentData() == "not-tested");
    CHECK(controller.checkSession().report()["settings"].toObject()["sourceNumber"] == 6);
    CHECK(QMetaObject::invokeMethod(&controller, "handleProgramOnAirChanged", Qt::DirectConnection, Q_ARG(bool, true)));
    CHECK(until([&] { return counts.inputs == 1; })); pump();
    CHECK(QJsonDocument(controller.checkSession().report()).toJson().contains("Test input request"));
    CHECK(answer->currentData() == "not-tested"); // a protocol event cannot mark success
    firmware->setText(QStringLiteral("2.10 + 日本語 & #"));
    answer->setCurrentIndex(answer->findData("failed")); comment->setPlainText(QString(4096, 'X'));
    stop->click(); CHECK(!controller.checkSession().active() && email->isEnabled());
    const auto observed = controller.checkSession().report();
    CHECK(observed["operatorResults"].toObject()["connection"] == "failed");
    auto mail = switcherCheckReportMailUrl(observed, temporary.filePath("private/report.json"));
    QUrlQuery query(mail); CHECK(mail.path() == "info@pondashi.com" && query.queryItems().size() == 2);
    const auto body = query.queryItemValue("body", QUrl::FullyDecoded);
    CHECK(body.contains("2.10 + 日本語 & #") && body.contains("report.json"));
    CHECK(!body.contains(temporary.path()) && !body.contains(QString(4096, 'X')));
    MailCapture capture;
    QDesktopServices::setUrlHandler("mailto", &capture, "handle");
    QDesktopServices::setUrlHandler("file", &capture, "handle");
    // The actual background save and mailto fallback run against a temporary directory and intercepted URLs.
    email->click(); CHECK(until([&] { return capture.urls.size() == 1 && folder->isEnabled(); }));
    CHECK(capture.urls.front().scheme() == "mailto");
    const QDir directory(temporary.filePath("VTR-PON/SwitcherReports"));
    const auto files = directory.entryList({"*.json"}, QDir::Files); CHECK(files.size() == 1);
    QFile saved(directory.filePath(files.front())); CHECK(saved.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(saved.readAll()).object() == observed); saved.close();
    folder->click(); CHECK(capture.urls.size() == 2 && capture.urls.last().toLocalFile() == directory.path());
    if (!captures.isEmpty()) CHECK(window->grab().save(QDir(captures).filePath("report-window.png")));
    // A failed write must leave the previous JSON available and never open mail.
    const auto blockedPath = temporary.filePath("blocked-directory");
    QFile blocker(blockedPath); CHECK(blocker.open(QIODevice::WriteOnly)); blocker.write("file, not a folder"); blocker.close();
    qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", blockedPath.toUtf8()); email->click();
    CHECK(until([&] {
      if (!email->isEnabled()) return false;
      for (auto *label : window->findChildren<QLabel *>()) if (label->text().contains(blockedPath)) return true;
      return false;
    }));
    CHECK(capture.urls.size() == 2 && QFile::exists(saved.fileName()));
    qputenv("VTRPON_TEST_SWITCHER_REPORT_DIR", temporary.path().toUtf8());
    // A changed configuration ends recording, rather than mixing sessions.
    start->click(); CHECK(controller.checkSession().active());
    settings.rolandV8HdSourceNumber = 7; controller.applySettings(settings);
    CHECK(!controller.checkSession().active() && controller.checkSession().report()["settings"].toObject()["sourceNumber"] == 6);
    pump(550); start->click(); CHECK(controller.checkSession().active());
    CHECK(controller.checkSession().report()["settings"].toObject()["sourceNumber"] == 7);
    window->close(); CHECK(!controller.checkSession().active()); pump();
    CHECK(!window); // no dangling completion callback or modal child
    CHECK(controller.startCheck());
    controller.requestShutdown(); CHECK(!controller.checkSession().active());
    CHECK(!controller.startCheck()); controller.retryConnection();
    CHECK(until([&] { return controller.isShutdownComplete(); }));
    // Existing setting keys still round-trip through JSON without a schema migration.
    auto store = makeControllerSettings();
    const auto snapshot = controllerSettingsSnapshot(store);
    CHECK(snapshot.contains("switcher/rolandV8Hd/sourceNumber"));
    const auto exported = temporary.filePath("settings.json"); CHECK(exportControllerSettings(store, exported));
    QSettings restored(temporary.filePath("restored.ini"), QSettings::IniFormat);
    CHECK(importControllerSettings(restored, exported) && controllerSettingsSnapshot(restored) == snapshot);
    std::puts("PASS switcher report: fake-worker isolation, page split, session lifecycle, bounded/redacted JSON, background save and intercepted mailto/folder, legacy settings");
    return true;
  }
};
}
bool runSwitcherReportRegression() { return vtrpon::SwitcherReportTest::run(); }
#include "switcher_report_test.moc"
