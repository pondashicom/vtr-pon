#include "macro_runner.h"
#include "playlist_model.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <functional>
#include <iostream>

namespace {
using A = ControllerOperations::Action;
using S = MacroRunner::State;
using T = MacroStepType;

bool check(bool condition, const char *expression, int line) {
  if (condition) return true;
  std::cerr << "line " << line << ": check failed: " << expression << '\n';
  return false;
}
#define CHECK(expression) do { if (!check((expression), #expression, __LINE__)) return false; } while (false)

struct Request {
  A action;
  quint64 id;
  quint64 itemId;
  bool ready = false;
  bool crossfade = false;
  qint64 durationMs = 0;
};

struct Fixture {
  QTemporaryDir directory;
  qint64 mono = 0;
  QDateTime wall = QDateTime::fromString(
      QStringLiteral("2026-09-22T23:59:59.500+09:00"), Qt::ISODateWithMs);
  PlaylistModel model;
  ControllerOperations operations;
  QList<Request> requests;
  QStringList events;
  QHash<QString, double> runtime;
  QStringList transports;
  std::function<void()> runtimeHook;
  std::function<void()> cancelHook;
  int cancellations = 0;
  bool manualSawPaused = false;
  bool acceptCue = true;
  bool synchronousCue = false;
  bool synchronousTake = false;
  bool valid = false;
  quint64 itemId = 0;
  MacroRunner runner;

  Fixture() : runner(&model, &operations,
      MacroRunner::Clock{[this] { return wall; }, [this] { return mono; }}) {
    const QString path = directory.filePath(QStringLiteral("clip.mov"));
    QFile file(path);
    if (!directory.isValid() || !file.open(QIODevice::WriteOnly) || file.write("test") != 4) return;
    file.close();
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = PlaylistMediaType::Video;
    const auto added = model.addItems({item});
    if (added.size() != 1) return;
    itemId = added[0];
    operations.bind<quint64, quint64>(A::MacroCue, &model,
        [this](quint64 request, quint64 target) {
      events.append(QStringLiteral("cue"));
      requests.append({A::MacroCue, request, target});
      if (synchronousCue) runner.finishRequest(request, true, {});
      return acceptCue;
    });
    operations.bind<quint64, quint64, bool, bool, qint64>(A::MacroTake, &model,
        [this](quint64 request, quint64 target, bool ready, bool crossfade, qint64 duration) {
      events.append(QStringLiteral("take"));
      requests.append({A::MacroTake, request, target, ready, crossfade, duration});
      if (synchronousTake) runner.finishRequest(request, true, {});
      return true;
    });
    operations.bind(A::CancelMacroRequests, &model, [this] {
      events.append(QStringLiteral("cancel"));
      ++cancellations;
      if (cancelHook) cancelHook();
    });
    operations.bind<quint64, QJsonObject>(A::MacroItemSettings, &model,
        [this](quint64 target, const QJsonObject &patch) {
      const auto item = model.itemState(target);
      PlaylistItemSettings edited;
      if (!item || !MacroDocumentIo::applySettingsPatch(item->settings, patch, &edited)) return false;
      events.append(QStringLiteral("settings"));
      return model.updateItemSettings(target, edited);
    });
    operations.bind<QString, double>(A::MacroRuntimeSetting, &model,
        [this](const QString &operation, double value) {
      events.append(QStringLiteral("runtime"));
      runtime.insert(operation, value);
      if (runtimeHook) runtimeHook();
      return true;
    });
    operations.bind<quint64, QString>(A::MacroTransport, &model, [this](quint64 request, const QString &operation) {
      events.append(QStringLiteral("transport"));
      transports.append(operation);
      runner.finishRequest(request, true, {});
      return true;
    });
    operations.bind(A::ProgramPlay, &model, [this] {
      events.append(QStringLiteral("manual"));
      manualSawPaused = runner.state() == S::Paused;
    });
    valid = true;
  }

