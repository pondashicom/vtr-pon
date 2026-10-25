#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QMap>
#include "roland_switcher_profiles.h"
#include "switcher_check_session.h"
#include <functional>
#include <memory>

class ProgramPlayoutController;
class ControllerOperations;

namespace vtrpon {
class SwitcherWorker;

struct SwitcherControlSettings {
  // Keep the existing V-8HD keys and fields for saved-settings compatibility.
  bool rolandV8HdEnabled = false;
  RolandSwitcherModel rolandModel = RolandSwitcherModel::V8Hd;
  QMap<QString, RolandModelSettings> rolandProfiles;
  QString rolandVr50HdHost;
  int rolandVr50HdPort = 8023;
  int rolandVr50HdSourceNumber = 1;
  bool rolandVr50HdTakeProgramWhenOnAir = false;
  bool rolandVr50HdTakeOnAirWhenProgramSelected = false;
  bool rolandVr50HdFtbFollowsProgram = false;
  int rolandV8HdSourceNumber = 1;
  bool takeProgramWhenOnAir = false;
  bool takeOnAirWhenProgramSelected = false;
  bool ftbFollowsProgram = false;
  bool atemEnabled = false;
  int atemSourceNumber = 1;
  bool atemTakeProgramWhenOnAir = false;
  bool atemTakeOnAirWhenProgramSelected = false;
  bool atemFtbFollowsProgram = false;

  bool operator==(const SwitcherControlSettings &) const = default;
};

SwitcherControlSettings loadSwitcherControlSettings();
bool saveSwitcherControlSettings(const SwitcherControlSettings &settings);
RolandModelSettings activeRolandSettings(const SwitcherControlSettings &settings);
void setActiveRolandSettings(SwitcherControlSettings &settings, const RolandModelSettings &modelSettings);

class SwitcherDevice : public QObject {
  Q_OBJECT

public:
  explicit SwitcherDevice(QObject *parent = nullptr) : QObject(parent) {}

  virtual bool open(QString *errorDetail = nullptr) = 0;
  // Asynchronous adapters publish readiness (including reconnects) themselves.
  virtual bool opensAsynchronously() const { return false; }
  virtual void close() = 0;
  virtual bool setProgramInput(int sourceNumber,
                               QString *errorDetail = nullptr) = 0;
  virtual bool setFadeToBlack(bool enabled,
                              QString *errorDetail = nullptr) = 0;
  virtual QString connectionDetail() const { return QStringLiteral("Ready"); }

signals:
  void connectionOpened(bool ready, const QString &detail);
  void programInputChanged(int sourceNumber);
  void errorOccurred(const QString &detail);
  void diagnosticEvent(const QString &kind, const QString &detail);
};

class SwitcherController final : public QObject {
  Q_OBJECT

public:
  explicit SwitcherController(ProgramPlayoutController *programController,
                              QObject *parent = nullptr);
  ~SwitcherController() override;

  void setOperations(ControllerOperations *operations);
  void applySettings(const SwitcherControlSettings &settings);
  bool switcherReady() const;
  QString switcherConnectionDetail() const;
  bool atemAddonInstalled() const;
  void requestShutdown();
  bool isShutdownComplete() const;
  SwitcherControlSettings settings() const { return settings_; }
  void retryConnection();
  bool startCheck();
  void stopCheck();
  SwitcherCheckSession &checkSession() { return checkSession_; }

signals:
  void connectionStateChanged(bool ready, const QString &detail);

private slots:
  void handleProgramOnAirChanged(bool onAir);
  void handleProgramFtbChanged(bool active);
  void handleSwitcherProgramInputChanged(int sourceNumber);

private:
  friend class SwitcherControlTest;
  friend class SwitcherReportTest;
  void rebuildDevice();
  void reportDeviceError(const QString &detail);
  void setConnectionState(bool ready, const QString &detail);
  int activeSourceNumber() const;
  bool activeTakeProgramWhenOnAir() const;
  bool activeTakeOnAirWhenProgramSelected() const;
  bool activeFtbFollowsProgram() const;

  QPointer<ControllerOperations> operations_;
  ProgramPlayoutController *programController_ = nullptr;
  SwitcherControlSettings settings_;
  SwitcherDevice *device_ = nullptr;
  quint64 deviceGeneration_ = 0;
  // Test seam: production uses the concrete adapters below.
  std::function<SwitcherDevice *(bool, bool, QObject *)> deviceFactory_;
  bool suppressProgramToSwitcher_ = false;
  bool switcherReady_ = false;
  QString switcherConnectionDetail_;
  std::shared_ptr<SwitcherWorker> worker_;
  bool shutdownRequested_ = false;
  SwitcherCheckSession checkSession_;
};

} // namespace vtrpon
