#include "addon_runtime_status_widget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QThread>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  CHECK(app.arguments().size() == 3);
  const QString library = app.arguments().at(1), probe = app.arguments().at(2);
  const QList<QPair<QByteArray, QString>> cases{{"0", "ready"}, {"1", "missing"}, {"2", "incompatible"},
      {"3", "error"}, {"hang", "timeout"}, {"crash", "error"}};
  for (const auto &[mode, expected] : cases) {
    qputenv("VTRPON_FIXTURE_RUNTIME", mode);
    vtrpon::AddonRuntimeStatusWidget widget(vtrpon::AddonRuntimeKind::Atem, library, nullptr, probe);
    QElapsedTimer timer; timer.start();
    while (widget.property("runtimeState").toString() == "checking" && timer.elapsed() < 7000) {
      app.processEvents(); QThread::msleep(10);
    }
    CHECK(widget.property("runtimeState").toString() == expected);
  }
  qunsetenv("VTRPON_FIXTURE_RUNTIME");
  vtrpon::AddonRuntimeStatusWidget missing(vtrpon::AddonRuntimeKind::Atem, library, nullptr, "/missing/helper");
  app.processEvents(); CHECK(missing.property("runtimeState").toString() == "check_unavailable");
  vtrpon::AddonRuntimeStatusWidget ndi(vtrpon::AddonRuntimeKind::NetworkVideo, library, nullptr, probe);
  CHECK(ndi.downloadUrl().host() == "ndi.video" && !ndi.downloadUrl().toString().contains("NDIRedistV6"));
  const auto *button = ndi.findChild<QPushButton *>("addonRuntimeDownload");
  CHECK(button && !button->accessibleDescription().contains("x64"));
  vtrpon::AddonRuntimeStatusWidget vst3(vtrpon::AddonRuntimeKind::Vst3, library, nullptr, probe);
  CHECK(vst3.downloadUrl().isEmpty());
  vtrpon::AddonRuntimeStatusWidget decklink(vtrpon::AddonRuntimeKind::DeckLink, library, nullptr, probe);
  CHECK(decklink.downloadUrl().host() == "www.blackmagicdesign.com");
  const auto *decklinkButton = decklink.findChild<QPushButton *>("addonRuntimeDownload");
  CHECK(decklinkButton && decklinkButton->accessibleDescription().contains("macOS") && !decklinkButton->accessibleDescription().contains("x64"));

  qputenv("VTRPON_FIXTURE_RUNTIME", "hang");
  QElapsedTimer close; close.start();
  { vtrpon::AddonRuntimeStatusWidget widget(vtrpon::AddonRuntimeKind::Atem, library, nullptr, probe);
    app.processEvents(); QThread::msleep(30); app.processEvents(); close.restart(); }
  CHECK(close.elapsed() < 250);
  qunsetenv("VTRPON_FIXTURE_RUNTIME");
  std::puts("Mac add-on isolated runtime diagnostics/status/timeout/close: PASS"); return 0;
}
