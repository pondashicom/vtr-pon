#include "network_video_settings_page.h"
#include "input_control_panel.h"
#include "file_management_settings_page.h"
#include <QDoubleSpinBox>
#include <QMenu>
#include <QAction>
#include "playlist_model.h"
#include "playlist_panel.h"
#include "live_audio_source.h"
#include "playlist_thumbnail_store.h"
#include "decklink.h"
#include "decklink_settings_page.h"
#include "media_thumbnailer.h"
#include "playlist_view_roles.h"
#include "controller_scaling.h"
#include "controller_window_scaling.h"
#include <QVBoxLayout>
#include "controller_settings_dialog.h"
#include "controller_settings.h"
#include "settings_style.h"
#include "addon_management_page.h"
#include "skins/controller_skin.h"
#include "view_menu_controller.h"
#include <QMenuBar>
#include "controller_settings_io.h"
#include "virtual_camera/output.h"
#include "controller_shortcut_settings.h"
#include <QKeySequenceEdit>
#include <QTableWidget>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include "audio_output_settings.h"
#include "switcher_control.h"
#include "atem_addon_switcher.h"
#include "vst_editor.h"
#include "vst_protocol.h"
#include "user_info.h"

#include <QApplication>
#include <QAction>
#include <QWindow>
#include <QScreen>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QFontComboBox>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QUrl>
#include <QElapsedTimer>
#include <QThread>
#include <QProcessEnvironment>

#include <array>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

namespace {

constexpr int kCompactDurationWidth = 41;

bool waitForAvailability(PlaylistModel &model, quint64 id, bool offline) {
  QElapsedTimer timer;
  timer.start();
  while (model.itemMediaOffline(id) != offline && timer.elapsed() < 2000) {
    QApplication::processEvents();
    QThread::msleep(1);
  }
  return model.itemMediaOffline(id) == offline;
}

bool check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "line %d: check failed: %s\n", line, expression);
  return false;
}

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!check((expression), #expression, __LINE__)) {                          \
      return 1;                                                                 \
    }                                                                           \
  } while (false)

bool createImageFile(QTemporaryFile &file, const QColor &color) {
  if (!file.open()) {
    return false;
  }
  const QString path = file.fileName();
  file.close();
  QImage image(32, 18, QImage::Format_RGB32);
  image.fill(color);
  return image.save(path, "PNG");
}

template <typename Predicate>
int matchingPixelCount(const QImage &image, const QRect &rect,
                       Predicate predicate) {
  int count = 0;
  const QRect bounded = rect.intersected(image.rect());
  for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
    for (int x = bounded.left(); x <= bounded.right(); ++x) {
      if (predicate(image.pixelColor(x, y))) {
        ++count;
      }
    }
  }
  return count;
}

qint64 redDominanceScore(const QImage &image, const QRect &rect) {
  qint64 score = 0;
  const QRect bounded = rect.intersected(image.rect());
  for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
    for (int x = bounded.left(); x <= bounded.right(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      score += (std::max)(0, pixel.red() - pixel.green());
    }
  }
  return score;
}

int differingPixelCount(const QImage &first, const QImage &second,
                        const QRect &rect) {
  int count = 0;
  const QRect bounded = rect.intersected(first.rect()).intersected(second.rect());
  for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
    for (int x = bounded.left(); x <= bounded.right(); ++x) {
      if (first.pixel(x, y) != second.pixel(x, y)) {
        ++count;
      }
    }
  }
  return count;
}

} // namespace

int verifyThumbnailReuse() {
  QTemporaryDir directory;
  CHECK(directory.isValid());
  const QString redPath = directory.filePath(QStringLiteral("red.png"));
  const QString bluePath = directory.filePath(QStringLiteral("blue.png"));
  QImage image(120, 68, QImage::Format_RGB32);
  image.fill(Qt::red);
  CHECK(image.save(redPath));
  image.fill(Qt::blue);
  CHECK(image.save(bluePath));
  ControllerOperations operations;
  PlaylistModel model;
  PlaylistPanel panel(&model, nullptr, true, nullptr, false, &operations);
  auto *thumbnailer = panel.findChild<MediaThumbnailer *>();
  auto *list = panel.findChild<QListWidget *>();
  CHECK(thumbnailer && list);
  int completions = 0;
  QObject::connect(thumbnailer, &MediaThumbnailer::thumbnailReady, &panel,
                   [&](quint64, const MediaThumbnailResult &) { ++completions; });
  const auto settle = [] {
    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();
  };
  const auto waitForCompletion = [&](int expected) {
    QEventLoop loop;
    const auto connection = QObject::connect(
        thumbnailer, &MediaThumbnailer::thumbnailReady, &loop,
        [&](quint64, const MediaThumbnailResult &) {
          if (completions >= expected) loop.quit();
        });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    if (completions < expected) loop.exec();
    QObject::disconnect(connection);
    settle();
    return completions == expected;
  };
  const auto thumbnail = [&] {
    return list->item(0)->data(Qt::UserRole + 2).value<QPixmap>();
  };
  const int secondPlaylist = model.createPlaylist();
  CHECK(model.setActivePlaylistNumber(1));
  PlaylistItemRegistration registration;
  registration.filePath = redPath;
  registration.mediaType = PlaylistMediaType::StillImage;
  const auto ids = model.addItems({registration});
  CHECK(ids.size() == 1);

  // Round trips before the queued completion arrives must share one request.
  for (int repeat = 0; repeat < 20; ++repeat) {
    CHECK(model.setActivePlaylistNumber(secondPlaylist));
    CHECK(model.setActivePlaylistNumber(1));
  }
  CHECK(waitForCompletion(1));
  CHECK(thumbnail().toImage().pixelColor(60, 34) == QColor(Qt::red));
  CHECK(list->item(0)->data(playlist_view::kItemResolutionRole).toString() == QStringLiteral("120x68"));
  CHECK(list->item(0)->data(playlist_view::kItemMediaTypeRole).toString() == QStringLiteral("PNG"));
  auto trimmed = model.itemState(ids.front())->settings;
  trimmed.inPointMs = 1250;
  trimmed.outPointMs = 6500;
  trimmed.nominalVolumeTenthsDb = -60;
  CHECK(model.updateItemSettings(ids.front(), trimmed));
  CHECK(list->item(0)->data(playlist_view::kItemDurationRole).toString() == QStringLiteral("00:00:05.25"));
  CHECK(list->item(0)->data(playlist_view::kItemVolumeRole).toString() == QStringLiteral("50%"));
  const qint64 cachedKey = thumbnail().cacheKey();
  for (int repeat = 0; repeat < 20; ++repeat) {
    CHECK(model.setActivePlaylistNumber(secondPlaylist));
    CHECK(model.setActivePlaylistNumber(1));
  }
  settle();
  CHECK(completions == 1);
  CHECK(thumbnail().cacheKey() == cachedKey);
  CHECK(list->item(0)->data(playlist_view::kItemResolutionRole).toString() == QStringLiteral("120x68"));

  // Replacement still invalidates the cached image, including on hidden tabs.
  CHECK(model.setActivePlaylistNumber(secondPlaylist));
  registration.filePath = bluePath;
  CHECK(model.replaceItemAtomically(ids.front(), {registration}) == ids);
  CHECK(model.setActivePlaylistNumber(1));
  CHECK(waitForCompletion(2));
  CHECK(thumbnail().toImage().pixelColor(60, 34) == QColor(Qt::blue));

  // A restore can reuse the same item ID while an old request is in flight.
  const auto blueSnapshot = model.snapshot();
  registration.filePath = redPath;
  CHECK(model.replaceItemAtomically(ids.front(), {registration}) == ids);
  CHECK(model.restoreSnapshot(blueSnapshot));
  CHECK(waitForCompletion(3));
  CHECK(thumbnail().toImage().pixelColor(60, 34) == QColor(Qt::blue));

  // Offline recovery must regenerate even when the item had a cached image.
  CHECK(QFile::remove(bluePath));
  model.refreshMediaAvailability();
  CHECK(waitForAvailability(model, ids.front(), true));
  image.fill(Qt::green);
  CHECK(image.save(bluePath));
  model.refreshMediaAvailability();
  CHECK(waitForAvailability(model, ids.front(), false));
  CHECK(waitForCompletion(4));
  CHECK(thumbnail().toImage().pixelColor(60, 34) == QColor(Qt::green));

  CHECK(model.replaceItemAtomically(ids.front(), {registration}) == ids);
  CHECK(model.removeItem(ids.front()));
  settle();
  CHECK(completions == 4);
  qInfo() << "Thumbnail reuse: 20 pending and 20 cached tab round trips;"
             " replacement, restore, offline recovery and removal passed";
  return 0;
}

int verifySelectionOperations() {
  ControllerOperations operations;
  PlaylistModel model;
  PlaylistPanel panel(&model, nullptr, false, nullptr, false, &operations);
  using A = ControllerOperations::Action;
  using S = ControllerOperations::Status;
  QTemporaryFile first(QDir::tempPath() + QStringLiteral("/operation-a-XXXXXX.mp4"));
  QTemporaryFile second(QDir::tempPath() + QStringLiteral("/operation-b-XXXXXX.mp4"));
  CHECK(first.open() && second.open());
  PlaylistItemRegistration registration;
  registration.filePath = first.fileName();
  PlaylistItemRegistration other;
  other.filePath = second.fileName();
  const auto ids = model.addItems({registration, other});
  CHECK(ids.size() == 2);
  int edits = 0;
  QObject::connect(&panel, &PlaylistPanel::editRequested, &panel, [&] { ++edits; });
  panel.setEditActivationMode(PlaylistEditActivationMode::Selection);
  CHECK(operations.execute(A::SelectItemNumber, 2).invoked());
  CHECK(panel.selectedItemId() == ids[1]);
  CHECK(edits == 1);
  CHECK(operations.execute(A::SelectItemNumber, 0).status == S::Rejected);
  CHECK(operations.execute(A::SelectItemNumber, 3).status == S::Rejected);
  CHECK(panel.selectedItemId() == ids[1] && edits == 1);
  CHECK(operations.execute(A::CopySelectedState).invoked());
  CHECK(operations.execute(A::SelectItem, ids[0]).invoked());
  CHECK(operations.execute(A::PasteSelectedState).invoked());
  CHECK(operations.execute(A::CreatePlaylist).invoked());
  CHECK(model.activePlaylistNumber() != 1);
  CHECK(operations.execute(A::SelectItem, ids[0]).status == S::Rejected);
  CHECK(operations.execute(A::ActivatePlaylist, 1).invoked());
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(operations.execute(A::ActivatePlaylist, 99999).status == S::Rejected);
  CHECK(model.activePlaylistNumber() == 1);

  auto *list = panel.findChild<QListWidget *>(QStringLiteral("playlistList"));
  auto *nameEdit = panel.findChild<QLineEdit *>(QStringLiteral("playlistNameEdit"));
  CHECK(list && nameEdit);
  const int beforeRestoredClick = edits;
  CHECK(panel.selectedItemId() == ids[0]);
  list->itemClicked(list->item(0));
  CHECK(edits == beforeRestoredClick + 1);
  list->itemClicked(list->item(0));
  CHECK(edits == beforeRestoredClick + 1);
  CHECK(operations.execute(A::SelectItem, ids[1]).invoked());
  const int afterNewSelection = edits;
  list->itemClicked(list->item(1));
  CHECK(edits == afterNewSelection);
  CHECK(operations.execute(A::SelectItem, ids[0]).invoked());
  CHECK(operations.executeFrom("test-device", A::SetItemDisplayName, ids[0],
                               QStringLiteral("External title")).invoked());
  CHECK(operations.executeFrom("test-device", A::SetItemColor, ids[0],
                               PlaylistItemColor::Green).invoked());
  CHECK(operations.executeFrom("test-device", A::SetItemSeparatorBelow, ids[0], true).invoked());
  CHECK(list->item(0)->data(Qt::UserRole + 7).toString() == QStringLiteral("External title"));
  CHECK(list->item(0)->data(Qt::UserRole + 15).toInt() == static_cast<int>(PlaylistItemColor::Green));
  CHECK(list->item(0)->data(Qt::UserRole + 16).toBool());
  nameEdit->textEdited(QStringLiteral("From UI"));
  CHECK(model.playlistName(1) == QStringLiteral("From UI"));
  CHECK(operations.executeFrom("test-device", A::RenamePlaylist, 1,
                               QStringLiteral("From device")).invoked());
  CHECK(nameEdit->text() == QStringLiteral("From device"));
  operations.disable();
  nameEdit->textEdited(QStringLiteral("Disabled"));
  CHECK(model.playlistName(1) == QStringLiteral("From device"));
  return 0;
}

int verifyPlaylistOperationGuards() {
  ControllerOperations operations;
  PlaylistModel model;
  PlaylistPanel panel(&model, nullptr, false, nullptr, false, &operations);
  using A = ControllerOperations::Action;
  using S = ControllerOperations::Status;
  int accepted = 0;
  int bulkCalls = 0;
  operations.bind<PlaylistBulkOperation>(A::PlaylistBulk, &panel,
      [&](PlaylistBulkOperation) { ++bulkCalls; });
  QObject::connect(&operations, &ControllerOperations::operationAboutToExecute,
                   &panel, [&](A, const QVariantList &, const QString &) { ++accepted; });
  for (const auto operation : {PlaylistBulkOperation::PlayOnce,
                               PlaylistBulkOperation::RepeatAll,
                               PlaylistBulkOperation::Shuffle,
                               PlaylistBulkOperation::Clear,
                               PlaylistBulkOperation::DeletePlaylist,
                               PlaylistBulkOperation::Undo}) {
    CHECK(operations.executeFrom("Shortcut", A::PlaylistBulk, operation).status == S::Rejected);
  }
  CHECK(operations.executeFrom("Shortcut", A::SelectRelativeItem, 1).status == S::Rejected);
  CHECK(operations.executeFrom("Shortcut", A::SelectItemNumber, 1).status == S::Rejected);
  CHECK(operations.executeFrom("Shortcut", A::RemoveSelectedItem).status == S::Rejected);
  CHECK(accepted == 0 && bulkCalls == 0);
  CHECK(operations.executeFrom("Shortcut", A::PlaylistBulk, PlaylistBulkOperation::BgmMode).invoked());
  CHECK(operations.executeFrom("test-device", A::PlaylistBulk, PlaylistBulkOperation::StandardMode).invoked());
  panel.setBulkUndoAvailable(true);
  CHECK(operations.executeFrom("Shortcut", A::PlaylistBulk, PlaylistBulkOperation::Undo).invoked());
  model.createPlaylist();
  CHECK(operations.executeFrom("Shortcut", A::PlaylistBulk, PlaylistBulkOperation::DeletePlaylist).invoked());
  CHECK(bulkCalls == 4 && accepted == 4);

  QTemporaryFile media(QDir::tempPath() + QStringLiteral("/relative-selection-XXXXXX.mp4"));
  CHECK(media.open());
  PlaylistItemRegistration registration;
  registration.filePath = media.fileName();
  const auto ids = model.addItems({registration, registration, registration});
  CHECK(ids.size() == 3);
  CHECK(model.moveItemTo(ids[2], 0));
  auto *list = panel.findChild<QListWidget *>(QStringLiteral("playlistList"));
  CHECK(list);
  list->clearSelection();
  CHECK(operations.executeFrom("Shortcut", A::SelectRelativeItem, -1).invoked());
  CHECK(panel.selectedItemId() == ids[2]);
  CHECK(operations.executeFrom("test-device", A::SelectRelativeItem, 1).invoked());
  CHECK(panel.selectedItemId() == ids[0]);
  CHECK(operations.executeFrom("Shortcut", A::SelectRelativeItem, 1).invoked());
  CHECK(panel.selectedItemId() == ids[1]);
  const int beforeBoundary = accepted;
  CHECK(operations.executeFrom("Shortcut", A::SelectRelativeItem, 1).status == S::Rejected);
  CHECK(operations.executeFrom("Shortcut", A::SelectRelativeItem, 0).status == S::Rejected);
  CHECK(accepted == beforeBoundary);
  QAction *playOnce = nullptr;
  for (auto *action : panel.bulkOperationsMenu()->actions()) {
    if (action->data().isValid() &&
        action->data().toInt() == static_cast<int>(PlaylistBulkOperation::PlayOnce)) playOnce = action;
  }
  CHECK(playOnce);
  playOnce->setEnabled(false); // Presentation cannot veto an allowed shared operation.
  CHECK(operations.executeFrom("Shortcut", A::PlaylistBulk, PlaylistBulkOperation::PlayOnce).invoked());
  CHECK(bulkCalls == 5);
  return 0;
}

int verifyBulkUndoPresentation() {
  ControllerOperations operations;
  PlaylistModel model;
  PlaylistPanel panel(&model, nullptr, false, nullptr, false, &operations);
  QObject::connect(&model, &PlaylistModel::bulkUndoAvailableChanged,
                   &panel, &PlaylistPanel::setBulkUndoAvailable);
  QTemporaryFile image(QDir::tempPath() + QStringLiteral("/undo-XXXXXX.png"));
  CHECK(createImageFile(image, Qt::blue));
  PlaylistItemRegistration registration;
  registration.filePath = image.fileName();
  registration.mediaType = PlaylistMediaType::StillImage;
  auto pdf = registration;
  pdf.sourceType = PlaylistSourceType::Pdf;
  pdf.mediaType = PlaylistMediaType::Pdf;
  auto powerPoint = registration;
  powerPoint.sourceType = PlaylistSourceType::PowerPoint;
  powerPoint.mediaType = PlaylistMediaType::Presentation;
  powerPoint.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
  PlaylistItemRegistration camera;
  camera.sourceType = PlaylistSourceType::Uvc;
  camera.filePath = QStringLiteral("bulk-undo-camera");
  camera.displayName = QStringLiteral("Camera");
  const auto ids = model.addItems({registration, registration, pdf, powerPoint, camera});
  CHECK(ids.size() == 5);
  auto *list = panel.findChild<QListWidget *>(QStringLiteral("playlistList"));
  CHECK(list);
  CHECK(operations.execute(ControllerOperations::Action::SelectItem, ids[1]).invoked());
  const auto before = model.snapshot();
  CHECK(model.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::Shuffle));
  model.rememberBulkOperation(before, 1);
  CHECK(model.setItemDisplayName(ids[1], QStringLiteral("Name after Shuffle")));
  CHECK(model.undoBulkOperation());
  CHECK(panel.selectedItemId() == ids[1]);
  for (int row = 0; row < ids.size(); ++row) {
    CHECK(list->item(row)->data(Qt::UserRole).toULongLong() == ids[row]);
    CHECK(list->item(row)->data(Qt::UserRole + 6).toInt() == row + 1);
  }
  CHECK(list->item(1)->data(Qt::UserRole + 7).toString() == QStringLiteral("Name after Shuffle"));
  const auto startModeBefore = list->item(1)->data(Qt::UserRole + 12);
  const auto beforePlayOnce = model.snapshot();
  CHECK(model.applyBulkPlaybackOperation(1, PlaylistBulkPlaybackOperation::PlayOnce));
  model.rememberBulkOperation(beforePlayOnce, 1);
  auto laterSettings = model.itemState(ids[1])->settings;
  laterSettings.endMode = PlaylistEndMode::Goto;
  laterSettings.gotoTargetItemId = ids[0];
  CHECK(model.updateItemSettings(ids[1], laterSettings));
  const auto laterEndMode = list->item(1)->data(Qt::UserRole + 13);
  bool keptLaterChanges = false;
  CHECK(model.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(keptLaterChanges && panel.selectedItemId() == ids[1]);
  CHECK(list->item(1)->data(Qt::UserRole + 12) == startModeBefore);
  CHECK(list->item(1)->data(Qt::UserRole + 13) == laterEndMode);
  CHECK(model.itemState(ids[1])->settings.gotoTargetItemId == ids[0]);
  const auto beforeClear = model.snapshot();
  for (const auto id : ids) CHECK(model.removeItem(id));
  model.rememberBulkOperation(beforeClear, 1);
  const auto added = model.addItems({registration});
  CHECK(operations.execute(ControllerOperations::Action::SelectItem, added[0]).invoked());
  CHECK(model.undoBulkOperation());
  CHECK(list->count() == ids.size() + 1 && panel.selectedItemId() == added[0]);
  for (int row = 0; row < list->count(); ++row) {
    CHECK(list->item(row)->data(Qt::UserRole).toULongLong() == model.itemIdAt(row));
    CHECK(list->item(row)->data(Qt::UserRole + 6).toInt() == row + 1);
  }
  return 0;
}

int verifyPlaylistDeletion() {
  PlaylistModel model;
  ControllerOperations operations;
  PlaylistPanel panel(&model, nullptr, false, nullptr, false, &operations);
  QObject::connect(&model, &PlaylistModel::bulkUndoAvailableChanged,
                   &panel, &PlaylistPanel::setBulkUndoAvailable);
  auto *tabs = panel.findChild<QTabBar *>(QStringLiteral("playlistTabs"));
  auto *combo = panel.findChild<QComboBox *>(QStringLiteral("playlistBulkOperationComboBox"));
  auto *apply = panel.findChild<QPushButton *>(QStringLiteral("playlistBulkApplyButton"));
  CHECK(tabs && combo && apply);
  const int deleteIndex = combo->findData(static_cast<int>(PlaylistBulkOperation::DeletePlaylist));
  CHECK(deleteIndex >= 0);
  combo->setCurrentIndex(deleteIndex);
  CHECK(!apply->isEnabled());
  CHECK(!model.removeEmptyPlaylist(1));
  CHECK(!model.removeEmptyPlaylist(999));
  const int second = model.createPlaylist();
  CHECK(apply->isEnabled());
  CHECK(model.setActivePlaylistNumber(second));
  CHECK(model.setPlaylistName(second, QStringLiteral("Delete and restore")));
  CHECK(model.setPlaylistMode(second, PlaylistMode::Bgm));
  model.setManualCrossfadeEnabled(second, true);
  const auto emptyBefore = model.snapshot();
  CHECK(model.removeEmptyPlaylist(second));
  model.rememberBulkOperation(emptyBefore, second);
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(tabs->count() == 2); // One playlist and the add tab.
  CHECK(!apply->isEnabled());
  bool keptLaterChanges = true;
  CHECK(model.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(!keptLaterChanges);
  CHECK(model.playlists() == emptyBefore.playlists);
  CHECK(tabs->count() == 3 && apply->isEnabled());

  CHECK(model.setActivePlaylistNumber(second));
  QTemporaryFile image(QDir::tempPath() + QStringLiteral("/delete-playlist-XXXXXX.png"));
  CHECK(createImageFile(image, Qt::blue));
  PlaylistItemRegistration registration;
  registration.filePath = image.fileName();
  registration.mediaType = PlaylistMediaType::StillImage;
  registration.playlistNumber = second;
  const auto ids = model.addItems({registration, registration});
  CHECK(ids.size() == 2);
  CHECK(!model.removeEmptyPlaylist(second));
  const auto before = model.snapshot();
  for (const auto id : ids) CHECK(model.removeItem(id));
  CHECK(model.removeEmptyPlaylist(second));
  model.rememberBulkOperation(before, second);
  const int later = model.createPlaylist();
  CHECK(later > second); // Creating a tab must not reuse the undo target.
  CHECK(model.setPlaylistName(later, QStringLiteral("Keep later changes")));
  CHECK(model.setActivePlaylistNumber(later));
  int requestCount = 0;
  QObject::connect(&panel, &PlaylistPanel::bulkOperationRequested, &panel,
      [&](PlaylistBulkOperation operation) {
        if (operation == PlaylistBulkOperation::DeletePlaylist) ++requestCount;
      });
  combo->setCurrentIndex(deleteIndex);
  apply->click();
  CHECK(requestCount == 1 && combo->currentIndex() == 0);
  CHECK(model.undoBulkOperation(nullptr, &keptLaterChanges));
  CHECK(!keptLaterChanges);
  CHECK(model.activePlaylistNumber() == later);
  CHECK(model.playlistName(later) == QStringLiteral("Keep later changes"));
  CHECK(model.itemCount(second) == 2);
  for (const auto &item : before.items) CHECK(model.itemState(item.id) == item);
  CHECK(model.playlists()[1] == before.playlists[1]);
  CHECK(tabs->tabData(1).toInt() == second);
  CHECK(tabs->tabData(2).toInt() == later);
  CHECK(PlaylistModel::canRestoreSnapshot(model.snapshot()));
  CHECK(model.setActivePlaylistNumber(second));
  CHECK(panel.itemCount() == 2);
  return 0;
}

int verifyAddonRestartPrompt(QApplication &application,
                            ControllerSettingsDialog &settings) {
  QTranslator japanese;
  const QDir binaryDirectory(QCoreApplication::applicationDirPath());
  const QString parentCatalog = binaryDirectory.filePath("../vtrpon_ja.qm");
  CHECK(japanese.load(QFileInfo::exists(parentCatalog) ? parentCatalog :
                       binaryDirectory.filePath("vtrpon_ja.qm")));
  CHECK(application.installTranslator(&japanese));
  auto *page = settings.findChild<AddonManagementPage *>("addonManagementPage");
  CHECK(page);
  const QString originalSkin = vtrpon::currentControllerSkinKey();
  int requests = 0;
  const auto connection = QObject::connect(&settings, &ControllerSettingsDialog::addonRestartRequested,
      &settings, [&requests] { ++requests; });
  for (const auto &skin : vtrpon::availableControllerSkins()) {
    CHECK(vtrpon::applyControllerSkin(application, skin.key));
    for (const auto answer : {QMessageBox::No, QMessageBox::Yes}) {
      bool verified = false;
      QTimer::singleShot(0, page, [&, answer] {
        auto *question = page->findChild<QMessageBox *>("addonRestartDialog");
        if (!question) return;
        verified = question->text() == QStringLiteral(
            "アドオンを有効にするためにはVTR-PONの再起動が必要です。\n再起動していいですか？") &&
            question->button(QMessageBox::Yes)->text().remove(QLatin1Char('&')) == QStringLiteral("Yes") &&
            question->button(QMessageBox::No)->text().remove(QLatin1Char('&')) == QStringLiteral("No") &&
            question->defaultButton() == question->button(QMessageBox::No) &&
            question->palette().color(QPalette::Window) == QColor("#292828");
        if (!verified) {
          std::fprintf(stderr, "restart question=[%s] yes=[%s] no=[%s] default=%d background=%s\n",
              qPrintable(question->text()), qPrintable(question->button(QMessageBox::Yes)->text()),
              qPrintable(question->button(QMessageBox::No)->text()),
              question->defaultButton() == question->button(QMessageBox::No),
              qPrintable(question->palette().color(QPalette::Window).name()));
        }
        const QString captureDirectory = qEnvironmentVariable("VTRPON_ADDON_RESTART_CAPTURE_DIR");
        if (!captureDirectory.isEmpty() && answer == QMessageBox::No) {
          verified = QDir().mkpath(captureDirectory) &&
              question->grab().save(QDir(captureDirectory).filePath(skin.key + ".png")) && verified;
        }
        question->button(answer)->click();
      });
      const int before = requests;
      CHECK(QMetaObject::invokeMethod(page, "promptRestart", Qt::DirectConnection));
      CHECK(verified);
      CHECK(requests == before + (answer == QMessageBox::Yes ? 1 : 0));
    }
  }
  QObject::disconnect(connection);
  CHECK(vtrpon::applyControllerSkin(application, originalSkin));
  CHECK(application.removeTranslator(&japanese));
  application.processEvents();
  return 0;
}

int verifySettingsStyle(QApplication &application,
                        ControllerSettingsDialog &dialog) {
  const QString originalSheet = application.styleSheet();
  const QString originalDialogSheet = dialog.styleSheet();
  const QFont originalFont = application.font();
  const QPalette originalPalette = application.palette();
  const QString originalSkin = vtrpon::currentControllerSkinKey();
  const QVariant originalSkinProperty = application.property("controllerSkinKey");
  const QVariant originalHardwareProperty = application.property("controllerSkinUsesHardwareEffects");
  const QVariant originalUnscaledSheet = application.property("controllerUnscaledStyleSheet");
  const int originalTextSize = vtrpon::uiTextSize();
  const qreal originalScale = vtrpon::uiScale();
  vtrpon::setUiScale(1.0);

  // Exercise the optional NDI page even when its addon is not installed.
  std::unique_ptr<NetworkVideoSettingsPage> networkProbe;
  QWidget *networkPage = dialog.findChild<QWidget *>(QStringLiteral("networkVideoSettingsPage"));
  if (!networkPage) {
    networkProbe = std::make_unique<NetworkVideoSettingsPage>(&dialog);
    networkPage = networkProbe.get();
    networkPage->hide();
  }
  QWidget *operatorPage = dialog.findChild<QWidget *>(QStringLiteral("operatorMonitorSettingsPage"));
  QWidget *patternPage = dialog.findChild<QWidget *>(QStringLiteral("testPatternSettingsPage"));
  CHECK(operatorPage && patternPage);
  auto *shortcutTable = dialog.findChild<QTableWidget *>(QStringLiteral("shortcutBindingsTable"));
  CHECK(shortcutTable && shortcutTable->styleSheet().isEmpty());
  for (QWidget *page : {operatorPage, networkPage, patternPage}) {
    CHECK(page->styleSheet().isEmpty());
    for (QWidget *child : page->findChildren<QWidget *>())
      CHECK(child->styleSheet().isEmpty());
  }

  const auto foreground = [](QWidget *widget) {
    widget->ensurePolished();
    return widget->palette().color(widget->isEnabled() ? QPalette::Active : QPalette::Disabled,
                                    widget->foregroundRole());
  };
  const auto repolish = [](QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
  };
  const QColor enabledColor(QStringLiteral("#FFFFFF"));
  const QColor disabledColor(QStringLiteral("#8A8A8A"));
  const QColor descriptionColor(QStringLiteral("#BBBBBB"));
  const QColor errorColor(QStringLiteral("#f28b82"));
  std::array<QImage, 3> settingsControls[2];

  for (const auto &skin : vtrpon::availableControllerSkins()) {
    for (int textSize : {0, 2}) {
      vtrpon::setUiTextSize(textSize);
      CHECK(vtrpon::applyControllerSkin(application, skin.key));
      // A Controller control outside Settings must retain its skin, even if
      // it happens to use the same semantic role as a Settings label.
      QWidget outside;
      outside.setObjectName(QStringLiteral("controllerWindow"));
      QComboBox outsideCombo(&outside);
      QLabel outsideLabel(QStringLiteral("Outside"), &outside);
      vtrpon::setSettingsRole(&outsideLabel, "pageTitle");
      const QColor outsideColor = foreground(&outsideCombo);
      const QFont outsideFont = outsideCombo.font();
      const QColor outsideLabelColor = foreground(&outsideLabel);
      const QFont outsideLabelFont = outsideLabel.font();
      dialog.setStyleSheet(vtrpon::settingsStyleSheet());
      CHECK(!dialog.styleSheet().isEmpty());
      CHECK(foreground(&outsideCombo) == outsideColor);
      CHECK(outsideCombo.font() == outsideFont);
      CHECK(foreground(&outsideLabel) == outsideLabelColor);
      CHECK(outsideLabel.font() == outsideLabelFont);

      // These controls previously depended on whichever Controller skin
      // happened to be active instead of sharing the Settings rules.
      for (const char *name : {"gpuAdapterComboBox", "timeZoneCombo",
                               "timeRemainingDefaultCombo", "currentTimeFontCombo",
                               "currentTimeFontSize", "defaultAudioFallbackCheckBox",
                               "useVstPluginsCheckBox", "virtualCameraEnabled",
                               "operatorMonitorDisplayCombo", "operatorMonitorInformation"}) {
        auto *control = dialog.findChild<QWidget *>(QString::fromLatin1(name));
        CHECK(control && control->styleSheet().isEmpty());
        CHECK(foreground(control) == (control->isEnabled() ? enabledColor : disabledColor));
        CHECK(control->font().pixelSize() == vtrpon::scaleTextPx(12));
      }
      CHECK(dialog.findChild<QFontComboBox *>(QStringLiteral("currentTimeFontCombo")));
      for (QWidget *page : {operatorPage, networkPage, patternPage}) {
        int titles = 0;
        int descriptions = 0;
        for (QLabel *label : page->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly)) {
          const QString role = label->property("settingsRole").toString();
          if (role == QStringLiteral("pageTitle")) {
            ++titles;
            CHECK(foreground(label) == enabledColor);
            CHECK(label->font().pixelSize() == vtrpon::scaleTextPx(20));
          } else if (role == QStringLiteral("description") || role == QStringLiteral("status")) {
            ++descriptions;
            CHECK(foreground(label) == descriptionColor);
            CHECK(label->font().pixelSize() == vtrpon::scaleTextPx(11));
          }
        }
        CHECK(titles == 1 && descriptions > 0);
      }

      // New controls created after styling need no object-name allowlist or
      // page-local stylesheet, including controls nested inside a new page.
      QWidget newPage(&dialog);
      vtrpon::setSettingsRole(&newPage, "page");
      QWidget nested(&newPage);
      QComboBox newCombo(&nested);
      QCheckBox newCheck(QStringLiteral("New setting"), &nested);
      QLineEdit newLine(&nested);
      QSpinBox newSpin(&nested);
      QPushButton newButton(QStringLiteral("New action"), &nested);
      for (QWidget *control : std::array<QWidget *, 4>{&newCombo, &newCheck, &newLine, &newSpin}) {
        CHECK(control->objectName().isEmpty() && control->styleSheet().isEmpty());
        CHECK(foreground(control) == enabledColor);
        CHECK(control->font().pixelSize() == vtrpon::scaleTextPx(12));
        control->setEnabled(false);
        repolish(control);
        CHECK(foreground(control) == disabledColor);
        control->setEnabled(true);
        repolish(control);
        CHECK(foreground(control) == enabledColor);
      }
      auto *existingCombo = dialog.findChild<QComboBox *>(QStringLiteral("playlistEditModeComboBox"));
      auto *existingButton = dialog.findChild<QPushButton *>(QStringLiteral("closeSettingsButton"));
      CHECK(existingCombo && existingButton);
      CHECK(foreground(&newCombo) == foreground(existingCombo));
      CHECK(newCombo.font() == existingCombo->font());
      CHECK(foreground(&newButton) == foreground(existingButton));
      CHECK(newButton.font().pixelSize() == existingButton->font().pixelSize());
      // Arrow/subcontrol rendering must also remain Clasic when the global
      // Controller palette changes, not only the text covered by QSS.
      newCombo.addItems({QStringLiteral("First"), QStringLiteral("Second")});
      newSpin.setValue(5);
      const std::array<QWidget *, 3> controls{&newCombo, &newSpin, &newButton};
      for (size_t i = 0; i < controls.size(); ++i) {
        controls[i]->resize(180, 36);
        const QImage rendered = controls[i]->grab().toImage();
        QImage &reference = settingsControls[textSize / 2][i];
        if (reference.isNull()) reference = rendered;
        CHECK(rendered == reference);
      }
      newButton.setEnabled(false);
      repolish(&newButton);
      CHECK(foreground(&newButton) == QColor(QStringLiteral("#B0B0B0")));

      QLabel status(QStringLiteral("Status"), &nested);
      vtrpon::setSettingsRole(&status, "status");
      CHECK(foreground(&status) == descriptionColor);
      const QString capture = qEnvironmentVariable("VTRPON_SETTINGS_UI_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        auto *navigation = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigationList"));
        CHECK(navigation && QDir().mkpath(capture));
        const int previousRow = navigation->currentRow();
        const int previousWidth = navigation->parentWidget()->width();
        const bool wasVisible = dialog.isVisible();
        newPage.hide();
        navigation->parentWidget()->setFixedWidth(vtrpon::scaleTextPx(190));
        for (int row = 0; row < navigation->count(); ++row) {
          if (navigation->item(row)->data(Qt::UserRole).toString() == QStringLiteral("time_language")) {
            navigation->setCurrentRow(row);
            break;
          }
        }
        dialog.show();
        application.processEvents();
        CHECK(dialog.grab().save(QDir(capture).filePath(
            QStringLiteral("settings-%1-%2.png").arg(skin.key).arg(textSize))));
        navigation->setCurrentRow(previousRow);
        navigation->parentWidget()->setFixedWidth(previousWidth);
        dialog.setVisible(wasVisible);
      }
      status.setProperty("error", true);
      repolish(&status);
      CHECK(foreground(&status) == errorColor);
      status.setProperty("error", false);
      repolish(&status);
      CHECK(foreground(&status) == descriptionColor);
      vtrpon::setSettingsRole(&status, "error");
      CHECK(foreground(&status) == errorColor);
      vtrpon::setSettingsRole(&status, "status");
      CHECK(foreground(&status) == descriptionColor);
    }
  }

  vtrpon::setUiScale(originalScale);
  vtrpon::setUiTextSize(originalTextSize);
  CHECK(vtrpon::applyControllerSkin(application, originalSkin));
  application.setStyleSheet(originalSheet);
  application.setFont(originalFont);
  application.setPalette(originalPalette);
  application.setProperty("controllerSkinKey", originalSkinProperty);
  application.setProperty("controllerSkinUsesHardwareEffects", originalHardwareProperty);
  application.setProperty("controllerUnscaledStyleSheet", originalUnscaledSheet);
  dialog.setStyleSheet(originalDialogSheet);
  return 0;
}

