#include "operator_monitor_transport_mac.h"
#include "operator_monitor_widget.h"
#include <QApplication>
#include <QJsonDocument>
#include <QTimer>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("VTR-PON Return Monitor"));
  const auto args = app.arguments();
  if (args.size() != 4 || args[1] != QStringLiteral("fd:0")) return 2;
  bool valid = false;
  const auto parent = args[2].toLongLong(&valid);
  struct stat info{};
  if (!valid || parent <= 1 || getppid() != parent || fstat(STDIN_FILENO, &info) != 0 ||
      info.st_uid != geteuid() || info.st_size < sizeof(vtrpon::MacOperatorMonitorShared)) return 3;
  void *memory = mmap(nullptr, sizeof(vtrpon::MacOperatorMonitorShared), PROT_READ | PROT_WRITE,
                      MAP_SHARED, STDIN_FILENO, 0);
  close(STDIN_FILENO);
  if (memory == MAP_FAILED) return 4;
  auto &shared = *static_cast<vtrpon::MacOperatorMonitorShared *>(memory);
  if (shared.magic != vtrpon::operatorMonitorMacMagic || shared.version != vtrpon::operatorMonitorMacVersion) {
    munmap(memory, sizeof(shared)); return 5;
  }
  const rlimit noCore{0,0}; setrlimit(RLIMIT_CORE, &noCore);
  // Parent disappearance is checked independently of the receiver's Qt loop.
  std::atomic_bool done{false};
  std::thread watchdog([&] {
    while (!done.load()) {
      if (getppid() != parent) _exit(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  OperatorMonitorWidget monitor;
  monitor.setWindowTitle(args[3]); monitor.resize(960,540); monitor.show();
  std::uint64_t serial = 0, infoSerial = 0;
  auto lastFrame = vtrpon::operatorMonitorNowMs();
  auto lastReport = lastFrame;
  const bool diagnostics = qEnvironmentVariableIsSet("VTRPON_OPERATOR_MONITOR_DIAGNOSTICS");
  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, &app, [&] {
    const auto now = vtrpon::operatorMonitorNowMs();
    if (pthread_mutex_trylock(&shared.infoMutex) == 0) {
      QByteArray json;
      const auto &info = shared.presentation;
      if (info.serial != infoSerial && info.size <= sizeof(info.json)) {
        json = QByteArray(info.json, info.size); infoSerial = info.serial;
      }
      pthread_mutex_unlock(&shared.infoMutex);
      if (!json.isEmpty()) {
        const auto document = QJsonDocument::fromJson(json);
        if (document.isObject()) {
          monitor.setPresentation(document.object());
          monitor.setWindowTitle(document.object().value("title").toString(args[3]));
          shared.receiverInfoSerial.store(infoSerial, std::memory_order_release);
        }
      }
    }
    monitor.applyDisplay();
    if (now - lastFrame > 5000) { app.exit(3); return; }
    if (now - lastFrame > 1500 && !monitor.image.isNull()) { monitor.image = {}; monitor.update(); }
    if (pthread_mutex_trylock(&shared.videoMutex) == 0) {
      const auto &frame = shared.frame;
      if (frame.serial && frame.serial != serial && now >= frame.publishedAt && now - frame.publishedAt < 1500 &&
          frame.width > 0 && frame.height > 0 && frame.width <= vtrpon::operatorMonitorMaxWidth &&
          frame.height <= vtrpon::operatorMonitorMaxHeight) {
        monitor.image = QImage(frame.pixels, int(frame.width), int(frame.height), int(frame.width * 4), QImage::Format_RGB32).copy();
        serial = frame.serial; lastFrame = now;
        shared.receiverFrameSerial.store(serial, std::memory_order_release);
        monitor.update();
      }
      pthread_mutex_unlock(&shared.videoMutex);
    }
    if (serial && !monitor.property("ready").toBool()) {
      monitor.setProperty("ready", true); std::puts("OPERATOR_MONITOR_READY"); std::fflush(stdout);
    }
    if (diagnostics && now - lastReport >= 1000) {
      std::printf("OPERATOR_MONITOR_STATS %llu %llu %llu %d %d\n",
          (unsigned long long)now, (unsigned long long)serial, (unsigned long long)infoSerial,
          monitor.image.width(), monitor.image.height());
      std::fflush(stdout); lastReport = now;
    }
  });
  poll.setTimerType(Qt::PreciseTimer); poll.start(16);
  const int result = app.exec();
  done = true; watchdog.join();
  munmap(memory, sizeof(shared));
  return result;
}
