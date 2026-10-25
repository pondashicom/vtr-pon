#include "program_playout_controller.h"
#include "async_shutdown_test_utils.h"
#include "cue_recovery_test.h"
#include <cstdlib>
#include "bgm_playout_controller.h"
#include "playlist_model.h"
#include "playlist_panel.h"
#include "playlist_view_roles.h"
#include "program_panel.h"
#include "audio_fader_widget.h"
#include <QSpinBox>
#include <QDoubleSpinBox>
#include "user_info.h"
#include "uvc_source_session_manager.h"
#include "uvc_device_monitor.h"
#include "uvc_fault_test_source.h"
#include "operator_timer_panel_test.h"
#include "view_menu_controller.h"
#include "controller_settings.h"
#include "background_io_queue.h"
#include "document_import_queue.h"
#include "language_text.h"
#include <QMenuBar>

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QToolButton>
#include <QFile>
#include <QDir>
#include <gst/gst.h>
#include <Windows.h>
#include <TlHelp32.h>

#include <iostream>
#include <atomic>
#include <cmath>
#include <future>
#include <thread>
#include <memory>
#include <set>

bool runSwitcherControlRegression(ProgramPlayoutController *program);
bool runControllerShortcutsRegression();
bool runDocumentImportQueueRegression(ProgramPlayoutController *program);

namespace {
bool require(bool condition, const char *message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}
std::set<DWORD> powerPointProcessIds() {
  std::set<DWORD> ids;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return ids;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"POWERPNT.EXE") == 0) ids.insert(entry.th32ProcessID);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return ids;
}
#define CHECK(condition, message) \
  do { if (!require((condition), message)) return false; } while (false)

// Confined to this process: exercise normal PROGRAM initialization without
// opening a physical audio device or changing the user's output preference.
GWeakRef programTestSink;
bool substituteProgramSink() {
  GstElement *fake = gst_element_factory_make("fakesink", nullptr);
  if (!fake) return false;
  const GType fakeType = G_OBJECT_TYPE(fake);
  gst_object_unref(fake);
  g_weak_ref_init(&programTestSink, nullptr);
  GTypeQuery query{};
  g_type_query(fakeType, &query);
  GTypeInfo info{};
  info.class_size = static_cast<guint16>(query.class_size);
  info.instance_size = static_cast<guint16>(query.instance_size);
  info.instance_init = [](GTypeInstance *instance, gpointer) {
    g_weak_ref_set(&programTestSink, G_OBJECT(instance));
  };
  const GType type = g_type_register_static(fakeType, "ProgramFaultTestSink", &info, GTypeFlags(0));
  GstRegistry *registry = gst_registry_get();
  if (auto *feature = gst_registry_find_feature(
          registry, "wasapi2sink", GST_TYPE_ELEMENT_FACTORY)) {
    gst_registry_remove_feature(registry, feature);
    gst_object_unref(feature);
  }
  return gst_element_register(nullptr, "wasapi2sink", GST_RANK_NONE, type);
}

bool createVideo(const QString &path, bool audio = false, bool av = false) {
  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(
      av ? "avimux name=mux ! filesink name=output "
      "videotestsrc num-buffers=300 ! video/x-raw,width=160,height=90,framerate=30/1 ! jpegenc ! queue ! mux. "
      "audiotestsrc num-buffers=469 ! audio/x-raw,rate=48000,channels=2,format=S16LE ! queue ! mux." :
      audio ? "audiotestsrc num-buffers=500 ! audioconvert ! wavenc ! filesink name=output" :
      "videotestsrc num-buffers=300 ! "
      "video/x-raw,width=160,height=90,framerate=30/1 ! "
      "jpegenc ! avimux ! filesink name=output", &error);
  if (error || !pipeline) {
    if (error) g_error_free(error);
    if (pipeline) gst_object_unref(pipeline);
    return false;
  }
  GstElement *output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
  g_object_set(output, "location", path.toUtf8().constData(), nullptr);
  gst_object_unref(output);
  GstBus *bus = gst_element_get_bus(pipeline);
  const bool started = gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
                       GST_STATE_CHANGE_FAILURE;
  GstMessage *message = started ? gst_bus_timed_pop_filtered(
      bus, 10 * GST_SECOND,
      static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)) : nullptr;
  const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message) gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return ok;
}

bool uvcInitialFailureRegression() {
  PlaylistModel model;
  UvcSourceSessionManager manager(false);
  const QString path = QStringLiteral("initial-failure-camera");
  PlaylistItemRegistration item;
  item.sourceType = PlaylistSourceType::Uvc;
  item.mediaType = PlaylistMediaType::Video;
  item.filePath = path;
  item.displayName = path;
  const auto ids = model.addItems({item});
  CHECK(ids.size() == 1, "initial camera item is registered");
  QObject::connect(&manager, &UvcSourceSessionManager::sourceAvailabilityChanged,
                   &model, &PlaylistModel::setUvcDeviceAvailable);
  manager.setAvailableDevicePaths({path});
  manager.setRegisteredDevicePaths({path});
  CHECK(!manager.sourceAvailable(path) && !manager.sourceOffline(path),
        "a newly added camera is awaiting frames without an offline fault");
  manager.reportSourceFailure(path, QStringLiteral("Injected initial open failure"));
  CHECK(manager.sourceOffline(path) && model.itemMediaOffline(ids[0]),
        "a failure before the first frame still marks the item offline");
  manager.setAvailableDevicePaths({path});
  CHECK(manager.sourceOffline(path) && model.itemMediaOffline(ids[0]),
        "device presence alone does not clear a confirmed input failure");
  manager.setRegisteredDevicePaths({});
  manager.setAvailableDevicePaths({});
  manager.setRegisteredDevicePaths({path});
  CHECK(manager.sourceOffline(path),
        "an item restored after discovery knows its camera is absent");
  return true;
}

bool uvcBoundedShutdownRegression() {
  struct Unblock {
    ~Unblock() { UvcFaultTest::holdOpen.store(false); UvcFaultTest::holdStop.store(false); }
  } unblock;
  for (const bool opening : {false, true}) {
    const int baseline = UvcFaultTest::liveSources.load();
    auto manager = std::make_unique<UvcSourceSessionManager>(false);
    const QString path = QStringLiteral("shutdown-fault-camera");
    UvcFaultTest::openEntered.store(false);
    UvcFaultTest::stopEntered.store(false);
    UvcFaultTest::holdOpen.store(opening);
    manager->setRegisteredDevicePaths({path});
    manager->setPreviewDevicePaths({path});
    CHECK(awaitShutdown([&] { return opening ? UvcFaultTest::openEntered.load()
                                            : manager->sourceAvailable(path); }, 6000),
          "shutdown fixture reaches the stalled open or active capture");
    if (!opening) UvcFaultTest::holdStop.store(true);
    QElapsedTimer elapsed; elapsed.start();
    manager->requestShutdown();
    CHECK(awaitShutdown([&] { return manager->shutdownComplete(); }, 5000),
          "application shutdown completes even when a camera driver never returns");
    CHECK(elapsed.elapsed() >= 2500 && elapsed.elapsed() < 5000,
          "shutdown gives the isolated worker a bounded cleanup interval");
    CHECK(UvcFaultTest::liveSources.load() > baseline,
          "a blocked camera retains its callback and pipeline storage");
    manager.reset();
    UvcFaultTest::holdOpen.store(false);
    UvcFaultTest::holdStop.store(false);
    CHECK(awaitShutdown([&] { return UvcFaultTest::liveSources.load() == baseline; }, 6000),
          "late cleanup finishes safely after the manager is destroyed");
  }
  std::cout << "UVC bounded shutdown: PASS" << std::endl;
  return true;
}
} // namespace

// Access only to stop the periodic timer and place edits at deterministic
// preroll/Ready/TAKE boundaries. All media preparation uses the real player.
class ProgramPlayoutTransitionTest {
public:
  bool documentImports() {
    std::atomic_bool conversionCanceled = false;
    controller_.powerPointConversionInProgress_ = true;
    controller_.powerPointConversionCancelRequested_ = &conversionCanceled;
    ProgramPlayoutController::CueRequest nativeCue;
    nativeCue.itemId = 42;
    nativeCue.sourceType = PlaylistSourceType::PowerPoint;
    nativeCue.path = QStringLiteral("queued-native.pptx");
    controller_.scheduleCueRequest(nativeCue);
    CHECK(!conversionCanceled.load() && !controller_.cue_ &&
              !controller_.pendingCue_ &&
              controller_.player_.cueStatus() == DualDeckPlayer::CueStatus::Empty,
          "selecting native PowerPoint waits without canceling active conversion");
    controller_.finishPowerPointConversion();
    CHECK(!controller_.powerPointConversionInProgress_ &&
              !controller_.powerPointConversionCancelRequested_ &&
              !conversionCanceled.load(),
          "conversion completion releases the native CUE wait");
    QTemporaryDir fixtureDirectory;
    CHECK(fixtureDirectory.isValid(), "on-air document queue fixture directory exists");
    const QString stillPath = fixtureDirectory.filePath(QStringLiteral("on-air.png"));
    QImage still(320, 180, QImage::Format_RGB32);
    still.fill(QColor(25, 100, 170));
    CHECK(still.save(stillPath) &&
              start(stillPath, PlaylistEndMode::Pause, PlaylistMediaType::StillImage, 0, 8000),
          "unrelated still is on air during document failures");
    const auto readFrame = [this] {
      const auto capture = controller_.captureStillFrame();
      if (!capture) return QImage{};
      std::packaged_task<QImage()> task(capture);
      auto future = task.get_future();
      std::thread(std::move(task)).detach();
      if (!await([&] { return future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }))
        return QImage{};
      return future.get();
    };
    const auto onAirId = model_.onAirItemId();
    const auto position = controller_.programPositionMs();
    QImage before;
    CHECK(await([&] {
      before = readFrame();
      if (before.isNull()) return false;
      const QColor center = before.pixelColor(before.width() / 2, before.height() / 2);
      return std::abs(center.red() - 25) <= 4 && std::abs(center.green() - 100) <= 4 &&
             std::abs(center.blue() - 170) <= 4;
    }), "authored on-air GPU frame is present before failures");
    const bool importsPassed = runDocumentImportQueueRegression(&controller_);
    const QImage after = readFrame();
    CHECK(model_.onAirItemId() == onAirId && controller_.programPositionMs() == position &&
              !controller_.programIsPlaying() && before == after,
          "document cancellation and cleanup leave unrelated on-air state and final frame intact");
    std::cout << "Document import on-air frame/state retention: PASS\n";
    return importsPassed;
  }

  bool powerPointQueueSelectionSmoke(const QString &nativePath,
                                     const QString &conversionPath) {
    QTemporaryDir generated;
    CHECK(generated.isValid(), "PowerPoint smoke output directory exists");
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue(QStringLiteral("files/documentConversionDirectory"),
                      generated.path());
    settings.sync();
    BackgroundIoQueue fileIo;
    DocumentImportQueue queue(&model_, &controller_, &info_, &fileIo);
    PowerPointAddSettings native;
    PowerPointAddSettings video;
    video.method = PowerPointAddMethod::VideoConversion;
    video.slideDurationSeconds = 0.5;
    video.crossDissolveEnabled = false;
    PowerPointAddSettings png;
    png.method = PowerPointAddMethod::PngSequence;
    QString error;
    CHECK(queue.enqueuePowerPoint(nativePath, native, 1, &error),
          "native PowerPoint is queued");
    CHECK(queue.enqueuePowerPoint(conversionPath, video, 1, &error),
          "video conversion is queued");
    CHECK(queue.enqueuePowerPoint(conversionPath, png, 1, &error),
          "PNG conversion is queued");
    const quint64 nativeId = model_.itemIdAt(0);
    const quint64 videoId = model_.itemIdAt(1);
    const quint64 pngId = model_.itemIdAt(2);
    CHECK(nativeId && videoId && pngId && nativeId != videoId &&
              videoId != pngId,
          "three PowerPoint methods retain distinct placeholders");
    queue.startNext();
    CHECK(await([&] {
      return controller_.powerPointConversionInProgress_ &&
             model_.itemConversionInProgress(videoId);
    }, 20000), "video conversion becomes active after native registration");
    (void)operations_.execute(ControllerOperations::Action::SelectItem, nativeId);
    CHECK(playlist_.selectedItemId() == nativeId && queue.contains(videoId) &&
              model_.itemState(videoId),
          "selecting native PowerPoint keeps the video conversion item");
    CHECK(await([&] { return !queue.hasPendingImports(); }, 120000),
          "video and PNG conversions finish after native selection");
    const auto nativeItem = model_.itemState(nativeId);
    const auto videoItem = model_.itemState(videoId);
    const auto pngItem = model_.itemState(pngId);
    CHECK(nativeItem && videoItem && pngItem &&
              nativeItem->sourceType == PlaylistSourceType::PowerPoint &&
              videoItem->powerPointMode == PlaylistPowerPointMode::VideoConversion &&
              pngItem->powerPointMode == PlaylistPowerPointMode::PngSequence &&
              QFileInfo::exists(videoItem->filePath) &&
              QFileInfo::exists(pngItem->filePath),
          "all three PowerPoint methods remain in the playlist");
    CHECK(await([&] {
      return model_.cueItemId() == nativeId &&
             model_.cueState() == PlaylistCueState::Ready;
    }, 20000), "native CUE starts after both conversions finish");
    controller_.scheduleCueRequest(std::nullopt);
    CHECK(await([&] {
      return model_.cueItemId() == 0 &&
             controller_.player_.cueStatus() == DualDeckPlayer::CueStatus::Empty;
    }, 20000), "native CUE is released before smoke teardown");
    controller_.requestShutdown();
    CHECK(awaitShutdown([&] { return controller_.shutdownComplete(); }, 60000),
          "PowerPoint conversion and native CUE shut down cleanly");
    std::cout << "PowerPoint queue selection smoke: PASS\n";
    return true;
  }

  bool selfTransition(const QString &path, PlaylistEndMode mode,
                      bool allowManualRetake) {
    CHECK(controller_.isAvailable(), "self-transition player initializes");
    CHECK(await([&] {
      return controller_.player_.programOutputDeviceStatusKey() ==
             QStringLiteral("settings.output.applied");
    }), "self-transition output initializes");
    PlaylistItemRegistration registration;
    registration.filePath = path;
    registration.mediaType = PlaylistMediaType::Video;
    registration.settings.outPointMs = 6000;
    registration.settings.endMode = mode;
    ids_ = model_.addItems({registration});
    CHECK(ids_.size() == 1, "one playable item is registered");
    const quint64 itemId = ids_.constFirst();
    if (mode == PlaylistEndMode::Goto) {
      auto settings = model_.itemState(itemId)->settings;
      settings.gotoTargetItemId = itemId;
      CHECK(model_.updateItemSettings(itemId, settings),
            "self GOTO destination is saved");
    }
    CHECK(model_.automaticEndTargetItemIdForPlayout(itemId) == itemId,
          "self is the automatic destination");
    const auto item = *model_.itemState(itemId);
    controller_.scheduleCueRequest(controller_.cueRequestFor(item));
    CHECK(await([&] { return ready(); }), "initial CUE is Ready");
    CHECK(controller_.takeCueOnAir(item, false, 1000),
          "initial item goes ON AIR");
    const int initialDeck = controller_.player_.currentDeckIndex();
    controller_.setAllowSameItemOnAirRetake(allowManualRetake);
    controller_.scheduleCueRequest(std::nullopt);
    CHECK(waitReady() && transition().itemId == itemId &&
              controller_.cue_ && controller_.cue_->itemId == itemId,
          "automatic self CUE is Ready on the spare deck");
    CHECK(model_.onAirItemId() == itemId &&
              controller_.player_.currentDeckIndex() == initialDeck,
          "preload leaves the outgoing deck ON AIR");
    transition().due = true;
    CHECK(controller_.takePreparedNextItem(),
          "automatic self TAKE succeeds");
    CHECK(model_.onAirItemId() == itemId &&
              controller_.player_.currentDeckIndex() == 1 - initialDeck,
          "self transition hands off to the other deck");
    return true;
  }