int verifyPlaylistWindowScaling(QApplication &application) {
  QFile skin(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
      .filePath(QStringLiteral("../apps/desktop/skins/hardware/controller.qss")));
  CHECK(skin.open(QIODevice::ReadOnly));
  const QString originalSheet = application.styleSheet();
  const QFont originalFont = application.font();
  application.setStyleSheet(QString::fromUtf8(skin.readAll()));
  {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    PlaylistModel model;
    auto *panel = new PlaylistPanel(&model, &window, false, nullptr, false);
    layout->addWidget(panel);
    QTemporaryFile media(QDir::tempPath() + QStringLiteral("/resize-XXXXXX.mp4"));
    CHECK(media.open());
    PlaylistItemRegistration item;
    item.filePath = media.fileName();
    const auto ids = model.addItems({item, item, item});
    CHECK(ids.size() == 3);
    auto *list = panel->findChild<QListWidget *>(QStringLiteral("playlistList"));
    auto *tabs = panel->findChild<QTabBar *>(QStringLiteral("playlistTabs"));
    CHECK(list && tabs);
    list->setCurrentRow(1);
    window.resize(750, 950);
    window.show();
    application.processEvents();
    const QSize normalSize = window.size();
    const int rowHeight = list->visualItemRect(list->item(0)).height();
    const auto before = model.snapshot();
    vtrpon::ControllerWindowScaling scaling(&window);
    const auto resizeAndWait = [&](qreal ratio) {
      window.resize(qRound(normalSize.width() * ratio), qRound(normalSize.height() * ratio));
      QElapsedTimer wait;
      wait.start();
      while (qAbs(vtrpon::uiScale() - ratio) > 0.003 && wait.elapsed() < 2000) {
        application.processEvents();
        QThread::msleep(2);
      }
      application.processEvents();
      return qAbs(vtrpon::uiScale() - ratio) < 0.003;
    };
    CHECK(resizeAndWait(0.75));
    CHECK(qAbs(list->visualItemRect(list->item(0)).height() - qRound(rowHeight * 0.75)) <= 1);
    CHECK(tabs->height() >= tabs->tabRect(0).height());
    auto *tileButton = panel->findChild<QToolButton *>(QStringLiteral("playlistTileViewButton"));
    auto *listButton = panel->findChild<QToolButton *>(QStringLiteral("playlistListViewButton"));
    CHECK(tileButton && listButton);
    tileButton->click();
    const QSize smallGrid = list->gridSize();
    CHECK(resizeAndWait(1.0));
    CHECK(qAbs(list->gridSize().width() - smallGrid.width() / 0.75) <= 1);
    CHECK(list->viewMode() == QListView::IconMode);
    CHECK(list->dragEnabled() && list->acceptDrops());
    CHECK(panel->selectedItemId() == ids[1]);
    CHECK(model.snapshot() == before);
    listButton->click();
    CHECK(resizeAndWait(0.75));
    CHECK(resizeAndWait(1.0));
    CHECK(list->gridSize().isEmpty());
    CHECK(list->visualItemRect(list->item(0)).height() == rowHeight);
    CHECK(panel->selectedItemId() == ids[1]);
    CHECK(tabs->height() >= tabs->tabRect(0).height());
    CHECK(model.snapshot() == before);
    window.close();
  }
  vtrpon::setUiScale(1.0);
  application.setStyleSheet(originalSheet);
  application.setFont(originalFont);
  return 0;
}

int baseNetworkSettingsRow(int original) {
  const bool ndi = vtrpon::networkVideoAddonInstalled();
  switch (original) {
  case 0: return 0;  // Playlist Settings
  case 1: return 1;  // ON AIR Settings
  case 2: return ndi ? 7 : 6;  // Audio Output
  case 3: return 2;  // Video Output
  case 4: return ndi ? 4 : 3;  // Virtual Camera
  case 5: return ndi ? 8 : 7;  // UVC Audio Mapping
  case 6: return ndi ? 9 : 8;  // BGM Player
  case 7: return ndi ? 5 : 4;  // Switcher Control
  case 8: return ndi ? 12 : 11;  // Shortcut Keys
  case 9:
  case 10: return ndi ? 10 : 9;  // Appearance / former Background
  case 11: return ndi ? 14 : 13;  // Test Pattern
  case 12: return ndi ? 11 : 10;  // Language & Time
  case 13: return ndi ? 6 : 5;  // Return Monitor
  case 14: return ndi ? 17 : 16;  // Save / Reset
  case 15: return ndi ? 19 : 18;  // Category count
  default: return original;
  }
}

int networkSettingsRow(int original) {
  auto row = baseNetworkSettingsRow(original);
  if (vtrpon::AtemAddonSwitcher::addonInstalled() && row >= baseNetworkSettingsRow(7)) ++row;
  return row + (row >= 3 && vtrpon::deckLinkAddonInstalled() ? 1 : 0);
}

int verifyStillCaptureControls(QApplication &application) {
  const auto previousSkin = vtrpon::currentControllerSkinKey();
  for (const auto &language : {QStringLiteral("en"), QStringLiteral("ja")}) {
    QTranslator translator;
    CHECK(translator.load(QCoreApplication::applicationDirPath() + "/vtrpon_" + language + ".qm"));
    CHECK(application.installTranslator(&translator));
    for (const auto &skin : vtrpon::availableControllerSkins()) {
      CHECK(vtrpon::applyControllerSkin(application, skin.key));
      ControllerOperations operations;
      InputControlPanel panel(nullptr, &operations);
      panel.setNetworkVideoSources({{QStringLiteral("Camera"), QStringLiteral("ndi://fixture")}}, true);
      auto *preview = panel.findChild<QPushButton *>("previewCaptureButton");
      auto *program = panel.findChild<QPushButton *>("programCaptureButton");
      auto *duration = panel.findChild<QDoubleSpinBox *>("stillCaptureDuration");
      CHECK(preview && program && duration);
      panel.show();
      application.processEvents();
      CHECK(preview->isVisible() && program->isVisible() && duration->isVisible());
      CHECK(preview->fontMetrics().horizontalAdvance(preview->text()) < preview->width());
      CHECK(program->fontMetrics().horizontalAdvance(program->text()) < program->width());
      CHECK(panel.rect().contains(duration->mapTo(&panel, duration->rect().bottomRight())));
      QList<QPair<bool, qint64>> requests;
      QStringList operationSources;
      int captureClicks = 0;
      QObject::connect(&operations, &ControllerOperations::operationAboutToExecute, &panel,
          [&](ControllerOperations::Action, const QVariantList &, const QString &source) {
            operationSources.append(source);
          });
      QObject::connect(preview, &QPushButton::clicked, &panel, [&] { ++captureClicks; });
      QObject::connect(program, &QPushButton::clicked, &panel, [&] { ++captureClicks; });
      // The UI represents tenths of a second; direct input and arrow controls share the value.
      duration->setValue(3.5);
      QObject::connect(&panel, &InputControlPanel::stillCaptureRequested, &panel,
          [&](bool isProgram, qint64 ms) { requests.append({isProgram, ms}); });
      preview->click(); program->click();
      CHECK(requests.size() == 2 && captureClicks == 2);
      CHECK(!requests[0].first && requests[0].second == 3500);
      CHECK(requests[1].first && requests[1].second == 3500);
      CHECK(operationSources == QStringList({QStringLiteral("UI"), QStringLiteral("UI")}));
      panel.setStillCaptureBusy(true);
      CHECK(!preview->isEnabled() && !program->isEnabled());
      using A = ControllerOperations::Action;
      using S = ControllerOperations::Status;
      // A stale presentation state cannot bypass the semantic busy guard.
      preview->setEnabled(true);
      program->setEnabled(true);
      CHECK(operations.executeFrom("Shortcut", A::PreviewCapture).status == S::Rejected);
      CHECK(operations.executeFrom("Shortcut", A::ProgramCapture).status == S::Rejected);
      CHECK(requests.size() == 2 && operationSources.size() == 2 && captureClicks == 2);
      panel.setStillCaptureBusy(false);
      panel.hide();
      preview->setEnabled(false);
      program->setEnabled(false);
      auto *durationEditor = duration->findChild<QLineEdit *>();
      CHECK(durationEditor);
      durationEditor->setText(duration->locale().toString(7.2, 'f', 1) + duration->suffix());
      CHECK(operations.executeFrom("Shortcut", A::PreviewCapture).invoked());
      CHECK(operations.executeFrom("Shortcut", A::ProgramCapture).invoked());
      CHECK(requests.size() == 4 && captureClicks == 2);
      CHECK(!requests[2].first && requests[2].second == 7200);
      CHECK(requests[3].first && requests[3].second == 7200);
      CHECK(operationSources.mid(2) == QStringList({QStringLiteral("Shortcut"), QStringLiteral("Shortcut")}));
      panel.setStillCaptureBusy(false);
      panel.show();
      application.processEvents();
      const auto captureDirectory = qEnvironmentVariable("VTRPON_STILL_CAPTURE_UI_DIR");
      if (!captureDirectory.isEmpty()) {
        QDir().mkpath(captureDirectory);
        CHECK(panel.grab().save(QDir(captureDirectory).filePath(skin.key + "-" + language + ".png")));
      }
    }
    CHECK(application.removeTranslator(&translator));
  }
  FileManagementSettingsPage files(nullptr);
  auto *still = files.findChild<QLineEdit *>("stillCaptureDirectory");
  auto *documents = files.findChild<QLineEdit *>("documentConversionDirectory");
  CHECK(still && documents && !still->placeholderText().isEmpty() && !documents->placeholderText().isEmpty());
  const auto path = QDir::tempPath() + "/vtrpon-custom-captures";
  still->setText(path);
  QMetaObject::invokeMethod(still, "editingFinished");
  CHECK(vtrpon::stillCaptureDirectory() == path);
  still->setText("relative/path");
  QMetaObject::invokeMethod(still, "editingFinished");
  CHECK(vtrpon::stillCaptureDirectory() == path);
  files.reload();
  CHECK(QDir::fromNativeSeparators(still->text()) == path);
  still->clear();
  QMetaObject::invokeMethod(still, "editingFinished");
  CHECK(vtrpon::stillCaptureDirectory() == vtrpon::defaultStillCaptureDirectory());
  CHECK(vtrpon::applyControllerSkin(application, previousSkin));
  return 0;
}

int verifyLiveAudioInputControls() {
  QTranslator english;
  CHECK(english.load(QCoreApplication::applicationDirPath() + QStringLiteral("/vtrpon_en.qm")));
  QCoreApplication::installTranslator(&english);
  InputControlPanel input;
  auto *combo = input.findChild<QComboBox *>(QStringLiteral("inputUvcDeviceComboBox"));
  auto *add = input.findChild<QPushButton *>(QStringLiteral("inputUvcAddButton"));
  CHECK(combo && add);
  const QString id = QStringLiteral("USB endpoint / 日本語 %1");
  const QString path = vtrpon::liveAudioSourcePath(id);
  CHECK(vtrpon::liveAudioDeviceId(path) == id);
  CHECK(vtrpon::liveAudioDeviceId(QStringLiteral("live-audio://bad%value")).isEmpty());
  input.setAudioInputDevices({{QStringLiteral("USB microphone"), id, false},
                             {QStringLiteral("USB microphone"), QStringLiteral("other-id"), false}});
  CHECK(combo->isEnabled() && add->isEnabled());
  CHECK(combo->currentData().toString() == path);
  CHECK(combo->currentText().contains(QStringLiteral("(1)")));
  CHECK(combo->itemText(combo->findData(vtrpon::liveAudioSourcePath(QStringLiteral("other-id")))).contains(QStringLiteral("(2)")));
  QString emittedPath, emittedName;
  QObject::connect(&input, &InputControlPanel::uvcAddRequested, &input,
      [&](const QString &name, const QString &value) { emittedName = name; emittedPath = value; });
  add->click();
  CHECK(emittedPath == path && emittedName == QStringLiteral("USB microphone"));
  input.setUvcDevices({{QStringLiteral("Camera"), QStringLiteral("camera-path")}});
  CHECK(combo->currentData().toString() == path);
  input.setUvcCaptureBusy(true);
  CHECK(!combo->isEnabled() && !add->isEnabled());
  input.setUvcCaptureBusy(false);
  input.setAudioInputDevices({});
  CHECK(combo->currentData().toString() == QStringLiteral("camera-path"));
  input.setUvcDevices({});
  CHECK(!combo->isEnabled() && !add->isEnabled());
  auto *title = input.findChild<QLabel *>(QStringLiteral("inputControlModuleTitle"));
  CHECK(title && title->text() == QStringLiteral("UVC/UAC INPUT"));
  QTranslator japanese;
  CHECK(japanese.load(QCoreApplication::applicationDirPath() + QStringLiteral("/vtrpon_ja.qm")));
  QCoreApplication::installTranslator(&japanese);
  QApplication::processEvents();
  CHECK(title->text() == QStringLiteral("UVC/UAC入力"));
  QCoreApplication::removeTranslator(&japanese);
  QApplication::processEvents();

  PlaylistModel model;
  PlaylistThumbnailStore thumbnails(&model, false);
  PlaylistPanel playlist(&model, nullptr, false);
  PlaylistItemRegistration item;
  item.filePath = path;
  item.displayName = QStringLiteral("USB microphone");
  item.sourceType = PlaylistSourceType::Uvc;
  item.mediaType = PlaylistMediaType::Audio;
  const auto ids = model.addItems({item});
  CHECK(ids.size() == 1);
  thumbnails.request(ids[0]);
  const auto image = thumbnails.thumbnail(ids[0]).toImage();
  auto *list = playlist.findChild<QListWidget *>(QStringLiteral("playlistList"));
  CHECK(list && list->count() == 1);
  CHECK(list->item(0)->data(playlist_view::kItemMediaTypeRole).toString() == QStringLiteral("Live Audio"));
  CHECK(list->item(0)->data(Qt::UserRole + 2).value<QPixmap>().toImage() == image);
  auto *tileView = playlist.findChild<QToolButton *>(QStringLiteral("playlistTileViewButton"));
  auto *listView = playlist.findChild<QToolButton *>(QStringLiteral("playlistListViewButton"));
  CHECK(tileView && listView);
  tileView->click();
  CHECK(list->viewMode() == QListView::IconMode);
  CHECK(list->item(0)->data(Qt::UserRole + 2).value<QPixmap>().toImage() == image);
  listView->click();
  CHECK(!image.isNull() && thumbnails.errorDetail(ids[0]).isEmpty());
  CHECK(!thumbnails.sourceDurationMs(ids[0]) && thumbnails.sourceSize(ids[0]).isEmpty());
  auto bad = item;
  bad.filePath = QStringLiteral("live-audio://bad%value");
  CHECK(model.addItems({bad}).isEmpty());
  auto settings = model.itemState(ids[0])->settings;
  settings.outPointMs = 1000;
  CHECK(!model.updateItemSettings(ids[0], settings));
  QString detail;
  CHECK(model.restoreSnapshot(model.snapshot(), &detail));
  thumbnails.request(ids[0]);
  CHECK(thumbnails.thumbnail(ids[0]).toImage() == image);
  CHECK(list->item(0)->data(Qt::UserRole + 2).value<QPixmap>().toImage() == image);
  model.setUvcDeviceAvailable(path, false);
  CHECK(model.itemMediaOffline(ids[0]) && !thumbnails.contains(ids[0]));
  model.refreshMediaAvailability();
  CHECK(model.itemMediaOffline(ids[0]));
  model.setUvcDeviceAvailable(path, true);
  CHECK(!model.itemMediaOffline(ids[0]) && thumbnails.contains(ids[0]));
  thumbnails.setPreferAudioAlbumArt(true);
  thumbnails.request(ids[0]);
  CHECK(thumbnails.thumbnail(ids[0]).toImage() == image);
  if (const auto screenshot = qgetenv("VTRPON_TEST_LIVE_AUDIO_THUMBNAIL"); !screenshot.isEmpty())
    CHECK(image.save(QString::fromLocal8Bit(screenshot)));
  QCoreApplication::removeTranslator(&english);
  return 0;
}

int verifyNetworkVideoInputControls() {
  InputControlPanel panel;
  auto *combo = panel.findChild<QComboBox *>(QStringLiteral("inputUvcDeviceComboBox"));
  auto *button = panel.findChild<QPushButton *>(QStringLiteral("inputUvcAddButton"));
  auto *title = panel.findChild<QLabel *>(QStringLiteral("inputControlModuleTitle"));
  CHECK(combo && button && title);
  const auto path = vtrpon::networkVideoSourcePath(QStringLiteral("LAN PC (Camera)"));
  panel.setUvcDevices({{QStringLiteral("USB camera"), QStringLiteral("usb-camera")}});
  panel.setNetworkVideoSources({{QStringLiteral("LAN PC (Camera)"), path}}, true);
  CHECK(combo->count() == 3);
  CHECK(combo->itemData(1, Qt::AccessibleDescriptionRole).toString() == QStringLiteral("separator"));
  CHECK(title->text() == vtrpon::uiText("input.network.title"));
  combo->setCurrentIndex(2);
  QString selected;
  QObject::connect(&panel, &InputControlPanel::uvcAddRequested, &panel,
      [&](const QString &, const QString &value) { selected = value; });
  button->click(); CHECK(selected == path);
  panel.setUvcDevices({{QStringLiteral("USB camera renamed"), QStringLiteral("usb-camera")}});
  CHECK(combo->currentData().toString() == path);
  panel.setUvcDevices({});
  CHECK(combo->count() == 3 && combo->currentData().toString() == path && button->isEnabled());
  panel.setNetworkVideoSources({}, false);
  CHECK(!button->isEnabled() && title->text() == vtrpon::uiText("input.uvc.title"));
  panel.setNetworkVideoSources({{QStringLiteral("LAN PC (Camera)"), path}}, true);
  CHECK(button->isEnabled());
  panel.setUvcCaptureBusy(true); CHECK(!button->isEnabled());
  panel.setUvcCaptureBusy(false); CHECK(button->isEnabled());
  const QString deckPath = QStringLiteral("decklink://100/1/1/0");
  panel.setDeckLinkSources({{QStringLiteral("DeckLink · Duo · CH 2 · SDI"), deckPath}}, true);
  CHECK(combo->findData(deckPath) >= 0 && title->text() == vtrpon::uiText("decklink.input_title"));
  combo->setCurrentIndex(combo->findData(deckPath)); button->click(); CHECK(selected == deckPath);
  panel.setNetworkVideoSources({}, false);
  panel.setUvcDevices({});
  CHECK(combo->currentData().toString() == deckPath && button->isEnabled());
  panel.setDeckLinkSources({{QStringLiteral("DeckLink renamed · CH 2 · SDI"), deckPath}}, true);
  CHECK(combo->currentData().toString() == deckPath);
  panel.setDeckLinkSources({}, false); CHECK(!button->isEnabled());
  panel.setNetworkVideoSources({{QStringLiteral("LAN PC (Camera)"), path}}, true);
  const auto captureDirectory = qEnvironmentVariable("VTRPON_NETWORK_VIDEO_UI_CAPTURE_DIR");
  if (!captureDirectory.isEmpty()) {
    QDir().mkpath(captureDirectory);
    panel.show(); QCoreApplication::processEvents();
    CHECK(panel.grab().save(QDir(captureDirectory).filePath(QStringLiteral("network-input.png"))));
  }
  return 0;
}

int verifyAbsentDeckLinkSettings() {
  const auto original = vtrpon::savedDeckLinkSettings();
  auto saved = original;
  saved.inputMode = 1001;
  saved.outputChannel = QStringLiteral("decklink://100/0/1/0");
  saved.outputMode = 1000;
  saved.fillEnabled = saved.keyEnabled = true;
  saved.premultipliedFill = true;
  saved.fillChannel = QStringLiteral("decklink://100/2/1/0"); saved.keyChannel = QStringLiteral("decklink://100/3/1/0"); saved.fillMode = saved.keyMode = 1001;
  vtrpon::saveDeckLinkSettings(saved);
  DeckLinkSettingsPage page;
  auto *inputMode = page.findChild<QComboBox *>("deckLinkInputMode");
  auto *outputMode = page.findChild<QComboBox *>("deckLinkOutputMode");
  auto *channel = page.findChild<QComboBox *>("deckLinkOutputChannel");
  auto *enabled = page.findChild<QCheckBox *>("deckLinkInputEnabled");
  CHECK(inputMode && outputMode && channel && enabled);
  CHECK(inputMode->currentData().toUInt() == saved.inputMode);
  CHECK(outputMode->currentData().toUInt() == saved.outputMode);
  CHECK(channel->currentData().toString() == saved.outputChannel);
  auto *fillChannel = page.findChild<QComboBox *>("deckLinkFillChannel"); auto *keyChannel = page.findChild<QComboBox *>("deckLinkKeyChannel");
  auto *fillMode = page.findChild<QComboBox *>("deckLinkFillMode"); auto *keyMode = page.findChild<QComboBox *>("deckLinkKeyMode");
  auto *fillFormat = page.findChild<QComboBox *>("deckLinkFillFormat"); auto *fillEnabled = page.findChild<QCheckBox *>("deckLinkFillEnabled");
  CHECK(fillChannel && keyChannel && fillMode && keyMode && fillFormat && fillEnabled);
  CHECK(fillChannel->currentData().toString() == saved.fillChannel && keyChannel->currentData().toString() == saved.keyChannel);
  CHECK(fillMode->currentData().toUInt() == saved.fillMode && keyMode->currentData().toUInt() == saved.keyMode && fillFormat->currentIndex() == 1);
  enabled->setChecked(!saved.inputEnabled);
  const auto changed = vtrpon::savedDeckLinkSettings();
  CHECK(changed.inputMode == saved.inputMode && changed.outputMode == saved.outputMode && changed.outputChannel == saved.outputChannel);
  CHECK(changed.fillChannel == saved.fillChannel && changed.keyChannel == saved.keyChannel && changed.fillMode == saved.fillMode && changed.keyMode == saved.keyMode && changed.premultipliedFill);
  fillEnabled->setChecked(false); CHECK(!vtrpon::savedDeckLinkSettings().fillEnabled && vtrpon::savedDeckLinkSettings().keyEnabled);
  page.reload(); CHECK(!fillEnabled->isChecked() && fillChannel->currentData().toString() == saved.fillChannel);
  vtrpon::saveDeckLinkSettings(original);
  return 0;
}

class RuntimeDownloadReceiver final : public QObject {
  Q_OBJECT
public:
  QUrl opened;
public slots:
  void open(const QUrl &url) { opened = url; }
};

