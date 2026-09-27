#include "language_text.h"
#include "media_duration_probe.h"
#include "media_thumbnailer.h"
#include "powerpoint_add_method_dialog.h"
#include "powerpoint_import_service.h"
#include "powerpoint_source_session_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/pbutils/gstdiscoverer.h>
#include <gst/video/video.h>

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

namespace {

bool check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

std::set<DWORD> powerPointProcessIds() {
  std::set<DWORD> processIds;
  const HANDLE snapshot =
      CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return processIds;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"POWERPNT.EXE") == 0) {
        processIds.insert(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return processIds;
}

bool verifyNativeWindowLayout() {
  struct LayoutCheck {
    std::set<DWORD> processIds = powerPointProcessIds();
    int checked = 0;
    bool succeeded = true;
  } result;
  EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
    auto &result = *reinterpret_cast<LayoutCheck *>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[128]{};
    GetClassNameW(window, className, 128);
    if (!result.processIds.contains(processId) || !IsWindowVisible(window) ||
        wcscmp(className, L"PPTFrameClass") != 0 ||
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != 0) {
      return TRUE;
    }
    const HWND client = FindWindowExW(window, nullptr, L"MDIClient", nullptr);
    RECT outer{}, inner{};
    if (!client || !GetClientRect(window, &outer) || !GetClientRect(client, &inner)) {
      result.succeeded = false;
      return TRUE;
    }
    MapWindowPoints(client, window, reinterpret_cast<POINT *>(&inner), 2);
    ++result.checked;
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    result.succeeded = check((extendedStyle & WS_EX_TOOLWINDOW) != 0 &&
                                 (extendedStyle & WS_EX_NOACTIVATE) != 0 &&
                                 (extendedStyle & WS_EX_APPWINDOW) == 0,
        "PowerPoint capture window can appear in the taskbar or activate") && result.succeeded;
    result.succeeded = check(EqualRect(&outer, &inner),
        "PowerPoint slide viewport does not fill the capture area after removing the frame") && result.succeeded;
    std::cout << "native viewport=" << inner.right - inner.left << 'x'
              << inner.bottom - inner.top << " capture=" << outer.right << 'x' << outer.bottom << '\n';
    return TRUE;
  }, reinterpret_cast<LPARAM>(&result));
  return check(result.checked > 0 && result.succeeded,
               "Native PowerPoint window layout verification failed");
}

QImage sampleImage(GstSample *sample) {
  if (!sample) {
    return {};
  }
  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  gst_video_info_init(&info);
  if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA) {
    return {};
  }
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    return {};
  }
  const QImage wrapped(
      static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
      static_cast<int>(GST_VIDEO_FRAME_WIDTH(&frame)),
      static_cast<int>(GST_VIDEO_FRAME_HEIGHT(&frame)),
      GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), QImage::Format_RGBA8888);
  const QImage copied = wrapped.copy();
  gst_video_frame_unmap(&frame);
  return copied;
}

QImage pullLatestSampleImage(GstElement *appSink,
                             GstClockTime timeout = 3 * GST_SECOND) {
  if (!appSink || !GST_IS_APP_SINK(appSink)) {
    return {};
  }
  GstSample *latest =
      gst_app_sink_try_pull_sample(GST_APP_SINK(appSink), timeout);
  if (!latest) {
    return {};
  }
  while (GstSample *newer =
             gst_app_sink_try_pull_sample(GST_APP_SINK(appSink), 0)) {
    gst_sample_unref(latest);
    latest = newer;
  }
  const QImage image = sampleImage(latest);
  gst_sample_unref(latest);
  return image;
}

double averageRgbDifference(const QImage &left, const QImage &right) {
  if (left.isNull() || right.isNull() || left.size() != right.size() ||
      left.format() != QImage::Format_RGBA8888 ||
      right.format() != QImage::Format_RGBA8888) {
    return 255.0;
  }
  long double difference = 0.0;
  quint64 componentCount = 0;
  constexpr int sampleStep = 8;
  for (int y = 0; y < left.height(); y += sampleStep) {
    const uchar *leftLine = left.constScanLine(y);
    const uchar *rightLine = right.constScanLine(y);
    for (int x = 0; x < left.width(); x += sampleStep) {
      const int offset = x * 4;
      for (int channel = 0; channel < 3; ++channel) {
        difference += std::abs(static_cast<int>(leftLine[offset + channel]) -
                               static_cast<int>(rightLine[offset + channel]));
        ++componentCount;
      }
    }
  }
  return componentCount > 0
             ? static_cast<double>(difference / componentCount)
             : 255.0;
}

bool verifyFramePlans() {
  PowerPointAddSettings settings;
  settings.method = PowerPointAddMethod::VideoConversion;
  settings.slideDurationSeconds = 5.0;
  settings.crossDissolveEnabled = true;
  settings.crossDissolveDurationSeconds = 1.0;
  QString errorDetail;
  const auto plan = makePowerPointVideoFramePlan(3, settings, &errorDetail);
  bool succeeded = check(plan.has_value(), "Expected a valid frame plan");
  if (plan) {
    succeeded = check(plan->framesPerSecond == 30,
                      "Video frame rate must be 30 fps") &&
                succeeded;
    succeeded = check(plan->framesPerSlide == 150,
                      "Five seconds must produce 150 frames") &&
                succeeded;
    succeeded = check(plan->transitionFrames == 30,
                      "One-second dissolve must produce 30 frames") &&
                succeeded;
    succeeded = check(plan->totalFrames == 450,
                      "Transitions must remain inside each slide interval") &&
                succeeded;
    succeeded = check(plan->durationMs == 15000,
                      "Three five-second slides must total 15 seconds") &&
                succeeded;
  }

  settings.crossDissolveEnabled = false;
  const auto cutPlan = makePowerPointVideoFramePlan(2, settings, &errorDetail);
  succeeded = check(cutPlan && cutPlan->transitionFrames == 0,
                    "Disabled Cross Dissolve must produce no transition frames") &&
              succeeded;

  settings.crossDissolveEnabled = true;
  settings.slideDurationSeconds = 0.5;
  settings.crossDissolveDurationSeconds = 0.6;
  succeeded = check(!makePowerPointVideoFramePlan(1, settings, &errorDetail),
                    "Transition longer than a slide must be rejected") &&
              succeeded;
  succeeded = check(errorDetail.contains(QStringLiteral("must not exceed")),
                    "Invalid transition must explain the duration boundary") &&
              succeeded;
  return succeeded;
}

