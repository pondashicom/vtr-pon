#include "mac_system_load_sampler.h"
#include "system_load_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <cmath>
#include <iostream>

#define CHECK(condition, message) do { if (!(condition)) { \
  std::cerr << "FAIL: " << message << '\n'; return 1; } } while (false)

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTranslator translator;
  if (app.arguments().size() > 1) {
    CHECK(translator.load(app.arguments()[1]), "load production Japanese translations");
    app.installTranslator(&translator);
  }
  vtrpon::MacSystemLoadSampler sampler;
  QThread::msleep(250);
  const auto idle = sampler.sample();
  const auto percentage = [](double v) { return std::isfinite(v) && v >= 0 && v <= 100; };
  CHECK(percentage(idle.systemCpu) && percentage(idle.processCpu), "real CPU counters available");
  CHECK(percentage(idle.systemRam) && idle.systemRam > 0, "real system RAM available");
  CHECK(percentage(idle.processRam) && idle.processRam > 0, "real process footprint available");
  int cores = 0;
  size_t size = sizeof(cores);
  CHECK(sysctlbyname("hw.logicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0, "core count available");
  QElapsedTimer busy;
  busy.start();
  while (busy.elapsed() < 600) {} // One fully busy logical core.
  const auto loaded = sampler.sample();
  CHECK(loaded.processCpu > 0.3 * 100.0 / cores && loaded.processCpu < 1.5 * 100.0 / cores,
        "process CPU reacts to one-core load and is normalized across all cores");

  constexpr size_t bytes = 64 * 1024 * 1024;
  void *allocation = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(allocation != MAP_FAILED, "temporary memory allocation succeeds");
  auto *memory = static_cast<volatile unsigned char *>(allocation);
  for (size_t i = 0; i < bytes; i += 4096) memory[i] = static_cast<unsigned char>(i / 4096);
  const auto increased = sampler.sample();
  std::uint64_t total = 0;
  size = sizeof(total);
  const bool totalRead = sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0 && total > 0;
  const double increase = increased.processRam - loaded.processRam;
  munmap(allocation, bytes);
  CHECK(totalRead && increase > 100.0 * bytes * 0.5 / total,
        "physical footprint grows after touching 64 MiB");
  std::cout << "CPU idle=" << idle.processCpu << " one-core=" << loaded.processCpu
            << " cores=" << cores << " system=" << loaded.systemCpu
            << " RAM system=" << increased.systemRam << " process=" << increased.processRam
            << " allocation_increase=" << increase << '\n';

  QElapsedTimer lifecycle;
  lifecycle.start();
  int beats = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
  heartbeat.start(20);
  {
    SystemLoadWidget widget;
    const auto label = [&](const char *name) { return widget.findChild<QLabel *>(QString::fromLatin1(name)); };
    auto *cpu = label("systemLoadCPU");
    auto *ram = label("processLoadRAM");
    CHECK(cpu && ram, "production widget has CPU and RAM metrics");
    QElapsedTimer wait;
    wait.start();
    while ((cpu->text().contains("--") || ram->text().contains("--")) && wait.elapsed() < 4000) {
      app.processEvents();
      QThread::msleep(5);
    }
    CHECK(!cpu->text().contains("--") && !ram->text().contains("--"), "worker publishes real metrics to GUI");
    for (const char *name : {"processLoadGPU"}) {
      const auto *metric = label(name);
      CHECK(metric && metric->text().contains("--"), "Metal without compositor timestamps remains unavailable");
    }
    CHECK(!label("systemLoadVRAM") && !label("processLoadVRAM"), "Mac omits both dedicated-VRAM columns");
    const auto *gpu = label("systemLoadGPU");
    CHECK(gpu && (idle.systemGpu < 0 || !gpu->text().contains("--")), "driver GPU is shown only when available");
    CHECK(label("processLoadGPU")->text().startsWith("Metal"), "application metric identifies Metal scope");
    CHECK(beats >= 20, "sampling keeps GUI heartbeat responsive");
    if (app.arguments().size() > 1)
      CHECK(widget.toolTip().contains(QStringLiteral("総RAM")), "Mac-specific explanation is localized");
    std::cout << "GUI " << cpu->text().toStdString() << ' ' << ram->text().toStdString()
              << " heartbeat=" << beats << '\n';
    lifecycle.restart();
  }
  CHECK(lifecycle.elapsed() < 1000, "sampling thread stops promptly when widget is destroyed");
  std::cout << "PASS: Mac load sampler and production widget\n";
  return 0;
}
