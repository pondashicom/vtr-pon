#pragma once
#include "operator_countdown_timer.h"
#include "operator_timer_settings_dialog.h"
#include "controller_scaling.h"
#include "program_panel.h"
#include "view_menu_controller.h"
#include "controller_settings.h"
#include "audio_fader_widget.h"
#include "hardware_panel_widget.h"
#include "skins/controller_skin.h"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTranslator>
#include <QImage>
#include <QMenuBar>
#include <QScopeGuard>
#include <iostream>

// Widget geometry is in logical pixels; Retina grabs contain device pixels.
inline QImage operatorTimerLogicalImage(QWidget &widget) {
  auto image = widget.grab().toImage();
  image = image.scaled(widget.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
  image.setDevicePixelRatio(1);
  return image;
}

inline bool runOperatorTimerPanelTest() {
  bool ok = true;
  const auto check = [&](bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ok = false; }
  };
  {
    auto config = vtrpon::makeControllerSettings();
    const auto saved = vtrpon::loadOperatorMonitorSettings(config);
    auto state = saved;
    state.timerEnabled = false;
    vtrpon::saveOperatorMonitorSettings(config, state);
    const auto restore = qScopeGuard([&] { vtrpon::saveOperatorMonitorSettings(config, saved); });
    QWidget controllerWindow, programWindow;
    QMenuBar menu;
    ControllerOperations operations;
    vtrpon::ViewMenuController view(&menu, &programWindow, &controllerWindow);
    view.bindOperations(&operations);
    int notifications = 0;
    QObject::connect(&operations, &ControllerOperations::operationAboutToExecute, &menu,
        [&](ControllerOperations::Action, const QVariantList &, const QString &) { ++notifications; });
    using A = ControllerOperations::Action;
    for (const auto action : {A::OperatorTimerStart, A::OperatorTimerPause,
                             A::OperatorTimerReset, A::OperatorTimerSettings})
      check(operations.executeFrom("Shortcut", action).status == ControllerOperations::Status::Rejected,
            "disabled timer operation is rejected");
    check(notifications == 0, "disabled timer does not notify manual intervention");
    state.timerEnabled = true;
    vtrpon::saveOperatorMonitorSettings(config, state);
    view.reloadOperatorMonitorSettings();
    check(operations.executeFrom("Shortcut", A::OperatorTimerStart).invoked() &&
          view.operatorTimerPresentation().value("running").toBool(),
          "common timer starts without a shown button");
    check(operations.executeFrom("Shortcut", A::OperatorTimerPause).invoked() &&
          !view.operatorTimerPresentation().value("running").toBool(), "common timer pauses");
    check(operations.executeFrom("Shortcut", A::OperatorTimerReset).invoked() &&
          !view.operatorTimerPresentation().value("started").toBool(), "common timer resets");
    check(notifications == 3, "timer has one common notification per operation");
  }
  QTranslator translator;
  check(translator.load(qApp->applicationDirPath() + "/vtrpon_ja.qm"), "timer UI Japanese catalog");
  qApp->installTranslator(&translator);
  const auto previousSkin = vtrpon::currentControllerSkinKey();
  const auto previousPalette = qApp->palette();
  const auto previousStyle = qApp->styleSheet();
  OperatorTimerSettingsDialog timerSettings;
  timerSettings.setCueText(QStringLiteral("スキン切り替え中のカンペ"));
  auto *settingsDuration = timerSettings.findChild<QSpinBox *>("operatorMonitorTimerDurationMinutes");
  auto *settingsSeconds = timerSettings.findChild<QSpinBox *>("operatorMonitorTimerDurationSeconds");
  settingsDuration->setValue(3);
  settingsSeconds->setValue(5);
  for (const auto &skin : {QStringLiteral("hardware"), QStringLiteral("material-dark"), QStringLiteral("vtr-pon-clasic")}) {
    check(vtrpon::applyControllerSkin(*qApp, skin), "timer UI skin applied");
    timerSettings.show();
    qApp->processEvents();
    const auto settingsImage = operatorTimerLogicalImage(timerSettings);
    settingsImage.save(qApp->applicationDirPath() + "/return-timer-settings-" + skin + ".png");
    QWidget backgroundReference;
    backgroundReference.setObjectName(QStringLiteral("controllerWindow"));
    backgroundReference.resize(timerSettings.size());
    const QPoint backgroundSample(8, timerSettings.height() - 8);
    check(settingsImage.pixelColor(backgroundSample) == operatorTimerLogicalImage(backgroundReference).pixelColor(backgroundSample),
          "timer settings background follows the selected controller skin");
    auto *saveSettings = timerSettings.findChild<QPushButton *>("operatorMonitorTimerSave");
    QPushButton buttonReference;
    buttonReference.setProperty("operatorTimerButton", true);
    buttonReference.resize(saveSettings->size());
    check(operatorTimerLogicalImage(*saveSettings).pixelColor(8, 5) == operatorTimerLogicalImage(buttonReference).pixelColor(8, 5),
          "timer settings buttons follow the selected controller skin");
    check(saveSettings->width() < timerSettings.width() / 2, "save button uses a normal button width");
    QSpinBox inputReference;
    inputReference.setObjectName(QStringLiteral("editRepeatCountSpinBox"));
    inputReference.setButtonSymbols(QAbstractSpinBox::NoButtons);
    inputReference.resize(settingsDuration->size());
    const QPoint inputSample(settingsDuration->width() / 2, 4);
    check(operatorTimerLogicalImage(*settingsDuration).pixelColor(inputSample) == operatorTimerLogicalImage(inputReference).pixelColor(inputSample),
          "timer settings inputs follow the selected controller skin");
    for (const auto &name : {QStringLiteral("operatorMonitorTimerDurationMinutes"),
                             QStringLiteral("operatorMonitorTimerDurationSeconds"),
                             QStringLiteral("operatorMonitorTimerWarning1"),
                             QStringLiteral("operatorMonitorTimerWarning2")}) {
      auto *spin = timerSettings.findChild<QSpinBox *>(name);
      auto *up = timerSettings.findChild<QToolButton *>(name + "IncreaseButton");
      auto *down = timerSettings.findChild<QToolButton *>(name + "DecreaseButton");
      check(spin->buttonSymbols() == QAbstractSpinBox::NoButtons && up && down &&
            up->arrowType() == Qt::UpArrow && down->arrowType() == Qt::DownArrow,
            "timer fields use the same stacked arrows as existing duration controls");
      QToolButton arrowReference;
      arrowReference.setProperty("fadeDurationStepButton", true);
      arrowReference.setArrowType(Qt::UpArrow);
      arrowReference.resize(vtrpon::scaleSize(18, 12));
      check(up->size() == arrowReference.size() &&
            operatorTimerLogicalImage(*up).convertToFormat(QImage::Format_ARGB32) ==
                operatorTimerLogicalImage(arrowReference).convertToFormat(QImage::Format_ARGB32),
            "timer step button geometry and skin match existing duration controls");
      check(spin->width() < timerSettings.width() / 3, "timer input width stays compact");
      const int before = spin->value();
      up->click();
      check(spin->value() == before + 1, "timer up arrow increments by one second or minute");
      down->click();
      check(spin->value() == before, "timer down arrow restores the value");
      check(!up->accessibleName().isEmpty() && !down->toolTip().isEmpty(), "timer step buttons are labeled");
    }
    check(settingsDuration->value() == 3 && settingsSeconds->value() == 5 &&
          timerSettings.findChild<QLineEdit *>("operatorMonitorCueInput")->text() == QStringLiteral("スキン切り替え中のカンペ"),
          "changing skins preserves unsaved timer and cue input");
    for (auto *button : timerSettings.findChildren<QPushButton *>())
      check(timerSettings.rect().contains(QRect(button->mapTo(&timerSettings, QPoint()), button->size())) &&
            button->fontMetrics().horizontalAdvance(button->text()) <= button->width() - 8,
            "timer settings controls fit every skin");
    ProgramPanel panel;
    panel.setBgmControlsVisible(true);
    panel.resize(panel.sizeHint());
    panel.grab();
    auto *meter = panel.findChild<QWidget *>("programItemAudioMeter");
    auto *master = panel.findChild<QWidget *>("programAudioMeter");
    auto *module = panel.findChild<QWidget *>("operatorTimerControlModule");
    auto *start = panel.findChild<QPushButton *>("operatorTimerStart");
    auto *pause = panel.findChild<QPushButton *>("operatorTimerPause");
    auto *reset = panel.findChild<QPushButton *>("operatorTimerReset");
    auto *settingsButton = panel.findChild<QPushButton *>("operatorTimerSettingsButton");
    auto *label = panel.findChild<QLabel *>("operatorTimerRemaining");
    const int normalHeight = meter->height();
    check(module->isHidden(), "timer module initially hidden");
    vtrpon::OperatorMonitorSettings settings;
    settings.timerEnabled = true;
    vtrpon::OperatorCountdownTimer timer;
    timer.configure(settings, 0);
    panel.setOperatorTimerPresentation(timer.presentation(0));
    panel.setPlaybackState(false, false);
    check(start->isEnabled() && pause->isEnabled() && reset->isEnabled(), "all timer controls work before ON AIR");
    const auto ready = operatorTimerLogicalImage(panel);
    const auto surface = [](QPushButton *button) { return operatorTimerLogicalImage(*button).pixelColor(8, 5); };
    const auto green = [&](QPushButton *button) {
      const auto color = surface(button);
      return color.green() > color.red() + 20 && color.green() > color.blue() + 20;
    };
    const auto grayStart = surface(start);
    const auto grayPause = surface(pause);
    const auto grayReset = surface(reset);
    const auto normalTimeImage = operatorTimerLogicalImage(*module).copy(
        QRect(label->mapTo(module, QPoint()), label->size()));
    const QColor timeBackground = normalTimeImage.pixelColor(4, label->height() / 2);
    auto *remaining = panel.findChild<QLabel *>("programRemainingTime");
    check(label->font().family() == remaining->font().family() &&
          label->font().weight() == remaining->font().weight() &&
          label->font().pointSizeF() <= remaining->font().pointSizeF() &&
          label->fontMetrics().height() <= label->height() - 2 * vtrpon::scalePx(2),
          "return timer retains the remaining timer typeface within its compact height");
    if (skin == QStringLiteral("material-dark")) {
      const auto remainingImage = operatorTimerLogicalImage(panel).copy(
          QRect(remaining->mapTo(&panel, QPoint()), remaining->size()));
      check(timeBackground == remainingImage.pixelColor(4, remaining->height() / 2) &&
            normalTimeImage.pixelColor(label->width() / 2, 0) == timeBackground,
            "Material Dark timer matches the remaining timer without a dark frame");
    } else {
      check(timeBackground.red() < 40 && timeBackground.green() < 40 && timeBackground.blue() < 40,
            "Hardware and Clasic retain the dark inset time display");
    }
    check(settingsButton->text() == QStringLiteral("設定") && settingsButton->width() == reset->width() &&
          settingsButton->geometry().right() == reset->geometry().right(), "settings and reset buttons have matching widths and alignment");
    ready.save(qApp->applicationDirPath() + "/operator-timer-panel-" + skin + ".png");
    if (skin == QStringLiteral("hardware")) {
      // Same height avoids sampling different positions along the gradient.
      QPushButton reference;
      reference.setProperty("bgmTransportButton", true);
      reference.resize(start->size());
      const auto expected = operatorTimerLogicalImage(reference);
      const auto actual = operatorTimerLogicalImage(*start);
      check(actual.pixelColor(8, 5) == expected.pixelColor(8, 5) &&
            actual.pixelColor(8, actual.height() - 5) == expected.pixelColor(8, expected.height() - 5),
            "Hardware timer surface matches BGM transport buttons");
    }
    check(!module->isHidden() && meter->height() < normalHeight && meter->height() == master->height(), "both meters compress for timer");
    check(module->mapTo(&panel, QPoint()).y() + module->height() <=
          panel.findChild<QWidget *>("programActionPanel")->y(), "timer fits above transport and FTB");
    check(label->geometry().right() < settingsButton->geometry().left() &&
          qAbs(label->geometry().center().y() - settingsButton->geometry().center().y()) <= 1,
          "timer sits on the left with settings alongside it");
    for (auto *button : {start, pause, reset, settingsButton}) {
      check(module->rect().contains(button->mapTo(module, QPoint()) + button->rect().bottomRight()), "timer buttons fit module");
      check(button->fontMetrics().horizontalAdvance(button->text()) <= button->width() - 4, "timer button label fits");
    }
    int starts = 0, pauses = 0, resets = 0, mediaActions = 0;
    int settingsRequests = 0;
    QObject::connect(&panel, &ProgramPanel::operatorTimerSettingsRequested, &panel, [&] { ++settingsRequests; });
    settingsButton->click();
    check(settingsRequests == 1, "timer settings entry works before ON AIR");
    QObject::connect(&panel, &ProgramPanel::operatorTimerStartRequested, &panel, [&] { ++starts; });
    QObject::connect(&panel, &ProgramPanel::operatorTimerPauseRequested, &panel, [&] { ++pauses; });
    QObject::connect(&panel, &ProgramPanel::operatorTimerResetRequested, &panel, [&] { ++resets; });
    QObject::connect(&panel, &ProgramPanel::playRequested, &panel, [&] { ++mediaActions; });
    QObject::connect(&panel, &ProgramPanel::pauseRequested, &panel, [&] { ++mediaActions; });
    start->click();
    timer.start(0);
    panel.setOperatorTimerPresentation(timer.presentation(180000));
    check(start->isEnabled() && pause->isEnabled() && reset->isEnabled() && label->text() == "02:00", "running controls and remaining time");
    check(green(start) && surface(pause) == grayPause && surface(reset) == grayReset,
          "only Start is green while the timer runs");
    const auto warning = operatorTimerLogicalImage(*module);
    HardwarePanelWidget panelReference(HardwarePanelWidget::Layout::SinglePlate);
    panelReference.setPalette(module->palette());
    panelReference.resize(module->size());
    check(warning.pixelColor(8, 8) == operatorTimerLogicalImage(panelReference).pixelColor(8, 8),
          "timer frame matches the shared control modules in every skin");
    const auto timerLabelImage = warning.copy(QRect(label->mapTo(module, QPoint()), label->size()));
    check(timerLabelImage.pixelColor(timerLabelImage.width() / 2, 0) == settings.timerWarning1Color &&
          timerLabelImage.pixelColor(4, timerLabelImage.height() / 2) == timeBackground &&
          label->width() < module->width() * 3 / 4,
          "controller warning outlines the time only without filling it");
    warning.save(qApp->applicationDirPath() + "/operator-timer-control-warning-" + skin + ".png");
    const QRect fixedFrame = label->geometry();
    for (const auto &text : {QStringLiteral("01:00"), QStringLiteral("55:55"),
                             QStringLiteral("99:59"), vtrpon::operatorTimerText(-6000)}) {
      auto state = timer.presentation(180000);
      state.insert("text", text);
      panel.setOperatorTimerPresentation(state);
      module->grab();
      check(label->geometry() == fixedFrame, "controller timer rectangle stays fixed as digits change");
      check(label->fontMetrics().horizontalAdvance(text) <= label->width() - 16,
            "normal and overtime digits fit the fixed controller frame");
    }
    pause->click();
    timer.pause(180000);
    panel.setOperatorTimerPresentation(timer.presentation(180000));
    check(start->isEnabled() && pause->isEnabled() && reset->isEnabled(), "all controls remain available while paused");
    check(surface(start) == grayStart && green(pause) && surface(reset) == grayReset,
          "only Pause is green while the timer is paused");
    module->grab().save(qApp->applicationDirPath() + "/operator-timer-control-paused-" + skin + ".png");
    reset->click();
    timer.reset(180000);
    panel.setOperatorTimerPresentation(timer.presentation(180000));
    check(surface(start) == grayStart && surface(pause) == grayPause && surface(reset) == grayReset,
          "Reset restores every timer button to gray");
    check(starts == 1 && pauses == 1 && resets == 1 && mediaActions == 0, "timer operations are independent of media controls");
    settings.timerEnabled = false;
    timer.configure(settings, 180000);
    panel.setOperatorTimerPresentation(timer.presentation(180000));
    panel.grab();
    check(module->isHidden() && meter->height() == normalHeight, "disable restores full meter height");
  }
  timerSettings.close();
  qApp->removeTranslator(&translator);
  QTranslator english;
  check(english.load(qApp->applicationDirPath() + "/vtrpon_en.qm"), "English panel catalog");
  qApp->installTranslator(&english);
  const auto previousScale = vtrpon::uiScale();
  vtrpon::setUiScale(0.75);
  vtrpon::applyControllerSkin(*qApp, QStringLiteral("hardware"));
  {
    ProgramPanel panel;
    vtrpon::OperatorMonitorSettings settings;
    settings.timerEnabled = true;
    vtrpon::OperatorCountdownTimer timer;
    timer.configure(settings, 0);
    panel.setOperatorTimerPresentation(timer.presentation(0));
    panel.resize(panel.sizeHint());
    panel.grab();
    auto *module = panel.findChild<QWidget *>("operatorTimerControlModule");
    auto *button = panel.findChild<QPushButton *>("operatorTimerSettingsButton");
    auto *label = panel.findChild<QLabel *>("operatorTimerRemaining");
    check(button->text() == "Settings" &&
          button->fontMetrics().horizontalAdvance(button->text()) <= button->width() - 4 &&
          label->geometry().right() < button->geometry().left() &&
          module->rect().contains(button->geometry()) &&
          button->width() == panel.findChild<QPushButton *>("operatorTimerReset")->width(),
          "English settings button matches Reset and fits at 75 percent UI scale");
    module->grab().save(qApp->applicationDirPath() + "/return-timer-panel-en-75.png");
  }
  qApp->removeTranslator(&english);
  vtrpon::setUiScale(previousScale);
  vtrpon::applyControllerSkin(*qApp, previousSkin);
  qApp->setPalette(previousPalette);
  qApp->setStyleSheet(previousStyle);
  std::cout << "Operator timer panel: " << (ok ? "PASS" : "FAIL") << std::endl;
  return ok;
}