bool verifyDialogContract() {
  PowerPointAddMethodDialog dialog;
  auto *native = dialog.findChild<QRadioButton *>(
      QStringLiteral("powerPointNativeMethod"));
  auto *video = dialog.findChild<QRadioButton *>(
      QStringLiteral("powerPointVideoMethod"));
  auto *png = dialog.findChild<QRadioButton *>(
      QStringLiteral("powerPointPngMethod"));
  auto *slideDuration = dialog.findChild<QDoubleSpinBox *>(
      QStringLiteral("powerPointSlideDuration"));
  auto *crossDissolve = dialog.findChild<QCheckBox *>(
      QStringLiteral("powerPointCrossDissolve"));
  auto *transitionDuration = dialog.findChild<QDoubleSpinBox *>(
      QStringLiteral("powerPointCrossDissolveDuration"));
  auto *remember = dialog.findChild<QCheckBox *>(
      QStringLiteral("powerPointRememberForSession"));
  bool succeeded =
      check(native && video && png && slideDuration && crossDissolve &&
                transitionDuration && remember,
            "PowerPoint method dialog controls are missing");
  if (!succeeded) {
    return false;
  }
  succeeded = check(native->isChecked(),
                    "Native slideshow must be the explicit default") &&
              succeeded;
  succeeded = check(!slideDuration->isEnabled(),
                    "Video settings must be disabled for native mode") &&
              succeeded;
  succeeded = check(std::abs(slideDuration->value() - 5.0) < 0.001 &&
                        crossDissolve->isChecked() &&
                        std::abs(transitionDuration->value() - 1.0) < 0.001 &&
                        !remember->isChecked(),
                    "PowerPoint conversion defaults are invalid") &&
              succeeded;

  video->setChecked(true);
  QApplication::processEvents();
  succeeded = check(slideDuration->isEnabled(),
                    "Video settings must enable for video conversion") &&
              succeeded;
  crossDissolve->setChecked(false);
  succeeded = check(!transitionDuration->isEnabled(),
                    "Transition duration must disable when dissolve is off") &&
              succeeded;
  crossDissolve->setChecked(true);
  slideDuration->setValue(0.5);
  succeeded = check(transitionDuration->maximum() <= 0.5,
                    "Transition maximum must follow slide duration") &&
              succeeded;
  png->setChecked(true);
  QApplication::processEvents();
  succeeded = check(slideDuration->isEnabled(),
                    "Slide duration must enable for PNG conversion") &&
              succeeded;
  succeeded = check(!crossDissolve->isEnabled() &&
                        !transitionDuration->isEnabled(),
                    "Cross Dissolve must disable for PNG conversion") &&
              succeeded;
  slideDuration->setValue(3.5);
  remember->setChecked(true);
  const PowerPointAddSettings selected = dialog.settings();
  succeeded = check(selected.method ==
                        PowerPointAddMethod::PngSequence,
                    "Dialog must return the selected method") &&
              succeeded;
  succeeded = check(std::abs(selected.slideDurationSeconds - 3.5) < 0.001,
                    "Dialog must return the PNG duration") &&
              succeeded;
  succeeded = check(selected.rememberForSession,
                    "Dialog must return session-only memory choice") &&
              succeeded;
  return succeeded;
}

bool verifyPngSequenceItemSettings() {
  PowerPointAddSettings settings;
  settings.method = PowerPointAddMethod::PngSequence;
  settings.slideDurationSeconds = 3.5;
  QString errorDetail;
  const auto itemSettings =
      makePowerPointPngSequenceItemSettings(settings, &errorDetail);
  bool succeeded = check(itemSettings.has_value() && errorDetail.isEmpty(),
                         "Valid PNG sequence settings were rejected");
  if (itemSettings) {
    succeeded = check(itemSettings->outPointMs == 3500 &&
                          itemSettings->startMode == PlaylistStartMode::Pause &&
                          itemSettings->endMode == PlaylistEndMode::Pause,
                      "PNG sequence duration or default modes did not propagate") &&
                succeeded;
  }

  const double invalidDurations[] = {
      0.09, 600.01, std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (const double invalidDuration : invalidDurations) {
    settings.slideDurationSeconds = invalidDuration;
    errorDetail.clear();
    succeeded = check(
                    !makePowerPointPngSequenceItemSettings(settings,
                                                           &errorDetail) &&
                        !errorDetail.isEmpty(),
                    "Invalid PNG sequence duration was accepted") &&
                succeeded;
  }
  return succeeded;
}

bool verifyCancellationBeforePowerPointOpen() {
  std::atomic_bool cancelRequested{true};
  const PowerPointSlideExportResult result = exportPowerPointSlides(
      QStringLiteral("unused.pptx"), QString(), &cancelRequested);
  return check(result.canceled && !result.succeeded &&
                   result.slidePaths.isEmpty() && result.errorDetail.isEmpty(),
               "Preflight cancellation must not open PowerPoint");
}

bool videoCodecIsH264(const QString &filePath) {
  GError *error = nullptr;
  GstDiscoverer *discoverer = gst_discoverer_new(5 * GST_SECOND, &error);
  if (!discoverer) {
    g_clear_error(&error);
    return false;
  }
  const QByteArray uri = QUrl::fromLocalFile(filePath).toEncoded();
  GstDiscovererInfo *info =
      gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
  g_clear_error(&error);
  bool h264 = false;
  if (info && gst_discoverer_info_get_result(info) == GST_DISCOVERER_OK) {
    GList *streams = gst_discoverer_info_get_video_streams(info);
    for (GList *node = streams; node && !h264; node = node->next) {
      GstCaps *caps = gst_discoverer_stream_info_get_caps(
          GST_DISCOVERER_STREAM_INFO(node->data));
      if (caps && !gst_caps_is_empty(caps)) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        h264 = structure &&
               gst_structure_has_name(structure, "video/x-h264");
      }
      if (caps) {
        gst_caps_unref(caps);
      }
    }
    gst_discoverer_stream_info_list_free(streams);
  }
  if (info) {
    gst_discoverer_info_unref(info);
  }
  g_object_unref(discoverer);
  return h264;
}

