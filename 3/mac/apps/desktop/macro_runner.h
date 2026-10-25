#pragma once

#include "macro_document.h"
#include "controller_operations.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <optional>

class PlaylistModel;

// Runs on the Controller thread. Media preparation is owned by the existing
// Program controller; only matching request completions advance this runner.
class MacroRunner final : public QObject {
  Q_OBJECT
public:
  enum class State { Stopped, Running, Waiting, Paused, Completed, Error };
  Q_ENUM(State)
  struct Clock {
    std::function<QDateTime()> wallNow;
    std::function<qint64()> monotonicNow;
  };
  MacroRunner(PlaylistModel *model, ControllerOperations *operations,
              Clock clock, QObject *parent = nullptr);
  ~MacroRunner() override;

  State state() const { return state_; }
  bool isActive() const;
  int currentStepIndex() const { return stepIndex_; }
  qint64 remainingMs() const;
  QString statusText() const;
  QStringList logLines() const { return log_; }
  QString activeMacroId() const { return definition_.id; }
  QString activeMacroName() const { return definition_.name; }
  QDateTime currentDateTime() const;
  QStringList validate(const MacroDefinition &definition) const;
  bool start(const MacroDefinition &definition);
  bool applyItemSettings(quint64 itemId, const QJsonObject &patch,
                         QString *error = nullptr);

public slots:
  void pause();
  void resume();
  void stop();
  void poll();
  void finishRequest(quint64 requestId, bool success, const QString &detail);
  void interrupt(const QString &reason);
  void fail(const QString &reason);
  void selectionChanged();

signals:
  void stateChanged();
  void progressChanged();
  void logAdded(const QString &line);

private:
  void setState(State state);
  void appendLog(const QString &result);
  void advance();
  void cancelPending();
  void executeStep();
  bool checkTarget(const MacroStep &step, QString *error, bool checkSettings = true) const;
  bool referencesItem(quint64 itemId) const;
  void itemChanged(quint64 itemId);
  void refreshObservedItems();
  void beforeOperation(ControllerOperations::Action action,
                       const QVariantList &arguments, const QString &source);
  qint64 monotonicNow() const;

  QPointer<PlaylistModel> model_;
  QPointer<ControllerOperations> operations_;
  Clock clock_;
  QElapsedTimer elapsed_;
  QTimer timer_;
  MacroDefinition definition_;
  State state_ = State::Stopped;
  State pausedFrom_ = State::Running;
  int stepIndex_ = -1;
  quint64 sequence_ = 0;
  quint64 pendingRequest_ = 0;
  qint64 requestStartedMs_ = 0;
  qint64 delayDeadlineMs_ = 0;
  qint64 pausedDelayMs_ = 0;
  bool stepStarted_ = false;
  bool dispatching_ = false;
  bool ownModelChange_ = false;
  bool manualWait_ = false;
  QDateTime lastWall_;
  qint64 lastMono_ = 0;
  QHash<quint64, PlaylistItemState> observedItems_;
  QString detail_;
  QStringList log_;
};
