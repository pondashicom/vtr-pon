#include "controller_settings.h"
#include "switcher_control.h"
#include "controller_operations.h"
#include <QScopedValueRollback>

#include "atem_addon_switcher.h"
#include "program_playout_controller.h"
#include "roland_v8hd_midi_switcher.h"

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
  WorkerSwitcher(Factory factory, bool atem, bool receive)
      : factory_(std::move(factory)), atem_(atem), receive_(receive) {}
  ~WorkerSwitcher() override { close(); }
  void cancel() { canceled_.store(true); }
  bool open(QString *) override {
    if (canceled_.load()) return false;
    device_ = factory_ ? factory_(atem_, receive_, this)
        : atem_ ? static_cast<SwitcherDevice *>(new AtemAddonSwitcher(receive_, this))
                : static_cast<SwitcherDevice *>(new RolandV8HdMidiSwitcher(receive_, this));
    connect(device_, &SwitcherDevice::programInputChanged, this, &SwitcherDevice::programInputChanged);
    connect(device_, &SwitcherDevice::errorOccurred, this, &SwitcherDevice::errorOccurred);
    QString detail;
    ready_ = device_->open(&detail);
    emit connectionOpened(ready_, ready_ ? device_->connectionDetail() : detail);
    return ready_;
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
    if (!canceled_.load() && ready_ && !device_->setProgramInput(number, &detail)) emit errorOccurred(detail);
    return true;
  }
  bool setFadeToBlack(bool enabled, QString *) override {
    QString detail;
    if (!canceled_.load() && ready_ && !device_->setFadeToBlack(enabled, &detail)) emit errorOccurred(detail);
    return true;
  }
private:
  Factory factory_;
  bool atem_, receive_, ready_ = false;
  SwitcherDevice *device_ = nullptr;
  std::atomic_bool canceled_{false};
};

class AsyncSwitcher final : public SwitcherDevice {
public:
  AsyncSwitcher(const std::shared_ptr<SwitcherWorker> &worker, WorkerSwitcher::Factory factory,
                bool atem, bool receive, QObject *parent)
      : SwitcherDevice(parent), worker_(worker), adapter_(new WorkerSwitcher(std::move(factory), atem, receive)) {
    adapter_->moveToThread(worker_->thread);
    connect(adapter_, &SwitcherDevice::programInputChanged, this, &SwitcherDevice::programInputChanged, Qt::QueuedConnection);
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
  QSettings store = controllerSettings();
  const bool atemEnabled = settings.atemEnabled;
  store.setValue(kRolandEnabledKey,
                 settings.rolandV8HdEnabled && !atemEnabled);
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
  normalized.atemSourceNumber =
      clampAtemSourceNumber(normalized.atemSourceNumber);
  if (normalized.atemEnabled) {
    normalized.rolandV8HdEnabled = false;
  }
  if (settings_ == normalized) {
    return;
  }
  const bool deviceEnableChanged =
      settings_.rolandV8HdEnabled != normalized.rolandV8HdEnabled ||
      settings_.atemEnabled != normalized.atemEnabled;
  const bool receiveModeChanged =
      settings_.takeOnAirWhenProgramSelected !=
          normalized.takeOnAirWhenProgramSelected ||
      settings_.atemTakeOnAirWhenProgramSelected !=
          normalized.atemTakeOnAirWhenProgramSelected;
  settings_ = normalized;
  if (deviceEnableChanged || receiveModeChanged) {
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

  if (!worker_) worker_ = std::make_shared<SwitcherWorker>();
  device_ = new AsyncSwitcher(worker_, deviceFactory_, settings_.atemEnabled,
                             activeTakeOnAirWhenProgramSelected(), this);
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
  if (!device_ || sender() != device_ ||
      (!settings_.rolandV8HdEnabled && !settings_.atemEnabled) ||
      !programController_ || !activeTakeOnAirWhenProgramSelected() ||
      sourceNumber != activeSourceNumber()) {
    return;
  }
  QScopedValueRollback<bool> suppress(suppressProgramToSwitcher_, true);
  if (operations_)
    (void)operations_->executeFrom(
        "Switcher", ControllerOperations::Action::TakeSelectedOnAir);
}

int SwitcherController::activeSourceNumber() const {
  return settings_.atemEnabled ? settings_.atemSourceNumber
                               : settings_.rolandV8HdSourceNumber;
}

bool SwitcherController::activeTakeProgramWhenOnAir() const {
  return settings_.atemEnabled ? settings_.atemTakeProgramWhenOnAir
                               : settings_.takeProgramWhenOnAir;
}

bool SwitcherController::activeTakeOnAirWhenProgramSelected() const {
  return settings_.atemEnabled ? settings_.atemTakeOnAirWhenProgramSelected
                               : settings_.takeOnAirWhenProgramSelected;
}

bool SwitcherController::activeFtbFollowsProgram() const {
  return settings_.atemEnabled ? settings_.atemFtbFollowsProgram
                               : settings_.ftbFollowsProgram;
}

void SwitcherController::reportDeviceError(const QString &detail) {
  if (!detail.isEmpty()) {
    qWarning() << "Switcher control:" << detail;
    setConnectionState(false, detail);
  }
}

void SwitcherController::setConnectionState(bool ready,
                                            const QString &detail) {
  if (switcherReady_ == ready && switcherConnectionDetail_ == detail) {
    return;
  }
  switcherReady_ = ready;
  switcherConnectionDetail_ = detail;
  emit connectionStateChanged(switcherReady_, switcherConnectionDetail_);
}

} // namespace vtrpon