void removeMediaFoundationH264ForFallbackSmoke() {
  GstRegistry *registry = gst_registry_get();
  GstPluginFeature *feature =
      gst_registry_lookup_feature(registry, "mfh264enc");
  if (!feature) {
    return;
  }
  gst_registry_remove_feature(registry, feature);
  gst_object_unref(feature);
}

bool verifyConversionSmoke(const QString &filePath,
                           PowerPointAddMethod method) {
  PowerPointAddSettings settings;
  settings.method = method;
  settings.slideDurationSeconds = 0.5;
  settings.crossDissolveEnabled = true;
  settings.crossDissolveDurationSeconds = 0.1;
  if (method == PowerPointAddMethod::PngSequence) {
    settings.slideDurationSeconds = 3.5;
  }
  const PowerPointImportResult result =
      PowerPointImportService::importFile(filePath, settings, 1);
  if (!result.succeeded && !result.errorDetail.isEmpty()) {
    std::cerr << result.errorDetail.toStdString() << '\n';
  }
  bool succeeded = check(result.succeeded,
                         "PowerPoint conversion smoke test failed");
  if (result.succeeded && method == PowerPointAddMethod::PngSequence) {
    succeeded = check(!result.items.isEmpty(),
                      "PNG conversion produced no playlist items") &&
                succeeded;
    for (qsizetype index = 0; index < result.items.size(); ++index) {
      const PlaylistItemRegistration &item = result.items.at(index);
      const QImage image(item.filePath);
      const QString expectedSuffix = QStringLiteral("-slide-%1.png").arg(
          index + 1, 4, 10, QLatin1Char('0'));
      succeeded = check(item.sourceType == PlaylistSourceType::File &&
                            item.mediaType == PlaylistMediaType::StillImage &&
                            item.powerPointMode ==
                                PlaylistPowerPointMode::PngSequence &&
                            item.settings.outPointMs == 3500 &&
                            item.settings.startMode ==
                                PlaylistStartMode::Pause &&
                            item.settings.endMode == PlaylistEndMode::Pause &&
                            image.size() == QSize(1920, 1080) &&
                            QFileInfo(item.filePath).fileName().endsWith(
                                expectedSuffix),
                        "PNG conversion output contract is invalid") &&
                  succeeded;
    }
  } else if (result.succeeded) {
    const QString videoPath = result.items.constFirst().filePath;
    QFile videoFile(videoPath);
    const bool openedVideo = videoFile.open(QIODevice::ReadOnly);
    const QByteArray header = openedVideo ? videoFile.read(12) : QByteArray();
    const bool isMp4 = QFileInfo(videoPath).suffix().compare(
                           QStringLiteral("mp4"), Qt::CaseInsensitive) == 0 &&
                       header.size() >= 8 && header.mid(4, 4) == "ftyp";
    const bool isH264 = videoCodecIsH264(videoPath);
    const QStringList generatedFiles =
        QDir(result.generatedDirectory).entryList(QDir::Files);
    const MediaDurationProbeResult probe = probeMediaDuration(videoPath);
    if (!probe.succeeded || probe.mediaType != PlaylistMediaType::Video ||
        probe.durationMs != result.items.constFirst().settings.outPointMs) {
      std::cerr << "Probe succeeded: " << probe.succeeded
                << ", media type: " << static_cast<int>(probe.mediaType)
                << ", probe duration: " << probe.durationMs.value_or(-1)
                << ", planned duration: "
                << result.items.constFirst()
                       .settings.outPointMs.value_or(-1)
                << ", detail: " << probe.errorDetail.toStdString() << '\n';
    }
    succeeded = check(
                    result.items.size() == 1 &&
                        result.items.constFirst().sourceType ==
                            PlaylistSourceType::File &&
                        result.items.constFirst().mediaType ==
                            PlaylistMediaType::Video &&
                        result.items.constFirst().powerPointMode ==
                            PlaylistPowerPointMode::VideoConversion &&
                        result.items.constFirst()
                                .settings.outPointMs.value_or(0) > 0 &&
                        QFileInfo(videoPath).size() > 0 && isMp4 && isH264 &&
                        generatedFiles.size() == 1 &&
                        probe.succeeded &&
                        probe.mediaType == PlaylistMediaType::Video &&
                        probe.durationMs ==
                            result.items.constFirst().settings.outPointMs,
                    "Video conversion output contract is invalid") &&
                succeeded;
  }
  if (!result.generatedDirectory.isEmpty()) {
    const bool removed = PowerPointImportService::discardGeneratedArtifacts(
        result.generatedDirectory);
    if (!removed) {
      const QString generatedRoot = QDir(
          QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                        .filePath(QStringLiteral(
                                            "VTR-PON3-Dev/Generated/PowerPoint"));
      std::cerr << "Generated directory: "
                << result.generatedDirectory.toStdString() << '\n'
                << "Generated root: " << generatedRoot.toStdString() << '\n'
                << "Directory canonical: "
                << QFileInfo(result.generatedDirectory)
                       .canonicalFilePath().toStdString() << '\n'
                << "Root canonical: "
                << QFileInfo(generatedRoot).canonicalFilePath().toStdString()
                << '\n';
    }
    succeeded = check(removed,
                      "Could not remove smoke-test generated artifacts") &&
                succeeded;
  }
  return succeeded;
}

bool verifyThumbnailSmoke(const QString &filePath) {
  const std::set<DWORD> processIdsBefore = powerPointProcessIds();
  PowerPointAddSettings settings;
  settings.method = PowerPointAddMethod::NativeSlideShow;
  const PowerPointImportResult importResult =
      PowerPointImportService::importFile(filePath, settings, 1);
  bool succeeded = check(
      importResult.succeeded && importResult.items.size() == 1 &&
          importResult.items.constFirst().powerPointMode ==
              PlaylistPowerPointMode::NativeSlideShow,
      "Native PowerPoint registration mode is invalid");

  const MediaThumbnailResult thumbnail = generateMediaThumbnail(
      filePath, PlaylistMediaType::Presentation, std::nullopt,
      QSize(120, 68));
  if (!thumbnail.succeeded && !thumbnail.errorDetail.isEmpty()) {
    std::cerr << thumbnail.errorDetail.toStdString() << '\n';
  }
  succeeded = check(thumbnail.succeeded &&
                        thumbnail.mediaType ==
                            PlaylistMediaType::Presentation &&
                        thumbnail.image.size() == QSize(120, 68),
                    "Native PowerPoint first-slide thumbnail failed") &&
              succeeded;
  succeeded = check(powerPointProcessIds() == processIdsBefore,
                    "Thumbnail generation changed pre-existing PowerPoint processes") &&
              succeeded;
  return succeeded;
}

bool waitForNativeReady(PowerPointSourceSessionManager &manager) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 15000) {
    QApplication::processEvents();
    if (manager.sourceIsReady(0)) {
      return true;
    }
    quint64 generation = 0;
    QString detail;
    if (manager.sourceErrorAfter(0, 0, generation, detail)) {
      std::cerr << detail.toStdString() << '\n';
      return false;
    }
    QThread::msleep(25);
  }
  return check(false, "PowerPoint did not become ready within 15 seconds");
}

