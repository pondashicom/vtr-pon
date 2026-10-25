#include "playlist_item_playout_state_machine.h"

#include "playlist_model.h"

#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <thread>

namespace {

bool check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  qCritical().nospace() << "line " << line << ": check failed: " << expression;
  return false;
}

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!check((expression), #expression, __LINE__)) {                          \
      return 1;                                                                 \
    }                                                                           \
  } while (false)

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 2000) {
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  return condition();
}

int checkAsyncAvailability(const QString &filePath) {
  struct ProbeState {
    std::atomic_bool hold{true};
    std::atomic_bool available{false};
    std::atomic_bool offGuiThread{true};
    std::atomic_int calls{0};
    std::atomic_int returned{0};
  };
  const auto guiThread = std::this_thread::get_id();
  const auto makeProbe = [guiThread](const std::shared_ptr<ProbeState> &state) {
    return [state, guiThread](const QString &) {
      const bool result = state->available.load();
      if (std::this_thread::get_id() == guiThread) state->offGuiThread = false;
      ++state->calls;
      QElapsedTimer limit;
      limit.start();
      while (state->hold && limit.elapsed() < 5000) QThread::msleep(1);
      ++state->returned;
      return result;
    };
  };
  PlaylistItemRegistration registration;
  registration.filePath = filePath;
  auto state = std::make_shared<ProbeState>();
  PlaylistModel model(nullptr, makeProbe(state));
  int availabilityRequests = 0;
  QObject::connect(&model, &PlaylistModel::mediaAvailabilityCheckRequested,
                   [&] { ++availabilityRequests; });
  model.refreshMediaAvailability();
  CHECK(availabilityRequests == 1); // Device checks also run with an empty list.
  const auto ids = model.addItems({registration, registration});
  CHECK(ids.size() == 2);
  PlaylistItemRegistration uvc;
  uvc.sourceType = PlaylistSourceType::Uvc;
  uvc.filePath = QStringLiteral("test-camera");
  uvc.displayName = QStringLiteral("Camera");
  CHECK(model.addItems({uvc}).size() == 1);
  int changes = 0;
  bool guiDelivery = true;
  QObject::connect(&model, &PlaylistModel::itemMediaAvailabilityChanged, &model,
                   [&](quint64, bool) {
    ++changes;
    guiDelivery &= std::this_thread::get_id() == guiThread;
  });
  QElapsedTimer elapsed;
  elapsed.start();
  model.refreshMediaAvailability();
  CHECK(elapsed.elapsed() < 100);
  CHECK(waitFor([&] { return state->calls == 1; }));
  for (int index = 0; index < 100; ++index) model.refreshMediaAvailability();
  CHECK(availabilityRequests == 102); // A stalled file probe cannot starve devices.
  int ticks = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
  heartbeat.start(5);
  CHECK(waitFor([&] { return ticks >= 10; }));
  CHECK(model.setPlaylistName(1, QStringLiteral("Still responsive")));
  CHECK(state->calls == 1);
  CHECK(changes == 0);
  CHECK(!model.itemMediaOffline(ids[0]));
  state->hold = false;
  CHECK(waitFor([&] { return model.itemMediaOffline(ids[0]) && state->returned == 2; }));
  CHECK(model.itemMediaOffline(ids[1]));
  CHECK(changes == 2);
  CHECK(guiDelivery && state->offGuiThread);
  // Two shared-path rows and a UVC row require only one probe per scan;
  // the hundred requests coalesce into one additional scan.
  state->available = true;
  model.refreshMediaAvailability();
  CHECK(waitFor([&] { return !model.itemMediaOffline(ids[0]); }));
  CHECK(changes == 4);

  // Reusing an ID, even with the same path, must reject the old result.
  for (int operation = 0; operation < 4; ++operation) {
    auto held = std::make_shared<ProbeState>();
    PlaylistModel changing(nullptr, makeProbe(held));
    const auto itemIds = changing.addItems({registration});
    const auto before = changing.snapshot();
    int staleChanges = 0;
    QObject::connect(&changing, &PlaylistModel::itemMediaAvailabilityChanged,
                     &changing, [&](quint64, bool) { ++staleChanges; });
    changing.refreshMediaAvailability();
    CHECK(waitFor([&] { return held->calls == 1; }));
    if (operation == 0) {
      CHECK(changing.restoreSnapshot(before));
    } else if (operation == 1) {
      CHECK(changing.replaceItemAtomically(itemIds[0], {registration}) == itemIds);
    } else {
      CHECK(changing.removeItem(itemIds[0]));
      if (operation == 3) {
        changing.rememberBulkOperation(before, 1);
        CHECK(changing.undoBulkOperation());
        CHECK(changing.itemMediaOffline(itemIds[0]));
      }
    }
    held->available = true;
    held->hold = false;
    if (operation != 2) {
      CHECK(waitFor([&] { return held->returned == 2; }));
      CHECK(waitFor([&] { return !changing.itemMediaOffline(itemIds[0]); }));
    } else {
      CHECK(waitFor([&] { return held->returned == 1; }));
      CHECK(changing.itemCount() == 0);
    }
    CHECK(staleChanges == (operation == 3 ? 1 : 0));
  }

  // Undo of Clear must return while the file probe is blocked. Restored rows
  // are unavailable before itemAdded; live-source health remains independent.
  auto undoProbe = std::make_shared<ProbeState>();
  undoProbe->available = true;
  PlaylistModel undoModel(nullptr, makeProbe(undoProbe));
  const auto undoIds = undoModel.addItems({registration, uvc});
  CHECK(undoIds.size() == 2);
  const auto beforeClear = undoModel.snapshot();
  for (const auto id : undoIds) CHECK(undoModel.removeItem(id));
  undoModel.rememberBulkOperation(beforeClear, 1);
  int restoredCount = 0;
  bool offlineWhenAdded = true;
  int undoAvailabilityRequests = 0;
  QObject::connect(&undoModel, &PlaylistModel::itemAdded, &undoModel,
                   [&](quint64 id) {
    ++restoredCount;
    offlineWhenAdded &= undoModel.itemMediaOffline(id);
  });
  QObject::connect(&undoModel, &PlaylistModel::mediaAvailabilityCheckRequested,
                   &undoModel, [&] { ++undoAvailabilityRequests; });
  elapsed.restart();
  CHECK(undoModel.undoBulkOperation());
  CHECK(elapsed.elapsed() < 100);
  CHECK(undoModel.snapshot() == beforeClear);
  CHECK(restoredCount == 2 && offlineWhenAdded);
  CHECK(undoAvailabilityRequests == 1);
  CHECK(waitFor([&] { return undoProbe->calls == 1; }));
  CHECK(undoModel.itemMediaOffline(undoIds[0]));
  CHECK(undoModel.itemMediaOffline(undoIds[1]));
  const int ticksBeforeUndoProbe = ticks;
  CHECK(waitFor([&] { return ticks >= ticksBeforeUndoProbe + 10; }));
  CHECK(undoModel.setPlaylistName(1, QStringLiteral("Responsive after Undo")));
  undoProbe->hold = false;
  CHECK(waitFor([&] { return !undoModel.itemMediaOffline(undoIds[0]); }));
  CHECK(undoProbe->returned == 1 && undoProbe->offGuiThread);
  CHECK(undoModel.itemMediaOffline(undoIds[1]));
  undoModel.setUvcDeviceAvailable(uvc.filePath, true);
  CHECK(!undoModel.itemMediaOffline(undoIds[1]));
  undoProbe->available = false;
  undoModel.refreshMediaAvailability();
  CHECK(waitFor([&] { return undoModel.itemMediaOffline(undoIds[0]); }));
  CHECK(!undoModel.itemMediaOffline(undoIds[1]));

  // Destruction must not join a worker blocked in an OS filesystem call.
  auto held = std::make_shared<ProbeState>();
  auto closing = std::make_unique<PlaylistModel>(nullptr, makeProbe(held));
  CHECK(closing->addItems({registration}).size() == 1);
  closing->refreshMediaAvailability();
  CHECK(waitFor([&] { return held->calls == 1; }));
  elapsed.restart();
  closing.reset();
  CHECK(elapsed.elapsed() < 100);
  held->hold = false;
  CHECK(waitFor([&] { return held->returned == 1; }));
  qInfo() << "Async availability: responsive event loop, coalescing, shared paths,"
             " stale result rejection and nonblocking destruction passed";
  return 0;
}