  ~ProgramPlayoutTransitionTest() {
    controller_.requestShutdown();
    if (!require(awaitShutdown([&] { return controller_.shutdownComplete(); }),
                 "PROGRAM worker finishes before transition fixture destruction")) std::abort();
    uvcSources_.requestShutdown();
    if (!require(awaitShutdown([&] { return uvcSources_.shutdownComplete(); }),
                 "UVC workers finish before transition fixture destruction")) std::abort();
  }
  enum class EditTiming { Loading, Ready, Due };

  bool macroItemIdRequests(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause), "macro fixture has a paused PROGRAM item");
    auto *list = playlist_.findChild<QListWidget *>();
    CHECK(list, "macro fixture exposes normal playlist selection");
    list->setCurrentRow(model_.indexOf(ids_[1]));
    CHECK(await([&] { return ready(); }), "operator CUE settles before macro");
    struct Completion { quint64 id; bool success; };
    QList<Completion> completions;
    int interventions = 0;
    int reservationFailures = 0;
    bool interventionBeforeReplacement = false;
    QObject listener;
    QObject::connect(&controller_, &ProgramPlayoutController::macroRequestFinished,
                     &listener, [&](quint64 id, bool success, const QString &) {
      completions.push_back({id, success});
    });
    QObject::connect(&controller_, &ProgramPlayoutController::macroIntervention,
                     &listener, [&](const QString &) {
      ++interventions;
      interventionBeforeReplacement = controller_.cue_ &&
          controller_.cue_->itemId == ids_[2];
    });
    QObject::connect(&controller_, &ProgramPlayoutController::macroReservationFailed,
                     &listener, [&](const QString &) { ++reservationFailures; });
    QString error;
    CHECK(controller_.requestMacroCue(101, ids_[2], &error), "macro CUE accepts item ID");
    CHECK(await([&] { return !completions.isEmpty(); }), "macro CUE reports completion");
    CHECK(completions.last().id == 101 && completions.last().success &&
          playlist_.selectedItemId() == ids_[1] && model_.onAirItemId() == ids_[0],
          "macro preparation preserves UI selection and existing ON AIR");
    CHECK(controller_.macroCueOwner_ && !controller_.macroRequest_,
          "CUE ownership survives completion until timed TAKE or cancellation");
    auto settings = model_.itemState(ids_[1])->settings;
    settings.startMode = PlaylistStartMode::Play;
    CHECK(model_.updateItemSettings(ids_[1], settings), "selected item mode edit succeeds");
    CHECK(interventions == 0 && controller_.cue_->itemId == ids_[2],
          "mode-only model notification does not replace macro CUE or pause runner");
    auto macroSettings = model_.itemState(ids_[2])->settings;
    macroSettings.inPointMs = 500;
    CHECK(model_.updateItemSettings(ids_[2], macroSettings) &&
          !controller_.macroCueOwner_ && interventions == 0,
          "an intentional settings step invalidates old CUE without pausing itself");
    CHECK(controller_.requestMacroCue(110, ids_[2], &error) &&
          await([&] { return completions.last().id == 110; }) &&
          controller_.cue_->inPointMs == 500,
          "macro source-range update prepares the new exact media key");
    CHECK(!controller_.requestMacroTake(102, ids_[1], true, false, 1000, &error) &&
          !error.isEmpty() && !controller_.macroRequest_,
          "timed TAKE rejects an exact-target mismatch without reserving late TAKE");
    CHECK(controller_.requestMacroTake(103, ids_[2], true, false, 1000, &error),
          "prepared item-ID TAKE accepts without selecting UI target");
    CHECK(completions.last().id == 103 && completions.last().success &&
          model_.onAirItemId() == ids_[2] && playlist_.selectedItemId() == ids_[1],
          "item-ID TAKE completes synchronously through normal PROGRAM path");
    CHECK(!controller_.requestMacroCue(104, ids_[2], &error),
          "macro retains the same-item retake guard");
    CHECK(controller_.executeMacroTransport(QStringLiteral("play"), &error) &&
          controller_.programIsPlaying(), "macro Play verifies playback");
    CHECK(controller_.executeMacroTransport(QStringLiteral("pause"), &error) &&
          !controller_.programIsPlaying(), "macro Pause verifies playback");
    CHECK(!controller_.setMacroPlaybackRate(4.0, &error), "unsupported macro rate is rejected");
    CHECK(controller_.setMacroPlaybackRate(1.25, &error) && controller_.playbackRate_ == 1.25,
          "macro rate verifies the applied value");

    controller_.scheduleCueRequest(std::nullopt);
    const auto completionCount = completions.size();
    CHECK(controller_.requestMacroTake(105, ids_[0], false, false, 1000, &error) &&
          controller_.macroRequest_, "normal macro TAKE may wait for preparation");
    controller_.cancelMacroRequests();
    CHECK(await([&] { return ready(); }), "cancelled request's late preroll still finishes");
    CHECK(model_.onAirItemId() == ids_[2] && completions.size() == completionCount &&
          !controller_.pendingTakeItemId_, "late readiness cannot TAKE after macro cancellation");

    CHECK(controller_.requestMacroCue(111, ids_[0], &error) && controller_.macroCueOwner_,
          "macro retains a completed CUE before its own OFF AIR step");
    CHECK(controller_.executeMacroTransport(QStringLiteral("offAir"), &error) &&
          reservationFailures == 0 && !controller_.macroCueOwner_,
          "macro OFF AIR verifies the resulting state");
    CHECK(controller_.requestMacroCue(106, ids_[2], &error), "second macro CUE starts");
    CHECK(await([&] { return completions.last().id == 106; }), "second macro CUE completes");
    controller_.setAutoSelectNextItemAfterOnAir(true);
    controller_.selectNextItemAfterOnAir(ids_[1]);
    CHECK(await([&] { return playlist_.selectedItemId() == ids_[2]; }) &&
          interventions == 0 && controller_.macroCueOwner_ &&
          controller_.cue_->itemId == ids_[2],
          "internal automatic selection preserves macro ownership and does not pause it");
    controller_.setAutoSelectNextItemAfterOnAir(false);
    list->setCurrentRow(model_.indexOf(ids_[0]));
    CHECK(interventions == 1 && interventionBeforeReplacement && !controller_.macroCueOwner_,
          "selection pauses macro before replacing its completed CUE reservation");
    controller_.pendingTakeItemId_ = ids_[0];
    controller_.cancelMacroRequests();
    CHECK(controller_.pendingTakeItemId_ == ids_[0],
          "cancelling macro does not cancel an unrelated manual pending TAKE");
    controller_.pendingTakeItemId_.reset();
    list->setCurrentRow(model_.indexOf(ids_[1]));
    CHECK(interventions == 2 && !controller_.macroCueOwner_,
          "real selection also notifies a runner waiting without a CUE reservation");
    CHECK(controller_.requestMacroTake(107, ids_[2], false, false, 1000, &error),
          "normal macro TAKE starts for an unselected unprepared item");
    CHECK(await([&] { return model_.onAirItemId() == ids_[2]; }) &&
          completions.last().id == 107 && completions.last().success,
          "normal macro TAKE waits for readiness and takes the exact requested item");
    model_.setDskItemId(ids_[0]);
    CHECK(!controller_.requestMacroCue(108, ids_[0], &error),
          "macro rejects a target currently owned by DSK");
    model_.clearDskItem();
    PlaylistItemRegistration unsupported;
    unsupported.sourceType = PlaylistSourceType::Uvc;
    unsupported.mediaType = PlaylistMediaType::Video;
    unsupported.filePath = QStringLiteral("unsupported-macro-camera");
    unsupported.displayName = QStringLiteral("Unsupported macro camera");
    const auto unsupportedIds = model_.addItems({unsupported});
    CHECK(unsupportedIds.size() == 1, "unsupported-source fixture registers a valid UVC item");
    CHECK(!controller_.requestMacroCue(109, unsupportedIds.front(), &error),
          "macro rejects live sources before sending a CUE request");
    std::cout << "Macro item-ID CUE/TAKE: PASS\n";
    return true;
  }

  bool macroAutomaticPriority(const QString &path, bool pendingTake = false,
                              bool readyTakeRace = false) {
    CHECK(start(path, PlaylistEndMode::Next), "macro priority fixture uses END NEXT");
    int failures = 0;
    int completions = 0;
    QObject listener;
    QObject::connect(&controller_, &ProgramPlayoutController::macroReservationFailed,
                     &listener, [&](const QString &) { ++failures; });
    QObject::connect(&controller_, &ProgramPlayoutController::macroRequestFinished,
                     &listener, [&](quint64, bool success, const QString &) {
      if (success) ++completions;
    });
    QString error;
    if (pendingTake) {
      CHECK(controller_.requestMacroTake(201, ids_[2], false, false, 1000, &error) &&
            controller_.macroRequest_, "normal macro TAKE can wait for its preroll");
    } else {
      CHECK(controller_.requestMacroCue(201, ids_[2], &error), "macro can prepare before automatic preload");
      CHECK(await([&] { return completions == 1; }), "macro preparation completes before preload");
    }
    if (readyTakeRace) {
      CHECK(controller_.player_.seek(5500), "advance PROGRAM to the NEXT preload boundary");
      CHECK(await([&] { return controller_.programPositionMs() >= 5000; }),
            "PROGRAM reaches NEXT boundary between normal bus polls");
      CHECK(!controller_.requestMacroTake(203, ids_[2], true, false, 1000, &error),
            "a synchronous ready macro TAKE cannot outrun due automatic preparation");
    } else {
      CHECK(controller_.maybePrepareNextItem(Target::Program, 5000, 6000),
            "automatic NEXT can reclaim the spare deck");
    }
    CHECK(failures == 1 && !controller_.macroCueOwner_ &&
          controller_.nextTransition_.itemId == ids_[1],
          "automatic preparation reports macro failure even after completed CUE");
    CHECK(!controller_.requestMacroTake(202, ids_[2], false, false, 1000, &error),
          "macro cannot replace an existing automatic reservation");
    CHECK(await([&] { return ready(); }), "automatic target still finishes preparation");
    CHECK(model_.onAirItemId() == ids_[0], "preempted macro never takes its target after late readiness");
    controller_.nextTransition_.due = true;
    CHECK(controller_.takePreparedNextItem() && model_.onAirItemId() == ids_[1],
          "existing automatic transition remains operative after macro conflict");
    std::cout << "Macro automatic CUE priority: PASS\n";
    return true;
  }

  bool macroTimedStillTransport(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause, PlaylistMediaType::StillImage, 0, 2000),
          "macro transport fixture prepares a real timed still");
    QList<quint64> completed;
    bool success = false;
    QObject listener;
    QObject::connect(&controller_, &ProgramPlayoutController::macroRequestFinished,
                     &listener, [&](quint64 id, bool ok, const QString &) {
      completed.append(id);
      success = ok;
    });
    const auto clearStillFrame = [&] {
      if (!controller_.player_.stopAtInAndClearProgramFrame()) return false;
      controller_.onAirTimedStillClock_.holdAtEnd();
      return controller_.onAirStateMachine_.markEnded();
    };
    QString error;
    CHECK(clearStillFrame(), "clear PROGRAM frame to require real decoder/GPU restoration");
    CHECK(controller_.requestMacroTransport(301, QStringLiteral("play"), &error),
          "macro Play accepts restoration without treating dispatch as completion");
    CHECK(await([&] {
      controller_.updatePlaybackUi();
      return completed.contains(301);
    }) && success && controller_.programIsPlaying(),
          "deferred timed-still Play reports completion only once playback starts");
    CHECK(controller_.requestMacroTransport(302, QStringLiteral("pause"), &error) &&
          completed.last() == 302 && success && !controller_.programIsPlaying(),
          "macro Pause uses the same request-completion contract");
    CHECK(clearStillFrame(), "clear timed still again for cancellation");
    CHECK(controller_.requestMacroTransport(303, QStringLiteral("play"), &error) &&
          controller_.macroTransportRequest_ && controller_.onAirTimedStillRestorePending_,
          "real timed-still preroll is pending before cancellation");
    controller_.cancelMacroRequests();
    CHECK(!controller_.macroTransportRequest_ && !controller_.onAirTimedStillRestorePending_,
          "cancellation removes only the macro-owned deferred Play");
    CHECK(await([&] {
      return controller_.player_.restoreCurrentProgramFrame() ==
          DualDeckPlayer::ProgramFrameRestoreStatus::Ready;
    }), "cancelled decoder/GPU restoration may still finish");
    controller_.updatePlaybackUi();
    CHECK(!controller_.programIsPlaying() && !completed.contains(303),
          "late restored frame does not restart playback or complete a cancelled request");
    controller_.playOnAir();
    CHECK(await([&] { controller_.updatePlaybackUi(); return controller_.programIsPlaying(); }),
          "ordinary manual Play still works after cancelling macro restoration");
    std::cout << "Macro timed-still asynchronous transport: PASS\n";
    return true;
  }

  bool audioOutputFailureMessage(const QString &path, bool hasAudio = false) {
    auto audibleBuffers = std::make_shared<std::atomic_uint>(0);
    auto *initialSink = GST_ELEMENT(g_weak_ref_get(&programTestSink));
    CHECK(initialSink != nullptr, "audio sink exists before recovery");
    auto *audioPad = gst_element_get_static_pad(initialSink, "sink");
    gst_pad_add_probe(audioPad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
          GstMapInfo map{};
          if (gst_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), &map, GST_MAP_READ)) {
            const auto *samples = reinterpret_cast<const float *>(map.data);
            for (gsize index = 0; index < map.size / sizeof(float); ++index) {
              if (std::abs(samples[index]) > 0.001F) {
                ++**static_cast<std::shared_ptr<std::atomic_uint> *>(data);
                break;
              }
            }
            gst_buffer_unmap(GST_PAD_PROBE_INFO_BUFFER(info), &map);
          }
          return GST_PAD_PROBE_OK;
        }, new std::shared_ptr<std::atomic_uint>(audibleBuffers), [](gpointer data) {
          delete static_cast<std::shared_ptr<std::atomic_uint> *>(data);
        });
    gst_object_unref(audioPad);
    gst_object_unref(initialSink);
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = PlaylistMediaType::Video;
    item.settings.startMode = PlaylistStartMode::Play;
    const auto ids = model_.addItems({item});
    CHECK(ids.size() == 1, "audio failure fixture registers video");
    controller_.scheduleCueRequest(controller_.cueRequestFor(*model_.itemState(ids[0])));
    CHECK(await([&] { return ready() && controller_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.failed"); }), "video CUE is ready while audio output failed");
    CHECK(controller_.takeCueOnAir(*model_.itemState(ids[0]), false, 1000),
          "unavailable audio does not block ON AIR");
    CHECK(model_.onAirItemId() == ids[0] && controller_.programIsPlaying(),
          "video starts while the audio endpoint is unavailable");
    CHECK(await([&] { return controller_.player_.positionMs() >= 250; }),
          "media position advances without an audio output consumer");
    controller_.pauseOnAir();
    controller_.playOnAir();
    CHECK(controller_.programIsPlaying(), "manual Play also works without audio output");
    controller_.requestProgramOutputDevice({});
    CHECK(await([&] { return controller_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.applied"); }), "valid audio output recovers");
    CHECK(model_.onAirItemId() == ids[0] && controller_.programIsPlaying(),
          "audio output recovers while video remains ON AIR");
    CHECK(!hasAudio || await([&] { return audibleBuffers->load() >= 3; }),
          "recovered output actually receives non-silent PCM");
    int audioFaults = 0;
    QObject faultObserver;
    QObject::connect(&controller_, &ProgramPlayoutController::programAudioOutputError,
                     &faultObserver, [&](const QString &) { ++audioFaults; });
    auto *sink = GST_ELEMENT(g_weak_ref_get(&programTestSink));
    CHECK(sink != nullptr, "test audio output is available for fault injection");
    GstPad *blockedPad = gst_element_get_static_pad(sink, "sink");
    const gulong blockedProbe = gst_pad_add_probe(blockedPad,
        GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
        [](GstPad *, GstPadProbeInfo *, gpointer) { return GST_PAD_PROBE_OK; }, nullptr, nullptr);
    gst_element_message_full(sink, GST_MESSAGE_ERROR, GST_RESOURCE_ERROR,
        GST_RESOURCE_ERROR_WRITE, g_strdup("Injected audio output disconnect"),
        nullptr, __FILE__, G_STRFUNC, __LINE__);
    gst_object_unref(sink);
    controller_.updatePlaybackUi();
    CHECK(audioFaults == 1 && controller_.programIsPlaying() && model_.onAirItemId() == ids[0],
          "a runtime audio disconnect reports the device fault without stopping PROGRAM");
    const qint64 faultPosition = controller_.player_.positionMs();
    const bool advancedWhileBlocked = await([&] {
      controller_.updatePlaybackUi();
      return !controller_.programIsPlaying() || controller_.player_.positionMs() >= faultPosition + 700;
    });
    gst_pad_remove_probe(blockedPad, blockedProbe);
    gst_object_unref(blockedPad);
    CHECK(advancedWhileBlocked && controller_.programIsPlaying(),
          "media keeps advancing beyond the audio queue limit while the failed consumer is blocked");
    const auto audibleBeforeRecovery = audibleBuffers->load();
    controller_.requestProgramOutputDevice({});
    CHECK(await([&] { return controller_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.applied"); }), "runtime audio failure can recover during ON AIR");
    CHECK(!hasAudio || await([&] { return audibleBuffers->load() >= audibleBeforeRecovery + 3; }),
          "audio PCM resumes after a stalled output restarts");
    DualDeckPlayer::TakeCueFailure failure = DualDeckPlayer::TakeCueFailure::AudioOutputUnavailable;
    CHECK(!controller_.player_.takeCue(false, 1.0F, &failure) &&
          failure == DualDeckPlayer::TakeCueFailure::Other,
          "empty CUE failure does not retain a stale audio error");
    controller_.clearOnAir();
    controller_.requestProgramOutputDevice(QStringLiteral("unsupported-device"));
    CHECK(await([&] { return controller_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.failed"); }), "a new device setting can fail independently");
    CHECK(controller_.programAudioOutputReady(),
          "a rejected setting with working old output must not report audio unavailable");
    std::cout << "Audio unavailable, stalled output and live recovery (audio=" << hasAudio << "): PASS\n";
    return true;
  }

  bool fileCueRecovery(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause), "existing PROGRAM is prepared");
    controller_.playOnAir();
    for (int index = 0; index < 40; ++index) {
      controller_.scheduleCueRequest(controller_.cueRequestFor(*model_.itemState(ids_[1 + index % 2])));
      CHECK(controller_.cue_ && controller_.cue_->itemId == ids_[1 + index % 2],
            "rapid replacement prepares the newest request without waiting for old preroll");
    }
    CHECK(await([&] { return ready(); }), "newest CUE becomes Ready after rapid replacements");
    CHECK(model_.onAirItemId() == ids_[0] && controller_.programIsPlaying(),
          "replacing CUE preserves current PROGRAM");
    auto *list = playlist_.findChild<QListWidget *>();
    CHECK(list != nullptr, "playlist list exists");
    list->setCurrentRow(model_.indexOf(ids_[1]));
    auto invalid = model_.itemState(ids_[1])->settings;
    invalid.inPointMs = 20000;
    invalid.outPointMs.reset();
    CHECK(model_.updateItemSettings(ids_[1], invalid), "inject invalid media range");
    CHECK(await([&] { return controller_.player_.cueStatus() == DualDeckPlayer::CueStatus::Error; }),
          "invalid IN becomes a failed CUE");
    controller_.takeSelectedPlaylistItemOnAir();
    CHECK(controller_.player_.cueStatus() != DualDeckPlayer::CueStatus::Error &&
          controller_.pendingTakeItemId_ == ids_[1], "explicit ON AIR retries same failed key");
    invalid.inPointMs = 0;
    CHECK(model_.updateItemSettings(ids_[1], invalid), "restore playable range");
    controller_.takeSelectedPlaylistItemOnAir();
    CHECK(await([&] { return model_.onAirItemId() == ids_[1]; }), "recovered file goes ON AIR");
    std::cout << "CUE replacement x40 and same-item retry: PASS" << std::endl;
    return true;
  }

  bool totalFutureCache(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Next), "PROGRAM prepares the TOTAL fixture");
    auto current = model_.itemState(ids_[0])->settings;
    current.startMode = PlaylistStartMode::Play;
    CHECK(model_.updateItemSettings(ids_[0], current), "current row is continuous");
    auto *totalMode = panel_.findChild<QPushButton *>(
        QStringLiteral("programRemainingTotalModeButton"));
    CHECK(totalMode != nullptr, "TOTAL mode control exists");
    totalMode->setChecked(true);
    auto next = model_.itemState(ids_[1])->settings;
    next.startMode = PlaylistStartMode::Play;
    CHECK(model_.updateItemSettings(ids_[1], next), "NEXT item becomes continuous");
    auto *remaining = panel_.findChild<QLabel *>(QStringLiteral("programRemainingTime"));
    CHECK(remaining != nullptr, "PROGRAM remaining display exists");
    controller_.updateTotalRemainingTime(5000);
    CHECK(remaining->text() == QStringLiteral("00:00:13.00"),
          "PROGRAM displays the combined TOTAL");
    CHECK(model_.totalRemainingForPlayout(ids_[0], {5000, 1, 1.0}).durationMs == 13000,
          "future duration is included in TOTAL");
    controller_.updateTotalRemainingTime(4000);
    CHECK(remaining->text() == QStringLiteral("00:00:12.00"),
          "ordinary tick updates the displayed TOTAL");
    CHECK(model_.totalRemainingForPlayout(ids_[0], {4000, 1, 1.0}).durationMs == 12000,
          "ordinary tick updates the current duration");
    next.outPointMs = 9000;
    CHECK(model_.updateItemSettings(ids_[1], next), "future OUT changes");
    controller_.updateTotalRemainingTime(4000);
    CHECK(remaining->text() == QStringLiteral("00:00:13.00"),
          "changed future item updates the displayed TOTAL");
    CHECK(model_.totalRemainingForPlayout(ids_[0], {4000, 1, 1.0}).durationMs == 13000,
          "next tick reflects the changed future duration");

    next.outPointMs = 6000;
    next.endMode = PlaylistEndMode::Next;
    CHECK(model_.updateItemSettings(ids_[1], next), "NEXT chain continues");
    auto third = model_.itemState(ids_[2])->settings;
    third.startMode = PlaylistStartMode::Play;
    third.endMode = PlaylistEndMode::Next;
    third.outPointMs = 6000;
    CHECK(model_.updateItemSettings(ids_[2], third), "third row continues");
    QList<PlaylistItemRegistration> longChain;
    constexpr int kLongChainCount = 1200;
    longChain.reserve(kLongChainCount);
    for (int row = 0; row < kLongChainCount; ++row) {
      PlaylistItemRegistration item;
      item.filePath = path;
      item.mediaType = PlaylistMediaType::Video;
      item.settings.startMode = PlaylistStartMode::Play;
      item.settings.endMode = row + 1 == kLongChainCount
                                  ? PlaylistEndMode::Pause
                                  : PlaylistEndMode::Next;
      item.settings.outPointMs = 6000;
      longChain.append(item);
    }
    CHECK(model_.addItems(longChain).size() == kLongChainCount,
          "long NEXT chain is registered");
    QElapsedTimer initial;
    initial.start();
    controller_.updateTotalRemainingTime(5000);
    const qint64 rebuildUs = initial.nsecsElapsed() / 1000;
    CHECK(model_.totalRemainingForPlayout(ids_[0], {5000, 1, 1.0}).durationMs ==
              5000 + (kLongChainCount + 2) * 6000,
          "long future total is correct");
    QElapsedTimer ticks;
    ticks.start();
    for (int tick = 0; tick < 1000; ++tick) {
      controller_.updateTotalRemainingTime(5000 - tick % 1000);
    }
    const qint64 tickTotalUs = ticks.nsecsElapsed() / 1000;
    qint64 queuedEventUs = -1;
    QElapsedTimer response;
    response.start();
    QTimer::singleShot(0, &panel_, [&] {
      queuedEventUs = response.nsecsElapsed() / 1000;
    });
    controller_.updateTotalRemainingTime(4000);
    QApplication::processEvents();
    CHECK(queuedEventUs >= 0, "queued UI event remains responsive");
    std::cout << "TOTAL benchmark: rows=" << kLongChainCount + 3
              << " rebuild_us=" << rebuildUs
              << " ticks_1000_us=" << tickTotalUs
              << " queued_event_us=" << queuedEventUs << std::endl;
    return true;
  }

  bool uvcFailureRegression(const QString &videoPath, const QString &physicalName = {},
                            const QString &controlDirectory = {}) {
    video_.resize(640, 360);
    video_.setWindowTitle(QStringLiteral("VTR-PON UVC stability evaluation - Program Out"));
    video_.show();
    controller_.refreshWindowHandles();
    std::unique_ptr<UvcDeviceMonitor> monitor;
    struct MonitorShutdown {
      std::unique_ptr<UvcDeviceMonitor> &monitor;
      ~MonitorShutdown() {
        if (!monitor) return;
        monitor->requestShutdown();
        if (!awaitShutdown([&] { return monitor->shutdownComplete(); })) std::abort();
      }
    } monitorShutdown{monitor};
    QString camera = QStringLiteral("fault-test-camera");
    if (!physicalName.isEmpty()) {
      monitor = std::make_unique<UvcDeviceMonitor>();
      CHECK(await([&] { return monitor->isAvailable(); }), "physical device monitor starts");
      camera.clear();
      for (const auto &device : monitor->devices())
        if (device.displayName == physicalName) camera = device.devicePath;
      CHECK(!camera.isEmpty(), "exact physical camera is discovered");
      QObject::connect(monitor.get(), &UvcDeviceMonitor::devicesChanged, &uvcSources_, [&] {
        QSet<QString> paths;
        for (const auto &device : monitor->devices()) paths.insert(device.devicePath);
        uvcSources_.setAvailableDevicePaths(paths);
      });
    }
    QObject::connect(&uvcSources_, &UvcSourceSessionManager::sourceAvailabilityChanged,
                     &model_, &PlaylistModel::setUvcDeviceAvailable);
    QObject::connect(&uvcSources_, &UvcSourceSessionManager::sourceAvailabilityChanged,
                     &panel_, [](const QString &, bool available) {
      std::cout << "UVC availability " << available << std::endl;
    });
    QObject::connect(&uvcSources_, &UvcSourceSessionManager::sourceError,
                     &panel_, [](const QString &, const QString &detail) {
      std::cout << "UVC error: " << detail.toStdString() << std::endl;
    });
    int thumbnails = 0;
    QObject::connect(&uvcSources_, &UvcSourceSessionManager::thumbnailReady,
                     &panel_, [&](const QString &path, const QImage &) { if (path == camera) ++thumbnails; });
    uvcSources_.setAvailableDevicePaths({camera});
    uvcSources_.setRegisteredDevicePaths({camera});
    PlaylistItemRegistration uvc;
    uvc.sourceType = PlaylistSourceType::Uvc;
    uvc.mediaType = PlaylistMediaType::Video;
    uvc.filePath = camera;
    uvc.displayName = physicalName.isEmpty() ? QStringLiteral("Fault test camera") : physicalName;
    auto uvcIds = model_.addItems({uvc});
    uvc.playlistNumber = model_.createPlaylist();
    uvcIds.append(model_.addItems({uvc}));
    CHECK(uvcIds.size() == 2, "same camera is registered in two playlists");
    model_.setUvcDeviceAvailable(camera, !uvcSources_.sourceOffline(camera));
    int initialOfflineEvents = 0;
    const auto initialAvailabilityConnection = QObject::connect(
        &model_, &PlaylistModel::itemMediaAvailabilityChanged, &panel_,
        [&](quint64 id, bool offline) { if (uvcIds.contains(id) && offline) ++initialOfflineEvents; });
    struct ReleaseFakeOpen {
      ~ReleaseFakeOpen() { UvcFaultTest::holdOpen.store(false); UvcFaultTest::dropFrames.store(false); }
    } releaseFakeOpen;
    if (physicalName.isEmpty()) {
      UvcFaultTest::openEntered.store(false);
      UvcFaultTest::holdOpen.store(true);
    }
    QElapsedTimer opening; opening.start();
    uvcSources_.setPreviewDevicePaths({camera});
    CHECK(opening.elapsed() < 100, "opening a UVC device never waits for its driver on the UI thread");
    if (physicalName.isEmpty()) {
      CHECK(await([] { return UvcFaultTest::openEntered.load(); }), "blocked camera opener runs on its worker");
      QElapsedTimer responsive; responsive.start();
      CHECK(await([&] { return responsive.elapsed() >= 200; }), "UI loop continues during a blocked open");
      const auto *list = playlist_.findChild<QListWidget *>(QStringLiteral("playlistList"));
      CHECK(!uvcSources_.sourceAvailable(camera) && !uvcSources_.sourceOffline(camera) &&
            !model_.itemMediaOffline(uvcIds[0]) && !model_.itemMediaOffline(uvcIds[1]) &&
            list && list->count() > 0 && !list->item(0)->data(playlist_view::kItemMediaOfflineRole).toBool(),
            "initial connection wait keeps both camera items and the visible row out of MEDIA OFFLINE");
      UvcFaultTest::holdOpen.store(false);
    }
    CHECK(await([&] { return uvcSources_.sourceAvailable(camera) && thumbnails > 1; }, 12000),
          "camera is online only after live frames arrive");
    CHECK(initialOfflineEvents == 0, "successful initial connection never flashes MEDIA OFFLINE");
    QObject::disconnect(initialAvailabilityConnection);

    auto takeItem = [&](quint64 id) {
      controller_.scheduleCueRequest(controller_.cueRequestFor(*model_.itemState(id)));
      return await([&] { return ready(); }, 12000) &&
          controller_.takeCueOnAir(*model_.itemState(id), false, 1000);
    };
    auto marker = [&](const QString &name) {
      if (controlDirectory.isEmpty()) return;
      QFile file(QDir(controlDirectory).filePath(name));
      if (file.open(QIODevice::WriteOnly)) file.write("ready\n");
    };
    PlaylistItemRegistration file;
    file.filePath = videoPath;
    file.mediaType = PlaylistMediaType::Video;
    file.settings.startMode = PlaylistStartMode::Play;
    file.settings.endMode = PlaylistEndMode::Repeat;
    const auto fileIds = model_.addItems({file});
    CHECK(fileIds.size() == 1, "unrelated file is registered");
    CHECK(await([&] { return controller_.player_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.applied"); }), "PROGRAM audio output is ready");
    for (int scenario = 0; scenario < (physicalName.isEmpty() ? 4 : 3); ++scenario) {
      controller_.clearOnAir();
      if (scenario == 0) CHECK(takeItem(uvcIds[0]), "UVC is ON AIR before failure");
      if (scenario == 1 || scenario == 3) CHECK(takeItem(fileIds[0]), "file is ON AIR before UVC failure");
      if (scenario == 1) {
        controller_.scheduleCueRequest(controller_.cueRequestFor(*model_.itemState(uvcIds[1])));
        CHECK(await([&] { return ready(); }), "failed camera is also prepared on CUE");
      }
      const qint64 position = controller_.player_.positionMs();
      const auto presented = controller_.fullscreenOutputStats().presentedFrames;
      if (physicalName.isEmpty()) {
        if (scenario == 2) uvcSources_.setAvailableDevicePaths({});
        else if (scenario == 3) UvcFaultTest::dropFrames.store(true);
        else {
          UvcSourceSessionManager::ProgramSource source;
          // Retain a pipeline reference without leaving a synthetic deck binding.
          CHECK(uvcSources_.bindProgramDeck(1, camera, source), "fault injection obtains the live UVC pipeline");
          auto *pipeline = GST_ELEMENT(gst_object_ref(source.pipeline));
          if (scenario == 0) uvcSources_.releaseProgramDeck(1);
          if (scenario == 0) {
            GError *error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ, "Injected UVC read failure");
            gst_element_post_message(pipeline, gst_message_new_error(GST_OBJECT(pipeline), error, "regression"));
            g_error_free(error);
          } else gst_element_post_message(pipeline, gst_message_new_eos(GST_OBJECT(pipeline)));
          gst_object_unref(pipeline);
        }
      } else marker(QStringLiteral("disable-%1").arg(scenario));
      CHECK(await([&] {
        controller_.updatePlaybackUi();
        return model_.itemMediaOffline(uvcIds[0]) && model_.itemMediaOffline(uvcIds[1]);
      }, physicalName.isEmpty() ? 8000 : 60000), "both camera items become MEDIA OFFLINE");
      CHECK(model_.onAirItemId() == ((scenario == 1 || scenario == 3) ? fileIds[0] : 0),
            "only the failed ON AIR UVC is taken OFF AIR");
      CHECK(model_.cueItemId() != uvcIds[1], "failed UVC CUE is released");
      const auto *list = playlist_.findChild<QListWidget *>(QStringLiteral("playlistList"));
      CHECK(list && list->count() > 0 && list->item(0)->data(playlist_view::kItemMediaOfflineRole).toBool(),
            "visible playlist row displays MEDIA OFFLINE");
      CHECK(!controller_.takeCueOnAir(*model_.itemState(uvcIds[0]), false, 1000),
            "offline UVC cannot be taken through the direct TAKE path");
      if (scenario == 2) uvcSources_.setPreviewDevicePaths({});
      model_.refreshMediaAvailability();
      QElapsedTimer hold; hold.start();
      CHECK(await([&] { controller_.updatePlaybackUi(); return hold.elapsed() >= 600; }), "event loop keeps responding");
      CHECK(model_.itemMediaOffline(uvcIds[0]) && model_.itemMediaOffline(uvcIds[1]),
            "file availability scanning preserves UVC offline state");
      if (scenario == 1 || scenario == 3)
        CHECK(controller_.player_.positionMs() > position && controller_.player_.isPlaying() &&
              controller_.fullscreenOutputStats().presentedFrames > presented,
              "unrelated Program file keeps advancing during camera failure");
      const int previousThumbnails = thumbnails;
      if (physicalName.isEmpty()) {
        UvcFaultTest::dropFrames.store(false);
        uvcSources_.setAvailableDevicePaths({camera});
      } else marker(QStringLiteral("enable-%1").arg(scenario));
      const bool recovered = await([&] { controller_.updatePlaybackUi(); return !model_.itemMediaOffline(uvcIds[0]) &&
          !model_.itemMediaOffline(uvcIds[1]) && thumbnails > previousThumbnails; }, 20000);
      if (!recovered) std::cerr << "Recovery state: retiring=" << uvcSources_.sourceIsRetiring(camera)
          << " playing=" << uvcSources_.sourceIsPlaying(camera) << " healthy=" << uvcSources_.sourceAvailable(camera)
          << " thumbnails=" << thumbnails << '/' << previousThumbnails << '\n';
      CHECK(recovered,
            "camera and both items automatically recover with fresh frames");
      CHECK(!list->item(0)->data(playlist_view::kItemMediaOfflineRole).toBool(), "playlist offline display clears on recovery");
      CHECK(model_.onAirItemId() == ((scenario == 1 || scenario == 3) ? fileIds[0] : 0),
            "recovery never automatically takes UVC ON AIR");
      std::cout << "UVC scenario " << scenario << " PASS" << std::endl;
      uvcSources_.setPreviewDevicePaths({camera});
    }
    CHECK(takeItem(uvcIds[0]), "recovered camera can be explicitly taken ON AIR again");
    controller_.clearOnAir();
    marker(QStringLiteral("completed"));
    return true;
  }

  bool nativeSelectedTake(const QString &path, bool recoverDisconnected = false) {
    CHECK(await([&] { return controller_.player_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.applied"); }), "PROGRAM output is ready");
    PlaylistItemRegistration item;
    item.filePath = path;
    item.sourceType = PlaylistSourceType::PowerPoint;
    item.mediaType = PlaylistMediaType::Presentation;
    item.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
    item.settings.startMode = PlaylistStartMode::Play;
    item.settings.endMode = PlaylistEndMode::Pause;
    const auto ids = model_.addItems({item});
    CHECK(ids.size() == 1, "native item registered");
    (void)operations_.execute(ControllerOperations::Action::SelectItem, ids[0]);
    CHECK(playlist_.selectedItemId() == ids[0], "native item selected through normal operation");
    QElapsedTimer preparation;
    preparation.start();
    CHECK(await([&] { return controller_.player_.cueStatus() ==
        DualDeckPlayer::CueStatus::Ready; }), "selected native CUE becomes Ready");
    const qint64 readyMs = preparation.elapsed();
    QElapsedTimer settled;
    settled.start();
    CHECK(await([&] { return settled.elapsed() >= 3000; }), "selection waits three seconds after Ready");
    CHECK(model_.onAirItemId() == 0, "selection alone does not take ON AIR");
    if (recoverDisconnected) {
      auto *status = playlist_.findChild<QLabel *>("playlistPowerPointCueStatus");
      CHECK(status && !status->isHidden(), "selected PPT preparation status is visible before TAKE");
      for (bool selectAgain : {true, false}) {
        // The command-line entry point requires Office to be closed before
        // constructing this fixture. This process belongs only to the test.
        const auto processIds = powerPointProcessIds();
        CHECK(processIds.size() == 1, "fixture owns one prepared Office process");
        const HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                           *processIds.begin());
        CHECK(process != nullptr, "test Office process handle is available");
        const bool stopped = TerminateProcess(process, 1) &&
            WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
        CloseHandle(process);
        CHECK(stopped, "test Office exits while selected but before ON AIR");
        CHECK(await([&] { return model_.cueState() == PlaylistCueState::Error; }),
              "Office failure is reported on the selected CUE without pressing ON AIR");
        CHECK(status->text() == vtrpon::uiText("playlist.powerpoint.failed"),
              "selected PPT status reports the preparation failure");
        CHECK(model_.onAirItemId() == 0, "failure detection never takes the PPT ON AIR");
        if (selectAgain) {
          (void)operations_.execute(ControllerOperations::Action::SelectItem, ids[0]);
          CHECK(controller_.player_.cueStatus() == DualDeckPlayer::CueStatus::Loading,
                "reselecting the same failed PPT starts preparation immediately");
          CHECK(await([&] { return model_.cueState() == PlaylistCueState::Ready; }, 15000),
                "selection reconnects Office before ON AIR is requested");
          CHECK(!controller_.pendingTakeItemId_ && model_.onAirItemId() == 0,
                "selection recovery preserves OFF AIR");
        } else {
          controller_.takeSelectedPlaylistItemOnAir();
          CHECK(controller_.pendingTakeItemId_ == ids[0] && model_.onAirItemId() == 0,
                "ON AIR retries a failed selected PPT and retains the pending TAKE");
          CHECK(await([&] { return model_.onAirItemId() == ids[0]; }, 15000),
                "queued ON AIR automatically sends the PPT when reconnection is ready");
          CHECK(!controller_.pendingTakeItemId_, "completed queued TAKE clears its request");
          std::cout << "Native selected failure detection, reselect and queued recovery: PASS\n";
          return true;
        }
      }
    }
    QElapsedTimer take;
    take.start();
    controller_.takeSelectedPlaylistItemOnAir();
    const qint64 takeMs = take.elapsed();
    CHECK(model_.onAirItemId() == ids[0] && controller_.player_.powerPointSourceIsOnAir(),
          "prepared selection is taken immediately without reopening PowerPoint");
    CHECK(!controller_.pendingTakeItemId_, "prepared native TAKE has no deferred load");
    CHECK(takeMs < 250, "prepared native TAKE returns within 250 ms");
    std::cout << "Native selected TAKE: readyMs=" << readyMs << " takeMs=" << takeMs << '\n';
    return true;
  }

  bool nativeTileTake(const QString &path, int initialShowCommand) {
    CHECK(await([&] { return controller_.player_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.applied"); }), "PROGRAM output is ready");
    operations_.bindMember(ControllerOperations::Action::TakeSelectedOnAir,
                          &controller_, &ProgramPlayoutController::takeSelectedPlaylistItemOnAir);
    operations_.connectInput(&playlist_, &PlaylistPanel::onAirRequested,
                             ControllerOperations::Action::TakeSelectedOnAir);
    PlaylistItemRegistration item;
    item.filePath = path;
    item.sourceType = PlaylistSourceType::PowerPoint;
    item.mediaType = PlaylistMediaType::Presentation;
    item.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
    item.settings.endMode = PlaylistEndMode::Pause;
    const auto ids = model_.addItems({item});
    CHECK(ids.size() == 1, "native tile registered");
    if (initialShowCommand != SW_SHOWNORMAL) {
      // The command-line entry point requires Office to be closed first.
      // Change only the automation window created by this test's warmup.
      struct WarmWindow { std::set<DWORD> processes; HWND window = nullptr; } warm;
      CHECK(await([&] {
        warm.processes = powerPointProcessIds();
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
          auto &warm = *reinterpret_cast<WarmWindow *>(parameter);
          DWORD processId = 0;
          GetWindowThreadProcessId(window, &processId);
          wchar_t className[128]{};
          GetClassNameW(window, className, 128);
          if (warm.processes.contains(processId) && wcscmp(className, L"PPTFrameClass") == 0) {
            warm.window = window;
            return FALSE;
          }
          return TRUE;
        }, reinterpret_cast<LPARAM>(&warm));
        return warm.window != nullptr;
      }, 15000), "test's prestarted Office window is available");
      ShowWindow(warm.window, initialShowCommand);
      CHECK(initialShowCommand == SW_SHOWMAXIMIZED ? IsZoomed(warm.window) : IsIconic(warm.window),
            "Office starts in the requested window state");
      ShowWindow(warm.window, SW_HIDE);
    }
    auto *list = playlist_.findChild<QListWidget *>("playlistList");
    auto *tiles = playlist_.findChild<QToolButton *>("playlistTileViewButton");
    CHECK(list && tiles, "tile controls exist");
    playlist_.resize(720, 700);
    playlist_.show();
    tiles->click();
    QCoreApplication::processEvents();
    const QPointF point(list->visualItemRect(list->item(0)).center());
    CHECK(list->itemAt(point.toPoint()) == list->item(0), "native tile is clickable");
    QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &release);
    CHECK(playlist_.selectedItemId() == ids[0], "tile click selects native PPTX");
    CHECK(controller_.pendingTakeItemId_ == ids[0] && model_.onAirItemId() == 0,
          "tile click queues ON AIR while native CUE is preparing");
    CHECK(await([&] {
      return model_.onAirItemId() == ids[0] || model_.cueState() == PlaylistCueState::Error;
    }, 20000), "native tile preparation finishes");
    if (model_.cueState() == PlaylistCueState::Error)
      std::cerr << controller_.player_.cueErrorDetail().toStdString() << '\n';
    CHECK(model_.onAirItemId() == ids[0] && controller_.player_.powerPointSourceIsOnAir(),
          "native tile goes ON AIR without another click");
    CHECK(!controller_.pendingTakeItemId_, "completed tile TAKE clears its request");
    std::cout << "Native tile TAKE: PASS, initial show command=" << initialShowCommand << '\n';
    return true;
  }

  bool nativePowerPointRegistration(const QString &path, bool restore) {
    CHECK(controller_.isAvailable(), "PROGRAM is available for registration");
    const int otherPlaylist = model_.createPlaylist();
    CHECK(otherPlaylist != model_.activePlaylistNumber(), "native item uses an unselected playlist");
    if (restore) {
      auto snapshot = model_.snapshot();
      PlaylistItemState item;
      item.id = 1;
      item.itemNumber = 1;
      item.playlistNumber = otherPlaylist;
      item.filePath = path;
      item.sourceType = PlaylistSourceType::PowerPoint;
      item.mediaType = PlaylistMediaType::Presentation;
      item.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
      snapshot.items.append(item);
      CHECK(model_.restoreSnapshot(snapshot), "native playlist restored");
    } else {
      const auto id = model_.addPendingMediaItem(path, otherPlaylist, PlaylistSourceType::PowerPoint);
      CHECK(id != 0 && model_.completePendingNativeDocument(id), "native registration completed");
    }
    QElapsedTimer timer;
    timer.start();
    while (powerPointProcessIds().empty() && timer.elapsed() < 15000) {
      QCoreApplication::processEvents();
      QThread::msleep(25);
    }
    CHECK(!powerPointProcessIds().empty(), "registration starts Office without selecting the item");
    CHECK(model_.cueItemId() == 0 && model_.onAirItemId() == 0 &&
              !isPowerPointNativeSourceActive(), "registration does not start native playback");
    std::cout << "Native PowerPoint " << (restore ? "restore" : "registration")
              << " before CUE: PASS\n";
    return true;
  }

  bool operatorTimerIndependence(const QString &path) {
    CHECK(await([&] { return controller_.player_.programOutputDeviceStatusKey() ==
        QStringLiteral("settings.output.applied"); }), "timer fixture waits for PROGRAM startup");
    auto saved = vtrpon::makeControllerSettings();
    const auto previous = vtrpon::loadOperatorMonitorSettings(saved);
    struct Restore {
      QSettings &settings;
      vtrpon::OperatorMonitorSettings previous;
      ~Restore() { vtrpon::saveOperatorMonitorSettings(settings, previous); }
    } restore{saved, previous};
    auto config = previous;
    config.timerEnabled = true;
    config.timerDurationSeconds = 300;
    CHECK(vtrpon::saveOperatorMonitorSettings(saved, config), "independent timer enabled");
    QMenuBar menu;
    vtrpon::ViewMenuController view(&menu, &video_, &panel_);
    QObject::connect(&view, &vtrpon::ViewMenuController::operatorTimerChanged,
                     &panel_, &ProgramPanel::setOperatorTimerPresentation);
    QObject::connect(&panel_, &ProgramPanel::operatorTimerStartRequested,
                     &view, &vtrpon::ViewMenuController::startOperatorTimer);
    QObject::connect(&panel_, &ProgramPanel::operatorTimerPauseRequested,
                     &view, &vtrpon::ViewMenuController::pauseOperatorTimer);
    QObject::connect(&panel_, &ProgramPanel::operatorTimerResetRequested,
                     &view, &vtrpon::ViewMenuController::resetOperatorTimer);
    panel_.setOperatorTimerPresentation(view.operatorTimerPresentation());
    auto *startButton = panel_.findChild<QPushButton *>("operatorTimerStart");
    auto *pauseButton = panel_.findChild<QPushButton *>("operatorTimerPause");
    auto *resetButton = panel_.findChild<QPushButton *>("operatorTimerReset");
    const auto operable = [&] {
      return startButton->isEnabled() && pauseButton->isEnabled() && resetButton->isEnabled();
    };
    CHECK(model_.onAirItemId() == 0 && operable(), "timer is operable at startup while OFF AIR");
    startButton->click();
    CHECK(startButton->property("active").toBool() && !pauseButton->property("active").toBool(),
          "timer Start indicates the running model state");
    CHECK(await([&] { return view.operatorTimerPresentation().value("seconds").toInt() <= 299; }),
          "timer counts down before any ON AIR");
    const auto uninterrupted = [&] {
      const auto state = view.operatorTimerPresentation();
      return operable() && state.value("running").toBool() && state.value("seconds").toInt() <= 299;
    };
    CHECK(start(path, PlaylistEndMode::Next), "media goes ON AIR while timer runs");
    CHECK(uninterrupted(), "ON AIR does not reset or stop the timer");
    controller_.playOnAir();
    controller_.pauseOnAir();
    CHECK(uninterrupted(), "media play and pause leave timer independent");
    CHECK(waitReady(), "next item prepared while timer runs");
    controller_.handleOnAirCompletion(6000);
    CHECK(await([&] { take(); return activeId() == ids_[1]; }), "next item goes ON AIR");
    CHECK(uninterrupted(), "item transition does not reset or stop the timer");
    controller_.clearOnAir();
    CHECK(await([&] { return model_.onAirItemId() == 0; }), "media returns OFF AIR");
    CHECK(uninterrupted(), "OFF AIR does not reset or stop the timer");
    pauseButton->click();
    const auto paused = view.operatorTimerPresentation();
    CHECK(!paused.value("running").toBool(), "timer pause works while OFF AIR");
    CHECK(!startButton->property("active").toBool() && pauseButton->property("active").toBool(),
          "timer Pause indicates the paused model state");
    pauseButton->click();
    CHECK(view.operatorTimerPresentation() == paused, "repeated pause is harmless");
    startButton->click();
    CHECK(view.operatorTimerPresentation().value("running").toBool(), "timer resumes while OFF AIR");
    resetButton->click();
    CHECK(!startButton->property("active").toBool() && !pauseButton->property("active").toBool() &&
          !resetButton->property("active").toBool(), "timer reset clears all button activity");
    CHECK(view.operatorTimerPresentation().value("seconds").toInt() == 300 &&
          !view.operatorTimerPresentation().value("running").toBool() && operable(),
          "timer reset works while OFF AIR");
    std::cout << "PASS: timer independent of ON AIR, OFF AIR and item transitions\n";
    return true;
  }

  bool switcherRegression() {
    // The fixture must not be destroyed while its output-start worker still
    // owns the pipeline; the next fixture uses the same named audio inputs.
    CHECK(await([&] {
      return controller_.player_.programOutputDeviceStatusKey() ==
             QStringLiteral("settings.output.applied");
    }), "switcher fixture waits for PROGRAM startup");
    return runSwitcherControlRegression(&controller_);
  }

  bool endOffRegression(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Stop), "END OFF item starts");
    CHECK(model_.onAirItemId() == ids_[0], "item is ON AIR before completion");
    int offAirSignals = 0;
    const auto connection = QObject::connect(
        &controller_, &ProgramPlayoutController::onAirChanged,
        &panel_, [&](bool onAir) { if (!onAir) ++offAirSignals; });
    controller_.handleOnAirCompletion(6000);
    QObject::disconnect(connection);
    CHECK(model_.onAirItemId() == 0, "END OFF clears playlist ON AIR state");
    CHECK(!controller_.onAirStateMachine_.preparedMedia(),
          "END OFF releases prepared ON AIR media");
    CHECK(!controller_.programIsPlaying(), "END OFF stops playback");
    CHECK(offAirSignals == 1, "END OFF notifies OFF AIR once");
    return true;
  }

  bool bgmOutputWarnings(const QString &path) {
    info_.setDisplayDuration(0);
    auto &bgm = *controller_.bgmController_;
    CHECK(model_.setPlaylistMode(1, PlaylistMode::Bgm), "warning fixture uses BGM mode");
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = PlaylistMediaType::Audio;
    item.settings.startMode = PlaylistStartMode::Play;
    const auto ids = model_.addItems({item, item});
    CHECK(ids.size() == 2, "two valid BGM items registered");
    bgm.setControlsVisibleRequested(true);
    bgm.play();
    CHECK(await([&] {
      return controller_.player_.bgmStatus() == DualDeckPlayer::BgmStatus::Ready &&
          controller_.programOutputDeviceStatusKey() == QStringLiteral("settings.output.failed");
    }), "valid BGM becomes Ready while the configured output is unavailable");
    bgm.update();
    CHECK(info_.text() == vtrpon::uiText("program.bgm.audio_output_unavailable") &&
          info_.hasAudioSettingsAction(), "audio failure has the correct warning and settings action");
    CHECK(model_.bgmItemId() == ids[0] && bgm.failedItemIds_.isEmpty() &&
          bgm.pendingStart_ == BgmPlayoutController::PendingStart::None && bgm.firstPlayPending_,
          "output failure retains the first item without skipping or automatic retry");
    bgm.update();
    bgm.play();
    CHECK(model_.bgmItemId() == ids[0] && bgm.failedItemIds_.isEmpty(),
          "repeated PLAY without an output never exhausts the valid playlist");
    controller_.requestProgramOutputDevice({});
    controller_.player_.processBusMessages();
    CHECK(controller_.programOutputDeviceStatusKey() == QStringLiteral("settings.output.applying"),
          "the output change is pending before its result is polled");
    bgm.play();
    CHECK(info_.text() == vtrpon::uiText("program.bgm.audio_output_changing") &&
          info_.hasAudioSettingsAction() && model_.bgmItemId() == ids[0],
          "PLAY during application reports output preparation and retains the item");
    CHECK(await([&] {
      bgm.update();
      return controller_.programOutputDeviceStatusKey() == QStringLiteral("settings.output.applied");
    }), "PROGRAM audio output recovers");
    CHECK(controller_.player_.bgmStatus() == DualDeckPlayer::BgmStatus::Ready,
          "recovering output does not start BGM unexpectedly");
    bgm.play();
    CHECK(controller_.player_.bgmStatus() == DualDeckPlayer::BgmStatus::Playing &&
          model_.bgmItemId() == ids[0] && bgm.failedItemIds_.isEmpty(),
          "explicit PLAY retries the same valid item successfully");
    bgm.cancelTransportFade();
    bgm.setTransportGain(0.0);
    bgm.stop();
    auto invalid = model_.itemState(ids[0])->settings;
    invalid.inPointMs = 20000;
    CHECK(model_.updateItemSettings(ids[0], invalid), "fixture supplies an invalid media range");
    bgm.play();
    CHECK(await([&] {
      bgm.update();
      return model_.bgmItemId() == ids[1] &&
          controller_.player_.bgmStatus() == DualDeckPlayer::BgmStatus::Playing;
    }), "an actual media failure still advances to the valid next item");
    CHECK(bgm.failedItemIds_.contains(ids[0]) && !bgm.failedItemIds_.contains(ids[1]),
          "only the invalid media is marked failed");
    std::cout << "BGM output warnings, retained retry and real media failure: PASS\n";
    return true;
  }

  bool bgmRegression(const QString &path) {
    CHECK(await([&] {
      return controller_.player_.programOutputDeviceStatusKey() ==
             QStringLiteral("settings.output.applied");
    }), "PROGRAM output is ready before BGM PLAY");
    auto &bgm = *controller_.bgmController_;
    CHECK(model_.setPlaylistMode(1, PlaylistMode::Bgm), "BGM mode selected");
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = PlaylistMediaType::Audio;
    item.settings.startMode = PlaylistStartMode::Pause;
    item.settings.fadeInEnabled = true;
    item.settings.fadeInDurationMs = 2000;
    const auto ids = model_.addItems({item});
    CHECK(ids.size() == 1, "BGM item registered");
    const int otherPlaylist = model_.createPlaylist();
    model_.setActivePlaylistNumber(otherPlaylist);
    CHECK(bgm.startingItemId() == ids.front(), "BGM found from a standard playlist");
    bgm.setControlsVisibleRequested(true);
    auto *toggle = panel_.findChild<QPushButton *>("programBgmFaderButton");
    auto *bgmFader = panel_.findChild<QSlider *>("programBgmFader");
    auto *itemFader = panel_.findChild<QSlider *>("programItemFader");
    CHECK(toggle && bgmFader && itemFader, "BGM fader controls exist");
    const double itemGain = panel_.itemGain();
    toggle->click();
    CHECK(itemFader->isHidden() && !bgmFader->isHidden(), "BGM replaces ITEM fader");
    auto *fadeIn = panel_.findChild<QPushButton *>("programBgmFadeInButton");
    auto *fadeOut = panel_.findChild<QPushButton *>("programBgmFadeOutButton");
    CHECK(fadeIn && fadeOut, "BGM fade controls exist");
    int configuredFadeNotifications = 0;
    const auto fadeObservation = QObject::connect(&operations_, &ControllerOperations::operationAboutToExecute,
        &panel_, [&](ControllerOperations::Action action, const QVariantList &, const QString &source) {
      if (action == ControllerOperations::Action::BgmFadeInConfigured ||
          action == ControllerOperations::Action::BgmFadeOutConfigured) {
        if (source == QStringLiteral("Shortcut")) ++configuredFadeNotifications;
      }
    });
    const auto fadeObservationCleanup = qScopeGuard([&] { QObject::disconnect(fadeObservation); });
    CHECK(operations_.executeFrom("Shortcut", ControllerOperations::Action::BgmFadeInConfigured).status ==
          ControllerOperations::Status::Rejected && configuredFadeNotifications == 0,
          "configured BGM fade without active playback rejects before intervention");
    CHECK(toggle->parentWidget() == panel_.findChild<QWidget *>("programBgmControlModule"),
          "fader visibility button belongs to BGM transport module");
    CHECK(!panel_.findChild<QWidget *>("programBgmControlModule")->isAncestorOf(fadeIn),
          "BGM fades moved out of transport module");
    CHECK(operations_.execute(ControllerOperations::Action::SetBgmVolume, -120).invoked(),
          "BGM volume operation accepted");
    CHECK(panel_.bgmGain() < 0.3 && panel_.itemGain() == itemGain,
          "BGM fader does not change ITEM gain");
    toggle->click();
    CHECK(!itemFader->isHidden() && bgmFader->isHidden(), "ITEM fader restored");
    toggle->click();
    CHECK(bgmFader->value() == -120, "BGM gain survives switching");
    if (QCoreApplication::arguments().contains(QStringLiteral("--bgm-only"))) {
      panel_.resize(760, 760);
      panel_.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/bgm-fader-test.png"));
    }
    panel_.setBgmControlsVisible(false);
    CHECK(!toggle->isChecked() && !itemFader->isHidden(), "BGM OFF restores ITEM");
    bgm.setControlsVisibleRequested(true);
    bgmFader->setValue(0);
    bgm.play();
    CHECK(await([&] {
      bgm.update();
      return bgm.pendingStart_ == BgmPlayoutController::PendingStart::None;
    }), "START PAUSE finishes preparation");
    CHECK(bgm.isActive() && bgm.transportGain_ == 0.0,
          "START PAUSE retains item with silent transport");
    bgm.play();
    CHECK(bgm.itemFadeAnimation_.state() == QAbstractAnimation::Running &&
          bgm.itemFadeGain_ == 0.0, "first PLAY starts the item Fade In at zero");
    bgm.itemFadeAnimation_.setCurrentTime(1000);
    CHECK(bgm.itemFadeGain_ > 0.4 && bgm.itemFadeGain_ < 0.6,
          "item Fade In uses the configured two-second duration");
    bgm.play();
    CHECK(bgm.itemFadeGain_ > 0.4, "repeated PLAY does not restart item Fade In");
    bgm.pause();
    CHECK(await([&] { return controller_.player_.bgmStatus() == DualDeckPlayer::BgmStatus::Paused; }),
          "ordinary PAUSE completes");
    bgm.play();
    CHECK(bgm.itemFadeGain_ > 0.4, "ordinary resume does not restart initial Fade In");

    bgm.setTransportGain(1.0);
    bgm.cancelTransportFade();
    CHECK(operations_.executeFrom("Shortcut", ControllerOperations::Action::BgmFadeOutConfigured).status ==
          ControllerOperations::Status::Rejected && configuredFadeNotifications == 0,
          "configured BGM fade respects the selected ITEM surface");
    toggle->click();
    auto *fadeDuration = fadeOut->parentWidget()->findChild<QDoubleSpinBox *>();
    CHECK(fadeDuration, "configured BGM duration control exists");
    fadeDuration->setValue(2.0);
    CHECK(operations_.executeFrom("Shortcut", ControllerOperations::Action::BgmFadeOutConfigured).invoked() &&
          configuredFadeNotifications == 1 && bgm.manualFadeAnimation_.duration() == 2000,
          "configured BGM fade uses the stored duration with one operation notification");
    bgm.manualFadeAnimation_.setCurrentTime(1000);
    CHECK(panel_.bgmGain() > 0.48 && panel_.bgmGain() < 0.52,
          "F/O moves the BGM fader halfway without squaring its gain");
    bgm.manualFadeAnimation_.setCurrentTime(2000);
    CHECK(panel_.bgmGain() == 0.0, "F/O reaches fader silence");
    bgm.fadeIn(2000);
    bgm.manualFadeAnimation_.setCurrentTime(1000);
    CHECK(panel_.bgmGain() > 0.48 && panel_.bgmGain() < 0.52,
          "F/I moves the BGM fader toward unity");
    operations_.execute(ControllerOperations::Action::SetBgmVolume, -120);
    CHECK(bgm.manualFadeAnimation_.state() == QAbstractAnimation::Stopped &&
          bgm.manualFadeGain_ > 0.24 && bgm.manualFadeGain_ < 0.26,
          "manual BGM fader operation takes over an active fade");
    bgm.fadeIn(2000);
    bgm.manualFadeAnimation_.setCurrentTime(2000);
    CHECK(panel_.bgmGain() == 1.0 && panel_.itemGain() == itemGain,
          "F/I reaches unity without changing ITEM");

    // Substitute an unavailable player only at the transport-call boundary.
    // This exercises false returns without disturbing a physical audio device.
    DualDeckPlayer unavailable(nullptr, nullptr);
    auto *realPlayer = bgm.player_;
    bgm.player_ = &unavailable;
    bgm.stop();
    const bool held = bgm.stopFailed_ && model_.bgmItemId() == ids.front() &&
        bgm.activePlaylistNumber() == 1 && bgm.stateMachine_.preparedMedia();
    bgm.update();
    bgm.play();
    bgm.fadeIn(100);
    const bool silent = bgm.transportGain_ == 0.0;
    bgm.stop();
    const bool retryHeld = bgm.stopFailed_ && bgm.isActive();
    bgm.player_ = realPlayer;
    unavailable.requestShutdown();
    CHECK(awaitShutdown([&] { return unavailable.shutdownComplete(); }),
          "unavailable player also completes terminal teardown");
    CHECK(held && retryHeld && silent, "failed STOP/reset retains item and blocks unmuting");
    CHECK(info_.source() == QStringLiteral("BGM"), "STOP failure notifies the user");
    bgm.stop();
    CHECK(!bgm.stopFailed_ && !bgm.isActive() && bgm.activePlaylistNumber() == 0 &&
          controller_.player_.bgmStatus() == DualDeckPlayer::BgmStatus::Empty,
          "STOP retry clears the pipeline before releasing the model");
    return true;
  }

  explicit ProgramPlayoutTransitionTest(bool dsk)
      : panel_(nullptr, &operations_),
        playlist_(&model_, nullptr, false, &info_, false, &operations_),
        controller_(&video_, &panel_, &playlist_, &model_, &info_, false,
                    &uvcSources_, nullptr),
        dsk_(dsk), target_(dsk ? Target::Dsk : Target::Program) {
    controller_.busTimer_.stop();
    using A = ControllerOperations::Action;
    operations_.bindMember(A::DskPlay, &controller_, &ProgramPlayoutController::playDsk);
    operations_.connectInput(&panel_, &ProgramPanel::dskPlayRequested, A::DskPlay);
    operations_.bindMember(A::DskPause, &controller_, &ProgramPlayoutController::pauseDsk);
    operations_.connectInput(&panel_, &ProgramPanel::dskPauseRequested, A::DskPause);
  }

  bool dskManualTransport(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause), "Start Pause DSK activates");
    auto *play = panel_.findChild<QPushButton *>(QStringLiteral("dskPlayButton"));
    auto *pause = panel_.findChild<QPushButton *>(QStringLiteral("dskPauseButton"));
    CHECK(play && pause && !play->isHidden() && !pause->isHidden(),
          "DSK ON exposes both transport buttons");
    CHECK(!controller_.player_.dskIsPlaying() && pause->property("active").toBool(),
          "Start Pause is reflected by the buttons");
    play->click();
    CHECK(await([&] { return controller_.player_.dskPositionMs() > 150; }),
          "Play button advances the real DSK video");
    pause->click();
    const qint64 held = controller_.player_.dskPositionMs();
    QThread::msleep(150);
    controller_.updatePlaybackUi();
    CHECK(!controller_.player_.dskIsPlaying() &&
          qAbs(controller_.player_.dskPositionMs() - held) < 80 &&
          pause->property("active").toBool() && !play->property("active").toBool(),
          "Pause holds the video and the state-driven button appearance");
    play->click();
    CHECK(await([&] { return controller_.player_.dskPositionMs() > held + 100; }),
          "Play resumes from the held position");
    CHECK(model_.onAirItemId() == 0 && !controller_.programIsPlaying(),
          "DSK transport does not activate PROGRAM");
    controller_.toggleDsk();
    CHECK(play->isHidden() && pause->isHidden() && model_.dskItemId() == 0,
          "DSK OFF hides transport and releases the playlist item");
    std::cout << "DSK button transport: PASS\n";
    return true;
  }

  bool dskFadeTransport(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause), "fade DSK starts");
    auto settings = model_.itemState(ids_[0])->settings;
    settings.fadeInEnabled = true;
    settings.fadeOutEnabled = true;
    settings.fadeInDurationMs = 1000;
    settings.fadeOutDurationMs = 1000;
    CHECK(model_.updateItemSettings(ids_[0], settings), "DSK fades configured");
    controller_.playDsk();
    controller_.startDskFadeIn(settings);
    controller_.dskFadeInAnimation_.setCurrentTime(200);
    controller_.pauseDsk();
    const qreal heldIn = controller_.dskFadeInProgress_;
    CHECK(controller_.dskFadeInAnimation_.state() == QAbstractAnimation::Paused,
          "manual Pause freezes FI");
    QThread::msleep(60);
    controller_.updatePlaybackUi();
    QCoreApplication::processEvents();
    CHECK(controller_.dskFadeInProgress_ == heldIn, "paused FI opacity is held");
    controller_.playDsk();
    CHECK(controller_.dskFadeInAnimation_.state() == QAbstractAnimation::Running,
          "Play resumes FI");
    controller_.dskFadeInAnimation_.setCurrentTime(1000);
    controller_.maybeStartDskFadeOut(5000, 6000);
    controller_.dskFadeOutAnimation_.setCurrentTime(250);
    controller_.pauseDsk();
    const qreal heldOut = controller_.dskFadeOutProgress_;
    QThread::msleep(60);
    controller_.updatePlaybackUi();
    QCoreApplication::processEvents();
    CHECK(controller_.dskFadeOutAnimation_.state() == QAbstractAnimation::Paused &&
          controller_.dskFadeOutProgress_ == heldOut,
          "Pause freezes FO instead of fading the held frame to transparent");
    controller_.playDsk();
    CHECK(controller_.dskFadeOutAnimation_.state() == QAbstractAnimation::Running,
          "Play resumes FO from its held opacity");
    controller_.pauseDsk();
    controller_.resetDskFadeState();
    controller_.applyDskFadeProgress();
    CHECK(stateMachine().markEnded(), "ended frame can be turned OFF");
    controller_.toggleDsk();
    auto *play = panel_.findChild<QPushButton *>(QStringLiteral("dskPlayButton"));
    auto *pause = panel_.findChild<QPushButton *>(QStringLiteral("dskPauseButton"));
    CHECK(controller_.pendingDskOff_ && !play->isEnabled() && !pause->isEnabled(),
          "OFF fade disables both transport controls");
    controller_.playDsk();
    controller_.pauseDsk();
    CHECK(controller_.pendingDskOff_ &&
          controller_.dskFadeOutAnimation_.state() == QAbstractAnimation::Running,
          "transport commands cannot interrupt or strand an OFF fade");
    controller_.dskFadeOutAnimation_.setCurrentTime(1000);
    CHECK(!controller_.pendingDskOff_ && model_.dskItemId() == 0 && play->isHidden(),
          "OFF completes even after Pause and transport attempts");
    std::cout << "DSK fade/Off transport: PASS\n";
    return true;
  }

  bool dskWaitingTransport(const QString &path, PlaylistEndMode mode,
                           bool pauseWhileWaiting = true) {
    CHECK(start(path, mode), "late destination fixture starts");
    auto outgoing = model_.itemState(ids_[0])->settings;
    outgoing.endCrossfadeEnabled = true;
    outgoing.endCrossfadeDurationMs = 1000;
    CHECK(model_.updateItemSettings(ids_[0], outgoing), "DSK crossfade enabled");
    auto settings = model_.itemState(ids_[1])->settings;
    settings.startMode = pauseWhileWaiting ? PlaylistStartMode::Play : PlaylistStartMode::Pause;
    CHECK(model_.updateItemSettings(ids_[1], settings), "destination Start Mode configured");
    // Withhold preparation deterministically, like the PROGRAM repeat test.
    const auto destination = *model_.itemState(ids_[1]);
    transition().itemId = destination.id;
    transition().mode = mode;
    transition().request = controller_.dskRequestFor(destination);
    transition().preparationAttempted = true;
    controller_.handleDskCompletion();
    CHECK(transition().due && activeId() == ids_[0], "late NEXT/GOTO waits at end");
    if (pauseWhileWaiting) controller_.pauseDsk();
    CHECK(controller_.cuePlaylistItemToDsk(destination, false), "late cue begins");
    CHECK(await([&] { return ready(); }), "late cue becomes Ready");
    if (!pauseWhileWaiting) {
      controller_.playDsk();
      CHECK(activeId() == ids_[1] && !controller_.player_.dskIsPlaying(),
            "Play while waiting does not override the destination's Start Pause");
      return true;
    }
    controller_.updatePlaybackUi();
    CHECK(activeId() == ids_[0] && !controller_.takePreparedNextDskItem(),
          "Pause prevents all automatic TAKE paths after late Ready");
    controller_.playDsk();
    CHECK(activeId() == ids_[1] && controller_.player_.dskIsPlaying(),
          "Play resumes the pending NEXT/GOTO and honors destination Start Play");
    CHECK(controller_.player_.dskCrossfadeActive(), "DSK transition crossfades");
    controller_.pauseDsk();
    CHECK(controller_.dskCrossfadeAnimation_.state() == QAbstractAnimation::Paused,
          "manual Pause also freezes transition opacity");
    controller_.playDsk();
    CHECK(controller_.dskCrossfadeAnimation_.state() == QAbstractAnimation::Running,
          "Play resumes transition opacity");
    std::cout << "DSK late NEXT/GOTO transport: PASS\n";
    return true;
  }

  bool dskEndedTransport(const QString &path, PlaylistEndMode mode) {
    CHECK(start(path, mode, PlaylistMediaType::Video, 1000, 1600),
          "short DSK range prepared");
    if (mode == PlaylistEndMode::Repeat) {
      auto settings = model_.itemState(ids_[0])->settings;
      settings.repeatCount = 2;
      CHECK(model_.updateItemSettings(ids_[0], settings), "finite repeat configured");
    }
    controller_.playDsk();
    CHECK(await([&] {
      controller_.updatePlaybackUi();
      return stateMachine().phase() == PlaylistItemPlayoutPhase::Ended;
    }), "real EOS executes the configured End Mode");
    CHECK(!controller_.player_.dskIsPlaying(), "ended DSK shows paused transport");
    if (mode == PlaylistEndMode::Repeat) {
      CHECK(stateMachine().currentPlayCount() == 2, "repeat count remains correct");
    }
    controller_.playDsk();
    CHECK(await([&] {
      controller_.updatePlaybackUi();
      return controller_.player_.dskPositionMs() > 1100 &&
             controller_.player_.dskPositionMs() < 1500;
    }), "manual Play after end restarts at In without stale completion");
    CHECK(controller_.player_.dskIsPlaying() &&
          stateMachine().phase() == PlaylistItemPlayoutPhase::Active,
          "restarted video remains active");
    std::cout << "DSK End Mode transport: PASS\n";
    return true;
  }

  bool dskPendingEosTransport(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause, PlaylistMediaType::Video, 1000, 1600),
          "pending EOS fixture prepared");
    controller_.playDsk();
    CHECK(await([&] { return !controller_.player_.dskIsPlaying(); }),
          "real EOS is latched without controller completion");
    CHECK(controller_.player_.restartDskFromIn(true), "backend restarts before EOS consumption");
    controller_.updatePlaybackUi();
    CHECK(controller_.player_.dskIsPlaying() &&
          stateMachine().phase() == PlaylistItemPlayoutPhase::Active,
          "backend restart retires the preceding iteration's EOS");
    CHECK(await([&] { return !controller_.player_.dskIsPlaying(); }), "second EOS is latched");
    controller_.pauseDsk();
    controller_.updatePlaybackUi();
    CHECK(controller_.dskEndOfStreamPending_ && controller_.dskManuallyPaused_,
          "Pause at EOS defers completion safely");
    controller_.playDsk();
    controller_.updatePlaybackUi();
    CHECK(!controller_.dskEndOfStreamPending_ && controller_.player_.dskIsPlaying(),
          "Play at held EOS starts a new iteration once");
    std::cout << "DSK pending EOS transport: PASS\n";
    return true;
  }

  bool dskStillTransport(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Pause, PlaylistMediaType::StillImage, 100, 2000),
          "timed still prepared");
    auto &clock = controller_.dskTimedStillClock_;
    CHECK(clock.active() && !clock.playing(), "timed still honors Start Pause");
    controller_.playDsk();
    const auto anchor = controller_.dskTimedStillElapsedTimer_;
    QThread::msleep(60);
    controller_.playDsk();
    CHECK(controller_.dskTimedStillElapsedTimer_ == anchor,
          "repeated Play never discards elapsed still time");
    controller_.pauseDsk();
    const qint64 held = clock.positionMs();
    CHECK(held >= 150 && !clock.playing(), "Pause accounts for elapsed still time");
    QThread::msleep(60);
    controller_.updatePlaybackUi();
    CHECK(clock.positionMs() == held, "still clock remains held while paused");
    controller_.playDsk();
    QThread::msleep(60);
    controller_.updatePlaybackUi();
    CHECK(clock.positionMs() > held, "still clock resumes from the held position");
    controller_.dskTimedStillCompletionDue_ = clock.advance(2000);
    controller_.pauseDsk();
    controller_.updatePlaybackUi();
    CHECK(stateMachine().phase() == PlaylistItemPlayoutPhase::Active,
          "Pause at still Out defers completion");
    controller_.playDsk();
    CHECK(clock.playing() && clock.positionMs() == 100 &&
          !controller_.dskTimedStillCompletionDue_, "Play at still Out restarts at In");
    std::cout << "DSK timed still transport: PASS\n";
    return true;
  }

  bool dskTransportFailureCleanup(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Next), "DSK error fixture prepared");
    controller_.pauseDsk();
    // Exercise the shared path used after a synchronous failure's event has
    // been consumed for its detail, as well as by asynchronous bus errors.
    controller_.failDsk(QStringLiteral("Injected DSK transport failure"));
    controller_.updatePlaybackUi();
    CHECK(model_.dskItemId() == 0 && !controller_.dskManuallyPaused_ &&
          !controller_.dskTimedStillClock_.active() && !transition().due &&
          stateMachine().phase() == PlaylistItemPlayoutPhase::Error,
          "transport error clears item, pending transition and clocks");
    auto *play = panel_.findChild<QPushButton *>(QStringLiteral("dskPlayButton"));
    CHECK(play->isHidden() && !controller_.player_.dskIsPlaying(),
          "error hides transport and stops DSK");
    CHECK(controller_.cuePlaylistItemToDsk(*model_.itemState(ids_[0]), true),
          "DSK may be cued again after error");
    CHECK(await([&] { return ready(); }) && controller_.activatePendingDsk(),
          "new DSK activation recovers from failure");
    CHECK(!play->isHidden() && activeId() == ids_[0], "recovered DSK exposes transport");
    std::cout << "DSK transport error cleanup: PASS\n";
    return true;
  }

  bool rangeChange(const QString &path, PlaylistEndMode mode, EditTiming timing) {
    CHECK(start(path, mode), "outgoing item is prepared and active");
    controller_.maybePrepareNextItem(target_, 5000, 6000);
    if (timing != EditTiming::Loading) {
      CHECK(waitReady(), "target is Ready before its range changes");
      CHECK(requestIn() == 0, "initial preparation uses In 0");
    }
    if (timing == EditTiming::Due) {
      CHECK(stateMachine().finish() == mode, "outgoing completion resolves mode");
      transition().mode = mode;
      transition().due = true;
    }
    auto settings = model_.itemState(ids_[1])->settings;
    settings.inPointMs = 2000;
    settings.outPointMs = 7000;
    CHECK(model_.updateItemSettings(ids_[1], settings), "first range edit accepted");
    controller_.maybePrepareNextItem(target_, 5000, 6000);
    settings.inPointMs = 4000;
    settings.outPointMs = 6000;
    CHECK(model_.updateItemSettings(ids_[1], settings), "latest range edit accepted");

    if (timing != EditTiming::Due) {
      // This also drives the common invalidation path before the end.
      CHECK(waitReady(), "latest range becomes Ready before completion");
      CHECK(requestIn() == 4000, "both routes retain the latest preparation");
      if (dsk_) controller_.handleDskCompletion();
      else controller_.handleOnAirCompletion(6000);
    }
    CHECK(await([&] {
      take();
      return activeId() == ids_[1];
    }), "NEXT/GOTO takes the edited item, including an already due transition");
    const qint64 position = dsk_ ? controller_.player_.dskPositionMs()
                                 : controller_.player_.positionMs();
    std::cout << (dsk_ ? "DSK" : "PROGRAM") << " mode=" << static_cast<int>(mode)
              << " timing=" << static_cast<int>(timing)
              << " actualIn=" << position << '\n';
    CHECK(qAbs(position - 4000) < 100, "real playback starts at the new In point");
    CHECK(stateMachine().preparedMedia()->outPointMs == 6000,
          "active state agrees with the latest Out point");
    CHECK(!transition().due && !transition().itemId && !transition().request,
          "successful TAKE resets the complete automatic preparation state");
    if (!dsk_) CHECK(!controller_.player_.isPlaying(), "Start Pause remains paused");
    // The engine must also stop at the new Out, not merely report it in the model.
    CHECK(dsk_ ? controller_.player_.restartDskFromIn(true) : controller_.player_.play(),
          "edited range can play");
    CHECK(await([&] {
      QString detail;
      return dsk_ ? controller_.player_.takeDskEvent(detail) == DualDeckPlayer::DskEvent::EndOfStream
                  : controller_.player_.takeEvent(detail) == DualDeckPlayer::Event::EndOfStream;
    }), "real playback reaches the edited Out");
    const qint64 end = dsk_ ? controller_.player_.dskPositionMs()
                            : controller_.player_.positionMs();
    CHECK(qAbs(end - 6000) < 150, "real playback ends at Out 6000");
    return true;
  }

  bool repeatRegression(const QString &path, bool crossfade,
                        PlaylistRepeatEndMode finalMode, bool naturalEof = false,
                        bool shortRange = false) {
    CHECK(startRepeat(path, crossfade, finalMode, naturalEof, 3,
                      shortRange ? 400 : 1000, shortRange ? 2000 : 150),
          "repeat fixture starts paused");
    CHECK(!controller_.allowSameItemOnAirRetake_ && !controller_.programIsPlaying(),
          "automatic repeat is independent of manual retake and initial Start Pause");
    panel_.setItemFaderValue(-60);
    panel_.setItemDelayFrames(2);
    panel_.setItemEqGains({25, -15, 5});
    controller_.setPlaybackRate(1.25);
    controller_.updatePlaybackUi();
    CHECK(await([&] { return ready(); }), "same item preloads while its first play is paused");
    CHECK(controller_.cue_->itemId == ids_[0] && controller_.cue_->playbackRate == 1.25,
          "next loop is prerolled at the current speed on the other deck");
    const int firstDeck = controller_.player_.currentDeckIndex();
    CHECK(firstDeck >= 0, "first physical deck is known");
    controller_.playOnAir();
    int changes = 0;
    int lastDeck = firstDeck;
    bool sawCrossfade = false;
    bool loopFadesSuppressed = true;
    CHECK(await([&] {
      controller_.updatePlaybackUi();
      const int deck = controller_.player_.currentDeckIndex();
      if (model_.onAirItemId() == ids_[0] && deck != lastDeck) {
        ++changes;
        lastDeck = deck;
        loopFadesSuppressed &= controller_.itemFadeInProgress_ == 1.0 &&
                               controller_.itemFadeOutProgress_ == 1.0 &&
                               qAbs(panel_.itemGain() - std::pow(10.0, -6.0 / 20.0)) < 0.01;
      }
      sawCrossfade |= controller_.player_.crossfadeActive();
      return finalMode == PlaylistRepeatEndMode::Pause
                 ? stateMachine().phase() == PlaylistItemPlayoutPhase::Ended
                 : finalMode == PlaylistRepeatEndMode::Stop ? model_.onAirItemId() == 0
                                                          : model_.onAirItemId() == ids_[1];
    }, 10000), "finite repeat reaches its configured final action");
    CHECK(changes == 2 && lastDeck == firstDeck,
          "three plays use A/B/A with exactly two loop handoffs");
    CHECK(loopFadesSuppressed, "loop handoffs do not fade through black");
    CHECK(sawCrossfade == crossfade, "CF OFF cuts; CF ON overlaps the decks");
    if (finalMode == PlaylistRepeatEndMode::Pause) {
      CHECK(stateMachine().currentPlayCount() == 3 && !controller_.programIsPlaying(),
            "final Pause holds count three instead of resetting or repeating again");
      CHECK(controller_.playbackRate_ == 1.25 && panel_.itemDelayFrames() == 2 &&
            panel_.itemEqGains() == (std::array<int, 3>{25, -15, 5}),
            "speed and live audio adjustments survive every repeat");
      CHECK(controller_.itemFadeOutProgress_ == 0.0,
            "FO is applied at the final end only");
    }
    std::cout << "Repeat A/B/A cf=" << crossfade << " end=" << static_cast<int>(finalMode)
          << " eof=" << naturalEof << " short=" << shortRange << ": PASS" << std::endl;
    return true;
  }

  bool repeatWaitAndManualTake(const QString &path) {
    CHECK(startRepeat(path, false, PlaylistRepeatEndMode::Pause, false, 0), "infinite wait fixture starts");
    const int initialDeck = controller_.player_.currentDeckIndex();
    controller_.playOnAir();
    // Deliberately withhold the CUE request until after EOS, modeling slow
    // preparation without relying on decoder timing or touching user media.
    auto request = controller_.cueRequestFor(*model_.itemState(ids_[0]));
    controller_.nextTransition_.mode = PlaylistEndMode::Repeat;
    controller_.nextTransition_.itemId = ids_[0];
    controller_.nextTransition_.request = request;
    controller_.nextTransition_.preparationAttempted = true;
    CHECK(await([&] {
      QString detail;
      return controller_.player_.takeEvent(detail) == DualDeckPlayer::Event::EndOfStream;
    }), "outgoing deck reaches real EOS while next preparation is withheld");
    const qint64 end = controller_.playbackEnd(controller_.programDurationMs());
    controller_.handleOnAirCompletion(end);
    CHECK(controller_.nextTransition_.due && stateMachine().currentPlayCount() == 1 &&
          controller_.player_.currentDeckIndex() == initialDeck &&
          controller_.player_.positionMs() >= end - 100 &&
          controller_.itemFadeOutProgress_ == 1.0,
          "unready loop retains the ending deck, frame, count and full opacity");
    controller_.pauseOnAir();
    controller_.scheduleCueRequest(request);
    CHECK(await([&] { return ready(); }), "withheld repeat becomes Ready");
    controller_.updatePlaybackUi();
    CHECK(controller_.player_.currentDeckIndex() == initialDeck,
          "operator Pause prevents an automatic TAKE when late CUE becomes Ready");
    controller_.playOnAir();
    CHECK(controller_.player_.currentDeckIndex() == 1 - initialDeck &&
          stateMachine().currentPlayCount() == 2 && controller_.programIsPlaying(),
          "Play takes the prepared opposite deck and advances count once");
    controller_.setPlaybackRate(2.0);
    CHECK(await([&] {
      controller_.updatePlaybackUi();
      return stateMachine().currentPlayCount() >= 4;
    }), "infinite repeat continues alternating beyond three plays");
    controller_.updatePlaybackUi();
    CHECK(await([&] { return ready(); }), "third play preloads");
    CHECK(controller_.cue_->playbackRate == 2.0,
          "changing speed during repeat also reprepares the reserved deck at that speed");
    CHECK(playlist_.selectNextItemAfter(ids_[0]), "operator selects a different row");
    CHECK(controller_.cue_->itemId == ids_[0], "selection alone retains the repeat reservation");
    controller_.takeSelectedPlaylistItemOnAir();
    CHECK(await([&] {
      controller_.updatePlaybackUi();
      return model_.onAirItemId() == ids_[1];
    }), "explicit manual ON AIR cancels repeat reservation and takes the selected item");
    CHECK(!controller_.nextTransition_.due && !controller_.programIsPlaying(),
          "manual destination honors Start Pause without an old repeat firing");
    std::cout << "Infinite repeat, late CUE hold, Pause/Play, rate change and manual TAKE: PASS" << std::endl;
    return true;
  }

  bool modeAndDestinationChanges(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Goto), "GOTO outgoing item starts");
    CHECK(waitReady(), "GOTO target is Ready");
    const auto oldCue = dsk_ ? controller_.dskCue_ : controller_.cue_;
    auto settings = model_.itemState(ids_[1])->settings;
    settings.startMode = PlaylistStartMode::Play;
    settings.endMode = PlaylistEndMode::Stop;
    settings.fadeInEnabled = true;
    CHECK(model_.updateItemSettings(ids_[1], settings), "mode-only edit accepted");
    controller_.maybePrepareNextItem(target_, 5000, 6000);
    CHECK((dsk_ ? controller_.dskCue_ : controller_.cue_) == oldCue && ready(),
          "mode-only changes retain Ready media");
    auto outgoing = model_.itemState(ids_[0])->settings;
    outgoing.gotoTargetItemId = ids_[2];
    outgoing.endCrossfadeEnabled = true;
    outgoing.endCrossfadeDurationMs = 150;
    CHECK(model_.updateItemSettings(ids_[0], outgoing), "GOTO destination changes");
    CHECK(model_.updateItemSettings(ids_[2], settings), "new destination uses Start Play");
    CHECK(stateMachine().finish() == PlaylistEndMode::Goto, "GOTO becomes due");
    transition().mode = PlaylistEndMode::Goto;
    transition().due = true;
    CHECK(await([&] { take(); return activeId() == ids_[2]; }),
          "TAKE rechecks the destination and preserves due during replacement");
    CHECK(await([&] {
      return (dsk_ ? controller_.player_.dskPositionMs()
                   : controller_.player_.positionMs()) > 100;
    }), "latest Start Play advances media after the crossfade TAKE");
    return true;
  }

  bool eligibilityChanges(const QString &path) {
    CHECK(start(path, PlaylistEndMode::Next), "NEXT outgoing item starts");
    CHECK(waitReady(), "NEXT initially prepares second item");
    CHECK(transition().itemId == ids_[1], "initial NEXT destination is second item");
    if (dsk_) model_.setOnAirItemId(ids_[1]);
    else model_.setDskItemId(ids_[1]);
    CHECK(waitReady() && transition().itemId == ids_[2],
          "newly occupied target is excluded on both routes");
    auto settings = model_.itemState(ids_[0])->settings;
    settings.endMode = PlaylistEndMode::Stop;
    CHECK(model_.updateItemSettings(ids_[0], settings), "outgoing mode changes to Stop");
    controller_.maybePrepareNextItem(target_, 5000, 6000);
    CHECK(!transition().itemId && !transition().request && !transition().due,
          "leaving NEXT/GOTO cancels the automatic preparation");
    return true;
  }

