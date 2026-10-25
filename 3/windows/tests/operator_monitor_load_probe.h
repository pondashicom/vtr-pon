#pragma once

#include "system_load_widget.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QLabel>
#include <QProcessEnvironment>
#include <QTextStream>
#include <psapi.h>
#include <array>

// Opt-in measurement using the production compositor, receiver and load widget.
// It is deliberately not part of CTest's short fault-isolation run.
int measureOperatorMonitorLoad(QApplication &app) {
  const auto args = app.arguments();
  const int option = args.indexOf(QStringLiteral("--measure-load"));
  CHECK(option >= 0 && option + 1 < args.size(), "measurement CSV path supplied");
  QFile csv(args[option + 1]);
  CHECK(csv.open(QIODevice::WriteOnly | QIODevice::Text), "measurement CSV opens");
  QTextStream out(&csv);
  out << "phase,second,pc_cpu,pc_gpu,pc_ram,pc_vram,main_cpu,main_gpu,main_ram,main_vram,"
         "helper_cpu,helper_ram_mb,program_fps,render_fps,received_fps,published_fps,receiver_width,receiver_height\n";

  gst_init(nullptr, nullptr);
  QWidget primary, onAir, controls;
  const QRect work = app.primaryScreen()->availableGeometry();
  // Keep display-driven measurements visible when the desktop app regains
  // focus. These flags apply only to the probe, never to the product windows.
  primary.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  onAir.setWindowFlag(Qt::WindowStaysOnTopHint, true);
  primary.setWindowTitle(QStringLiteral("Program Out load measurement"));
  primary.setGeometry(work.x() + 10, work.y() + 40, 640, 360);
  onAir.setGeometry(work.x() + 10, work.y() + 450, 592, 333);
  primary.show();
  onAir.show();
  SystemLoadWidget load;
  load.setWindowFlag(Qt::WindowStaysOnTopHint, true);
  load.setWindowTitle(QStringLiteral("VTR-PON load monitor - measurement"));
  load.move(work.x() + 10, work.bottom() - 100);
  load.show();
  QMenuBar bar(&controls);
  vtrpon::ViewMenuController menus(&bar, &primary, &controls);
  auto *action = bar.findChild<QAction *>(QStringLiteral("operatorMonitorAction"));
  auto *receiver = menus.findChild<QProcess *>();
  CHECK(action && receiver, "production menu and receiver exist");
  receiver->setStandardOutputFile(QString());
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("VTRPON_OPERATOR_MONITOR_DIAGNOSTICS"), QStringLiteral("1"));
  receiver->setProcessEnvironment(environment);
  {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("operatorMonitor/information", true);
    settings.sync();
  }
  menus.reloadOperatorMonitorSettings();
  menus.operatorMonitorStatusProvider = [] {
    return QStringList{"00:01:00.00", "Color_Bars_Motion_1080p30_180s.mp4", "Play / Repeat", "1x", {}};
  };
  const bool fourK = args.contains(QStringLiteral("--output-4k"));
  ProgramGpuCompositor compositor(primary.winId(), onAir.winId(),
      fourK ? ProgramGpuCompositor::OutputGeometry{3840, 2160} : ProgramGpuCompositor::OutputGeometry{});
  CHECK(compositor.start(), "measurement Program compositor starts");
  const auto originalHandle = primary.winId();
  const QString media = QDir(app.applicationDirPath()).filePath(
      QStringLiteral("assets/testpatterns/media/Color_Bars_Motion_1080p30_180s.mp4"));
  CHECK(QFileInfo::exists(media), "180-second moving video exists");
  GError *parseError = nullptr;
  GstElement *inputRaw = gst_parse_launch(
      "filesrc name=source ! qtdemux ! h264parse ! d3d11h264dec ! d3d11convert ! "
      "video/x-raw(memory:D3D11Memory),format=BGRA ! "
      "appsink name=video sync=true max-buffers=1 drop=true", &parseError);
  const auto releaseInput = [](GstElement *input) {
    if (input) { gst_element_set_state(input, GST_STATE_NULL); gst_object_unref(input); }
  };
  std::unique_ptr<GstElement, decltype(releaseInput)> input(inputRaw, releaseInput);
  if (parseError) { std::cerr << parseError->message << std::endl; g_error_free(parseError); return 1; }
  CHECK(input, "measurement decode pipeline creates");
  GstElement *source = gst_bin_get_by_name(GST_BIN(input.get()), "source");
  g_object_set(source, "location", media.toUtf8().constData(), nullptr);
  gst_object_unref(source);
  GstElement *sink = gst_bin_get_by_name(GST_BIN(input.get()), "video");
  const bool connected = compositor.configureDeckInput(0, input.get(), sink);
  gst_object_unref(sink);
  CHECK(connected, "real video feeds production compositor");
  CHECK(gst_element_set_state(input.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
        "measurement video starts");
  CHECK(waitFor([&] { return compositor.deckFrameReady(0); }), "decoded video frame arrives");
  CHECK(compositor.setDeckLayers(0, 1.0f, -1, 0.0f), "decoded video is on Program");
  std::cout << "MEASUREMENT source=1080p30-H264 output=" << (fourK ? "3840x2160" : "1920x1080")
            << " receiver=" << vtrpon::operatorMonitorMaxWidth << 'x' << vtrpon::operatorMonitorMaxHeight
            << '/' << vtrpon::operatorMonitorFramesPerSecond
            << " load=production-SystemLoadWidget process_scope=probe helper_scope=separate"
            << " information=ON" << std::endl;
  for (auto *screen : app.screens())
    std::cout << "DISPLAY " << screen->name().toStdString() << ' '
              << screen->geometry().width() << 'x' << screen->geometry().height()
              << " refresh=" << screen->refreshRate() << " dpr=" << screen->devicePixelRatio() << std::endl;

  struct ReceiverStats {
    quint64 time = 0, received = 0, published = 0;
    int width = 0, height = 0;
  } stats;
  QByteArray pendingOutput;
  const auto drain = [&] {
    if (!receiver->isOpen()) return;
    pendingOutput += receiver->readAllStandardOutput();
    for (int end; (end = pendingOutput.indexOf('\n')) >= 0;) {
      const auto fields = pendingOutput.left(end).trimmed().split(' ');
      pendingOutput.remove(0, end + 1);
      if (fields.size() == 6 && fields[0] == "OPERATOR_MONITOR_STATS")
        stats = {fields[1].toULongLong(), fields[2].toULongLong(), fields[3].toULongLong(),
                 fields[4].toInt(), fields[5].toInt()};
    }
  };
  const auto processTicks = [](HANDLE process) -> quint64 {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (quint64(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
           (quint64(user.dwHighDateTime) << 32) + user.dwLowDateTime;
  };
  const std::array<QString, 8> labelNames{
      "systemLoadCPU", "systemLoadGPU", "systemLoadRAM", "systemLoadVRAM",
      "processLoadCPU", "processLoadGPU", "processLoadRAM", "processLoadVRAM"};
  std::array<QLabel *, 8> loadLabels{};
  for (size_t i = 0; i < labelNames.size(); ++i) {
    loadLabels[i] = load.findChild<QLabel *>(labelNames[i]);
    CHECK(loadLabels[i], "production load metric exists");
  }
  const auto number = [](double value) {
    return value >= 0 ? QString::number(value, 'f', 3) : QString();
  };
  const std::array<const char *, 4> phases{"off_before", "on_window", "on_large", "off_after"};
  bool healthy = true;
  for (int phase = 0; phase < int(phases.size()); ++phase) {
    HANDLE helper = nullptr;
    if (phase == 1) {
      action->setChecked(true);
      CHECK(waitFor([&] { return receiver->state() == QProcess::Running &&
          receiverWindow(DWORD(receiver->processId())); }), "measurement receiver starts");
    }
    if (phase == 3) {
      action->setChecked(false);
      CHECK(waitFor([&] { return receiver->state() == QProcess::NotRunning; }), "measurement receiver stops");
    }
    const bool enabled = phase == 1 || phase == 2;
    if (enabled) {
      HWND window = receiverWindow(DWORD(receiver->processId()));
      if (phase == 1)
        SetWindowPos(window, HWND_TOPMOST, work.x() + 680, work.y() + 40, 976, 579, SWP_NOACTIVATE);
      else
        SetWindowPos(window, HWND_TOPMOST, work.x() + 200, work.y() + 100,
                     work.width() - 230, work.height() - 210, SWP_NOACTIVATE);
      helper = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, DWORD(receiver->processId()));
      CHECK(helper, "measurement can sample receiver CPU and memory");
    }
    std::cout << "MEASUREMENT_PHASE " << phases[phase] << " warmup=5s samples=20" << std::endl;
    QElapsedTimer warmup;
    warmup.start();
    waitFor([&] { drain(); return warmup.elapsed() >= 5000; }, 5200);
    quint64 lastTicks = processTicks(helper);
    quint64 lastPresents = compositor.presentedFrameCount();
    quint64 lastRenders = compositor.renderedFrameCount();
    auto lastStats = stats;
    const auto phaseStartStats = stats;
    QElapsedTimer duration;
    duration.start();
    qint64 lastSample = 0;
    for (int sample = 1; sample <= 20; ++sample) {
      waitFor([&] { drain(); return duration.elapsed() >= sample * 1000; }, 1300);
      const qint64 elapsed = duration.elapsed();
      const double seconds = double(elapsed - lastSample) / 1000;
      QStringList row{QString::fromLatin1(phases[phase]), QString::number(sample)};
      for (auto *label : loadLabels) {
        QString value = label->text().section(' ', -1);
        value.remove('%');
        row << (value == "--" ? QString() : value);
      }
      const auto ticks = processTicks(helper);
      const double helperCpu = helper ? double(ticks - lastTicks) / 100000.0 / seconds /
          GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) : -1;
      PROCESS_MEMORY_COUNTERS memory{};
      const double helperRam = helper && K32GetProcessMemoryInfo(helper, &memory, sizeof(memory))
          ? double(memory.WorkingSetSize) / (1024 * 1024) : -1;
      const auto presents = compositor.presentedFrameCount();
      const auto renders = compositor.renderedFrameCount();
      const double programFps = double(presents - lastPresents) / seconds;
      const double statsSeconds = enabled && stats.time > lastStats.time
          ? double(stats.time - lastStats.time) / 1000 : 0;
      row << number(helperCpu) << number(helperRam) << number(programFps)
          << number(double(renders - lastRenders) / seconds)
          << number(statsSeconds > 0 ? double(stats.received - lastStats.received) / statsSeconds : -1)
          << number(statsSeconds > 0 ? double(stats.published - lastStats.published) / statsSeconds : -1)
          << (enabled ? QString::number(stats.width) : QString())
          << (enabled ? QString::number(stats.height) : QString());
      out << row.join(',') << '\n';
      out.flush();
      std::string error;
      healthy &= primary.winId() == originalHandle && compositor.isAvailable() &&
                 !compositor.takeRuntimeError(error) && programFps >= 28;
      if (!error.empty()) std::cerr << error << std::endl;
      if (enabled) healthy &= receiver->state() == QProcess::Running &&
          stats.width == vtrpon::operatorMonitorMaxWidth && stats.height == vtrpon::operatorMonitorMaxHeight;
      lastSample = elapsed;
      lastTicks = ticks;
      lastPresents = presents;
      lastRenders = renders;
      lastStats = stats;
    }
    if (enabled) {
      const double span = double(stats.time - phaseStartStats.time) / 1000;
      const double receiveRate = span > 0 ? double(stats.received - phaseStartStats.received) / span : 0;
      healthy &= receiveRate >= 0.95 * vtrpon::operatorMonitorFramesPerSecond;
      std::cout << "MEASUREMENT_RATE " << phases[phase] << " received=" << receiveRate << std::endl;
    }
    load.grab().save(QFileInfo(csv).absolutePath() + "/load-" + phases[phase] + ".png");
    if (helper) CloseHandle(helper);
  }
  gst_element_set_state(input.get(), GST_STATE_NULL);
  CHECK(compositor.stop(), "measurement Program stops normally");
  CHECK(healthy, "Program stayed healthy and receiver delivered the configured resolution and frame rate");
  return 0;
}
