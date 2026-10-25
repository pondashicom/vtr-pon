#include "operator_monitor_transport.h"
#include "operator_monitor_widget.h"
#include <QJsonDocument>
#include <QApplication>
#include <QIcon>
#include <QTimer>
#include <QWidget>
#include <QPainter>
#include <QImage>
#include <cstdio>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QIcon appIcon;
  for (const int size : {16, 32, 48, 64, 128, 256}) {
    appIcon.addFile(QStringLiteral(":/vtrpon/assets/icons/vtr-pon3/windows/png/VTR-PON3_%1x%1.png")
                        .arg(size));
  }
  app.setWindowIcon(appIcon);
  const auto args = app.arguments();
  if (args.size() != 4) return 1;
  bool valid = false;
  const DWORD parentPid = args[2].toULong(&valid);
  if (!valid || !parentPid) return 1;
  HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
  if (!parent) return 1;
  HANDLE mapping = nullptr, mutex = nullptr;
  HANDLE infoMapping = nullptr, infoMutex = nullptr;
  const vtrpon::OperatorMonitorPresentation *info = nullptr;
  std::uint64_t infoSerial = 0;
  const vtrpon::OperatorMonitorFrame *frame = nullptr;
  OperatorMonitorWidget monitor;
  monitor.setWindowTitle(args[3]);
  monitor.resize(960, 540);
  monitor.show();
  const auto name = args[1].toStdWString();
  const auto infoName = name + L"-info";
  std::uint64_t serial = 0;
  auto lastFrameAt = GetTickCount64();
  const bool diagnostics = qEnvironmentVariableIsSet("VTRPON_OPERATOR_MONITOR_DIAGNOSTICS");
  auto lastReportAt = lastFrameAt;
  std::uint64_t receivedFrames = 0;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &app, [&] {
    const auto now = GetTickCount64();
    if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) { app.quit(); return; }
    if (!infoMapping) infoMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, infoName.c_str());
    if (!infoMutex) infoMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                          (infoName + L"-mutex").c_str());
    if (infoMapping && !info) info = static_cast<const vtrpon::OperatorMonitorPresentation *>(
        MapViewOfFile(infoMapping, FILE_MAP_READ, 0, 0, sizeof(vtrpon::OperatorMonitorPresentation)));
    if (info && infoMutex) {
      QByteArray json;
      const DWORD acquired = WaitForSingleObject(infoMutex, 0);
      if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) {
        if (info->serial != infoSerial && info->size <= sizeof(info->json)) {
          json = QByteArray(info->json, info->size);
          infoSerial = info->serial;
        }
        ReleaseMutex(infoMutex);
      }
      if (!json.isEmpty()) {
        const auto document = QJsonDocument::fromJson(json);
        if (document.isObject()) {
          monitor.setPresentation(document.object());
          monitor.setWindowTitle(document.object().value("title").toString(args[3]));
        }
      }
    }
    monitor.applyDisplay();
    if (now - lastFrameAt > 5000) { app.exit(3); return; }
    if (now - lastFrameAt > 1500 && !monitor.image.isNull()) {
      monitor.image = QImage();
      monitor.update();
    }
    if (!mapping) mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mutex) mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                   (name + L"-mutex").c_str());
    if (mapping && !frame) frame = static_cast<const vtrpon::OperatorMonitorFrame *>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(vtrpon::OperatorMonitorFrame)));
    if (!frame || !mutex) return;
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) return;
    if (frame->serial && serial != frame->serial && now - frame->publishedAt < 1500 &&
        frame->width > 0 && frame->height > 0 &&
        frame->width <= vtrpon::operatorMonitorMaxWidth &&
        frame->height <= vtrpon::operatorMonitorMaxHeight) {
      monitor.image = QImage(frame->pixels, frame->width, frame->height,
          frame->width * 4, QImage::Format_RGB32).copy();
      if (!serial) { std::puts("OPERATOR_MONITOR_READY"); std::fflush(stdout); }
      serial = frame->serial;
      lastFrameAt = now;
      ++receivedFrames;
      monitor.update();
    }
    ReleaseMutex(mutex);
    if (diagnostics && now - lastReportAt >= 1000) {
      std::printf("OPERATOR_MONITOR_STATS %llu %llu %llu %d %d\n",
          static_cast<unsigned long long>(now),
          static_cast<unsigned long long>(receivedFrames),
          static_cast<unsigned long long>(serial), monitor.image.width(), monitor.image.height());
      std::fflush(stdout);
      lastReportAt = now;
    }
  });
  // Poll faster than the producer, repaint only when a new frame arrives.
  timer.setTimerType(Qt::PreciseTimer);
  timer.start(16);
  const int result = app.exec();
  if (frame) UnmapViewOfFile(frame);
  if (mapping) CloseHandle(mapping);
  if (mutex) CloseHandle(mutex);
  if (info) UnmapViewOfFile(info);
  if (infoMapping) CloseHandle(infoMapping);
  if (infoMutex) CloseHandle(infoMutex);
  CloseHandle(parent);
  return result;
}
