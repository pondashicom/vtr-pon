#include "atem_addon_switcher.h"
#include "addon_platform.h"
#include "controller_settings_dialog.h"
#include "switcher_control.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return false; } } while (false)
namespace {
bool until(const std::function<bool()> &ready, int ms = 5000) {
  QElapsedTimer time; time.start();
  while (!ready() && time.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
  return ready();
}
void pump(int ms) { QElapsedTimer t; t.start(); until([&] { return t.elapsed() >= ms; }, ms + 100); }
}
bool runMacAtemRegression(const QString &fixture) {
  using namespace vtrpon;
  QTemporaryDir dir; CHECK(dir.isValid());
  CHECK(AtemAddonSwitcher::addonFilePath() == addons::addonFilePath(QCoreApplication::applicationDirPath(), "vtr-pon-atem-addon"));
  qputenv("VTRPON_SWITCHER_ADDON_DIR", dir.path().toUtf8());
  const auto path = dir.filePath("vtr-pon-atem-addon.dylib");
  CHECK(AtemAddonSwitcher::addonFilePath() == path);
  { QFile bad(path); CHECK(bad.open(QIODevice::WriteOnly)); bad.write("Windows DLL must not load"); }
  CHECK(AtemAddonSwitcher::addonInstalled());
  QString error;
  { AtemAddonSwitcher d(false); CHECK(!d.open(&error) && error.contains("CPU")); }
  QFile source(fixture); CHECK(source.open(QIODevice::ReadOnly));
  auto damaged = source.readAll(); CHECK(damaged.size() > 4096);
  damaged[4096] = damaged[4096] ^ 1; // Change a hashed code page, not signature padding.
  { QFile bad(path); CHECK(bad.open(QIODevice::WriteOnly | QIODevice::Truncate)); CHECK(bad.write(damaged) == damaged.size()); }
  { AtemAddonSwitcher d(false); CHECK(!d.open(&error) && error.contains("signature")); }
  CHECK(QFile::remove(path) && QFile::copy(fixture, path));
  QLibrary control(path); CHECK(control.load());
  auto set = reinterpret_cast<void (*)(int,int)>(control.resolve("fixtureSet"));
  auto get = reinterpret_cast<int (*)(int)>(control.resolve("fixtureGet"));
  CHECK(set && get);
  set(1, 1);
  for (int mode : {5, 6, 7, 8}) {
    set(0, mode); AtemAddonSwitcher d(false);
    CHECK(!d.open(&error) && error.contains("ABI") && get(5) == 0);
  }
  set(0, 1);
  { AtemAddonSwitcher d(false); CHECK(!d.open(&error) && error.contains("open failed")); }
  CHECK(get(14) == 0 && get(5) == get(6));
  set(0, 2);
  { AtemAddonSwitcher d(false); CHECK(!d.open(&error) && error.contains("poll failed")); }
  CHECK(get(14) == 0);
  set(0, 0);
  {
    AtemAddonSwitcher d(true); std::vector<int> events; int failures = 0;
    QObject::connect(&d, &SwitcherDevice::programInputChanged, &d, [&](int n) { events.push_back(n); });
    QObject::connect(&d, &SwitcherDevice::connectionOpened, &d, [&](bool ready, const QString &) { if (!ready) ++failures; });
    CHECK(d.open(&error) && d.connectionDetail().contains("テスト"));
    CHECK(events == std::vector<int>{1});
    pump(550); CHECK(events.size() == 1);
    CHECK(d.setProgramInput(40, &error));
    CHECK(until([&] { return events.size() == 2; }) && events.back() == 40);
    set(1, 0); pump(300); set(1, 41); pump(300); CHECK(events.size() == 2);
    CHECK(d.setFadeToBlack(true, &error) && get(2) == 1);
    CHECK(d.setFadeToBlack(false, &error) && get(2) == 0);
    set(0, 2); CHECK(until([&] { return failures == 1; }));
    CHECK(!d.setProgramInput(1, &error) && get(14) == 0);
    const int polls = get(10); pump(400); CHECK(get(10) == polls);
    set(0, 0); CHECK(d.open(&error)); // Explicit OFF/ON reopen, no automatic reconnect.
  }
  for (int mode : {3, 4}) {
    AtemAddonSwitcher d(false); CHECK(d.open(&error)); set(0, mode);
    CHECK(mode == 3 ? !d.setProgramInput(1, &error) : !d.setFadeToBlack(true, &error));
    CHECK(get(14) == 0); set(0, 0);
  }
  CHECK(get(5) == get(6) && get(13) == 0);
  {
    AtemAddonSwitcher d(false); int events = 0;
    QObject::connect(&d, &SwitcherDevice::programInputChanged, &d, [&](int) { ++events; });
    CHECK(d.open(&error)); set(1, 2); pump(300); CHECK(events == 0);
  }
  CHECK(saveSwitcherControlSettings({}));
  {
    ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *atem = dialog.findChild<QCheckBox *>("atemEnabledCheckBox");
    auto *roland = dialog.findChild<QCheckBox *>("rolandV8HdEnabledCheckBox");
    auto *card = dialog.findChild<QWidget *>("atemSwitcherSettingsCard");
    CHECK(atem && atem->isEnabled() && roland && card && !card->isHidden());
    auto *atemPage = dialog.findChild<QWidget *>("atemSettingsScroll");
    auto *rolandPage = dialog.findChild<QWidget *>("switcherSettingsScroll");
    CHECK(atemPage && rolandPage && atemPage->isAncestorOf(atem) && !atemPage->isAncestorOf(roland));
    CHECK(rolandPage->isAncestorOf(roland) && !rolandPage->isAncestorOf(atem));
    atem->setChecked(true); CHECK(loadSwitcherControlSettings().atemEnabled);
    roland->setChecked(true); CHECK(!atem->isChecked() && !loadSwitcherControlSettings().atemEnabled);
    atem->setChecked(true); CHECK(!roland->isChecked() && !loadSwitcherControlSettings().rolandV8HdEnabled);
    atem->setChecked(false);
  }
  set(11, 1); set(3, 250); set(4, 250);
  {
    SwitcherController controller(nullptr);
    SwitcherControlSettings settings;
    settings.atemEnabled = true; settings.atemSourceNumber = 8;
    settings.atemTakeProgramWhenOnAir = true; settings.atemFtbFollowsProgram = true;
    CHECK(saveSwitcherControlSettings(settings) && loadSwitcherControlSettings() == settings);
    QElapsedTimer time; time.start(); controller.applySettings(settings);
    CHECK(time.elapsed() < 100 && !controller.switcherReady());
    CHECK(until([&] { return controller.switcherReady(); }));
    CHECK(QMetaObject::invokeMethod(&controller, "handleProgramOnAirChanged", Q_ARG(bool, true)));
    CHECK(until([&] { return get(1) == 8; }));
    CHECK(QMetaObject::invokeMethod(&controller, "handleProgramFtbChanged", Q_ARG(bool, true)));
    CHECK(until([&] { return get(2) == 1; }));
    set(0, 2); CHECK(until([&] { return !controller.switcherReady(); }));
    CHECK(controller.switcherConnectionDetail().contains("poll failed"));
    set(0, 0); controller.applySettings({}); controller.applySettings(settings);
    CHECK(until([&] { return controller.switcherReady(); }));
    time.restart(); controller.requestShutdown(); CHECK(time.elapsed() < 100);
    CHECK(until([&] { return controller.isShutdownComplete(); }));
  }
  CHECK(get(12) == 0 && get(14) == 0 && get(5) == get(6));
  // Cancel during a slow open: drain the same worker and release every object.
  CHECK(saveSwitcherControlSettings({}));
  {
    SwitcherController controller(nullptr); SwitcherControlSettings settings; settings.atemEnabled = true;
    const int opens = get(7); controller.applySettings(settings); CHECK(until([&] { return get(7) > opens; }));
    QElapsedTimer time; time.start(); controller.requestShutdown(); CHECK(time.elapsed() < 100);
    CHECK(until([&] { return controller.isShutdownComplete(); }));
  }
  CHECK(get(12) == 0 && get(14) == 0 && get(5) == get(6));
  CHECK(saveSwitcherControlSettings({}));
  std::printf("ATEM Mac host: signed fixture, ABI, USB address, Program/FTB, failure, UI, worker and shutdown PASS (no hardware)\n");
  return true;
}