bool verifyNativeReconnectSmoke(const QString &filePath) {
  // Fault injection is limited to the Office process started by this test,
  // before any presentation is opened. Never terminate an existing Office.
  if (!check(powerPointProcessIds().empty(),
             "Reconnect smoke requires PowerPoint to be closed")) return false;
  PowerPointSourceSessionManager manager(1920, 1080);
  manager.requestApplicationWarmup();
  QElapsedTimer timer;
  timer.start();
  while (!manager.applicationIsWarm() && timer.elapsed() < 15000) {
    QApplication::processEvents();
    QThread::msleep(25);
  }
  const auto startedIds = powerPointProcessIds();
  if (!check(manager.applicationIsWarm() && startedIds.size() == 1 &&
                 !isPowerPointNativeSourceActive(),
             "Test owns one idle prestarted PowerPoint process")) return false;
  const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                     *startedIds.begin());
  if (!check(process != nullptr, "Test Office process handle is available")) return false;
  const bool terminated = TerminateProcess(process, 1) &&
                         WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
  CloseHandle(process);
  if (!check(terminated, "Test prestarted Office exits before CUE")) return false;
  PowerPointSourceSessionManager::ProgramSource source;
  QString detail;
  bool succeeded = check(manager.bindProgramDeck(0, filePath, source, &detail),
                          "CUE is accepted after Office disconnects") &&
                   waitForNativeReady(manager);
  source = manager.programSource(0);
  if (succeeded && source.isValid()) {
    GstSample *frame = gst_app_sink_try_pull_sample(
        GST_APP_SINK(source.appSink), 5 * GST_SECOND);
    succeeded = check(frame != nullptr, "Reconnected Office produces a native frame");
    if (frame) gst_sample_unref(frame);
  } else {
    succeeded = false;
  }
  succeeded = check(manager.shutdown(), "Reconnected Office shuts down") && succeeded;
  succeeded = check(powerPointProcessIds().empty(),
                    "Reconnection retains correct ownership of the new Office process") && succeeded;
  std::cout << "Native PowerPoint reconnect: " << (succeeded ? "PASS" : "FAIL") << '\n';
  return succeeded;
}

bool waitForNativeSlide(PowerPointSourceSessionManager &manager, int slide,
                        PowerPointNavigationState &state) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 10000) {
    QApplication::processEvents();
    state = manager.navigationState(0);
    if (state.active && !state.busy && state.errorDetail.isEmpty() &&
        state.currentSlide == slide) {
      return true;
    }
    QThread::msleep(25);
  }
  std::cerr << "Expected slide " << slide << ", actual " << state.currentSlide
            << ", busy=" << state.busy << ", error="
            << state.errorDetail.toStdString() << '\n';
  return false;
}

PowerPointCommandResponse navigateNative(PowerPointSourceSessionManager &manager,
                                          bool forward) {
  QElapsedTimer timer;
  timer.start();
  PowerPointCommandResponse result;
  do {
    result = forward ? manager.next(0) : manager.previous(0);
    if (result.result != PowerPointCommandResult::Busy) return result;
    QApplication::processEvents();
    QThread::msleep(25);
  } while (timer.elapsed() < 10000);
  return result;
}

