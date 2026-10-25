#include "switcher_control.h"
#include "controller_operations.h"

#include <QCoreApplication>
#include <QEvent>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QSettings>
#include <atomic>
#include <iostream>
#include <thread>

bool runRolandVr50HdLanRegression();
bool runRolandSwitcherTrialRegression();

namespace vtrpon {
namespace {
std::atomic_bool adapterOnGui{false};
std::atomic_int liveAdapters{0};
std::atomic_bool overlappingAdapters{false};
std::atomic_int commandSequence{0};
std::atomic_bool commandOrderFailed{false};
class FakeSwitcher final : public SwitcherDevice {
public:
  using SwitcherDevice::SwitcherDevice;
  bool open(QString *) override {
    adapterOnGui.store(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (liveAdapters.fetch_add(1) != 0) overlappingAdapters.store(true);
    opened_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    return true;
  }
  void close() override {
    if (opened_) { --liveAdapters; opened_ = false; }
    emit programInputChanged(1);
    emit errorOccurred(QStringLiteral("retired device"));
  }
  bool setProgramInput(int, QString *) override {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) adapterOnGui.store(true);
    if (commandSequence.fetch_add(1) != 0) commandOrderFailed.store(true);
    return true;
  }
  bool setFadeToBlack(bool, QString *) override {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) adapterOnGui.store(true);
    if (commandSequence.fetch_add(1) != 1) commandOrderFailed.store(true);
    return true;
  }
  void notify(int source = 1) { emit programInputChanged(source); }
  void queueNotifications() {
    // AutoConnection queues the signal to the Controller thread. Joining
    // leaves the delivery pending while applySettings replaces the device.
    std::thread worker([this] {
      emit programInputChanged(1);
      emit errorOccurred(QStringLiteral("queued retired device"));
    });
    worker.join();
  }
private:
  bool opened_ = false;
};
}

