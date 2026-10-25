#include "platform_capabilities.h"
#include "controller_settings.h"
#include "switcher_control.h"
#include "controller_operations.h"
#include <QScopedValueRollback>

#include "atem_addon_switcher.h"
#include "program_playout_controller.h"
#include "roland_v8hd_midi_switcher.h"
#include "roland_vr50hd_lan_switcher.h"
#include "roland_remote_switcher.h"
#include <QJsonObject>

#include <QDebug>
#include <QSettings>
#include <QThread>
#include <atomic>
#include <utility>

namespace {

const QString kRolandEnabledKey =
    QStringLiteral("switcher/rolandV8Hd/enabled");
const QString kRolandSourceNumberKey =
    QStringLiteral("switcher/rolandV8Hd/sourceNumber");
const QString kTakeProgramWhenOnAirKey =
    QStringLiteral("switcher/rolandV8Hd/takeProgramWhenOnAir");
const QString kTakeOnAirWhenProgramSelectedKey =
    QStringLiteral("switcher/rolandV8Hd/takeOnAirWhenProgramSelected");
const QString kFtbFollowsProgramKey =
    QStringLiteral("switcher/rolandV8Hd/ftbFollowsProgram");
const QString kAtemEnabledKey = QStringLiteral("switcher/atem/enabled");
const QString kAtemSourceNumberKey =
    QStringLiteral("switcher/atem/sourceNumber");
const QString kAtemTakeProgramWhenOnAirKey =
    QStringLiteral("switcher/atem/takeProgramWhenOnAir");
const QString kAtemTakeOnAirWhenProgramSelectedKey =
    QStringLiteral("switcher/atem/takeOnAirWhenProgramSelected");
const QString kAtemFtbFollowsProgramKey =
    QStringLiteral("switcher/atem/ftbFollowsProgram");

QSettings controllerSettings() {
  const QString overrideFile =
      qEnvironmentVariable("VTRPON_SWITCHER_SETTINGS_FILE").trimmed();
  if (!overrideFile.isEmpty()) {
    return QSettings(overrideFile, QSettings::IniFormat);
  }
  return vtrpon::makeControllerSettings();
}

int clampRolandSourceNumber(int value) {
  return qBound(1, value, 8);
}

int clampAtemSourceNumber(int value) { return qBound(1, value, 40); }

} // namespace

