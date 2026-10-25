#include "playlist_document_io.h"
#include "live_audio_source.h"

#include "playlist_model.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <limits>
#include <atomic>

namespace {

bool check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  std::cerr << "line " << line << ": check failed: " << expression << '\n';
  return false;
}

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!check((expression), #expression, __LINE__)) {                          \
      return 1;                                                                 \
    }                                                                           \
  } while (false)

bool createFile(const QString &filePath) {
  QFile file(filePath);
  return file.open(QIODevice::WriteOnly) && file.write("test") == 4;
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir temporaryDirectory;
  CHECK(temporaryDirectory.isValid());

  {
    QString error;
    const QByteArray oversized(PlaylistDocumentIo::kMaximumJsonBytes + 1, ' ');
    CHECK(!PlaylistDocumentIo::fromJson(oversized, &error));
    CHECK(error.contains(QStringLiteral("bytes")));
    QFile largeFile(temporaryDirectory.filePath(QStringLiteral("oversized.vtrpon")));
    CHECK(largeFile.open(QIODevice::WriteOnly));
    CHECK(largeFile.resize(PlaylistDocumentIo::kMaximumJsonBytes + 1));
    largeFile.close();
    CHECK(!PlaylistDocumentIo::readFile(largeFile.fileName(), &error));
    CHECK(error.contains(QStringLiteral("bytes")));

    const auto base = QJsonDocument::fromJson(PlaylistDocumentIo::toJson(PlaylistModel().snapshot())).object();
    auto root = base;
    root.insert(QStringLiteral("documentId"), QString(PlaylistDocumentIo::kMaximumStringLength + 1, QLatin1Char('x')));
    CHECK(!PlaylistDocumentIo::fromJson(QJsonDocument(root).toJson(), &error));
    CHECK(error.contains(QStringLiteral("string")));

    root = base;
    QJsonArray playlists;
    for (int index = 0; index <= PlaylistDocumentIo::kMaximumPlaylists; ++index)
      playlists.append(base.value(QStringLiteral("playlists")).toArray().first());
    root.insert(QStringLiteral("playlists"), playlists);
    CHECK(!PlaylistDocumentIo::fromJson(QJsonDocument(root).toJson(), &error));
    CHECK(error.contains(QStringLiteral("playlists")));

    root = base;
    QJsonArray items;
    for (int index = 0; index <= PlaylistDocumentIo::kMaximumItems; ++index)
      items.append(QJsonObject{});
    playlists = base.value(QStringLiteral("playlists")).toArray();
    auto playlist = playlists.first().toObject();
    playlist.insert(QStringLiteral("items"), items);
    playlists.replace(0, playlist);
    root.insert(QStringLiteral("playlists"), playlists);
    CHECK(!PlaylistDocumentIo::fromJson(QJsonDocument(root).toJson(), &error));
    CHECK(error.contains(QStringLiteral("items")));

    root = base;
    QJsonArray extraStrings;
    for (int index = 0; index < 2050; ++index)
      extraStrings.append(QString(4096, QLatin1Char('x')));
    root.insert(QStringLiteral("extra"), extraStrings);
    CHECK(!PlaylistDocumentIo::fromJson(QJsonDocument(root).toJson(), &error));
    CHECK(error.contains(QStringLiteral("total")));

    std::atomic_bool cancelled{true};
    CHECK(!PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(PlaylistModel().snapshot()), &error, &cancelled));
    CHECK(error.contains(QStringLiteral("cancelled")));
    CHECK(!PlaylistDocumentIo::readFile(largeFile.fileName(), &error, &cancelled));
    CHECK(error.contains(QStringLiteral("cancelled")));
  }

  {
    PlaylistModel reordered;
    CHECK(reordered.createPlaylist() == 2);
    CHECK(reordered.createPlaylist() == 3);
    CHECK(reordered.setPlaylistName(3, QStringLiteral("Last")));
    CHECK(reordered.setActivePlaylistNumber(3));
    CHECK(reordered.movePlaylist(3, 0));
    const auto restored = PlaylistDocumentIo::fromJson(
        PlaylistDocumentIo::toJson(reordered.snapshot()));
    CHECK(restored.has_value());
    CHECK(restored->playlists.at(0).number == 3);
    CHECK(restored->playlists.at(0).name == QStringLiteral("Last"));
    CHECK(restored->playlists.at(1).number == 1);
    CHECK(restored->playlists.at(2).number == 2);
    CHECK(restored->activePlaylistNumber == 3);
  }

  const QString firstMediaPath =
      temporaryDirectory.filePath(QStringLiteral("first.mov"));
  const QString secondMediaPath =
      temporaryDirectory.filePath(QStringLiteral("second.wav"));
  const QString thirdMediaPath =
      temporaryDirectory.filePath(QStringLiteral("third.png"));
  const QString powerPointPath =
      temporaryDirectory.filePath(QStringLiteral("show.pptx"));
  const QString pdfPath =
      temporaryDirectory.filePath(QStringLiteral("rundown.pdf"));
  CHECK(createFile(firstMediaPath));
  CHECK(createFile(secondMediaPath));
  CHECK(createFile(thirdMediaPath));
  CHECK(createFile(powerPointPath));
  CHECK(createFile(pdfPath));

  PlaylistModel sourceModel;
  CHECK(sourceModel.setPlaylistName(1, QStringLiteral("Morning")));
  PlaylistItemRegistration firstItem;
  firstItem.filePath = firstMediaPath;
  firstItem.mediaType = PlaylistMediaType::Video;
  firstItem.settings.inPointMs = 125;
  firstItem.settings.outPointMs = 8125;
  firstItem.settings.startMode = PlaylistStartMode::Play;
  firstItem.settings.endMode = PlaylistEndMode::Next;
  firstItem.settings.fadeInEnabled = true;
  firstItem.settings.fadeInDurationMs = 350;
  firstItem.settings.fadeOutEnabled = true;
  firstItem.settings.fadeOutDurationMs = 750;
  firstItem.settings.endCrossfadeEnabled = true;
  firstItem.settings.endCrossfadeDurationMs = 1250;
  firstItem.settings.nominalVolumeTenthsDb = -120;
  firstItem.settings.nominalAudioDelayFrames = 6;
  firstItem.settings.nominalEqTenthsDb = {-150, 35, 150};
  const QList<quint64> firstIds = sourceModel.addItems({firstItem});
  CHECK(firstIds.size() == 1);
  const QString firstPathBeforeRename =
      sourceModel.itemState(firstIds.constFirst())->filePath;
  CHECK(sourceModel.setItemDisplayName(firstIds.constFirst(),
                                       QStringLiteral("Opening Title")));
  CHECK(sourceModel.setItemColor(firstIds.constFirst(),
                                 PlaylistItemColor::Red));
  CHECK(sourceModel.setItemSeparatorBelow(firstIds.constFirst(), true));
  CHECK(sourceModel.itemState(firstIds.constFirst())->filePath ==
        firstPathBeforeRename);

  const int secondPlaylistNumber = sourceModel.createPlaylist();
  CHECK(secondPlaylistNumber == 2);
  CHECK(sourceModel.setPlaylistName(secondPlaylistNumber,
                                    QStringLiteral("夕方ニュース")));
  CHECK(sourceModel.setPlaylistMode(secondPlaylistNumber,
                                    PlaylistMode::Bgm));
  CHECK(sourceModel.setActivePlaylistNumber(secondPlaylistNumber));

  PlaylistItemRegistration secondItem;
  secondItem.filePath = secondMediaPath;
  secondItem.mediaType = PlaylistMediaType::Audio;
  secondItem.settings.outPointMs = 4500;
  secondItem.settings.endMode = PlaylistEndMode::Stop;
  secondItem.playlistNumber = secondPlaylistNumber;
  PlaylistItemRegistration uvcItem;
  uvcItem.filePath = QStringLiteral("camera-device-path");
  uvcItem.displayName = QStringLiteral("Studio Camera");
  uvcItem.sourceType = PlaylistSourceType::Uvc;
  uvcItem.mediaType = PlaylistMediaType::Video;
  uvcItem.settings.startMode = PlaylistStartMode::Play;
  uvcItem.settings.endMode = PlaylistEndMode::Pause;
  uvcItem.playlistNumber = secondPlaylistNumber;
  PlaylistItemRegistration powerPointItem;
  powerPointItem.filePath = powerPointPath;
  powerPointItem.sourceType = PlaylistSourceType::PowerPoint;
  powerPointItem.mediaType = PlaylistMediaType::Presentation;
  powerPointItem.powerPointMode =
      PlaylistPowerPointMode::NativeSlideShow;
  powerPointItem.settings.startMode = PlaylistStartMode::Play;
  powerPointItem.settings.endMode = PlaylistEndMode::Pause;
  powerPointItem.playlistNumber = secondPlaylistNumber;
  PlaylistItemRegistration pdfItem;
  pdfItem.filePath = pdfPath;
  pdfItem.sourceType = PlaylistSourceType::Pdf;
  pdfItem.mediaType = PlaylistMediaType::Pdf;
  pdfItem.settings.startMode = PlaylistStartMode::Play;
  pdfItem.settings.endMode = PlaylistEndMode::Pause;
  pdfItem.playlistNumber = secondPlaylistNumber;
  const QList<quint64> secondIds =
      sourceModel.addItems({secondItem, uvcItem, powerPointItem, pdfItem});
  CHECK(secondIds.size() == 4);
  PlaylistItemSettings gotoSettings =
      sourceModel.itemState(firstIds.constFirst())->settings;
  gotoSettings.endMode = PlaylistEndMode::Goto;
  gotoSettings.gotoTargetItemId = secondIds.constFirst();
  gotoSettings.repeatCount = 3;
  gotoSettings.repeatEndMode = PlaylistRepeatEndMode::Goto;
  CHECK(sourceModel.updateItemSettings(firstIds.constFirst(), gotoSettings));

  const PlaylistModelSnapshot expectedSnapshot = sourceModel.snapshot();
  CHECK(expectedSnapshot.playlists.size() == 2);
  CHECK(expectedSnapshot.items.size() == 5);
  CHECK(expectedSnapshot.activePlaylistNumber == 2);

  const QString documentPath =
      temporaryDirectory.filePath(QStringLiteral("playlist.json"));
  QString errorDetail;
  CHECK(PlaylistDocumentIo::writeFile(documentPath, expectedSnapshot,
                                      &errorDetail));
  CHECK(errorDetail.isEmpty());

  QFile documentFile(documentPath);
  CHECK(documentFile.open(QIODevice::ReadOnly));
  const QByteArray documentBytes = documentFile.readAll();
  const QJsonObject root = QJsonDocument::fromJson(documentBytes).object();
  CHECK(root.value(QStringLiteral("format")).toString() ==
        QStringLiteral("vtr-pon-playlist"));
  CHECK(root.value(QStringLiteral("version")).toInt() == 1);
  const QJsonArray playlists = root.value(QStringLiteral("playlists")).toArray();
  CHECK(playlists.size() == 2);
  CHECK(playlists.at(0).toObject().value(QStringLiteral("mode")).toString() ==
        QStringLiteral("standard"));
  CHECK(playlists.at(1).toObject().value(QStringLiteral("mode")).toString() ==
        QStringLiteral("bgm"));
  CHECK(playlists.at(0).toObject()
            .value(QStringLiteral("items"))
            .toArray()
            .at(0)
            .toObject()
            .value(QStringLiteral("id"))
            .isString());
  const QJsonObject firstItemObject =
      playlists.at(0)
          .toObject()
          .value(QStringLiteral("items"))
          .toArray()
          .at(0)
          .toObject();
  CHECK(firstItemObject.value(QStringLiteral("displayName")).toString() ==
        QStringLiteral("Opening Title"));
  CHECK(firstItemObject.value(QStringLiteral("filePath")).toString() ==
        firstPathBeforeRename);
  CHECK(firstItemObject.value(QStringLiteral("color")).toString() ==
        QStringLiteral("red"));
  CHECK(firstItemObject.value(QStringLiteral("separatorBelow")).toBool());
  CHECK(playlists.at(0).toObject()
            .value(QStringLiteral("items"))
            .toArray()
            .at(0)
            .toObject()
            .value(QStringLiteral("settings"))
            .toObject()
            .value(QStringLiteral("endMode"))
            .toString() == QStringLiteral("goto"));
  const QJsonObject firstSettings =
      playlists.at(0)
          .toObject()
          .value(QStringLiteral("items"))
          .toArray()
          .at(0)
          .toObject()
          .value(QStringLiteral("settings"))
          .toObject();
  CHECK(firstSettings.value(QStringLiteral("endCrossfade"))
            .toObject()
            .value(QStringLiteral("enabled"))
            .toBool());
  CHECK(firstSettings.value(QStringLiteral("endCrossfade"))
            .toObject()
            .value(QStringLiteral("durationMs"))
            .toInteger() == 1250);
  CHECK(firstSettings.value(QStringLiteral("gotoTargetItemId")).toString() ==
        QString::number(secondIds.constFirst()));
  CHECK(firstSettings.value(QStringLiteral("repeatCount")).toInt() == 3);
  CHECK(firstSettings.value(QStringLiteral("repeatEndMode")).toString() ==
        QStringLiteral("goto"));
  const QJsonObject powerPointItemObject =
      playlists.at(1)
          .toObject()
          .value(QStringLiteral("items"))
          .toArray()
          .at(2)
          .toObject();
  CHECK(powerPointItemObject.value(QStringLiteral("sourceType")).toString() ==
        QStringLiteral("powerpoint"));
  CHECK(powerPointItemObject.value(QStringLiteral("mediaType")).toString() ==
        QStringLiteral("presentation"));
  CHECK(powerPointItemObject.value(QStringLiteral("powerPointMode"))
            .toString() == QStringLiteral("native"));
  const QJsonObject pdfItemObject =
      playlists.at(1)
          .toObject()
          .value(QStringLiteral("items"))
          .toArray()
          .at(3)
          .toObject();
  CHECK(pdfItemObject.value(QStringLiteral("sourceType")).toString() ==
        QStringLiteral("pdf"));
  CHECK(pdfItemObject.value(QStringLiteral("mediaType")).toString() ==
        QStringLiteral("pdf"));
  CHECK(pdfItemObject.value(QStringLiteral("powerPointMode")).isUndefined());

  const std::optional<PlaylistModelSnapshot> importedSnapshot =
      PlaylistDocumentIo::readFile(documentPath, &errorDetail);
  CHECK(importedSnapshot.has_value());
  CHECK(errorDetail.isEmpty());
  CHECK(*importedSnapshot == expectedSnapshot);
  auto crossfadeSnapshot = expectedSnapshot;
  crossfadeSnapshot.playlists[0].manualCrossfadeEnabled = true;
  crossfadeSnapshot.manualCrossfadeDurationMs = 2700;
  const QString crossfadePath = documentPath + QStringLiteral(".crossfade");
  CHECK(PlaylistDocumentIo::writeFile(crossfadePath, crossfadeSnapshot, &errorDetail));
  const auto savedCrossfade = PlaylistDocumentIo::readFile(crossfadePath, &errorDetail);
  CHECK(savedCrossfade && *savedCrossfade == crossfadeSnapshot);
  auto legacyCrossfade = QJsonDocument::fromJson(
      PlaylistDocumentIo::toJson(crossfadeSnapshot)).object();
  legacyCrossfade.remove(QStringLiteral("manualCrossfadeDurationMs"));
  auto legacyLists = legacyCrossfade.value(QStringLiteral("playlists")).toArray();
  for (int i = 0; i < legacyLists.size(); ++i) {
    auto list = legacyLists[i].toObject();
    list.remove(QStringLiteral("manualCrossfadeEnabled"));
    legacyLists[i] = list;
  }
  legacyCrossfade.insert(QStringLiteral("playlists"), legacyLists);
  const auto legacyResult = PlaylistDocumentIo::fromJson(QJsonDocument(legacyCrossfade).toJson());
  CHECK(legacyResult && *legacyResult == expectedSnapshot);
  crossfadeSnapshot.manualCrossfadeDurationMs = kMaximumItemFadeDurationMs + 1;
  CHECK(!PlaylistModel::canRestoreSnapshot(crossfadeSnapshot, &errorDetail));
  CHECK(!PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(crossfadeSnapshot), &errorDetail));
  errorDetail.clear();
  CHECK(importedSnapshot->items.at(0).settings.nominalEqTenthsDb == firstItem.settings.nominalEqTenthsDb);
  CHECK(importedSnapshot->items.at(0).settings.nominalAudioDelayFrames == 6);
  auto invalidDelaySnapshot = expectedSnapshot;
  invalidDelaySnapshot.items[0].settings.nominalAudioDelayFrames = 7;
  CHECK(!PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(invalidDelaySnapshot), &errorDetail));
  auto invalidDelaySettings = sourceModel.itemState(firstIds.constFirst())->settings;
  invalidDelaySettings.nominalAudioDelayFrames = -1;
  CHECK(!sourceModel.updateItemSettings(firstIds.constFirst(), invalidDelaySettings));
  auto invalidEqSnapshot = expectedSnapshot;
  invalidEqSnapshot.items[0].settings.nominalEqTenthsDb[1] = 151;
  CHECK(!PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(invalidEqSnapshot), &errorDetail));
  auto invalidEqSettings = sourceModel.itemState(firstIds.constFirst())->settings;
  invalidEqSettings.nominalEqTenthsDb[0] = -151;
  CHECK(!sourceModel.updateItemSettings(firstIds.constFirst(), invalidEqSettings));
  errorDetail.clear();

  QJsonObject legacyRoot = root;
  QJsonArray legacyPlaylists =
      legacyRoot.value(QStringLiteral("playlists")).toArray();
  QJsonObject legacyFirstPlaylist = legacyPlaylists.at(0).toObject();
  legacyFirstPlaylist.remove(QStringLiteral("mode"));
  QJsonArray legacyItems =
      legacyFirstPlaylist.value(QStringLiteral("items")).toArray();
  QJsonObject legacyFirstItem = legacyItems.at(0).toObject();
  legacyFirstItem.remove(QStringLiteral("color"));
  legacyFirstItem.remove(QStringLiteral("separatorBelow"));
  QJsonObject legacySettings =
      legacyFirstItem.value(QStringLiteral("settings")).toObject();
  legacySettings.remove(QStringLiteral("endCrossfade"));
  legacySettings.remove(QStringLiteral("gotoTargetItemId"));
  legacySettings.remove(QStringLiteral("repeatCount"));
  legacySettings.remove(QStringLiteral("repeatEndMode"));
  legacySettings.remove(QStringLiteral("nominalAudioDelayFrames"));
  legacySettings.remove(QStringLiteral("nominalEqLowTenthsDb"));
  legacySettings.remove(QStringLiteral("nominalEqMidTenthsDb"));
  legacySettings.remove(QStringLiteral("nominalEqHighTenthsDb"));
  legacyFirstItem.insert(QStringLiteral("settings"), legacySettings);
  legacyItems.replace(0, legacyFirstItem);
  legacyFirstPlaylist.insert(QStringLiteral("items"), legacyItems);
  legacyPlaylists.replace(0, legacyFirstPlaylist);
  legacyRoot.insert(QStringLiteral("playlists"), legacyPlaylists);
  QJsonObject legacySecondPlaylist = legacyPlaylists.at(1).toObject();
  legacySecondPlaylist.remove(QStringLiteral("mode"));
  QJsonArray legacySecondItems =
      legacySecondPlaylist.value(QStringLiteral("items")).toArray();
  QJsonObject legacyPowerPointItem = legacySecondItems.at(2).toObject();
  legacyPowerPointItem.remove(QStringLiteral("powerPointMode"));
  legacySecondItems.replace(2, legacyPowerPointItem);
  legacySecondPlaylist.insert(QStringLiteral("items"), legacySecondItems);
  legacyPlaylists.replace(1, legacySecondPlaylist);
  legacyRoot.insert(QStringLiteral("playlists"), legacyPlaylists);
  const auto legacySnapshot = PlaylistDocumentIo::fromJson(
      QJsonDocument(legacyRoot).toJson(), &errorDetail);
  CHECK(legacySnapshot.has_value());
  CHECK(legacySnapshot->items.at(0).settings.nominalAudioDelayFrames == 0);
  for (int gain : legacySnapshot->items.at(0).settings.nominalEqTenthsDb) CHECK(gain == 0);
  CHECK(legacySnapshot->playlists.at(0).mode == PlaylistMode::Standard);
  CHECK(legacySnapshot->playlists.at(1).mode == PlaylistMode::Standard);
  CHECK(!legacySnapshot->items.at(0).settings.endCrossfadeEnabled);
  CHECK(legacySnapshot->items.at(0).settings.endCrossfadeDurationMs ==
        kDefaultItemFadeDurationMs);
  CHECK(legacySnapshot->items.at(0).settings.gotoTargetItemId == 0);
  CHECK(legacySnapshot->items.at(0).settings.repeatCount ==
        kInfiniteItemRepeatCount);
  CHECK(legacySnapshot->items.at(0).settings.repeatEndMode ==
        PlaylistRepeatEndMode::Pause);
  CHECK(legacySnapshot->items.at(0).color == PlaylistItemColor::Default);
  CHECK(!legacySnapshot->items.at(0).separatorBelow);
  CHECK(legacySnapshot->items.at(3).powerPointMode ==
        PlaylistPowerPointMode::NativeSlideShow);

  QJsonObject invalidPlaylistModeRoot = root;
  QJsonArray invalidPlaylistModePlaylists =
      invalidPlaylistModeRoot.value(QStringLiteral("playlists")).toArray();
  QJsonObject invalidPlaylistModePlaylist =
      invalidPlaylistModePlaylists.at(0).toObject();
  invalidPlaylistModePlaylist.insert(QStringLiteral("mode"),
                                     QStringLiteral("bed"));
  invalidPlaylistModePlaylists.replace(0, invalidPlaylistModePlaylist);
  invalidPlaylistModeRoot.insert(QStringLiteral("playlists"),
                                 invalidPlaylistModePlaylists);
  CHECK(!PlaylistDocumentIo::fromJson(
      QJsonDocument(invalidPlaylistModeRoot).toJson(), &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("mode is not supported")));

  QJsonObject invalidPowerPointModeRoot = root;
  QJsonArray invalidPowerPointModePlaylists =
      invalidPowerPointModeRoot.value(QStringLiteral("playlists")).toArray();
  QJsonObject invalidPowerPointModePlaylist =
      invalidPowerPointModePlaylists.at(1).toObject();
  QJsonArray invalidPowerPointModeItems =
      invalidPowerPointModePlaylist.value(QStringLiteral("items")).toArray();
  QJsonObject invalidPowerPointModeItem =
      invalidPowerPointModeItems.at(2).toObject();
  invalidPowerPointModeItem.insert(QStringLiteral("powerPointMode"),
                                   QStringLiteral("webm"));
  invalidPowerPointModeItems.replace(2, invalidPowerPointModeItem);
  invalidPowerPointModePlaylist.insert(QStringLiteral("items"),
                                       invalidPowerPointModeItems);
  invalidPowerPointModePlaylists.replace(1,
                                         invalidPowerPointModePlaylist);
  invalidPowerPointModeRoot.insert(QStringLiteral("playlists"),
                                   invalidPowerPointModePlaylists);
  CHECK(!PlaylistDocumentIo::fromJson(
      QJsonDocument(invalidPowerPointModeRoot).toJson(), &errorDetail));
  CHECK(errorDetail.contains(
      QStringLiteral("powerPointMode is not supported")));

  QJsonObject invalidColorRoot = root;
  QJsonArray invalidColorPlaylists =
      invalidColorRoot.value(QStringLiteral("playlists")).toArray();
  QJsonObject invalidColorPlaylist = invalidColorPlaylists.at(0).toObject();
  QJsonArray invalidColorItems =
      invalidColorPlaylist.value(QStringLiteral("items")).toArray();
  QJsonObject invalidColorItem = invalidColorItems.at(0).toObject();
  invalidColorItem.insert(QStringLiteral("color"),
                          QStringLiteral("magenta"));
  invalidColorItems.replace(0, invalidColorItem);
  invalidColorPlaylist.insert(QStringLiteral("items"), invalidColorItems);
  invalidColorPlaylists.replace(0, invalidColorPlaylist);
  invalidColorRoot.insert(QStringLiteral("playlists"),
                          invalidColorPlaylists);
  CHECK(!PlaylistDocumentIo::fromJson(
      QJsonDocument(invalidColorRoot).toJson(), &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("color is not supported")));

  QJsonObject nullRepeatRoot = root;
  QJsonArray nullRepeatPlaylists =
      nullRepeatRoot.value(QStringLiteral("playlists")).toArray();
  QJsonObject nullRepeatPlaylist = nullRepeatPlaylists.at(0).toObject();
  QJsonArray nullRepeatItems =
      nullRepeatPlaylist.value(QStringLiteral("items")).toArray();
  QJsonObject nullRepeatItem = nullRepeatItems.at(0).toObject();
  QJsonObject nullRepeatSettings =
      nullRepeatItem.value(QStringLiteral("settings")).toObject();
  nullRepeatSettings.insert(QStringLiteral("repeatEndMode"),
                            QJsonValue(QJsonValue::Null));
  nullRepeatItem.insert(QStringLiteral("settings"), nullRepeatSettings);
  nullRepeatItems.replace(0, nullRepeatItem);
  nullRepeatPlaylist.insert(QStringLiteral("items"), nullRepeatItems);
  nullRepeatPlaylists.replace(0, nullRepeatPlaylist);
  nullRepeatRoot.insert(QStringLiteral("playlists"), nullRepeatPlaylists);
  const auto nullRepeatSnapshot = PlaylistDocumentIo::fromJson(
      QJsonDocument(nullRepeatRoot).toJson(), &errorDetail);
  CHECK(nullRepeatSnapshot.has_value());
  CHECK(nullRepeatSnapshot->items.at(0).settings.repeatEndMode ==
        PlaylistRepeatEndMode::Pause);

  QJsonObject incompleteRepeatRoot = root;
  QJsonArray incompleteRepeatPlaylists =
      incompleteRepeatRoot.value(QStringLiteral("playlists")).toArray();
  QJsonObject incompleteRepeatPlaylist =
      incompleteRepeatPlaylists.at(0).toObject();
  QJsonArray incompleteRepeatItems =
      incompleteRepeatPlaylist.value(QStringLiteral("items")).toArray();
  QJsonObject incompleteRepeatItem = incompleteRepeatItems.at(0).toObject();
  QJsonObject incompleteRepeatSettings =
      incompleteRepeatItem.value(QStringLiteral("settings")).toObject();
  incompleteRepeatSettings.remove(QStringLiteral("repeatEndMode"));
  incompleteRepeatItem.insert(QStringLiteral("settings"),
                              incompleteRepeatSettings);
  incompleteRepeatItems.replace(0, incompleteRepeatItem);
  incompleteRepeatPlaylist.insert(QStringLiteral("items"),
                                  incompleteRepeatItems);
  incompleteRepeatPlaylists.replace(0, incompleteRepeatPlaylist);
  incompleteRepeatRoot.insert(QStringLiteral("playlists"),
                              incompleteRepeatPlaylists);
  CHECK(!PlaylistDocumentIo::fromJson(
      QJsonDocument(incompleteRepeatRoot).toJson(), &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("must be provided together")));

  PlaylistModel restoredModel;
  CHECK(restoredModel.canRestoreSnapshot(*importedSnapshot, &errorDetail));
  CHECK(restoredModel.restoreSnapshot(*importedSnapshot, &errorDetail));
  CHECK(errorDetail.isEmpty());
  CHECK(restoredModel.snapshot() == expectedSnapshot);
  CHECK(restoredModel.itemIdAt(1, 0) == firstIds.constFirst());
  CHECK(restoredModel.itemIdAt(2, 0) == secondIds.at(0));
  CHECK(restoredModel.itemIdAt(2, 1) == secondIds.at(1));
  CHECK(restoredModel.itemIdAt(2, 2) == secondIds.at(2));

  PlaylistModelSnapshot missingGotoTargetSnapshot = expectedSnapshot;
  missingGotoTargetSnapshot.items[0].settings.gotoTargetItemId = 999999;
  CHECK(!restoredModel.canRestoreSnapshot(missingGotoTargetSnapshot,
                                          &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("missing GOTO target")));

  PlaylistItemRegistration thirdItem;
  thirdItem.filePath = thirdMediaPath;
  thirdItem.mediaType = PlaylistMediaType::StillImage;
  thirdItem.playlistNumber = 2;
  const QList<quint64> thirdIds = restoredModel.addItems({thirdItem});
  CHECK(thirdIds.size() == 1);
  CHECK(thirdIds.constFirst() == secondIds.constLast() + 1);

  QJsonObject unsupportedRoot = root;
  unsupportedRoot.insert(QStringLiteral("version"), 99);
  CHECK(!PlaylistDocumentIo::fromJson(
      QJsonDocument(unsupportedRoot).toJson(), &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("version 99")));
  CHECK(!PlaylistDocumentIo::fromJson(QByteArray("{"), &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("Invalid JSON")));

  PlaylistModelSnapshot missingFileSnapshot = expectedSnapshot;
  missingFileSnapshot.items[0].filePath =
      temporaryDirectory.filePath(QStringLiteral("missing.mov"));
  CHECK(restoredModel.canRestoreSnapshot(missingFileSnapshot, &errorDetail));
  CHECK(restoredModel.restoreSnapshot(missingFileSnapshot, &errorDetail));
  CHECK(errorDetail.isEmpty());
  CHECK(restoredModel.snapshot() == missingFileSnapshot);
  CHECK(restoredModel.itemMediaOffline(missingFileSnapshot.items[0].id));

  PlaylistModelSnapshot emptyFilePathSnapshot = expectedSnapshot;
  emptyFilePathSnapshot.items[0].filePath.clear();
  CHECK(!restoredModel.canRestoreSnapshot(emptyFilePathSnapshot,
                                          &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("no media file path")));

  const PlaylistModelSnapshot stateBeforeRejectedRestore =
      restoredModel.snapshot();

  restoredModel.setCueItemState(restoredModel.itemIdAt(2, 0),
                                PlaylistCueState::Ready);
  CHECK(!restoredModel.restoreSnapshot(expectedSnapshot, &errorDetail));
  CHECK(errorDetail.contains(QStringLiteral("must be released")));
  CHECK(restoredModel.snapshot() == stateBeforeRejectedRestore);

  // Legacy documents acquire an identity once, then retain it across save/restore.
  QJsonObject oldDocument = root;
  oldDocument.remove(QStringLiteral("documentId"));
  oldDocument.remove(QStringLiteral("macros"));
  oldDocument.remove(QStringLiteral("nextItemId"));
  const auto migrated = PlaylistDocumentIo::fromJson(QJsonDocument(oldDocument).toJson(), &errorDetail);
  CHECK(migrated.has_value());
  CHECK(!QUuid(migrated->documentId).isNull());
  CHECK(migrated->macros.isEmpty());
  const auto migratedAgain = PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(*migrated), &errorDetail);
  CHECK(migratedAgain && *migratedAgain == *migrated);

  PlaylistModel macroModel;
  CHECK(macroModel.restoreSnapshot(*migrated, &errorDetail));
  MacroDefinition macro;
  macro.name = QStringLiteral("Evening rundown");
  MacroStep cue;
  cue.type = MacroStepType::Cue;
  cue.documentId = macroModel.documentId();
  cue.itemId = firstIds.constFirst();
  MacroStep wait;
  wait.type = MacroStepType::WaitUntil;
  wait.scheduledAt = QDateTime::fromString(QStringLiteral("2026-09-23T00:00:01.250+09:00"), Qt::ISODateWithMs);
  MacroStep take = cue;
  take.id = newMacroId();
  take.type = MacroStepType::Take;
  take.requireReady = true;
  MacroStep settings = cue;
  settings.id = newMacroId();
  settings.type = MacroStepType::ItemSettings;
  settings.settingsPatch = {{QStringLiteral("startMode"), QStringLiteral("play")},
                           {QStringLiteral("fadeIn"), QJsonObject{{QStringLiteral("enabled"), true}}}};
  MacroStep volume;
  volume.type = MacroStepType::RuntimeSetting;
  volume.operation = QStringLiteral("masterVolume");
  volume.value = -60;
  macro.steps = {settings, cue, wait, take, volume};
  CHECK(macroModel.setMacros({macro}, &errorDetail));
  CHECK(macroModel.macroReferenceErrors().isEmpty());
  const auto macroSnapshot = macroModel.snapshot();
  const auto macroRestored = PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(macroSnapshot), &errorDetail);
  CHECK(macroRestored && *macroRestored == macroSnapshot);
  CHECK(macroRestored->macros.at(0).steps.at(2).scheduledAt == wait.scheduledAt);
  CHECK(macroModel.restoreSnapshot(*macroRestored, &errorDetail));
  CHECK(macroModel.documentId() == cue.documentId);

  // References survive moves and deletion never silently discards a macro draft.
  CHECK(macroModel.moveItemToPlaylist(cue.itemId, 2, 0));
  CHECK(macroModel.macroReferenceErrors().isEmpty());
  CHECK(macroModel.removeItem(cue.itemId));
  CHECK(macroModel.macros() == QList<MacroDefinition>{macro});
  CHECK(!macroModel.macroReferenceErrors().isEmpty());
  const auto danglingDraft = PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(macroModel.snapshot()), &errorDetail);
  CHECK(danglingDraft && danglingDraft->macros == QList<MacroDefinition>{macro});
  const auto preservedDocument = macroModel.documentId();
  CHECK(macroModel.restoreSnapshot(*danglingDraft, &errorDetail));
  CHECK(macroModel.documentId() == preservedDocument);
  auto foreignMacro = macro;
  foreignMacro.steps[1].documentId = newMacroId();
  CHECK(!MacroDocumentIo::referenceErrors({foreignMacro}, cue.documentId, {cue.itemId}).isEmpty());

  // Invalid definitions are rejected as a whole without replacing saved drafts.
  auto badMacro = macro;
  badMacro.steps[1].id = badMacro.steps[0].id;
  CHECK(!macroModel.setMacros({badMacro}, &errorDetail));
  CHECK(macroModel.macros() == QList<MacroDefinition>{macro});
  badMacro = macro;
  badMacro.steps[4].operation = QStringLiteral("importSettings");
  CHECK(!macroModel.setMacros({badMacro}, &errorDetail));
  badMacro = macro;
  badMacro.steps[4].value = 100;
  CHECK(!macroModel.setMacros({badMacro}, &errorDetail));
  QJsonArray invalidMacros = MacroDocumentIo::toJson({macro});
  QJsonObject invalidMacro = invalidMacros[0].toObject();
  invalidMacro.insert(QStringLiteral("version"), 2);
  invalidMacros[0] = invalidMacro;
  CHECK(!MacroDocumentIo::fromJson(invalidMacros, &errorDetail));
  invalidMacros = MacroDocumentIo::toJson({macro});
  invalidMacro = invalidMacros[0].toObject();
  QJsonArray invalidSteps = invalidMacro.value(QStringLiteral("steps")).toArray();
  QJsonObject invalidStep = invalidSteps[0].toObject();
  invalidStep.insert(QStringLiteral("enabled"), 1);
  invalidSteps[0] = invalidStep;
  invalidMacro.insert(QStringLiteral("steps"), invalidSteps);
  invalidMacros[0] = invalidMacro;
  CHECK(!MacroDocumentIo::fromJson(invalidMacros, &errorDetail));

  // Patches keep unmentioned current values and validate all fields before write.
  PlaylistItemSettings current;
  current.fadeInDurationMs = 2750;
  current.nominalEqTenthsDb = {30, 40, 50};
  PlaylistItemSettings changed;
  CHECK(MacroDocumentIo::applySettingsPatch(current, settings.settingsPatch, &changed, &errorDetail));
  CHECK(changed.startMode == PlaylistStartMode::Play);
  CHECK(changed.fadeInEnabled);
  CHECK(changed.fadeInDurationMs == 2750);
  CHECK(changed.nominalEqTenthsDb == current.nominalEqTenthsDb);
  const auto beforeFailure = changed;
  CHECK(!MacroDocumentIo::applySettingsPatch(current,
      {{QStringLiteral("inPointMs"), 1000}, {QStringLiteral("outPointMs"), 500}}, &changed, &errorDetail));
  CHECK(changed == beforeFailure);
  CHECK(!MacroDocumentIo::applySettingsPatch(current,
      {{QStringLiteral("startMode"), QStringLiteral("play")}, {QStringLiteral("unexpected"), true}}, &changed, &errorDetail));
  CHECK(changed == beforeFailure);
  CHECK(!MacroDocumentIo::applySettingsPatch(current,
      {{QStringLiteral("fadeIn"), QJsonObject{{QStringLiteral("enabled"), QStringLiteral("true")}}}}, &changed, &errorDetail));
  CHECK(!MacroDocumentIo::applySettingsPatch(current,
      {{QStringLiteral("nominalVolumeTenthsDb"), 61}}, &changed, &errorDetail));
  CHECK(!MacroDocumentIo::applySettingsPatch(current,
      {{QStringLiteral("endMode"), QStringLiteral("goto")}}, &changed, &errorDetail));
  CHECK(MacroDocumentIo::applySettingsPatch(current,
      {{QStringLiteral("endMode"), QStringLiteral("goto")},
       {QStringLiteral("gotoTargetItemId"), QString::number(secondIds[0])}}, &changed, &errorDetail));
  CHECK(changed.gotoTargetItemId == secondIds[0]);

  // A deleted highest ID must not bind a saved macro to a newly added file.
  PlaylistModel allocationModel;
  PlaylistItemRegistration allocationItem;
  allocationItem.filePath = firstMediaPath;
  allocationItem.mediaType = PlaylistMediaType::Video;
  const auto allocatedIds = allocationModel.addItems({allocationItem, allocationItem});
  CHECK(allocatedIds.size() == 2);
  MacroDefinition allocationMacro;
  MacroStep deletedReference;
  deletedReference.type = MacroStepType::Cue;
  deletedReference.itemId = allocatedIds.last();
  deletedReference.documentId = allocationModel.documentId();
  allocationMacro.steps = {deletedReference};
  CHECK(allocationModel.setMacros({allocationMacro}));
  CHECK(allocationModel.removeItem(deletedReference.itemId));
  const auto allocationSnapshot = allocationModel.snapshot();
  const auto allocationRoundTrip = PlaylistDocumentIo::fromJson(
      PlaylistDocumentIo::toJson(allocationSnapshot), &errorDetail);
  CHECK(allocationRoundTrip && *allocationRoundTrip == allocationSnapshot);
  CHECK(allocationModel.restoreSnapshot(*allocationRoundTrip, &errorDetail));
  const auto newAllocated = allocationModel.addItems({allocationItem});
  CHECK(newAllocated.size() == 1 && newAllocated[0] > deletedReference.itemId);
  CHECK(!allocationModel.itemState(deletedReference.itemId));
  CHECK(!allocationModel.macroReferenceErrors().isEmpty());

  // Older documents lacking the counter reserve retained macro and GOTO IDs.
  auto legacyAllocation = QJsonDocument::fromJson(
      PlaylistDocumentIo::toJson(allocationSnapshot)).object();
  legacyAllocation.remove(QStringLiteral("nextItemId"));
  const auto legacyAllocationRead = PlaylistDocumentIo::fromJson(
      QJsonDocument(legacyAllocation).toJson(), &errorDetail);
  CHECK(legacyAllocationRead && legacyAllocationRead->nextItemId > deletedReference.itemId);
  CHECK(allocationModel.restoreSnapshot(*legacyAllocationRead, &errorDetail));
  CHECK(allocationModel.addItems({allocationItem})[0] > deletedReference.itemId);
  auto reservedMacro = allocationMacro;
  reservedMacro.steps[0].enabled = false;
  reservedMacro.steps[0].itemId = 40;
  reservedMacro.steps[0].settingsPatch = {{QStringLiteral("gotoTargetItemId"), QStringLiteral("70")}};
  CHECK(allocationModel.setMacros({reservedMacro}, &errorDetail));
  CHECK(allocationModel.snapshot().nextItemId == 71);
  CHECK(allocationModel.setMacros({}));
  const auto afterMacroRemoved = PlaylistDocumentIo::fromJson(
      PlaylistDocumentIo::toJson(allocationModel.snapshot()), &errorDetail);
  CHECK(afterMacroRemoved && afterMacroRemoved->nextItemId == 71);
  CHECK(allocationModel.restoreSnapshot(*afterMacroRemoved, &errorDetail));
  CHECK(allocationModel.addItems({allocationItem})[0] == 71);

  // Malformed and overflow IDs fail atomically; another document's ID is isolated.
  auto overflowMacro = allocationMacro;
  overflowMacro.steps[0].itemId = (std::numeric_limits<quint64>::max)();
  CHECK(!allocationModel.setMacros({overflowMacro}, &errorDetail));
  CHECK(allocationModel.macros().isEmpty());
  overflowMacro.steps[0].documentId = newMacroId();
  CHECK(allocationModel.setMacros({overflowMacro}, &errorDetail));
  CHECK(allocationModel.snapshot().nextItemId == 72);
  auto badAllocation = QJsonDocument::fromJson(
      PlaylistDocumentIo::toJson(allocationSnapshot)).object();
  for (const QJsonValue &invalidId : {QJsonValue(3), QJsonValue(QStringLiteral("0")),
       QJsonValue(QStringLiteral("18446744073709551616"))}) {
    badAllocation.insert(QStringLiteral("nextItemId"), invalidId);
    CHECK(!PlaylistDocumentIo::fromJson(QJsonDocument(badAllocation).toJson(), &errorDetail));
  }
  auto exhaustedSnapshot = allocationSnapshot;
  exhaustedSnapshot.macros[0].steps[0].itemId = (std::numeric_limits<quint64>::max)();
  CHECK(!PlaylistModel::canRestoreSnapshot(exhaustedSnapshot, &errorDetail));
  CHECK(!PlaylistDocumentIo::fromJson(PlaylistDocumentIo::toJson(exhaustedSnapshot), &errorDetail));

  {
    PlaylistModel liveAudioModel;
    PlaylistItemRegistration audio;
    const QString endpoint = QStringLiteral("USB audio endpoint / 入力 %1");
    audio.filePath = vtrpon::liveAudioSourcePath(endpoint);
    audio.displayName = QStringLiteral("USB Microphone");
    audio.sourceType = PlaylistSourceType::Uvc;
    audio.mediaType = PlaylistMediaType::Audio;
    const auto ids = liveAudioModel.addItems({audio, audio});
    CHECK(ids.size() == 2);
    const auto path = temporaryDirectory.filePath(QStringLiteral("live-audio.json"));
    CHECK(PlaylistDocumentIo::writeFile(path, liveAudioModel.snapshot(), &errorDetail));
    const auto restored = PlaylistDocumentIo::readFile(path, &errorDetail);
    CHECK(restored && restored->items.size() == 2);
    CHECK(liveAudioModel.restoreSnapshot(*restored, &errorDetail));
    for (const auto id : ids) {
      const auto state = liveAudioModel.itemState(id);
      CHECK(state && state->sourceType == PlaylistSourceType::Uvc && state->mediaType == PlaylistMediaType::Audio);
      CHECK(vtrpon::liveAudioDeviceId(state->filePath) == endpoint);
      CHECK(state->settings.inPointMs == 0 && !state->settings.outPointMs && state->settings.startMode == PlaylistStartMode::Play);
    }
    liveAudioModel.setUvcDeviceAvailable(audio.filePath, false);
    CHECK(liveAudioModel.itemMediaOffline(ids[0]) && liveAudioModel.itemMediaOffline(ids[1]));
    liveAudioModel.refreshMediaAvailability();
    CHECK(liveAudioModel.itemMediaOffline(ids[0]) && liveAudioModel.itemMediaOffline(ids[1]));
    liveAudioModel.setUvcDeviceAvailable(audio.filePath, true);
    CHECK(!liveAudioModel.itemMediaOffline(ids[0]) && !liveAudioModel.itemMediaOffline(ids[1]));
  }
  return 0;
}
