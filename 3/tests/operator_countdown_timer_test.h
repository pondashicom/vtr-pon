#pragma once
#include "operator_countdown_timer.h"
#include "operator_monitor_settings_page.h"
#include "operator_timer_settings_dialog.h"
#include "operator_monitor_widget.h"
#include "view_menu_controller.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QMenuBar>
#include <QThread>
#include <QTranslator>
#include <iostream>

inline bool runOperatorCountdownTimerTest() {
  bool ok = true;
  const auto check = [&](bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ok = false; }
  };
  vtrpon::OperatorMonitorSettings config;
  config.timerEnabled = true;
  config.timerDurationSeconds = 185;
  vtrpon::OperatorCountdownTimer timer;
  timer.configure(config, 1000);
  check(timer.presentation(1000).value("text") == "03:05", "duration formatting");
  timer.start(1000);
  timer.start(5000);
  check(timer.presentation(6000).value("text") == "03:00", "repeated start does not restart");
  check(timer.presentation(65999).value("warning").toInt() == 0, "warning not early");
  check(timer.presentation(66000).value("warning").toInt() == 1, "first warning boundary");
  timer.pause(66750);
  check(timer.presentation(999999).value("text") == "02:00", "pause freezes elapsed time");
  timer.start(1000000);
  check(timer.presentation(1059250).value("text") == "01:00" &&
        timer.presentation(1059250).value("warning").toInt() == 2, "resume preserves fractional seconds and second warning");
  config.timerWarning2Color = QColor("#014060");
  timer.configure(config, 1059250);
  check(timer.presentation(1059250).value("warningColor") == "#014060" &&
        timer.presentation(1059250).value("running").toBool(), "color changes keep counting");
  check(timer.presentation(1119250).value("text") == "00:00", "zero boundary");
  check(timer.presentation(1120250).value("text").toString() == vtrpon::operatorTimerText(-1), "overtime continues");
  timer.reset(1120250);
  check(!timer.presentation(1120250).value("running").toBool() &&
        timer.presentation(1120250).value("warning").toInt() == 0, "reset stops and clears warning");
  config.timerEnabled = false;
  timer.configure(config, 1120250);
  timer.start(1120250);
  check(!timer.presentation(1121250).value("running").toBool(), "disabled timer cannot start");

  QTemporaryDir directory;
  check(directory.isValid(), "timer settings directory");
  const QVariant original = qApp->property("vtrpon.settingsFile");
  struct RestoreSettings {
    QVariant original;
    ~RestoreSettings() { qApp->setProperty("vtrpon.settingsFile", original); }
  } restore{original};
  qApp->setProperty("vtrpon.settingsFile", directory.filePath("timer.ini"));
  OperatorMonitorSettingsPage page;
  page.resize(540, 420);
  OperatorTimerSettingsDialog dialog;
  auto *enable = page.findChild<QCheckBox *>("operatorMonitorTimerEnabled");
  auto *information = page.findChild<QCheckBox *>("operatorMonitorInformation");
  auto *duration = dialog.findChild<QSpinBox *>("operatorMonitorTimerDurationMinutes");
  auto *durationSeconds = dialog.findChild<QSpinBox *>("operatorMonitorTimerDurationSeconds");
  auto *first = dialog.findChild<QSpinBox *>("operatorMonitorTimerWarning1");
  auto *second = dialog.findChild<QSpinBox *>("operatorMonitorTimerWarning2");
  auto *save = dialog.findChild<QPushButton *>("operatorMonitorTimerSave");
  check(!page.findChild<QLineEdit *>("operatorMonitorCueInput") &&
        !page.findChild<QSpinBox *>("operatorMonitorTimerDurationMinutes") &&
        !dialog.findChild<QCheckBox *>("operatorMonitorTimerEnabled"), "settings are split between the two windows");
  check(duration->isEnabled() && save->isEnabled(), "timer can be configured while disabled");
  int openRequests = 0;
  page.timerSettingsRequested = [&] { ++openRequests; };
  page.findChild<QPushButton *>("operatorMonitorTimerSettingsButton")->click();
  check(openRequests == 1, "monitor settings exposes timer settings entry");
  information->setChecked(true);
  enable->setChecked(true);
  for (const auto field : {"remaining", "file", "modes", "rate"})
    check(!page.findChild<QCheckBox *>(QStringLiteral("operatorMonitorField_") + field)->isEnabled(), "conflicting information disabled");
  check(page.findChild<QCheckBox *>("operatorMonitorField_pages")->isEnabled(), "page information remains available");
  duration->findChild<QLineEdit *>()->setText("3");
  durationSeconds->findChild<QLineEdit *>()->setText("5");
  first->setValue(2);
  second->setValue(1);
  {
    auto settings = vtrpon::makeControllerSettings();
    check(vtrpon::loadOperatorMonitorSettings(settings).timerDurationSeconds == 300, "draft is not saved implicitly");
  }
  save->click();
  dialog.reload();
  check(duration->value() == 3 && durationSeconds->value() == 5 && first->value() == 2 && second->value() == 1, "explicit save reloads timer settings");
  dialog.grab().save(qApp->applicationDirPath() + "/operator-timer-settings.png");
  second->setValue(3);
  save->click();
  dialog.reload();
  check(second->value() == 1, "invalid warning order is rejected");

  duration->setValue(0);
  durationSeconds->setValue(0);
  save->click();
  check(!dialog.findChild<QLabel *>("operatorTimerSettingsStatus")->text().isEmpty(), "zero duration explains validation inline");
  dialog.reload();
  check(duration->value() == 3 && durationSeconds->value() == 5, "zero duration does not replace the saved time");
  check(durationSeconds->minimum() == 0 && durationSeconds->maximum() == 59, "seconds are bounded to one minute");
  for (const int total : {1, 59, 60, 59999, 185}) {
    duration->setValue(total / 60);
    durationSeconds->setValue(total % 60);
    save->click();
    dialog.reload();
    auto settings = vtrpon::makeControllerSettings();
    check(vtrpon::loadOperatorMonitorSettings(settings).timerDurationSeconds == total &&
          duration->value() * 60 + durationSeconds->value() == total,
          "separate minute and second fields preserve the stored duration including boundaries");
  }

  QMenuBar menu;
  QWidget program, controller;
  vtrpon::ViewMenuController view(&menu, &program, &controller);
  QJsonObject last;
  QObject::connect(&view, &vtrpon::ViewMenuController::operatorTimerChanged, &controller,
                   [&](const QJsonObject &value) { last = value; });
  view.startOperatorTimer();
  check(last.value("running").toBool(), "controller gets timer start without a receiver");
  view.setOperatorMonitorCue(QStringLiteral("Cue"));
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < 1100) { qApp->processEvents(); QThread::msleep(10); }
  check(last.value("seconds").toInt() == 184, "timer keeps ticking while cue is sent and receiver closed");
  view.pauseOperatorTimer();
  check(!last.value("running").toBool(), "pause publishes immediately");
  view.resetOperatorTimer();
  check(last.value("seconds").toInt() == 185, "reset restores configured duration");
  enable->setChecked(false);
  view.reloadOperatorMonitorSettings();
  check(!last.value("enabled").toBool() && page.findChild<QCheckBox *>("operatorMonitorField_rate")->isEnabled(),
        "disable restores normal information controls");

  view.showOperatorTimerSettings();
  auto *window = controller.findChild<QDialog *>("operatorTimerSettingsDialog");
  check(window && window->isVisible() && !window->isModal() && !qApp->activeModalWidget(),
        "dedicated timer settings is a nonmodal window");
  auto *windowDuration = window->findChild<QSpinBox *>("operatorMonitorTimerDurationMinutes");
  auto *windowSeconds = window->findChild<QSpinBox *>("operatorMonitorTimerDurationSeconds");
  windowDuration->setValue(4);
  windowSeconds->setValue(0);
  view.showOperatorTimerSettings();
  check(controller.findChildren<QDialog *>("operatorTimerSettingsDialog").size() == 1 &&
        windowDuration->value() == 4 && windowSeconds->value() == 0, "repeated open keeps one window and unsaved input");
  information->setChecked(false);
  view.reloadOperatorMonitorSettings();
  check(windowDuration->value() == 4 && windowSeconds->value() == 0, "display settings changes preserve the timer draft");
  auto *cueInput = window->findChild<QLineEdit *>("operatorMonitorCueInput");
  cueInput->setText(QStringLiteral("  Updated cue  "));
  window->findChild<QPushButton *>("operatorMonitorCueSend")->click();
  check(view.operatorMonitorCue() == "Updated cue", "dedicated window sends cue to the return monitor");
  window->close();
  view.showOperatorTimerSettings();
  check(cueInput->text() == "Updated cue" && windowDuration->value() == 3 && windowSeconds->value() == 5,
        "reopening restores saved time and current cue");
  windowDuration->setValue(3);
  windowSeconds->setValue(15);
  window->findChild<QPushButton *>("operatorMonitorTimerSave")->click();
  check(view.operatorTimerPresentation().value("seconds").toInt() == 195 &&
        !view.operatorTimerPresentation().value("enabled").toBool(), "save updates the timer without enabling it");
  information->setChecked(true);
  {
    auto settings = vtrpon::makeControllerSettings();
    check(vtrpon::loadOperatorMonitorSettings(settings).timerDurationSeconds == 195,
          "monitor settings do not overwrite separately saved time");
  }
  window->findChild<QPushButton *>("operatorMonitorCueClear")->click();
  check(view.operatorMonitorCue().isEmpty(), "dedicated window clears the cue");
  window->close();

  QTranslator english;
  check(english.load(qApp->applicationDirPath() + "/vtrpon_en.qm"), "English timer catalog");
  qApp->installTranslator(&english);
  {
    OperatorTimerSettingsDialog largeDialog;
    largeDialog.setStyleSheet(largeDialog.styleSheet() + QStringLiteral(
        "QWidget#operatorTimerSettingsDialog QWidget { font-size: 24px; }"));
    largeDialog.resize(largeDialog.sizeHint());
    largeDialog.grab().save(qApp->applicationDirPath() + "/return-timer-settings-en-large.png");
    check(largeDialog.windowTitle() == "Return Timer Settings", "dedicated English window title");
    for (auto *spin : largeDialog.findChildren<QSpinBox *>()) {
      check(spin->fontMetrics().horizontalAdvance(QString::number(spin->maximum()) + spin->suffix()) <= spin->width() - 8 &&
            largeDialog.rect().contains(QRect(spin->mapTo(&largeDialog, QPoint()), spin->size())),
            "large-font English time values fit the compact inputs");
    }
    for (auto *button : largeDialog.findChildren<QPushButton *>()) {
      check(button->font().pixelSize() == 24, "large-font fixture is applied");
      check(largeDialog.rect().contains(QRect(button->mapTo(&largeDialog, QPoint()), button->size())) &&
            button->fontMetrics().horizontalAdvance(button->text()) <= button->width() - 8,
            "large-font English controls fit the settings window");
    }
  }
  qApp->removeTranslator(&english);

  config.timerEnabled = true;
  timer.configure(config, 0);
  timer.start(0);
  OperatorMonitorWidget monitor;
  monitor.resize(960, 540);
  QJsonObject presentation{{"timer", timer.presentation(125000)}};
  monitor.setPresentation(presentation);
  const auto rendered = monitor.grab().toImage();
  check(rendered.pixelColor(15, 450) == Qt::black, "receiver warning keeps the band background black");
  int warningPixels = 0;
  QRect warningBounds;
  for (int y = monitor.informationRect().top(); y < rendered.height(); ++y)
    for (int x = 0; x < rendered.width(); ++x)
      if (rendered.pixelColor(x, y) == config.timerWarning2Color) {
        ++warningPixels;
        warningBounds = warningBounds.united(QRect(x, y, 1, 1));
      }
  check(warningPixels > 100 && warningPixels < 2000 && warningBounds.width() < rendered.width() / 2,
        "receiver warning is a thin outline around the timer text only");
  rendered.save(qApp->applicationDirPath() + "/operator-timer-warning.png");
  for (const auto &text : {QStringLiteral("01:00"), QStringLiteral("55:55"),
                           QStringLiteral("99:59"), vtrpon::operatorTimerText(-6000)}) {
    auto state = timer.presentation(125000);
    state.insert("text", text);
    presentation.insert("timer", state);
    monitor.setPresentation(presentation);
    const auto changed = monitor.grab().toImage();
    bool identicalOutline = true;
    for (int y = monitor.informationRect().top(); y < rendered.height(); ++y)
      for (int x = 0; x < rendered.width(); ++x)
        identicalOutline &= (changed.pixelColor(x, y) == config.timerWarning2Color) ==
                            (rendered.pixelColor(x, y) == config.timerWarning2Color);
    check(identicalOutline, "receiver frame is identical across narrow, wide and overtime digits");
  }
  presentation.insert("cue", QStringLiteral("Cue card"));
  monitor.setPresentation(presentation);
  const auto cue = monitor.grab().toImage();
  presentation.remove("timer");
  monitor.setPresentation(presentation);
  check(cue == monitor.grab().toImage(), "cue fully replaces timer including warning outline");
  std::cout << "Operator countdown: " << (ok ? "PASS" : "FAIL") << std::endl;
  return ok;
}
