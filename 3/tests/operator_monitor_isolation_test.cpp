#include "program_gpu_compositor.h"
#include "network_video.h"
#include "network_video_addon_api.h"
#include <QLibrary>
#include <array>
#include <cmath>
#include "operator_monitor_output.h"
#include "operator_monitor_transport.h"
#include "view_menu_controller.h"
#include "operator_monitor_settings_page.h"
#include "operator_monitor_widget.h"
#include <QTemporaryDir>
#include <QTranslator>
#include <QJsonDocument>
#include <QApplication>
#include <QAction>
#include <QMenuBar>
#include <QProcess>
#include <QElapsedTimer>
#include <QScreen>
#include <QThread>
#include <QImage>
#include <QPixmap>
#include <gst/gst.h>
#include <tlhelp32.h>
#include <functional>
#include <iostream>

namespace {
bool waitFor(const std::function<bool()> &predicate, int timeout = 5000) {
  QElapsedTimer timer;
  timer.start();
  do {
    QApplication::processEvents();
    if (predicate()) return true;
    QThread::msleep(10);
  } while (timer.elapsed() < timeout);
  return false;
}
void pump(int duration) {
  QElapsedTimer timer;
  timer.start();
  waitFor([&] { return timer.elapsed() >= duration; }, duration + 100);
}
HWND receiverWindow(DWORD pid) {
  struct Search { DWORD pid; HWND hwnd; } search{pid, nullptr};
  EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
    auto &search = *reinterpret_cast<Search *>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search.pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
      search.hwnd = hwnd;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.hwnd;
}
COLORREF centerPixel(HWND window) {
  RECT r{};
  GetClientRect(window, &r);
  POINT point{r.right / 2, r.bottom / 2};
  ClientToScreen(window, &point);
  HDC dc = GetDC(nullptr);
  const auto pixel = GetPixel(dc, point.x, point.y);
  ReleaseDC(nullptr, dc);
  return pixel;
}
COLORREF edgePixel(HWND window) {
  RECT r{};
  GetClientRect(window, &r);
  POINT point{r.right / 50, r.bottom / 4};
  ClientToScreen(window, &point);
  HDC dc = GetDC(nullptr);
  const auto pixel = GetPixel(dc, point.x, point.y);
  ReleaseDC(nullptr, dc);
  return pixel;
}
bool green(COLORREF value) {
  return value != CLR_INVALID && GetGValue(value) > 200 && GetRValue(value) < 40;
}
bool red(COLORREF value) {
  return value != CLR_INVALID && GetRValue(value) > 200 && GetGValue(value) < 40;
}
bool freeze(DWORD pid) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  THREADENTRY32 entry{sizeof(THREADENTRY32)};
  bool suspended = false;
  if (Thread32First(snapshot, &entry)) do {
    if (entry.th32OwnerProcessID != pid) continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
    if (thread) {
      suspended |= SuspendThread(thread) != DWORD(-1);
      CloseHandle(thread);
    }
  } while (Thread32Next(snapshot, &entry));
  CloseHandle(snapshot);
  return suspended;
}
}

#define CHECK(condition, message) do { if (!(condition)) { \
  std::cerr << "FAIL: " << message << '\n'; return 1; \
} else std::cout << "PASS: " << message << std::endl; } while (false)