int verifyAddonRuntimeGuidance(QApplication &application) {
  QTemporaryDir directory;
  CHECK(directory.isValid());
  const auto addonFile = directory.filePath(QStringLiteral("addon.json"));
  const auto writeStatus = [&](const char *status) {
    QFile file(addonFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(QJsonObject{{QStringLiteral("status"), QString::fromLatin1(status)}})
                          .toJson(QJsonDocument::Compact)) > 0;
  };
  const auto waitState = [&](QWidget &widget, const char *state, int timeout = 3000) {
    QElapsedTimer timer; timer.start();
    while (widget.property("runtimeState").toString() != QLatin1StringView(state)
           && timer.elapsed() < timeout) { application.processEvents(); QThread::msleep(5); }
    return widget.property("runtimeState").toString() == QLatin1StringView(state);
  };
  using vtrpon::AddonRuntimeKind;
  RuntimeDownloadReceiver receiver;
  QDesktopServices::setUrlHandler(QStringLiteral("https"), &receiver, "open");
  for (const auto kind : {AddonRuntimeKind::Atem, AddonRuntimeKind::NetworkVideo}) {
    CHECK(writeStatus("missing"));
    vtrpon::AddonRuntimeStatusWidget widget(kind, addonFile, nullptr, application.applicationFilePath());
    widget.resize(520, 190); widget.show();
    CHECK(waitState(widget, "missing"));
    auto *message = widget.findChild<QLabel *>(QStringLiteral("addonRuntimeMessage"));
    auto *download = widget.findChild<QPushButton *>(QStringLiteral("addonRuntimeDownload"));
    auto *retry = widget.findChild<QPushButton *>(QStringLiteral("addonRuntimeRetry"));
    CHECK(message && download && retry && download->isVisible() && retry->isEnabled());
    CHECK(message->text().contains(kind == AddonRuntimeKind::Atem ? QStringLiteral("ATEM Switchers Software") : QStringLiteral("NDI Runtime 6")));
    CHECK(widget.downloadUrl().scheme() == QStringLiteral("https"));
    CHECK(widget.downloadUrl().host() == (kind == AddonRuntimeKind::Atem ? QStringLiteral("www.blackmagicdesign.com") : QStringLiteral("ndi.link")));
    receiver.opened.clear(); download->click(); CHECK(receiver.opened == widget.downloadUrl());
    const auto captureDirectory = qEnvironmentVariable("VTRPON_ADDON_RUNTIME_UI_CAPTURE_DIR");
    if (!captureDirectory.isEmpty()) {
      QDir().mkpath(captureDirectory);
      CHECK(widget.grab().save(QDir(captureDirectory).filePath(kind == AddonRuntimeKind::Atem
          ? QStringLiteral("atem-runtime-missing.png") : QStringLiteral("network-runtime-missing.png"))));
      QTranslator japanese;
      CHECK(japanese.load(application.applicationDirPath() + QStringLiteral("/vtrpon_ja.qm")));
      CHECK(application.installTranslator(&japanese)); widget.retranslate(); application.processEvents();
      CHECK(message->text().contains(QStringLiteral("アドオンは導入済みですが")));
      CHECK(download->text() == QStringLiteral("公式ダウンロードを開く"));
      CHECK(widget.grab().save(QDir(captureDirectory).filePath(kind == AddonRuntimeKind::Atem
          ? QStringLiteral("atem-runtime-missing-ja.png") : QStringLiteral("network-runtime-missing-ja.png"))));
      application.removeTranslator(&japanese); widget.retranslate();
    }
    CHECK(writeStatus("ready")); retry->click();
    CHECK(waitState(widget, "ready")); CHECK(download->isHidden());
    CHECK(writeStatus("incompatible")); retry->click();
    CHECK(waitState(widget, "incompatible")); CHECK(download->isVisible());
    CHECK(writeStatus("addon_error")); retry->click();
    CHECK(waitState(widget, "addon_error")); CHECK(download->isHidden());
    CHECK(writeStatus("malformed")); retry->click();
    CHECK(waitState(widget, "error")); CHECK(download->isHidden());
  }
  QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
  CHECK(writeStatus("slow"));
  {
    vtrpon::AddonRuntimeStatusWidget widget(AddonRuntimeKind::Atem, addonFile, nullptr, application.applicationFilePath());
    QElapsedTimer responsive; responsive.start();
    CHECK(waitState(widget, "timeout", 6500));
    CHECK(responsive.elapsed() < 6400);
    application.processEvents();
  }
  {
    auto *widget = new vtrpon::AddonRuntimeStatusWidget(AddonRuntimeKind::NetworkVideo, addonFile, nullptr, application.applicationFilePath());
    widget->check(); application.processEvents();
    QElapsedTimer elapsed; elapsed.start(); delete widget;
    CHECK(elapsed.elapsed() < 250);
    application.processEvents();
  }
  CHECK(writeStatus("ready"));
  vtrpon::AddonRuntimeStatusWidget noHelper(AddonRuntimeKind::Atem, addonFile, nullptr,
      directory.filePath(QStringLiteral("missing-probe.exe")));
  CHECK(waitState(noHelper, "check_unavailable"));
  CHECK(QFile::remove(addonFile)); noHelper.check();
  CHECK(noHelper.isHidden());
  return 0;
}

int verifyMissingAddons(QApplication &application) {
  using namespace vtrpon;
  QTranslator english;
  CHECK(english.load(application.applicationDirPath() + QStringLiteral("/vtrpon_en.qm")));
  application.installTranslator(&english);
  CHECK(!AtemAddonSwitcher::addonInstalled());
  CHECK(!networkVideoAddonInstalled());
  {
    auto settings = makeControllerSettings();
    settings.setValue("networkVideo/inputEnabled", true);
    settings.setValue("networkVideo/outputEnabled", true);
    settings.setValue("audio/useVstPlugins", true);
    settings.setValue("audio/vst/master/path", "saved-plugin.vst3");
    settings.setValue("audio/vst/master/uid", QString(32, '1'));
  }
  SwitcherControlSettings switcherSettings;
  switcherSettings.atemEnabled = true;
  CHECK(saveSwitcherControlSettings(switcherSettings));
  SwitcherController switcher(nullptr);
  CHECK(switcher.isShutdownComplete()); // No worker was created for saved ATEM ON.
  switcher.applySettings(switcherSettings);
  CHECK(switcher.isShutdownComplete() && !switcher.switcherReady());

  NetworkVideoRuntime network;
  CHECK(network.shutdownComplete()); // No discovery/retry thread was launched.
  applyNetworkVideoSettings({true, true, QStringLiteral("saved-output")});
  CHECK(!networkVideoInputAllowed(networkVideoSourcePath(QStringLiteral("saved-source"))));
  auto &vst = VstService::instance();
  CHECK(!vst.available() && !vstModeEnabled() && !vst.setEnabled(true));
  vst.scan(); vst.activate(VstSlot::Master);
  CHECK(!vst.scanning() && vst.findChildren<QProcess *>().isEmpty());
  auto *monitor = vst.findChild<QTimer *>(QStringLiteral("vstMonitorTimer"));
  CHECK(monitor && !monitor->isActive());
  QPushButton eq;
  bindVstButton(&eq, VstSlot::Master);
  CHECK(eq.text() == QStringLiteral("EQ") && !showVstEditor(&eq, VstSlot::Master));

  ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, nullptr, nullptr);
  auto *atemCard = dialog.findChild<QWidget *>(QStringLiteral("atemSwitcherSettingsCard"));
  auto *exclusive = dialog.findChild<QLabel *>(QStringLiteral("switcherExclusiveLabel"));
  auto *useVst = dialog.findChild<QCheckBox *>(QStringLiteral("useVstPluginsCheckBox"));
  auto *vstStatus = dialog.findChild<QLabel *>(QStringLiteral("vstAddonStatusLabel"));
  auto *roland = dialog.findChild<QCheckBox *>(QStringLiteral("rolandV8HdEnabledCheckBox"));
  CHECK(atemCard && atemCard->isHidden() && !atemCard->isEnabled());
  CHECK(exclusive && exclusive->isHidden());
  CHECK(useVst && useVst->isHidden() && !useVst->isEnabled());
  CHECK(vstStatus && vstStatus->isHidden());
  CHECK(!dialog.findChild<QWidget *>(QStringLiteral("networkVideoSettingsPage")));
  CHECK(roland && roland->toolTip().isEmpty());
  CHECK(loadSwitcherControlSettings().atemEnabled); // Opening Settings preserves saved ON.
  roland->setChecked(true);
  CHECK(loadSwitcherControlSettings().rolandV8HdEnabled);
  CHECK(!loadSwitcherControlSettings().atemEnabled); // Explicit Roland selection remains exclusive.
  dialog.reloadSavedSettings();
  CHECK(atemCard->isHidden() && useVst->isHidden() && roland->isChecked());

  QElapsedTimer elapsed; elapsed.start();
  while (elapsed.elapsed() < 3200) {
    application.processEvents(); QThread::msleep(10);
  }
  const auto state = networkVideoState();
  CHECK(!state.inputRunning && !state.outputRunning && state.sources.isEmpty());
  CHECK(state.error.isEmpty() && state.sentFrames == 0 && !networkVideoOutputGeneration());
  CHECK(!monitor->isActive() && vst.findChildren<QProcess *>().isEmpty());
  auto *atemRuntime = dialog.findChild<QWidget *>(QStringLiteral("atemRuntimeStatus"));
  CHECK(atemRuntime && atemRuntime->findChildren<QProcess *>().isEmpty());
  CHECK(savedNetworkVideoSettings().inputEnabled && savedNetworkVideoSettings().outputEnabled);
  CHECK(makeControllerSettings().value("audio/useVstPlugins").toBool());
  network.requestShutdown(); switcher.requestShutdown();
  CHECK(network.shutdownComplete() && switcher.isShutdownComplete());
  std::puts("PASS: absent ATEM/NDI/VST UI, saved ON and idle runtime");
  return 0;
}

int verifyVstScanUi(QApplication &application) {
  using namespace vtrpon;
  const QDir fixture(application.applicationDirPath());
  application.setProperty("vtrpon.settingsFile", fixture.filePath("test.ini"));
  QTranslator japanese;
  CHECK(japanese.load(application.arguments().value(2)));
  CHECK(application.installTranslator(&japanese));
  CHECK(applyControllerSkin(application, QStringLiteral("hardware")));
  auto &service = VstService::instance();
  CHECK(service.available() && service.setEnabled(true));
  QPushButton button;
  bindVstButton(&button, VstSlot::Master);
  button.show();
  int ticks = 0;
  QElapsedTimer lastTick; lastTick.start();
  qint64 longestTick = 0;
  QTimer heartbeat;
  heartbeat.setInterval(5);
  QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] {
    longestTick = std::max(longestTick, lastTick.restart()); ++ticks;
  });
  heartbeat.start();
  const auto wait = [&](const std::function<bool()> &predicate, int timeout = 4000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout) {
      application.processEvents(); QThread::msleep(2);
    }
    return predicate();
  };
  QElapsedTimer opening; opening.start();
  CHECK(showVstEditor(&button, VstSlot::Master));
  CHECK(opening.elapsed() < 400);
  auto *window = button.findChild<QDialog *>("vstMASTERWindow");
  CHECK(window && window->isVisible());
  auto *scan = window->findChild<QPushButton *>("vstScanButton");
  auto *status = window->findChild<QLabel *>("vstScanStatus");
  CHECK(scan && status && scan->isEnabled() && scan->text() == QStringLiteral("検索を中止"));
  const QString hanging = fixture.filePath("common/VST3/z-hang.vst3");
  CHECK(wait([&] { return service.scanProgress().currentPath.endsWith("z-hang.vst3") &&
                         service.scanProgress().checked == 2; }));
  CHECK(wait([&] { return QFileInfo::exists(hanging + ".pid"); }));
  CHECK(service.scanProgress().total == 3 && service.plugins().size() == 2);
  CHECK(status->text().contains("z-hang.vst3"));
  const int before = ticks;
  CHECK(wait([&] { return ticks >= before + 20; }));
  CHECK(service.scanning());
  QElapsedTimer cancel; cancel.start(); scan->click();
  CHECK(cancel.elapsed() < 150 && !service.scanning() && service.scanProgress().canceled);
  CHECK(service.plugins().size() == 2);
  CHECK(scan->text() == QStringLiteral("再検索"));
#ifdef Q_OS_WIN
  QFile pidFile(hanging + ".pid");
  CHECK(wait([&] { return pidFile.exists(); }));
  CHECK(pidFile.open(QIODevice::ReadOnly));
  const DWORD pid = pidFile.readAll().toULong(); pidFile.close();
  HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (child) {
    CHECK(wait([&] { return WaitForSingleObject(child, 0) == WAIT_OBJECT_0; }));
    CloseHandle(child);
  }