bool verifyNativeSmoke(const QString &filePath, int expectedSlides = 2,
                       const QString &captureDirectory = {}) {
  bool succeeded = verifyThumbnailSmoke(filePath);
  const std::set<DWORD> processIdsBefore = powerPointProcessIds();
  PowerPointSourceSessionManager manager(1920, 1080);
  // Registration starts Office before a deck exists. This must remain a
  // nonblocking request and must not create a slide show or reserve Program.
  QElapsedTimer warmupTimer;
  warmupTimer.start();
  manager.requestApplicationWarmup();
  succeeded = check(warmupTimer.elapsed() < 100,
                    "PowerPoint warmup blocked its caller") && succeeded;
  while (!manager.applicationIsWarm() && warmupTimer.elapsed() < 15000) {
    QApplication::processEvents();
    QThread::msleep(25);
  }
  if (!check(manager.applicationIsWarm(), "PowerPoint did not start before CUE")) return false;
  const auto warmProcessIds = powerPointProcessIds();
  succeeded = check(!warmProcessIds.empty() && !isPowerPointNativeSourceActive() &&
                        !manager.navigationState(0).active &&
                        !manager.programSource(0).isValid(),
                    "Warmup started a native deck or failed to keep Office running") && succeeded;
  manager.requestApplicationWarmup();
  std::cout << "Office ready before CUE in " << warmupTimer.elapsed() << " ms\n";
  PowerPointSourceSessionManager::ProgramSource source;
  QString errorDetail;
  if (!manager.bindProgramDeck(0, filePath, source, &errorDetail)) {
    std::cerr << errorDetail.toStdString() << '\n';
    return false;
  }

  succeeded = check(!source.isValid(), "Queued PowerPoint bind returned a synchronous input") && succeeded;
  if (!waitForNativeReady(manager)) {
    return false;
  }
  source = manager.programSource(0);
  if (!check(source.isValid(), "Prepared PowerPoint input is missing")) return false;

  succeeded = check(powerPointProcessIds() == warmProcessIds,
                    "CUE did not reuse the prestarted PowerPoint process") && succeeded;

  succeeded = verifyNativeWindowLayout() && succeeded;
  GstSample *sample =
      gst_app_sink_try_pull_sample(GST_APP_SINK(source.appSink),
                                   15 * GST_SECOND);
  succeeded = check(sample != nullptr,
                    "Native PowerPoint capture produced no frame") &&
              succeeded;
  if (sample) {
    int width = 0;
    int height = 0;
    const GstCaps *caps = gst_sample_get_caps(sample);
    const GstStructure *structure =
        caps ? gst_caps_get_structure(caps, 0) : nullptr;
    succeeded = check(structure &&
                          gst_structure_get_int(structure, "width", &width) &&
                          gst_structure_get_int(structure, "height", &height) &&
                          width == 1920 && height == 1080,
                      "Native PowerPoint capture size is invalid") &&
                succeeded;
    gst_sample_unref(sample);
  }

  PowerPointNavigationState state = manager.navigationState(0);
  succeeded = check(state.active && !state.busy && state.slideCount == expectedSlides &&
                        state.currentSlide == 1 && state.canGoNext,
                    "Native PowerPoint initial navigation state is invalid") &&
              succeeded;
  QImage previousImage;
  const auto captureSlide = [&](int slide) {
    // Drain frames from before the navigation settled, then allow a repaint.
    (void)pullLatestSampleImage(source.appSink);
    QElapsedTimer repaintTimer;
    repaintTimer.start();
    while (repaintTimer.elapsed() < 300) {
      QApplication::processEvents();
      QThread::msleep(10);
    }
    const QImage frame = pullLatestSampleImage(source.appSink);
    succeeded = check(!frame.isNull() && frame.size() == QSize(1920, 1080),
                      "Native slide frame is missing or incorrectly sized") && succeeded;
    if (!captureDirectory.isEmpty()) {
      QDir().mkpath(captureDirectory);
      succeeded = check(frame.save(QDir(captureDirectory).filePath(
                            QStringLiteral("slide-%1.png").arg(slide))),
                        "Could not save native verification frame") && succeeded;
    }
    if (!previousImage.isNull()) {
      const double difference = averageRgbDifference(previousImage, frame);
      std::cout << "slide=" << slide << " frame difference=" << difference << '\n';
      succeeded = check(difference > 0.25, "Native navigation kept the previous frame") && succeeded;
    }
    previousImage = frame;
  };
  captureSlide(1);
  const int lastSlide = (std::min)(expectedSlides, 3);
  for (int slide = 2; slide <= lastSlide; ++slide) {
    succeeded = check(navigateNative(manager, true).result == PowerPointCommandResult::Completed,
                      "Native PowerPoint NEXT failed") && succeeded;
    if (!waitForNativeSlide(manager, slide, state)) {
      succeeded = false;
      break;
    }
    succeeded = check(state.canGoPrevious, "PREV should be available after NEXT") && succeeded;
    captureSlide(slide);
  }
  for (int slide = state.currentSlide - 1; slide >= 1; --slide) {
    succeeded = check(navigateNative(manager, false).result == PowerPointCommandResult::Completed,
                      "Native PowerPoint PREV failed") && succeeded;
    if (!waitForNativeSlide(manager, slide, state)) {
      succeeded = false;
      break;
    }
    captureSlide(slide);
  }
  succeeded = check(manager.releaseProgramDeck(0),
                    "Native PowerPoint deck release failed") &&
              succeeded;
  succeeded = check(manager.shutdown(),
                    "Native PowerPoint manager shutdown failed") &&
              succeeded;
  succeeded = check(powerPointProcessIds() == processIdsBefore,
                    "Native capture changed pre-existing PowerPoint processes") &&
              succeeded;
  return succeeded;
}

