#include "settings_style.h"
#include "view_menu_controller.h"
#include "controller_operations.h"

#include "language_text.h"
#include "controller_scaling.h"
#include "controller_settings.h"
#include "video_output_settings.h"
#include "operator_timer_settings_dialog.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QSignalBlocker>
#include <QWidget>
#include <QWindow>
#include <QProcess>
#include <QDir>

#ifdef _WIN32
#include <windows.h>
#include "operator_monitor_output.h"
#endif

namespace {

QScreen *screenForWindow(QWidget *window) {
  if (!window) {
    return nullptr;
  }
  if (QWindow *windowHandle = window->windowHandle()) {
    if (QScreen *screen = windowHandle->screen()) {
      return screen;
    }
  }
  return window->screen();
}

void placeWindow(QWidget *window, QScreen *screen, const QRect &geometry) {
  if (!window || !screen) {
    return;
  }

  window->winId();
  if (QWindow *windowHandle = window->windowHandle()) {
    windowHandle->setScreen(screen);
    windowHandle->setGeometry(geometry);
  }
  window->setGeometry(geometry);
}

bool isWindowAlwaysOnTop(QWidget *window) {
  if (!window) {
    return false;
  }
#ifdef _WIN32
  const auto nativeWindow = reinterpret_cast<HWND>(window->winId());
  if (!IsWindow(nativeWindow)) {
    return false;
  }
  return (GetWindowLongPtrW(nativeWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
#else
  return window->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
#endif
}

bool setWindowAlwaysOnTop(QWidget *window, bool enabled) {
  if (!window) {
    return false;
  }
#ifdef _WIN32
  const auto nativeWindow = reinterpret_cast<HWND>(window->winId());
  if (!IsWindow(nativeWindow)) {
    return false;
  }
  return SetWindowPos(nativeWindow, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                      0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                          SWP_NOOWNERZORDER) != FALSE;
#else
  const bool wasVisible = window->isVisible();
  const bool wasMinimized = window->isMinimized();
  const QRect geometry = window->geometry();
  QScreen *screen = screenForWindow(window);
  window->setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
  if (wasVisible) {
    if (wasMinimized) {
      window->showMinimized();
    } else {
      window->show();
      placeWindow(window, screen, geometry);
      if (enabled) {
        window->raise();
      }
    }
  }
  return true;
#endif
}

} // namespace

namespace vtrpon {

ViewMenuController::ViewMenuController(QMenuBar *menuBar,
                                       QWidget *programWindow,
                                       QWidget *controllerWindow,
                                       QObject *parent)
    : QObject(parent), programWindow_(programWindow),
      controllerWindow_(controllerWindow),
      displayTargetActionGroup_(new QActionGroup(this)) {
  displayTargetActionGroup_->setExclusive(true);

  if (!menuBar) {
    return;
  }

  viewMenu_ = menuBar->addMenu(QString());
  viewMenu_->setObjectName(QStringLiteral("viewMenu"));

  programWindowMenu_ = viewMenu_->addMenu(QString());
  programWindowMenu_->setObjectName(QStringLiteral("programWindowMenu"));
  programAlwaysOnTopAction_ = programWindowMenu_->addAction(QString());
  programAlwaysOnTopAction_->setObjectName(
      QStringLiteral("programAlwaysOnTopAction"));
  programAlwaysOnTopAction_->setCheckable(true);
  programMinimizeAction_ = programWindowMenu_->addAction(QString());
  programMinimizeAction_->setObjectName(
      QStringLiteral("programMinimizeAction"));
  programWindowMenu_->addSeparator();
  displayTargetMenu_ = programWindowMenu_->addMenu(QString());
  displayTargetMenu_->setObjectName(QStringLiteral("displayTargetMenu"));

  controllerWindowMenu_ = viewMenu_->addMenu(QString());
  controllerWindowMenu_->setObjectName(QStringLiteral("controllerWindowMenu"));
  controllerFullScreenAction_ = controllerWindowMenu_->addAction(QString());
  controllerFullScreenAction_->setObjectName(
      QStringLiteral("controllerFullScreenAction"));
  controllerFullScreenAction_->setCheckable(true);

  operatorTimerClock_.start();
  reloadOperatorMonitorSettings();
  auto *countdownTick = new QTimer(this);
  connect(countdownTick, &QTimer::timeout, this, &ViewMenuController::updateOperatorTimer);
  countdownTick->start(50);
#ifdef _WIN32
  operatorMonitorAction_ = viewMenu_->addAction(QString());
  operatorMonitorAction_->setObjectName(QStringLiteral("operatorMonitorAction"));
  operatorMonitorAction_->setCheckable(true);
  operatorMonitorProcess_ = new QProcess(this);
  auto *operatorInfoTimer = new QTimer(this);
  connect(operatorInfoTimer, &QTimer::timeout, this, [this] {
    if (operatorMonitorProcess_->state() == QProcess::Running)
      publishOperatorMonitorPresentation();
  });
  operatorInfoTimer->start(100);
  operatorMonitorProcess_->setStandardOutputFile(QProcess::nullDevice());
  operatorMonitorProcess_->setStandardErrorFile(QProcess::nullDevice());
  const auto resetMonitor = [this] {
    setOperatorMonitorEnabled(false);
    const QSignalBlocker blocker(operatorMonitorAction_);
    operatorMonitorAction_->setChecked(false);
    operatorMonitorAction_->setEnabled(!operatorMonitorClosing_);
  };
  connect(operatorMonitorProcess_, &QProcess::finished, this,
          [this, resetMonitor](int exitCode, QProcess::ExitStatus status) {
    const bool unexpected = operatorMonitorAction_->isChecked() &&
        (exitCode != 0 || status == QProcess::CrashExit);
    resetMonitor();
    if (unexpected) emit operatorMonitorUnavailable();
  });
  connect(operatorMonitorProcess_, &QProcess::started, this, [this] {
    operatorMonitorAction_->setEnabled(!operatorMonitorClosing_);
  });
  connect(operatorMonitorProcess_, &QProcess::errorOccurred, this,
          [this, resetMonitor](QProcess::ProcessError error) {
    qWarning() << "Operator monitor process error" << error;
    if (error == QProcess::FailedToStart) {
      resetMonitor();
      emit operatorMonitorUnavailable();
    }
  });
  connect(operatorMonitorAction_, &QAction::toggled, this, [this](bool enabled) {
    if (operatorMonitorClosing_) return;
    if (!enabled) {
      setOperatorMonitorEnabled(false);
      operatorMonitorAction_->setEnabled(false);
      operatorMonitorProcess_->kill(); // Only the disposable receiver is stopped.
      return;
    }
    setOperatorMonitorEnabled(true);
    if (!operatorMonitorFeed_) operatorMonitorFeed_ = std::make_unique<OperatorMonitorControlFeed>();
    publishOperatorMonitorPresentation();
    operatorMonitorAction_->setEnabled(false);
    operatorMonitorProcess_->start(
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("vtr-pon-operator-monitor.exe")),
        {operatorMonitorChannelName(),
         QString::number(QCoreApplication::applicationPid()),
         uiText("menu.view.operator_monitor")});
  });
#endif

  connect(programAlwaysOnTopAction_, &QAction::toggled, this,
          &ViewMenuController::setProgramAlwaysOnTop);
  connect(programMinimizeAction_, &QAction::triggered, this,
          &ViewMenuController::minimizeProgramWindow);
  connect(controllerFullScreenAction_, &QAction::toggled, this,
          &ViewMenuController::setControllerFullScreen);
  connect(programWindowMenu_, &QMenu::aboutToShow, this,
          &ViewMenuController::syncActionStates);
  connect(controllerWindowMenu_, &QMenu::aboutToShow, this,
          &ViewMenuController::syncActionStates);
  connect(displayTargetMenu_, &QMenu::aboutToShow, this,
          &ViewMenuController::rebuildDisplayTargetMenu);

  if (auto *application =
          qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
    connect(application, &QGuiApplication::screenAdded, this,
            [this](QScreen *) { rebuildDisplayTargetMenu(); syncActionStates(); });
    connect(application, &QGuiApplication::screenRemoved, this,
            [this](QScreen *) { rebuildDisplayTargetMenu(); syncActionStates(); });
  }

  if (programWindow_) {
    programWindow_->installEventFilter(this);
  }
  if (controllerWindow_) {
    controllerWindow_->installEventFilter(this);
  }
  for (QWidget *window : {programWindow_.data(), controllerWindow_.data()}) {
    if (!window) continue;
    window->winId();
    connect(window->windowHandle(), &QWindow::screenChanged, this,
            [this](QScreen *) { syncActionStates(); });
  }

  retranslate();
  rebuildDisplayTargetMenu();
  syncActionStates();
}

ViewMenuController::~ViewMenuController() {
  delete operatorTimerSettingsDialog_.data();
#ifdef _WIN32
  requestShutdown();
  if (operatorMonitorProcess_) {
    operatorMonitorProcess_->disconnect(this);
    if (operatorMonitorProcess_->state() != QProcess::NotRunning) {
      // QProcess destruction waits synchronously. Keep it alive independently
      // until the child exits, even when the window bypasses beginClose().
      QProcess *process = operatorMonitorProcess_;
      process->setParent(nullptr);
      QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                       process, [process] { process->deleteLater(); });
    }
  }
#endif
}

void ViewMenuController::requestShutdown() {
#ifdef _WIN32
  if (operatorMonitorClosing_) return;
  operatorMonitorClosing_ = true;
  setOperatorMonitorEnabled(false);
  if (operatorMonitorAction_) {
    const QSignalBlocker blocker(operatorMonitorAction_);
    operatorMonitorAction_->setChecked(false);
    operatorMonitorAction_->setEnabled(false);
  }
  if (operatorMonitorProcess_ && operatorMonitorProcess_->state() != QProcess::NotRunning)
    operatorMonitorProcess_->kill();
#endif
}

bool ViewMenuController::shutdownComplete() const {
#ifdef _WIN32
  return !operatorMonitorProcess_ || operatorMonitorProcess_->state() == QProcess::NotRunning;
#else
  return true;
#endif
}

void ViewMenuController::reloadOperatorMonitorSettings() {
  auto settings = makeControllerSettings();
  const auto previous = operatorMonitorSettings_;
  operatorMonitorSettings_ = loadOperatorMonitorSettings(settings);
  operatorTimer_.configure(operatorMonitorSettings_, operatorTimerClock_.elapsed());
  if (operatorTimerSettingsDialog_ &&
      (previous.timerDurationSeconds != operatorMonitorSettings_.timerDurationSeconds ||
       previous.timerWarning1Minutes != operatorMonitorSettings_.timerWarning1Minutes ||
       previous.timerWarning2Minutes != operatorMonitorSettings_.timerWarning2Minutes ||
       previous.timerWarning1Color != operatorMonitorSettings_.timerWarning1Color ||
       previous.timerWarning2Color != operatorMonitorSettings_.timerWarning2Color))
    operatorTimerSettingsDialog_->reload();
  updateOperatorTimer();
  publishOperatorMonitorPresentation();
}

void ViewMenuController::bindOperations(ControllerOperations *operations) {
  using A = ControllerOperations::Action;
  operations->setGuard<>(A::NextOutputDisplay, this, [this] {
    return programWindow_ && QGuiApplication::screens().size() > 1;
  });
  operations->bindMember(A::NextOutputDisplay, this, &ViewMenuController::cycleProgramDisplay);
  for (auto action : {A::OperatorTimerStart, A::OperatorTimerPause,
                      A::OperatorTimerReset, A::OperatorTimerSettings}) {
    operations->setGuard<>(action, this, [this] {
      return controllerWindow_ && operatorTimerPresentation().value("enabled").toBool();
    });
  }
  operations->bindMember(A::OperatorTimerStart, this, &ViewMenuController::startOperatorTimer);
  operations->bindMember(A::OperatorTimerPause, this, &ViewMenuController::pauseOperatorTimer);
  operations->bindMember(A::OperatorTimerReset, this, &ViewMenuController::resetOperatorTimer);
  operations->bindMember(A::OperatorTimerSettings, this, &ViewMenuController::showOperatorTimerSettings);
}

void ViewMenuController::showOperatorTimerSettings() {
  if (!controllerWindow_) return;
  if (!operatorTimerSettingsDialog_) {
    operatorTimerSettingsDialog_ = new OperatorTimerSettingsDialog(controllerWindow_);
    operatorTimerSettingsDialog_->changed = [this] { reloadOperatorMonitorSettings(); };
    operatorTimerSettingsDialog_->cueSent = [this](const QString &text) { setOperatorMonitorCue(text); };
    operatorTimerSettingsDialog_->cueCleared = [this] { setOperatorMonitorCue({}); };
  }
  if (!operatorTimerSettingsDialog_->isVisible()) {
    operatorTimerSettingsDialog_->reload();
    operatorTimerSettingsDialog_->setCueText(operatorMonitorCue_);
  }
  if (operatorTimerSettingsDialog_->isMinimized()) operatorTimerSettingsDialog_->showNormal();
  else operatorTimerSettingsDialog_->show();
  operatorTimerSettingsDialog_->raise();
  operatorTimerSettingsDialog_->activateWindow();
}

QJsonObject ViewMenuController::operatorTimerPresentation() const {
  return operatorTimer_.presentation(operatorTimerClock_.elapsed());
}

void ViewMenuController::updateOperatorTimer() {
  const auto state = operatorTimerPresentation();
  if (state == lastOperatorTimerPresentation_) return;
  lastOperatorTimerPresentation_ = state;
  emit operatorTimerChanged(state);
  publishOperatorMonitorPresentation();
}

void ViewMenuController::startOperatorTimer() {
  operatorTimer_.start(operatorTimerClock_.elapsed());
  updateOperatorTimer();
}

void ViewMenuController::pauseOperatorTimer() {
  operatorTimer_.pause(operatorTimerClock_.elapsed());
  updateOperatorTimer();
}

void ViewMenuController::resetOperatorTimer() {
  operatorTimer_.reset(operatorTimerClock_.elapsed());
  updateOperatorTimer();
}

void ViewMenuController::setOperatorMonitorCue(const QString &text) {
  operatorMonitorCue_ = text.left(500).trimmed();
  publishOperatorMonitorPresentation();
}

void ViewMenuController::publishOperatorMonitorPresentation() {
#ifdef _WIN32
  if (!operatorMonitorFeed_) return;
  const QStringList status = operatorMonitorStatusProvider ? operatorMonitorStatusProvider() : QStringList();
  QJsonArray fields;
  for (size_t i = 0; i < operatorMonitorFields.size(); ++i) {
    if (!operatorMonitorSettings_.fields[i]) continue;
    if (operatorMonitorSettings_.timerEnabled && i != 4) continue;
    if (i == 4 && status.value(int(i)).isEmpty()) continue;
    const auto key = QByteArray(operatorMonitorFields[i]);
    fields.append(QJsonObject{{"key", QString::fromLatin1(key)},
        {"label", uiText((QByteArray("operator_monitor.") + key).constData())},
        {"value", status.value(int(i), QStringLiteral("—")).left(1024)}});
  }
  const QJsonObject state{{"screen", operatorMonitorSettings_.screen},
      {"information", operatorMonitorSettings_.information}, {"fields", fields},
      {"timer", operatorTimerPresentation()},
      {"cue", operatorMonitorCue_}, {"title", uiText("menu.view.operator_monitor")}};
  operatorMonitorFeed_->publish(QJsonDocument(state).toJson(QJsonDocument::Compact));
#endif
}

QWidget *ViewMenuController::createSettingsWidget(QWidget *parent) {
  auto *widget = new QWidget(parent);
  widget->setObjectName(QStringLiteral("videoOutputControls"));
  auto *layout = new QVBoxLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(scalePx(16));
  auto *programGroup = new QGroupBox(widget);
  auto *programLayout = new QVBoxLayout(programGroup);
  programLayout->setSpacing(scalePx(10));
  auto *topmost = new QCheckBox(programGroup);
  topmost->setObjectName(QStringLiteral("videoOutputAlwaysOnTopCheckBox"));
  auto *minimize = new QPushButton(programGroup);
  minimize->setObjectName(QStringLiteral("videoOutputMinimizeButton"));
  auto *displayLabel = new QLabel(programGroup);
  setSettingsRole(displayLabel, "fieldLabel");
  auto *displays = new QComboBox(programGroup);
  displays->setObjectName(QStringLiteral("videoOutputDisplayComboBox"));
  displayLabel->setBuddy(displays);
  programLayout->addWidget(topmost);
  programLayout->addWidget(minimize, 0, Qt::AlignLeft);
  programLayout->addWidget(displayLabel);
  programLayout->addWidget(displays);
  auto *resolutionLabel = new QLabel(programGroup);
  setSettingsRole(resolutionLabel, "fieldLabel");
  auto *resolution = new QComboBox(programGroup);
  resolution->setObjectName(QStringLiteral("videoOutputResolutionComboBox"));
  resolution->addItem(QStringLiteral("1080p (1920 x 1080)"), false);
  resolution->addItem(QStringLiteral("4K (3840 x 2160)"), true);
  resolutionLabel->setBuddy(resolution);
  auto *resolutionNote = new QLabel(programGroup);
  setSettingsRole(resolutionNote, "description");
  resolutionNote->setObjectName(QStringLiteral("videoOutputResolutionNote"));
  resolutionNote->setWordWrap(true);
  auto *resolutionStatus = new QLabel(programGroup);
  setSettingsRole(resolutionStatus, "status");
  resolutionStatus->setObjectName(QStringLiteral("videoOutputResolutionStatus"));
  resolutionStatus->setWordWrap(true);
  programLayout->addWidget(resolutionLabel);
  programLayout->addWidget(resolution);
  programLayout->addWidget(resolutionNote);
  programLayout->addWidget(resolutionStatus);
#ifndef Q_OS_WIN
  resolution->setEnabled(false);
#endif
  const bool active4k = initializeProgramOutputResolution();
  const auto saveFailed = std::make_shared<bool>(false);
  const auto refreshResolution = [resolution, resolutionStatus, active4k, saveFailed] {
    const auto settings = makeControllerSettings();
    const bool saved4k = load4kProgramOutput(settings);
    const QSignalBlocker blocker(resolution);
    resolution->setCurrentIndex(saved4k ? 1 : 0);
    const QString active = resolution->itemText(active4k ? 1 : 0);
    resolutionStatus->setText(*saveFailed
        ? uiText("settings.video_output.resolution_save_failed")
        : uiText(saved4k == active4k
            ? "settings.video_output.resolution_current"
            : "settings.video_output.resolution_pending").arg(active));
#ifndef Q_OS_WIN
    resolutionStatus->setText(uiText("settings.gpu.windows_only"));
#endif
  };
  connect(resolution, &QComboBox::currentIndexChanged, widget,
          [resolution, refreshResolution, saveFailed] {
    auto settings = makeControllerSettings();
    *saveFailed = !save4kProgramOutput(settings, resolution->currentData().toBool());
    refreshResolution();
  });
  layout->addWidget(programGroup);
  auto *controllerGroup = new QGroupBox(widget);
  auto *controllerLayout = new QVBoxLayout(controllerGroup);
  auto *fullscreen = new QCheckBox(controllerGroup);
  fullscreen->setObjectName(QStringLiteral("videoOutputControllerFullScreenCheckBox"));
  controllerLayout->addWidget(fullscreen);
  layout->addWidget(controllerGroup);
  const auto translate = [this, programGroup, controllerGroup, topmost,
                          minimize, displayLabel, displays, fullscreen,
                          resolutionLabel, resolution, resolutionNote, refreshResolution] {
    programGroup->setTitle(programWindowMenu_->title());
    controllerGroup->setTitle(controllerWindowMenu_->title());
    topmost->setText(programAlwaysOnTopAction_->text());
    minimize->setText(programMinimizeAction_->text());
    displayLabel->setText(displayTargetMenu_->title());
    displays->setAccessibleName(displayLabel->text());
    fullscreen->setText(controllerFullScreenAction_->text());
    resolutionLabel->setText(uiText("settings.video_output.maximum_resolution"));
    resolution->setAccessibleName(resolutionLabel->text());
    resolutionNote->setText(uiText("settings.video_output.resolution_note"));
    refreshResolution();
  };
  for (auto *action : {programWindowMenu_->menuAction(), controllerWindowMenu_->menuAction(),
                      programAlwaysOnTopAction_.data(), programMinimizeAction_.data(),
                      displayTargetMenu_->menuAction(), controllerFullScreenAction_.data()})
    connect(action, &QAction::changed, widget, translate);
  translate();
  // Read actual window state, including changes made outside this page.
  const auto screens = std::make_shared<QList<QPointer<QScreen>>>();
  const auto refresh = [this, topmost, fullscreen, displays, screens, refreshResolution] {
    refreshResolution();
    const QSignalBlocker topmostBlocker(topmost);
    const QSignalBlocker fullscreenBlocker(fullscreen);
    topmost->setChecked(isWindowAlwaysOnTop(programWindow_));
    topmost->setEnabled(canUseProgramAlwaysOnTop());
    fullscreen->setChecked(controllerWindow_ && controllerWindow_->isFullScreen());
    const auto available = QGuiApplication::screens();
    bool changed = screens->size() != available.size();
    for (int i = 0; !changed && i < available.size(); ++i)
      changed = screens->at(i) != available.at(i) || displays->itemText(i) != available.at(i)->name();
    if (changed) {
      const QSignalBlocker blocker(displays);
      displays->clear();
      screens->clear();
      for (auto *screen : available) {
        screens->append(QPointer<QScreen>(screen));
        displays->addItem(screen->name());
      }
    }
    displays->setEnabled(!available.isEmpty());
    const QSignalBlocker blocker(displays);
    displays->setCurrentIndex(available.indexOf(screenForWindow(programWindow_)));
  };
  connect(topmost, &QCheckBox::toggled, this, [this, refresh](bool enabled) {
    setProgramAlwaysOnTop(enabled);
    syncActionStates();
    refresh();
  });
  connect(minimize, &QPushButton::clicked, this, &ViewMenuController::minimizeProgramWindow);
  connect(fullscreen, &QCheckBox::toggled, this, [this, refresh](bool enabled) {
    setControllerFullScreen(enabled);
    syncActionStates();
    refresh();
  });
  connect(displays, &QComboBox::activated, this, [this, screens, refresh](int index) {
    if (index >= 0 && index < screens->size() && screens->at(index))
      showProgramWindowOnScreen(screens->at(index));
    refresh();
  });
  auto *timer = new QTimer(widget);
  timer->setInterval(250);
  connect(timer, &QTimer::timeout, widget, [widget, refresh] {
    if (widget->isVisible()) refresh();
  });
  timer->start();
  refresh();
  return widget;
}

void ViewMenuController::retranslate() {
  if (!viewMenu_) {
    return;
  }
  viewMenu_->setTitle(uiText("menu.view"));
  if (operatorMonitorAction_)
    operatorMonitorAction_->setText(uiText("menu.view.operator_monitor"));
  programWindowMenu_->setTitle(uiText("menu.view.fullscreen"));
  programAlwaysOnTopAction_->setText(
      uiText("menu.view.fullscreen.always_on_top"));
  programMinimizeAction_->setText(uiText("menu.view.fullscreen.minimize"));
  displayTargetMenu_->setTitle(
      uiText("menu.view.fullscreen.display_target_window"));
  controllerWindowMenu_->setTitle(uiText("menu.view.controller"));
  controllerFullScreenAction_->setText(
      uiText("menu.view.controller.fullscreen"));
}

bool ViewMenuController::eventFilter(QObject *watched, QEvent *event) {
  if ((watched == programWindow_ || watched == controllerWindow_) &&
      event->type() == QEvent::WindowStateChange) {
    syncActionStates();
  }
  return QObject::eventFilter(watched, event);
}

void ViewMenuController::rebuildDisplayTargetMenu() {
  if (!displayTargetMenu_) {
    return;
  }

  displayTargetMenu_->clear();
  const QList<QScreen *> screens = QGuiApplication::screens();
  displayTargetMenu_->setEnabled(!screens.isEmpty());
  QScreen *currentScreen = screenForWindow(programWindow_);
  for (QScreen *screen : screens) {
    QAction *action = displayTargetMenu_->addAction(screen->name());
    action->setObjectName(QStringLiteral("displayTargetAction"));
    action->setCheckable(true);
    action->setChecked(screen == currentScreen);
    displayTargetActionGroup_->addAction(action);
    const QPointer<QScreen> guardedScreen(screen);
    connect(action, &QAction::triggered, this, [this, guardedScreen] {
      if (guardedScreen) {
        showProgramWindowOnScreen(guardedScreen);
      }
    });
  }
}

void ViewMenuController::syncActionStates() {
  if (!canUseProgramAlwaysOnTop() && isWindowAlwaysOnTop(programWindow_)) {
    if (setWindowAlwaysOnTop(programWindow_, false)) restoreControllerAccess();
  }
  if (programAlwaysOnTopAction_) {
    const QSignalBlocker blocker(programAlwaysOnTopAction_);
    programAlwaysOnTopAction_->setChecked(isWindowAlwaysOnTop(programWindow_));
    programAlwaysOnTopAction_->setEnabled(canUseProgramAlwaysOnTop());
  }
  if (controllerFullScreenAction_) {
    const QSignalBlocker blocker(controllerFullScreenAction_);
    controllerFullScreenAction_->setChecked(controllerWindow_ &&
                                            controllerWindow_->isFullScreen());
  }
}

bool ViewMenuController::canUseProgramAlwaysOnTop() const {
  return QGuiApplication::screens().size() > 1 && programWindow_ &&
         controllerWindow_ && screenForWindow(programWindow_) &&
         screenForWindow(controllerWindow_) &&
         screenForWindow(programWindow_) != screenForWindow(controllerWindow_);
}

void ViewMenuController::restoreControllerAccess() {
  if (!controllerWindow_ || !controllerWindow_->isVisible()) return;
  controllerWindow_->raise();
  QWidget *target = QApplication::activeModalWidget();
  if (!target) target = controllerWindow_;
  target->raise();
  target->activateWindow();
}

void ViewMenuController::setProgramAlwaysOnTop(bool enabled) {
  enabled = enabled && canUseProgramAlwaysOnTop();
  if (setWindowAlwaysOnTop(programWindow_, enabled)) {
    syncActionStates();
    return;
  }
  qWarning() << "Could not change the ON AIR window always-on-top state.";
  syncActionStates();
}

void ViewMenuController::minimizeProgramWindow() {
  if (programWindow_) {
    programWindow_->showMinimized();
  }
}

void ViewMenuController::cycleProgramDisplay() {
  const auto screens = QGuiApplication::screens();
  if (!programWindow_ || screens.size() < 2) {
    return;
  }
  const auto current = screens.indexOf(screenForWindow(programWindow_));
  showProgramWindowOnScreen(screens.at((current + 1) % screens.size()));
}

void ViewMenuController::showProgramWindowOnScreen(QScreen *screen) {
  if (!programWindow_ || !screen) {
    return;
  }
  // Clear topmost before moving onto the operator's display.
  if (screen == screenForWindow(controllerWindow_) &&
      !setWindowAlwaysOnTop(programWindow_, false)) {
    qWarning() << "Could not safely move the ON AIR window onto the Controller display.";
    return;
  }

  if (programWindow_->isMinimized()) {
    programWindow_->showNormal();
  }
  const QRect geometry = screen->geometry();
  placeWindow(programWindow_, screen, geometry);
  programWindow_->show();
  placeWindow(programWindow_, screen, geometry);
  programWindow_->raise();
  syncActionStates();
  if (screen == screenForWindow(controllerWindow_)) restoreControllerAccess();
}

void ViewMenuController::setControllerFullScreen(bool enabled) {
  if (!controllerWindow_) {
    return;
  }
  if (enabled) {
    controllerWindow_->showFullScreen();
  } else {
    controllerWindow_->showNormal();
  }
}

} // namespace vtrpon