private:
  bool startRepeat(const QString &path, bool crossfade, PlaylistRepeatEndMode finalMode,
                   bool naturalEof = false, int repeatCount = 3,
                   qint64 rangeMs = 1000, qint64 crossfadeMs = 150) {
    CHECK(controller_.isAvailable(), "repeat real player initializes");
    CHECK(await([&] { return controller_.programOutputDeviceStatusKey() ==
                             QStringLiteral("settings.output.applied"); }), "repeat output ready");
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = PlaylistMediaType::Video;
    item.settings.inPointMs = naturalEof ? 9000 : 1000;
    item.settings.outPointMs = naturalEof ? std::nullopt : std::optional<qint64>{1000 + rangeMs};
    item.settings.endMode = PlaylistEndMode::Repeat;
    item.settings.repeatCount = repeatCount;
    item.settings.repeatEndMode = finalMode;
    item.settings.endCrossfadeEnabled = crossfade;
    item.settings.endCrossfadeDurationMs = crossfadeMs;
    item.settings.fadeInEnabled = true;
    item.settings.fadeInDurationMs = 100;
    item.settings.fadeOutEnabled = true;
    item.settings.fadeOutDurationMs = 100;
    PlaylistItemRegistration destination = item;
    destination.settings.endMode = PlaylistEndMode::Pause;
    destination.settings.fadeInEnabled = false;
    destination.settings.fadeOutEnabled = false;
    ids_ = model_.addItems({item, destination});
    CHECK(ids_.size() == 2, "repeat and destination registered");
    auto settings = item.settings;
    settings.gotoTargetItemId = ids_[1];
    CHECK(model_.updateItemSettings(ids_[0], settings), "repeat final destination set");
    const auto repeatItem = *model_.itemState(ids_[0]);
    controller_.scheduleCueRequest(controller_.cueRequestFor(repeatItem));
    CHECK(await([&] { return ready(); }), "initial repeat CUE ready");
    CHECK(controller_.takeCueOnAir(repeatItem, false, 100), "initial repeat TAKE succeeds");
    CHECK(await([&] { return controller_.itemFadeInProgress_ == 1.0; }), "initial FI completes");
    return true;
  }
  using Target = ProgramPlayoutController::PlayoutTarget;
  auto &transition() { return dsk_ ? controller_.nextDskTransition_ : controller_.nextTransition_; }
  auto &stateMachine() { return dsk_ ? controller_.dskStateMachine_ : controller_.onAirStateMachine_; }
  quint64 activeId() const { return dsk_ ? model_.dskItemId() : model_.onAirItemId(); }
  qint64 requestIn() const {
    const auto &cue = dsk_ ? controller_.dskCue_ : controller_.cue_;
    return cue ? cue->inPointMs : -1;
  }
  bool ready() {
    return dsk_ ? controller_.player_.dskCueStatus() == DualDeckPlayer::DskStatus::Ready
                : !controller_.cueReplacementPending_ &&
                  controller_.player_.cueStatus() == DualDeckPlayer::CueStatus::Ready;
  }
  void take() {
    if (dsk_) (void)controller_.takePreparedNextDskItem();
    else (void)controller_.takePreparedNextItem();
  }
  template <typename Predicate> bool await(Predicate predicate, int timeoutMs = 6000) {
    QElapsedTimer timer;
    timer.start();
    do {
      QCoreApplication::processEvents();
      controller_.player_.processBusMessages();
      controller_.updateCueUi();
      if (predicate()) return true;
      QThread::msleep(5);
    } while (timer.elapsed() < timeoutMs);
    return false;
  }
  bool waitReady() {
    return await([&] {
      const bool refreshed = controller_.maybePrepareNextItem(target_, 5000, 6000);
      return refreshed && transition().itemId && ready();
    });
  }
  bool start(const QString &path, PlaylistEndMode mode,
             PlaylistMediaType mediaType = PlaylistMediaType::Video,
             qint64 inPointMs = 0, qint64 outPointMs = 6000) {
    CHECK(controller_.isAvailable(), "real player initializes");
    CHECK(await([&] {
      return controller_.player_.programOutputDeviceStatusKey() ==
             QStringLiteral("settings.output.applied");
    }), "PROGRAM output finishes asynchronous initialization");
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = mediaType;
    item.settings.startMode = PlaylistStartMode::Pause;
    item.settings.endMode = PlaylistEndMode::Pause;
    item.settings.outPointMs = 8000;
    ids_ = model_.addItems({item, item, item});
    CHECK(ids_.size() == 3, "test playlist created");
    auto settings = item.settings;
    settings.inPointMs = inPointMs;
    settings.outPointMs = outPointMs;
    settings.endMode = mode;
    settings.gotoTargetItemId = ids_[1];
    settings.nominalAudioDelayFrames = 4;
    settings.nominalEqTenthsDb = {-30, 15, 0};
    CHECK(model_.updateItemSettings(ids_[0], settings), "outgoing settings applied");
    const auto outgoing = *model_.itemState(ids_[0]);
    if (dsk_) {
      CHECK(controller_.cuePlaylistItemToDsk(outgoing, true), "DSK CUE starts");
      CHECK(await([&] { return ready(); }), "DSK CUE becomes Ready");
      CHECK(controller_.activatePendingDsk(), "DSK activates");
    } else {
      controller_.scheduleCueRequest(controller_.cueRequestFor(outgoing));
      CHECK(await([&] { return ready(); }), "PROGRAM CUE becomes Ready");
      CHECK(controller_.takeCueOnAir(outgoing, false, 1000), "PROGRAM activates");
      auto *eq = panel_.findChild<QPushButton *>(QStringLiteral("programItemEqButton"));
      CHECK(eq && eq->property("eqAdjusted").toBool(), "TAKE restores the nominal ITEM EQ presentation");
      auto *delay = panel_.findChild<QSpinBox *>(QStringLiteral("programItemDelaySpin"));
      CHECK(delay && delay->value() == 4, "TAKE restores nominal audio delay");
    }
    return true;
  }

  ControllerOperations operations_;
  QWidget video_;
  ProgramPanel panel_;
  PlaylistModel model_;
  UserInfoCenter info_;
  PlaylistPanel playlist_;
  UvcSourceSessionManager uvcSources_{false};
  ProgramPlayoutController controller_;
  bool dsk_;
  Target target_;
  QList<quint64> ids_;
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTemporaryDir directory;
  if (!directory.isValid()) return 1;
  QCoreApplication::setOrganizationName(QStringLiteral("VtrPonTransitionTest"));
  QCoreApplication::setApplicationName(QStringLiteral("Playout"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
  if (app.arguments().contains(QStringLiteral("--switcher-only"))) {
    gst_init(nullptr, nullptr);
    if (!require(substituteProgramSink(), "test audio sink registered")) return 1;
    return ProgramPlayoutTransitionTest(false).switcherRegression() ? 0 : 1;
  }
  if (!app.arguments().contains(QStringLiteral("--audio-output-message-only")) &&
      !app.arguments().contains(QStringLiteral("--document-import-queue-only")) &&
      !runOperatorTimerPanelTest()) return 1;
  if (app.arguments().contains(QStringLiteral("--timer-only"))) return 0;
  if (!app.arguments().contains(QStringLiteral("--audio-output-message-only")) &&
      !app.arguments().contains(QStringLiteral("--document-import-queue-only")) &&
      !runControllerShortcutsRegression()) return 1;
  gst_init(nullptr, nullptr);
  if (!require(substituteProgramSink(), "test audio sink registered")) return 1;
  if (app.arguments().contains(QStringLiteral("--document-import-queue-only")))
    return ProgramPlayoutTransitionTest(false).documentImports() ? 0 : 1;
  if (app.arguments().size() == 4 &&
      app.arguments().at(1) == QStringLiteral("--powerpoint-queue-selection-smoke"))
    return ProgramPlayoutTransitionTest(false).powerPointQueueSelectionSmoke(
        app.arguments().at(2), app.arguments().at(3)) ? 0 : 1;
  const auto checkBgmOutputWarnings = [](const QString &path) {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue(QStringLiteral("audio/programOutputDeviceId"), QStringLiteral("missing-test-output"));
    settings.sync();
    const bool passed = ProgramPlayoutTransitionTest(false).bgmOutputWarnings(path);
    settings.remove(QStringLiteral("audio/programOutputDeviceId"));
    settings.sync();
    return passed;
  };
  if (app.arguments().contains(QStringLiteral("--bgm-only"))) {
    QTranslator translator;
    if (!require(translator.load(app.applicationDirPath() + QStringLiteral("/vtrpon_ja.qm")),
                 "BGM UI Japanese catalog")) return 1;
    app.installTranslator(&translator);
    vtrpon::applyControllerSkin(app, QStringLiteral("hardware"));
    const QString audioPath = directory.filePath(QStringLiteral("bgm.wav"));
    if (!require(createVideo(audioPath, true), "BGM audio generated")) return 1;
    if (!checkBgmOutputWarnings(audioPath)) return 1;
    return ProgramPlayoutTransitionTest(false).bgmRegression(audioPath) ? 0 : 1;
  }
  if (app.arguments().size() == 4 &&
      app.arguments().at(1) == QStringLiteral("--powerpoint-tile-smoke")) {
    if (!require(powerPointProcessIds().empty(), "tile smoke requires Office to be closed")) return 1;
    const QString state = app.arguments().at(3);
    const int command = state == QStringLiteral("maximized") ? SW_SHOWMAXIMIZED
                      : state == QStringLiteral("minimized") ? SW_SHOWMINIMIZED : SW_SHOWNORMAL;
    const bool succeeded = ProgramPlayoutTransitionTest(false).nativeTileTake(app.arguments().at(2), command);
    return require(powerPointProcessIds().empty(), "tile smoke releases its Office process") && succeeded ? 0 : 1;
  }
  if (app.arguments().size() == 3 &&
      app.arguments().at(1) == QStringLiteral("--powerpoint-recovery-smoke")) {
    if (!require(powerPointProcessIds().empty(), "recovery smoke requires Office to be closed")) return 1;
    const bool succeeded = ProgramPlayoutTransitionTest(false).nativeSelectedTake(app.arguments().at(2), true);
    return require(powerPointProcessIds().empty(), "recovered Office is released on shutdown") && succeeded ? 0 : 1;
  }
  if (app.arguments().size() == 3 &&
      app.arguments().at(1) == QStringLiteral("--powerpoint-take-smoke")) {
    return ProgramPlayoutTransitionTest(false).nativeSelectedTake(app.arguments().at(2)) ? 0 : 1;
  }
  if (app.arguments().size() == 3 &&
      app.arguments().at(1) == QStringLiteral("--powerpoint-warmup-smoke")) {
    if (!require(powerPointProcessIds().empty(), "warmup smoke requires Office to be closed")) return 1;
    bool ok = true;
    for (bool restore : {false, true}) {
      {
        ProgramPlayoutTransitionTest test(false);
        ok &= test.nativePowerPointRegistration(app.arguments().at(2), restore);
      }
      ok &= require(powerPointProcessIds().empty(), "prestarted Office exits after shutdown without CUE");
    }
    return ok ? 0 : 1;
  }
  const QString path = directory.filePath(QStringLiteral("range.avi"));
  if (!require(createVideo(path), "ten-second video generated")) return 1;
  if (app.arguments().contains(QStringLiteral("--total-benchmark-only")))
    return ProgramPlayoutTransitionTest(false).totalFutureCache(path) ? 0 : 1;
  for (const auto mode : {PlaylistEndMode::Next, PlaylistEndMode::Goto}) {
    for (const bool allowManualRetake : {false, true}) {
      if (!ProgramPlayoutTransitionTest(false).selfTransition(
              path, mode, allowManualRetake)) return 1;
    }
  }
  if (app.arguments().contains(QStringLiteral("--self-transition-only"))) return 0;
  if (!ProgramPlayoutTransitionTest(false).macroItemIdRequests(path)) return 1;
  if (!ProgramPlayoutTransitionTest(false).macroAutomaticPriority(path)) return 1;
  if (!ProgramPlayoutTransitionTest(false).macroAutomaticPriority(path, true)) return 1;
  if (!ProgramPlayoutTransitionTest(false).macroAutomaticPriority(path, false, true)) return 1;
  const QString macroStillPath = directory.filePath(QStringLiteral("macro-still.png"));
  QImage macroStill(3840, 2160, QImage::Format_RGB32);
  macroStill.fill(QColor(25, 100, 170));
  if (!require(macroStill.save(macroStillPath), "macro timed-still image generated")) return 1;
  if (!ProgramPlayoutTransitionTest(false).macroTimedStillTransport(macroStillPath)) return 1;
  if (app.arguments().contains(QStringLiteral("--macro-only"))) return 0;
  {
    using Test = ProgramPlayoutTransitionTest;
    const QString stillPath = directory.filePath(QStringLiteral("dsk-still.png"));
    QImage still(160, 90, QImage::Format_ARGB32);
    still.fill(QColor(0, 200, 50, 180));
    if (!require(still.save(stillPath), "DSK alpha still generated")) return 1;
    bool dskOk = Test(true).dskManualTransport(path);
    dskOk &= Test(true).dskFadeTransport(path);
    dskOk &= Test(true).dskWaitingTransport(path, PlaylistEndMode::Next);
    dskOk &= Test(true).dskWaitingTransport(path, PlaylistEndMode::Goto);
    dskOk &= Test(true).dskWaitingTransport(path, PlaylistEndMode::Next, false);
    for (auto mode : {PlaylistEndMode::Stop, PlaylistEndMode::Pause, PlaylistEndMode::Repeat}) {
      dskOk &= Test(true).dskEndedTransport(path, mode);
    }
    dskOk &= Test(true).dskPendingEosTransport(path);
    dskOk &= Test(true).dskStillTransport(stillPath);
    dskOk &= Test(true).dskTransportFailureCleanup(path);
    if (!dskOk) return 1;
    if (app.arguments().contains(QStringLiteral("--dsk-transport-only"))) return 0;
  }
  const QString avPath = directory.filePath(QStringLiteral("av-range.avi"));
  if (!require(createVideo(avPath, false, true), "video with audio generated")) return 1;
  const auto checkAudioRecovery = [&] {
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue(QStringLiteral("audio/programOutputDeviceId"), QStringLiteral("missing-test-output"));
    settings.sync();
    const bool silentPassed = ProgramPlayoutTransitionTest(false).audioOutputFailureMessage(path);
    const bool soundPassed = ProgramPlayoutTransitionTest(false).audioOutputFailureMessage(avPath, true);
    settings.remove(QStringLiteral("audio/programOutputDeviceId"));
    settings.sync();
    return silentPassed && soundPassed;
  };
  if (app.arguments().contains(QStringLiteral("--audio-output-message-only")))
    return checkAudioRecovery() ? 0 : 1;
  if (!DeckSourceSessionTest::run(path) ||
      !DeckSourceSessionTest::run(avPath)) return 1;
  // Each temporary must finish shutdown before the next creates the named
  // PROGRAM audio receivers (not two temporaries in one full expression).
  if (!ProgramPlayoutTransitionTest(false).fileCueRecovery(path)) return 1;
  if (!ProgramPlayoutTransitionTest(false).fileCueRecovery(avPath)) return 1;
  if (app.arguments().contains(QStringLiteral("--cue-recovery-only"))) return 0;
  {
    using Test = ProgramPlayoutTransitionTest;
    bool repeatOk = Test(false).repeatRegression(path, false, PlaylistRepeatEndMode::Pause);
    repeatOk &= Test(false).repeatRegression(path, true, PlaylistRepeatEndMode::Pause);
    repeatOk &= Test(false).repeatRegression(path, true, PlaylistRepeatEndMode::Pause, false, true);
    repeatOk &= Test(false).repeatRegression(path, false, PlaylistRepeatEndMode::Pause, true);
    repeatOk &= Test(false).repeatRegression(path, false, PlaylistRepeatEndMode::Next);
    repeatOk &= Test(false).repeatRegression(path, false, PlaylistRepeatEndMode::Goto);
    repeatOk &= Test(false).repeatRegression(path, false, PlaylistRepeatEndMode::Stop);
    repeatOk &= Test(false).repeatWaitAndManualTake(path);
    if (!repeatOk) return 1;
    if (app.arguments().contains(QStringLiteral("--repeat-only"))) return 0;
  }
  if (!checkAudioRecovery()) return 1;
  if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--uvc-device-smoke"))
    return ProgramPlayoutTransitionTest(false).uvcFailureRegression(path, app.arguments().at(2), app.arguments().at(3)) ? 0 : 1;
  if (!require(UvcFaultTest::install(), "fault test camera registered")) return 1;
  if (!uvcInitialFailureRegression()) return 1;
  if (!uvcBoundedShutdownRegression()) return 1;
  if (app.arguments().contains(QStringLiteral("--uvc-shutdown-only"))) return 0;
  if (!ProgramPlayoutTransitionTest(false).uvcFailureRegression(path)) return 1;
  if (app.arguments().contains(QStringLiteral("--uvc-only"))) return 0;
  bool ok = true;
  using Test = ProgramPlayoutTransitionTest;
  ok &= Test(false).operatorTimerIndependence(path);
  if (app.arguments().contains(QStringLiteral("--timer-integration-only"))) return ok ? 0 : 1;
  ok &= Test(false).switcherRegression();
  const QString audioPath = directory.filePath(QStringLiteral("bgm.wav"));
  if (!require(createVideo(audioPath, true), "BGM audio generated")) return 1;
  ok &= Test(false).endOffRegression(path);
  ok &= Test(false).bgmRegression(audioPath);
  ok &= checkBgmOutputWarnings(audioPath);
  for (bool dsk : {false, true}) {
    for (auto mode : {PlaylistEndMode::Next, PlaylistEndMode::Goto}) {
      for (auto timing : {Test::EditTiming::Loading, Test::EditTiming::Ready, Test::EditTiming::Due}) {
        ok &= Test(dsk).rangeChange(path, mode, timing);
      }
    }
    ok &= Test(dsk).modeAndDestinationChanges(path);
    ok &= Test(dsk).eligibilityChanges(path);
  }
  std::cout << "PROGRAM/DSK transition regression: " << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