namespace vtrpon {

// One event loop serializes replacement, SDK calls and adapter timers. Even a
// slow close completes before the next adapter is opened.
class SwitcherWorker {
public:
  SwitcherWorker() : thread(new QThread), dispatcher(new QObject) {
    dispatcher->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, dispatcher, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    const auto completion = finished;
    QObject::connect(thread, &QThread::finished, [completion] { completion->store(true); });
    thread->start();
  }
  ~SwitcherWorker() { stop(); }
  void stop() {
    if (stopping) return;
    stopping = true;
    QMetaObject::invokeMethod(dispatcher, [] { QThread::currentThread()->quit(); }, Qt::QueuedConnection);
  }
  QThread *thread;
  QObject *dispatcher;
  std::shared_ptr<std::atomic_bool> finished = std::make_shared<std::atomic_bool>(false);
  bool stopping = false;
};

namespace {
class WorkerSwitcher final : public SwitcherDevice {
public:
  using Factory = std::function<SwitcherDevice *(bool, bool, QObject *)>;
  WorkerSwitcher(Factory factory, const SwitcherControlSettings &settings, bool receive)
      : factory_(std::move(factory)), settings_(settings), receive_(receive) {}
  ~WorkerSwitcher() override { close(); }
  void cancel() { canceled_.store(true); }
  bool open(QString *) override {
    if (canceled_.load()) return false;
    device_ = factory_ ? factory_(settings_.atemEnabled, receive_, this)
        : settings_.atemEnabled ? static_cast<SwitcherDevice *>(new AtemAddonSwitcher(receive_, this))
        : settings_.rolandModel == RolandSwitcherModel::Vr50HdMk2
            ? static_cast<SwitcherDevice *>(new RolandVr50HdLanSwitcher(
                  settings_.rolandVr50HdHost, settings_.rolandVr50HdPort, receive_, this))
            : rolandSwitcherProfile(settings_.rolandModel).transport == RolandTransport::Midi
                ? static_cast<SwitcherDevice *>(new RolandV8HdMidiSwitcher(settings_.rolandModel,
                      activeRolandSettings(settings_), receive_, this))
                : static_cast<SwitcherDevice *>(new RolandRemoteSwitcher(settings_.rolandModel,
                      activeRolandSettings(settings_), receive_, this));
    connect(device_, &SwitcherDevice::programInputChanged, this, &SwitcherDevice::programInputChanged);
    connect(device_, &SwitcherDevice::diagnosticEvent, this, &SwitcherDevice::diagnosticEvent);
    connect(device_, &SwitcherDevice::errorOccurred, this, &SwitcherDevice::errorOccurred);
    connect(device_, &SwitcherDevice::connectionOpened, this,
            [this](bool ready, const QString &detail) {
              ready_ = ready;
              emit connectionOpened(ready, detail);
            });
    QString detail;
    const bool opened = device_->open(&detail);
    if (!opened || !device_->opensAsynchronously()) {
      ready_ = opened;
      emit connectionOpened(ready_, ready_ ? device_->connectionDetail() : detail);
    }
    return opened;
  }
  void close() override {
    if (!device_) return;
    disconnect(device_, nullptr, this, nullptr);
    device_->close();
    delete std::exchange(device_, nullptr);
    ready_ = false;
  }
  bool setProgramInput(int number, QString *) override {
    QString detail;
    if (canceled_.load()) return false;
    if (!ready_) { emit diagnosticEvent(QStringLiteral("SKIPPED"), QStringLiteral("Input request skipped: connection not ready.")); return true; }
    if (!device_->setProgramInput(number, &detail)) emit errorOccurred(detail);
    return true;
  }
  bool setFadeToBlack(bool enabled, QString *) override {
    QString detail;
    if (canceled_.load()) return false;
    if (!ready_) { emit diagnosticEvent(QStringLiteral("SKIPPED"), QStringLiteral("FTB request skipped: connection not ready.")); return true; }
    if (!device_->setFadeToBlack(enabled, &detail)) emit errorOccurred(detail);
    return true;
  }
private:
  Factory factory_;
  SwitcherControlSettings settings_;
  bool receive_, ready_ = false;
  SwitcherDevice *device_ = nullptr;
  std::atomic_bool canceled_{false};
};

class AsyncSwitcher final : public SwitcherDevice {
public:
  AsyncSwitcher(const std::shared_ptr<SwitcherWorker> &worker, WorkerSwitcher::Factory factory,
                const SwitcherControlSettings &settings, bool receive, QObject *parent)
      : SwitcherDevice(parent), worker_(worker), adapter_(new WorkerSwitcher(std::move(factory), settings, receive)) {
    adapter_->moveToThread(worker_->thread);
    connect(adapter_, &SwitcherDevice::programInputChanged, this, &SwitcherDevice::programInputChanged, Qt::QueuedConnection);
    connect(adapter_, &SwitcherDevice::diagnosticEvent, this, &SwitcherDevice::diagnosticEvent, Qt::QueuedConnection);
    connect(adapter_, &SwitcherDevice::errorOccurred, this, &SwitcherDevice::errorOccurred, Qt::QueuedConnection);
    connect(adapter_, &SwitcherDevice::connectionOpened, this, &SwitcherDevice::connectionOpened, Qt::QueuedConnection);
  }
  ~AsyncSwitcher() override { close(); }
  bool open(QString *) override {
    auto *adapter = adapter_;
    QMetaObject::invokeMethod(adapter, [adapter] { adapter->open(nullptr); }, Qt::QueuedConnection);
    return true;
  }
  void close() override {
    if (!adapter_) return;
    auto *adapter = std::exchange(adapter_, nullptr);
    adapter->cancel();
    disconnect(adapter, nullptr, this, nullptr);
    QMetaObject::invokeMethod(worker_->dispatcher, [adapter] { delete adapter; }, Qt::QueuedConnection);
  }
  bool setProgramInput(int number, QString *) override {
    if (!adapter_) return false;
    auto *adapter = adapter_;
    QMetaObject::invokeMethod(adapter, [adapter, number] { adapter->setProgramInput(number, nullptr); }, Qt::QueuedConnection);
    return true;
  }
  bool setFadeToBlack(bool enabled, QString *) override {
    if (!adapter_) return false;
    auto *adapter = adapter_;
    QMetaObject::invokeMethod(adapter, [adapter, enabled] { adapter->setFadeToBlack(enabled, nullptr); }, Qt::QueuedConnection);
    return true;
  }
private:
  std::shared_ptr<SwitcherWorker> worker_;
  WorkerSwitcher *adapter_;
};
}

SwitcherControlSettings loadSwitcherControlSettings() {
  const QSettings settings = controllerSettings();
  SwitcherControlSettings result;
  result.rolandV8HdEnabled =
      settings.value(kRolandEnabledKey, false).toBool();
  result.rolandModel = rolandSwitcherModel(settings.value("switcher/roland/model").toString());
  const auto profiles = QJsonObject::fromVariantMap(settings.value("switcher/roland/profiles").toMap());
  for (auto it = profiles.begin(); it != profiles.end(); ++it) {
    if (isRolandSwitcherModel(it.key()) && it.value().isObject())
      result.rolandProfiles.insert(it.key(), normalizeRolandSettings(rolandSwitcherModel(it.key()), rolandSettingsFromJson(it.value().toObject())));
  }
  result.rolandVr50HdHost = settings.value("switcher/rolandVr50Hd/host").toString().trimmed();
  result.rolandVr50HdPort = qBound(1, settings.value("switcher/rolandVr50Hd/port", 8023).toInt(), 65535);
  result.rolandVr50HdSourceNumber = qBound(1, settings.value("switcher/rolandVr50Hd/sourceNumber", 1).toInt(), 4);
  result.rolandVr50HdTakeProgramWhenOnAir = settings.value("switcher/rolandVr50Hd/takeProgramWhenOnAir", false).toBool();
  result.rolandVr50HdTakeOnAirWhenProgramSelected = settings.value("switcher/rolandVr50Hd/takeOnAirWhenProgramSelected", false).toBool();
  result.rolandVr50HdFtbFollowsProgram = settings.value("switcher/rolandVr50Hd/ftbFollowsProgram", false).toBool();
  result.rolandV8HdSourceNumber = clampRolandSourceNumber(
      settings.value(kRolandSourceNumberKey, 1).toInt());
  result.takeProgramWhenOnAir =
      settings.value(kTakeProgramWhenOnAirKey, false).toBool();
  result.takeOnAirWhenProgramSelected =
      settings.value(kTakeOnAirWhenProgramSelectedKey, false).toBool();
  result.ftbFollowsProgram =
      settings.value(kFtbFollowsProgramKey, false).toBool();
  result.atemEnabled = settings.value(kAtemEnabledKey, false).toBool();
  result.atemSourceNumber = clampAtemSourceNumber(
      settings.value(kAtemSourceNumberKey, 1).toInt());
  result.atemTakeProgramWhenOnAir =
      settings.value(kAtemTakeProgramWhenOnAirKey, false).toBool();
  result.atemTakeOnAirWhenProgramSelected =
      settings.value(kAtemTakeOnAirWhenProgramSelectedKey, false).toBool();
  result.atemFtbFollowsProgram =
      settings.value(kAtemFtbFollowsProgramKey, false).toBool();
  if (result.atemEnabled) {
    result.rolandV8HdEnabled = false;
  }
  return result;
}

bool saveSwitcherControlSettings(const SwitcherControlSettings &settings) {
  if (!platformFeatureImplemented(PlatformFeature::Switcher)) return false;
  if (settings.atemEnabled && !platformFeatureImplemented(PlatformFeature::Atem)) return false;
  if (settings.rolandV8HdEnabled && !settings.atemEnabled && !rolandSwitcherSupported(settings.rolandModel)) return false;
  QSettings store = controllerSettings();
  const bool atemEnabled = settings.atemEnabled;
  store.setValue(kRolandEnabledKey,
                 settings.rolandV8HdEnabled && !atemEnabled);
  store.setValue("switcher/roland/model", rolandSwitcherProfile(settings.rolandModel).id);
  QJsonObject profiles;
  for (auto it = settings.rolandProfiles.begin(); it != settings.rolandProfiles.end(); ++it)
    if (isRolandSwitcherModel(it.key())) profiles.insert(it.key(), rolandSettingsJson(normalizeRolandSettings(rolandSwitcherModel(it.key()), it.value()), true));
  store.setValue("switcher/roland/profiles", profiles.toVariantMap());
  store.setValue("switcher/rolandVr50Hd/host", settings.rolandVr50HdHost.trimmed());
  store.setValue("switcher/rolandVr50Hd/port", qBound(1, settings.rolandVr50HdPort, 65535));
  store.setValue("switcher/rolandVr50Hd/sourceNumber", qBound(1, settings.rolandVr50HdSourceNumber, 4));
  store.setValue("switcher/rolandVr50Hd/takeProgramWhenOnAir", settings.rolandVr50HdTakeProgramWhenOnAir);
  store.setValue("switcher/rolandVr50Hd/takeOnAirWhenProgramSelected", settings.rolandVr50HdTakeOnAirWhenProgramSelected);
  store.setValue("switcher/rolandVr50Hd/ftbFollowsProgram", settings.rolandVr50HdFtbFollowsProgram);
  store.setValue(kRolandSourceNumberKey,
                 clampRolandSourceNumber(settings.rolandV8HdSourceNumber));
  store.setValue(kTakeProgramWhenOnAirKey, settings.takeProgramWhenOnAir);
  store.setValue(kTakeOnAirWhenProgramSelectedKey,
                 settings.takeOnAirWhenProgramSelected);
  store.setValue(kFtbFollowsProgramKey, settings.ftbFollowsProgram);
  store.setValue(kAtemEnabledKey, atemEnabled);
  store.setValue(kAtemSourceNumberKey,
                 clampAtemSourceNumber(settings.atemSourceNumber));
  store.setValue(kAtemTakeProgramWhenOnAirKey,
                 settings.atemTakeProgramWhenOnAir);
  store.setValue(kAtemTakeOnAirWhenProgramSelectedKey,
                 settings.atemTakeOnAirWhenProgramSelected);
  store.setValue(kAtemFtbFollowsProgramKey, settings.atemFtbFollowsProgram);
  store.sync();
  return store.status() == QSettings::NoError;
}

RolandModelSettings activeRolandSettings(const SwitcherControlSettings &s) {
  RolandModelSettings result;
  if (s.rolandModel == RolandSwitcherModel::V8Hd) {
    result.sourceNumber = s.rolandV8HdSourceNumber;
    result.takeProgramWhenOnAir = s.takeProgramWhenOnAir;
    result.takeOnAirWhenProgramSelected = s.takeOnAirWhenProgramSelected;
    result.ftbFollowsProgram = s.ftbFollowsProgram;
  } else if (s.rolandModel == RolandSwitcherModel::Vr50HdMk2) {
    result.host = s.rolandVr50HdHost; result.port = s.rolandVr50HdPort;
    result.sourceNumber = s.rolandVr50HdSourceNumber;
    result.takeProgramWhenOnAir = s.rolandVr50HdTakeProgramWhenOnAir;
    result.takeOnAirWhenProgramSelected = s.rolandVr50HdTakeOnAirWhenProgramSelected;
    result.ftbFollowsProgram = s.rolandVr50HdFtbFollowsProgram;
  } else result = s.rolandProfiles.value(QLatin1String(rolandSwitcherProfile(s.rolandModel).id));
  return normalizeRolandSettings(s.rolandModel, result);
}
void setActiveRolandSettings(SwitcherControlSettings &s, const RolandModelSettings &value) {
  const auto m = normalizeRolandSettings(s.rolandModel, value);
  if (s.rolandModel == RolandSwitcherModel::V8Hd) {
    s.rolandV8HdSourceNumber = m.sourceNumber; s.takeProgramWhenOnAir = m.takeProgramWhenOnAir;
    s.takeOnAirWhenProgramSelected = m.takeOnAirWhenProgramSelected; s.ftbFollowsProgram = m.ftbFollowsProgram;
  } else if (s.rolandModel == RolandSwitcherModel::Vr50HdMk2) {
    s.rolandVr50HdHost = m.host; s.rolandVr50HdPort = m.port;
    s.rolandVr50HdSourceNumber = m.sourceNumber; s.rolandVr50HdTakeProgramWhenOnAir = m.takeProgramWhenOnAir;
    s.rolandVr50HdTakeOnAirWhenProgramSelected = m.takeOnAirWhenProgramSelected; s.rolandVr50HdFtbFollowsProgram = m.ftbFollowsProgram;
  } else s.rolandProfiles.insert(QLatin1String(rolandSwitcherProfile(s.rolandModel).id), m);
}

SwitcherController::SwitcherController(
    ProgramPlayoutController *programController, QObject *parent)
    : QObject(parent), programController_(programController),
      settings_(loadSwitcherControlSettings()) {
  settings_.atemEnabled = settings_.atemEnabled && AtemAddonSwitcher::addonInstalled();
  if (programController_) {
    connect(programController_, SIGNAL(onAirChanged(bool)), this,
            SLOT(handleProgramOnAirChanged(bool)));
    connect(programController_, SIGNAL(ftbChanged(bool)), this,
            SLOT(handleProgramFtbChanged(bool)));
  }
  rebuildDevice();
}

SwitcherController::~SwitcherController() {
  requestShutdown();
}

void SwitcherController::requestShutdown() {
  if (shutdownRequested_) return;
  checkSession_.stop(QStringLiteral("Application shutdown."));
  shutdownRequested_ = true;
  ++deviceGeneration_;
  if (device_) {
    auto *oldDevice = device_;
    device_ = nullptr;
    disconnect(oldDevice, nullptr, this, nullptr);
    oldDevice->close();
  }
  if (worker_) worker_->stop();
}

bool SwitcherController::isShutdownComplete() const {
  return !worker_ || worker_->finished->load();
}

void SwitcherController::applySettings(
    const SwitcherControlSettings &settings) {
  if (shutdownRequested_) return;
  SwitcherControlSettings normalized = settings;
  if (!deviceFactory_ && !AtemAddonSwitcher::addonInstalled()) normalized.atemEnabled = false;
  normalized.rolandV8HdSourceNumber =
      clampRolandSourceNumber(normalized.rolandV8HdSourceNumber);
  normalized.rolandVr50HdHost = normalized.rolandVr50HdHost.trimmed();
  normalized.rolandVr50HdPort = qBound(1, normalized.rolandVr50HdPort, 65535);
  normalized.rolandVr50HdSourceNumber = qBound(1, normalized.rolandVr50HdSourceNumber, 4);
  normalized.atemSourceNumber =
      clampAtemSourceNumber(normalized.atemSourceNumber);
  if (normalized.atemEnabled) {
    normalized.rolandV8HdEnabled = false;
  }
  if (settings_ == normalized) {
    return;
  }
  const auto before = activeRolandSettings(settings_);
  const auto after = activeRolandSettings(normalized);
  if (checkSession_.active() && (settings_.rolandModel != normalized.rolandModel || before != after ||
      settings_.rolandV8HdEnabled != normalized.rolandV8HdEnabled || normalized.atemEnabled))
    checkSession_.stop(QStringLiteral("Switcher settings changed. Start a new report for the new configuration."));
  const bool deviceEnableChanged =
      settings_.rolandV8HdEnabled != normalized.rolandV8HdEnabled ||
      settings_.atemEnabled != normalized.atemEnabled;
  const bool receiveModeChanged =
      settings_.takeOnAirWhenProgramSelected !=
          normalized.takeOnAirWhenProgramSelected ||
      settings_.atemTakeOnAirWhenProgramSelected !=
          normalized.atemTakeOnAirWhenProgramSelected ||
      settings_.rolandVr50HdTakeOnAirWhenProgramSelected !=
          normalized.rolandVr50HdTakeOnAirWhenProgramSelected ||
      before.takeOnAirWhenProgramSelected != after.takeOnAirWhenProgramSelected;
  const bool rolandConnectionChanged =
      settings_.rolandModel != normalized.rolandModel ||
      settings_.rolandVr50HdHost != normalized.rolandVr50HdHost ||
      settings_.rolandVr50HdPort != normalized.rolandVr50HdPort ||
      before.sourceNumber != after.sourceNumber || before.host != after.host || before.port != after.port || before.password != after.password ||
      before.serialPort != after.serialPort || before.baudRate != after.baudRate || before.midiDeviceId != after.midiDeviceId;
  settings_ = normalized;
  if (deviceEnableChanged || receiveModeChanged || rolandConnectionChanged) {
    rebuildDevice();
  }
}

bool SwitcherController::switcherReady() const { return switcherReady_; }

QString SwitcherController::switcherConnectionDetail() const {
  return switcherConnectionDetail_;
}

bool SwitcherController::atemAddonInstalled() const {
  return AtemAddonSwitcher::addonInstalled();
}

void SwitcherController::rebuildDevice() {
  // Invalidate even already queued deliveries before close() can emit signals.
  const quint64 generation = ++deviceGeneration_;
  if (device_) {
    auto *oldDevice = device_;
    device_ = nullptr;
    disconnect(oldDevice, nullptr, this, nullptr);
    oldDevice->close();
    oldDevice->deleteLater();
  }
  if (!settings_.rolandV8HdEnabled && !settings_.atemEnabled) {
    setConnectionState(false, QStringLiteral("Disabled"));
    return;
  }

  if (!deviceFactory_ && (!platformFeatureImplemented(PlatformFeature::Switcher) ||
      (!settings_.atemEnabled && !rolandSwitcherSupported(settings_.rolandModel)))) {
    setConnectionState(false, QStringLiteral("Selected switcher is unavailable on this platform."));
    return;
  }
  if (!worker_) worker_ = std::make_shared<SwitcherWorker>();
  device_ = new AsyncSwitcher(worker_, deviceFactory_, settings_,
                             activeTakeOnAirWhenProgramSelected(), this);
  connect(device_, &SwitcherDevice::diagnosticEvent, this,
          [this, generation](const QString &kind, const QString &detail) {
            if (generation == deviceGeneration_) checkSession_.append(kind, detail);
          });
  connect(device_, &SwitcherDevice::programInputChanged, this,
          [this, generation](int sourceNumber) {
            if (generation == deviceGeneration_) {
              handleSwitcherProgramInputChanged(sourceNumber);
            }
          });
  connect(device_, &SwitcherDevice::errorOccurred, this,
          [this, generation](const QString &detail) {
            if (generation == deviceGeneration_) {
              reportDeviceError(detail);
            }
          });
  connect(device_, &SwitcherDevice::connectionOpened, this,
          [this, generation](bool ready, const QString &detail) {
            if (generation == deviceGeneration_) setConnectionState(ready, detail);
          });
  QString errorDetail;
  if (!device_->open(&errorDetail)) {
    reportDeviceError(errorDetail);
    setConnectionState(false, errorDetail);
    return;
  }
  setConnectionState(false, QStringLiteral("Connecting"));
}

void SwitcherController::handleProgramOnAirChanged(bool onAir) {
  if (!onAir || suppressProgramToSwitcher_ || !device_ ||
      !activeTakeProgramWhenOnAir()) {
    return;
  }
  checkSession_.append("ACTION", QStringLiteral("ON AIR -> input %1 requested; device acknowledgement is recorded separately.").arg(activeSourceNumber()));
  QString errorDetail;
  if (!device_->setProgramInput(activeSourceNumber(), &errorDetail)) {
    reportDeviceError(errorDetail);
    setConnectionState(false, errorDetail);
  }
}

void SwitcherController::handleProgramFtbChanged(bool active) {
  if (suppressProgramToSwitcher_ || !device_ ||
      !activeFtbFollowsProgram()) {
    return;
  }
  checkSession_.append("ACTION", active ? QStringLiteral("FTB ON requested.") : QStringLiteral("FTB OFF requested."));
  QString errorDetail;
  if (!device_->setFadeToBlack(active, &errorDetail)) {
    reportDeviceError(errorDetail);
    setConnectionState(false, errorDetail);
  }
}

void SwitcherController::setOperations(ControllerOperations *operations) {
  operations_ = operations;
}

void SwitcherController::handleSwitcherProgramInputChanged(int sourceNumber) {
  checkSession_.append("INPUT", QStringLiteral("Device input changed to %1").arg(sourceNumber));
  if (!device_ || sender() != device_ ||
      (!settings_.rolandV8HdEnabled && !settings_.atemEnabled) ||
      !programController_ || !activeTakeOnAirWhenProgramSelected() ||
      sourceNumber != activeSourceNumber()) {
    return;
  }
  QScopedValueRollback<bool> suppress(suppressProgramToSwitcher_, true);
  if (operations_) {
    const bool executed = operations_->executeFrom("Switcher", ControllerOperations::Action::TakeSelectedOnAir).invoked();
    checkSession_.append("ON_AIR", executed ? QStringLiteral("ON AIR operation accepted.") : QStringLiteral("ON AIR operation blocked by the current application state."));
  }
}

int SwitcherController::activeSourceNumber() const {
  return settings_.atemEnabled ? settings_.atemSourceNumber
      : activeRolandSettings(settings_).sourceNumber;
}

bool SwitcherController::activeTakeProgramWhenOnAir() const {
  return settings_.atemEnabled ? settings_.atemTakeProgramWhenOnAir
      : activeRolandSettings(settings_).takeProgramWhenOnAir;
}

bool SwitcherController::activeTakeOnAirWhenProgramSelected() const {
  return settings_.atemEnabled ? settings_.atemTakeOnAirWhenProgramSelected
      : activeRolandSettings(settings_).takeOnAirWhenProgramSelected;
}

bool SwitcherController::activeFtbFollowsProgram() const {
  return settings_.atemEnabled ? settings_.atemFtbFollowsProgram
      : activeRolandSettings(settings_).ftbFollowsProgram;
}

void SwitcherController::reportDeviceError(const QString &detail) {
  if (!detail.isEmpty()) {
    checkSession_.append("ERROR", detail);
    qWarning() << "Switcher control:" << detail;
    setConnectionState(false, detail);
  }
}

void SwitcherController::setConnectionState(bool ready,
                                            const QString &detail) {
  if (switcherReady_ == ready && switcherConnectionDetail_ == detail) {
    return;
  }
  checkSession_.append("CONNECTION", (ready ? QStringLiteral("Ready: ") : QStringLiteral("Not ready: ")) + detail);
  switcherReady_ = ready;
  switcherConnectionDetail_ = detail;
  emit connectionStateChanged(switcherReady_, switcherConnectionDetail_);
}

void SwitcherController::retryConnection() { if (!shutdownRequested_) rebuildDevice(); }
bool SwitcherController::startCheck() {
  if (shutdownRequested_ || !settings_.rolandV8HdEnabled || settings_.atemEnabled || !rolandSwitcherSupported(settings_.rolandModel)) return false;
  checkSession_.start(settings_.rolandModel, activeRolandSettings(settings_));
  rebuildDevice();
  return true;
}
void SwitcherController::stopCheck() { checkSession_.stop(); }

} // namespace vtrpon