#include "operator_monitor_load_probe.h"
#include "fullscreen_cadence_probe.h"
#include "operator_countdown_timer_test.h"

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  // This fault fixture deliberately holds the mailbox forever. It is a separate
  // process so killing it also exercises Windows' abandoned-mutex recovery.
  if (app.arguments().contains(QStringLiteral("--hold-mailbox"))) {
    const auto name = (app.arguments().last() + QStringLiteral("-mutex")).toStdWString();
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
    if (!mutex || WaitForSingleObject(mutex, 2000) != WAIT_OBJECT_0) return 2;
    std::cout << "LOCKED" << std::endl;
    Sleep(INFINITE);
    return 0;
  }
  QTemporaryDir testSettings;
  CHECK(testSettings.isValid(), "isolated settings directory");
  app.setProperty("vtrpon.settingsFile", testSettings.filePath("settings.ini"));
  if (app.arguments().contains(QStringLiteral("--measure-fullscreen-cadence")))
    return measureFullscreenCadence(app);
  if (app.arguments().contains(QStringLiteral("--measure-load")))
    return measureOperatorMonitorLoad(app);
  QTranslator translator;
  CHECK(translator.load(app.applicationDirPath() + "/vtrpon_ja.qm"), "Japanese catalog loads");
  app.installTranslator(&translator);
  CHECK(runOperatorCountdownTimerTest(), "countdown settings, clock, warning, receiver and cue regression");
  if (app.arguments().contains(QStringLiteral("--timer-only"))) return 0;
  OperatorMonitorSettingsPage settingsPage;
  settingsPage.resize(650, 470);
  OperatorTimerSettingsDialog timerSettings;
  auto *infoCheck = settingsPage.findChild<QCheckBox *>("operatorMonitorInformation");
  auto *rateCheck = settingsPage.findChild<QCheckBox *>("operatorMonitorField_rate");
  auto *cueInput = timerSettings.findChild<QLineEdit *>("operatorMonitorCueInput");
  auto *sendCue = timerSettings.findChild<QPushButton *>("operatorMonitorCueSend");
  auto *clearCue = timerSettings.findChild<QPushButton *>("operatorMonitorCueClear");
  CHECK(infoCheck && rateCheck && cueInput && sendCue && clearCue, "settings controls exist");
  CHECK(!infoCheck->isChecked() && !rateCheck->isEnabled(), "information defaults OFF");
  infoCheck->setChecked(true);
  rateCheck->setChecked(false);
  settingsPage.reload();
  CHECK(infoCheck->isChecked() && rateCheck->isEnabled() && !rateCheck->isChecked(), "field selections persist");
  QString sentCue;
  timerSettings.cueSent = [&](const QString &text) { sentCue = text; };
  timerSettings.cueCleared = [&] { sentCue.clear(); };
  cueInput->setText(QString::fromUtf8("次の説明に進んでください"));
  CHECK(sentCue.isEmpty(), "typing does not transmit cue card");
  sendCue->click();
  CHECK(sentCue == cueInput->text(), "Send transmits the entered cue");
  settingsPage.grab().save(app.applicationDirPath() + "/operator-monitor-settings.png");
  clearCue->click();
  CHECK(sentCue.isEmpty() && cueInput->text().isEmpty(), "Delete clears cue and input");
  OperatorMonitorWidget preview;
  preview.resize(960, 540);
  preview.image = QImage(960, 540, QImage::Format_RGB32);
  preview.image.fill(Qt::green);
  QJsonArray previewFields{
      QJsonObject{{"key", "remaining"}, {"label", QString::fromUtf8("残り時間")}, {"value", "00:02:15"}},
      QJsonObject{{"key", "file"}, {"label", QString::fromUtf8("ファイル名")}, {"value", QString::fromUtf8("会場案内_オープニング.mp4")}},
      QJsonObject{{"key", "modes"}, {"label", "START / END"}, {"value", "PLAY / NEXT"}},
      QJsonObject{{"key", "rate"}, {"label", QString::fromUtf8("再生速度")}, {"value", "1.25x"}},
      QJsonObject{{"key", "pages"}, {"label", QString::fromUtf8("ページ数（総 / 現在）")}, {"value", "12 / 3"}}};
  QJsonObject presentation{{"information", true}, {"fields", previewFields}};
  preview.setPresentation(presentation);
  CHECK(preview.videoRect() == QRect(96, 0, 768, 432), "16:9 video occupies upper 80 percent with pillarboxes");
  QImage infoImage = preview.grab().toImage();
  CHECK(infoImage.pixelColor(10, 100) == Qt::black && infoImage.pixelColor(480, 100) == Qt::green,
        "information layout preserves image and black sidebars");
  infoImage.save(app.applicationDirPath() + "/operator-monitor-information.png");
  presentation.insert("cue", QString::fromUtf8("次の説明に進んでください"));
  preview.setPresentation(presentation);
  const QImage cueImage = preview.grab().toImage();
  cueImage.save(app.applicationDirPath() + "/operator-monitor-cue.png");
  presentation.insert("fields", QJsonArray{});
  preview.setPresentation(presentation);
  CHECK(preview.grab().toImage() == cueImage, "cue replaces every other overlay in the band");
  CHECK(cueImage.pixelColor(480, 100) == Qt::green, "cue retains Program video above the band");
  presentation.remove("cue");
  presentation.insert("information", false);
  preview.setPresentation(presentation);
  CHECK(preview.videoRect() == QRect(0, 0, 960, 540), "information OFF restores full image area");
  // Keep the fault scenarios independent of settings saved by the user.
  { auto settings = vtrpon::makeControllerSettings(); settings.clear(); settings.sync(); }
  gst_init(&argc, &argv);
  QWidget primary, existingMonitor, controls;
  const QRect screen = app.primaryScreen()->availableGeometry();
  // Pixel probes read the visible desktop, so keep this test's source above
  // unrelated desktop windows during the short rendering checks.
  primary.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  primary.setGeometry(screen.x() + 10, screen.y() + 40, 640, 360);
  primary.setWindowTitle(QStringLiteral("Program Out isolation test"));
  primary.show();
  existingMonitor.setWindowFlag(Qt::WindowStaysOnTopHint, true);
  existingMonitor.setGeometry(screen.x() + 10, screen.y() + 430, 320, 180);
  existingMonitor.show();
  QMenuBar menuBar(&controls);
  vtrpon::ViewMenuController menus(&menuBar, &primary, &controls);
  auto *action = menuBar.findChild<QAction *>(QStringLiteral("operatorMonitorAction"));
  auto *receiver = menus.findChild<QProcess *>();
  CHECK(action && receiver && !action->isChecked(), "menu starts OFF");
  const bool fourK = app.arguments().contains(QStringLiteral("--output-4k"));
  ProgramGpuCompositor compositor(primary.winId(), existingMonitor.winId(),
      fourK ? ProgramGpuCompositor::OutputGeometry{3840, 2160}
            : ProgramGpuCompositor::OutputGeometry{});
  CHECK(compositor.start(), "real Program GPU compositor starts");
  if (app.arguments().contains(QStringLiteral("--ndi-alpha-only"))) {
    vtrpon::NetworkVideoRuntime runtime;
    vtrpon::NetworkVideoSettings outputSettings{true, false, QStringLiteral("VTR-PON GPU alpha test")};
    outputSettings.fillEnabled = outputSettings.keyEnabled = outputSettings.alphaEnabled = true;
    vtrpon::saveNetworkVideoSettings(outputSettings);
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask() == 15; }, 10000), "all four senders ready");
    QLibrary library(vtrpon::networkVideoAddonPath());
    const auto get = reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
    CHECK(get, "NDI add-on loads");
    const auto *api = get(VTRPON_NETWORK_VIDEO_ABI);
    CHECK(api, "NDI ABI 2");
    char message[1024]{};
    void *context = api->create(message, sizeof(message));
    CHECK(context, "NDI Runtime available");
    struct NdiCleanup {
      const VtrPonNetworkVideoApi *api; void *context; std::array<void *, 4> receivers{};
      ~NdiCleanup() { for (auto *receiver : receivers) if (receiver) api->destroyReceiver(context, receiver); api->destroy(context); }
    } ndi{api, context};
    for (unsigned i = 0; i < 4; ++i) {
      ndi.receivers[i] = api->createReceiver(context, vtrpon::networkVideoState().outputs[i].name.toUtf8().constData(), false);
      CHECK(ndi.receivers[i], "NDI test receiver");
    }
    struct Pixel {
      int r = -1, g = -1, b = -1, a = -1; qint64 timecode = 0;
      static void receive(void *value, const VtrPonNetworkVideoFrame *frame) {
        auto &pixel = *static_cast<Pixel *>(value);
        const auto *p = frame->data + (frame->height / 2) * frame->stride + (frame->width / 2) * 4;
        pixel = {p[2], p[1], p[0], frame->hasAlpha ? p[3] : 255, frame->timecode};
      }
    };
    const auto checkPixel = [&](unsigned kind, int r, int g, int b, int a) {
      Pixel pixel;
      const auto after = vtrpon::networkVideoTimecode();
      const bool ok = waitFor([&] {
        api->receive(context, ndi.receivers[kind], false, Pixel::receive, nullptr, &pixel, 10);
        return pixel.timecode >= after && std::abs(pixel.r - r) <= 10 && std::abs(pixel.g - g) <= 10
          && std::abs(pixel.b - b) <= 10 && std::abs(pixel.a - a) <= 3;
      }, 10000);
      if (!ok) std::cerr << "NDI kind " << kind << " got RGBA " << pixel.r << ',' << pixel.g << ',' << pixel.b << ',' << pixel.a
        << " expected " << r << ',' << g << ',' << b << ',' << a << std::endl;
      return ok;
    };
    compositor.setBackgroundColor(0x00ff00);
    CHECK(checkPixel(0, 0, 255, 0, 255), "Composite includes background");
    CHECK(checkPixel(3, 0, 0, 0, 0), "empty Program remains transparent");
    struct Sources {
      std::array<GstElement *, 2> pipelines{};
      ~Sources() { for (auto *p : pipelines) if (p) { gst_element_set_state(p, GST_STATE_NULL); gst_object_unref(p); } }
    } sources;
    for (unsigned i = 0; i < 2; ++i) {
      const auto description = QStringLiteral("videotestsrc is-live=true pattern=solid-color foreground-color=%1 ! "
        "video/x-raw,format=BGRA,width=64,height=36,framerate=60/1 ! d3d11upload ! "
        "video/x-raw(memory:D3D11Memory),format=BGRA ! appsink name=alpha-input max-buffers=2 drop=true sync=true")
        .arg(i == 0 ? qulonglong(0x80ff0000u) : qulonglong(0x800000ffu));
      auto *pipeline = gst_parse_launch(description.toUtf8().constData(), nullptr);
      CHECK(pipeline, "alpha source pipeline"); sources.pipelines[i] = pipeline;
      auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "alpha-input");
      const bool configured = i == 0 ? compositor.configureDeckInput(0, pipeline, sink)
        : compositor.configureDskInput(0, pipeline, sink);
      gst_object_unref(sink);
      CHECK(configured, "alpha source uses Program GPU context");
      CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "alpha source starts");
    }
    CHECK(waitFor([&] { return compositor.deckFrameReady(0) && compositor.dskFrameReady(0); }), "alpha samples decoded and uploaded");
    compositor.setDeckLayers(0, 1, -1, 0);
    CHECK(checkPixel(3, 255, 0, 0, 128), "straight alpha preserves source coverage");
    CHECK(checkPixel(0, 128, 127, 0, 255), "Composite flattens red over green once");
    CHECK(checkPixel(1, 255, 0, 0, 255), "straight Fill is unassociated color");
    CHECK(checkPixel(2, 128, 128, 128, 255), "Key is linear grayscale coverage");
    compositor.setItemFadeProgress(0.5F);
    CHECK(checkPixel(3, 255, 0, 0, 64), "item fade scales alpha");
    compositor.setDskLayers(0, 1, -1, 0);
    CHECK(checkPixel(3, 51, 0, 204, 160), "DSK alpha-over follows item group fade");
    compositor.setFtbCoverage(0.5F);
    CHECK(checkPixel(3, 51, 0, 204, 80), "FTB scales composed color and alpha together");
    compositor.setFtbCoverage(1);
    CHECK(checkPixel(3, 0, 0, 0, 0), "FTB clears transparent outputs");
    CHECK(checkPixel(0, 0, 255, 0, 255), "FTB preserves Composite selected background");
    compositor.setFtbCoverage(0); compositor.setDeckLayers(-1, 0, -1, 0);
    CHECK(checkPixel(3, 0, 0, 255, 128), "OFF AIR base does not erase remaining DSK");
    compositor.setDskLayers(-1, 0, -1, 0);
    CHECK(checkPixel(3, 0, 0, 0, 0), "cleared layers remain transparent");
    runtime.requestShutdown();
    CHECK(waitFor([&] { return runtime.shutdownComplete(); }), "all NDI workers stop");
    CHECK(waitFor([&] { return compositor.prepareShutdown(); }), "GPU callbacks drained");
    CHECK(compositor.stop(), "GPU stops");
    std::cout << "NDI GPU alpha: composition, Fill/Key, fades, DSK and FTB passed" << std::endl;
    return 0;
  }
  const WId originalHandle = primary.winId();
  std::string error;
  const auto continuous = [&] (const char *label) {
    const auto start = compositor.presentedFrameCount();
    pump(1100);
    const auto delta = compositor.presentedFrameCount() - start;
    std::cout << label << " fullscreen presents=" << delta << std::endl;
    return delta >= 30 && primary.winId() == originalHandle &&
           compositor.isAvailable() && !compositor.takeRuntimeError(error);
  };
  CHECK(continuous("baseline"), "baseline Program presentation continues");
  compositor.setBackgroundColor(0x00ff00);
  CHECK(waitFor([&] { return green(centerPixel(reinterpret_cast<HWND>(primary.winId()))); }),
        "actual Program window displays the green composition");
  CHECK(waitFor([&] { return green(centerPixel(reinterpret_cast<HWND>(existingMonitor.winId()))); }),
        "ON AIR monitor displays the same composition after conversion");
  const auto staysGreen = [&] {
    const auto firstFrame = compositor.presentedFrameCount();
    return waitFor([&] {
      return compositor.presentedFrameCount() >= firstFrame + 4 &&
             green(centerPixel(reinterpret_cast<HWND>(primary.winId())));
    });
  };
  compositor.setFtbCoverage(0.5F);
  CHECK(staysGreen(), "partial FTB preserves the selected background color");
  compositor.setFtbCoverage(1.0F);
  CHECK(staysGreen(), "full FTB ends at the selected background color");
  compositor.setFtbCoverage(0.0F);
  compositor.setItemFadeProgress(0.5F);
  CHECK(staysGreen(), "partial item fade preserves the selected background color");
  compositor.setItemFadeProgress(0.0F);
  CHECK(staysGreen(), "item fade-out ends at the selected background color");
  compositor.setItemFadeProgress(1.0F);
  primary.hide();
  const auto hiddenRenderStart = compositor.renderedFrameCount();
  compositor.setBackgroundColor(0xff0000);
  pump(1100);
  CHECK(compositor.renderedFrameCount() - hiddenRenderStart >= 45 &&
            compositor.isAvailable() && !compositor.takeRuntimeError(error),
        "hidden fullscreen does not block the compositor");
  primary.show();
  CHECK(waitFor([&] { return red(centerPixel(reinterpret_cast<HWND>(primary.winId()))); }),
        "restored fullscreen displays the latest composition");
  CHECK(continuous("fullscreen restored"), "fullscreen cadence recovers after hiding");
  compositor.setBackgroundColor(0x00ff00);
  CHECK(waitFor([&] { return green(centerPixel(reinterpret_cast<HWND>(primary.winId()))); }),
        "restored fullscreen accepts subsequent compositions");
  const auto startReceiver = [&] {
    action->setChecked(true);
    if (!waitFor([&] { return receiver->state() == QProcess::Running; })) return HWND{};
    HWND window = nullptr;
    if (!waitFor([&] { window = receiverWindow(DWORD(receiver->processId())); return window != nullptr; })) return HWND{};
    SetWindowPos(window, HWND_TOPMOST, screen.x() + 680, screen.y() + 40, 640, 400, SWP_NOACTIVATE);
    return window;
  };
  HWND window = startReceiver();
  CHECK(window, "menu launches a separate receiver window");
  CHECK(waitFor([&] { return green(centerPixel(window)); }), "receiver displays the same green composition");
  menus.operatorMonitorStatusProvider = [] {
    return QStringList{"00:01:00", "sample.mp4", "Play / Next", "1x", "12 / 3"};
  };
  {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("operatorMonitor/information", true);
    settings.sync();
  }
  menus.reloadOperatorMonitorSettings();
  CHECK(waitFor([&] { return edgePixel(window) == RGB(0, 0, 0); }),
        "live settings resize only receiver image and add sidebars");
  pump(200);
  const auto receiverImage = [&] { return app.primaryScreen()->grabWindow(WId(window)).toImage(); };
  const QImage receiverInfo = receiverImage();
  menus.setOperatorMonitorCue(QStringLiteral("TEST CUE"));
  CHECK(waitFor([&] { return receiverImage() != receiverInfo; }), "cue reaches the actual receiver process");
  CHECK(green(centerPixel(reinterpret_cast<HWND>(primary.winId()))), "cue leaves primary Program pixels unchanged");
  menus.setOperatorMonitorCue({});
  CHECK(waitFor([&] { return receiverImage() == receiverInfo; }), "cue deletion restores live information fields");
  {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("operatorMonitor/information", false);
    settings.sync();
  }
  menus.reloadOperatorMonitorSettings();
  CHECK(waitFor([&] { return green(edgePixel(window)); }), "live information OFF restores receiver image size");
  compositor.setBackgroundColor(0xff0000);
  CHECK(waitFor([&] { return red(centerPixel(window)) &&
      red(centerPixel(reinterpret_cast<HWND>(primary.winId()))); }),
      "both actual windows follow a changing composition");
  compositor.setFtbCoverage(1.0f);
  compositor.setBackgroundColor(0x00ff00);
  CHECK(waitFor([&] { return green(centerPixel(window)) &&
      green(centerPixel(reinterpret_cast<HWND>(primary.winId()))); }),
      "both outputs follow background color changes while fully faded out");
  compositor.setBackgroundColor(0xff0000);
  CHECK(waitFor([&] { return red(centerPixel(window)) &&
      red(centerPixel(reinterpret_cast<HWND>(primary.winId()))); }),
      "receiver includes final FTB composition in the selected color");
  compositor.setFtbCoverage(0.0f);
  CHECK(continuous("receiver enabled"), "Program continues with receiver enabled");
  CHECK(freeze(DWORD(receiver->processId())), "deliberately freeze receiver threads");
  CHECK(continuous("receiver frozen"), "frozen receiver cannot stop Program");
  receiver->kill();
  CHECK(waitFor([&] { return !action->isChecked() && action->isEnabled(); }),
        "forced receiver termination resets menu to OFF");
  CHECK(continuous("receiver killed"), "Program continues after receiver process termination");
  window = startReceiver();
  CHECK(window && waitFor([&] { return red(centerPixel(window)); }), "menu can restart receiver after forced termination");
  QProcess holder;
  holder.start(app.applicationFilePath(), {QStringLiteral("--hold-mailbox"), vtrpon::operatorMonitorChannelName()});
  QByteArray output;
  CHECK(waitFor([&] { output += holder.readAllStandardOutput(); return output.contains("LOCKED"); }),
        "fault fixture holds receiver mailbox mutex indefinitely");
  compositor.setBackgroundColor(0x00ff00);
  CHECK(continuous("mailbox locked"), "Program continues even with mailbox locked");
  CHECK(green(centerPixel(reinterpret_cast<HWND>(primary.winId()))), "Program pixels update while receiver mailbox is locked");
  holder.kill();
  CHECK(waitFor([&] { return holder.state() == QProcess::NotRunning; }), "terminate lock holder");
  CHECK(waitFor([&] { return green(centerPixel(window)); }), "receiver recovers after abandoned mutex");
  PostMessageW(window, WM_CLOSE, 0, 0);
  CHECK(waitFor([&] { return !action->isChecked(); }), "window close resets menu to OFF");
  CHECK(continuous("window closed"), "closing receiver does not affect Program");
  for (int i = 0; i < 3; ++i) {
    CHECK(startReceiver(), "repeated menu ON");
    action->setChecked(false);
    CHECK(waitFor([&] { return receiver->state() == QProcess::NotRunning && action->isEnabled(); }),
          "repeated menu OFF stops only receiver");
  }
  CHECK(continuous("after toggles"), "Program remains healthy after repeated toggles");
  CHECK(startReceiver(), "start receiver for display selection test");
  {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("operatorMonitor/screen", app.primaryScreen()->name());
    settings.sync();
  }
  menus.reloadOperatorMonitorSettings();
  // GetWindowRect includes Windows' invisible resize borders; Qt's frame
  // placement can differ by those margins. Still require a move to the center.
  const int centerTolerance = 2 * (GetSystemMetrics(SM_CYSIZEFRAME) +
                                    GetSystemMetrics(SM_CXPADDEDBORDER) + 1);
  const bool selectedDisplayPlaced = waitFor([&] {
    window = receiverWindow(DWORD(receiver->processId()));
    if (!window || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) || IsZoomed(window)) return false;
    MONITORINFO info{sizeof(MONITORINFO)};
    RECT rect{};
    return GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info) &&
        GetWindowRect(window, &rect) && rect.left >= info.rcWork.left &&
        rect.top >= info.rcWork.top && rect.right <= info.rcWork.right &&
        rect.bottom <= info.rcWork.bottom &&
        std::abs((rect.left + rect.right) - (info.rcWork.left + info.rcWork.right)) <= centerTolerance &&
        std::abs((rect.top + rect.bottom) - (info.rcWork.top + info.rcWork.bottom)) <= centerTolerance &&
        rect.right - rect.left < info.rcWork.right - info.rcWork.left &&
        rect.bottom - rect.top < info.rcWork.bottom - info.rcWork.top;
  });
  if (!selectedDisplayPlaced && window) {
    MONITORINFO info{sizeof(MONITORINFO)};
    RECT rect{};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    GetWindowRect(window, &rect);
    std::cerr << "receiver rect=" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom
              << " work=" << info.rcWork.left << ',' << info.rcWork.top << ',' << info.rcWork.right << ',' << info.rcWork.bottom
              << " Qt work=" << screen.x() << ',' << screen.y() << ',' << screen.width() << ',' << screen.height()
              << " caption=" << bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION)
              << " maximized=" << IsZoomed(window) << std::endl;
  }
  CHECK(selectedDisplayPlaced, "selected display receives a small normal window with a title bar");
  {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("operatorMonitor/screen", "missing-display-fixture");
    settings.sync();
  }
  menus.reloadOperatorMonitorSettings();
  CHECK(waitFor([&] {
    window = receiverWindow(DWORD(receiver->processId()));
    return window && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION);
  }), "missing display falls back to a reachable window");
  action->setChecked(false);
  CHECK(waitFor([&] { return receiver->state() == QProcess::NotRunning; }), "display test receiver exits");
  CHECK(waitFor([&] { return compositor.prepareShutdown(); }), "Program callbacks finish before output stop");
  CHECK(compositor.stop(), "Program shuts down normally");
  return 0;
}