  MacroStep step(T type) const {
    MacroStep result;
    result.type = type;
    if (type == T::Cue || type == T::Take || type == T::ItemSettings) {
      result.itemId = itemId;
      result.documentId = model.documentId();
    }
    if (type == T::RuntimeSetting) result.operation = QStringLiteral("masterVolume");
    if (type == T::Transport) result.operation = QStringLiteral("play");
    return result;
  }
  MacroDefinition macro(const QList<MacroStep> &steps) const {
    MacroDefinition result;
    result.name = QStringLiteral("Test rundown");
    result.steps = steps;
    return result;
  }
  void tick(qint64 milliseconds) {
    mono += milliseconds;
    wall = wall.addMSecs(milliseconds);
    runner.poll();
  }
};

bool sequenceAndRequestCorrelation() {
  Fixture f;
  CHECK(f.valid);
  auto disabled = f.step(T::Take);
  disabled.enabled = false;
  auto take = f.step(T::Take);
  take.crossfade = true;
  take.crossfadeDurationMs = 750;
  auto volume = f.step(T::RuntimeSetting);
  volume.value = -65;
  CHECK(f.runner.start(f.macro({disabled, f.step(T::Cue), take, volume,
                              f.step(T::Transport), f.step(T::Note)})));
  f.runner.poll();
  CHECK(f.requests.size() == 1);
  CHECK(f.requests[0].action == A::MacroCue);
  CHECK(f.requests[0].itemId == f.itemId);
  CHECK(f.runner.currentStepIndex() == 1);
  CHECK(f.runner.state() == S::Waiting);
  f.tick(100);
  CHECK(f.requests.size() == 1);
  f.runner.finishRequest(f.requests[0].id + 500, true, {});
  CHECK(f.runner.currentStepIndex() == 1);
  f.runner.finishRequest(f.requests[0].id, true, {});
  CHECK(f.runner.currentStepIndex() == 2);
  f.runner.poll();
  CHECK(f.requests.size() == 2);
  CHECK(f.requests[1].action == A::MacroTake);
  CHECK(f.requests[1].id != f.requests[0].id);
  CHECK(f.requests[1].crossfade && f.requests[1].durationMs == 750);
  f.runner.finishRequest(f.requests[0].id, false, QStringLiteral("stale failure"));
  CHECK(f.runner.state() == S::Waiting);
  f.runner.finishRequest(f.requests[1].id, true, {});
  f.runner.poll();
  CHECK(f.runtime.value(QStringLiteral("masterVolume")) == -65);
  f.runner.poll();
  CHECK(f.transports == QStringList{QStringLiteral("play")});
  f.runner.poll();
  f.runner.poll();
  CHECK(f.runner.state() == S::Completed);
  CHECK(!f.runner.isActive());
  CHECK(!f.runner.logLines().isEmpty());
  return true;
}

bool delayPauseResume() {
  Fixture f;
  CHECK(f.valid);
  auto delay = f.step(T::Delay);
  delay.durationMs = 2000;
  CHECK(f.runner.start(f.macro({delay, f.step(T::Note)})));
  f.runner.poll();
  CHECK(f.runner.remainingMs() == 2000);
  f.tick(750);
  f.runner.pause();
  CHECK(f.runner.state() == S::Paused);
  CHECK(f.runner.remainingMs() == 1250);
  f.tick(10000); // Paused time does not count toward the relative delay.
  CHECK(f.runner.remainingMs() == 1250);
  f.runner.resume();
  CHECK(f.runner.state() == S::Waiting);
  f.tick(1000);
  CHECK(f.runner.remainingMs() == 250);
  f.tick(249);
  CHECK(f.runner.currentStepIndex() == 0);
  f.tick(1);
  CHECK(f.runner.currentStepIndex() == 1);
  f.runner.poll();
  f.runner.poll();
  CHECK(f.runner.state() == S::Completed);
  return true;
}

bool stoppedAndCancelledResponsesStayInert() {
  Fixture f;
  CHECK(f.valid);
  const auto definition = f.macro({f.step(T::Take), f.step(T::Transport)});
  CHECK(f.runner.start(definition));
  f.runner.poll();
  CHECK(f.requests.size() == 1);
  const quint64 firstRequest = f.requests.last().id;
  f.cancelHook = [&f, firstRequest] { f.runner.finishRequest(firstRequest, true, {}); };
  f.runner.stop(); // The cancellation itself delivers a completion synchronously.
  CHECK(f.runner.state() == S::Stopped);
  CHECK(f.cancellations == 1);
  f.runner.finishRequest(firstRequest, true, {});
  f.runner.poll();
  CHECK(f.transports.isEmpty());
  CHECK(f.runner.state() == S::Stopped);
  CHECK(f.runner.start(definition));
  f.runner.poll();
  const quint64 secondRequest = f.requests.last().id;
  CHECK(secondRequest != firstRequest);
  f.runner.finishRequest(firstRequest, false, QStringLiteral("old generation"));
  CHECK(f.runner.state() == S::Waiting);
  CHECK(f.runner.currentStepIndex() == 0);
  f.runner.finishRequest(secondRequest, true, {});
  f.runner.poll();
  CHECK(f.transports.size() == 1);
  f.runner.poll();
  CHECK(f.runner.state() == S::Completed);
  return true;
}

bool schedulesAndLateness() {
  {
    Fixture f;
    CHECK(f.valid);
    auto expired = f.step(T::WaitUntil);
    expired.scheduledAt = f.wall.addMSecs(-1);
    CHECK(!f.runner.start(f.macro({expired, f.step(T::Take)})));
    CHECK(f.runner.state() == S::Error);
    CHECK(f.requests.isEmpty());
  }
  {
    Fixture f;
    CHECK(f.valid);
    auto wait = f.step(T::WaitUntil);
    wait.scheduledAt = f.wall.addMSecs(500);
    CHECK(f.runner.start(f.macro({wait, f.step(T::Take)})));
    f.runner.poll();
    f.tick(1501); // Normal polling interval, but one millisecond beyond tolerance.
    CHECK(f.runner.state() == S::Error);
    CHECK(f.requests.isEmpty());
  }
  {
    Fixture f;
    CHECK(f.valid);
    auto wait = f.step(T::WaitUntil);
    wait.scheduledAt = f.wall.addMSecs(500);
    auto take = f.step(T::Take);
    take.requireReady = true;
    CHECK(f.runner.start(f.macro({wait, f.step(T::Note), take})));
    f.runner.poll();
    f.tick(500); // Wait completes on time.
    f.tick(501); // Intervening note.
    f.tick(500); // TAKE must independently recheck the schedule deadline.
    CHECK(f.runner.state() == S::Error);
    CHECK(f.requests.isEmpty());
  }
  {
    Fixture f;
    CHECK(f.valid);
    auto wait = f.step(T::WaitUntil);
    wait.scheduledAt = f.wall.addMSecs(1500); // Explicitly crosses midnight.
    auto take = f.step(T::Take);
    take.requireReady = true;
    CHECK(f.runner.start(f.macro({wait, take})));
    f.runner.poll();
    f.tick(750);
    CHECK(f.runner.remainingMs() == 750);
    f.tick(750);
    f.runner.poll();
    CHECK(f.requests.size() == 1 && f.requests[0].ready);
    CHECK(f.wall.date() == QDate(2026, 9, 23));
    f.runner.finishRequest(f.requests[0].id, true, {});
    f.runner.poll();
    CHECK(f.runner.state() == S::Completed);
  }
  return true;
}

bool clockChangeAndExpiredResume() {
  Fixture f;
  CHECK(f.valid);
  auto wait = f.step(T::WaitUntil);
  wait.scheduledAt = f.wall.addSecs(10);
  CHECK(f.runner.start(f.macro({wait, f.step(T::Take)})));
  f.runner.poll();
  f.mono += 100;
  f.wall = f.wall.addMSecs(2500);
  f.runner.poll();
  CHECK(f.runner.state() == S::Paused);
  CHECK(f.requests.isEmpty());
  f.runner.resume();
  CHECK(f.runner.state() == S::Waiting);
  f.tick(1000);
  CHECK(f.runner.state() == S::Waiting);
  f.runner.pause();
  f.tick(10000);
  f.runner.resume();
  CHECK(f.runner.state() == S::Error);
  CHECK(f.requests.isEmpty());
  return true;
}

bool manualOperationHasPriority() {
  for (const char *source : {"UI", "Shortcut", "Switcher"}) {
    Fixture f;
    CHECK(f.valid);
    CHECK(f.runner.start(f.macro({f.step(T::Cue), f.step(T::Take)})));
    f.runner.poll();
    const quint64 original = f.requests.last().id;
    CHECK(f.operations.executeFrom(source, A::ProgramPlay).invoked());
    CHECK(f.manualSawPaused);
    CHECK(f.runner.state() == S::Paused);
    CHECK(f.events.size() == 3);
    CHECK(f.events[0] == QStringLiteral("cue"));
    CHECK(f.events[1] == QStringLiteral("cancel"));
    CHECK(f.events[2] == QStringLiteral("manual"));
    f.runner.finishRequest(original, true, {});
    CHECK(f.runner.currentStepIndex() == 0);
    f.runner.resume();
    f.runner.poll();
    CHECK(f.requests.size() == 2);
    CHECK(f.requests.last().id != original);
    f.runner.finishRequest(original, true, {});
    CHECK(f.runner.currentStepIndex() == 0);
    f.runner.finishRequest(f.requests.last().id, true, {});
    CHECK(f.runner.currentStepIndex() == 1);
  }
  return true;
}

bool rejectedInputAndMacroSourceDoNotInterrupt() {
  using Status = ControllerOperations::Status;
  for (const char *source : {"Shortcut", "Switcher"}) {
    Fixture f;
    CHECK(f.valid);
    bool available = false;
    f.operations.setGuard(A::ProgramPlay, &f.model, [&available] { return available; });
    CHECK(f.runner.start(f.macro({f.step(T::Cue), f.step(T::Take)})));
    f.runner.poll();
    const quint64 original = f.requests.last().id;
    CHECK(f.operations.executeFrom(source, A::ProgramPlay, 1).status == Status::InvalidArguments);
    CHECK(f.operations.executeFrom(source, A::ProgramPlay).status == Status::Rejected);
    CHECK(f.operations.executeFrom(source, A::OffAir).status == Status::Unavailable);
    CHECK(f.runner.state() == S::Waiting);
    CHECK(f.cancellations == 0);
    CHECK(f.events == QStringList{QStringLiteral("cue")});

    available = true;
    CHECK(f.operations.executeFrom("Macro", A::ProgramPlay).invoked());
    CHECK(f.runner.state() == S::Waiting);
    CHECK(!f.manualSawPaused);
    CHECK(f.cancellations == 0);

    CHECK(f.operations.executeFrom(source, A::ProgramPlay).invoked());
    CHECK(f.runner.state() == S::Paused);
    CHECK(f.manualSawPaused);
    CHECK(f.cancellations == 1);
    CHECK(f.events == (QStringList{QStringLiteral("cue"), QStringLiteral("manual"),
                                  QStringLiteral("cancel"), QStringLiteral("manual")}));
    f.runner.finishRequest(original, true, {});
    CHECK(f.runner.state() == S::Paused);
    CHECK(f.runner.currentStepIndex() == 0);
  }
  return true;
}

bool missingForeignAndChangedTargets() {
  Fixture f;
  CHECK(f.valid);
  auto missing = f.step(T::Cue);
  missing.itemId += 1000;
  CHECK(!f.runner.start(f.macro({missing})));
  auto foreign = f.step(T::Cue);
  foreign.documentId = newMacroId();
  CHECK(!f.runner.start(f.macro({foreign})));
  CHECK(f.requests.isEmpty());
  foreign.enabled = false;
  CHECK(f.runner.start(f.macro({foreign, f.step(T::Cue)})));
  f.runner.poll();
  CHECK(f.runner.state() == S::Waiting);
  CHECK(f.model.setItemDisplayName(f.itemId, QStringLiteral("Renamed only")));
  CHECK(f.runner.state() == S::Waiting);
  auto settings = f.model.itemState(f.itemId)->settings;
  settings.startMode = PlaylistStartMode::Play;
  CHECK(f.model.updateItemSettings(f.itemId, settings));
  CHECK(f.runner.state() == S::Paused);
  f.runner.resume();
  f.runner.poll();
  CHECK(f.runner.state() == S::Waiting);
  CHECK(f.model.removeItem(f.itemId));
  CHECK(f.runner.state() == S::Paused);
  f.runner.resume();
  CHECK(f.runner.state() == S::Error);
  return true;
}

bool modelResetStopsOutstandingRequest() {
  Fixture f;
  CHECK(f.valid);
  CHECK(f.runner.start(f.macro({f.step(T::Take), f.step(T::Transport)})));
  f.runner.poll();
  const quint64 request = f.requests.last().id;
  CHECK(f.model.restoreSnapshot(f.model.snapshot()));
  CHECK(f.runner.state() == S::Error);
  CHECK(f.cancellations == 1);
  f.runner.finishRequest(request, true, {});
  f.runner.poll();
  CHECK(f.transports.isEmpty());
  CHECK(f.runner.state() == S::Error);
  return true;
}

bool ownSettingsChangesAndPredictedPatches() {
  Fixture f;
  CHECK(f.valid);
  auto setTarget = f.step(T::ItemSettings);
  setTarget.settingsPatch = {{QStringLiteral("gotoTargetItemId"), QString::number(f.itemId)}};
  auto setMode = f.step(T::ItemSettings);
  setMode.settingsPatch = {{QStringLiteral("endMode"), QStringLiteral("goto")},
                          {QStringLiteral("startMode"), QStringLiteral("play")}};
  const auto definition = f.macro({setTarget, setMode, f.step(T::Cue)});
  CHECK(f.runner.validate(definition).isEmpty());
  CHECK(f.runner.start(definition));
  f.runner.poll();
  CHECK(f.runner.state() == S::Running);
  CHECK(f.model.itemState(f.itemId)->settings.gotoTargetItemId == f.itemId);
  f.runner.poll();
  CHECK(f.runner.state() == S::Running);
  CHECK(f.model.itemState(f.itemId)->settings.endMode == PlaylistEndMode::Goto);
  f.runner.poll();
  CHECK(f.runner.state() == S::Waiting);
  CHECK(f.cancellations == 0);
  f.runner.finishRequest(f.requests.last().id, true, {});
  f.runner.poll();
  CHECK(f.runner.state() == S::Completed);
  CHECK(f.runner.applyItemSettings(f.itemId,
      {{QStringLiteral("nominalAudioDelayFrames"), 3}}));
  CHECK(f.model.itemState(f.itemId)->settings.nominalAudioDelayFrames == 3);
  return true;
}

bool synchronousFailureDoesNotAdvanceOrRestart() {
  {
    Fixture f;
    CHECK(f.valid);
    f.runtimeHook = [&f] { f.runner.fail(QStringLiteral("runtime rejected by backend")); };
    CHECK(f.runner.start(f.macro({f.step(T::RuntimeSetting), f.step(T::Transport)})));
    f.runner.poll();
    CHECK(f.runner.state() == S::Error);
    CHECK(f.runner.currentStepIndex() == 0);
    f.runner.poll();
    CHECK(f.transports.isEmpty());
  }
  {
    Fixture f;
    CHECK(f.valid);
    f.runtimeHook = [&f] { f.runner.interrupt(QStringLiteral("manual override during dispatch")); };
    CHECK(f.runner.start(f.macro({f.step(T::RuntimeSetting), f.step(T::Transport)})));
    f.runner.poll();
    CHECK(f.runner.state() == S::Paused);
    CHECK(f.runner.currentStepIndex() == 0);
    f.runner.poll();
    CHECK(f.transports.isEmpty());
    f.runtimeHook = {};
    f.runner.resume();
    f.runner.poll();
    CHECK(f.runner.state() == S::Running);
    CHECK(f.runner.currentStepIndex() == 1);
    f.runner.poll();
    CHECK(f.transports.size() == 1);
    f.runner.poll();
    CHECK(f.runner.state() == S::Completed);
  }
  return true;
}

bool lockedSettingsFailPreflight() {
  Fixture f;
  CHECK(f.valid);
  auto settings = f.step(T::ItemSettings);
  settings.settingsPatch = {{QStringLiteral("nominalVolumeTenthsDb"), -100}};
  f.model.setItemSettingsLocked(f.itemId, true);
  CHECK(!f.runner.validate(f.macro({f.step(T::Cue), settings})).isEmpty());
  CHECK(!f.runner.start(f.macro({f.step(T::Cue), settings})));
  CHECK(f.requests.isEmpty());
  f.model.setItemSettingsLocked(f.itemId, false);
  f.model.setOnAirItemId(f.itemId);
  settings.settingsPatch = {{QStringLiteral("inPointMs"), 100}};
  CHECK(!f.runner.validate(f.macro({f.step(T::Cue), settings})).isEmpty());
  CHECK(!f.runner.start(f.macro({f.step(T::Cue), settings})));
  CHECK(f.requests.isEmpty());
  return true;
}

bool knownMediaDurationBoundsSettings() {
  Fixture f;
  CHECK(f.valid);
  auto patch = f.step(T::ItemSettings);
  patch.settingsPatch = {{QStringLiteral("outPointMs"), 1001}};
  // The model and macro preflight must agree once the source length is known.
  f.model.setKnownMediaDuration(f.itemId, 1000);
  auto settings = f.model.itemState(f.itemId)->settings;
  settings.outPointMs = 1001;
  CHECK(!f.model.updateItemSettings(f.itemId, settings));
  CHECK(!f.runner.validate(f.macro({patch})).isEmpty());
  CHECK(!f.runner.start(f.macro({patch})));
  CHECK(f.events.isEmpty());

  patch.settingsPatch = {{QStringLiteral("inPointMs"), 1000}};
  CHECK(!f.runner.validate(f.macro({patch})).isEmpty());
  CHECK(!f.runner.start(f.macro({patch})));
  settings.inPointMs = 999;
  settings.outPointMs = 1000;
  CHECK(f.model.updateItemSettings(f.itemId, settings));
  patch.settingsPatch = {{QStringLiteral("outPointMs"), 1000}};
  CHECK(f.runner.validate(f.macro({patch})).isEmpty());

  // Unknown length retains the existing structural checks and CUE can report
  // any later discoverer failure independently.
  Fixture unknown;
  CHECK(unknown.valid);
  auto unknownSettings = unknown.model.itemState(unknown.itemId)->settings;
  unknownSettings.outPointMs = 1001;
  CHECK(unknown.model.updateItemSettings(unknown.itemId, unknownSettings));
  auto unknownPatch = unknown.step(T::ItemSettings);
  unknownPatch.settingsPatch = {{QStringLiteral("inPointMs"), 1000}};
  CHECK(unknown.runner.validate(unknown.macro({unknownPatch})).isEmpty());

  PlaylistModel imported;
  const quint64 pending = imported.addPendingMediaItem(
      unknown.directory.filePath(QStringLiteral("clip.mov")), 1);
  CHECK(pending != 0);
  CHECK(imported.completePendingMediaItem(pending, PlaylistMediaType::Video, 1000));
  auto importedSettings = imported.itemState(pending)->settings;
  importedSettings.outPointMs = 1001;
  CHECK(!imported.updateItemSettings(pending, importedSettings));
  return true;
}

bool gotoDestinationReplacementPauses() {
  Fixture f;
  CHECK(f.valid);
  const QString destinationPath = f.directory.filePath(QStringLiteral("destination.mov"));
  QFile file(destinationPath);
  CHECK(file.open(QIODevice::WriteOnly));
  CHECK(file.write("destination") == 11);
  file.close();
  PlaylistItemRegistration destination;
  destination.filePath = destinationPath;
  destination.mediaType = PlaylistMediaType::Video;
  const auto added = f.model.addItems({destination});
  CHECK(added.size() == 1);
  auto settings = f.step(T::ItemSettings);
  settings.settingsPatch = {{QStringLiteral("endMode"), QStringLiteral("goto")},
      {QStringLiteral("gotoTargetItemId"), QString::number(added[0])}};
  auto delay = f.step(T::Delay);
  delay.durationMs = 5000;
  CHECK(f.runner.start(f.macro({settings, delay, f.step(T::Cue)})));
  f.runner.poll();
  f.runner.poll();
  CHECK(f.runner.state() == S::Waiting);
  // B is referenced only as A's GOTO destination, never as a direct step target.
  destination.filePath = f.model.itemState(f.itemId)->filePath;
  CHECK(f.model.replaceItemAtomically(added[0], {destination}).size() == 1);
  CHECK(f.runner.state() == S::Paused);
  CHECK(f.requests.isEmpty());
  return true;
}

bool synchronousCompletionRejectionAndTimeout() {
  {
    Fixture f;
    CHECK(f.valid);
    f.synchronousCue = true;
    f.synchronousTake = true;
    CHECK(f.runner.start(f.macro({f.step(T::Cue), f.step(T::Take)})));
    f.runner.poll();
    CHECK(f.runner.currentStepIndex() == 1);
    f.runner.poll();
    CHECK(f.runner.currentStepIndex() == 2);
    f.runner.poll();
    CHECK(f.runner.state() == S::Completed);
    CHECK(f.requests.size() == 2);
  }
  {
    Fixture f;
    CHECK(f.valid);
    f.acceptCue = false;
    CHECK(f.runner.start(f.macro({f.step(T::Cue), f.step(T::Take)})));
    f.runner.poll();
    CHECK(f.runner.state() == S::Error);
    CHECK(f.requests.size() == 1);
  }
  {
    Fixture f;
    CHECK(f.valid);
    CHECK(f.runner.start(f.macro({f.step(T::Cue), f.step(T::Take)})));
    f.runner.poll();
    for (int i = 0; i < 29; ++i) f.tick(1000);
    CHECK(f.runner.state() == S::Waiting);
    f.tick(1000);
    CHECK(f.runner.state() == S::Error);
    f.runner.finishRequest(f.requests[0].id, true, {});
    f.runner.poll();
    CHECK(f.requests.size() == 1);
  }
  return true;
}

bool manualWaitRequiresExplicitResume() {
  Fixture f;
  CHECK(f.valid);
  CHECK(f.runner.start(f.macro({f.step(T::ManualWait), f.step(T::Transport)})));
  f.runner.poll();
  CHECK(f.runner.state() == S::Paused);
  f.tick(1000);
  CHECK(f.transports.isEmpty());
  f.runner.resume();
  CHECK(f.runner.currentStepIndex() == 1);
  f.runner.poll();
  CHECK(f.transports.size() == 1);
  f.runner.poll();
  CHECK(f.runner.state() == S::Completed);
  return true;
}
} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  if (!sequenceAndRequestCorrelation() || !delayPauseResume() ||
      !stoppedAndCancelledResponsesStayInert() || !schedulesAndLateness() ||
      !clockChangeAndExpiredResume() || !manualOperationHasPriority() ||
      !rejectedInputAndMacroSourceDoNotInterrupt() ||
      !missingForeignAndChangedTargets() || !modelResetStopsOutstandingRequest() ||
      !ownSettingsChangesAndPredictedPatches() || !synchronousFailureDoesNotAdvanceOrRestart() ||
      !lockedSettingsFailPreflight() || !knownMediaDurationBoundsSettings() ||
      !gotoDestinationReplacementPauses() ||
      !synchronousCompletionRejectionAndTimeout() || !manualWaitRequiresExplicitResume()) return 1;
  return 0;
}
