#pragma once

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <QStringList>
#include "operator_monitor_settings.h"
#include "operator_countdown_timer.h"
#include <QElapsedTimer>

class QAction;
class QActionGroup;
class QMenu;
class QMenuBar;
class QScreen;
class QProcess;
class QWidget;
class OperatorTimerSettingsDialog;
class ControllerOperations;

namespace vtrpon {
class OperatorMonitorControlFeed;

class ViewMenuController final : public QObject {
  Q_OBJECT
public:
  ViewMenuController(QMenuBar *menuBar, QWidget *programWindow,
                     QWidget *controllerWindow, QObject *parent = nullptr);
  ~ViewMenuController() override;

  void retranslate();
  void cycleProgramDisplay();
  void bindOperations(ControllerOperations *operations);
  QWidget *createSettingsWidget(QWidget *parent);
  void reloadOperatorMonitorSettings();
  void requestShutdown();
  bool shutdownComplete() const;
  void setOperatorMonitorCue(const QString &text);
  QString operatorMonitorCue() const { return operatorMonitorCue_; }
  std::function<QStringList()> operatorMonitorStatusProvider;
  QJsonObject operatorTimerPresentation() const;
  void startOperatorTimer();
  void pauseOperatorTimer();
  void resetOperatorTimer();
  void showOperatorTimerSettings();

signals:
  void operatorMonitorUnavailable();
  void operatorTimerChanged(const QJsonObject &presentation);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void publishOperatorMonitorPresentation();
  void updateOperatorTimer();
  QElapsedTimer operatorTimerClock_;
  OperatorCountdownTimer operatorTimer_;
  QJsonObject lastOperatorTimerPresentation_;
  OperatorMonitorSettings operatorMonitorSettings_;
  QString operatorMonitorCue_;
  QPointer<OperatorTimerSettingsDialog> operatorTimerSettingsDialog_;
#ifdef _WIN32
  std::unique_ptr<OperatorMonitorControlFeed> operatorMonitorFeed_;
#endif
  void rebuildDisplayTargetMenu();
  void syncActionStates();
  bool canUseProgramAlwaysOnTop() const;
  void restoreControllerAccess();
  void setProgramAlwaysOnTop(bool enabled);
  void minimizeProgramWindow();
  void showProgramWindowOnScreen(QScreen *screen);
  void setControllerFullScreen(bool enabled);

  QPointer<QWidget> programWindow_;
  QPointer<QWidget> controllerWindow_;
  QPointer<QMenu> viewMenu_;
  QPointer<QMenu> programWindowMenu_;
  QPointer<QMenu> displayTargetMenu_;
  QPointer<QMenu> controllerWindowMenu_;
  QPointer<QAction> programAlwaysOnTopAction_;
  QPointer<QAction> programMinimizeAction_;
  QPointer<QAction> controllerFullScreenAction_;
  QPointer<QAction> operatorMonitorAction_;
  QProcess *operatorMonitorProcess_ = nullptr;
  bool operatorMonitorClosing_ = false;
  QActionGroup *displayTargetActionGroup_ = nullptr;
};

} // namespace vtrpon