bool verifyNativeBlackSlideSmoke(const QString &filePath) {
  // Fixture: a static black first slide and a nonblack second slide, without
  // animations or automatic advance. Exercise actual capture past the former
  // five-second cutoff, both at startup and after navigating back to black.
  const std::set<DWORD> processIdsBefore = powerPointProcessIds();
  PowerPointSourceSessionManager manager(1920, 1080);
  PowerPointSourceSessionManager::ProgramSource source;
  QString detail;
  if (!manager.bindProgramDeck(0, filePath, source, &detail)) {
    std::cerr << detail.toStdString() << '\n';
    return false;
  }
  if (!waitForNativeReady(manager)) return false;
  source = manager.programSource(0);
  if (!check(source.isValid(), "Prepared PowerPoint input is missing")) return false;

  const auto isBlack = [](const QImage &frame) {
    if (frame.isNull() || frame.format() != QImage::Format_RGBA8888) return false;
    for (int y = 0; y < frame.height(); y += 8) {
      const uchar *line = frame.constScanLine(y);
      for (int x = 0; x < frame.width(); x += 8) {
        if (line[x * 4] > 8 || line[x * 4 + 1] > 8 || line[x * 4 + 2] > 8) {
          return false;
        }
      }
    }
    return true;
  };
  const auto noSourceError = [&] {
    quint64 generation = 0;
    QString error;
    if (!manager.sourceErrorAfter(0, 0, generation, error)) return true;
    std::cerr << error.toStdString() << '\n';
    return false;
  };
  const auto holdBlackSlide = [&] {
    QElapsedTimer deadline;
    QElapsedTimer blackDuration;
    deadline.start();
    while (deadline.elapsed() < 10000) {
      QApplication::processEvents();
      // Each pull consumes fresh samples; a cached final black frame cannot
      // satisfy this check after the capture worker stops.
      const QImage frame = pullLatestSampleImage(source.appSink, GST_SECOND);
      if (!check(!frame.isNull(), "Black slide stopped delivering frames") ||
          !noSourceError()) return false;
      if (isBlack(frame)) {
        if (!blackDuration.isValid()) blackDuration.start();
        if (blackDuration.elapsed() >= 6000) return true;
      } else if (blackDuration.isValid() || deadline.elapsed() >= 3000) {
        return check(false, "Expected a continuous black slide");
      }
      QThread::msleep(100);
    }
    return check(false, "Black slide did not remain healthy for six seconds");
  };

  PowerPointNavigationState state;
  bool succeeded = waitForNativeSlide(manager, 1, state) && holdBlackSlide();
  if (succeeded) {
    succeeded = check(navigateNative(manager, true).result ==
                          PowerPointCommandResult::Completed,
                      "NEXT failed after holding a black slide") &&
                waitForNativeSlide(manager, 2, state);
  }
  if (succeeded) {
    QElapsedTimer repaint;
    repaint.start();
    bool nonblackFrame = false;
    while (!nonblackFrame && repaint.elapsed() < 3000) {
      QApplication::processEvents();
      const QImage frame = pullLatestSampleImage(source.appSink, GST_SECOND);
      nonblackFrame = !frame.isNull() && !isBlack(frame);
      if (!nonblackFrame) QThread::msleep(50);
    }
    succeeded = check(nonblackFrame, "NEXT did not produce the nonblack slide") &&
                noSourceError();
  }
  if (succeeded) {
    succeeded = check(navigateNative(manager, false).result ==
                          PowerPointCommandResult::Completed,
                      "PREV failed after holding a black slide") &&
                waitForNativeSlide(manager, 1, state) && holdBlackSlide();
  }
  succeeded = check(manager.releaseProgramDeck(0),
                    "Black slide deck release failed") && succeeded;
  succeeded = check(manager.shutdown(),
                    "Black slide manager shutdown failed") && succeeded;
  succeeded = check(powerPointProcessIds() == processIdsBefore,
                    "Black slide smoke changed pre-existing PowerPoint processes") && succeeded;
  std::cout << "Native PowerPoint black slide: "
            << (succeeded ? "PASS" : "FAIL") << '\n';
  return succeeded;
}

bool verifyNativeMovingMediaSmoke(const QString &filePath) {
  const std::set<DWORD> processIdsBefore = powerPointProcessIds();
  PowerPointSourceSessionManager manager(1920, 1080);
  PowerPointSourceSessionManager::ProgramSource source;
  QString errorDetail;
  if (!manager.bindProgramDeck(0, filePath, source, &errorDetail)) {
    std::cerr << errorDetail.toStdString() << '\n';
    return false;
  }

  if (!waitForNativeReady(manager)) {
    return false;
  }
  source = manager.programSource(0);
  if (!check(source.isValid(), "Prepared PowerPoint input is missing")) return false;
  bool succeeded = true;
  PowerPointNavigationState state = manager.navigationState(0);
  // This fixture may have click animations on slide 1 and a click-to-play
  // video on slide 2. NEXT must consume those clicks, not skip their slide.
  const int maximumNextCommands = (std::max)(0, state.clickCount) + 2;
  for (int command = 0; command < maximumNextCommands && state.currentSlide == 1;
       ++command) {
    const int previousClick = state.clickIndex;
    const bool clickRemains = state.clickIndex >= 0 &&
                              state.clickIndex < state.clickCount;
    succeeded = check(navigateNative(manager, true).result == PowerPointCommandResult::Completed,
                      "Native moving-media NEXT failed") && succeeded;
    QElapsedTimer settleTimer;
    settleTimer.start();
    do {
      QApplication::processEvents();
      QThread::msleep(25);
      state = manager.navigationState(0);
    } while (settleTimer.elapsed() < 10000 &&
             (state.busy || (state.currentSlide == 1 && state.clickIndex == previousClick)));
    std::cout << "NEXT slide=" << state.currentSlide << " click=" << state.clickIndex
              << "/" << state.clickCount << '\n';
    if (clickRemains) {
      succeeded = check(state.currentSlide == 1 && state.clickIndex != previousClick,
                        "NEXT skipped a click animation on the first slide") && succeeded;
    }
  }
  succeeded = check(waitForNativeSlide(manager, 2, state),
                    "Native moving-media smoke did not reach slide 2") &&
              succeeded;
  if (state.currentSlide == 2 && state.clickIndex >= 0 &&
      state.clickIndex < state.clickCount) {
    succeeded = check(navigateNative(manager, true).result == PowerPointCommandResult::Completed,
                      "Native video play click failed") && succeeded;
    succeeded = check(waitForNativeSlide(manager, 2, state),
                      "The video play click skipped its slide") && succeeded;
  }

  const auto waitWithEvents = [](int durationMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
      QApplication::processEvents();
      QThread::msleep(25);
    }
  };
  waitWithEvents(1500);
  const QImage first = pullLatestSampleImage(source.appSink);
  waitWithEvents(2500);
  const QImage second = pullLatestSampleImage(source.appSink);
  const double frameDifference = averageRgbDifference(first, second);
  std::cout << "Native moving-media frame difference=" << frameDifference
            << '\n';
  succeeded = check(!first.isNull() && !second.isNull() &&
                        first.size() == QSize(1920, 1080) &&
                        second.size() == QSize(1920, 1080) &&
                        frameDifference > 0.25,
                    "Native embedded video did not keep updating") &&
              succeeded;

  succeeded = check(manager.releaseProgramDeck(0),
                    "Native moving-media deck release failed") &&
              succeeded;
  succeeded = check(manager.shutdown(),
                    "Native moving-media manager shutdown failed") &&
              succeeded;
  succeeded = check(powerPointProcessIds() == processIdsBefore,
                    "Moving-media smoke changed pre-existing PowerPoint processes") &&
              succeeded;
  return succeeded;
}