#endif
  QFile module(hanging);
  CHECK(module.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(module.write("ok") == 2); module.close();
  scan->click();
  CHECK(wait([&] { return !service.scanning(); }));
  CHECK(service.plugins().size() == 3 && service.scanProgress().skipped == 0);
  CHECK(status->text().contains(QStringLiteral("3件")));
  CHECK(module.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(module.write("hang") == 4); module.close();
  const int beforeTimeout = ticks;
  longestTick = 0; lastTick.restart();
  scan->click();
  CHECK(wait([&] { return !service.scanning(); }, 24000));
  CHECK(service.plugins().size() == 2 && service.scanProgress().skipped == 1);
  CHECK(ticks > beforeTimeout + 100 && longestTick < 400);
  const QString helper = VstService::addonPath();
  CHECK(QFile::rename(helper, helper + ".saved"));
  scan->click();
  CHECK(wait([&] { return !service.scanning(); }));
  CHECK(!service.scanProgress().error.isEmpty());
  CHECK(status->text().contains(QStringLiteral("検索できませんでした")));
  CHECK(QFile::rename(helper + ".saved", helper));
  CHECK(qputenv("CommonProgramW6432", fixture.filePath("empty").toUtf8()));
  scan->click();
  CHECK(wait([&] { return !service.scanning(); }));
  CHECK(service.plugins().isEmpty() && service.scanProgress().error.isEmpty());
  QPushButton second;
  bindVstButton(&second, VstSlot::Edit);
  CHECK(showVstEditor(&second, VstSlot::Edit));
  CHECK(!service.scanning()); // An empty result must not trigger another automatic scan.
  service.setEnabled(false);
  return 0;
}

int verifyVstScanInChild(QApplication &application) {
  QTemporaryDir directory;
  CHECK(directory.isValid());
  const QDir fixture(directory.path());
  CHECK(fixture.mkpath("audio-addons") && fixture.mkpath("common/VST3/b-bundle.vst3/Contents") &&
        fixture.mkpath("local"));
  for (const QString &relative : {QStringLiteral("common/VST3/a-good.vst3"),
      QStringLiteral("common/VST3/b-bundle.vst3/Contents/inner.vst3"),
      QStringLiteral("common/VST3/z-hang.vst3")}) {
    QFile module(fixture.filePath(relative));
    CHECK(module.open(QIODevice::WriteOnly));
    CHECK(module.write(relative.contains("hang") ? "hang" : "ok") > 0);
  }
  const QString controller = fixture.filePath("test.exe");
  CHECK(QFile::copy(application.applicationFilePath(), controller));
  CHECK(QFile::copy(application.applicationFilePath(), fixture.filePath("audio-addons/vtr-pon-vst3-addon.exe")));
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("CommonProgramW6432", fixture.filePath("common"));
  environment.insert("LOCALAPPDATA", fixture.filePath("local"));
  environment.insert("QT_QPA_PLATFORM_PLUGIN_PATH",
      QDir(application.applicationDirPath()).filePath("platforms"));
  QProcess child;
  child.setProcessEnvironment(environment);
  child.setProcessChannelMode(QProcess::ForwardedChannels);
  const QDir executableDirectory(application.applicationDirPath());
  QString translation = executableDirectory.filePath("../vtrpon_ja.qm");
  if (!QFileInfo::exists(translation))
    translation = executableDirectory.filePath("vtrpon_ja.qm");
  CHECK(QFileInfo::exists(translation));
  child.start(controller, {"--verify-vst-scan-ui", translation});
  CHECK(child.waitForStarted(5000));
  CHECK(child.waitForFinished(35000));
  CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
  return 0;
}

int verifyMissingAddonsInChild(QApplication &application) {
  // Run the normal test binary without optional payloads. Do not rename/remove
  // the developer's installed add-ons or touch their configuration.
  QTemporaryDir directory;
  CHECK(directory.isValid());
  const auto executable = directory.filePath(QFileInfo(application.applicationFilePath()).fileName());
  CHECK(QFile::copy(application.applicationFilePath(), executable));
  CHECK(QFile::copy(application.applicationDirPath() + QStringLiteral("/vtrpon_en.qm"),
                   directory.filePath(QStringLiteral("vtrpon_en.qm"))));
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.remove("VTRPON_DECKLINK_ADDON_FILE");
  environment.insert("VTRPON_SWITCHER_ADDON_DIR", directory.filePath("switcher-addons"));
  environment.insert("QT_PLUGIN_PATH", application.libraryPaths().join(QDir::listSeparator()));
  // The normal Windows deployment includes qwindows, not the offscreen plug-in.
#ifdef Q_OS_WIN
  environment.insert("QT_QPA_PLATFORM", "windows");
#endif
  QProcess child;
  child.setProcessEnvironment(environment);
  child.start(executable, {QStringLiteral("--verify-missing-addons")});
  CHECK(child.waitForStarted(5000));
  const bool finished = child.waitForFinished(20000);
  if (!finished) { child.kill(); child.waitForFinished(5000); }
  const auto output = child.readAllStandardOutput() + child.readAllStandardError();
  std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
  CHECK(finished);
  CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
  return 0;
}

#include "playlist_panel_drag_drop_test.moc"

int main(int argc, char *argv[]) {
  if (argc > 1 && (QByteArray(argv[1]) == "--verify-vst-scan-ui" ||
                   QByteArray(argv[1]) == "--scan")) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
      std::fprintf(stderr, "%s\n", qPrintable(message)); std::fflush(stderr);
    });
  }
  if (argc == 7 && QByteArray(argv[1]) == "--scan" && QByteArray(argv[3]) == "--result") {
    QCoreApplication probe(argc, argv);
    QFile module(QString::fromLocal8Bit(argv[2]));
    if (module.open(QIODevice::ReadOnly) && module.readAll() == "hang") {
      QFile pid(module.fileName() + ".pid");
      if (pid.open(QIODevice::WriteOnly)) pid.write(QByteArray::number(probe.applicationPid()));
      pid.close();
#ifdef Q_OS_WIN
      HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, QString::fromLocal8Bit(argv[6]).toULong());
      if (parent) { WaitForSingleObject(parent, INFINITE); CloseHandle(parent); }
#else
      QThread::msleep(30000);
#endif
      return 1;
    }
    QFile result(QString::fromLocal8Bit(argv[4]));
    if (!result.open(QIODevice::WriteOnly)) return 2;
    result.write(QJsonDocument(QJsonArray{QJsonObject{{"uid", QString(32, '1')},
        {"name", QFileInfo(module.fileName()).fileName()}, {"vendor", "Test"}}}).toJson());
    return 0;
  }
  if (argc == 2 && QByteArray(argv[1]) == "--operator-monitor-shutdown-child") {
    QThread::msleep(15000);
    return 0;
  }
  if (argc == 2 && QByteArray(argv[1]) == "--verify-missing-addons") {
    // Keep child startup diagnostics in the test output, including Qt failures.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
      std::fprintf(stderr, "%s\n", qPrintable(message)); std::fflush(stderr);
    });
  }
  // Disposable probe used only by the runtime-guidance tests. It never loads
  // a vendor DLL or changes the machine's installed runtime/COM registration.
  if (argc == 5 && QByteArray(argv[1]) == "--kind" && QByteArray(argv[3]) == "--addon-file") {
    QCoreApplication probeApplication(argc, argv);
    QFile file(QString::fromLocal8Bit(argv[4]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto data = file.readAll();
    const auto status = QJsonDocument::fromJson(data).object().value(QStringLiteral("status")).toString();
    if (status == QLatin1StringView("slow")) QThread::msleep(15000);
    if (status == QLatin1StringView("malformed")) { std::fputs("not JSON", stdout); return 0; }
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    return 0;
  }
  QTemporaryDir settingsDirectory;
  CHECK(settingsDirectory.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settingsDirectory.path());
  qputenv("VTRPON_SWITCHER_SETTINGS_FILE",
          QDir(settingsDirectory.path())
              .filePath(QStringLiteral("switcher-settings.ini"))
              .toUtf8());
  QApplication application(argc, argv);
  if (application.arguments().contains(QStringLiteral("--verify-vst-scan-ui")))
    return verifyVstScanUi(application);
  if (application.arguments().contains(QStringLiteral("--verify-vst-scan-only")))
    return verifyVstScanInChild(application);
  if (application.arguments().contains(QStringLiteral("--verify-addon-restart-only"))) {
    ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, nullptr, nullptr);
    return verifyAddonRestartPrompt(application, dialog);
  }
  if (application.arguments().contains(QStringLiteral("--decklink-settings-only"))) {
    CHECK(verifyAbsentDeckLinkSettings() == 0);
    QTranslator japanese; CHECK(japanese.load(QCoreApplication::applicationDirPath() + QStringLiteral("/vtrpon_ja.qm"))); application.installTranslator(&japanese);
    CHECK(vtrpon::applyControllerSkin(application, QStringLiteral("material-dark")));
    vtrpon::DeckLinkRuntime runtime;
    ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *navigation = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigationList")); CHECK(navigation);
    auto *page = dialog.findChild<QWidget *>(QStringLiteral("deckLinkSettingsPage")); CHECK(page);
    const auto items = navigation->findItems(QStringLiteral("DeckLink"), Qt::MatchExactly); CHECK(items.size() == 1); navigation->setCurrentItem(items.front());
    dialog.show(); QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 1300) { application.processEvents(); QThread::msleep(10); }
    CHECK(vtrpon::deckLinkState().availability == vtrpon::DeckLinkState::Availability::NoDevices);
    const auto capture = qEnvironmentVariable("VTRPON_DECKLINK_UI_CAPTURE_DIR");
    if (!capture.isEmpty()) { CHECK(QDir().mkpath(capture)); CHECK(dialog.grab().save(QDir(capture).filePath("decklink-fillkey-settings.png"))); }
    dialog.hide(); runtime.requestShutdown();
    while (!runtime.shutdownComplete() && timer.elapsed() < 5000) { application.processEvents(); QThread::msleep(10); } CHECK(runtime.shutdownComplete());
    return 0;
  }
  if (application.arguments().contains(QStringLiteral("--verify-still-capture-only")))
    return verifyStillCaptureControls(application);
  if (application.arguments().contains(QStringLiteral("--verify-missing-addons")))
    return verifyMissingAddons(application);
  CHECK(verifyMissingAddonsInChild(application) == 0);
  if (application.arguments().contains(QStringLiteral("--verify-addon-absence-only"))) return 0;
  CHECK(verifyVstScanInChild(application) == 0);
  CHECK(verifyAbsentDeckLinkSettings() == 0);
  {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *pflButton = new QPushButton(QStringLiteral("PFL"), &window);
    layout->addWidget(pflButton);
    PlaylistModel model;
    auto *panel = new PlaylistPanel(&model, &window, false);
    layout->addWidget(panel);
    auto *list = panel->findChild<QListWidget *>(QStringLiteral("playlistList"));
    auto *name = panel->findChild<QLineEdit *>(QStringLiteral("playlistNameEdit"));
    auto *combo = panel->findChild<QComboBox *>(QStringLiteral("playlistBulkOperationComboBox"));
    CHECK(list && name && combo);
    QTemporaryFile media(QDir::tempPath() + QStringLiteral("/arrows-XXXXXX.mp4"));
    CHECK(media.open());
    PlaylistItemRegistration registration;
    registration.filePath = media.fileName();
    const auto ids = model.addItems({registration, registration, registration});
    CHECK(ids.size() == 3);
    window.show();
    window.activateWindow();
    application.processEvents();
    const auto press = [](QWidget *target, int key, bool repeat = false) {
      QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, QString(), repeat);
      QApplication::sendEvent(target, &event);
    };
    for (const auto mode : {QListView::ListMode, QListView::IconMode}) {
      list->setViewMode(mode);
      for (const int key : {Qt::Key_Up, Qt::Key_Down}) {
        for (QWidget *origin : {static_cast<QWidget *>(pflButton),
                               static_cast<QWidget *>(name),
                               static_cast<QWidget *>(combo),
                               static_cast<QWidget *>(list)}) {
          // A stale current row must not count as a selected item.
          list->setCurrentRow(2);
          list->clearSelection();
          origin->setFocus();
          CHECK(panel->selectedItemId() == 0);
          const QString originalName = name->text();
          const int originalOption = combo->currentIndex();
#ifdef Q_OS_MACOS
          if (origin == name) {
            press(origin, key);
            CHECK(panel->selectedItemId() == 0 && window.focusWidget() == name);
            CHECK(name->text() == originalName);
            continue;
          }
#endif
          QKeyEvent overrideEvent(QEvent::ShortcutOverride, key, Qt::NoModifier);
          overrideEvent.ignore();
          QApplication::sendEvent(origin, &overrideEvent);
          CHECK(overrideEvent.isAccepted());
          CHECK(panel->selectedItemId() == 0);
          press(origin, key);
          CHECK(panel->selectedItemId() == ids[0]);
          CHECK(window.focusWidget() == list);
          CHECK(name->text() == originalName && combo->currentIndex() == originalOption);
          press(list, Qt::Key_Down);
          CHECK(panel->selectedItemId() == ids[1]);
          press(list, Qt::Key_Down, true);
          CHECK(panel->selectedItemId() == ids[2]);
          press(list, Qt::Key_Down);
          CHECK(panel->selectedItemId() == ids[2]);
          press(list, Qt::Key_Up);
          CHECK(panel->selectedItemId() == ids[1]);
          press(list, Qt::Key_Up);
          press(list, Qt::Key_Up);
          CHECK(panel->selectedItemId() == ids[0]);
          CHECK(window.focusWidget() == list);
          CHECK(model.itemIdAt(model.activePlaylistNumber(), 0) == ids[0]);
        }
      }
    }
    // Dialog and popup arrows retain their own navigation.
    QDialog dialog(&window);
    auto *dialogLayout = new QVBoxLayout(&dialog);
    auto *dialogCombo = new QComboBox(&dialog);
    dialogCombo->addItems({QStringLiteral("A"), QStringLiteral("B")});
    dialogLayout->addWidget(dialogCombo);
    dialog.show();
    application.processEvents();
    press(dialogCombo, Qt::Key_Down);
    CHECK(dialogCombo->currentIndex() == 1 && panel->selectedItemId() == ids[0]);
    dialog.hide();
    window.activateWindow();
    combo->showPopup();
    QElapsedTimer popupWait;
    popupWait.start();
    while (!QApplication::activePopupWidget() && popupWait.elapsed() < 1000) {
      application.processEvents();
      QThread::msleep(5);
    }
    CHECK(QApplication::activePopupWidget());
    const auto popupRow = combo->view()->currentIndex().row();
    press(combo->view(), Qt::Key_Down);
    CHECK(combo->view()->currentIndex().row() > popupRow);
    CHECK(panel->selectedItemId() == ids[0]);
    combo->hidePopup();
    // An empty active playlist consumes arrows without jumping to controls.
    const int emptyPlaylist = model.createPlaylist();
    CHECK(model.setActivePlaylistNumber(emptyPlaylist));
    CHECK(list->count() == 0);
    for (const int key : {Qt::Key_Up, Qt::Key_Down}) {
      pflButton->setFocus();
      press(pflButton, key);
      CHECK(panel->selectedItemId() == 0 && window.focusWidget() == list);
    }
  }
  {
    PlaylistModel deleteModel;
    ControllerOperations deleteOperations;
    PlaylistPanel deletePanel(&deleteModel, nullptr, false, nullptr, false, &deleteOperations);
    QTemporaryFile media(QDir::tempPath() + QStringLiteral("/delete-XXXXXX.mp4"));
    CHECK(media.open());
    PlaylistItemRegistration registration;
    registration.filePath = media.fileName();
    const auto ids = deleteModel.addItems({registration, registration});
    CHECK(ids.size() == 2);
    auto *list = deletePanel.findChild<QListWidget *>(QStringLiteral("playlistList"));
    auto *nameEdit = deletePanel.findChild<QLineEdit *>(QStringLiteral("playlistNameEdit"));
    CHECK(list && nameEdit);
    int removals = 0;
    quint64 removedId = 0;
    deleteOperations.bind<quint64>(ControllerOperations::Action::RemoveItem, &deletePanel,
                     [&](quint64 id) {
      ++removals;
      removedId = id;
      deleteModel.removeItem(id);
    });
    QStringList removalSources;
    QObject::connect(&deleteOperations, &ControllerOperations::operationAboutToExecute,
                     &deletePanel, [&](ControllerOperations::Action action,
                                       const QVariantList &, const QString &source) {
      if (action == ControllerOperations::Action::RemoveSelectedItem ||
          action == ControllerOperations::Action::RemoveItem) removalSources.append(source);
    });
    deletePanel.show();
    deletePanel.activateWindow();
    list->setCurrentRow(0);
    list->setFocus();
    QApplication::processEvents();
    auto pressDelete = [](QWidget *target, bool repeat = false) {
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier,
                      QString(), repeat);
      QApplication::sendEvent(target, &press);
      QKeyEvent release(QEvent::KeyRelease, Qt::Key_Delete, Qt::NoModifier);
      QApplication::sendEvent(target, &release);
    };
    deleteModel.setOnAirItemId(ids[0]);
    pressDelete(list);
    CHECK(removals == 0);
    deleteModel.setOnAirItemId(0);
    deleteModel.setBgmItemId(ids[0]);
    pressDelete(list);
    CHECK(removals == 0);
    deleteModel.setBgmItemId(0);
    nameEdit->setFocus();
    nameEdit->setText(QStringLiteral("abc"));
    nameEdit->setCursorPosition(0);
    pressDelete(nameEdit);
    CHECK(nameEdit->text() == QStringLiteral("bc"));
    CHECK(removals == 0);
    list->setFocus();
    list->clearSelection();
    pressDelete(list);
    CHECK(removals == 0);
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    pressDelete(list, true);
    CHECK(removals == 0);
    CHECK(removalSources.isEmpty());
    auto *removeButton = deletePanel.findChild<QPushButton *>(QStringLiteral("playlistDeleteButton"));
    CHECK(removeButton);
    int buttonClicks = 0;
    QObject::connect(removeButton, &QPushButton::clicked, &deletePanel, [&] { ++buttonClicks; });
    removeButton->setEnabled(false);
    pressDelete(list);
    CHECK(removals == 1 && removedId == ids[0] && deleteModel.itemCount() == 1);
    CHECK(buttonClicks == 0);
    CHECK(removalSources == QStringList({QStringLiteral("Shortcut"), QStringLiteral("Shortcut")}));
    list->setViewMode(QListView::IconMode);
    list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    pressDelete(list);
    CHECK(removals == 2 && removedId == ids[1] && deleteModel.itemCount() == 0);
  }
  {
    PlaylistModel crossfadeModel;
    PlaylistPanel crossfadePanel(&crossfadeModel, nullptr, false);
    auto *checkBox = crossfadePanel.findChild<QCheckBox *>(
        QStringLiteral("playlistCrossfadeCheckBox"));
    auto *duration = crossfadePanel.findChild<QDoubleSpinBox *>(
        QStringLiteral("playlistCrossfadeDurationSpinBox"));
    auto *tabs = crossfadePanel.findChild<QTabBar *>();
    CHECK(checkBox && duration && tabs);
    const int first = crossfadeModel.activePlaylistNumber();
    CHECK(!crossfadePanel.crossfadeEnabled());
    checkBox->setChecked(true);
    const int second = crossfadeModel.createPlaylist();
    CHECK(crossfadeModel.setActivePlaylistNumber(second));
    CHECK(!crossfadePanel.crossfadeEnabled());
    CHECK(!duration->isEnabled());
    tabs->setCurrentIndex(0);
    CHECK(crossfadeModel.activePlaylistNumber() == first);
    CHECK(crossfadePanel.crossfadeEnabled());
    CHECK(duration->isEnabled());
    CHECK(crossfadeModel.setActivePlaylistNumber(second));
    checkBox->setChecked(true);
    CHECK(crossfadeModel.setActivePlaylistNumber(first));
    checkBox->setChecked(false);
    CHECK(crossfadeModel.setActivePlaylistNumber(second));
    CHECK(crossfadePanel.crossfadeEnabled());
    CHECK(duration->isEnabled());
    CHECK(crossfadeModel.setActivePlaylistNumber(first));
    CHECK(!crossfadePanel.crossfadeEnabled());
    CHECK(!duration->isEnabled());
    CHECK(crossfadeModel.setActivePlaylistNumber(second));
    duration->setValue(2.7);
    const auto savedCrossfade = crossfadeModel.snapshot();
    checkBox->setChecked(false);
    duration->setValue(0.5);
    CHECK(crossfadeModel.restoreSnapshot(savedCrossfade));
    CHECK(crossfadePanel.crossfadeEnabled());
    CHECK(duration->isEnabled());
    CHECK(crossfadePanel.crossfadeDurationMs() == 2700);
    CHECK(crossfadeModel.setActivePlaylistNumber(first));
    CHECK(!crossfadePanel.crossfadeEnabled());
    CHECK(crossfadePanel.crossfadeDurationMs() == 2700);
    CHECK(crossfadeModel.setActivePlaylistNumber(second));
    CHECK(crossfadeModel.restoreSnapshot(PlaylistModel().snapshot()));
    CHECK(!crossfadePanel.crossfadeEnabled());
    CHECK(!duration->isEnabled());
    CHECK(crossfadePanel.crossfadeDurationMs() == kDefaultItemFadeDurationMs);
  }
  CHECK(verifyNetworkVideoInputControls() == 0);
  CHECK(verifyLiveAudioInputControls() == 0);
  CHECK(verifyStillCaptureControls(application) == 0);
  CHECK(verifyPlaylistWindowScaling(application) == 0);
  CHECK(verifySelectionOperations() == 0);
  CHECK(verifyBulkUndoPresentation() == 0);
  CHECK(verifyPlaylistOperationGuards() == 0);
  CHECK(verifyPlaylistDeletion() == 0);
  CHECK(verifyThumbnailReuse() == 0);
  application.setProperty("controllerSkinKey", QStringLiteral("hardware"));
  QSettings isolatedSettings(
      QDir(settingsDirectory.path()).filePath(QStringLiteral("settings.ini")),
      QSettings::IniFormat);
  CHECK(vtrpon::loadPlaylistEditActivationMode(isolatedSettings) ==
        PlaylistEditActivationMode::EditButton);
  CHECK(!vtrpon::loadAutoSelectNextItemAfterOnAir(isolatedSettings));
  CHECK(!vtrpon::loadPreferAudioAlbumArt(isolatedSettings));
  CHECK(!vtrpon::loadAllowSameItemOnAirRetake(isolatedSettings));
  CHECK(vtrpon::loadShowPlaybackRateControls(isolatedSettings));
  CHECK(!vtrpon::loadDisableFtbButton(isolatedSettings));
  CHECK(!vtrpon::loadShowBgmPlayerControls(isolatedSettings));
  CHECK(vtrpon::savePlaylistSettings(
      isolatedSettings, PlaylistEditActivationMode::Selection, true, true));
  CHECK(vtrpon::saveOnAirSettings(isolatedSettings, true, false, true));
  CHECK(vtrpon::saveAudioSettings(isolatedSettings, true));
  CHECK(vtrpon::loadPlaylistEditActivationMode(isolatedSettings) ==
        PlaylistEditActivationMode::Selection);
  CHECK(vtrpon::loadAutoSelectNextItemAfterOnAir(isolatedSettings));
  CHECK(vtrpon::loadPreferAudioAlbumArt(isolatedSettings));
  CHECK(vtrpon::loadAllowSameItemOnAirRetake(isolatedSettings));
  CHECK(!vtrpon::loadShowPlaybackRateControls(isolatedSettings));
  CHECK(vtrpon::loadDisableFtbButton(isolatedSettings));
  CHECK(!vtrpon::loadOnAirToggleMode(isolatedSettings));
  CHECK(vtrpon::saveOnAirSettings(isolatedSettings, true, false, true, true));
  CHECK(vtrpon::loadOnAirToggleMode(isolatedSettings));
  CHECK(!vtrpon::loadAllowSameItemOnAirRetake(isolatedSettings));
  CHECK(vtrpon::saveOnAirSettings(isolatedSettings, true, false, true));
  CHECK(vtrpon::loadShowBgmPlayerControls(isolatedSettings));
  {
    const QString mappingKey = QStringLiteral("audio/uvcMappings/") + QString(64, QLatin1Char('a'));
    isolatedSettings.setValue(mappingKey, QStringLiteral("device-test"));
    isolatedSettings.setValue(QStringLiteral("fileDialog/lastMediaDirectory"), QStringLiteral("keep"));
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings).value("player/backgroundColor").toString() == QStringLiteral("#000000"));
    CHECK(!vtrpon::controllerSettingsSnapshot(isolatedSettings).contains("player/fadeToBackground"));
    isolatedSettings.setValue(QStringLiteral("player/backgroundColor"), QStringLiteral("#123abc"));
    isolatedSettings.setValue(QStringLiteral("player/fadeToBackground"), true);
    isolatedSettings.setValue("time/timeZone", "Asia/Tokyo");
    isolatedSettings.setValue("time/remainingTotal", true);
    isolatedSettings.setValue("time/fontFamily", "Consolas");
    isolatedSettings.setValue("time/fontSize", 24);
    isolatedSettings.setValue("operatorMonitor/timerEnabled", true);
    isolatedSettings.setValue("operatorMonitor/timerDurationSeconds", 630);
    isolatedSettings.setValue("operatorMonitor/timerWarning1Minutes", 3);
    isolatedSettings.setValue("operatorMonitor/timerWarning2Minutes", 0);
    isolatedSettings.setValue("operatorMonitor/timerWarning1Color", "#ffffff");
    isolatedSettings.setValue("operatorMonitor/timerWarning2Color", "#aa0000");
    isolatedSettings.setValue("video/programOutput4k", true);
    isolatedSettings.setValue("virtualCamera/enabled", true);
    const QStringList patternEntries{
        QStringLiteral("builtin:bars.png"),
        settingsDirectory.filePath(QStringLiteral("custom-pattern.png"))};
    isolatedSettings.setValue("testPatterns/entries", patternEntries);
    isolatedSettings.setValue("files/stillCaptureDirectory", settingsDirectory.filePath("captures"));
    isolatedSettings.setValue("files/documentConversionDirectory", settingsDirectory.filePath("documents"));
    isolatedSettings.setValue("networkVideo/compositeEnabled", false);
    isolatedSettings.setValue("networkVideo/fillEnabled", true);
    isolatedSettings.setValue("networkVideo/keyEnabled", true);
    isolatedSettings.setValue("networkVideo/alphaEnabled", true);
    isolatedSettings.setValue("networkVideo/fillFormat", "premultiplied");
    const auto snapshot = vtrpon::controllerSettingsSnapshot(isolatedSettings);
    CHECK(!snapshot.contains("player/fadeToBackground"));
    auto legacySnapshot = snapshot;
    legacySnapshot.insert("player/fadeToBackground", false);
    CHECK(vtrpon::replaceControllerSettings(isolatedSettings, legacySnapshot));
    CHECK(!isolatedSettings.contains("player/fadeToBackground"));
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings) == snapshot);
    CHECK(snapshot.value("video/programOutput4k").toBool());
    CHECK(snapshot.value("playlist/preferAudioAlbumArt").toBool());
    CHECK(snapshot.value("virtualCamera/enabled").toBool());
    CHECK(snapshot.value("testPatterns/entries").toArray().size() == 2);
    const QString exportPath = settingsDirectory.filePath(QStringLiteral("settings.json"));
    CHECK(vtrpon::exportControllerSettings(isolatedSettings, exportPath));
    CHECK(vtrpon::replaceControllerSettings(isolatedSettings, {}));
    CHECK(!vtrpon::controllerSettingsSnapshot(isolatedSettings).value("video/programOutput4k").toBool());
    CHECK(!isolatedSettings.contains("virtualCamera/enabled"));
    CHECK(!isolatedSettings.contains("testPatterns/entries"));
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings).value("testPatterns/entries").isNull());
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings).value("networkVideo/compositeEnabled").toBool());
    CHECK(!isolatedSettings.value("networkVideo/fillEnabled").toBool());
    CHECK(!isolatedSettings.value("networkVideo/keyEnabled").toBool());
    CHECK(!isolatedSettings.value("networkVideo/alphaEnabled").toBool());
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings).value("networkVideo/fillFormat").toString() == QStringLiteral("straight"));
    CHECK(!isolatedSettings.value("operatorMonitor/timerEnabled").toBool());
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings).value("operatorMonitor/timerDurationSeconds").toInt() == 300);
    CHECK(!vtrpon::loadShowBgmPlayerControls(isolatedSettings));
    CHECK(!isolatedSettings.contains(mappingKey));
    CHECK(isolatedSettings.value(QStringLiteral("fileDialog/lastMediaDirectory")).toString() == QStringLiteral("keep"));
    CHECK(vtrpon::importControllerSettings(isolatedSettings, exportPath));
    QSettings reopened(isolatedSettings.fileName(), QSettings::IniFormat);
    CHECK(vtrpon::controllerSettingsSnapshot(reopened) == snapshot);
    CHECK(vtrpon::loadPreferAudioAlbumArt(reopened));
    CHECK(reopened.value("testPatterns/entries").toStringList() == patternEntries);
    auto emptyPatterns = snapshot;
    emptyPatterns.insert("testPatterns/entries", QJsonArray{});
    CHECK(vtrpon::replaceControllerSettings(isolatedSettings, emptyPatterns));
    CHECK(isolatedSettings.contains("testPatterns/entries"));
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings).value("testPatterns/entries").toArray().isEmpty());
    auto olderExport = snapshot;
    olderExport.remove("testPatterns/entries");
    olderExport.remove("virtualCamera/enabled");
    CHECK(vtrpon::replaceControllerSettings(isolatedSettings, olderExport));
    CHECK(!isolatedSettings.contains("testPatterns/entries"));
    CHECK(!isolatedSettings.contains("virtualCamera/enabled"));
    CHECK(vtrpon::replaceControllerSettings(isolatedSettings, snapshot));
    auto invalid = snapshot;
    invalid.insert("testPatterns/entries", QStringLiteral("not an array"));
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("testPatterns/entries", QJsonArray{QStringLiteral("relative.png")});
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("testPatterns/entries", QJsonArray{QStringLiteral("builtin:../bad.png")});
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("testPatterns/entries", QJsonArray{true});
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("virtualCamera/enabled", QStringLiteral("true"));
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("networkVideo/fillFormat", "invalid");
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("operatorMonitor/timerDurationSeconds", 0);
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("operatorMonitor/timerWarning2Minutes", 4);
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("operatorMonitor/timerWarning1Color", "invalid");
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("time/timeZone", "Invalid/Zone");
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert("time/fontSize", 49);
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert(QStringLiteral("player/backgroundColor"), QStringLiteral("#00000000"));
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert(QStringLiteral("player/backgroundColor"), QStringLiteral("nonsense"));
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert(QStringLiteral("onAir/allowSameItemRetake"), QStringLiteral("true"));
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert(QStringLiteral("switcher/atem/sourceNumber"), 41);
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    invalid = snapshot;
    invalid.insert(QStringLiteral("unknown/key"), true);
    CHECK(!vtrpon::replaceControllerSettings(isolatedSettings, invalid));
    QFile corrupt(exportPath);
    CHECK(corrupt.open(QIODevice::WriteOnly));
    CHECK(corrupt.write("{broken") > 0);
    corrupt.close();
    CHECK(!vtrpon::importControllerSettings(isolatedSettings, exportPath));
    CHECK(vtrpon::controllerSettingsSnapshot(isolatedSettings) == snapshot);
    CHECK(!vtrpon::exportControllerSettings(isolatedSettings, settingsDirectory.path()));
  }

  QTranslator englishTranslator;
  CHECK(englishTranslator.load(
      QCoreApplication::applicationDirPath()
      + QStringLiteral("/vtrpon_en.qm")));
  CHECK(application.installTranslator(&englishTranslator));
  CHECK(verifyAddonRuntimeGuidance(application) == 0);
  {
    QFile skin(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
        .filePath(QStringLiteral("../apps/desktop/skins/hardware/controller.qss")));
    CHECK(skin.open(QIODevice::ReadOnly));
    const QString sheet = QString::fromUtf8(skin.readAll());
    const QFont baseFont = application.font();
    for (qreal scale : {1.0, 4.0 / 3.0, 2.0}) {
      vtrpon::setUiScale(scale);
      QFont font = baseFont;
      font.setPointSizeF(baseFont.pointSizeF() * scale);
      application.setFont(font);
      application.setStyleSheet(vtrpon::scaledStyleSheet(sheet));
      PlaylistModel scaledModel;
      PlaylistPanel scaledPanel(&scaledModel, nullptr, false);
      scaledPanel.resize(vtrpon::scaleSize(664, 940));
      scaledPanel.show();
      application.processEvents();
      auto *tabs = scaledPanel.findChild<QTabBar *>(QStringLiteral("playlistTabs"));
      CHECK(tabs && tabs->count() >= 2);
      std::fprintf(stderr, "density tabs scale=%.3f height=%d tabHeight=%d hint=%d\n",
          scale, tabs->height(), tabs->tabRect(0).height(), tabs->sizeHint().height());
      CHECK(tabs->rect().contains(tabs->tabRect(0)));
      const QString captureDir = qEnvironmentVariable("VTRPON_DENSITY_CAPTURE_DIR");
      if (!captureDir.isEmpty()) {
        CHECK(QDir().mkpath(captureDir));
        CHECK(scaledPanel.grab().save(QDir(captureDir).filePath(
            QStringLiteral("playlist-%1.png").arg(qRound(scale * 100)))));
      }
    }
    application.setStyleSheet(QString());
    application.setFont(baseFont);
    vtrpon::setUiScale(1.0);
  }
  {
    PlaylistModel toggleModel;
    PlaylistPanel togglePanel(&toggleModel, nullptr, false);
    PlaylistItemRegistration item;
    QTemporaryFile image(QDir::tempPath() + QStringLiteral("/toggle-XXXXXX.png"));
    CHECK(createImageFile(image, Qt::blue));
    item.filePath = image.fileName();
    item.mediaType = PlaylistMediaType::StillImage;
    const auto ids = toggleModel.addItems({item, item});
    CHECK(ids.size() == 2);
    auto *button = togglePanel.findChild<QPushButton *>(QStringLiteral("playlistOnAirButton"));
    auto *list = togglePanel.findChild<QListWidget *>();
    CHECK(button && list);
    list->setCurrentRow(0);
    int takeCount = 0, offCount = 0;
    QObject::connect(&togglePanel, &PlaylistPanel::onAirRequested, &togglePanel,
        [&] { ++takeCount; });
    QObject::connect(&togglePanel, &PlaylistPanel::offAirRequested, &togglePanel,
        [&] { ++offCount; toggleModel.setOnAirItemId(0); });
    togglePanel.setOnAirToggleMode(true);
    CHECK(toggleModel.onAirItemId() == 0);
    CHECK(button->text() == QStringLiteral("ON AIR"));
    button->click();
    CHECK(takeCount == 1 && offCount == 0);
    // A failed take must not change the presentation just because it was clicked.
    CHECK(button->text() == QStringLiteral("ON AIR"));
    toggleModel.setOnAirItemId(ids[0]);
    CHECK(button->text() == QStringLiteral("ON AIR"));
    list->setCurrentRow(1);
    button->click();
    CHECK(takeCount == 1 && offCount == 1);
    CHECK(button->text() == QStringLiteral("ON AIR"));
    toggleModel.setOnAirItemId(ids[0]);
    list->setCurrentRow(-1);
    CHECK(button->isEnabled());
    button->click();
    CHECK(offCount == 2);
    togglePanel.setOnAirToggleMode(false);
    CHECK(button->text() == QStringLiteral("ON AIR"));
    CHECK(!button->isEnabled());
  }

  {
    QWidget programWindow;
    QWidget controllerWindow;
    QMenuBar viewMenuBar;
    vtrpon::ViewMenuController viewController(&viewMenuBar, &programWindow, &controllerWindow);
#ifdef Q_OS_WIN
    auto *monitorProcess = viewController.findChild<QProcess *>();
    CHECK(monitorProcess);
    monitorProcess->start(QCoreApplication::applicationFilePath(),
                          {QStringLiteral("--operator-monitor-shutdown-child")});
    CHECK(monitorProcess->waitForStarted(5000));
    CHECK(!viewController.shutdownComplete());
    viewController.requestShutdown();
    QElapsedTimer monitorShutdownTimer;
    monitorShutdownTimer.start();
    while (!viewController.shutdownComplete() && monitorShutdownTimer.elapsed() < 3000) {
      QApplication::processEvents();
      QThread::msleep(1);
    }
    CHECK(viewController.shutdownComplete());
#endif
    ControllerSettingsDialog settingsDialog(nullptr, nullptr, nullptr,
                                            nullptr, nullptr);
    settingsDialog.addVideoOutputControls(viewController.createSettingsWidget(settingsDialog.videoOutputPage()));
    auto *videoFullscreen = settingsDialog.findChild<QCheckBox *>(QStringLiteral("videoOutputControllerFullScreenCheckBox"));
    auto *videoMinimize = settingsDialog.findChild<QPushButton *>(QStringLiteral("videoOutputMinimizeButton"));
    auto *videoDisplays = settingsDialog.findChild<QComboBox *>(QStringLiteral("videoOutputDisplayComboBox"));
    CHECK(videoFullscreen && videoMinimize && videoDisplays);
    auto *resolution = settingsDialog.findChild<QComboBox *>(QStringLiteral("videoOutputResolutionComboBox"));
    auto *resolutionStatus = settingsDialog.findChild<QLabel *>(QStringLiteral("videoOutputResolutionStatus"));
    CHECK(resolution && resolutionStatus && resolution->count() == 2);
#ifdef Q_OS_WIN
    const auto active4k = application.property("video.startupProgramOutput4k");
    const int savedResolution = resolution->currentIndex();
    resolution->setCurrentIndex(active4k.toBool() ? 0 : 1);
    CHECK(vtrpon::makeControllerSettings().value("video/programOutput4k").toBool() != active4k.toBool());
    CHECK(application.property("video.startupProgramOutput4k") == active4k);
    CHECK(resolutionStatus->text().contains(QStringLiteral("Restart")));
    resolution->setCurrentIndex(savedResolution);
#endif
    auto *gpuChoice = settingsDialog.findChild<QComboBox *>(QStringLiteral("gpuAdapterComboBox"));
    auto *softwareDecode = settingsDialog.findChild<QCheckBox *>(QStringLiteral("softwareDecodingCheckBox"));
    auto *gpuRestart = settingsDialog.findChild<QLabel *>(QStringLiteral("gpuRestartLabel"));
    CHECK(gpuChoice && softwareDecode && gpuRestart);
    CHECK(gpuChoice->itemData(0).toString().isEmpty());
#ifdef Q_OS_WIN
    const bool previousSoftware = softwareDecode->isChecked();
    const auto activeSoftware = application.property("gpu.startupSoftwareDecoding");
    softwareDecode->setChecked(!previousSoftware);
    CHECK(vtrpon::makeControllerSettings().value(QStringLiteral("video/softwareDecoding")).toBool() == !previousSoftware);
    CHECK(application.property("gpu.startupSoftwareDecoding") == activeSoftware);
    CHECK(gpuRestart->text().contains(QStringLiteral("Restart")));
    softwareDecode->setChecked(previousSoftware);
#endif
    CHECK(videoDisplays->count() == QGuiApplication::screens().size());
    auto *topmost = settingsDialog.findChild<QCheckBox *>(QStringLiteral("videoOutputAlwaysOnTopCheckBox"));
    auto *topmostAction = viewMenuBar.findChild<QAction *>(QStringLiteral("programAlwaysOnTopAction"));
    CHECK(topmost && topmostAction);
    CHECK(!topmost->isEnabled() && !topmostAction->isEnabled());
    topmost->setChecked(true);
    CHECK(!topmost->isChecked() && !topmostAction->isChecked());
    const WId programId = programWindow.winId();
    if (QGuiApplication::screens().size() > 1) {
      const auto screens = QGuiApplication::screens();
      QScreen *controllerScreen = controllerWindow.windowHandle()->screen();
      const int other = controllerScreen == screens.first() ? 1 : 0;
      videoDisplays->activated(other);
      CHECK(topmost->isEnabled() && topmostAction->isEnabled());
      topmost->setChecked(true);
      CHECK(topmostAction->isChecked());
      videoDisplays->activated(screens.indexOf(controllerScreen));
      CHECK(!topmost->isChecked() && !topmost->isEnabled());
      CHECK(!topmostAction->isChecked() && !topmostAction->isEnabled());
      CHECK(programWindow.winId() == programId);
    }
    videoFullscreen->setChecked(true);
    CHECK(controllerWindow.isFullScreen());
    videoFullscreen->setChecked(false);
    CHECK(!controllerWindow.isFullScreen());
    videoMinimize->click();
    CHECK(programWindow.isMinimized());
    controllerWindow.hide();
    programWindow.hide();
    auto *settingsNavigation = settingsDialog.findChild<QListWidget *>(
        QStringLiteral("settingsNavigationList"));
    auto *editModeCombo = settingsDialog.findChild<QComboBox *>(
        QStringLiteral("playlistEditModeComboBox"));
    auto *autoSelectNext = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("playlistAutoSelectNextItemCheckBox"));
    auto *audioThumbnailCombo = settingsDialog.findChild<QComboBox *>(
        QStringLiteral("playlistAudioThumbnailComboBox"));
    auto *allowSameItemRetake = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("allowSameItemOnAirRetakeCheckBox"));
    auto *showPlaybackRateControls = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("showPlaybackRateControlsCheckBox"));
    auto *disableFtbButton = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("disableFtbButtonCheckBox"));
    auto *showBgmPlayerControls = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("showBgmPlayerControlsCheckBox"));
    auto *atemEnabled = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("atemEnabledCheckBox"));
    auto *rolandEnabled = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("rolandV8HdEnabledCheckBox"));
    auto *atemSource = settingsDialog.findChild<QSpinBox *>(
        QStringLiteral("atemSourceSpinBox"));
    auto *rolandConnection = settingsDialog.findChild<QLabel *>(
        QStringLiteral("rolandConnectionLabel"));
    auto *atemConnection = settingsDialog.findChild<QLabel *>(
        QStringLiteral("atemConnectionLabel"));
    auto *switcherExclusive = settingsDialog.findChild<QLabel *>(
        QStringLiteral("switcherExclusiveLabel"));
    auto *settingsPages = settingsDialog.findChild<QStackedWidget *>();
    auto *fallbackCheckBox = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("defaultAudioFallbackCheckBox"));
    CHECK(fallbackCheckBox != nullptr);
    CHECK(fallbackCheckBox->isChecked() == vtrpon::savedUseDefaultAudioOutputWhenMissing());
    auto *audioOutputCard = settingsDialog.findChild<QWidget *>(
        QStringLiteral("audioOutputSettingsCard"));
    auto *vstCard = settingsDialog.findChild<QWidget *>(
        QStringLiteral("vstSettingsCard"));
    auto *useVst = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("useVstPluginsCheckBox"));
    CHECK(audioOutputCard && vstCard && useVst);
    CHECK(vstCard->isAncestorOf(useVst));
    CHECK(!audioOutputCard->isAncestorOf(useVst));
    auto *pflDeviceCombo = settingsDialog.findChild<QComboBox *>(
        QStringLiteral("editPflOutputDeviceComboBox"));
    CHECK(pflDeviceCombo != nullptr);
    CHECK(pflDeviceCombo->findData(QString::fromLatin1(vtrpon::kSystemDefaultPflDeviceId)) >= 0);
    CHECK(settingsNavigation != nullptr);
    CHECK(settingsNavigation->count() == networkSettingsRow(15));
    auto *addonPage = settingsDialog.findChild<QWidget *>(
        QStringLiteral("addonManagementPage"));
    auto *installLocal = settingsDialog.findChild<QPushButton *>(
        QStringLiteral("addonInstallLocal"));
    CHECK(addonPage && installLocal && installLocal->isEnabled());
    CHECK(verifyAddonRestartPrompt(application, settingsDialog) == 0);
    CHECK(settingsDialog.findChild<QPushButton *>(
        QStringLiteral("addonUninstall0")) != nullptr);
    CHECK(!settingsNavigation->dragEnabled());
    CHECK(!settingsNavigation->acceptDrops());
    QStringList expectedSettingsOrder{
        QStringLiteral("Playlist Settings"),
        QStringLiteral("ON AIR Settings"),
        QStringLiteral("Video Output")};
    if (vtrpon::networkVideoAddonInstalled()) {
      expectedSettingsOrder.push_back(QStringLiteral("NDI"));
    }
    if (vtrpon::deckLinkAddonInstalled()) {
      expectedSettingsOrder.push_back(QStringLiteral("DeckLink"));
      const auto screenshot = qEnvironmentVariable("VTRPON_DECKLINK_UI_CAPTURE_DIR");
      if (!screenshot.isEmpty()) {
        CHECK(QDir().mkpath(screenshot));
        vtrpon::DeckLinkRuntime runtime;
        QElapsedTimer discovery; discovery.start();
        while (vtrpon::deckLinkState().availability == vtrpon::DeckLinkState::Availability::Checking && discovery.elapsed() < 3000) {
          application.processEvents(); QThread::msleep(10);
        }
        settingsNavigation->setCurrentRow(expectedSettingsOrder.size() - 1);
        settingsDialog.show();
        QElapsedTimer refresh; refresh.start();
        while (refresh.elapsed() < 700) { application.processEvents(); QThread::msleep(10); }
        CHECK(settingsDialog.grab().save(QDir(screenshot).filePath("decklink-no-hardware.png")));
        const auto addonNavigation = settingsNavigation->findItems(vtrpon::uiText("settings.addons.title"), Qt::MatchExactly);
        CHECK(addonNavigation.size() == 1);
        settingsNavigation->setCurrentItem(addonNavigation.front());
        application.processEvents();
        CHECK(settingsDialog.grab().save(QDir(screenshot).filePath("addons-decklink.png")));
        settingsDialog.hide(); runtime.requestShutdown();
        while (!runtime.shutdownComplete() && discovery.elapsed() < 5000) { application.processEvents(); QThread::msleep(10); }
        CHECK(runtime.shutdownComplete()); settingsNavigation->setCurrentRow(0);
      }
    }
    expectedSettingsOrder.append(QStringLiteral("Virtual Camera"));
    if (vtrpon::AtemAddonSwitcher::addonInstalled()) expectedSettingsOrder.append(QStringLiteral("ATEM Integration"));
    expectedSettingsOrder.append(
        {QStringLiteral("Roland Integration"),
         QStringLiteral("Return Monitor"),
         QStringLiteral("Audio OUTPUT"),
         QStringLiteral("UVC Audio Mapping"),
         QStringLiteral("BGM Player"),
         QStringLiteral("Appearance"),
         QStringLiteral("Language & Time"),
         QStringLiteral("Shortcut Keys"),
         QStringLiteral("MIDI Control"),
         QStringLiteral("Test Pattern"),
         QStringLiteral("File Management"),
         QStringLiteral("Add-ons"),
         QStringLiteral("Save / Reset"),
         QStringLiteral("Updates and notices")});
    CHECK(settingsNavigation->count() == expectedSettingsOrder.size());
    for (int row = 0; row < expectedSettingsOrder.size(); ++row) {
      CHECK(settingsNavigation->item(row)->text() ==
            expectedSettingsOrder.at(row));
      CHECK(!(settingsNavigation->item(row)->flags() &
              Qt::ItemIsDragEnabled));
      CHECK(!(settingsNavigation->item(row)->flags() &
              Qt::ItemIsDropEnabled));
    }
    auto *automaticUpdate = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("automaticUpdateCheckBox"));
    auto *activityNotice = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("activityNoticeCheckBox"));
    CHECK(automaticUpdate && activityNotice && settingsPages);
    settingsNavigation->setCurrentRow(settingsNavigation->count() - 1);
    CHECK(settingsPages->currentWidget()->isAncestorOf(automaticUpdate));
    CHECK(settingsPages->currentWidget()->isAncestorOf(activityNotice));
    CHECK(automaticUpdate->isChecked());
    const bool previousNotice = activityNotice->isChecked();
    automaticUpdate->setChecked(false);
    activityNotice->setChecked(!previousNotice);
    {
      auto settings = vtrpon::makeControllerSettings();
      CHECK(!settings.value(QStringLiteral("updates/checkAutomatically"), true).toBool());
      CHECK(settings.value(QStringLiteral("privacy/sendActivityNotice"), false).toBool()
            == !previousNotice);
    }
    automaticUpdate->setChecked(true);
    activityNotice->setChecked(previousNotice);
    settingsNavigation->setCurrentRow(0);
    auto *networkPage = settingsDialog.findChild<QWidget *>(QStringLiteral("networkVideoSettingsPage"));
    CHECK(bool(networkPage) == vtrpon::networkVideoAddonInstalled());
    if (networkPage) {
      CHECK(settingsNavigation->item(2)->text() == QStringLiteral("Video Output"));
      CHECK(settingsNavigation->item(3)->text() == QStringLiteral("NDI"));
      CHECK(settingsNavigation->item(4)->text() == QStringLiteral("Virtual Camera"));
      auto *output = networkPage->findChild<QCheckBox *>(QStringLiteral("networkVideoOutputEnabled"));
      auto *input = networkPage->findChild<QCheckBox *>(QStringLiteral("networkVideoInputEnabled"));
      auto *name = networkPage->findChild<QLineEdit *>(QStringLiteral("networkVideoOutputName"));
      CHECK(output && input && name);
      CHECK(name->placeholderText() == QStringLiteral("VTR-PON-NDI-OUTPPUT"));
      name->setText(QStringLiteral("Custom network output"));
      output->setChecked(true); input->setChecked(true);
      const auto saved = vtrpon::savedNetworkVideoSettings();
      CHECK(saved.outputEnabled && saved.inputEnabled && saved.outputName == name->text());
      auto *composite = networkPage->findChild<QCheckBox *>(QStringLiteral("networkVideoCompositeEnabled"));
      auto *fill = networkPage->findChild<QCheckBox *>(QStringLiteral("networkVideoFillEnabled"));
      auto *key = networkPage->findChild<QCheckBox *>(QStringLiteral("networkVideoKeyEnabled"));
      auto *alpha = networkPage->findChild<QCheckBox *>(QStringLiteral("networkVideoAlphaEnabled"));
      auto *format = networkPage->findChild<QComboBox *>(QStringLiteral("networkVideoFillFormat"));
      auto *details = networkPage->findChild<QToolButton *>(QStringLiteral("networkVideoDetails"));
      CHECK(composite && fill && key && alpha && format && details);
      CHECK(composite->isChecked() && !fill->isChecked() && !key->isChecked() && !alpha->isChecked());
      CHECK(!format->isEnabled() && !details->isChecked());
      fill->setChecked(true); key->setChecked(true); alpha->setChecked(true);
      details->setChecked(true); format->setCurrentIndex(1);
      CHECK(format->isEnabled() && vtrpon::savedNetworkVideoSettings().outputMask() == 15);
      CHECK(vtrpon::savedNetworkVideoSettings().premultipliedFill);
      static_cast<NetworkVideoSettingsPage *>(networkPage)->reload();
      CHECK(fill->isChecked() && key->isChecked() && alpha->isChecked() && format->currentIndex() == 1);
      composite->setChecked(false); fill->setChecked(false); key->setChecked(false); alpha->setChecked(false);
      CHECK(vtrpon::savedNetworkVideoSettings().outputMask() == 0);
      composite->setChecked(true); format->setCurrentIndex(0); details->setChecked(false);
      output->setChecked(false); input->setChecked(false);
      name->clear(); QMetaObject::invokeMethod(name, "editingFinished");
      const auto captureDirectory = qEnvironmentVariable("VTRPON_NETWORK_VIDEO_UI_CAPTURE_DIR");
      if (!captureDirectory.isEmpty()) {
        settingsNavigation->setCurrentRow(3); settingsDialog.show(); QCoreApplication::processEvents();
        CHECK(settingsDialog.grab().save(QDir(captureDirectory).filePath(QStringLiteral("network-settings-en.png"))));
      }
    }
    CHECK(settingsNavigation->item(networkSettingsRow(4))->text() == QStringLiteral("Virtual Camera"));
    auto *virtualCameraEnabled = settingsDialog.findChild<QCheckBox *>(QStringLiteral("virtualCameraEnabled"));
    CHECK(virtualCameraEnabled != nullptr);
    CHECK(virtualCameraEnabled->text() == QStringLiteral("Enable virtual camera output"));
    auto *virtualCameraSaveStatus = settingsDialog.findChild<QLabel *>(QStringLiteral("virtualCameraSaveStatus"));
    CHECK(virtualCameraSaveStatus != nullptr);
    {
      const bool originalEnabled = vtrpon::virtualCameraEnabled();
      const QVariant originalSettingsFile = application.property("vtrpon.settingsFile");
      application.setProperty("vtrpon.settingsFile", settingsDirectory.path());
      virtualCameraEnabled->setChecked(!originalEnabled);
      CHECK(vtrpon::virtualCameraEnabled() == originalEnabled);
      CHECK(virtualCameraEnabled->isChecked() == originalEnabled);
      CHECK(virtualCameraSaveStatus->text().contains(QStringLiteral("Could not save")));
      application.setProperty("vtrpon.settingsFile", originalSettingsFile);
      virtualCameraEnabled->setChecked(!originalEnabled);
      CHECK(vtrpon::virtualCameraEnabled() != originalEnabled);
      CHECK(virtualCameraSaveStatus->text().isEmpty());
      virtualCameraEnabled->setChecked(originalEnabled);
      CHECK(vtrpon::virtualCameraEnabled() == originalEnabled);
    }
    auto *testPatternList = settingsDialog.findChild<QListWidget *>(QStringLiteral("testPatternList"));
    CHECK(testPatternList != nullptr);
    {
      auto savedSettings = vtrpon::makeControllerSettings();
      const bool hadCamera = savedSettings.contains(QStringLiteral("virtualCamera/enabled"));
      const QVariant oldCamera = savedSettings.value(QStringLiteral("virtualCamera/enabled"));
      const bool hadPatterns = savedSettings.contains(QStringLiteral("testPatterns/entries"));
      const QVariant oldPatterns = savedSettings.value(QStringLiteral("testPatterns/entries"));
      savedSettings.setValue(QStringLiteral("virtualCamera/enabled"), true);
      savedSettings.setValue(QStringLiteral("testPatterns/entries"),
                             QStringList{settingsDirectory.filePath(QStringLiteral("custom-pattern.png"))});
      savedSettings.sync();
      vtrpon::reloadVirtualCameraEnabled();
      settingsDialog.reloadSavedSettings();
      CHECK(virtualCameraEnabled->isChecked());
      CHECK(testPatternList->count() == 1);
      CHECK(testPatternList->item(0)->text() == QStringLiteral("custom-pattern.png"));
      if (hadCamera) savedSettings.setValue(QStringLiteral("virtualCamera/enabled"), oldCamera);
      else savedSettings.remove(QStringLiteral("virtualCamera/enabled"));
      if (hadPatterns) savedSettings.setValue(QStringLiteral("testPatterns/entries"), oldPatterns);
      else savedSettings.remove(QStringLiteral("testPatterns/entries"));
      savedSettings.sync();
      vtrpon::reloadVirtualCameraEnabled();
      settingsDialog.reloadSavedSettings();
    }
    CHECK(settingsNavigation->item(networkSettingsRow(11))->text() == QStringLiteral("Test Pattern"));
    CHECK(settingsNavigation->item(networkSettingsRow(8))->text() == QStringLiteral("Shortcut Keys"));
    CHECK(settingsNavigation->item(networkSettingsRow(9))->text() == QStringLiteral("Appearance"));
    CHECK(settingsNavigation->item(networkSettingsRow(12))->text() ==
          QStringLiteral("Language & Time"));
    auto *textSlider = settingsDialog.findChild<QSlider *>(QStringLiteral("uiTextSizeSlider"));
    CHECK(textSlider && textSlider->minimum() == 0 && textSlider->maximum() == 2);
    auto *accessibilityCard = settingsDialog.findChild<QWidget *>(
        QStringLiteral("accessibilitySettingsCard"));
    auto *languageCombo = settingsDialog.findChild<QComboBox *>(
        QStringLiteral("languageComboBox"));
    CHECK(accessibilityCard && accessibilityCard->isAncestorOf(textSlider));
    CHECK(languageCombo != nullptr);
    settingsNavigation->setCurrentRow(networkSettingsRow(9));
    CHECK(settingsPages->currentWidget()->isAncestorOf(textSlider));
    settingsNavigation->setCurrentRow(networkSettingsRow(12));
    CHECK(settingsPages->currentWidget()->isAncestorOf(languageCombo));
    CHECK(verifySettingsStyle(application, settingsDialog) == 0);
    const QString textSheet = QStringLiteral("QLabel { font-size: 10px; padding: 4px; } QSpinBox[audioDelayControl=\"true\"] { font-size: 9px; }");
    const auto smallSheet = vtrpon::scaledStyleSheet(textSheet);
    const auto waitForTextSize = [](int size) {
      QElapsedTimer timeout;
      timeout.start();
      while (vtrpon::uiTextSize() != size && timeout.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
      }
      return vtrpon::uiTextSize() == size;
    };
    // Dragging must move the handle without restyling, even after the debounce
    // interval. Releasing applies only the final position, asynchronously.
    textSlider->setSliderDown(true);
    textSlider->setSliderPosition(1);
    textSlider->setSliderPosition(2);
    CHECK(textSlider->sliderPosition() == 2);
    QEventLoop dragWait;
    QTimer::singleShot(220, &dragWait, &QEventLoop::quit);
    dragWait.exec();
    CHECK(vtrpon::uiTextSize() == 0);
    textSlider->setSliderDown(false);
    CHECK(vtrpon::uiTextSize() == 0);
    CHECK(waitForTextSize(2));
    textSlider->setValue(0);
    CHECK(waitForTextSize(0));
    // Rapid clicks/keys are coalesced; returning to the current size cancels
    // the intermediate value instead of briefly applying it.
    textSlider->setValue(2);
    textSlider->setValue(1);
    textSlider->setValue(0);
    QTimer::singleShot(220, &dragWait, &QEventLoop::quit);
    dragWait.exec();
    CHECK(vtrpon::uiTextSize() == 0);

    for (int size : {1, 2, 0, 2, 0}) {
      const int previousSize = vtrpon::uiTextSize();
      textSlider->setValue(size);
      CHECK(textSlider->value() == size);
      CHECK(vtrpon::uiTextSize() == previousSize);
      CHECK(waitForTextSize(size));
      QCoreApplication::processEvents();
      CHECK(vtrpon::uiTextSize() == size);
      CHECK(vtrpon::makeControllerSettings().value("accessibility/textSize").toInt() == size);
      CHECK(vtrpon::controllerSettingsSnapshot(vtrpon::makeControllerSettings()).value("accessibility/textSize").toInt() == size);
      CHECK(vtrpon::scaledStyleSheet(textSheet).contains(QStringLiteral("font-size: %1px").arg(vtrpon::scaleTextPx(10))));
      CHECK(vtrpon::scaledStyleSheet(textSheet).contains(QStringLiteral("font-size: %1px").arg(vtrpon::scalePx(9))));
      CHECK(vtrpon::scaledStyleSheet(textSheet).contains(QStringLiteral("padding: %1px").arg(vtrpon::scalePx(4))));
      if (size == 0) CHECK(vtrpon::scaledStyleSheet(textSheet) == smallSheet);
      const auto capture = qEnvironmentVariable("VTRPON_ACCESSIBILITY_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        settingsNavigation->setCurrentRow(networkSettingsRow(9));
        settingsDialog.show();
        QCoreApplication::processEvents();
        CHECK(settingsDialog.grab().save(QDir(capture).filePath(QStringLiteral("accessibility-%1.png").arg(size))));
      }
    }
    settingsDialog.hide();

    const auto accessibilityCapture = qEnvironmentVariable("VTRPON_ACCESSIBILITY_CAPTURE_DIR");
    if (!accessibilityCapture.isEmpty()) {
      QTranslator japaneseAccessibility;
      CHECK(japaneseAccessibility.load(application.applicationDirPath() + QStringLiteral("/vtrpon_ja.qm")));
      CHECK(application.installTranslator(&japaneseAccessibility));
      textSlider->setValue(2);
      CHECK(waitForTextSize(2));
      settingsDialog.show();
      QCoreApplication::processEvents();
      CHECK(settingsDialog.grab().save(QDir(accessibilityCapture).filePath(QStringLiteral("accessibility-ja-large.png"))));
      const int originalRow = settingsNavigation->currentRow();
      for (int row = 0; row < settingsNavigation->count(); ++row) {
        settingsNavigation->setCurrentRow(row);
        QCoreApplication::processEvents();
        CHECK(settingsDialog.grab().save(QDir(accessibilityCapture).filePath(
            QStringLiteral("settings-ja-large-%1.png").arg(row, 2, 10, QLatin1Char('0')))));
      }
      settingsNavigation->setCurrentRow(originalRow);
      application.removeTranslator(&japaneseAccessibility);
      textSlider->setValue(0);
      CHECK(waitForTextSize(0));
      settingsDialog.hide();
    }
    auto *editKey = settingsDialog.findChild<QKeySequenceEdit *>(QStringLiteral("shortcut_LoadEdit_1"));
    auto *displayKey = settingsDialog.findChild<QKeySequenceEdit *>(QStringLiteral("shortcut_NextOutputDisplay_1"));
    CHECK(displayKey && displayKey->keySequence() == QKeySequence(Qt::ALT | Qt::Key_W));
    auto *saveKeys = settingsDialog.findChild<QPushButton *>(QStringLiteral("saveShortcutsButton"));
    auto *resetKeys = settingsDialog.findChild<QPushButton *>(QStringLiteral("resetShortcutsButton"));
    auto *previewKey = settingsDialog.findChild<QKeySequenceEdit *>(QStringLiteral("shortcut_PreviewCapture_1"));
    auto *timerKey = settingsDialog.findChild<QKeySequenceEdit *>(QStringLiteral("shortcut_OperatorTimerStart_1"));
    CHECK(editKey && saveKeys && resetKeys && previewKey && timerKey);
    int shortcutChanges = 0;
    QObject::connect(&settingsDialog, &ControllerSettingsDialog::shortcutsChanged,
                     &settingsDialog, [&] { ++shortcutChanges; });
    editKey->setKeySequence(QKeySequence(Qt::Key_F6));
    displayKey->setKeySequence(QKeySequence(Qt::Key_F7));
    previewKey->setKeySequence(QKeySequence(Qt::Key_F8));
    timerKey->setKeySequence(QKeySequence(Qt::Key_F9));
    saveKeys->click();
    CHECK(shortcutChanges == 1);
    settingsDialog.reloadSavedSettings();
    CHECK(editKey->keySequence() == QKeySequence(Qt::Key_F6));
    CHECK(displayKey->keySequence() == QKeySequence(Qt::Key_F7));
    CHECK(previewKey->keySequence() == QKeySequence(Qt::Key_F8));
    CHECK(timerKey->keySequence() == QKeySequence(Qt::Key_F9));
    editKey->setKeySequence(QKeySequence(Qt::Key_Space));
    saveKeys->click();
    CHECK(shortcutChanges == 1);
    settingsDialog.reloadSavedSettings();
    CHECK(editKey->keySequence() == QKeySequence(Qt::Key_F6));
    editKey->clear();
    saveKeys->click();
    CHECK(shortcutChanges == 2);
    settingsDialog.reloadSavedSettings();
    CHECK(editKey->keySequence().isEmpty());
    resetKeys->click();
    CHECK(shortcutChanges == 2);
    saveKeys->click();
    CHECK(shortcutChanges == 3);
    auto *shortcutTable = settingsDialog.findChild<QTableWidget *>(QStringLiteral("shortcutBindingsTable"));
    CHECK(shortcutTable && shortcutTable->rowCount() == 64);
    const auto &shortcutOrder = vtrpon::shortcutSettings::displayOrder();
    CHECK(shortcutOrder.size() == shortcutTable->rowCount());
    for (int index = 0; index < shortcutOrder.size(); ++index)
      CHECK(shortcutOrder.count(index) == 1);
    const auto rowFor = [&shortcutOrder](vtrpon::shortcutSettings::Action action) {
      return shortcutOrder.indexOf(static_cast<int>(action));
    };
    using ShortcutAction = vtrpon::shortcutSettings::Action;
    CHECK(rowFor(ShortcutAction::PreviewCapture) == rowFor(ShortcutAction::EditSetOut) + 1);
    CHECK(rowFor(ShortcutAction::ProgramCapture) == rowFor(ShortcutAction::PreviewCapture) + 1);
    CHECK(rowFor(ShortcutAction::TogglePfl) == rowFor(ShortcutAction::LoadEdit) + 1);
    CHECK(rowFor(ShortcutAction::OpenNominalEq) == rowFor(ShortcutAction::TogglePfl) + 1);
    CHECK(rowFor(ShortcutAction::OpenItemEq) == rowFor(ShortcutAction::ItemFadeOut) + 1);
    CHECK(rowFor(ShortcutAction::OpenMasterEq) == rowFor(ShortcutAction::MasterFadeOut) + 1);
    CHECK(rowFor(ShortcutAction::PlaylistDelete) == rowFor(ShortcutAction::PlaylistClear) + 1);
    CHECK(rowFor(ShortcutAction::OperatorTimerStart) == rowFor(ShortcutAction::OffAir) + 1);
    CHECK(rowFor(ShortcutAction::OperatorTimerSettings) == rowFor(ShortcutAction::OperatorTimerStart) + 3);
    CHECK(previewKey->keySequence() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    CHECK(timerKey->keySequence() == QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T));
    CHECK(shortcutTable->item(rowFor(ShortcutAction::PreviewCapture), 0)->text() == QStringLiteral("EDIT Capture"));
    CHECK(shortcutTable->item(rowFor(ShortcutAction::PlaylistDelete), 0)->text() == QStringLiteral("Playlist: Delete Playlist"));
    CHECK(displayKey->keySequence() == QKeySequence(Qt::ALT | Qt::Key_W));
    CHECK(shortcutTable->item(shortcutOrder.indexOf(18), 0)->text() == QStringLiteral("Select item 1"));
    CHECK(shortcutTable->item(shortcutOrder.indexOf(19), 0)->text() == QStringLiteral("Activate playlist 1"));
    CHECK(shortcutTable->item(shortcutOrder.indexOf(36), 0)->text() == QStringLiteral("Playlist: Play List Once"));
    CHECK(shortcutTable->item(shortcutOrder.indexOf(42), 0)->text() == QStringLiteral("Playlist: Undo Bulk Operation"));
    auto *bulkKey = settingsDialog.findChild<QKeySequenceEdit *>(QStringLiteral("shortcut_PlaylistPlayOnce_1"));
    CHECK(bulkKey && bulkKey->keySequence() == QKeySequence(Qt::CTRL | Qt::Key_L));

    CHECK(settingsDialog.findChild<QKeySequenceEdit *>(QStringLiteral("shortcut_LoadEdit_1"))->keySequence() == QKeySequence(Qt::Key_E));
    {
      namespace shortcuts = vtrpon::shortcutSettings;
      auto bindings = shortcuts::defaults();
      CHECK(shortcuts::validationError(bindings).isEmpty());
      // Previously customized keys win over all newly introduced defaults.
      for (int index = static_cast<int>(ShortcutAction::PreviewCapture);
           index < shortcuts::definitions().size(); ++index) {
        auto legacy = bindings;
        for (int added = static_cast<int>(ShortcutAction::PreviewCapture);
             added < shortcuts::definitions().size(); ++added)
          legacy.remove(shortcuts::definitions().at(added).id);
        const auto id = shortcuts::definitions().at(index).id;
        legacy.insert("LoadEdit", bindings.value(id));
        CHECK(shortcuts::validationError(legacy).isEmpty());
        const auto resolved = shortcuts::resolved(legacy);
        CHECK(resolved.value("LoadEdit") == bindings.value(id));
        CHECK(resolved.value(id).toArray().isEmpty());
      }

      CHECK(bindings.value("EditToggleStartMode") == QJsonArray{"Alt+S"});
      CHECK(bindings.value("EditReset") == QJsonArray{"Alt+C"});
      CHECK(bindings.value("EditSetIn") == QJsonArray{"Alt+Shift+I"});
      CHECK(bindings.value("EditSetOut") == QJsonArray{"Alt+Shift+O"});
      auto duplicateEdit = bindings;
      duplicateEdit.insert("EditReset", QJsonArray{"Alt+S"});
      CHECK(!shortcuts::validationError(duplicateEdit).isEmpty());
      auto oldEditBindings = bindings;
      for (int row = 43; row < shortcuts::definitions().size(); ++row)
        oldEditBindings.remove(shortcuts::definitions().at(row).id);
      oldEditBindings.insert("LoadEdit", QJsonArray{"Alt+S"});
      CHECK(shortcuts::validationError(oldEditBindings).isEmpty());
      CHECK(shortcuts::resolved(oldEditBindings).value("EditToggleStartMode").toArray().isEmpty());
      auto duplicateBulk = bindings;
      duplicateBulk.insert("PlaylistPlayOnce", QJsonArray{"Ctrl+1"});
      CHECK(!shortcuts::validationError(duplicateBulk).isEmpty());
      auto olderBindings = bindings;
      for (int row = 36; row < shortcuts::definitions().size(); ++row)
        olderBindings.remove(shortcuts::definitions().at(row).id);
      olderBindings.insert("LoadEdit", QJsonArray{"Ctrl+L"});
      CHECK(shortcuts::validationError(olderBindings).isEmpty());
      const auto migrated = shortcuts::resolved(olderBindings);
      CHECK(migrated.value("LoadEdit") == QJsonArray{"Ctrl+L"});
      CHECK(migrated.value("PlaylistPlayOnce").toArray().isEmpty());
      CHECK(migrated.value("PlaylistRepeatAll") == QJsonArray{"Ctrl+R"});
      bindings.insert("LoadEdit", QJsonArray{"Space"});
      CHECK(!shortcuts::validationError(bindings).isEmpty());
      bindings.insert("LoadEdit", QJsonArray{"Ctrl+Up"});
      CHECK(!shortcuts::validationError(bindings).isEmpty());
      bindings.insert("LoadEdit", QJsonArray{"Ctrl+Alt+S"});
      CHECK(!shortcuts::validationError(bindings).isEmpty());
      bindings.insert("LoadEdit", QJsonArray{"Ctrl+K, Ctrl+E"});
      CHECK(!shortcuts::validationError(bindings).isEmpty());
      bindings.insert("PlaylistPlayOnce", QJsonArray{"F8"});
      bindings.insert("LoadEdit", QJsonArray{"F6"});
      bindings.insert("TakeOnAir", QJsonArray{});
      CHECK(shortcuts::validationError(bindings).isEmpty());
      QSettings isolated(settingsDirectory.filePath("shortcut-test.ini"), QSettings::IniFormat);
      auto snapshot = vtrpon::controllerSettingsSnapshot(isolated);
      snapshot.insert(shortcuts::key(), bindings);
      CHECK(vtrpon::replaceControllerSettings(isolated, snapshot));
      CHECK(shortcuts::load(isolated) == bindings);
      const auto path = settingsDirectory.filePath("shortcut-export.json");
      CHECK(vtrpon::exportControllerSettings(isolated, path));
      CHECK(vtrpon::replaceControllerSettings(isolated, {}));
      CHECK(shortcuts::load(isolated) == shortcuts::defaults());
      CHECK(vtrpon::importControllerSettings(isolated, path));
      CHECK(shortcuts::load(isolated) == bindings);
      auto invalid = bindings;
      invalid.insert("LoadEdit", QJsonArray{"Space"});
      snapshot.insert(shortcuts::key(), invalid);
      CHECK(!vtrpon::replaceControllerSettings(isolated, snapshot));
      CHECK(shortcuts::load(isolated) == bindings);
    }
    CHECK(settingsNavigation->item(networkSettingsRow(5))->text() == QStringLiteral("UVC Audio Mapping"));
    CHECK(settingsNavigation->item(networkSettingsRow(2))->text() == QStringLiteral("Audio OUTPUT"));
    CHECK(settingsNavigation->item(networkSettingsRow(0))->text() ==
          QStringLiteral("Playlist Settings"));
    CHECK(settingsNavigation->item(networkSettingsRow(1))->text() ==
          QStringLiteral("ON AIR Settings"));
    CHECK(settingsNavigation->item(networkSettingsRow(6))->text() ==
          QStringLiteral("BGM Player"));
    CHECK(settingsNavigation->item(networkSettingsRow(7))->text() ==
          QStringLiteral("Roland Integration"));
    CHECK(settingsPages != nullptr);
    CHECK(settingsPages->count() == networkSettingsRow(15));
    CHECK(settingsNavigation->item(networkSettingsRow(13))->text() == QStringLiteral("Return Monitor"));
    settingsNavigation->setCurrentRow(networkSettingsRow(13));
    auto *operatorInformation = settingsDialog.findChild<QCheckBox *>("operatorMonitorInformation");
    CHECK(operatorInformation && settingsPages->currentWidget()->isAncestorOf(operatorInformation));
    CHECK(!settingsDialog.findChild<QLineEdit *>("operatorMonitorCueInput"));
    CHECK(!settingsDialog.findChild<QSpinBox *>("operatorMonitorTimerDurationMinutes"));
    auto *timerSettingsButton = settingsDialog.findChild<QPushButton *>("operatorMonitorTimerSettingsButton");
    CHECK(timerSettingsButton);
    bool timerSettingsRequestedAfterClose = false;
    QObject::connect(&settingsDialog, &ControllerSettingsDialog::operatorTimerSettingsRequested,
                     &settingsDialog, [&] { timerSettingsRequestedAfterClose = !settingsDialog.isVisible(); });
    settingsDialog.show();
    application.processEvents();
    timerSettingsButton->click();
    CHECK(timerSettingsRequestedAfterClose && settingsDialog.result() == QDialog::Accepted);
    CHECK(settingsNavigation->item(networkSettingsRow(3))->text() == QStringLiteral("Video Output"));
    settingsNavigation->setCurrentRow(networkSettingsRow(3));
    CHECK(settingsPages->currentWidget()->isAncestorOf(videoFullscreen));
    settingsDialog.showAudioOutputPage();
    CHECK(settingsNavigation->currentRow() == networkSettingsRow(2));
    CHECK(settingsPages->currentWidget()->isAncestorOf(pflDeviceCombo));
    CHECK(editModeCombo != nullptr);
    CHECK(autoSelectNext != nullptr);
    CHECK(audioThumbnailCombo != nullptr);
    CHECK(allowSameItemRetake != nullptr);
    CHECK(showPlaybackRateControls != nullptr);
    CHECK(editModeCombo->findData(
              static_cast<int>(PlaylistEditActivationMode::EditButton)) >= 0);
    CHECK(editModeCombo->findData(
              static_cast<int>(PlaylistEditActivationMode::Selection)) >= 0);
    CHECK(autoSelectNext->text() ==
          QStringLiteral("Select the next item after ON AIR ends"));
    CHECK(audioThumbnailCombo->itemText(audioThumbnailCombo->findData(false)) ==
          QStringLiteral("Waveform"));
    CHECK(audioThumbnailCombo->itemText(audioThumbnailCombo->findData(true)) ==
          QStringLiteral("Album art (waveform if unavailable)"));
    CHECK(allowSameItemRetake->text() == QStringLiteral(
        "Allow the same item to be taken ON AIR again while it is already ON AIR"));
    CHECK(showPlaybackRateControls->text() ==
          QStringLiteral("Show playback-rate buttons"));
    CHECK(disableFtbButton != nullptr);
    CHECK(disableFtbButton->text() == QStringLiteral("Disable the FTB button"));
    CHECK(showBgmPlayerControls != nullptr);
    CHECK(showBgmPlayerControls->text() ==
          QStringLiteral("Show BGM Player"));
    CHECK(atemEnabled != nullptr);
    CHECK(atemEnabled->text() ==
          QStringLiteral("Enable Blackmagic ATEM control"));
    CHECK(atemSource != nullptr);
    CHECK(atemSource->minimum() == 1);
    CHECK(atemSource->maximum() == 40);
    CHECK(rolandEnabled != nullptr);
    CHECK(rolandConnection != nullptr);
    CHECK(atemConnection != nullptr);
    CHECK(switcherExclusive != nullptr);
    CHECK(switcherExclusive->text().contains(
        QStringLiteral("mutually exclusive")));
    CHECK(rolandConnection->text() == QStringLiteral("Disabled"));
    CHECK(atemConnection->text() == QStringLiteral("Disabled"));
    rolandEnabled->setChecked(true);
    CHECK(rolandEnabled->isChecked());
    CHECK(!atemEnabled->isChecked());
    CHECK(rolandConnection->text() == QStringLiteral("No Connection"));
    CHECK(atemConnection->text() == QStringLiteral("Disabled"));
    if (vtrpon::AtemAddonSwitcher::addonInstalled()) {
      atemEnabled->setChecked(true);
      CHECK(atemEnabled->isChecked());
      CHECK(!rolandEnabled->isChecked());
      CHECK(rolandConnection->text() == QStringLiteral("Disabled"));
      CHECK(atemConnection->text() == QStringLiteral("No Connection"));
      const auto savedSwitcherSettings = vtrpon::loadSwitcherControlSettings();
      CHECK(savedSwitcherSettings.atemEnabled);
      CHECK(!savedSwitcherSettings.rolandV8HdEnabled);
    } else {
      CHECK(!atemEnabled->isEnabled() && switcherExclusive->isHidden());
    }
    settingsNavigation->setCurrentRow(networkSettingsRow(6));
    CHECK(settingsPages->currentWidget()->isAncestorOf(
        showBgmPlayerControls));
    CHECK(!settingsDialog.findChild<QWidget *>(QStringLiteral("playerBackgroundSettingsCard")));
    CHECK(!settingsDialog.findChild<QCheckBox *>(QStringLiteral("fadeToBackgroundCheckBox")));
    auto *resetButton = settingsDialog.findChild<QPushButton *>(QStringLiteral("resetSettingsButton"));
    CHECK(resetButton != nullptr);
    settingsNavigation->setCurrentRow(networkSettingsRow(14));
    CHECK(settingsPages->currentWidget()->isAncestorOf(resetButton));
    showBgmPlayerControls->setChecked(true);
    audioThumbnailCombo->setCurrentIndex(audioThumbnailCombo->findData(true));
    CHECK(settingsDialog.preferAudioAlbumArt());
    CHECK(ControllerSettingsDialog::savedPreferAudioAlbumArt());
    const QByteArray switcherOverride = qgetenv("VTRPON_SWITCHER_SETTINGS_FILE");
    qunsetenv("VTRPON_SWITCHER_SETTINGS_FILE");
    resetButton->click();
    CHECK(!showBgmPlayerControls->isChecked());
    CHECK(!audioThumbnailCombo->currentData().toBool());
    CHECK(!allowSameItemRetake->isChecked());
    CHECK(!disableFtbButton->isChecked());
    auto *toggleMode = settingsDialog.findChild<QCheckBox *>(QStringLiteral("onAirToggleModeCheckBox"));
    CHECK(toggleMode && !toggleMode->isChecked());
    allowSameItemRetake->setChecked(true);
    toggleMode->setChecked(true);
    CHECK(!allowSameItemRetake->isChecked());
    CHECK(ControllerSettingsDialog::savedOnAirToggleMode());
    CHECK(!ControllerSettingsDialog::savedAllowSameItemOnAirRetake());
    allowSameItemRetake->setChecked(true);
    CHECK(!toggleMode->isChecked());
    CHECK(!ControllerSettingsDialog::savedOnAirToggleMode());
    CHECK(ControllerSettingsDialog::savedAllowSameItemOnAirRetake());
    allowSameItemRetake->setChecked(false);
    CHECK(showPlaybackRateControls->isChecked());
    disableFtbButton->setChecked(true);
    CHECK(ControllerSettingsDialog::savedDisableFtbButton());
    disableFtbButton->setChecked(false);
    CHECK(!atemEnabled->isChecked());
    CHECK(!rolandEnabled->isChecked());
    CHECK(ControllerSettingsDialog::savedPlayerBackgroundColor() == QColor(Qt::black));
    CHECK(!operatorInformation->isChecked());
    CHECK(settingsNavigation->currentRow() == networkSettingsRow(14));
    CHECK(settingsNavigation->item(networkSettingsRow(14))->text() ==
          QStringLiteral("Save / Reset"));
    qputenv("VTRPON_SWITCHER_SETTINGS_FILE", switcherOverride);
  }

  {
    PlaylistModel clockModel;
    UserInfoCenter clockInfo;
    PlaylistPanel clockPanel(&clockModel, nullptr, false, &clockInfo, false);
    clockPanel.resize(clockPanel.width(), 600);
    clockPanel.show();
    QApplication::processEvents();

    auto *infoBar = clockPanel.findChild<QWidget *>(
        QStringLiteral("userInfoBar"));
    auto *clockRow = clockPanel.findChild<QWidget *>(
        QStringLiteral("playlistClockRow"));
    auto *clockTitle = clockPanel.findChild<QLabel *>(
        QStringLiteral("playlistClockTitle"));
    auto *clockValue = clockPanel.findChild<QLabel *>(
        QStringLiteral("playlistCurrentTime"));
    auto *actionPanel = clockPanel.findChild<QWidget *>(
        QStringLiteral("playlistActionPanel"));
    CHECK(infoBar != nullptr);
    CHECK(clockRow != nullptr);
    CHECK(clockTitle != nullptr);
    CHECK(clockValue != nullptr);
    CHECK(actionPanel != nullptr);
    CHECK(clockTitle->text() == QStringLiteral("CURRENT TIME"));
    CHECK(clockRow->height() == vtrpon::scalePx(22));
    CHECK(infoBar->geometry().bottom() < clockRow->geometry().top());
    CHECK(clockRow->geometry().bottom() < actionPanel->geometry().top());
    CHECK(qAbs(clockValue->geometry().center().x()
               - clockRow->rect().center().x()) <= vtrpon::scalePx(1));
    CHECK(clockTitle->geometry().right() < clockValue->geometry().left());
    CHECK(clockValue->font().pointSizeF()
          == 12.0 * vtrpon::uiScale());
    const QString displayedTime = clockValue->text();
    CHECK(displayedTime.size() == 8);
    CHECK(displayedTime.at(2) == QLatin1Char(':'));
    CHECK(displayedTime.at(5) == QLatin1Char(':'));
    for (int index : {0, 1, 3, 4, 6, 7}) {
      CHECK(displayedTime.at(index).isDigit());
    }
    CHECK(clockRow->accessibleName().startsWith(
        QStringLiteral("Current time:")));
    CHECK(clockRow->accessibleDescription() == QStringLiteral(
        "Showing the local computer time because NTP is unavailable."));
  }

  QTemporaryFile imageA(QDir::tempPath() + QStringLiteral("/vtrpon-a-XXXXXX.png"));
  QTemporaryFile imageB(QDir::tempPath() + QStringLiteral("/vtrpon-b-XXXXXX.png"));
  QTemporaryFile imageC(QDir::tempPath() + QStringLiteral("/vtrpon-c-XXXXXX.png"));
  CHECK(createImageFile(imageA, Qt::red));
  CHECK(createImageFile(imageB, Qt::green));
  CHECK(createImageFile(imageC, Qt::blue));

  {
    PlaylistModel modeModel;
    PlaylistPanel modePanel(&modeModel, nullptr, false);
    PlaylistItemRegistration nativeItem;
    nativeItem.filePath = imageA.fileName();
    nativeItem.sourceType = PlaylistSourceType::PowerPoint;
    nativeItem.mediaType = PlaylistMediaType::Presentation;
    nativeItem.powerPointMode =
        PlaylistPowerPointMode::NativeSlideShow;
    PlaylistItemRegistration videoItem;
    videoItem.filePath = imageB.fileName();
    videoItem.mediaType = PlaylistMediaType::Video;
    videoItem.powerPointMode =
        PlaylistPowerPointMode::VideoConversion;
    PlaylistItemRegistration pngItem;
    pngItem.filePath = imageC.fileName();
    pngItem.mediaType = PlaylistMediaType::StillImage;
    pngItem.powerPointMode = PlaylistPowerPointMode::PngSequence;
    const QList<quint64> modeIds =
        modeModel.addItems({nativeItem, videoItem, pngItem});
    CHECK(modeIds.size() == 3);

    QImage thumbnail(120, 68, QImage::Format_RGB32);
    thumbnail.fill(QColor(QStringLiteral("#101010")));
    for (quint64 itemId : modeIds) {
      modePanel.setItemThumbnail(itemId, thumbnail);
    }
    modePanel.resize(modePanel.width(), 600);
    modePanel.show();
    QApplication::processEvents();
    auto *modeList = modePanel.findChild<QListWidget *>(
        QStringLiteral("playlistList"));
    CHECK(modeList != nullptr);
    CHECK(modeList->count() == 3);
    auto *nativeStatus = modePanel.findChild<QLabel *>("playlistPowerPointCueStatus");
    auto *nativeTake = modePanel.findChild<QPushButton *>("playlistOnAirButton");
    CHECK(nativeStatus && nativeTake);
    modeList->setCurrentRow(0);
    modeModel.setCueItemState(modeIds[0], PlaylistCueState::Loading);
    CHECK(nativeStatus->isVisible() && nativeStatus->text() == QStringLiteral("PPT: PREPARING"));
    CHECK(nativeTake->isEnabled()); // Preparing sources still accept a queued TAKE.
    modeModel.setCueItemState(modeIds[0], PlaylistCueState::Ready);
    CHECK(nativeStatus->text() == QStringLiteral("PPT: READY"));
    CHECK(nativeStatus->toolTip() == QStringLiteral("Ready for ON AIR."));
    modeModel.setCueItemState(modeIds[0], PlaylistCueState::Error, QStringLiteral("Office disconnected"));
    CHECK(nativeStatus->text() == QStringLiteral("PPT: ERROR"));
    CHECK(nativeStatus->toolTip() == QStringLiteral("Office disconnected"));
    {
      QTranslator japaneseCueTranslator;
      CHECK(japaneseCueTranslator.load(QCoreApplication::applicationDirPath()
                                      + QStringLiteral("/vtrpon_ja.qm")));
      CHECK(application.installTranslator(&japaneseCueTranslator));
      QApplication::processEvents();
      CHECK(nativeStatus->text() == QStringLiteral("PPT：準備エラー"));
      modeModel.setCueItemState(modeIds[0], PlaylistCueState::Loading);
      CHECK(nativeStatus->text() == QStringLiteral("PPT：準備中"));
      modeModel.setCueItemState(modeIds[0], PlaylistCueState::Ready);
      CHECK(nativeStatus->text() == QStringLiteral("PPT：スタンバイ完了"));
      CHECK(nativeStatus->toolTip() == QStringLiteral("スタンバイ完了です。"));
      if (const auto screenshot = qgetenv("VTRPON_TEST_PPT_CUE_SCREENSHOT"); !screenshot.isEmpty()) {
        QFile style(QString::fromLocal8Bit(qgetenv("VTRPON_TEST_PPT_CUE_STYLE")));
        CHECK(style.open(QIODevice::ReadOnly));
        modePanel.setStyleSheet(QString::fromUtf8(style.readAll()));
        QApplication::processEvents();
        CHECK(modePanel.grab().save(QString::fromLocal8Bit(screenshot)));
        modePanel.setStyleSheet({});
      }
      application.removeTranslator(&japaneseCueTranslator);
      QApplication::processEvents();
    }
    modeList->setCurrentRow(1);
    CHECK(!nativeStatus->isVisible());
    constexpr int kPowerPointModeRole = Qt::UserRole + 18;
    const std::array<PlaylistPowerPointMode, 3> expectedModes{
        PlaylistPowerPointMode::NativeSlideShow,
        PlaylistPowerPointMode::VideoConversion,
        PlaylistPowerPointMode::PngSequence,
    };
    const std::array<QString, 3> expectedLabels{
        QStringLiteral("PPT NATIVE"),
        QStringLiteral("PPT MP4"),
        QStringLiteral("PPT PNG"),
    };
    QImage rendered(modeList->viewport()->size(), QImage::Format_RGB32);
    modeList->viewport()->render(&rendered);
    for (int row = 0; row < modeList->count(); ++row) {
      QListWidgetItem *item = modeList->item(row);
      CHECK(item->data(kPowerPointModeRole).toInt() ==
            static_cast<int>(expectedModes.at(row)));
      CHECK(item->toolTip().isEmpty());
      CHECK(item->data(Qt::AccessibleTextRole).toString().contains(expectedLabels.at(row)));
      const QRect itemRect = modeList->visualItemRect(item);
      const QRect thumbnailRect(
          itemRect.left() + 7,
          itemRect.top() + (itemRect.height() - 68) / 2,
          120, 68);
      const QRect badgeBand(thumbnailRect.left() + 38,
                            thumbnailRect.top() + 2, 79, 15);
      CHECK(matchingPixelCount(
                rendered, badgeBand,
                [row](const QColor &pixel) {
                  const int hue = pixel.toHsv().hue();
                  const int saturation = pixel.toHsv().saturation();
                  if (saturation < 70) {
                    return false;
                  }
                  if (row == 0) {
                    return hue >= 245 && hue <= 285;
                  }
                  if (row == 1) {
                    return hue >= 0 && hue <= 30;
                  }
                  return hue >= 125 && hue <= 170;
                }) > 25);
      CHECK(matchingPixelCount(
                rendered, badgeBand,
                [](const QColor &pixel) {
                  return pixel.red() > 220 && pixel.green() > 220 &&
                         pixel.blue() > 220;
                }) > 4);
    }
  }

  {
    PlaylistModel conversionModel;
    UserInfoCenter conversionInfo;
    conversionInfo.setDisplayDuration(0);
    PlaylistPanel conversionPanel(&conversionModel, nullptr, false,
                                  &conversionInfo);
    PlaylistItemRegistration firstItem;
    firstItem.filePath = imageA.fileName();
    firstItem.mediaType = PlaylistMediaType::StillImage;
    firstItem.settings.outPointMs = 1000;
    PlaylistItemRegistration conversionItem;
    conversionItem.filePath = imageB.fileName();
    conversionItem.sourceType = PlaylistSourceType::PowerPoint;
    conversionItem.mediaType = PlaylistMediaType::Presentation;
    conversionItem.powerPointMode =
        PlaylistPowerPointMode::NativeSlideShow;
    conversionItem.conversionState = PlaylistItemConversionState{};
    PlaylistItemRegistration lastItem = firstItem;
    lastItem.filePath = imageC.fileName();
    const QList<quint64> conversionIds = conversionModel.addItems(
        {firstItem, conversionItem, lastItem});
    CHECK(conversionIds.size() == 3);
    const quint64 conversionId = conversionIds.at(1);
    CHECK(conversionModel.itemConversionInProgress(conversionId));
    CHECK(conversionModel.nextItemIdForPlayout(conversionIds.constFirst()) ==
          conversionIds.constLast());
    CHECK(conversionModel.copyItemToPlaylist(conversionId, 1, 0) == 0);
    CHECK(conversionModel.setItemConversionProgress(
        conversionId, PlaylistItemConversionStage::Encoding, 456));

    conversionPanel.resize(conversionPanel.width(), 420);
    conversionPanel.show();
    QApplication::processEvents();
    auto *conversionList = conversionPanel.findChild<QListWidget *>(
        QStringLiteral("playlistList"));
    CHECK(conversionList != nullptr);
    CHECK(conversionList->count() == 3);
    constexpr int kItemConversionActiveRole = Qt::UserRole + 19;
    constexpr int kItemConversionProgressRole = Qt::UserRole + 20;
    constexpr int kItemConversionStageRole = Qt::UserRole + 21;
    QListWidgetItem *pendingItem = conversionList->item(1);
    CHECK(pendingItem->data(kItemConversionActiveRole).toBool());
    CHECK(pendingItem->data(kItemConversionProgressRole).toInt() == 456);
    CHECK(pendingItem->data(kItemConversionStageRole).toString() ==
          QStringLiteral("Encoding video…"));
    CHECK(pendingItem->data(Qt::AccessibleTextRole).toString().contains(
        QStringLiteral("Encoding video… 46%")));
    CHECK(!conversionPanel.allowItemAction(
        conversionId, PlaylistItemAction::OnAir));
    CHECK(conversionInfo.severity() == UserInfoSeverity::Warning);
    CHECK(conversionInfo.text().contains(QStringLiteral("ON AIR")));

    PlaylistItemRegistration replacementA;
    replacementA.filePath = imageB.fileName();
    replacementA.mediaType = PlaylistMediaType::StillImage;
    replacementA.settings.outPointMs = 1500;
    PlaylistItemRegistration replacementB = replacementA;
    replacementB.filePath = imageC.fileName();
    CHECK(conversionModel.setItemDisplayName(
        conversionId, QStringLiteral("Edited while converting")));
    CHECK(conversionModel.setItemColor(conversionId, PlaylistItemColor::Blue));
    CHECK(conversionModel.setItemSeparatorBelow(conversionId, true));
    const QList<quint64> replacements = conversionModel.replaceItemAtomically(
        conversionId, {replacementA, replacementB});
    CHECK(replacements.size() == 2);
    CHECK(replacements.constFirst() == conversionId);
    CHECK(!conversionModel.itemConversionInProgress(conversionId));
    CHECK(conversionModel.itemCount() == 4);
    CHECK(conversionModel.itemIdAt(1) == conversionId);
    CHECK(conversionModel.itemIdAt(2) == replacements.constLast());
    CHECK(conversionModel.itemState(conversionId)->displayName ==
          QStringLiteral("Edited while converting"));
    CHECK(conversionModel.itemState(conversionId)->color ==
          PlaylistItemColor::Blue);
    CHECK(!conversionModel.itemState(conversionId)->separatorBelow);
    CHECK(conversionModel.itemState(replacements.constLast())->displayName ==
          QFileInfo(replacementB.filePath).fileName());
    CHECK(conversionModel.itemState(replacements.constLast())->color ==
          PlaylistItemColor::Blue);
    CHECK(conversionModel.itemState(replacements.constLast())->separatorBelow);
    CHECK(conversionModel.setItemDisplayName(
        conversionId, QStringLiteral("Edited after conversion")));
    CHECK(conversionModel.itemState(conversionId)->displayName ==
          QStringLiteral("Edited after conversion"));
    QApplication::processEvents();
    CHECK(conversionList->count() == 4);
    CHECK(!conversionList->item(1)
               ->data(kItemConversionActiveRole)
               .toBool());

    const auto singleIds = conversionModel.addItems({conversionItem});
    CHECK(singleIds.size() == 1);
    const quint64 singleId = singleIds.constFirst();
    CHECK(conversionModel.setItemDisplayName(
        singleId, QStringLiteral("Single converted item")));
    CHECK(conversionModel.setItemColor(singleId, PlaylistItemColor::Red));
    CHECK(conversionModel.setItemSeparatorBelow(singleId, true));
    CHECK(conversionModel.replaceItemAtomically(singleId, {replacementA}) ==
          singleIds);
    CHECK(conversionModel.itemState(singleId)->displayName ==
          QStringLiteral("Single converted item"));
    CHECK(conversionModel.itemState(singleId)->color == PlaylistItemColor::Red);
    CHECK(conversionModel.itemState(singleId)->separatorBelow);

    const auto uneditedIds = conversionModel.addItems({conversionItem});
    CHECK(uneditedIds.size() == 1);
    CHECK(conversionModel.replaceItemAtomically(
              uneditedIds.constFirst(), {replacementA}) == uneditedIds);
    CHECK(conversionModel.itemState(uneditedIds.constFirst())->displayName ==
          QFileInfo(replacementA.filePath).fileName());
  }

  {
    PlaylistModel analysisModel;
    UserInfoCenter analysisInfo;
    PlaylistPanel analysisPanel(&analysisModel, nullptr, false, &analysisInfo);
    const auto id = analysisModel.addPendingMediaItem(imageA.fileName(), 1);
    CHECK(id != 0);
    auto *list = analysisPanel.findChild<QListWidget *>("playlistList");
    CHECK(list && list->count() == 1);
    analysisPanel.setEditActivationMode(PlaylistEditActivationMode::Selection);
    int editRequests = 0;
    QObject::connect(&analysisPanel, &PlaylistPanel::editRequested,
                     &analysisPanel, [&] { ++editRequests; });
    list->setCurrentItem(list->item(0));
    CHECK(analysisPanel.selectedItemId() == id);
    CHECK(editRequests == 0);
    CHECK(list->item(0)->data(Qt::AccessibleTextRole).toString().contains(
        QStringLiteral("Waiting to analyze…")));
    CHECK(!analysisPanel.allowItemAction(id, PlaylistItemAction::OnAir));
    CHECK(!analysisPanel.allowItemAction(id, PlaylistItemAction::Edit));
    CHECK(analysisModel.setItemConversionProgress(
        id, PlaylistItemConversionStage::Analyzing, 0));
    CHECK(list->item(0)->data(Qt::AccessibleTextRole).toString().contains(
        QStringLiteral("Analyzing media…")));
    CHECK(!list->item(0)->data(Qt::AccessibleTextRole).toString().contains('%'));
    CHECK(analysisModel.completePendingMediaItem(
        id, PlaylistMediaType::StillImage, 5500));
    QApplication::processEvents();
    CHECK(editRequests == 1);
    CHECK(analysisPanel.allowItemAction(id, PlaylistItemAction::Edit));
    CHECK(analysisModel.itemState(id)->settings.outPointMs == 5500);
  }

  PlaylistModel model;
  UserInfoCenter userInfoCenter;
  PlaylistPanel panel(&model, nullptr, false, &userInfoCenter);
  QWidget *userInfoBar =
      panel.findChild<QWidget *>(QStringLiteral("userInfoBar"));
  QLabel *userInfoSeverity =
      panel.findChild<QLabel *>(QStringLiteral("userInfoSeverityLabel"));
  QLabel *userInfoSource =
      panel.findChild<QLabel *>(QStringLiteral("userInfoSourceLabel"));
  QLabel *userInfoMessage =
      panel.findChild<QLabel *>(QStringLiteral("userInfoMessageLabel"));
  CHECK(userInfoBar != nullptr);
  CHECK(userInfoSeverity != nullptr);
  CHECK(userInfoSource != nullptr);
  CHECK(userInfoMessage != nullptr);
  CHECK(panel.findChild<QLabel *>(QStringLiteral("playlistCueStatus")) ==
        nullptr);
  CHECK(userInfoCenter.severity() == UserInfoSeverity::None);
  CHECK(userInfoCenter.source().isEmpty());
  CHECK(userInfoCenter.text().isEmpty());
  CHECK(userInfoSeverity->text().isEmpty());
  CHECK(userInfoSource->text().isEmpty());
  CHECK(userInfoMessage->text().isEmpty());
  CHECK(userInfoSeverity->isHidden());
  CHECK(userInfoSource->isHidden());
  CHECK(userInfoMessage->isHidden());
  userInfoCenter.showWarning(QStringLiteral("PLAYLIST"),
                             QStringLiteral("Import is blocked."));
  CHECK(!userInfoSeverity->isHidden());
  CHECK(!userInfoSource->isHidden());
  CHECK(!userInfoMessage->isHidden());
  userInfoCenter.showSuccess(QStringLiteral("PLAYLIST"),
                             QStringLiteral("Import completed."));
  CHECK(userInfoCenter.severity() == UserInfoSeverity::Warning);
  CHECK(userInfoMessage->text() == QStringLiteral("Import is blocked."));
  userInfoCenter.showError(QStringLiteral("PROGRAM"),
                           QStringLiteral("Playback failed."),
                           QStringLiteral("test detail"));
  CHECK(userInfoBar->property("severity").toString() ==
        QStringLiteral("error"));
  CHECK(userInfoMessage->text() == QStringLiteral("Playback failed."));
  CHECK(userInfoBar->toolTip() == QStringLiteral("test detail"));

  UserInfoCenter audioWarningCenter;
  audioWarningCenter.setDisplayDuration(100);
  UserInfoBar audioWarningBar(&audioWarningCenter);
  auto *audioWarningLabel = audioWarningBar.findChild<QLabel *>(
      QStringLiteral("userInfoMessageLabel"));
  CHECK(audioWarningLabel != nullptr);
  int audioSettingsRequests = 0;
  QObject::connect(&audioWarningCenter, &UserInfoCenter::audioSettingsRequested,
                   &audioWarningBar, [&] { ++audioSettingsRequests; });
  audioWarningCenter.showAudioOutputWarning(
      vtrpon::LocalizedText("startup.audio_outputs_missing"));
  CHECK(audioWarningCenter.severity() == UserInfoSeverity::Warning);
  CHECK(audioWarningLabel->text().contains(QStringLiteral("href=\"audio-settings\"")));
  CHECK(audioWarningLabel->text().contains(QStringLiteral("text-decoration: underline")));
  CHECK(audioWarningLabel->text().contains(QStringLiteral("Open audio device settings")));
  CHECK(!audioWarningLabel->openExternalLinks());
  CHECK(audioWarningLabel->textInteractionFlags() & Qt::LinksAccessibleByKeyboard);
  audioWarningLabel->linkActivated(QStringLiteral("audio-settings"));
  CHECK(audioSettingsRequests == 1);
  audioWarningLabel->linkActivated(QStringLiteral("https://example.invalid"));
  CHECK(audioSettingsRequests == 1);
  QEventLoop audioWarningLoop;
  QTimer::singleShot(150, &audioWarningLoop, &QEventLoop::quit);
  audioWarningLoop.exec();
  CHECK(!audioWarningCenter.hasAudioSettingsAction());
  CHECK(!audioWarningLabel->text().contains(QStringLiteral("href=")));
  audioWarningLabel->linkActivated(QStringLiteral("audio-settings"));
  CHECK(audioSettingsRequests == 1);

  audioWarningCenter.showAudioOutputError(
      vtrpon::LocalizedText("startup.pfl_output_missing"));
  CHECK(audioWarningCenter.severity() == UserInfoSeverity::Error);
  CHECK(audioWarningCenter.text() == QStringLiteral("PFL audio device not found."));
  CHECK(audioWarningLabel->text().contains(QStringLiteral("href=\"audio-settings\"")));
  audioWarningLabel->linkActivated(QStringLiteral("audio-settings"));
  CHECK(audioSettingsRequests == 2);

  UserInfoCenter pflConfirmCenter;
  pflConfirmCenter.setDisplayDuration(50);
  UserInfoBar pflConfirmBar(&pflConfirmCenter);
  auto *pflConfirmLabel = pflConfirmBar.findChild<QLabel *>(
      QStringLiteral("userInfoMessageLabel"));
  CHECK(pflConfirmLabel != nullptr);
  int pflAccepted = 0;
  int pflDeclined = 0;
  int pflSettingsRequests = 0;
  QObject::connect(&pflConfirmCenter, &UserInfoCenter::pflSameDeviceDecision,
                   &pflConfirmBar, [&](bool accepted) {
                     if (accepted) ++pflAccepted;
                     else ++pflDeclined;
                   });
  QObject::connect(&pflConfirmCenter, &UserInfoCenter::audioSettingsRequested,
                   &pflConfirmBar, [&] { ++pflSettingsRequests; });
  pflConfirmCenter.showPflSameDeviceConfirmation();
  CHECK(pflConfirmCenter.hasPflSameDeviceConfirmation());
  CHECK(pflConfirmLabel->text().contains(QStringLiteral("href=\"pfl-yes\"")));
  CHECK(pflConfirmLabel->text().contains(QStringLiteral("href=\"pfl-no\"")));
  CHECK(pflConfirmLabel->text().contains(QStringLiteral("href=\"audio-settings\"")));
  QEventLoop pflConfirmLoop;
  QTimer::singleShot(80, &pflConfirmLoop, &QEventLoop::quit);
  pflConfirmLoop.exec();
  CHECK(pflConfirmCenter.hasPflSameDeviceConfirmation());
  pflConfirmCenter.showWarning(QStringLiteral("SYSTEM"), QStringLiteral("Queued warning."));
  CHECK(pflConfirmCenter.hasPflSameDeviceConfirmation());
  pflConfirmLabel->linkActivated(QStringLiteral("pfl-no"));
  CHECK(pflDeclined == 1 && pflAccepted == 0 && pflSettingsRequests == 0);
  CHECK(!pflConfirmCenter.hasPflSameDeviceConfirmation());
  pflConfirmLabel->linkActivated(QStringLiteral("pfl-yes"));
  CHECK(pflAccepted == 0);
  pflConfirmCenter.showPflSameDeviceConfirmation();
  pflConfirmLabel->linkActivated(QStringLiteral("audio-settings"));
  CHECK(pflDeclined == 2 && pflSettingsRequests == 1);
  CHECK(!pflConfirmCenter.hasPflSameDeviceConfirmation());
  pflConfirmCenter.showPflSameDeviceConfirmation();
  pflConfirmLabel->linkActivated(QStringLiteral("pfl-yes"));
  CHECK(pflAccepted == 1 && pflDeclined == 2);
  CHECK(!pflConfirmCenter.hasPflSameDeviceConfirmation());

  UserInfoCenter timingCenter;
  timingCenter.setDisplayDuration(100);
  timingCenter.showWarning(QStringLiteral("PLAYLIST"),
                           QStringLiteral("First important message."));
  timingCenter.showSuccess(QStringLiteral("PLAYLIST"),
                           QStringLiteral("Second important message."));
  CHECK(timingCenter.text() == QStringLiteral("First important message."));
  QEventLoop timingLoop;
  QTimer::singleShot(150, &timingLoop, &QEventLoop::quit);
  timingLoop.exec();
  CHECK(timingCenter.text() == QStringLiteral("Second important message."));
  QEventLoop clearLoop;
  QTimer::singleShot(100, &clearLoop, &QEventLoop::quit);
  clearLoop.exec();
  CHECK(timingCenter.severity() == UserInfoSeverity::None);
  CHECK(timingCenter.source().isEmpty());
  CHECK(timingCenter.text().isEmpty());
  QListWidget *list = panel.findChild<QListWidget *>(QStringLiteral("playlistList"));
  CHECK(list != nullptr);
  QTabBar *tabs = panel.findChild<QTabBar *>(QStringLiteral("playlistTabs"));
  QLineEdit *nameEdit =
      panel.findChild<QLineEdit *>(QStringLiteral("playlistNameEdit"));
  CHECK(tabs != nullptr);
  CHECK(nameEdit != nullptr);
  CHECK(tabs->acceptDrops());
  CHECK(tabs->count() == 2);
  CHECK(tabs->tabText(0) == QStringLiteral("1"));
  CHECK(tabs->tabText(1) == QStringLiteral("+"));
  CHECK(tabs->usesScrollButtons());
  CHECK(!tabs->expanding());
  auto *listViewButton = panel.findChild<QToolButton *>(
      QStringLiteral("playlistListViewButton"));
  auto *tileViewButton = panel.findChild<QToolButton *>(
      QStringLiteral("playlistTileViewButton"));
  CHECK(listViewButton != nullptr);
  CHECK(tileViewButton != nullptr);
  CHECK(listViewButton->text().isEmpty());
  CHECK(tileViewButton->text().isEmpty());
  CHECK(!listViewButton->icon().isNull());
  CHECK(!tileViewButton->icon().isNull());
  const QImage tileViewIcon = tileViewButton->icon()
      .pixmap(vtrpon::scaleSize(16, 16)).toImage();
  int tileIconVerticalPixels = 0;
  int tileIconHorizontalPixels = 0;
  const int tileIconCenterX = tileViewIcon.width() / 2;
  const int tileIconCenterY = tileViewIcon.height() / 2;
  for (int y = 0; y < tileViewIcon.height(); ++y) {
    if (tileViewIcon.pixelColor(tileIconCenterX, y).alpha() > 80) {
      ++tileIconVerticalPixels;
    }
  }
  for (int x = 0; x < tileViewIcon.width(); ++x) {
    if (tileViewIcon.pixelColor(x, tileIconCenterY).alpha() > 80) {
      ++tileIconHorizontalPixels;
    }
  }
  CHECK(tileIconVerticalPixels > tileViewIcon.height() / 2);
  CHECK(tileIconHorizontalPixels > tileViewIcon.width() / 2);
  CHECK(listViewButton->isChecked());
  CHECK(!tileViewButton->isChecked());
  CHECK(listViewButton->toolTip() == QStringLiteral("List view"));
  CHECK(tileViewButton->toolTip() == QStringLiteral(
      "Tile view\nClick a tile: ON AIR\nCtrl+Click: Select and prepare only"));
  CHECK(list->viewMode() == QListView::ListMode);
  CHECK(list->acceptDrops());
  auto *crossfadeCheckBox = panel.findChild<QCheckBox *>(
      QStringLiteral("playlistCrossfadeCheckBox"));
  auto *crossfadeDuration = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("playlistCrossfadeDurationSpinBox"));
  auto *crossfadeIncrease = panel.findChild<QToolButton *>(
      QStringLiteral("playlistCrossfadeDurationIncreaseButton"));
  auto *crossfadeDecrease = panel.findChild<QToolButton *>(
      QStringLiteral("playlistCrossfadeDurationDecreaseButton"));
  auto *bulkMenu = panel.bulkOperationsMenu();
  CHECK(bulkMenu != nullptr);
  CHECK(bulkMenu->title() == QStringLiteral("Playlist"));
  QList<QAction *> bulkActions;
  for (auto *action : bulkMenu->actions()) {
    if (action->data().isValid()) bulkActions.append(action);
  }
  CHECK(bulkActions.size() == 8);
  CHECK(!bulkActions[0]->isEnabled());
  CHECK(bulkActions[3]->isEnabled());
  CHECK(!bulkActions[7]->isEnabled());
  auto *bulkApply = panel.findChild<QPushButton *>(QStringLiteral("playlistBulkApplyButton"));
  auto *bulkCombo = panel.findChild<QComboBox *>(QStringLiteral("playlistBulkOperationComboBox"));
  CHECK(bulkApply && bulkCombo);
  CHECK(!bulkApply->isEnabled());
  CHECK(bulkCombo->findData(static_cast<int>(PlaylistBulkOperation::BgmMode)) >= 0);
  CHECK(crossfadeCheckBox != nullptr);
  CHECK(crossfadeDuration != nullptr);
  CHECK(crossfadeIncrease != nullptr);
  CHECK(crossfadeDecrease != nullptr);
  CHECK(crossfadeDuration->width() ==
        vtrpon::scalePx(kCompactDurationWidth));
  CHECK(crossfadeIncrease->size() == vtrpon::scaleSize(18, 12));
  CHECK(crossfadeDecrease->size() == vtrpon::scaleSize(18, 12));
  CHECK(!crossfadeDuration->isEnabled());
  CHECK(crossfadeDuration->buttonSymbols() == QAbstractSpinBox::NoButtons);
  CHECK(panel.crossfadeDurationMs() == 1000);
  crossfadeCheckBox->setChecked(true);
  CHECK(panel.crossfadeEnabled());
  CHECK(crossfadeDuration->isEnabled());
  crossfadeIncrease->click();
  CHECK(panel.crossfadeDurationMs() == 1100);
  crossfadeDecrease->click();
  CHECK(panel.crossfadeDurationMs() == 1000);
  list->setStyleSheet(QStringLiteral(
      "QListWidget#playlistList::item { background: #1d2123; }"
      "QListWidget#playlistList::item:selected {"
      " background: #21472b; color: #f3fff5;"
      " border: 2px solid #5fc36a; }"));

  QStringList droppedPaths;
  QObject::connect(&panel, &PlaylistPanel::filesDropped, &panel,
                   [&](const QStringList &paths) { droppedPaths = paths; });
  QObject::connect(
      &panel, &PlaylistPanel::itemDropRequested, &model,
      [&](quint64 itemId, int targetPlaylistNumber,
          int targetInsertionRow, bool copy) {
        if (copy) {
          (void)model.copyItemToPlaylist(itemId, targetPlaylistNumber,
                                         targetInsertionRow);
        } else {
          (void)model.moveItemToPlaylist(itemId, targetPlaylistNumber,
                                         targetInsertionRow);
        }
      });
  CHECK(qFuzzyCompare(list->property("dragPixmapOpacity").toDouble(), 0.60));

  QMimeData fileDropData;
  fileDropData.setUrls({QUrl::fromLocalFile(imageA.fileName()),
                        QUrl::fromLocalFile(imageB.fileName())});
  QDragEnterEvent dragEnter(QPoint(10, 10), Qt::CopyAction, &fileDropData,
                            Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &dragEnter);
  CHECK(dragEnter.isAccepted());
  QDropEvent fileDrop(QPointF(10, 10), Qt::CopyAction, &fileDropData,
                      Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &fileDrop);
  CHECK(fileDrop.isAccepted());
  CHECK(droppedPaths == QStringList({imageA.fileName(), imageB.fileName()}));

  QMimeData webDropData;
  webDropData.setUrls({QUrl(QStringLiteral("https://example.com/media.mp4"))});
  QDragEnterEvent webDragEnter(QPoint(10, 10), Qt::CopyAction, &webDropData,
                               Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &webDragEnter);
  CHECK(!webDragEnter.isAccepted());

  QList<PlaylistItemRegistration> registrations;
  for (const QString &path : {imageA.fileName(), imageB.fileName(),
                              imageC.fileName()}) {
    PlaylistItemRegistration item;
    item.filePath = path;
    item.mediaType = PlaylistMediaType::StillImage;
    item.settings.outPointMs = 1000;
    registrations.append(item);
  }
  model.addItems(registrations);
  CHECK(model.itemCount() == 3);
  CHECK(list->count() == 3);
  if (const QString captureDirectory = qEnvironmentVariable("VTRPON_TEST_PLAYLIST_ATTRIBUTES_CAPTURE");
      !captureDirectory.isEmpty()) {
    CHECK(QDir().mkpath(captureDirectory));
    const QString previousStyle = application.styleSheet();
    // Representative source attributes for visual inspection of the real delegate.
    list->item(0)->setData(playlist_view::kItemNameRole, QStringLiteral("1409899-uhd_3840_2160_25fps.mp4"));
    list->item(0)->setData(playlist_view::kItemResolutionRole, QStringLiteral("3840x2160"));
    list->item(0)->setData(playlist_view::kItemDurationRole, QStringLiteral("00:00:21.12"));
    list->item(0)->setData(playlist_view::kItemOutPointRole, QStringLiteral("00:00:21.12"));
    list->item(0)->setData(playlist_view::kItemMediaTypeRole, QStringLiteral("MP4"));
    for (const QString &skin : {QStringLiteral("hardware"), QStringLiteral("material")}) {
      QFile style(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()).filePath(
          skin == QStringLiteral("hardware") ? QStringLiteral("../apps/desktop/skins/hardware/controller.qss")
                                             : QStringLiteral("../apps/desktop/skins/material-dark/controller.qss")));
      CHECK(style.open(QIODevice::ReadOnly));
      application.setStyleSheet(QString::fromUtf8(style.readAll()));
      for (const int textSize : {0, 2}) {
        vtrpon::setUiTextSize(textSize);
        list->doItemsLayout();
        QApplication::processEvents();
        CHECK(list->grab().save(QDir(captureDirectory).filePath(
            QStringLiteral("%1-%2.png").arg(skin).arg(textSize))));
      }
    }
    vtrpon::setUiTextSize(0);
    application.setStyleSheet(previousStyle);
    list->doItemsLayout();
  }
  const quint64 firstId = model.itemIdAt(0);
  const quint64 secondId = model.itemIdAt(1);
  const quint64 thirdId = model.itemIdAt(2);
  CHECK(model.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::BgmMode));
  QApplication::processEvents();
  CHECK(tabs->tabText(0).contains(QStringLiteral("BGM")));
  constexpr int kBgmModeRole = Qt::UserRole + 22;
  CHECK(list->item(0)->data(kBgmModeRole).toBool());
  CHECK(model.applyBulkPlaybackOperation(
      1, PlaylistBulkPlaybackOperation::StandardMode));
  QApplication::processEvents();
  CHECK(!tabs->tabText(0).contains(QStringLiteral("BGM")));
  int bulkRequestCount = 0;
  PlaylistBulkOperation requestedBulkOperation =
      PlaylistBulkOperation::Undo;
  QObject::connect(
      &panel, &PlaylistPanel::bulkOperationRequested, &panel,
      [&](PlaylistBulkOperation operation) {
        ++bulkRequestCount;
        requestedBulkOperation = operation;
      });
  CHECK(bulkActions[0]->isEnabled());
  bulkActions[0]->trigger();
  CHECK(bulkRequestCount == 1);
  CHECK(requestedBulkOperation == PlaylistBulkOperation::PlayOnce);
  CHECK(!bulkActions[7]->isEnabled());
  panel.setBulkUndoAvailable(true);
  CHECK(bulkActions[7]->isEnabled());
  bulkActions[7]->trigger();
  CHECK(bulkRequestCount == 2);
  CHECK(requestedBulkOperation == PlaylistBulkOperation::Undo);
  panel.setBulkUndoAvailable(false);
  panel.resize(panel.width(), 600);
  panel.show();
  QApplication::processEvents();
  auto *playlistDeleteButton = panel.findChild<QPushButton *>(
      QStringLiteral("playlistDeleteButton"));
  CHECK(playlistDeleteButton != nullptr);
  CHECK(playlistDeleteButton->geometry().right() < panel.width());
  CHECK(tabs->geometry().right() < listViewButton->geometry().left());
  CHECK(listViewButton->geometry().right() <
        tileViewButton->geometry().left());
  CHECK(panel.width() - tileViewButton->geometry().right() <=
        vtrpon::scalePx(14));
  CHECK(list->visualItemRect(list->item(0)).height() >= 88);

  list->setCurrentRow(1);
  const quint64 selectedBeforeViewChange = panel.selectedItemId();
  tileViewButton->click();
  QApplication::processEvents();
  CHECK(tileViewButton->isChecked());
  CHECK(!listViewButton->isChecked());
  CHECK(list->viewMode() == QListView::IconMode);
  CHECK(list->flow() == QListView::LeftToRight);
  CHECK(list->isWrapping());
  CHECK(list->gridSize() == vtrpon::scaleSize(150, 120));
  CHECK(panel.selectedItemId() == selectedBeforeViewChange);
  const QRect firstTileRect = list->visualItemRect(list->item(0));
  const QRect secondTileRect = list->visualItemRect(list->item(1));
  CHECK(firstTileRect.width() >= vtrpon::scalePx(140));
  CHECK(firstTileRect.height() >= vtrpon::scalePx(110));
  CHECK(qAbs(firstTileRect.top() - secondTileRect.top()) <=
        vtrpon::scalePx(2));
  CHECK(firstTileRect.right() < secondTileRect.left());
  constexpr int kThumbnailRole = Qt::UserRole + 2;
  CHECK(!list->item(0)->data(kThumbnailRole).value<QPixmap>().isNull());
  CHECK(list->viewport()->cursor().shape() == Qt::PointingHandCursor);
  CHECK(list->accessibleDescription() == QStringLiteral(
      "Tile view. Click a tile to take it ON AIR. Ctrl+Click selects and "
      "prepares it without taking it ON AIR."));

  int tileOnAirRequestCount = 0;
  int tileEditRequestCount = 0;
  QObject::connect(&panel, &PlaylistPanel::onAirRequested, &panel,
                   [&tileOnAirRequestCount] { ++tileOnAirRequestCount; });
  QObject::connect(&panel, &PlaylistPanel::editRequested, &panel,
                   [&tileEditRequestCount] { ++tileEditRequestCount; });
  list->setCurrentRow(0);
  QApplication::processEvents();
  QImage liftedTileImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&liftedTileImage);

  QMouseEvent tilePress(QEvent::MouseButtonPress,
                        QPointF(firstTileRect.center()), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &tilePress);
  QApplication::processEvents();
  CHECK(list->property("pressedTileItemId").toULongLong() == firstId);
  QImage pressedTileImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&pressedTileImage);
  CHECK(differingPixelCount(liftedTileImage, pressedTileImage,
                            firstTileRect) > firstTileRect.width());
  QMouseEvent tileRelease(QEvent::MouseButtonRelease,
                          QPointF(firstTileRect.center()), Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &tileRelease);
  QApplication::processEvents();
  CHECK(list->property("pressedTileItemId").toULongLong() == 0);
  CHECK(panel.selectedItemId() == firstId);
  CHECK(tileOnAirRequestCount == 1);

  panel.setEditActivationMode(PlaylistEditActivationMode::Selection);
  QMouseEvent tileSelectPress(QEvent::MouseButtonPress,
                              QPointF(secondTileRect.center()),
                              Qt::LeftButton, Qt::LeftButton,
                              Qt::ControlModifier);
  QApplication::sendEvent(list->viewport(), &tileSelectPress);
  QMouseEvent tileSelectRelease(QEvent::MouseButtonRelease,
                                QPointF(secondTileRect.center()),
                                Qt::LeftButton, Qt::NoButton,
                                Qt::ControlModifier);
  QApplication::sendEvent(list->viewport(), &tileSelectRelease);
  QApplication::processEvents();
  CHECK(panel.selectedItemId() == secondId);
  CHECK(tileOnAirRequestCount == 1);
  CHECK(tileEditRequestCount == 0);
  panel.setEditActivationMode(PlaylistEditActivationMode::EditButton);

  QMimeData tileReorderData;
  tileReorderData.setData(
      QStringLiteral("application/x-qabstractitemmodeldatalist"),
      QByteArrayLiteral("playlist-tile-reorder-test"));
  QDragEnterEvent tileReorderEnter(firstTileRect.center(), Qt::MoveAction,
                                   &tileReorderData, Qt::LeftButton,
                                   Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &tileReorderEnter);
  CHECK(tileReorderEnter.isAccepted());
  CHECK(list->property("dropMarkerGap").toInt() == 1);
  QDragLeaveEvent tileReorderLeave;
  QApplication::sendEvent(list->viewport(), &tileReorderLeave);
  CHECK(list->property("dropMarkerGap").toInt() == -1);

  listViewButton->click();
  QApplication::processEvents();
  CHECK(listViewButton->isChecked());
  CHECK(!tileViewButton->isChecked());
  CHECK(list->viewMode() == QListView::ListMode);
  CHECK(panel.selectedItemId() == selectedBeforeViewChange);
  CHECK(list->viewport()->cursor().shape() == Qt::ArrowCursor);
  CHECK(list->dragEnabled());
  CHECK(list->acceptDrops());
  CHECK(list->dragDropMode() == QAbstractItemView::DragDrop);
  CHECK(list->visualItemRect(list->item(0)).height() >= 88);

  QMimeData reorderDragData;
  reorderDragData.setData(
      QStringLiteral("application/x-qabstractitemmodeldatalist"),
      QByteArrayLiteral("playlist-reorder-test"));
  QDragEnterEvent reorderEnter(QPoint(list->viewport()->width() / 2, 1),
                               Qt::MoveAction, &reorderDragData,
                               Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &reorderEnter);
  CHECK(reorderEnter.isAccepted());
  CHECK(list->property("dropMarkerGap").toInt() == 0);

  QDragMoveEvent reorderAtEnd(
      QPoint(list->viewport()->width() / 2,
             list->viewport()->height() - 2),
      Qt::MoveAction, &reorderDragData, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &reorderAtEnd);
  CHECK(reorderAtEnd.isAccepted());
  CHECK(list->property("dropMarkerGap").toInt() == 3);

  QImage markerImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&markerImage);
  int markerPixelCount = 0;
  for (int y = 0; y < markerImage.height(); ++y) {
    for (int x = 0; x < markerImage.width(); ++x) {
      const QColor pixel = markerImage.pixelColor(x, y);
      if (pixel.blue() > 220 && pixel.green() > 100 && pixel.red() < 80) {
        ++markerPixelCount;
      }
    }
  }
  CHECK(markerPixelCount > 100);

  QDragLeaveEvent reorderLeave;
  QApplication::sendEvent(list->viewport(), &reorderLeave);
  CHECK(list->property("dropMarkerGap").toInt() == -1);

  list->setCurrentRow(0);
  CHECK(panel.selectedItemId() == firstId);
  CHECK(!panel.pasteSelectedItemState());
  auto *playlistEditButton =
      panel.findChild<QPushButton *>(QStringLiteral("playlistEditButton"));
  CHECK(playlistEditButton != nullptr);
  int editRequestCount = 0;
  QObject::connect(&panel, &PlaylistPanel::editRequested,
                   [&editRequestCount] { ++editRequestCount; });
  panel.setEditActivationMode(PlaylistEditActivationMode::EditButton);
  CHECK(panel.editActivationMode() ==
        PlaylistEditActivationMode::EditButton);
  CHECK(playlistEditButton->isVisible());
  list->setCurrentRow(1);
  CHECK(editRequestCount == 0);
  playlistEditButton->click();
  CHECK(editRequestCount == 1);
  panel.setEditActivationMode(PlaylistEditActivationMode::Selection);
  CHECK(panel.editActivationMode() == PlaylistEditActivationMode::Selection);
  CHECK(!playlistEditButton->isVisible());
  list->setCurrentRow(2);
  CHECK(editRequestCount == 2);
  CHECK(panel.selectNextItemAfter(firstId));
  CHECK(panel.selectedItemId() == secondId);
  CHECK(editRequestCount == 3);
  panel.setEditActivationMode(PlaylistEditActivationMode::EditButton);
  list->setCurrentRow(0);
  CHECK(editRequestCount == 3);

  PlaylistItemSettings copiedModeSettings = model.itemState(firstId)->settings;
  copiedModeSettings.inPointMs = 111;
  copiedModeSettings.outPointMs = 911;
  copiedModeSettings.startMode = PlaylistStartMode::Play;
  copiedModeSettings.endMode = PlaylistEndMode::Goto;
  copiedModeSettings.fadeInEnabled = true;
  copiedModeSettings.fadeInDurationMs = 1111;
  copiedModeSettings.fadeOutEnabled = true;
  copiedModeSettings.fadeOutDurationMs = 1222;
  copiedModeSettings.endCrossfadeEnabled = true;
  copiedModeSettings.endCrossfadeDurationMs = 1333;
  copiedModeSettings.gotoTargetItemId = thirdId;
  copiedModeSettings.repeatCount = 3;
  copiedModeSettings.repeatEndMode = PlaylistRepeatEndMode::Pause;
  copiedModeSettings.nominalVolumeTenthsDb = -110;
  CHECK(model.updateItemSettings(firstId, copiedModeSettings));

  PlaylistItemSettings pasteTargetSettings =
      model.itemState(secondId)->settings;
  pasteTargetSettings.inPointMs = 222;
  pasteTargetSettings.outPointMs = 822;
  pasteTargetSettings.startMode = PlaylistStartMode::Pause;
  pasteTargetSettings.endMode = PlaylistEndMode::Repeat;
  pasteTargetSettings.fadeInEnabled = false;
  pasteTargetSettings.fadeInDurationMs = 2111;
  pasteTargetSettings.fadeOutEnabled = false;
  pasteTargetSettings.fadeOutDurationMs = 2222;
  pasteTargetSettings.endCrossfadeEnabled = false;
  pasteTargetSettings.endCrossfadeDurationMs = 2333;
  pasteTargetSettings.gotoTargetItemId = firstId;
  pasteTargetSettings.repeatCount = 7;
  pasteTargetSettings.repeatEndMode = PlaylistRepeatEndMode::Next;
  pasteTargetSettings.nominalVolumeTenthsDb = -230;
  CHECK(model.updateItemSettings(secondId, pasteTargetSettings));
  model.setItemCurrentPlayCount(secondId, 4);
  const PlaylistItemState pasteTargetBefore = *model.itemState(secondId);

  const QString firstFilePathBeforeRename = model.itemState(firstId)->filePath;
  CHECK(model.setItemDisplayName(firstId,
                                 QStringLiteral("  Opening Title  ")));
  CHECK(model.itemState(firstId)->displayName ==
        QStringLiteral("Opening Title"));
  CHECK(model.itemState(firstId)->filePath == firstFilePathBeforeRename);
  CHECK(!model.setItemDisplayName(firstId, QStringLiteral("   ")));
  CHECK(model.itemState(firstId)->displayName ==
        QStringLiteral("Opening Title"));
  CHECK(model.setItemColor(firstId, PlaylistItemColor::Yellow));
  CHECK(model.setItemSeparatorBelow(firstId, true));
  QApplication::processEvents();
  constexpr int kItemNameRole = Qt::UserRole + 7;
  constexpr int kItemColorRole = Qt::UserRole + 15;
  constexpr int kItemSeparatorBelowRole = Qt::UserRole + 16;
  CHECK(list->item(0)->data(kItemNameRole).toString() ==
        QStringLiteral("Opening Title"));
  CHECK(list->item(0)->data(kItemColorRole).toInt() ==
        static_cast<int>(PlaylistItemColor::Yellow));
  CHECK(list->item(0)->data(kItemSeparatorBelowRole).toBool());

  bool contextMenuVerified = false;
  QTimer::singleShot(0, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu || menu->objectName() !=
                     QStringLiteral("playlistItemContextMenu")) {
      return;
    }
    QAction *renameAction = menu->findChild<QAction *>(
        QStringLiteral("playlistItemRenameAction"));
    QAction *copyStateAction = menu->findChild<QAction *>(
        QStringLiteral("playlistItemCopyStateAction"));
    QAction *pasteStateAction = menu->findChild<QAction *>(
        QStringLiteral("playlistItemPasteStateAction"));
    QMenu *colorMenu = menu->findChild<QMenu *>(
        QStringLiteral("playlistItemColorMenu"));
    QAction *separatorAction = menu->findChild<QAction *>(
        QStringLiteral("playlistItemSeparatorBelowAction"));
    QAction *mapAction = menu->findChild<QAction *>(
        QStringLiteral("playlistItemMapAction"));
    auto *startMenu = menu->findChild<QMenu *>(QStringLiteral("playlistItemStartModeMenu"));
    auto *endMenu = menu->findChild<QMenu *>(QStringLiteral("playlistItemEndModeMenu"));
    contextMenuVerified = copyStateAction && pasteStateAction &&
                          startMenu && endMenu && startMenu->isEnabled() && endMenu->isEnabled() &&
                          startMenu->actions().size() == 4 && endMenu->actions().size() == 7 &&
                          startMenu->actions().at(static_cast<int>(copiedModeSettings.startMode))->isChecked() &&
                          copyStateAction->text() ==
                              QStringLiteral("Copy State") &&
                          pasteStateAction->text() ==
                              QStringLiteral("Paste State") &&
                          !pasteStateAction->isEnabled() &&
                          renameAction && colorMenu && separatorAction &&
                          mapAction && mapAction->text() ==
                              QStringLiteral("Map") &&
                          colorMenu->actions().size() == 5 &&
                          separatorAction->isCheckable() &&
                          separatorAction->isChecked();
    if (copyStateAction) {
      copyStateAction->trigger();
    }
    menu->close();
  });
  const QPoint contextPosition = list->visualItemRect(list->item(0)).center();
  QContextMenuEvent contextEvent(
      QContextMenuEvent::Mouse, contextPosition,
      list->viewport()->mapToGlobal(contextPosition));
  QApplication::sendEvent(list->viewport(), &contextEvent);
  CHECK(contextMenuVerified);
  CHECK(model.itemState(firstId)->settings == copiedModeSettings);

  list->setCurrentRow(1);
  bool contextPasteVerified = false;
  QTimer::singleShot(0, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu || menu->objectName() !=
                     QStringLiteral("playlistItemContextMenu")) {
      return;
    }
    QAction *pasteStateAction = menu->findChild<QAction *>(
        QStringLiteral("playlistItemPasteStateAction"));
    contextPasteVerified = pasteStateAction && pasteStateAction->isEnabled();
    if (pasteStateAction) {
      pasteStateAction->trigger();
    }
    menu->close();
  });
  const QPoint pasteContextPosition =
      list->visualItemRect(list->item(1)).center();
  QContextMenuEvent pasteContextEvent(
      QContextMenuEvent::Mouse, pasteContextPosition,
      list->viewport()->mapToGlobal(pasteContextPosition));
  QApplication::sendEvent(list->viewport(), &pasteContextEvent);
  CHECK(contextPasteVerified);

  PlaylistItemSettings expectedPastedSettings = pasteTargetSettings;
  expectedPastedSettings.startMode = copiedModeSettings.startMode;
  expectedPastedSettings.endMode = copiedModeSettings.endMode;
  expectedPastedSettings.fadeInEnabled = copiedModeSettings.fadeInEnabled;
  expectedPastedSettings.fadeOutEnabled = copiedModeSettings.fadeOutEnabled;
  expectedPastedSettings.endCrossfadeEnabled = copiedModeSettings.endCrossfadeEnabled;
  CHECK(list->item(0)->toolTip().isEmpty());
  CHECK(list->item(1)->toolTip().isEmpty());
  const PlaylistItemState pasteTargetAfter = *model.itemState(secondId);
  CHECK(pasteTargetAfter.settings == expectedPastedSettings);
  CHECK(pasteTargetAfter.id == pasteTargetBefore.id);
  CHECK(pasteTargetAfter.itemNumber == pasteTargetBefore.itemNumber);
  CHECK(pasteTargetAfter.playlistNumber == pasteTargetBefore.playlistNumber);
  CHECK(pasteTargetAfter.filePath == pasteTargetBefore.filePath);
  CHECK(pasteTargetAfter.displayName == pasteTargetBefore.displayName);
  CHECK(pasteTargetAfter.sourceType == pasteTargetBefore.sourceType);
  CHECK(pasteTargetAfter.mediaType == pasteTargetBefore.mediaType);
  CHECK(pasteTargetAfter.color == pasteTargetBefore.color);
  CHECK(pasteTargetAfter.separatorBelow == pasteTargetBefore.separatorBelow);
  CHECK(model.itemCurrentPlayCount(secondId) == 4);

  PlaylistItemSettings shortcutSourceSettings = copiedModeSettings;
  shortcutSourceSettings.startMode = PlaylistStartMode::Pause;
  shortcutSourceSettings.endMode = PlaylistEndMode::Stop;
  CHECK(model.updateItemSettings(firstId, shortcutSourceSettings));
  list->setCurrentRow(0);
  auto *menuCopyState = bulkMenu->findChild<QAction *>(QStringLiteral("playlistMenuCopyStateAction"));
  CHECK(menuCopyState && menuCopyState->isEnabled());
  menuCopyState->trigger();
  list->setCurrentRow(1);
  auto *menuPasteState = bulkMenu->findChild<QAction *>(QStringLiteral("playlistMenuPasteStateAction"));
  CHECK(menuPasteState && menuPasteState->isEnabled());
  menuPasteState->trigger();
  expectedPastedSettings.startMode = shortcutSourceSettings.startMode;
  expectedPastedSettings.endMode = shortcutSourceSettings.endMode;
  CHECK(model.itemState(secondId)->settings == expectedPastedSettings);
  CHECK(model.itemCurrentPlayCount(secondId) == 4);
  list->setCurrentRow(0);

  QImage selectedImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&selectedImage);
  const QRect selectedRect = list->visualItemRect(list->item(0));
  const QRect selectedTextRect = selectedRect.adjusted(130, 0, 0, 0);
  CHECK(matchingPixelCount(selectedImage, selectedTextRect,
                           [](const QColor &pixel) {
                             return pixel.red() > 150 &&
                                    pixel.green() > 150 &&
                                    pixel.blue() > 150;
                           }) > 80);
  CHECK(matchingPixelCount(selectedImage, selectedRect,
                           [](const QColor &pixel) {
                             return pixel.red() > pixel.blue() + 20 &&
                                    pixel.green() > pixel.blue() + 20;
                           }) > 500);
  PlaylistItemSettings repeatSettings = model.itemState(firstId)->settings;
  repeatSettings.startMode = PlaylistStartMode::Play;
  repeatSettings.fadeInEnabled = true;
  repeatSettings.fadeInDurationMs = 1234;
  repeatSettings.endMode = PlaylistEndMode::Repeat;
  repeatSettings.repeatCount = 3;
  repeatSettings.repeatEndMode = PlaylistRepeatEndMode::Pause;
  CHECK(model.updateItemSettings(firstId, repeatSettings));
  QApplication::processEvents();
  constexpr int kItemStartModeRole = Qt::UserRole + 12;
  constexpr int kItemEndModeRole = Qt::UserRole + 13;
  CHECK(list->item(0)->data(kItemStartModeRole).toString() ==
        QStringLiteral("PLAY + FI"));
  CHECK(list->item(0)->data(kItemEndModeRole).toString().contains(
      QStringLiteral("REPEAT(0/3)-PAUSE")));
  model.setOnAirItemId(firstId);
  model.setItemCurrentPlayCount(firstId, 1);
  QApplication::processEvents();
  CHECK(list->item(0)->data(kItemEndModeRole).toString().contains(
      QStringLiteral("REPEAT(1/3)-PAUSE")));
  model.setItemCurrentPlayCount(firstId, 2);
  QApplication::processEvents();
  CHECK(list->item(0)->data(kItemEndModeRole).toString().contains(
      QStringLiteral("REPEAT(2/3)-PAUSE")));
  repeatSettings.repeatEndMode = PlaylistRepeatEndMode::Next;
  repeatSettings.fadeOutEnabled = true;
  repeatSettings.fadeOutDurationMs = 2345;
  repeatSettings.endCrossfadeEnabled = true;
  repeatSettings.endCrossfadeDurationMs = 3456;
  CHECK(model.updateItemSettings(firstId, repeatSettings));
  QApplication::processEvents();
  CHECK(list->item(0)->data(kItemEndModeRole).toString() ==
        QStringLiteral("REPEAT(2/3)-NEXT + FO + CF"));
  model.setCueItemState(secondId, PlaylistCueState::Ready);
  model.setDskItemId(thirdId);
  model.setEditItemId(firstId);

  QApplication::processEvents();
  QImage onAirSelectedImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&onAirSelectedImage);
  const QRect onAirRect = list->visualItemRect(list->item(0));
  CHECK(matchingPixelCount(onAirSelectedImage, onAirRect,
                           [](const QColor &pixel) {
                             return pixel.red() > pixel.green() + 25;
                           }) > 1000);

  model.setOnAirItemId(secondId);
  QApplication::processEvents();
  QImage highlightEndpointsImage(list->viewport()->size(),
                                 QImage::Format_RGB32);
  list->viewport()->render(&highlightEndpointsImage);
  const QRect secondRect = list->visualItemRect(list->item(1));
  const qint64 outgoingHiddenScore =
      redDominanceScore(highlightEndpointsImage, onAirRect);
  const qint64 incomingFullScore =
      redDominanceScore(highlightEndpointsImage, secondRect);

  panel.setOnAirHighlightOpacity(firstId, 0.5);
  panel.setOnAirHighlightOpacity(secondId, 0.5);
  QApplication::processEvents();
  QImage highlightMidpointImage(list->viewport()->size(),
                                QImage::Format_RGB32);
  list->viewport()->render(&highlightMidpointImage);
  CHECK(redDominanceScore(highlightMidpointImage, onAirRect)
        > outgoingHiddenScore);
  CHECK(redDominanceScore(highlightMidpointImage, secondRect)
        < incomingFullScore);

  model.setOnAirItemId(firstId);

  list->setCurrentRow(2);
  QApplication::processEvents();
  QImage dskSelectedImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&dskSelectedImage);
  const QRect dskRect = list->visualItemRect(list->item(2));
  CHECK(matchingPixelCount(dskSelectedImage, dskRect,
                           [](const QColor &pixel) {
                             return pixel.blue() > pixel.green() + 10;
                           }) > 1000);
  const QRect dskTextRect = dskRect.adjusted(130, 0, 0, 0);
  CHECK(matchingPixelCount(dskSelectedImage, dskTextRect,
                           [](const QColor &pixel) {
                             return pixel.red() > 150 &&
                                    pixel.green() > 150 &&
                                    pixel.blue() > 150;
                           }) > 80);
  list->setCurrentRow(0);

  QMimeData moveFirstItemData;
  moveFirstItemData.setData(
      QStringLiteral("application/x-vtrpon-playlist-item"),
      QByteArray::number(firstId));
  const QPoint listBottom(list->viewport()->width() / 2,
                          list->viewport()->height() - 2);
  QDragEnterEvent moveFirstEnter(
      listBottom, Qt::MoveAction | Qt::CopyAction, &moveFirstItemData,
      Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &moveFirstEnter);
  CHECK(moveFirstEnter.isAccepted());
  QDropEvent moveFirstDrop(
      QPointF(listBottom), Qt::MoveAction | Qt::CopyAction,
      &moveFirstItemData, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &moveFirstDrop);
  CHECK(moveFirstDrop.isAccepted());
  CHECK(moveFirstDrop.dropAction() == Qt::MoveAction);
  CHECK(panel.selectedItemId() == firstId);
  CHECK(list->item(0)->text().startsWith(QStringLiteral("1  ")));
  CHECK(list->item(1)->text().startsWith(QStringLiteral("2  ")));
  CHECK(list->item(2)->text().startsWith(QStringLiteral("3  ")));
  CHECK(model.itemState(secondId)->itemNumber == 1);
  CHECK(model.itemState(thirdId)->itemNumber == 2);
  CHECK(model.itemState(firstId)->itemNumber == 3);
  CHECK(list->item(2)->text().contains(QStringLiteral("Opening Title")));
  const QString movedDescription =
      list->item(2)->data(Qt::AccessibleDescriptionRole).toString();
  CHECK(movedDescription.contains(QStringLiteral("ON AIR")));
  CHECK(movedDescription.contains(QStringLiteral("EDIT")));
  CHECK(movedDescription.contains(QStringLiteral("Yellow")));
  CHECK(movedDescription.contains(QStringLiteral("Separator below")));
  const QString dskDescription =
      list->item(1)->data(Qt::AccessibleDescriptionRole).toString();
  CHECK(dskDescription.contains(QStringLiteral("DSK")));
  CHECK(model.onAirItemId() == firstId);
  CHECK(model.cueItemId() == secondId);
  CHECK(model.dskItemId() == thirdId);
  CHECK(model.editItemId() == firstId);

  QMimeData moveThirdItemData;
  moveThirdItemData.setData(
      QStringLiteral("application/x-vtrpon-playlist-item"),
      QByteArray::number(thirdId));
  const QPoint listTop(list->viewport()->width() / 2, 1);
  QDragEnterEvent moveThirdEnter(
      listTop, Qt::MoveAction | Qt::CopyAction, &moveThirdItemData,
      Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &moveThirdEnter);
  CHECK(moveThirdEnter.isAccepted());
  QDropEvent moveThirdDrop(
      QPointF(listTop), Qt::MoveAction | Qt::CopyAction,
      &moveThirdItemData, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &moveThirdDrop);
  CHECK(moveThirdDrop.isAccepted());
  CHECK(model.itemCount() == 3);
  CHECK(list->count() == 3);
  CHECK(list->item(0)->text().contains(QFileInfo(imageC.fileName()).fileName()));
  CHECK(list->item(1)->text().contains(QFileInfo(imageB.fileName()).fileName()));
  CHECK(list->item(2)->text().contains(QStringLiteral("Opening Title")));
  CHECK(model.itemState(thirdId)->itemNumber == 1);
  CHECK(model.itemState(secondId)->itemNumber == 2);
  CHECK(model.itemState(firstId)->itemNumber == 3);

  const quint64 selectedInFirstPlaylist = panel.selectedItemId();
  tabs->setCurrentIndex(1);
  QApplication::processEvents();
  CHECK(model.activePlaylistNumber() == 2);
  CHECK(tabs->count() == 3);
  CHECK(tabs->tabText(1) == QStringLiteral("2"));
  CHECK(tabs->tabText(2) == QStringLiteral("+"));
  CHECK(list->count() == 0);
  nameEdit->setFocus();
  QApplication::processEvents();
  CHECK(nameEdit->testAttribute(Qt::WA_InputMethodEnabled)
        || nameEdit->inputMethodQuery(Qt::ImEnabled).toBool());
  QKeyEvent spacePress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
                       QStringLiteral(" "));
  QKeyEvent spaceRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier,
                         QStringLiteral(" "));
  QApplication::sendEvent(nameEdit, &spacePress);
  QApplication::sendEvent(nameEdit, &spaceRelease);
  CHECK(nameEdit->text() == QStringLiteral(" "));
  CHECK(model.playlistName(2) == QStringLiteral(" "));
  QKeyEvent enterPress(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QKeyEvent enterRelease(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(nameEdit, &enterPress);
  QApplication::sendEvent(nameEdit, &enterRelease);
  CHECK(model.activePlaylistNumber() == 2);
  CHECK(model.playlistName(2) == QStringLiteral(" "));

  PlaylistItemRegistration secondPlaylistItem;
  secondPlaylistItem.filePath = imageA.fileName();
  secondPlaylistItem.mediaType = PlaylistMediaType::StillImage;
  secondPlaylistItem.settings.outPointMs = 1000;
  secondPlaylistItem.playlistNumber = 2;
  model.addItems({secondPlaylistItem});
  QApplication::processEvents();
  CHECK(list->count() == 1);
  const quint64 secondPlaylistItemId = model.itemIdAt(0);
  CHECK(model.itemState(secondPlaylistItemId)->playlistNumber == 2);
  CHECK(model.itemState(secondPlaylistItemId)->itemNumber == 1);

  tabs->setCurrentIndex(0);
  QApplication::processEvents();
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(list->count() == 3);
  CHECK(panel.selectedItemId() == selectedInFirstPlaylist);

  QMimeData crossPlaylistDragData;
  crossPlaylistDragData.setData(
      QStringLiteral("application/x-vtrpon-playlist-item"),
      QByteArray::number(firstId));
  QDragEnterEvent enterTabBarAtAddTab(
      tabs->tabRect(2).center(), Qt::MoveAction | Qt::CopyAction,
      &crossPlaylistDragData, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(tabs, &enterTabBarAtAddTab);
  CHECK(enterTabBarAtAddTab.isAccepted());
  CHECK(enterTabBarAtAddTab.dropAction() == Qt::MoveAction);
  QApplication::processEvents();
  CHECK(model.activePlaylistNumber() == 1);

  QDragEnterEvent hoverSecondPlaylist(
      tabs->tabRect(1).center(), Qt::MoveAction | Qt::CopyAction,
      &crossPlaylistDragData, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(tabs, &hoverSecondPlaylist);
  CHECK(hoverSecondPlaylist.isAccepted());
  CHECK(hoverSecondPlaylist.dropAction() == Qt::MoveAction);
  QApplication::processEvents();
  CHECK(model.activePlaylistNumber() == 2);
  CHECK(list->count() == 1);

  QDragEnterEvent enterSecondPlaylistList(
      QPoint(list->viewport()->width() / 2,
             list->viewport()->height() - 2),
      Qt::MoveAction | Qt::CopyAction, &crossPlaylistDragData,
      Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &enterSecondPlaylistList);
  CHECK(enterSecondPlaylistList.isAccepted());

  QDropEvent moveAcrossPlaylists(
      QPointF(list->viewport()->width() / 2,
              list->viewport()->height() - 2),
      Qt::MoveAction | Qt::CopyAction, &crossPlaylistDragData,
      Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &moveAcrossPlaylists);
  CHECK(moveAcrossPlaylists.isAccepted());
  CHECK(moveAcrossPlaylists.dropAction() == Qt::MoveAction);
  CHECK(model.itemCount(1) == 2);
  CHECK(model.itemCount(2) == 2);
  CHECK(model.itemIdAt(2, 1) == firstId);
  CHECK(model.itemState(firstId)->playlistNumber == 2);
  CHECK(model.itemState(firstId)->itemNumber == 2);
  CHECK(model.onAirItemId() == firstId);
  CHECK(model.editItemId() == firstId);
  QApplication::processEvents();
  CHECK(tabs->property("onAirPlaylistNumber").toInt() == 2);
  QImage onAirTabImage(tabs->size(), QImage::Format_RGB32);
  tabs->render(&onAirTabImage);
  CHECK(matchingPixelCount(onAirTabImage, tabs->tabRect(1),
                           [](const QColor &pixel) {
                             return pixel.red() > pixel.green() + 25 &&
                                    pixel.red() > pixel.blue() + 15;
                           }) > 100);

  QDragEnterEvent hoverFirstPlaylist(
      tabs->tabRect(0).center(), Qt::MoveAction | Qt::CopyAction,
      &crossPlaylistDragData, Qt::LeftButton, Qt::ControlModifier);
  QApplication::sendEvent(tabs, &hoverFirstPlaylist);
  CHECK(hoverFirstPlaylist.isAccepted());
  CHECK(hoverFirstPlaylist.dropAction() == Qt::CopyAction);
  QApplication::processEvents();
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(list->count() == 2);

  QDragEnterEvent enterFirstPlaylistList(
      QPoint(list->viewport()->width() / 2,
             list->viewport()->height() - 2),
      Qt::MoveAction | Qt::CopyAction, &crossPlaylistDragData,
      Qt::LeftButton, Qt::ControlModifier);
  QApplication::sendEvent(list->viewport(), &enterFirstPlaylistList);
  CHECK(enterFirstPlaylistList.isAccepted());
  CHECK(enterFirstPlaylistList.dropAction() == Qt::CopyAction);

  const auto sourceBeforeCopy = model.itemState(firstId);
  CHECK(sourceBeforeCopy.has_value());
  QDropEvent copyAcrossPlaylists(
      QPointF(list->viewport()->width() / 2,
              list->viewport()->height() - 2),
      Qt::MoveAction | Qt::CopyAction, &crossPlaylistDragData,
      Qt::LeftButton, Qt::ControlModifier);
  QApplication::sendEvent(list->viewport(), &copyAcrossPlaylists);
  CHECK(copyAcrossPlaylists.isAccepted());
  CHECK(copyAcrossPlaylists.dropAction() == Qt::CopyAction);
  CHECK(model.itemCount(1) == 3);
  CHECK(model.itemCount(2) == 2);
  const quint64 copiedItemId = model.itemIdAt(1, 2);
  CHECK(copiedItemId != 0);
  CHECK(copiedItemId != firstId);
  const auto copiedItemState = model.itemState(copiedItemId);
  CHECK(copiedItemState.has_value());
  CHECK(copiedItemState->playlistNumber == 1);
  CHECK(copiedItemState->itemNumber == 3);
  CHECK(copiedItemState->filePath == sourceBeforeCopy->filePath);
  CHECK(copiedItemState->displayName == sourceBeforeCopy->displayName);
  CHECK(copiedItemState->color == sourceBeforeCopy->color);
  CHECK(copiedItemState->separatorBelow ==
        sourceBeforeCopy->separatorBelow);
  CHECK(copiedItemState->sourceType == sourceBeforeCopy->sourceType);
  CHECK(copiedItemState->mediaType == sourceBeforeCopy->mediaType);
  CHECK(copiedItemState->powerPointMode ==
        sourceBeforeCopy->powerPointMode);
  CHECK(copiedItemState->settings == sourceBeforeCopy->settings);
  CHECK(panel.selectedItemId() == copiedItemId);

  QMimeData dropOnTabData;
  dropOnTabData.setData(
      QStringLiteral("application/x-vtrpon-playlist-item"),
      QByteArray::number(copiedItemId));
  QDragEnterEvent enterSecondPlaylistTabForDrop(
      tabs->tabRect(1).center(), Qt::MoveAction | Qt::CopyAction,
      &dropOnTabData, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(tabs, &enterSecondPlaylistTabForDrop);
  CHECK(enterSecondPlaylistTabForDrop.isAccepted());
  QApplication::processEvents();
  QDropEvent dropOnSecondPlaylistTab(
      QPointF(tabs->tabRect(1).center()),
      Qt::MoveAction | Qt::CopyAction, &dropOnTabData,
      Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(tabs, &dropOnSecondPlaylistTab);
  CHECK(dropOnSecondPlaylistTab.isAccepted());
  CHECK(dropOnSecondPlaylistTab.dropAction() == Qt::MoveAction);
  CHECK(model.activePlaylistNumber() == 2);
  CHECK(model.itemCount(1) == 2);
  CHECK(model.itemCount(2) == 3);
  CHECK(model.itemIdAt(2, 2) == copiedItemId);
  CHECK(model.moveItemToPlaylist(copiedItemId, 1, model.itemCount(1)));
  tabs->setCurrentIndex(0);
  QApplication::processEvents();
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(model.itemCount(1) == 3);
  CHECK(model.itemCount(2) == 2);

  const auto dragTab = [tabs](int from, int to) -> bool {
    const QPointF start = tabs->tabRect(from).center();
    const QPointF end = tabs->tabRect(to).center();
    QMouseEvent press(QEvent::MouseButtonPress, start, tabs->mapToGlobal(start.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &press);
    const QImage beforeDrag = tabs->grab().toImage();
    const int sourceNumber = tabs->tabData(from).toInt();
    QMouseEvent move(QEvent::MouseMove, end, tabs->mapToGlobal(end.toPoint()),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &move);
    const QImage duringDrag = tabs->grab().toImage();
    const bool orderUnchanged = tabs->tabData(from).toInt() == sourceNumber;
    const bool ghostVisible = duringDrag != beforeDrag;
    const bool validTarget = tabs->tabData(to).toInt() > 0;
    const int markerX = to > from ? tabs->tabRect(to).right() : tabs->tabRect(to).left();
    const bool markerVisible = !validTarget || matchingPixelCount(
        duringDrag, QRect(markerX - 4, 0, 9, tabs->height()),
        [](const QColor &color) { return color == QColor(QStringLiteral("#0096ff")); }) > 0;
    QMouseEvent release(QEvent::MouseButtonRelease, end, tabs->mapToGlobal(end.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tabs, &release);
    return orderUnchanged && ghostVisible && markerVisible;
  };
  CHECK(dragTab(0, 1));
  CHECK(tabs->tabData(0).toInt() == 2);
  CHECK(tabs->tabData(1).toInt() == 1);
  CHECK(model.playlists().at(0).number == 2);
  CHECK(model.snapshot().playlists.at(0).number == 2);
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(model.itemCount(1) == 3);
  CHECK(model.itemCount(2) == 2);
  CHECK(dragTab(1, 2)); // The add tab stays at the right edge.
  CHECK(tabs->count() == 3);
  CHECK(tabs->tabData(2).toInt() == 0);
  CHECK(model.playlists().at(0).number == 2);
  CHECK(!model.movePlaylist(1, 2));
  CHECK(!model.movePlaylist(999, 0));
  CHECK(model.movePlaylist(1, 0));
  CHECK(tabs->tabData(0).toInt() == 1);
  CHECK(model.activePlaylistNumber() == 1);

  for (int expectedPlaylistNumber = 3; expectedPlaylistNumber <= 20;
       ++expectedPlaylistNumber) {
    CHECK(model.createPlaylist() == expectedPlaylistNumber);
  }
  QApplication::processEvents();
  CHECK(tabs->count() == 21);
  CHECK(tabs->tabText(19) == QStringLiteral("20"));
  CHECK(tabs->tabText(20) == QStringLiteral("+"));

  PlaylistItemRegistration uvcRegistration;
  uvcRegistration.sourceType = PlaylistSourceType::Uvc;
  uvcRegistration.mediaType = PlaylistMediaType::Video;
  uvcRegistration.filePath = QStringLiteral("camera-device-path");
  uvcRegistration.displayName = QStringLiteral("Test UVC Camera");
  uvcRegistration.settings.startMode = PlaylistStartMode::Play;
  const QList<quint64> uvcItemIds = model.addItems({uvcRegistration});
  CHECK(uvcItemIds.size() == 1);
  QApplication::processEvents();
  CHECK(list->count() == 4);
  QListWidgetItem *uvcListItem = list->item(3);
  CHECK(uvcListItem->text().contains(QStringLiteral("Test UVC Camera")));
  CHECK(uvcListItem->text().contains(QStringLiteral("LIVE")));
  CHECK(uvcListItem->text().contains(QStringLiteral("Play")));
  panel.setEditActivationMode(PlaylistEditActivationMode::Selection);
  const int editRequestCountBeforeUvcSelection = editRequestCount;
  list->setCurrentItem(uvcListItem);
  QApplication::processEvents();
  CHECK(editRequestCount == editRequestCountBeforeUvcSelection + 1);
  panel.setEditActivationMode(PlaylistEditActivationMode::EditButton);
  CHECK(playlistEditButton->isEnabled());
  QImage uvcThumbnail(120, 68, QImage::Format_RGB32);
  uvcThumbnail.fill(Qt::magenta);
  panel.setItemThumbnail(uvcItemIds.first(), uvcThumbnail);
  CHECK(!uvcListItem->data(Qt::UserRole + 2).value<QPixmap>().isNull());
  tabs->setCurrentIndex(1);
  QApplication::processEvents();
  tabs->setCurrentIndex(0);
  QApplication::processEvents();
  CHECK(list->count() == 4);
  uvcListItem = list->item(3);
  CHECK(!uvcListItem->data(Qt::UserRole + 2).value<QPixmap>().isNull());

  model.clearOnAirItem();
  model.clearCueItem();
  model.clearDskItem();
  model.clearEditItem();
  PlaylistModelSnapshot restoredSnapshot;
  restoredSnapshot.playlists = {
      PlaylistState{5, QStringLiteral("Imported A")},
      PlaylistState{7, QStringLiteral("Imported B")},
  };
  restoredSnapshot.activePlaylistNumber = 7;
  PlaylistItemState restoredFirstItem;
  restoredFirstItem.id = 101;
  restoredFirstItem.itemNumber = 1;
  restoredFirstItem.playlistNumber = 5;
  restoredFirstItem.filePath = imageB.fileName();
  restoredFirstItem.displayName = QFileInfo(imageB.fileName()).fileName();
  restoredFirstItem.mediaType = PlaylistMediaType::StillImage;
  restoredFirstItem.settings.outPointMs = 1000;
  PlaylistItemState restoredSecondItem;
  restoredSecondItem.id = 102;
  restoredSecondItem.itemNumber = 1;
  restoredSecondItem.playlistNumber = 7;
  restoredSecondItem.filePath = imageC.fileName();
  restoredSecondItem.displayName = QFileInfo(imageC.fileName()).fileName();
  restoredSecondItem.mediaType = PlaylistMediaType::StillImage;
  restoredSecondItem.settings.outPointMs = 1000;
  restoredSnapshot.items = {restoredFirstItem, restoredSecondItem};
  CHECK(model.restoreSnapshot(restoredSnapshot));
  QApplication::processEvents();
  CHECK(tabs->count() == 3);
  CHECK(tabs->tabText(0) == QStringLiteral("5"));
  CHECK(tabs->tabText(1) == QStringLiteral("7"));
  CHECK(tabs->tabText(2) == QStringLiteral("+"));
  CHECK(tabs->currentIndex() == 1);
  CHECK(nameEdit->text() == QStringLiteral("Imported B"));
  CHECK(list->count() == 1);
  CHECK(list->item(0)->text().contains(
      QFileInfo(imageC.fileName()).fileName()));
  CHECK(panel.selectedItemId() == 0);

  CHECK(imageC.remove());
  model.refreshMediaAvailability();
  QApplication::processEvents();
  constexpr int kItemMediaOfflineRole = Qt::UserRole + 17;
  CHECK(waitForAvailability(model, restoredSecondItem.id, true));
  CHECK(list->item(0)->data(kItemMediaOfflineRole).toBool());
  CHECK(list->item(0)->toolTip().isEmpty());
  QImage offlineImage(list->viewport()->size(), QImage::Format_RGB32);
  list->viewport()->render(&offlineImage);
  const QRect offlineItemRect = list->visualItemRect(list->item(0));
  const QRect offlineThumbnailRect(
      offlineItemRect.left() + 7,
      offlineItemRect.top() + (offlineItemRect.height() - 68) / 2,
      120, 68);
  CHECK(matchingPixelCount(offlineImage, offlineThumbnailRect,
                           [](const QColor &pixel) {
                             return pixel.red() > 150 &&
                                    pixel.red() > pixel.green() * 2 &&
                                    pixel.red() > pixel.blue() * 2;
                           }) > 5000);
  CHECK(matchingPixelCount(offlineImage, offlineThumbnailRect,
                           [](const QColor &pixel) {
                             return pixel.red() > 220 &&
                                    pixel.green() > 220 &&
                                    pixel.blue() > 220;
                           }) > 50);

  UserInfoCenter localizationCenter;
  localizationCenter.setDisplayDuration(0);
  UserInfoBar localizationBar(&localizationCenter);
  localizationCenter.showError(
      QStringLiteral("PLAYLIST"),
      vtrpon::LocalizedText("playlist.error.cue"));
  auto *localizedSeverity = localizationBar.findChild<QLabel *>(
      QStringLiteral("userInfoSeverityLabel"));
  auto *localizedSource = localizationBar.findChild<QLabel *>(
      QStringLiteral("userInfoSourceLabel"));
  auto *localizedMessage = localizationBar.findChild<QLabel *>(
      QStringLiteral("userInfoMessageLabel"));
  CHECK(localizedSeverity != nullptr);
  CHECK(localizedSource != nullptr);
  CHECK(localizedMessage != nullptr);
  CHECK(localizedMessage->text() == QStringLiteral("PREPARATION ERROR"));

  application.removeTranslator(&englishTranslator);
  QTranslator japaneseTranslator;
  CHECK(japaneseTranslator.load(
      QCoreApplication::applicationDirPath()
      + QStringLiteral("/vtrpon_ja.qm")));
  CHECK(application.installTranslator(&japaneseTranslator));
  localizationCenter.retranslate();
  QApplication::processEvents();

  CHECK(panel.findChild<QLabel *>(QStringLiteral("playlistTitle"))->text()
        == QStringLiteral("プレイリスト"));
  CHECK(panel.findChild<QPushButton *>(
            QStringLiteral("playlistAddButton"))->text()
        == QStringLiteral("ファイル追加"));
  CHECK(panel.findChild<QPushButton *>(
            QStringLiteral("playlistDeleteButton"))->text()
        == QStringLiteral("削除"));
  CHECK(bulkMenu->title() ==
        QStringLiteral("プレイリスト"));
  CHECK(bulkActions[0]->text() ==
        QStringLiteral("リストを一巡"));
  CHECK(bulkActions[1]->text() ==
        QStringLiteral("全部リピート"));
  CHECK(bulkActions[2]->text() ==
        QStringLiteral("ランダム再生"));
  CHECK(bulkActions[3]->text() ==
        QStringLiteral("BGMモードに設定"));
  CHECK(bulkActions[4]->text() ==
        QStringLiteral("BGMモードを解除"));
  CHECK(bulkActions[5]->text() ==
        QStringLiteral("プレイリストをクリア"));
  CHECK(bulkActions[6]->text() ==
        QStringLiteral("プレイリストを削除"));
  CHECK(bulkActions[7]->text() ==
        QStringLiteral("元に戻す"));
  CHECK(listViewButton->toolTip() == QStringLiteral("リスト表示"));
  CHECK(tileViewButton->toolTip() == QStringLiteral(
      "タイル表示\nクリック: オンエア\nCtrl+クリック: 選択・スタンバイのみ"));
  CHECK(panel.findChild<QLabel *>(
            QStringLiteral("playlistItemCount"))->text()
        .contains(QStringLiteral("項目")));
  CHECK(list->item(0)->text().contains(QStringLiteral("開始")));
  CHECK(localizedSeverity->text() == QStringLiteral("エラー"));
  CHECK(localizedSource->text() == QStringLiteral("プレイリスト"));
  CHECK(localizedMessage->text() == QStringLiteral("スタンバイエラー"));

  {
    ControllerSettingsDialog settingsDialog(nullptr, nullptr, nullptr,
                                            nullptr, nullptr);
    auto *settingsNavigation = settingsDialog.findChild<QListWidget *>(
        QStringLiteral("settingsNavigationList"));
    auto *allowSameItemRetake = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("allowSameItemOnAirRetakeCheckBox"));
    auto *showPlaybackRateControls = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("showPlaybackRateControlsCheckBox"));
    auto *disableFtbButton = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("disableFtbButtonCheckBox"));
    auto *showBgmPlayerControls = settingsDialog.findChild<QCheckBox *>(
        QStringLiteral("showBgmPlayerControlsCheckBox"));
    auto *audioThumbnailCombo = settingsDialog.findChild<QComboBox *>(
        QStringLiteral("playlistAudioThumbnailComboBox"));
    CHECK(settingsNavigation != nullptr);
    CHECK(settingsNavigation->item(0)->text() ==
          QStringLiteral("プレイリスト設定"));
    CHECK(settingsNavigation->item(networkSettingsRow(1))->text() ==
          QStringLiteral("オンエア設定"));
    CHECK(settingsNavigation->item(networkSettingsRow(3))->text() ==
          QStringLiteral("ビデオアウトプット"));
    CHECK(settingsNavigation->item(networkSettingsRow(4))->text() ==
          QStringLiteral("仮想カメラ"));
    CHECK(settingsNavigation->item(networkSettingsRow(13))->text() ==
          QStringLiteral("返しモニター"));
    CHECK(settingsNavigation->item(networkSettingsRow(2))->text() ==
          QStringLiteral("オーディオアウトプット"));
    CHECK(settingsNavigation->item(networkSettingsRow(6))->text() ==
          QStringLiteral("BGMプレイヤー"));
    CHECK(settingsNavigation->item(networkSettingsRow(7))->text() ==
          QStringLiteral("Roland連携"));
    CHECK(settingsNavigation->item(networkSettingsRow(8))->text() == QStringLiteral("ショットカットキー"));
    CHECK(settingsNavigation->item(networkSettingsRow(9))->text() == QStringLiteral("外観"));
    CHECK(settingsNavigation->item(networkSettingsRow(12))->text() ==
          QStringLiteral("言語と時刻"));
    if (vtrpon::networkVideoAddonInstalled()) {
      CHECK(settingsNavigation->item(2)->text() == QStringLiteral("ビデオアウトプット"));
      CHECK(settingsNavigation->item(3)->text() == QStringLiteral("NDI"));
      auto *input = settingsDialog.findChild<QCheckBox *>(QStringLiteral("networkVideoInputEnabled"));
      CHECK(input && input->text() == QStringLiteral("NDI入力を有効にする"));
      const auto directory = qEnvironmentVariable("VTRPON_NETWORK_VIDEO_UI_CAPTURE_DIR");
      if (!directory.isEmpty()) {
        settingsNavigation->setCurrentRow(3); settingsDialog.show(); QCoreApplication::processEvents();
        CHECK(settingsDialog.grab().save(QDir(directory).filePath(QStringLiteral("network-settings-ja.png"))));
      }
    }
    auto *timeZone = settingsDialog.findChild<QComboBox *>(QStringLiteral("timeZoneCombo"));
    auto *timeSize = settingsDialog.findChild<QSpinBox *>(QStringLiteral("currentTimeFontSize"));
    auto *timeRemaining = settingsDialog.findChild<QComboBox *>(QStringLiteral("timeRemainingDefaultCombo"));
    auto *timeSync = settingsDialog.findChild<QPushButton *>(QStringLiteral("timeSyncButton"));
    CHECK(timeZone && timeSize && timeRemaining && timeSync);
    timeZone->setCurrentIndex(timeZone->findData(QStringLiteral("Asia/Tokyo")));
    timeSize->setValue(24);
    timeRemaining->setCurrentIndex(1);
    settingsDialog.reloadSavedSettings();
    CHECK(timeZone->currentData().toString() == QStringLiteral("Asia/Tokyo"));
    CHECK(timeSize->value() == 24 && timeRemaining->currentIndex() == 1);
    auto *timeFont = settingsDialog.findChild<QFontComboBox *>(QStringLiteral("currentTimeFontCombo"));
    auto *resetSettings = settingsDialog.findChild<QPushButton *>(QStringLiteral("resetSettingsButton"));
    CHECK(timeFont && resetSettings);
    timeFont->setCurrentFont(QFont(QStringLiteral("Arial")));
    const auto fontRefresh = QObject::connect(&settingsDialog, &ControllerSettingsDialog::settingsReplaced,
        &settingsDialog, [&] { timeFont->setCurrentFont(QFont(QStringLiteral("Consolas"))); });
    resetSettings->click();
    QObject::disconnect(fontRefresh);
    CHECK(timeSize->value() == 12);
    CHECK(vtrpon::makeControllerSettings().value("time/fontSize", 12).toInt() == 12);
    CHECK(vtrpon::makeControllerSettings().value("time/fontFamily").toString().isEmpty());
    CHECK(timeFont->currentFont().family() == QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    int syncRequests = 0;
    QObject::connect(&settingsDialog, &ControllerSettingsDialog::timeSynchronizationRequested,
                     &settingsDialog, [&] { ++syncRequests; });
    timeSync->click();
    CHECK(syncRequests == 1);
    settingsDialog.setTimeSynchronizationState(true);
    CHECK(!timeSync->isEnabled());
    settingsDialog.setTimeSynchronizationState(false);
    CHECK(timeSync->isEnabled());
    if (const auto screenshot = qgetenv("VTRPON_TEST_TIME_SCREENSHOT"); !screenshot.isEmpty()) {
      settingsNavigation->setCurrentRow(networkSettingsRow(12));
      settingsDialog.show();
      application.processEvents();
      CHECK(settingsDialog.grab().save(QString::fromLocal8Bit(screenshot)));
      settingsDialog.close();
    }
    if (const auto screenshot = qgetenv("VTRPON_TEST_OPERATOR_SCREENSHOT"); !screenshot.isEmpty()) {
      settingsNavigation->setCurrentRow(networkSettingsRow(13));
      settingsDialog.findChild<QCheckBox *>("operatorMonitorInformation")->setChecked(true);
      settingsDialog.show();
      application.processEvents();
      CHECK(settingsDialog.grab().save(QString::fromLocal8Bit(screenshot)));
      settingsDialog.close();
    }
    if (const auto screenshot = qgetenv("VTRPON_TEST_SHORTCUT_SCREENSHOT"); !screenshot.isEmpty()) {
      settingsNavigation->setCurrentRow(networkSettingsRow(8));
      settingsDialog.show();
      application.processEvents();
      CHECK(settingsDialog.grab().save(QString::fromLocal8Bit(screenshot)));
      settingsDialog.close();
    }
    CHECK(allowSameItemRetake != nullptr);
    CHECK(allowSameItemRetake->text() == QStringLiteral(
        "オンエア中に、同じアイテムのオンエアを許可する"));
    CHECK(showPlaybackRateControls != nullptr);
    CHECK(showPlaybackRateControls->text() ==
          QStringLiteral("速度変更ボタンを表示する"));
    CHECK(disableFtbButton != nullptr);
    CHECK(disableFtbButton->text() ==
          QStringLiteral("FTBボタンを無効化"));
    CHECK(showBgmPlayerControls != nullptr);
    CHECK(showBgmPlayerControls->text() ==
          QStringLiteral("BGMプレイヤーを表示"));
    CHECK(audioThumbnailCombo != nullptr);
    CHECK(audioThumbnailCombo->itemText(audioThumbnailCombo->findData(false)) ==
          QStringLiteral("波形"));
    CHECK(audioThumbnailCombo->itemText(audioThumbnailCombo->findData(true)) ==
          QStringLiteral("アルバムアート（取得できない場合は波形）"));
    if (const auto screenshot = qgetenv("VTRPON_TEST_PLAYLIST_SETTINGS_SCREENSHOT");
        !screenshot.isEmpty()) {
      settingsNavigation->setCurrentRow(0);
      settingsDialog.show();
      application.processEvents();
      CHECK(settingsDialog.grab().save(QString::fromLocal8Bit(screenshot)));
      settingsDialog.close();
    }
  }

  return 0;
}