class SwitcherControlTest {
public:
  static bool run(ProgramPlayoutController *program) {
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    const auto previousFile = qgetenv("VTRPON_SWITCHER_SETTINGS_FILE");
    qputenv("VTRPON_SWITCHER_SETTINGS_FILE",
            directory.filePath(QStringLiteral("switcher.ini")).toUtf8());
    // Existing users keep their V-8HD settings without any migration.
    {
      QSettings legacy(directory.filePath(QStringLiteral("switcher.ini")), QSettings::IniFormat);
      legacy.setValue("switcher/rolandV8Hd/sourceNumber", 8);
      legacy.setValue("switcher/rolandV8Hd/takeProgramWhenOnAir", true);
      legacy.sync();
    }
    auto persisted = loadSwitcherControlSettings();
    if (persisted.rolandModel != RolandSwitcherModel::V8Hd ||
        persisted.rolandV8HdSourceNumber != 8 || !persisted.takeProgramWhenOnAir)
      return false;
    persisted.rolandModel = RolandSwitcherModel::Vr50HdMk2;
    persisted.rolandVr50HdHost = "127.0.0.1";
    persisted.rolandVr50HdSourceNumber = 4;
    persisted.rolandVr50HdTakeOnAirWhenProgramSelected = true;
    if (!saveSwitcherControlSettings(persisted) || loadSwitcherControlSettings() != persisted)
      return false;
    for (const auto &profile : rolandSwitcherProfiles()) {
      if (profile.model == RolandSwitcherModel::V8Hd || profile.model == RolandSwitcherModel::Vr50HdMk2) continue;
      persisted.rolandModel = profile.model;
      RolandModelSettings model; model.sourceNumber = profile.inputs; model.host = "127.0.0.1";
      model.password = "Password"; model.serialPort = "COM3";
      model.takeProgramWhenOnAir = true; model.takeOnAirWhenProgramSelected = profile.receive; model.ftbFollowsProgram = profile.fade;
      setActiveRolandSettings(persisted, model);
      if (!saveSwitcherControlSettings(persisted) || loadSwitcherControlSettings() != persisted) return false;
    }
    saveSwitcherControlSettings({});
    std::atomic<FakeSwitcher *> currentAdapter{nullptr};
    SwitcherController controller(program);
    if (previousFile.isNull()) qunsetenv("VTRPON_SWITCHER_SETTINGS_FILE");
    else qputenv("VTRPON_SWITCHER_SETTINGS_FILE", previousFile);
    controller.deviceFactory_ = [&](bool, bool, QObject *parent) {
      auto *adapter = new FakeSwitcher(parent);
      currentAdapter.store(adapter);
      return adapter;
    };
    ControllerOperations operations;
    int takes = 0;
    int notifications = 0;
    bool takeAvailable = true;
    bool outboundSuppressedDuringTake = true;
    bool allSourcesAreSwitcher = true;
    QString lastSource;
    operations.setGuard(ControllerOperations::Action::TakeSelectedOnAir,
                        &operations, [&] { return takeAvailable; });
    operations.bind(ControllerOperations::Action::TakeSelectedOnAir,
                    &operations, [&] {
      ++takes;
      outboundSuppressedDuringTake &= controller.suppressProgramToSwitcher_;
      // The actual operation can synchronously publish both state changes.
      controller.handleProgramOnAirChanged(true);
      controller.handleProgramFtbChanged(true);
    });
    QObject::connect(&operations, &ControllerOperations::operationAboutToExecute,
                     &operations, [&](ControllerOperations::Action action,
                                      const QVariantList &, const QString &source) {
      if (action != ControllerOperations::Action::TakeSelectedOnAir) return;
      ++notifications;
      lastSource = source;
      allSourcesAreSwitcher &= source == QLatin1String("Switcher");
    });
    controller.setOperations(&operations);
    bool ok = true;
    const auto expect = [&](bool condition, const char *message) {
      if (!condition) std::cerr << "FAIL: switcher: " << message << '\n';
      ok &= condition;
    };
    const auto flush = [&] {
      QElapsedTimer timer;
      timer.start();
      while (timer.elapsed() < 250) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };
    const auto queueNotifications = [](SwitcherDevice *device) {
      std::thread sender([device] {
        device->programInputChanged(1);
        device->errorOccurred(QStringLiteral("queued retired device"));
      });
      sender.join();
    };
    SwitcherControlSettings settings;
    settings.rolandV8HdEnabled = true;
    settings.takeProgramWhenOnAir = true;
    settings.takeOnAirWhenProgramSelected = true;
    settings.ftbFollowsProgram = true;
    settings.atemTakeProgramWhenOnAir = true;
    settings.atemTakeOnAirWhenProgramSelected = true;
    settings.atemFtbFollowsProgram = true;
    controller.applySettings(settings);
    auto *old = controller.device_;
    flush();
    const auto notifyCurrentAdapter = [&] {
      auto *adapter = currentAdapter.load();
      return adapter && QMetaObject::invokeMethod(adapter, [adapter] {
        adapter->notify();
      }, Qt::QueuedConnection);
    };
    expect(notifyCurrentAdapter(), "current fake adapter can queue input");
    flush();
    expect(takes == 1, "current Roland notification takes");
    expect(notifications == 1 && lastSource == QLatin1String("Switcher"),
           "adapter input enters the common operation with Switcher source");
    takeAvailable = false;
    expect(notifyCurrentAdapter(), "current fake adapter can queue guarded input");
    flush();
    expect(takes == 1 && notifications == 1,
           "rejected common guard neither takes nor notifies manual intervention");
    expect(!controller.suppressProgramToSwitcher_, "guard rejection restores outbound state");
    takeAvailable = true;
    old->programInputChanged(2);
    expect(takes == 1, "different source is ignored");
    controller.handleSwitcherProgramInputChanged(1);
    expect(takes == 1, "notification without the current sender is ignored");

    queueNotifications(old);
    settings.rolandV8HdEnabled = false;
    controller.applySettings(settings);
    old->programInputChanged(1);
    flush();
    expect(takes == 1, "disabled device, close and queued notifications ignored");
    expect(controller.switcherConnectionDetail() == QStringLiteral("Disabled"),
           "retired errors do not overwrite disabled state");

    settings.rolandV8HdEnabled = true;
    controller.applySettings(settings);
    old = controller.device_;
    queueNotifications(old);
    settings.rolandV8HdEnabled = false;
    controller.applySettings(settings);
    settings.rolandV8HdEnabled = true;
    controller.applySettings(settings);
    old->programInputChanged(1);
    flush();
    expect(takes == 1, "old generation ignored after re-enable");
    expect(controller.switcherReady(), "old queued error cannot clear ready state");
    controller.device_->programInputChanged(1);
    expect(takes == 2, "new generation still takes");

    old = controller.device_;
    queueNotifications(old);
    settings.atemEnabled = true;
    controller.applySettings(settings);
    old->programInputChanged(1);
    flush();
    expect(takes == 2 && controller.switcherReady(),
           "Roland notifications ignored after switching to ATEM");
    controller.device_->programInputChanged(1);
    expect(takes == 3, "current ATEM notification takes");

    old = controller.device_;
    queueNotifications(old);
    settings.atemTakeOnAirWhenProgramSelected = false;
    controller.applySettings(settings);
    flush();
    controller.device_->programInputChanged(1);
    expect(takes == 3, "receive mode disabled rejects old and current notifications");
    expect(notifications == 3 && allSourcesAreSwitcher,
           "only current accepted inputs notify with Switcher source");
    expect(outboundSuppressedDuringTake && commandSequence.load() == 0,
           "PROGRAM and FTB state changes during received takes do not echo outbound");
    expect(!controller.suppressProgramToSwitcher_, "received take restores outbound state");
    // Subsequent UI-originated Program state events must still reach the adapter.
    controller.handleProgramOnAirChanged(true);
    controller.handleProgramFtbChanged(true);
    flush();
    expect(commandSequence.load() == 2 && !commandOrderFailed.load(),
           "PROGRAM and FTB commands execute serially on the worker");
    settings.atemEnabled = false;
    settings.rolandV8HdEnabled = true;
    settings.rolandModel = RolandSwitcherModel::Vr50HdMk2;
    settings.rolandVr50HdHost = "127.0.0.1";
    settings.rolandVr50HdSourceNumber = 4;
    settings.rolandVr50HdTakeOnAirWhenProgramSelected = true;
    settings.rolandVr50HdTakeProgramWhenOnAir = true;
    settings.rolandVr50HdFtbFollowsProgram = true;
    controller.applySettings(settings);
    flush();
    expect(controller.activeSourceNumber() == 4 &&
           controller.activeTakeProgramWhenOnAir() &&
           controller.activeTakeOnAirWhenProgramSelected() &&
           controller.activeFtbFollowsProgram(), "VR model selects its own settings");
    controller.device_->programInputChanged(1);
    expect(takes == 3, "V-8HD source is ignored with VR selected");
    controller.device_->programInputChanged(4);
    expect(takes == 4, "VR input reaches the common ON AIR operation");
    old = controller.device_;
    settings.rolandVr50HdHost = "127.0.0.2";
    controller.applySettings(settings);
    old->programInputChanged(4);
    flush();
    expect(takes == 4 && controller.device_ != old, "LAN endpoint changes replace the adapter");
    settings.atemEnabled = false;
    settings.rolandV8HdEnabled = false;
    controller.applySettings(settings);
    controller.requestShutdown();
    QElapsedTimer shutdown;
    shutdown.start();
    while (!controller.isShutdownComplete() && shutdown.elapsed() < 3000) flush();
    expect(controller.isShutdownComplete(), "worker stops after all adapter cleanup");
    expect(!adapterOnGui.load(), "adapter open executes off the Controller thread");
    expect(!overlappingAdapters.load() && liveAdapters.load() == 0,
           "configuration replacement closes each adapter before the next open");
    if (ok) std::cout << "PASS: switcher stale notification regression\n";
    return ok;
  }
};
}

bool runSwitcherControlRegression(ProgramPlayoutController *program) {
  const bool controllerOk = vtrpon::SwitcherControlTest::run(program);
  const bool vrOk = runRolandVr50HdLanRegression();
  const bool trialOk = runRolandSwitcherTrialRegression();
  return vrOk && trialOk && controllerOk;
}