bool verifyNativeExportIsolationSmoke(const QString &filePath) {
  const std::set<DWORD> processIdsBefore = powerPointProcessIds();
  PowerPointSourceSessionManager manager(1920, 1080);
  PowerPointSourceSessionManager::ProgramSource source;
  QString errorDetail;
  if (!manager.bindProgramDeck(0, filePath, source, &errorDetail)) {
    std::cerr << errorDetail.toStdString() << '\n';
    return false;
  }

  if (!waitForNativeReady(manager)) {
    return false;
  }
  source = manager.programSource(0);
  if (!check(source.isValid(), "Prepared PowerPoint input is missing")) return false;
  QThread::msleep(400);
  const QImage before = pullLatestSampleImage(source.appSink);
  bool succeeded = check(!before.isNull(),
                         "Could not read the native frame before export");

  QTemporaryDir outputDirectory;
  succeeded = check(outputDirectory.isValid(),
                    "Could not create the blocked export directory") &&
              succeeded;
  succeeded = check(isPowerPointNativeSourceActive(),
                    "Native PowerPoint source was not registered active") &&
              succeeded;
  const PowerPointSlideExportResult exportResult = exportPowerPointSlides(
      filePath, outputDirectory.path());
  succeeded = check(!exportResult.succeeded &&
                        exportResult.slidePaths.isEmpty() &&
                        exportResult.errorDetail.contains(
                            QStringLiteral("native PowerPoint source"),
                            Qt::CaseInsensitive),
                    "PowerPoint export was not blocked by the native source") &&
              succeeded;
  succeeded = check(
                  QDir(outputDirectory.path())
                      .entryList(QDir::Files | QDir::NoDotAndDotDot)
                      .isEmpty(),
                  "Blocked PowerPoint export wrote slide artifacts") &&
              succeeded;

  QString thumbnailError;
  const QImage thumbnail = generatePowerPointThumbnail(
      filePath, QSize(120, 68), &thumbnailError);
  succeeded = check(thumbnail.isNull() &&
                        isPowerPointNativeSourceActiveError(thumbnailError),
                    "PowerPoint thumbnail export was not blocked") &&
              succeeded;
  const MediaThumbnailResult playlistThumbnail = generateMediaThumbnail(
      filePath, PlaylistMediaType::Presentation, std::nullopt,
      QSize(120, 68));
  succeeded = check(playlistThumbnail.succeeded &&
                        !playlistThumbnail.image.isNull() &&
                        playlistThumbnail.errorDetail.isEmpty(),
                    "Native-source contention became a terminal playlist thumbnail failure") &&
              succeeded;

  QThread::msleep(200);
  const QImage afterBlock = pullLatestSampleImage(source.appSink);
  const double blockedDifference = averageRgbDifference(before, afterBlock);
  succeeded = check(!afterBlock.isNull() && blockedDifference <= 4.0,
                    "Blocked export changed the native Program frame") &&
              succeeded;
  const PowerPointCommandResponse next = navigateNative(manager, true);
  succeeded = check(next.result == PowerPointCommandResult::Completed,
                    "Native NEXT stopped working after blocked export") &&
              succeeded;
  PowerPointNavigationState state;
  QElapsedTimer navigationTimer;
  navigationTimer.start();
  do {
    QApplication::processEvents();
    state = manager.navigationState(0);
    if (state.currentSlide == 2) {
      break;
    }
    QThread::msleep(50);
  } while (navigationTimer.elapsed() < 3000);
  succeeded = check(state.active && !state.busy && state.currentSlide == 2,
                    "Native slideshow did not continue after blocked export") &&
              succeeded;
  std::cout << "Native/export blocked=1, frame difference="
            << blockedDifference << ", current slide="
            << state.currentSlide << '\n';
  succeeded = check(manager.releaseProgramDeck(0),
                    "Blocked export deck release failed") &&
              succeeded;
  succeeded = check(manager.shutdown(),
                    "Blocked export manager shutdown failed") &&
              succeeded;
  succeeded = check(!isPowerPointNativeSourceActive(),
                    "Native PowerPoint source remained registered active") &&
              succeeded;
  succeeded = check(powerPointProcessIds() == processIdsBefore,
                    "Blocked export changed pre-existing PowerPoint processes") &&
              succeeded;
  return succeeded;
}