int checkBulkUndo(const QString &filePath) {
  PlaylistItemRegistration registration;
  registration.filePath = filePath;
  registration.settings.outPointMs = 3000;
  registration.settings.endMode = PlaylistEndMode::Stop;

  PlaylistItemRegistration pdf;
  pdf.filePath = filePath;
  pdf.sourceType = PlaylistSourceType::Pdf;
  pdf.mediaType = PlaylistMediaType::Pdf;
  PlaylistItemRegistration powerPoint;
  powerPoint.filePath = filePath;
  powerPoint.sourceType = PlaylistSourceType::PowerPoint;
  powerPoint.mediaType = PlaylistMediaType::Presentation;
  powerPoint.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
  PlaylistItemRegistration uvc;
  uvc.filePath = QStringLiteral("bulk-undo-camera");
  uvc.displayName = QStringLiteral("Camera");
  uvc.sourceType = PlaylistSourceType::Uvc;

  PlaylistModel liveSettingsModel;
  const auto liveIds = liveSettingsModel.addItems({uvc});
  CHECK(liveIds.size() == 1);
  const quint64 liveId = liveIds.first();
  auto liveSettings = liveSettingsModel.itemState(liveId)->settings;
  CHECK(liveSettings.startMode == PlaylistStartMode::Play);
  liveSettings.fadeInEnabled = true;
  liveSettings.nominalVolumeTenthsDb = -120;
  liveSettings.nominalEqTenthsDb = {20, -10, 30};
  CHECK(liveSettingsModel.updateItemSettings(liveId, liveSettings));
  CHECK(liveSettingsModel.itemState(liveId)->settings == liveSettings);
  auto invalidLiveSettings = liveSettings;
  invalidLiveSettings.startMode = PlaylistStartMode::Pause;
  CHECK(!liveSettingsModel.updateItemSettings(liveId, invalidLiveSettings));
  invalidLiveSettings = liveSettings;
  invalidLiveSettings.endMode = PlaylistEndMode::Next;
  CHECK(!liveSettingsModel.updateItemSettings(liveId, invalidLiveSettings));
  invalidLiveSettings = liveSettings;
  invalidLiveSettings.inPointMs = 100;
  CHECK(!liveSettingsModel.updateItemSettings(liveId, invalidLiveSettings));
  invalidLiveSettings = liveSettings;
  invalidLiveSettings.nominalAudioDelayFrames = 1;
  CHECK(!liveSettingsModel.updateItemSettings(liveId, invalidLiveSettings));
  liveSettingsModel.setItemSettingsLocked(liveId, true);
  liveSettings.fadeInEnabled = false;
  CHECK(!liveSettingsModel.updateItemSettings(liveId, liveSettings));

  // Native-source individual edit restrictions must not prevent reversing
  // bulk settings, shuffled order, or mode-only changes in a mixed playlist.
  for (const auto operation : {PlaylistBulkPlaybackOperation::PlayOnce,
                               PlaylistBulkPlaybackOperation::RepeatAll,
                               PlaylistBulkPlaybackOperation::Shuffle,
                               PlaylistBulkPlaybackOperation::BgmMode,
                               PlaylistBulkPlaybackOperation::StandardMode}) {
    PlaylistModel model;
    const auto ids = model.addItems({registration, pdf, powerPoint, uvc});
    CHECK(ids.size() == 4);
    for (int index = 1; index < ids.size(); ++index) {
      CHECK(model.itemSettingsLocked(ids[index]) == (index != 3));
      CHECK(!model.updateItemSettings(ids[index], registration.settings));
    }
    if (operation == PlaylistBulkPlaybackOperation::StandardMode) {
      CHECK(model.setPlaylistMode(1, PlaylistMode::Bgm));
    }
    const auto before = model.snapshot();
    CHECK(model.applyBulkPlaybackOperation(1, operation));
    model.rememberBulkOperation(before, 1);
    CHECK(model.hasBulkUndo());
    bool keptLaterChanges = true;
    CHECK(model.undoBulkOperation(nullptr, &keptLaterChanges));
    CHECK(!keptLaterChanges);
    CHECK(model.snapshot() == before);
    CHECK(!model.hasBulkUndo());
  }

  // Explicit locks still prevent bulk changes for every source type, while
  // an in-progress import must not be partially changed or reordered.
  PlaylistModel guarded;
  const auto guardedIds = guarded.addItems({registration, pdf, powerPoint, uvc});
  CHECK(guardedIds.size() == 4);
  for (const auto id : guardedIds) {
    guarded.setItemSettingsLocked(id, true);
    const auto before = guarded.snapshot();
    CHECK(!guarded.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::Shuffle));
    CHECK(guarded.snapshot() == before);
    guarded.setItemSettingsLocked(id, false);
  }
  const auto beforeGuarded = guarded.snapshot();
  CHECK(guarded.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::Shuffle));
  guarded.rememberBulkOperation(beforeGuarded, 1);
  guarded.setItemSettingsLocked(guardedIds[1], true);
  const auto lockedOrder = guarded.snapshot();
  bool keptGuardedChanges = false;
  CHECK(guarded.undoBulkOperation(nullptr, &keptGuardedChanges));
  CHECK(keptGuardedChanges);
  for (int row = 0; row < lockedOrder.items.size(); ++row) {
    CHECK(guarded.itemIdAt(row) == lockedOrder.items[row].id);
  }
  CHECK(guarded.itemState(guardedIds[1])->settings ==
        lockedOrder.items[guarded.indexOf(guardedIds[1])].settings);
  CHECK(guarded.itemState(guardedIds[2])->settings == powerPoint.settings);
  guarded.setItemSettingsLocked(guardedIds[1], false);
  const auto pending = guarded.addPendingMediaItem(filePath, 1);
  CHECK(pending != 0);
  const auto importing = guarded.snapshot();
  CHECK(!guarded.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::Shuffle));
  CHECK(guarded.snapshot() == importing);

  PlaylistModel model;
  const auto ids = model.addItems({registration, registration, registration});
  CHECK(ids.size() == 3);
  const int secondPlaylist = model.createPlaylist();
  CHECK(model.setPlaylistName(secondPlaylist, QStringLiteral("AUDIT-2")));
  const auto before = model.snapshot();
  CHECK(model.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::PlayOnce));
  model.rememberBulkOperation(before, 1);
  // Repeating a no-op retains the preceding real operation.
  const auto noOp = model.snapshot();
  CHECK(model.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::PlayOnce));
  model.rememberBulkOperation(noOp, 1);
  CHECK(model.setPlaylistName(secondPlaylist, QStringLiteral("AUDIT-2-EDIT-AFTER-BULK")));
  CHECK(model.setPlaylistName(1, QStringLiteral("Also keep this name")));
  auto settings = model.itemState(ids[0])->settings;
  settings.inPointMs = 250;
  settings.outPointMs = 2500;
  settings.nominalVolumeTenthsDb = -120;
  settings.fadeInEnabled = true;
  settings.fadeOutEnabled = true;
  settings.endCrossfadeEnabled = true;
  settings.repeatCount = 7;
  settings.repeatEndMode = PlaylistRepeatEndMode::Goto;
  settings.gotoTargetItemId = ids[2];
  CHECK(model.updateItemSettings(ids[0], settings));
  CHECK(model.setItemDisplayName(ids[0], QStringLiteral("Later name")));
  CHECK(model.setItemColor(ids[0], PlaylistItemColor::Red));
  CHECK(model.setItemSeparatorBelow(ids[0], true));
  CHECK(model.moveItemToPlaylist(ids[0], secondPlaylist, 0));
  const auto added = model.addItems({registration});
  CHECK(added.size() == 1);
  CHECK(model.setActivePlaylistNumber(secondPlaylist));
  model.setCueItemState(ids[1], PlaylistCueState::Ready);
  model.setEditItemId(ids[0]);
  model.setItemCurrentPlayCount(ids[0], 4);
  int resets = 0;
  QObject::connect(&model, &PlaylistModel::modelReset, &model, [&] { ++resets; });
  CHECK(model.undoBulkOperation());
  CHECK(model.playlistName(secondPlaylist) == QStringLiteral("AUDIT-2-EDIT-AFTER-BULK"));
  CHECK(model.playlistName(1) == QStringLiteral("Also keep this name"));
  CHECK(model.activePlaylistNumber() == secondPlaylist);
  CHECK(model.itemState(added[0]).has_value());
  const auto retained = model.itemState(ids[0]);
  CHECK(retained->displayName == QStringLiteral("Later name"));
  CHECK(retained->color == PlaylistItemColor::Red && retained->separatorBelow);
  CHECK(retained->playlistNumber == secondPlaylist);
  settings.startMode = registration.settings.startMode;
  settings.endMode = registration.settings.endMode;
  CHECK(retained->settings == settings);
  CHECK(model.cueItemId() == ids[1] && model.editItemId() == ids[0]);
  CHECK(model.itemCurrentPlayCount(ids[0]) == 4);
  CHECK(resets == 0);

  // Reproduce Play Once -> item 2 END GOTO -> Undo from the user's log.
  // END/GOTO stay edited, while START and the other items are restored.
  PlaylistModel conflict;
  const auto conflictIds = conflict.addItems({registration, registration, registration});
  const auto conflictBefore = conflict.snapshot();
  CHECK(conflict.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::PlayOnce));
  conflict.rememberBulkOperation(conflictBefore, 1);
  auto edited = conflict.itemState(conflictIds[1])->settings;
  edited.endMode = PlaylistEndMode::Goto;
  edited.gotoTargetItemId = conflictIds[0];
  CHECK(conflict.updateItemSettings(conflictIds[1], edited));
  const auto conflictingState = conflict.snapshot();
  QString error;
  conflict.setOnAirItemId(conflictIds[0]);
  CHECK(!conflict.undoBulkOperation(&error));
  CHECK(!error.isEmpty() && conflict.snapshot() == conflictingState);
  CHECK(conflict.hasBulkUndo());
  conflict.clearOnAirItem();
  bool keptLaterChanges = false;
  CHECK(conflict.undoBulkOperation(&error, &keptLaterChanges));
  CHECK(error.isEmpty() && keptLaterChanges && !conflict.hasBulkUndo());
  edited.startMode = registration.settings.startMode;
  CHECK(conflict.itemState(conflictIds[1])->settings == edited);
  CHECK(conflict.itemState(conflictIds[0])->settings == registration.settings);
  CHECK(conflict.itemState(conflictIds[2])->settings == registration.settings);

  // A locked or deleted target is skipped without blocking other targets or
  // emitting notifications for missing/unchanged items.
  PlaylistModel locked;
  const auto lockedIds = locked.addItems({registration, registration, registration});
  const auto beforeLocked = locked.snapshot();
  CHECK(locked.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::RepeatAll));
  locked.rememberBulkOperation(beforeLocked, 1);
  locked.setItemSettingsLocked(lockedIds[0], true);
  const auto lockedSettings = locked.itemState(lockedIds[0])->settings;
  CHECK(locked.removeItem(lockedIds[1]));
  QList<quint64> notified;
  QObject::connect(&locked, &PlaylistModel::itemSettingsChanged, &locked,
                   [&](quint64 id) { notified.append(id); });
  CHECK(locked.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges && !locked.itemState(lockedIds[1]));
  CHECK(locked.itemState(lockedIds[0])->settings == lockedSettings);
  CHECK(locked.itemState(lockedIds[2])->settings == registration.settings);
  CHECK(notified == QList<quint64>{lockedIds[2]});

  // If every change is already superseded, finish without changing the model.
  PlaylistModel modeConflict;
  CHECK(modeConflict.setPlaylistMode(1, PlaylistMode::Bgm));
  const auto beforeMode = modeConflict.snapshot();
  CHECK(modeConflict.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::StandardMode));
  modeConflict.rememberBulkOperation(beforeMode, 1);
  CHECK(modeConflict.setPlaylistMode(1, PlaylistMode::Bgm));
  CHECK(modeConflict.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges && !modeConflict.hasBulkUndo());
  CHECK(modeConflict.snapshot() == beforeMode);

  PlaylistModel shuffle;
  const auto shuffleIds = shuffle.addItems({registration, registration, registration});
  const auto shuffleBefore = shuffle.snapshot();
  CHECK(shuffle.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::Shuffle));
  shuffle.rememberBulkOperation(shuffleBefore, 1);
  const auto inserted = shuffle.addItems({registration});
  CHECK(shuffle.moveItemTo(inserted[0], 1));
  CHECK(shuffle.undoBulkOperation());
  CHECK(shuffle.itemIdAt(0) == shuffleIds[0]);
  CHECK(shuffle.itemIdAt(1) == inserted[0]);
  CHECK(shuffle.itemIdAt(2) == shuffleIds[1]);
  CHECK(shuffle.itemIdAt(3) == shuffleIds[2]);
  const auto reshuffleBefore = shuffle.snapshot();
  CHECK(shuffle.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::Shuffle));
  shuffle.rememberBulkOperation(reshuffleBefore, 1);
  CHECK(shuffle.moveItemTo(shuffle.itemIdAt(0), 2));
  const auto manuallyMoved = shuffle.snapshot();
  CHECK(shuffle.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges);
  for (int row = 0; row < manuallyMoved.items.size(); ++row) {
    CHECK(shuffle.itemIdAt(row) == manuallyMoved.items[row].id);
  }
  for (const auto &item : reshuffleBefore.items) {
    CHECK(shuffle.itemState(item.id)->settings == item.settings);
  }

  // Clear also owns the GOTO references that removal cleared in other lists.
  PlaylistModel clear;
  const auto clearedIds = clear.addItems({registration, registration});
  const int other = clear.createPlaylist();
  registration.playlistNumber = other;
  registration.settings.gotoTargetItemId = clearedIds[0];
  const auto references = clear.addItems({registration, registration});
  const auto clearBefore = clear.snapshot();
  for (const auto id : clearedIds) CHECK(clear.removeItem(id));
  clear.rememberBulkOperation(clearBefore, 1);
  CHECK(clear.itemState(references[0])->settings.gotoTargetItemId == 0);
  registration.playlistNumber = 1;
  registration.settings.gotoTargetItemId = 0;
  const auto afterClear = clear.addItems({registration});
  CHECK(afterClear[0] > references[1]);
  CHECK(clear.setPlaylistName(other, QStringLiteral("Keep after Clear")));
  CHECK(clear.setActivePlaylistNumber(other));
  CHECK(clear.undoBulkOperation());
  CHECK(clear.itemIdAt(1, 0) == clearedIds[0]);
  CHECK(clear.itemIdAt(1, 1) == clearedIds[1]);
  CHECK(clear.itemIdAt(1, 2) == afterClear[0]);
  CHECK(clear.itemState(references[0])->settings.gotoTargetItemId == clearedIds[0]);
  CHECK(clear.activePlaylistNumber() == other);
  CHECK(clear.playlistName(other) == QStringLiteral("Keep after Clear"));
  CHECK(clear.addItems({registration})[0] > afterClear[0]);
  CHECK(clear.canRestoreSnapshot(clear.snapshot()));

  const auto clearAgainBefore = clear.snapshot();
  const auto target = clear.itemIdAt(1, 0);
  while (clear.itemCount(1) > 0) CHECK(clear.removeItem(clear.itemIdAt(1, 0)));
  clear.rememberBulkOperation(clearAgainBefore, 1);
  auto gotoEdited = clear.itemState(references[0])->settings;
  gotoEdited.gotoTargetItemId = references[1];
  CHECK(clear.updateItemSettings(references[0], gotoEdited));
  CHECK(clear.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges && clear.itemState(target));
  CHECK(clear.itemState(references[0])->settings == gotoEdited);
  CHECK(clear.itemState(references[1])->settings.gotoTargetItemId == target);

  // Restoring cleared items must not resurrect another subsequently deleted row
  // merely because it used to be their GOTO destination.
  const auto beforeMissingGoto = clear.snapshot();
  CHECK(clear.removeItem(references[0]));
  CHECK(clear.removeItem(references[1]));
  clear.rememberBulkOperation(beforeMissingGoto, other);
  CHECK(clear.removeItem(target));
  CHECK(clear.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges && !clear.itemState(target));
  CHECK(clear.itemState(references[0])->settings.gotoTargetItemId == references[1]);
  CHECK(clear.itemState(references[1])->settings.gotoTargetItemId == 0);
  CHECK(clear.canRestoreSnapshot(clear.snapshot()));

  // Same-ID replacement and document import must never revive stale history.
  PlaylistModel replacement;
  const auto replacementIds = replacement.addItems({registration, registration});
  const auto replacementBefore = replacement.snapshot();
  CHECK(replacement.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::PlayOnce));
  replacement.rememberBulkOperation(replacementBefore, 1);
  registration.settings = replacement.itemState(replacementIds[0])->settings;
  CHECK(!replacement.replaceItemAtomically(replacementIds[0], {registration}).isEmpty());
  const auto replaced = replacement.snapshot();
  CHECK(replacement.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges);
  CHECK(replacement.itemState(replacementIds[0]) == replaced.items[0]);
  CHECK(replacement.itemState(replacementIds[1])->settings == replacementBefore.items[1].settings);
  const auto beforeImport = replacement.snapshot();
  CHECK(replacement.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::BgmMode));
  replacement.rememberBulkOperation(beforeImport, 1);
  CHECK(replacement.hasBulkUndo());
  CHECK(replacement.restoreSnapshot(beforeImport));
  CHECK(!replacement.hasBulkUndo());
  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);

  QTemporaryFile mediaFile;
  CHECK(mediaFile.open());
  CHECK(checkAsyncAvailability(mediaFile.fileName()) == 0);
  CHECK(checkBulkUndo(mediaFile.fileName()) == 0);

  PlaylistModel model;
  std::optional<PlaylistItemState> stateWhenAdded;
  QObject::connect(&model, &PlaylistModel::itemAdded, &model,
                   [&model, &stateWhenAdded](quint64 itemId) {
                     stateWhenAdded = model.itemState(itemId);
                   });
  PlaylistItemRegistration registration;
  registration.filePath = mediaFile.fileName();
  registration.settings.outPointMs = 3000;
  model.addItems(QList<PlaylistItemRegistration>{registration});
  CHECK(model.itemCount() == 1);
  const quint64 itemId = model.itemIdAt(0);
  CHECK(itemId != 0);
  CHECK(stateWhenAdded.has_value());
  CHECK(stateWhenAdded->itemNumber == 1);
  CHECK(stateWhenAdded->playlistNumber == 1);
  CHECK(stateWhenAdded->settings.outPointMs == 3000);
  CHECK(model.itemState(itemId)->itemNumber == 1);
  CHECK(model.itemState(itemId)->playlistNumber == 1);
  CHECK(model.itemState(itemId)->settings.outPointMs == 3000);

  model.setOnAirItemId(itemId);
  CHECK(model.itemCurrentPlayCount(itemId) == 0);
  model.setItemCurrentPlayCount(itemId, 2);
  CHECK(model.itemCurrentPlayCount(itemId) == 2);
  CHECK(!model.removeItem(itemId));
  CHECK(model.itemCount() == 1);
  model.clearOnAirItem();
  CHECK(model.itemCurrentPlayCount(itemId) == 0);

  PlaylistItemSettings initialSettings;
  initialSettings.inPointMs = 100;
  initialSettings.outPointMs = 1000;
  initialSettings.startMode = PlaylistStartMode::Pause;
  initialSettings.endMode = PlaylistEndMode::Pause;
  initialSettings.fadeInEnabled = true;
  initialSettings.fadeInDurationMs = 500;
  initialSettings.fadeOutEnabled = true;
  initialSettings.fadeOutDurationMs = 700;
  initialSettings.endCrossfadeEnabled = true;
  initialSettings.endCrossfadeDurationMs = 650;
  initialSettings.nominalVolumeTenthsDb = -120;
  CHECK(model.updateItemSettings(itemId, initialSettings));
  CHECK(model.itemState(itemId)->settings.fadeInEnabled);
  CHECK(model.itemState(itemId)->settings.fadeInDurationMs == 500);
  CHECK(model.itemState(itemId)->settings.fadeOutEnabled);
  CHECK(model.itemState(itemId)->settings.fadeOutDurationMs == 700);
  CHECK(model.itemState(itemId)->settings.endCrossfadeEnabled);
  CHECK(model.itemState(itemId)->settings.endCrossfadeDurationMs == 650);
  CHECK(model.itemState(itemId)->settings.repeatCount ==
        kInfiniteItemRepeatCount);
  CHECK(model.itemState(itemId)->settings.repeatEndMode ==
        PlaylistRepeatEndMode::Pause);
  CHECK(model.itemState(itemId)->settings.nominalVolumeTenthsDb == -120);
  PlaylistItemSettings invalidGotoSettings = initialSettings;
  invalidGotoSettings.gotoTargetItemId = 999999;
  CHECK(!model.updateItemSettings(itemId, invalidGotoSettings));

  PlaylistItemSettings invalidFadeSettings = initialSettings;
  invalidFadeSettings.fadeInDurationMs = kMinimumItemFadeDurationMs - 1;
  CHECK(!model.updateItemSettings(itemId, invalidFadeSettings));
  invalidFadeSettings = initialSettings;
  invalidFadeSettings.fadeOutDurationMs = kMaximumItemFadeDurationMs + 1;
  CHECK(!model.updateItemSettings(itemId, invalidFadeSettings));
  invalidFadeSettings = initialSettings;
  invalidFadeSettings.endCrossfadeDurationMs =
      kMinimumItemFadeDurationMs - 1;
  CHECK(!model.updateItemSettings(itemId, invalidFadeSettings));
  PlaylistItemSettings invalidRepeatSettings = initialSettings;
  invalidRepeatSettings.repeatCount = kMaximumItemRepeatCount + 1;
  CHECK(!model.updateItemSettings(itemId, invalidRepeatSettings));
  PlaylistItemSettings invalidVolumeSettings = initialSettings;
  invalidVolumeSettings.nominalVolumeTenthsDb =
      kMinimumItemVolumeTenthsDb - 1;
  CHECK(!model.updateItemSettings(itemId, invalidVolumeSettings));
  invalidVolumeSettings = initialSettings;
  invalidVolumeSettings.nominalVolumeTenthsDb =
      kMaximumItemVolumeTenthsDb + 1;
  CHECK(!model.updateItemSettings(itemId, invalidVolumeSettings));

  PlaylistItemSettings lockedSettings = initialSettings;
  lockedSettings.inPointMs = 200;
  lockedSettings.outPointMs = 2000;
  model.setOnAirItemId(itemId);
  CHECK(model.itemEditPointsLocked(itemId));
  CHECK(!model.itemSettingsLocked(itemId));
  CHECK(!model.updateItemSettings(itemId, lockedSettings));

  PlaylistItemSettings editableOnAirSettings = initialSettings;
  editableOnAirSettings.startMode = PlaylistStartMode::Play;
  editableOnAirSettings.endMode = PlaylistEndMode::Stop;
  editableOnAirSettings.fadeInEnabled = false;
  editableOnAirSettings.fadeOutDurationMs = 900;
  editableOnAirSettings.endCrossfadeDurationMs = 850;
  editableOnAirSettings.nominalVolumeTenthsDb = -60;
  CHECK(model.updateItemSettings(itemId, editableOnAirSettings));
  CHECK(model.itemState(itemId)->settings == editableOnAirSettings);
  model.clearOnAirItem();
  model.setItemSettingsLocked(itemId, true);
  CHECK(model.itemEditPointsLocked(itemId));
  CHECK(model.itemSettingsLocked(itemId));
  CHECK(!model.updateItemSettings(itemId, lockedSettings));
  model.setItemSettingsLocked(itemId, false);
  CHECK(!model.itemEditPointsLocked(itemId));
  CHECK(!model.itemSettingsLocked(itemId));

  CHECK(model.updateItemSettings(itemId, initialSettings));

  PlaylistItemSettings finiteRepeatSettings = initialSettings;
  finiteRepeatSettings.endMode = PlaylistEndMode::Repeat;
  finiteRepeatSettings.repeatCount = 3;
  finiteRepeatSettings.repeatEndMode = PlaylistRepeatEndMode::Goto;
  finiteRepeatSettings.gotoTargetItemId = itemId;
  CHECK(model.updateItemSettings(itemId, finiteRepeatSettings));
  PlaylistItemPlayoutStateMachine repeatMachine(&model);
  CHECK(repeatMachine.prepare(itemId));
  CHECK(repeatMachine.currentPlayCount() == 0);
  CHECK(repeatMachine.start() == PlaylistStartMode::Pause);
  CHECK(repeatMachine.currentPlayCount() == 1);
  CHECK(repeatMachine.endModeForCurrentIteration() == PlaylistEndMode::Repeat);
  CHECK(repeatMachine.finish() == PlaylistEndMode::Repeat);
  CHECK(repeatMachine.currentPlayCount() == 2);
  CHECK(repeatMachine.endModeForCurrentIteration() == PlaylistEndMode::Repeat);
  CHECK(repeatMachine.finish() == PlaylistEndMode::Repeat);
  CHECK(repeatMachine.currentPlayCount() == 3);
  CHECK(repeatMachine.endModeForCurrentIteration() == PlaylistEndMode::Goto);
  CHECK(repeatMachine.finish() == PlaylistEndMode::Goto);
  CHECK(repeatMachine.currentPlayCount() == 3);
  CHECK(repeatMachine.phase() == PlaylistItemPlayoutPhase::Ended);
  CHECK(repeatMachine.markActive());
  CHECK(repeatMachine.currentPlayCount() == 1);
  CHECK(repeatMachine.endModeForCurrentIteration() == PlaylistEndMode::Repeat);

  finiteRepeatSettings.repeatCount = 1;
  finiteRepeatSettings.repeatEndMode = PlaylistRepeatEndMode::Pause;
  CHECK(model.updateItemSettings(itemId, finiteRepeatSettings));
  CHECK(repeatMachine.endModeForCurrentIteration() == PlaylistEndMode::Pause);
  CHECK(repeatMachine.finish() == PlaylistEndMode::Pause);
  CHECK(repeatMachine.currentPlayCount() == 1);
  CHECK(repeatMachine.phase() == PlaylistItemPlayoutPhase::Ended);

  CHECK(model.updateItemSettings(itemId, initialSettings));

  PlaylistItemPlayoutStateMachine machine(&model);
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Empty);
  CHECK(!machine.start());
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Empty);

  CHECK(machine.prepare(itemId));
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Prepared);
  const PlaylistPreparedMedia *prepared = machine.preparedMedia();
  CHECK(prepared != nullptr);
  CHECK(prepared->itemId == itemId);
  CHECK(prepared->filePath == mediaFile.fileName());
  CHECK(prepared->inPointMs == 100);
  CHECK(prepared->outPointMs == 1000);

  PlaylistItemSettings changedAfterPrepare = initialSettings;
  changedAfterPrepare.inPointMs = 200;
  changedAfterPrepare.outPointMs = 2000;
  changedAfterPrepare.startMode = PlaylistStartMode::Play;
  changedAfterPrepare.endMode = PlaylistEndMode::Repeat;
  CHECK(model.updateItemSettings(itemId, changedAfterPrepare));

  const auto startMode = machine.start();
  CHECK(startMode == PlaylistStartMode::Play);
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Active);
  CHECK(machine.preparedMedia()->inPointMs == 100);
  CHECK(machine.preparedMedia()->outPointMs == 1000);

  const auto repeatMode = machine.finish();
  CHECK(repeatMode == PlaylistEndMode::Repeat);
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Active);

  PlaylistItemSettings changedBeforeFinish = changedAfterPrepare;
  changedBeforeFinish.endMode = PlaylistEndMode::Next;
  CHECK(model.updateItemSettings(itemId, changedBeforeFinish));
  const auto nextMode = machine.finish();
  CHECK(nextMode == PlaylistEndMode::Next);
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Ended);
  CHECK(machine.markActive());

  changedBeforeFinish.endMode = PlaylistEndMode::Goto;
  changedBeforeFinish.gotoTargetItemId = itemId;
  CHECK(model.updateItemSettings(itemId, changedBeforeFinish));
  const auto gotoMode = machine.finish();
  CHECK(gotoMode == PlaylistEndMode::Goto);
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Ended);
  CHECK(machine.markActive());

  changedBeforeFinish.endMode = PlaylistEndMode::Stop;
  CHECK(model.updateItemSettings(itemId, changedBeforeFinish));
  const auto stopMode = machine.finish();
  CHECK(stopMode == PlaylistEndMode::Stop);
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Ended);
  machine.fail();
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Error);
  CHECK(machine.deactivate());
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Prepared);

  CHECK(machine.start() == PlaylistStartMode::Play);
  CHECK(machine.markEnded());
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Ended);
  CHECK(machine.markActive());
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Active);
  CHECK(model.removeItem(itemId));
  CHECK(!machine.finish());
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::MissingItem);
  CHECK(machine.preparedMedia()->itemId == itemId);

  machine.reset();
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::Empty);
  CHECK(!machine.prepare(itemId));
  CHECK(machine.phase() == PlaylistItemPlayoutPhase::MissingItem);
  CHECK(!machine.preparedMedia());

  QTemporaryFile reorderFileA;
  QTemporaryFile reorderFileB;
  QTemporaryFile reorderFileC;
  CHECK(reorderFileA.open());
  CHECK(reorderFileB.open());
  CHECK(reorderFileC.open());
  PlaylistModel reorderModel;
  QList<PlaylistItemRegistration> reorderItems;
  for (const QString &path : {reorderFileA.fileName(),
                              reorderFileB.fileName(),
                              reorderFileC.fileName()}) {
    PlaylistItemRegistration item;
    item.filePath = path;
    item.settings.outPointMs = 1000;
    reorderItems.append(item);
  }
  reorderModel.addItems(reorderItems);
  CHECK(reorderModel.itemCount() == 3);
  const quint64 firstId = reorderModel.itemIdAt(0);
  const quint64 secondId = reorderModel.itemIdAt(1);
  const quint64 thirdId = reorderModel.itemIdAt(2);

  PlaylistModel mapModel;
  QList<PlaylistItemRegistration> mapItems;
  for (int index = 0; index < 3; ++index) {
    PlaylistItemRegistration item;
    item.filePath = reorderItems.at(index).filePath;
    item.displayName = QStringLiteral("Map %1").arg(index + 1);
    item.settings.inPointMs = 100;
    item.settings.outPointMs = 1100 + index * 1000;
    item.settings.startMode = PlaylistStartMode::Play;
    item.settings.endMode = index < 2 ? PlaylistEndMode::Next
                                     : PlaylistEndMode::Pause;
    mapItems.append(item);
  }
  const QList<quint64> mapIds = mapModel.addItems(mapItems);
  CHECK(mapIds.size() == 3);
  PlaylistMapAnalysis map = mapModel.analyzePlaySequence(mapIds.at(0));
  CHECK(!map.infinite);
  CHECK(map.totalDurationMs == 6000);
  CHECK(map.rows.size() == 3);
  CHECK(map.rows.constLast().itemId == mapIds.at(2));
  CHECK(map.rows.constLast().itemName == QStringLiteral("Map 3"));

  PlaylistItemSettings mapRepeat =
      mapModel.itemState(mapIds.at(0))->settings;
  mapRepeat.endMode = PlaylistEndMode::Repeat;
  mapRepeat.repeatCount = 3;
  mapRepeat.repeatEndMode = PlaylistRepeatEndMode::Next;
  CHECK(mapModel.updateItemSettings(mapIds.at(0), mapRepeat));
  map = mapModel.analyzePlaySequence(mapIds.at(0));
  CHECK(map.totalDurationMs == 8000);
  CHECK(map.rows.at(0).durationMs == 3000);
  const PlaylistMapAnalysis runningMap = mapModel.analyzePlaySequence(
      mapIds.at(0), PlaylistMapPlaybackProgress{400, 2});
  CHECK(runningMap.totalDurationMs == 6400);
  CHECK(runningMap.startingItemContributionMs == 1400);
  const PlaylistMapAnalysis currentOnly = mapModel.analyzePlaySequence(
      mapIds.at(0), PlaylistMapPlaybackProgress{400, 2}, true);
  const PlaylistMapAnalysis futureOnly = mapModel.analyzePlaySequence(
      mapIds.at(0), PlaylistMapPlaybackProgress{0, 3});
  CHECK(currentOnly.rows.size() == 1);
  CHECK(currentOnly.totalDurationMs == 1400);
  CHECK(futureOnly.totalDurationMs == 5000);
  CHECK(*currentOnly.totalDurationMs + *futureOnly.totalDurationMs ==
        *runningMap.totalDurationMs);

  mapRepeat.endCrossfadeEnabled = true;
  mapRepeat.endCrossfadeDurationMs = 200;
  CHECK(mapModel.updateItemSettings(mapIds.at(0), mapRepeat));
  map = mapModel.analyzePlaySequence(mapIds.at(0));
  CHECK(map.rows.at(0).durationMs == 2600); // 3 x 1 s, two 0.2 s overlaps.
  CHECK(mapModel.analyzePlaySequence(mapIds.at(0), PlaylistMapPlaybackProgress{400, 2})
            .startingItemContributionMs == 1200);
  CHECK(mapModel.analyzePlaySequence(mapIds.at(0), PlaylistMapPlaybackProgress{400, 2, 2.0})
            .startingItemContributionMs == 1000); // CF duration is wall-clock time.
  CHECK(mapModel.analyzePlaySequence(mapIds.at(0), PlaylistMapPlaybackProgress{0, 2})
            .startingItemContributionMs == 1000); // Late CUE: the last play is still whole.
  mapRepeat.endCrossfadeDurationMs = 60000;
  CHECK(mapModel.updateItemSettings(mapIds.at(0), mapRepeat));
  CHECK(mapModel.analyzePlaySequence(mapIds.at(0)).rows.at(0).durationMs == 2000);
  mapRepeat.endCrossfadeEnabled = false;
  CHECK(mapModel.updateItemSettings(mapIds.at(0), mapRepeat));

  PlaylistItemSettings pauseDestination =
      mapModel.itemState(mapIds.at(1))->settings;
  pauseDestination.startMode = PlaylistStartMode::Pause;
  CHECK(mapModel.updateItemSettings(mapIds.at(1), pauseDestination));
  map = mapModel.analyzePlaySequence(mapIds.at(0));
  CHECK(map.totalDurationMs == 3000);
  CHECK(map.rows.size() == 1);
  CHECK(map.stopReason ==
        PlaylistMapStopReason::DestinationStartModePause);
  pauseDestination.startMode = PlaylistStartMode::Play;
  CHECK(mapModel.updateItemSettings(mapIds.at(1), pauseDestination));

  PlaylistItemSettings mapLoop =
      mapModel.itemState(mapIds.at(2))->settings;
  mapLoop.endMode = PlaylistEndMode::Goto;
  mapLoop.gotoTargetItemId = mapIds.at(0);
  CHECK(mapModel.updateItemSettings(mapIds.at(2), mapLoop));
  map = mapModel.analyzePlaySequence(mapIds.at(0));
  CHECK(map.infinite);
  CHECK(!map.totalDurationMs);
  CHECK(map.stopReason == PlaylistMapStopReason::Loop);

  PlaylistItemRegistration liveMapItem;
  liveMapItem.filePath = QStringLiteral("uvc://map-camera");
  liveMapItem.displayName = QStringLiteral("Map Camera");
  liveMapItem.sourceType = PlaylistSourceType::Uvc;
  liveMapItem.settings.startMode = PlaylistStartMode::Play;
  liveMapItem.settings.endMode = PlaylistEndMode::Pause;
  liveMapItem.settings.outPointMs = 5000;
  const QList<quint64> liveMapIds = mapModel.addItems({liveMapItem});
  CHECK(liveMapIds.size() == 1);
  map = mapModel.analyzePlaySequence(liveMapIds.constFirst());
  CHECK(map.infinite);
  CHECK(map.rows.size() == 1);
  CHECK(map.rows.constFirst().infiniteDuration);
  CHECK(map.stopReason ==
        PlaylistMapStopReason::LiveOrUnknownDuration);

  PlaylistModel crossPlaylistMapModel;
  PlaylistItemRegistration crossMapStart;
  crossMapStart.filePath = reorderFileA.fileName();
  crossMapStart.displayName = QStringLiteral("Cross Map Start");
  crossMapStart.settings.startMode = PlaylistStartMode::Play;
  crossMapStart.settings.endMode = PlaylistEndMode::Goto;
  crossMapStart.settings.outPointMs = 1000;
  const quint64 crossMapStartId =
      crossPlaylistMapModel.addItems({crossMapStart}).constFirst();
  const int crossMapTargetPlaylist = crossPlaylistMapModel.createPlaylist();
  PlaylistItemRegistration crossMapTarget;
  crossMapTarget.filePath = reorderFileB.fileName();
  crossMapTarget.displayName = QStringLiteral("Cross Map Target");
  crossMapTarget.playlistNumber = crossMapTargetPlaylist;
  crossMapTarget.settings.startMode = PlaylistStartMode::Play;
  crossMapTarget.settings.endMode = PlaylistEndMode::Stop;
  crossMapTarget.settings.outPointMs = 2000;
  const quint64 crossMapTargetId =
      crossPlaylistMapModel.addItems({crossMapTarget}).constFirst();
  PlaylistItemSettings crossMapStartSettings =
      crossPlaylistMapModel.itemState(crossMapStartId)->settings;
  crossMapStartSettings.gotoTargetItemId = crossMapTargetId;
  CHECK(crossPlaylistMapModel.updateItemSettings(crossMapStartId,
                                                 crossMapStartSettings));
  const PlaylistMapAnalysis crossMap =
      crossPlaylistMapModel.analyzePlaySequence(crossMapStartId);
  CHECK(crossMap.totalDurationMs == 3000);
  CHECK(crossMap.rows.size() == 2);
  CHECK(crossMap.rows.at(0).playlistNumber == 1);
  CHECK(crossMap.rows.at(1).playlistNumber == crossMapTargetPlaylist);
  CHECK(crossMap.rows.at(1).itemId == crossMapTargetId);

  PlaylistModel bulkModel;
  bulkModel.addItems(reorderItems);
  CHECK(bulkModel.itemCount() == 3);
  const QList<quint64> bulkOriginalOrder = {
      bulkModel.itemIdAt(0), bulkModel.itemIdAt(1), bulkModel.itemIdAt(2)};
  QHash<quint64, PlaylistItemSettings> bulkOriginalSettings;
  for (int row = 0; row < bulkOriginalOrder.size(); ++row) {
    const quint64 bulkItemId = bulkOriginalOrder.at(row);
    PlaylistItemSettings settings = bulkModel.itemState(bulkItemId)->settings;
    settings.startMode = PlaylistStartMode::Pause;
    settings.endMode = PlaylistEndMode::Stop;
    settings.inPointMs = 10 + row;
    settings.outPointMs = 900 - row;
    settings.fadeInEnabled = row % 2 == 0;
    settings.fadeInDurationMs = 500 + row;
    settings.nominalVolumeTenthsDb = -10 * (row + 1);
    CHECK(bulkModel.updateItemSettings(bulkItemId, settings));
    bulkOriginalSettings.insert(bulkItemId, settings);
  }
  CHECK(bulkModel.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::PlayOnce));
  for (int row = 0; row < bulkOriginalOrder.size(); ++row) {
    const quint64 bulkItemId = bulkOriginalOrder.at(row);
    PlaylistItemSettings expected = bulkOriginalSettings.value(bulkItemId);
    expected.startMode = PlaylistStartMode::Play;
    expected.endMode = row + 1 == bulkOriginalOrder.size()
                           ? PlaylistEndMode::Pause
                           : PlaylistEndMode::Next;
    CHECK(bulkModel.itemState(bulkItemId)->settings == expected);
  }
  CHECK(bulkModel.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::RepeatAll));
  for (const quint64 bulkItemId : bulkOriginalOrder) {
    CHECK(bulkModel.itemState(bulkItemId)->settings.startMode ==
          PlaylistStartMode::Play);
    CHECK(bulkModel.itemState(bulkItemId)->settings.endMode ==
          PlaylistEndMode::Next);
  }

  CHECK(bulkModel.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::BgmMode));
  CHECK(bulkModel.playlistMode(1) == PlaylistMode::Bgm);
  for (const quint64 bulkItemId : bulkOriginalOrder) {
    CHECK(bulkModel.itemState(bulkItemId)->settings.startMode ==
          PlaylistStartMode::Play);
    CHECK(bulkModel.itemState(bulkItemId)->settings.endMode ==
          PlaylistEndMode::Next);
  }
  PlaylistItemSettings customizedBgmSettings =
      bulkModel.itemState(bulkOriginalOrder.constFirst())->settings;
  customizedBgmSettings.inPointMs = 250;
  customizedBgmSettings.outPointMs = 800;
  customizedBgmSettings.startMode = PlaylistStartMode::Pause;
  customizedBgmSettings.endMode = PlaylistEndMode::Stop;
  customizedBgmSettings.fadeInEnabled = true;
  customizedBgmSettings.fadeInDurationMs = 650;
  customizedBgmSettings.fadeOutEnabled = true;
  customizedBgmSettings.fadeOutDurationMs = 750;
  customizedBgmSettings.nominalVolumeTenthsDb = -90;
  CHECK(bulkModel.updateItemSettings(bulkOriginalOrder.constFirst(),
                                    customizedBgmSettings));
  CHECK(bulkModel.setPlaylistMode(1, PlaylistMode::Bgm));
  CHECK(bulkModel.itemState(bulkOriginalOrder.constFirst())->settings ==
        customizedBgmSettings);
  CHECK(bulkModel.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::StandardMode));
  CHECK(bulkModel.playlistMode(1) == PlaylistMode::Standard);
  CHECK(bulkModel.itemState(bulkOriginalOrder.constFirst())->settings ==
        customizedBgmSettings);

  bulkModel.setBgmItemId(bulkOriginalOrder.constFirst());
  CHECK(bulkModel.bgmItemId() == bulkOriginalOrder.constFirst());
  CHECK(bulkModel.itemEditPointsLocked(bulkOriginalOrder.constFirst()));
  PlaylistItemSettings changedBgmEditPoints = customizedBgmSettings;
  changedBgmEditPoints.inPointMs = 300;
  CHECK(!bulkModel.updateItemSettings(bulkOriginalOrder.constFirst(),
                                     changedBgmEditPoints));
  PlaylistItemSettings changedBgmEndMode = customizedBgmSettings;
  changedBgmEndMode.endMode = PlaylistEndMode::Next;
  CHECK(bulkModel.updateItemSettings(bulkOriginalOrder.constFirst(),
                                    changedBgmEndMode));
  CHECK(!bulkModel.removeItem(bulkOriginalOrder.constFirst()));
  bulkModel.clearBgmItem();
  CHECK(bulkModel.bgmItemId() == 0);

  bulkModel.setItemSettingsLocked(bulkOriginalOrder.at(1), true);
  const PlaylistModelSnapshot lockedBulkSnapshot = bulkModel.snapshot();
  CHECK(!bulkModel.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::Shuffle));
  CHECK(bulkModel.snapshot() == lockedBulkSnapshot);
  bulkModel.setItemSettingsLocked(bulkOriginalOrder.at(1), false);

  bulkModel.setOnAirItemId(bulkOriginalOrder.at(0));
  bulkModel.setCueItemState(bulkOriginalOrder.at(1),
                            PlaylistCueState::Ready);
  bulkModel.setDskItemId(bulkOriginalOrder.at(2));
  bulkModel.setEditItemId(bulkOriginalOrder.at(0));
  CHECK(bulkModel.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::Shuffle));
  const QList<quint64> bulkShuffledOrder = {
      bulkModel.itemIdAt(0), bulkModel.itemIdAt(1), bulkModel.itemIdAt(2)};
  CHECK(bulkShuffledOrder != bulkOriginalOrder);
  for (const quint64 bulkItemId : bulkShuffledOrder) {
    CHECK(bulkModel.itemState(bulkItemId)->settings.startMode ==
          PlaylistStartMode::Play);
    CHECK(bulkModel.itemState(bulkItemId)->settings.endMode ==
          PlaylistEndMode::Next);
  }
  CHECK(bulkModel.onAirItemId() == bulkOriginalOrder.at(0));
  CHECK(bulkModel.cueItemId() == bulkOriginalOrder.at(1));
  CHECK(bulkModel.dskItemId() == bulkOriginalOrder.at(2));
  CHECK(bulkModel.editItemId() == bulkOriginalOrder.at(0));

  const PlaylistModelSnapshot bulkBeforeClear = bulkModel.snapshot();
  bulkModel.clearOnAirItem();
  bulkModel.clearCueItem();
  bulkModel.clearDskItem();
  bulkModel.clearEditItem();
  for (const quint64 bulkItemId : bulkShuffledOrder) {
    CHECK(bulkModel.removeItem(bulkItemId));
  }
  CHECK(bulkModel.itemCount() == 0);
  CHECK(bulkModel.restoreSnapshot(bulkBeforeClear));
  CHECK(bulkModel.snapshot() == bulkBeforeClear);

  CHECK(reorderModel.nextItemIdForPlayout(firstId) == secondId);
  CHECK(reorderModel.nextItemIdForPlayout(firstId, secondId) == thirdId);
  CHECK(reorderModel.nextItemIdForPlayout(thirdId) == firstId);
  CHECK(reorderModel.nextItemIdForPlayout(999999) == 0);
  PlaylistItemSettings automaticSettings =
      reorderModel.itemState(firstId)->settings;
  automaticSettings.endMode = PlaylistEndMode::Next;
  CHECK(reorderModel.updateItemSettings(firstId, automaticSettings));
  CHECK(reorderModel.automaticEndTargetItemIdForPlayout(firstId) == secondId);
  automaticSettings.endMode = PlaylistEndMode::Goto;
  automaticSettings.gotoTargetItemId = thirdId;
  CHECK(reorderModel.updateItemSettings(firstId, automaticSettings));
  CHECK(reorderModel.automaticEndTargetItemIdForPlayout(firstId) == thirdId);
  CHECK(reorderModel.automaticEndTargetItemIdForPlayout(firstId, thirdId) == 0);
  CHECK(reorderModel.endTargetItemIdForPlayout(
            firstId, PlaylistEndMode::Next) == secondId);
  CHECK(reorderModel.endTargetItemIdForPlayout(
            firstId, PlaylistEndMode::Goto) == thirdId);
  CHECK(reorderModel.itemState(firstId)->itemNumber == 1);
  CHECK(reorderModel.itemState(secondId)->itemNumber == 2);
  CHECK(reorderModel.itemState(thirdId)->itemNumber == 3);
  reorderModel.setOnAirItemId(firstId);
  reorderModel.setCueItemState(secondId, PlaylistCueState::Ready);
  reorderModel.setDskItemId(thirdId);
  reorderModel.setEditItemId(firstId);

  quint64 movedId = 0;
  int movedFrom = -1;
  int movedTo = -1;
  QObject::connect(&reorderModel, &PlaylistModel::itemMoved, &reorderModel,
                   [&](quint64 id, int previousRow, int currentRow) {
                     movedId = id;
                     movedFrom = previousRow;
                     movedTo = currentRow;
                   });
  CHECK(reorderModel.moveItemTo(firstId, 2));
  CHECK(movedId == firstId);
  CHECK(movedFrom == 0);
  CHECK(movedTo == 2);
  CHECK(reorderModel.itemIdAt(0) == secondId);
  CHECK(reorderModel.itemIdAt(1) == thirdId);
  CHECK(reorderModel.itemIdAt(2) == firstId);
  CHECK(reorderModel.itemState(secondId)->itemNumber == 1);
  CHECK(reorderModel.itemState(thirdId)->itemNumber == 2);
  CHECK(reorderModel.itemState(firstId)->itemNumber == 3);
  CHECK(reorderModel.onAirItemId() == firstId);
  CHECK(reorderModel.cueItemId() == secondId);
  CHECK(reorderModel.cueState() == PlaylistCueState::Ready);
  CHECK(reorderModel.dskItemId() == thirdId);
  CHECK(reorderModel.editItemId() == firstId);
  CHECK(!reorderModel.moveItemTo(firstId, 2));
  CHECK(!reorderModel.moveItemTo(firstId, 3));
  CHECK(!reorderModel.moveItemTo(999999, 0));

  reorderModel.clearOnAirItem();
  PlaylistItemSettings removedTargetSettings =
      reorderModel.itemState(firstId)->settings;
  removedTargetSettings.gotoTargetItemId = secondId;
  CHECK(reorderModel.updateItemSettings(firstId, removedTargetSettings));
  CHECK(reorderModel.removeItem(secondId));
  CHECK(reorderModel.itemState(firstId)->settings.gotoTargetItemId == 0);
  CHECK(reorderModel.itemState(thirdId)->itemNumber == 1);
  CHECK(reorderModel.itemState(firstId)->itemNumber == 2);

  QTemporaryFile secondPlaylistFile;
  CHECK(secondPlaylistFile.open());
  const int secondPlaylistNumber = reorderModel.createPlaylist();
  CHECK(secondPlaylistNumber == 2);
  CHECK(reorderModel.setPlaylistName(secondPlaylistNumber,
                                     QStringLiteral("夕方ニュース")));
  CHECK(reorderModel.setActivePlaylistNumber(secondPlaylistNumber));
  CHECK(reorderModel.itemCount() == 0);
  PlaylistItemRegistration secondPlaylistRegistration;
  secondPlaylistRegistration.filePath = secondPlaylistFile.fileName();
  secondPlaylistRegistration.settings.outPointMs = 1000;
  secondPlaylistRegistration.playlistNumber = secondPlaylistNumber;
  reorderModel.addItems({secondPlaylistRegistration});
  CHECK(reorderModel.itemCount() == 1);
  const quint64 secondPlaylistItemId = reorderModel.itemIdAt(0);
  CHECK(secondPlaylistItemId != 0);
  CHECK(reorderModel.itemState(secondPlaylistItemId)->playlistNumber == 2);
  CHECK(reorderModel.itemState(secondPlaylistItemId)->itemNumber == 1);
  CHECK(reorderModel.itemCount(1) == 2);
  CHECK(reorderModel.itemIdAt(1, 0) == thirdId);
  CHECK(reorderModel.setActivePlaylistNumber(1));
  CHECK(reorderModel.itemCount() == 2);
  CHECK(reorderModel.playlistName(2) == QStringLiteral("夕方ニュース"));

  PlaylistItemSettings copiedSettings =
      reorderModel.itemState(firstId)->settings;
  copiedSettings.inPointMs = 125;
  copiedSettings.outPointMs = 875;
  copiedSettings.startMode = PlaylistStartMode::Play;
  copiedSettings.endMode = PlaylistEndMode::Repeat;
  copiedSettings.fadeInEnabled = true;
  copiedSettings.fadeInDurationMs = 350;
  copiedSettings.fadeOutEnabled = true;
  copiedSettings.fadeOutDurationMs = 450;
  copiedSettings.endCrossfadeEnabled = true;
  copiedSettings.endCrossfadeDurationMs = 550;
  copiedSettings.repeatCount = 4;
  copiedSettings.repeatEndMode = PlaylistRepeatEndMode::Next;
  copiedSettings.nominalVolumeTenthsDb = -35;
  CHECK(reorderModel.updateItemSettings(firstId, copiedSettings));

  CHECK(reorderModel.moveItemToPlaylist(thirdId, 2, 0));
  CHECK(reorderModel.itemCount(1) == 1);
  CHECK(reorderModel.itemCount(2) == 2);
  CHECK(reorderModel.itemIdAt(2, 0) == thirdId);
  CHECK(reorderModel.itemState(thirdId)->playlistNumber == 2);
  CHECK(reorderModel.itemState(thirdId)->itemNumber == 1);
  CHECK(reorderModel.itemState(secondPlaylistItemId)->itemNumber == 2);
  CHECK(reorderModel.dskItemId() == thirdId);
  CHECK(!reorderModel.moveItemToPlaylist(thirdId, 99, 0));

  const quint64 copiedId =
      reorderModel.copyItemToPlaylist(firstId, 2, 1);
  CHECK(copiedId != 0);
  CHECK(copiedId != firstId);
  CHECK(reorderModel.itemCount(1) == 1);
  CHECK(reorderModel.itemCount(2) == 3);
  CHECK(reorderModel.itemIdAt(2, 1) == copiedId);
  const auto copiedState = reorderModel.itemState(copiedId);
  const auto sourceState = reorderModel.itemState(firstId);
  CHECK(copiedState.has_value());
  CHECK(sourceState.has_value());
  CHECK(copiedState->playlistNumber == 2);
  CHECK(copiedState->itemNumber == 2);
  CHECK(copiedState->filePath == sourceState->filePath);
  CHECK(copiedState->displayName == sourceState->displayName);
  CHECK(copiedState->sourceType == sourceState->sourceType);
  CHECK(copiedState->mediaType == sourceState->mediaType);
  CHECK(copiedState->powerPointMode == sourceState->powerPointMode);
  CHECK(copiedState->settings == copiedSettings);
  CHECK(reorderModel.onAirItemId() == 0);
  CHECK(reorderModel.editItemId() == firstId);

  const quint64 samePlaylistCopyId =
      reorderModel.copyItemToPlaylist(firstId, 1, 0);
  CHECK(samePlaylistCopyId != 0);
  CHECK(samePlaylistCopyId != firstId);
  CHECK(reorderModel.itemIdAt(1, 0) == samePlaylistCopyId);
  CHECK(reorderModel.itemIdAt(1, 1) == firstId);
  CHECK(reorderModel.itemState(samePlaylistCopyId)->settings ==
        copiedSettings);
  CHECK(reorderModel.createPlaylist() == 3);
  CHECK(reorderModel.playlists().size() == 3);

  QTemporaryFile atomicFileA;
  QTemporaryFile atomicFileB;
  CHECK(atomicFileA.open());
  CHECK(atomicFileB.open());
  PlaylistModel atomicModel;
  int atomicItemAddedCount = 0;
  int atomicCountChangedCount = 0;
  QObject::connect(&atomicModel, &PlaylistModel::itemAdded, &atomicModel,
                   [&](quint64) { ++atomicItemAddedCount; });
  QObject::connect(&atomicModel, &PlaylistModel::countChanged, &atomicModel,
                   [&](int) { ++atomicCountChangedCount; });
  PlaylistItemRegistration atomicValidA;
  atomicValidA.filePath = atomicFileA.fileName();
  atomicValidA.mediaType = PlaylistMediaType::StillImage;
  PlaylistItemRegistration atomicInvalid = atomicValidA;
  atomicInvalid.filePath = atomicFileA.fileName() + QStringLiteral(".missing");
  CHECK(atomicModel.addItemsAtomically({atomicValidA, atomicInvalid})
            .isEmpty());
  CHECK(atomicModel.itemCount() == 0);
  CHECK(atomicItemAddedCount == 0);
  CHECK(atomicCountChangedCount == 0);
  PlaylistItemRegistration atomicValidB;
  atomicValidB.filePath = atomicFileB.fileName();
  atomicValidB.mediaType = PlaylistMediaType::StillImage;
  const QList<quint64> atomicIds =
      atomicModel.addItemsAtomically({atomicValidA, atomicValidB});
  CHECK(atomicIds.size() == 2);
  CHECK(atomicIds.constFirst() == 1);
  CHECK(atomicIds.at(1) == atomicIds.at(0) + 1);
  CHECK(atomicModel.itemCount() == 2);
  CHECK(atomicItemAddedCount == 2);
  CHECK(atomicCountChangedCount == 1);

  QTemporaryFile powerPointFile;
  CHECK(powerPointFile.open());
  PlaylistModel powerPointModel;
  PlaylistItemRegistration powerPointRegistration;
  powerPointRegistration.sourceType = PlaylistSourceType::PowerPoint;
  powerPointRegistration.mediaType = PlaylistMediaType::Presentation;
  powerPointRegistration.powerPointMode =
      PlaylistPowerPointMode::NativeSlideShow;
  powerPointRegistration.filePath = powerPointFile.fileName();
  powerPointRegistration.settings.startMode = PlaylistStartMode::Play;
  powerPointRegistration.settings.endMode = PlaylistEndMode::Pause;
  const QList<quint64> powerPointIds =
      powerPointModel.addItems({powerPointRegistration});
  CHECK(powerPointIds.size() == 1);
  const quint64 powerPointItemId = powerPointIds.constFirst();
  const auto powerPointState = powerPointModel.itemState(powerPointItemId);
  CHECK(powerPointState.has_value());
  CHECK(powerPointState->sourceType == PlaylistSourceType::PowerPoint);
  CHECK(powerPointState->mediaType == PlaylistMediaType::Presentation);
  CHECK(powerPointState->powerPointMode ==
        PlaylistPowerPointMode::NativeSlideShow);
  CHECK(powerPointModel.itemEditPointsLocked(powerPointItemId));
  CHECK(powerPointModel.itemSettingsLocked(powerPointItemId));
  PlaylistItemSettings changedPowerPointSettings = powerPointState->settings;
  changedPowerPointSettings.startMode = PlaylistStartMode::Pause;
  CHECK(!powerPointModel.updateItemSettings(powerPointItemId,
                                            changedPowerPointSettings));
  PlaylistItemRegistration invalidPowerPointMode = powerPointRegistration;
  invalidPowerPointMode.powerPointMode =
      PlaylistPowerPointMode::VideoConversion;
  CHECK(powerPointModel.addItems({invalidPowerPointMode}).isEmpty());

  QTemporaryFile pdfFile;
  CHECK(pdfFile.open());
  PlaylistModel pdfModel;
  PlaylistItemRegistration pdfRegistration;
  pdfRegistration.sourceType = PlaylistSourceType::Pdf;
  pdfRegistration.mediaType = PlaylistMediaType::Pdf;
  pdfRegistration.filePath = pdfFile.fileName();
  pdfRegistration.settings.startMode = PlaylistStartMode::Play;
  pdfRegistration.settings.endMode = PlaylistEndMode::Pause;
  const QList<quint64> pdfIds = pdfModel.addItems({pdfRegistration});
  CHECK(pdfIds.size() == 1);
  const auto pdfState = pdfModel.itemState(pdfIds.constFirst());
  CHECK(pdfState.has_value());
  CHECK(pdfState->sourceType == PlaylistSourceType::Pdf);
  CHECK(pdfState->mediaType == PlaylistMediaType::Pdf);
  CHECK(pdfModel.itemEditPointsLocked(pdfIds.constFirst()));
  CHECK(pdfModel.itemSettingsLocked(pdfIds.constFirst()));
  PlaylistItemRegistration invalidPdfRegistration = pdfRegistration;
  invalidPdfRegistration.mediaType = PlaylistMediaType::Video;
  CHECK(pdfModel.addItems({invalidPdfRegistration}).isEmpty());

  PlaylistModel uvcModel;
  PlaylistItemRegistration invalidUvcRegistration;
  invalidUvcRegistration.sourceType = PlaylistSourceType::Uvc;
  invalidUvcRegistration.filePath = QStringLiteral("camera-device-path");
  CHECK(uvcModel.addItems({invalidUvcRegistration}).isEmpty());

  PlaylistItemRegistration uvcRegistration;
  uvcRegistration.sourceType = PlaylistSourceType::Uvc;
  uvcRegistration.mediaType = PlaylistMediaType::Video;
  uvcRegistration.filePath = QStringLiteral("camera-device-path");
  uvcRegistration.displayName = QStringLiteral("Test UVC Camera");
  uvcRegistration.settings.startMode = PlaylistStartMode::Play;
  uvcRegistration.settings.endMode = PlaylistEndMode::Stop;
  const QList<quint64> uvcItemIds = uvcModel.addItems({uvcRegistration});
  CHECK(uvcItemIds.size() == 1);
  const quint64 uvcItemId = uvcItemIds.first();
  CHECK(uvcModel.nextItemIdForPlayout(uvcItemId) == uvcItemId);
  CHECK(uvcModel.nextItemIdForPlayout(uvcItemId, uvcItemId) == 0);
  const auto uvcState = uvcModel.itemState(uvcItemId);
  CHECK(uvcState.has_value());
  CHECK(uvcState->sourceType == PlaylistSourceType::Uvc);
  CHECK(uvcState->filePath == QStringLiteral("camera-device-path"));
  CHECK(uvcState->displayName == QStringLiteral("Test UVC Camera"));
  CHECK(uvcState->settings.startMode == PlaylistStartMode::Play);
  CHECK(!uvcState->settings.outPointMs.has_value());
  CHECK(uvcModel.itemEditPointsLocked(uvcItemId));
  CHECK(!uvcModel.itemSettingsLocked(uvcItemId));
  PlaylistItemSettings changedUvcSettings = uvcState->settings;
  changedUvcSettings.startMode = PlaylistStartMode::Pause;
  CHECK(!uvcModel.updateItemSettings(uvcItemId, changedUvcSettings));

  PlaylistItemPlayoutStateMachine uvcMachine(&uvcModel);
  CHECK(uvcMachine.prepare(uvcItemId));
  CHECK(uvcMachine.start() == PlaylistStartMode::Play);
  CHECK(uvcMachine.phase() == PlaylistItemPlayoutPhase::Active);

  QTemporaryFile offlineFileA;
  QTemporaryFile offlineFileB;
  QTemporaryFile offlineFileC;
  CHECK(offlineFileA.open());
  CHECK(offlineFileB.open());
  CHECK(offlineFileC.open());
  PlaylistModel offlineModel;
  QList<PlaylistItemRegistration> offlineRegistrations;
  for (const QString &path : {offlineFileA.fileName(),
                              offlineFileB.fileName(),
                              offlineFileC.fileName()}) {
    PlaylistItemRegistration item;
    item.filePath = path;
    item.settings.outPointMs = 1000;
    offlineRegistrations.append(item);
  }
  const QList<quint64> offlineIds =
      offlineModel.addItems(offlineRegistrations);
  CHECK(offlineIds.size() == 3);
  PlaylistItemSettings nextSettings =
      offlineModel.itemState(offlineIds.at(0))->settings;
  nextSettings.endMode = PlaylistEndMode::Next;
  CHECK(offlineModel.updateItemSettings(offlineIds.at(0), nextSettings));
  PlaylistItemSettings gotoSettings =
      offlineModel.itemState(offlineIds.at(2))->settings;
  gotoSettings.endMode = PlaylistEndMode::Goto;
  gotoSettings.gotoTargetItemId = offlineIds.at(1);
  CHECK(offlineModel.updateItemSettings(offlineIds.at(2), gotoSettings));

  offlineFileB.close();
  CHECK(offlineFileB.remove());
  int offlineChangeCount = 0;
  QObject::connect(
      &offlineModel, &PlaylistModel::itemMediaAvailabilityChanged,
      &offlineModel, [&](quint64 itemId, bool offline) {
        if (itemId == offlineIds.at(1) && offline) {
          ++offlineChangeCount;
        }
      });
  offlineModel.refreshMediaAvailability();
  CHECK(waitFor([&] { return offlineChangeCount == 1; }));
  CHECK(offlineChangeCount == 1);
  CHECK(offlineModel.itemMediaOffline(offlineIds.at(1)));
  CHECK(offlineModel.nextItemIdForPlayout(offlineIds.at(0)) ==
        offlineIds.at(2));
  CHECK(offlineModel.automaticEndTargetItemIdForPlayout(
            offlineIds.at(0)) == offlineIds.at(2));
  CHECK(offlineModel.automaticEndTargetItemIdForPlayout(
            offlineIds.at(2)) == 0);

  offlineFileC.close();
  CHECK(offlineFileC.remove());
  offlineModel.refreshMediaAvailability();
  CHECK(waitFor([&] { return offlineModel.itemMediaOffline(offlineIds.at(2)); }));
  nextSettings.startMode = PlaylistStartMode::Play;
  CHECK(offlineModel.updateItemSettings(offlineIds.at(0), nextSettings));
  CHECK(offlineModel.nextItemIdForPlayout(offlineIds.at(0)) ==
        offlineIds.at(0));
  CHECK(offlineModel.automaticEndTargetItemIdForPlayout(
            offlineIds.at(0)) == offlineIds.at(0));
  auto selfMap = offlineModel.analyzePlaySequence(offlineIds.at(0));
  CHECK(selfMap.infinite && selfMap.stopReason == PlaylistMapStopReason::Loop);
  CHECK(selfMap.rows.size() == 1);

  nextSettings.endMode = PlaylistEndMode::Goto;
  nextSettings.gotoTargetItemId = offlineIds.at(0);
  CHECK(offlineModel.updateItemSettings(offlineIds.at(0), nextSettings));
  CHECK(offlineModel.automaticEndTargetItemIdForPlayout(
            offlineIds.at(0)) == offlineIds.at(0));
  selfMap = offlineModel.analyzePlaySequence(offlineIds.at(0));
  CHECK(selfMap.infinite && selfMap.stopReason == PlaylistMapStopReason::Loop);
  CHECK(selfMap.rows.size() == 1);

  return 0;
}
