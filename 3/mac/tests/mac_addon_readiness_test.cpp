// Mac execution of the readiness cases received in Windows commit 23ba35d.
// No vendor runtime, physical device or normal application settings are used.
#include "addon_runtime_guidance.h"
#include "addon_runtime_initialization.h"
#include "addon_management_page.h"
#include "skins/controller_skin.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>
#include <QFile>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
namespace {
int verifyAddonInitializationChecks() {
  using namespace vtrpon;
  struct State { int creates = 0, destroys = 0, listed = 0; bool failCreate = false, failList = false; uint32_t count = 2; };
  static State state;
  state = {};
  VtrPonNetworkVideoApi ndi{};
  ndi.create = [](char *, uint32_t) -> void * { ++state.creates; return state.failCreate ? nullptr : &state; };
  ndi.destroy = [](void *) { ++state.destroys; };
  CHECK(QByteArray(checkNetworkVideoInitialization(&ndi).status) == "ready");
  CHECK(state.creates == 1 && state.destroys == 1 && state.listed == 0);
  state.failCreate = true;
  CHECK(QByteArray(checkNetworkVideoInitialization(&ndi).status) == "initialization_failed");
  CHECK(state.destroys == 1);
  state = {};
  VtrPonDeckLinkApi decklink{};
  decklink.create = [](void **context, char *, uint32_t) -> int32_t {
    ++state.creates; *context = state.failCreate ? nullptr : &state;
    return state.failCreate ? VtrPonDeckLinkBackendError : VtrPonDeckLinkOk;
  };
  decklink.destroy = [](void *, char *, uint32_t) -> int32_t { ++state.destroys; return VtrPonDeckLinkOk; };
  decklink.listDevices = [](void *, VtrPonDeckLinkDevice *devices, uint32_t capacity,
      uint32_t *count, char *, uint32_t) -> int32_t {
    ++state.listed;
    if (devices || capacity) return VtrPonDeckLinkInvalidArgument;
    *count = state.count;
    return state.failList ? VtrPonDeckLinkBackendError : state.count ? VtrPonDeckLinkBufferTooSmall : VtrPonDeckLinkOk;
  };
  CHECK(QByteArray(checkDeckLinkInitialization(&decklink).status) == "ready");
  CHECK(state.listed == 1 && state.destroys == 1);
  state.count = 0;
  CHECK(QByteArray(checkDeckLinkInitialization(&decklink).status) == "no_devices");
  state.failList = true;
  CHECK(QByteArray(checkDeckLinkInitialization(&decklink).status) == "initialization_failed");
  CHECK(state.destroys == 3);
  state.failCreate = true;
  CHECK(QByteArray(checkDeckLinkInitialization(&decklink).status) == "initialization_failed");
  CHECK(state.destroys == 3 && state.listed == 3);
  return 0;
}

int verifyAddonOperationalStatus() {
  using namespace vtrpon;
  NetworkVideoSettings ndi;
  NetworkVideoState network;
  network.error = QStringLiteral("old failure");
  CHECK(networkVideoOperationalStatus(ndi, network) == uiText("addons.runtime.operation_off"));
  network.error.clear();
  ndi.inputEnabled = true;
  network.sources.push_back({QStringLiteral("Old source"), QStringLiteral("old-source")});
  CHECK(networkVideoOperationalStatus(ndi, network) == uiText("addons.runtime.operation_input_starting"));
  network.inputRunning = true;
  CHECK(networkVideoOperationalStatus(ndi, network) == uiText("addons.runtime.operation_input").arg(1));
  ndi.inputEnabled = false;
  ndi.outputEnabled = true; ndi.fillEnabled = true;
  network.outputs[0].running = true;
  CHECK(networkVideoOperationalStatus(ndi, network) == uiText("addons.runtime.operation_partial").arg(1).arg(2));
  network.outputs[1].running = true;
  network.outputs[2].error = QStringLiteral("disabled output failure");
  CHECK(networkVideoOperationalStatus(ndi, network) == uiText("addons.runtime.operation_running"));
  network.outputs[1].error = QStringLiteral("sender initialization failed");
  CHECK(networkVideoOperationalStatus(ndi, network).contains(QStringLiteral("sender initialization failed")));
  ndi.outputEnabled = false;
  CHECK(networkVideoOperationalStatus(ndi, network) == uiText("addons.runtime.operation_off"));

  DeckLinkSettings settings;
  DeckLinkState decklink;
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_unavailable"));
  decklink.availability = DeckLinkState::Availability::Checking;
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_checking"));
  decklink.availability = DeckLinkState::Availability::NoDevices;
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_no_devices"));
  decklink.availability = DeckLinkState::Availability::Ready;
  DeckLinkChannel channel; channel.name = QStringLiteral("DeckLink test channel");
  decklink.channels.push_back(channel);
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_devices").arg(1));
  settings.outputEnabled = true; settings.fillEnabled = true;
  decklink.outputs[0].running = true;
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_partial").arg(1).arg(2));
  decklink.outputs[1].running = true;
  settings.inputEnabled = false;
  decklink.readbackError = QStringLiteral("disabled input failure");
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_running"));
  decklink.outputs[1].error = QStringLiteral("channel is busy");
  CHECK(deckLinkOperationalStatus(settings, decklink).contains(QStringLiteral("channel is busy")));
  settings.outputEnabled = false;
  decklink.error = QStringLiteral("old failure");
  CHECK(deckLinkOperationalStatus(settings, decklink) == uiText("addons.runtime.operation_off"));
  return 0;
}

int verifyAddonRuntimeStartupWarning(QApplication &application) {
  using namespace vtrpon;
  QTemporaryDir directory;
  CHECK(directory.isValid());
  const auto writeStatus = [&](const QString &fileName, const char *status) {
    QFile file(directory.filePath(fileName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(QJsonObject{{QStringLiteral("status"), QString::fromLatin1(status)}})
        .toJson(QJsonDocument::Compact)) > 0;
  };
  const auto waitFor = [&](auto predicate, int timeout = 3000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
      application.processEvents();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QThread::msleep(5);
    }
    return predicate();
  };
  QWidget controller;
  controller.show();
  const QString atemFile = directory.filePath(QStringLiteral("atem.json"));
  const QString ndiFile = directory.filePath(QStringLiteral("ndi.json"));
  const QString deckLinkFile = directory.filePath(QStringLiteral("decklink.json"));
  const QVector<AddonRuntimeTarget> targets{
      {QStringLiteral("atem"), QStringLiteral("ATEM"), AddonRuntimeKind::Atem, atemFile},
      {QStringLiteral("ndi"), QStringLiteral("NDI"), AddonRuntimeKind::NetworkVideo, ndiFile},
      {QStringLiteral("decklink"), QStringLiteral("DeckLink"), AddonRuntimeKind::DeckLink, deckLinkFile}};
  const auto start = [&](const QString &probe = {}) {
    return QPointer<AddonRuntimeStartupWarning>(new AddonRuntimeStartupWarning(&controller, targets,
        probe.isEmpty() ? application.applicationFilePath() : probe));
  };
  auto absent = start();
  CHECK(!absent->isVisible()); CHECK(waitFor([&] { return absent.isNull(); }));
  CHECK(writeStatus(QStringLiteral("atem.json"), "ready"));
  CHECK(writeStatus(QStringLiteral("ndi.json"), "ready"));
  CHECK(writeStatus(QStringLiteral("decklink.json"), "no_devices"));
  auto ready = start();
  CHECK(!ready->isVisible()); CHECK(waitFor([&] { return ready.isNull(); }));

  CHECK(writeStatus(QStringLiteral("ndi.json"), "initialization_failed"));
  auto initialization = start();
  CHECK(waitFor([&] { return initialization && initialization->isVisible(); }));
  auto *failed = initialization->findChild<QWidget *>("networkVideoRuntimeStatus");
  CHECK(failed && failed->property("runtimeState").toString() == "initialization_failed");
  CHECK(failed->findChild<QLabel *>("addonRuntimeSetupHelp")->isVisible());
  CHECK(failed->findChild<QPushButton *>("addonRuntimeDownload")->isHidden());
  initialization->close(); CHECK(waitFor([&] { return initialization.isNull(); }));
  CHECK(writeStatus(QStringLiteral("ndi.json"), "ready"));
  CHECK(writeStatus(QStringLiteral("atem.json"), "missing"));
  CHECK(writeStatus(QStringLiteral("decklink.json"), "incompatible"));
  auto warning = start();
  CHECK(waitFor([&] { return warning && warning->isVisible(); }));
  CHECK(!warning->isModal() && controller.isEnabled() && QApplication::activeModalWidget() == nullptr);
  auto *atem = static_cast<AddonRuntimeStatusWidget *>(warning->findChild<QWidget *>("atemRuntimeStatus"));
  auto *ndi = static_cast<AddonRuntimeStatusWidget *>(warning->findChild<QWidget *>("networkVideoRuntimeStatus"));
  auto *decklink = static_cast<AddonRuntimeStatusWidget *>(warning->findChild<QWidget *>("deckLinkRuntimeStatus"));
  CHECK(atem && ndi && decklink);
  CHECK(atem->isVisible() && !ndi->isVisibleTo(warning) && decklink->isVisible());
  CHECK(atem->findChild<QPushButton *>("addonRuntimeDownload")->isVisible());
  CHECK(decklink->findChild<QPushButton *>("addonRuntimeDownload")->isVisible());
  const auto captures = qEnvironmentVariable("VTRPON_ADDON_RUNTIME_UI_CAPTURE_DIR");
  if (!captures.isEmpty()) {
    CHECK(QDir().mkpath(captures));
    QTranslator japanese;
    CHECK(japanese.load(application.applicationDirPath() + QStringLiteral("/vtrpon_ja.qm")));
    CHECK(application.installTranslator(&japanese)); application.processEvents();
    CHECK(warning->windowTitle() == QStringLiteral("アドオンの動作環境に注意が必要です"));
    CHECK(warning->findChild<QPushButton *>("addonRuntimeClose")->text() == QStringLiteral("閉じる"));
    const QString originalSkin = currentControllerSkinKey();
    for (const QString &skin : {QStringLiteral("hardware"), QStringLiteral("material-dark"), QStringLiteral("vtr-pon-clasic")}) {
      CHECK(applyControllerSkin(application, skin)); application.processEvents();
      CHECK(warning->findChild<QPushButton *>("addonRuntimeClose")->text() == QStringLiteral("閉じる"));
      CHECK(warning->grab().save(QDir(captures).filePath(QStringLiteral("startup-runtime-%1-ja.png").arg(skin))));
    }
    CHECK(applyControllerSkin(application, originalSkin));
    application.removeTranslator(&japanese); application.processEvents();
  }
  CHECK(writeStatus(QStringLiteral("atem.json"), "ready")); atem->check();
  CHECK(waitFor([&] { return atem->runtimeState() == QLatin1StringView("ready"); }));
  CHECK(warning && warning->isVisible() && !atem->isVisibleTo(warning));
  CHECK(writeStatus(QStringLiteral("decklink.json"), "ready")); decklink->check();
  CHECK(waitFor([&] { return warning.isNull(); }));

  auto noHelper = start(directory.filePath(QStringLiteral("missing-probe")));
  CHECK(waitFor([&] { return noHelper && noHelper->isVisible(); }));
  for (auto *widget : noHelper->findChildren<QWidget *>()) {
    auto *runtime = dynamic_cast<AddonRuntimeStatusWidget *>(widget);
    if (!runtime) continue;
    CHECK(runtime->runtimeState() == QLatin1StringView("check_unavailable"));
    CHECK(runtime->findChild<QPushButton *>("addonRuntimeDownload")->isHidden());
  }
  noHelper->close(); CHECK(waitFor([&] { return noHelper.isNull(); }));
  CHECK(writeStatus(QStringLiteral("atem.json"), "slow"));
  auto stalled = start();
  int ticks = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, &controller, [&] { ++ticks; });
  heartbeat.start(10);
  CHECK(waitFor([&] { return stalled && stalled->isVisible(); }, 6500));
  CHECK(ticks > 20 && controller.isEnabled());
  CHECK(stalled->findChild<QWidget *>("atemRuntimeStatus")->property("runtimeState").toString() == QLatin1StringView("timeout"));
  stalled->close(); CHECK(waitFor([&] { return stalled.isNull(); }));
  heartbeat.stop();
  AddonManagementPage management;
  CHECK(management.findChild<QWidget *>("atemRuntimeStatus"));
  CHECK(management.findChild<QWidget *>("networkVideoRuntimeStatus"));
  CHECK(management.findChild<QWidget *>("deckLinkRuntimeStatus"));
  return 0;
}

}
int runMacAddonReadinessRegression(QApplication &app) {
  CHECK(verifyAddonInitializationChecks() == 0);
  CHECK(verifyAddonOperationalStatus() == 0);
  CHECK(verifyAddonRuntimeStartupWarning(app) == 0);
  // Allow killed disposable probes to finish and delete themselves before
  // QApplication is destroyed; closing the notice itself stays nonblocking.
  QElapsedTimer cleanup;
  cleanup.start();
  while (!app.findChildren<QProcess *>().isEmpty() && cleanup.elapsed() < 1000) {
    app.processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThread::msleep(5);
  }
  CHECK(app.findChildren<QProcess *>().isEmpty());
  std::puts("Mac add-on initialization/operational state/startup warning: PASS");
  return 0;
}