bool verifyNativeVideoImportIsolationSmoke(const QString &filePath) {
  const std::set<DWORD> processIdsBefore = powerPointProcessIds();
  PowerPointSourceSessionManager manager(1920, 1080);
  PowerPointSourceSessionManager::ProgramSource source;
  QString errorDetail;
  if (!manager.bindProgramDeck(0, filePath, source, &errorDetail)) {
    std::cerr << errorDetail.toStdString() << '\n';
    return false;
  }
  if (!waitForNativeReady(manager)) {
    return false;
  }
  source = manager.programSource(0);
  if (!check(source.isValid(), "Prepared PowerPoint input is missing")) return false;
  QThread::msleep(400);
  const QImage before = pullLatestSampleImage(source.appSink);
  bool succeeded = check(!before.isNull(),
                         "Could not read the native frame before video import");

  PowerPointAddSettings settings;
  settings.method = PowerPointAddMethod::VideoConversion;
  settings.slideDurationSeconds = 0.5;
  settings.crossDissolveEnabled = true;
  settings.crossDissolveDurationSeconds = 0.1;
  const PowerPointImportResult importResult =
      PowerPointImportService::importFile(filePath, settings, 1);
  QThread::msleep(200);
  const QImage after = pullLatestSampleImage(source.appSink);
  const double afterDifference = averageRgbDifference(before, after);
  std::cout << "Native/video-import blocked=1, frame difference="
            << afterDifference << '\n';

  succeeded = check(!importResult.succeeded &&
                        !importResult.canceled &&
                        importResult.items.isEmpty() &&
                        importResult.generatedDirectory.isEmpty() &&
                        importResult.errorDetail == vtrpon::uiText(
                            "powerpoint.import.native_source_active"),
                    "Video conversion was not blocked by the native source") &&
              succeeded;
  succeeded = check(!after.isNull() && afterDifference <= 4.0,
                    "Blocked video conversion changed the native Program frame") &&
              succeeded;
  const PowerPointCommandResponse next = navigateNative(manager, true);
  succeeded = check(next.result == PowerPointCommandResult::Completed,
                    "Native NEXT stopped working after blocked video import") &&
              succeeded;
  PowerPointNavigationState state;
  QElapsedTimer navigationTimer;
  navigationTimer.start();
  do {
    QApplication::processEvents();
    state = manager.navigationState(0);
    if (state.currentSlide == 2) {
      break;
    }
    QThread::msleep(50);
  } while (navigationTimer.elapsed() < 3000);
  succeeded = check(state.active && !state.busy && state.currentSlide == 2,
                    "Native slideshow did not continue after blocked video import") &&
              succeeded;
  succeeded = check(manager.releaseProgramDeck(0),
                    "Blocked video-import deck release failed") &&
              succeeded;
  std::atomic_bool programPriorityCancel{true};
  const PowerPointImportResult canceledImportResult =
      PowerPointImportService::importFile(
          filePath, settings, 1, nullptr, &programPriorityCancel);
  succeeded = check(canceledImportResult.canceled &&
                        !canceledImportResult.succeeded &&
                        canceledImportResult.items.isEmpty() &&
                        canceledImportResult.generatedDirectory.isEmpty(),
                    "Program-priority cancellation did not stop conversion") &&
              succeeded;
  const PowerPointImportResult releasedImportResult =
      PowerPointImportService::importFile(filePath, settings, 1);
  succeeded = check(releasedImportResult.succeeded &&
                        releasedImportResult.items.size() == 1 &&
                        !releasedImportResult.generatedDirectory.isEmpty(),
                    "Video conversion did not resume after native deck release") &&
              succeeded;
  if (!releasedImportResult.generatedDirectory.isEmpty()) {
    succeeded = check(PowerPointImportService::discardGeneratedArtifacts(
                          releasedImportResult.generatedDirectory),
                      "Could not remove the released-deck conversion artifacts") &&
                succeeded;
  }
  std::cout << "Program-priority cancel="
            << (canceledImportResult.canceled ? 1 : 0)
            << ", released conversion="
            << (releasedImportResult.succeeded ? 1 : 0) << '\n';
  succeeded = check(manager.shutdown(),
                    "Blocked video-import manager shutdown failed") &&
              succeeded;
  succeeded = check(!isPowerPointNativeSourceActive(),
                    "Native source remained active after blocked video import") &&
              succeeded;
  succeeded = check(powerPointProcessIds() == processIdsBefore,
                    "Blocked video import changed pre-existing PowerPoint processes") &&
              succeeded;
  return succeeded;
}

} // namespace

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  QApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--native-reconnect-smoke")) {
    return verifyNativeReconnectSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      (arguments.at(1) == QStringLiteral("--png-smoke") ||
       arguments.at(1) == QStringLiteral("--video-smoke") ||
       arguments.at(1) == QStringLiteral("--video-smoke-openh264"))) {
    if (arguments.at(1) == QStringLiteral("--video-smoke-openh264")) {
      removeMediaFoundationH264ForFallbackSmoke();
    }
    const PowerPointAddMethod method =
        arguments.at(1) == QStringLiteral("--png-smoke")
            ? PowerPointAddMethod::PngSequence
            : PowerPointAddMethod::VideoConversion;
    return verifyConversionSmoke(arguments.at(2), method) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--thumbnail-smoke")) {
    return verifyThumbnailSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--native-smoke")) {
    return verifyNativeSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--native-black-slide-smoke")) {
    return verifyNativeBlackSlideSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 5 &&
      arguments.at(1) == QStringLiteral("--native-smoke")) {
    bool valid = false;
    const int expectedSlides = arguments.at(3).toInt(&valid);
    return valid && expectedSlides >= 2 &&
                   verifyNativeSmoke(arguments.at(2), expectedSlides, arguments.at(4))
               ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--native-moving-media-smoke")) {
    return verifyNativeMovingMediaSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--native-export-smoke")) {
    return verifyNativeExportIsolationSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--native-video-import-smoke")) {
    return verifyNativeVideoImportIsolationSmoke(arguments.at(2)) ? 0 : 1;
  }
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--discard-generated")) {
    return PowerPointImportService::discardGeneratedArtifacts(
               arguments.at(2))
               ? 0
               : 1;
  }
  return verifyFramePlans() && verifyDialogContract() &&
                 verifyPngSequenceItemSettings() &&
                 verifyCancellationBeforePowerPointOpen()
             ? 0
             : 1;
}
