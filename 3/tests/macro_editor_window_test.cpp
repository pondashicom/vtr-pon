#include "controller_operations.h"
#include "language_manager.h"
#include "macro_editor_window.h"
#include "macro_runner.h"
#include "playlist_model.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryFile>
#include <QTimer>

#include <cstdio>
#include <memory>

#define CHECK(expression)                                                       \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression);              \
      return 1;                                                                \
    }                                                                          \
  } while (false)

template <typename T>
T *find(MacroEditorWindow &window, const char *name) {
  return window.findChild<T *>(QString::fromLatin1(name));
}

int main(int argc, char **argv) {
  QApplication application(argc, argv);
#ifdef Q_OS_WIN
  // The offscreen plugin may use a font database without Windows fallback.
  // Provide a Japanese-capable test font without changing product preferences.
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    const QString windowsDirectory = qEnvironmentVariable("WINDIR");
    if (!windowsDirectory.isEmpty()) {
      const int fontId = QFontDatabase::addApplicationFont(
          QDir(windowsDirectory).filePath(QStringLiteral("Fonts/meiryo.ttc")));
      const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
      if (!families.isEmpty()) {
        QFont font = application.font();
        font.setFamily(families.front());
        application.setFont(font);
      }
    }
  }
#endif
  const QString stylesheet = qEnvironmentVariable("VTRPON_MACRO_STYLE");
  if (!stylesheet.isEmpty()) {
    QFile qss(stylesheet);
    CHECK(qss.open(QIODevice::ReadOnly));
    application.setStyleSheet(QString::fromUtf8(qss.readAll()));
  }
  ControllerOperations operations;
  PlaylistModel model;
  QTemporaryFile file;
  CHECK(file.open());
  PlaylistItemRegistration registration;
  registration.filePath = file.fileName();
  registration.displayName = QStringLiteral("Opening");
  registration.mediaType = PlaylistMediaType::Video;
  registration.playlistNumber = 1;
  const auto initial = model.addItems({registration});
  CHECK(initial.size() == 1);
  const quint64 first = initial.front();
  const int secondPlaylist = model.createPlaylist();
  registration.playlistNumber = secondPlaylist;
  registration.displayName = QStringLiteral("Next segment");
  const auto next = model.addItems({registration});
  CHECK(next.size() == 1);
  const quint64 second = next.front();
  model.setActivePlaylistNumber(1);
  model.setEditItemId(first);
  model.setCueItemState(first, PlaylistCueState::Ready);

  qint64 elapsed = 0;
  const QDateTime wall = QDateTime::fromString(QStringLiteral("2030-01-01T17:59:00+09:00"), Qt::ISODate);
  MacroRunner runner(&model, &operations,
      {[&] { return wall.addMSecs(elapsed); }, [&] { return elapsed; }});
  vtrpon::UiLanguageManager languageManager(&application);
  MacroEditorWindow window(&model, &runner);
  window.setLanguageManager(&languageManager);
  window.show();
  application.processEvents();
  CHECK(!window.isModal());
  CHECK(window.isWindow());
  auto *playlists = find<QComboBox>(window, "macroSourcePlaylistCombo");
  auto *items = find<QListWidget>(window, "macroSourceItems");
  auto *table = find<QTableView>(window, "macroStepTable");
  CHECK(playlists && items && table);
  playlists->setCurrentIndex(playlists->findData(secondPlaylist));
  CHECK(playlists->currentText() == QStringLiteral("Playlist %1").arg(secondPlaylist));
  CHECK(items->count() == 1);
  items->setCurrentRow(0);
  CHECK(model.activePlaylistNumber() == 1);
  CHECK(model.editItemId() == first);
  CHECK(model.cueItemId() == first);

  // The timed helper creates normal editable steps and references exact IDs.
  auto *date = find<QDateTimeEdit>(window, "macroTimeCueDateTime");
  auto *timed = find<QPushButton>(window, "macroAddTimeCueButton");
  CHECK(date && timed);
  const QDateTime scheduled = wall.addSecs(60);
  date->setDateTime(scheduled);
  timed->click();
  CHECK(model.macros().size() == 1);
  CHECK(model.macros().front().steps.size() == 3);
  const auto generated = model.macros().front().steps;
  CHECK(generated[0].type == MacroStepType::Cue);
  CHECK(generated[1].type == MacroStepType::WaitUntil);
  CHECK(generated[2].type == MacroStepType::Take);
  CHECK(generated[0].itemId == second && generated[2].itemId == second);
  CHECK(generated[0].documentId == model.documentId());
  CHECK(generated[1].scheduledAt == scheduled);
  CHECK(generated[2].requireReady);
  CHECK(!generated[2].scheduledAt.isValid());
  CHECK(model.activePlaylistNumber() == 1 && model.cueItemId() == first);

  // Row moves and the enabled column update the persisted definition, keeping IDs.
  table->selectRow(2);
  find<QPushButton>(window, "macroMoveStepUpButton")->click();
  CHECK(model.macros().front().steps[1].id == generated[2].id);
  CHECK(table->model()->setData(table->model()->index(1, 1), Qt::Unchecked, Qt::CheckStateRole));
  CHECK(!model.macros().front().steps[1].enabled);
  const std::unique_ptr<QMimeData> drag(table->model()->mimeData({table->model()->index(1, 0)}));
  CHECK(table->model()->dropMimeData(drag.get(), Qt::MoveAction, 3, 0, {}));
  CHECK(model.macros().front().steps[2].id == generated[2].id);
  CHECK(!model.macros().front().steps[2].enabled);
  CHECK(table->model()->setData(table->model()->index(2, 1), Qt::Checked, Qt::CheckStateRole));

  // Settings editing records only checked fields; it must not mutate the item.
  auto *addType = find<QComboBox>(window, "macroAddStepTypeCombo");
  CHECK(addType);
  addType->setCurrentIndex(addType->findData(int(MacroStepType::ItemSettings)));
  find<QPushButton>(window, "macroAddStepButton")->click();
  auto *enableStart = find<QCheckBox>(window, "macroPatchEnable_startMode");
  auto *startMode = find<QComboBox>(window, "macroPatchValue_startMode");
  CHECK(enableStart && startMode);
  enableStart->setChecked(true);
  startMode->setCurrentIndex(startMode->findData(QStringLiteral("play")));
  CHECK(model.itemState(second)->settings.startMode == PlaylistStartMode::Pause);
  CHECK((model.macros().front().steps.back().settingsPatch ==
        QJsonObject{{QStringLiteral("startMode"), QStringLiteral("play")}}));
  enableStart->setChecked(false);
  CHECK(model.macros().front().steps.back().settingsPatch.isEmpty());
  // Status ticks must not rebuild the table or destroy the selected field editor.
  const int settingsRow = table->currentIndex().row();
  QEventLoop statusTick;
  QTimer::singleShot(350, &statusTick, &QEventLoop::quit);
  statusTick.exec();
  CHECK(table->currentIndex().row() == settingsRow);
  CHECK(find<QComboBox>(window, "macroPatchValue_startMode") == startMode);
  enableStart->setChecked(true);
  startMode->setCurrentIndex(startMode->findData(QStringLiteral("pause")));
  CHECK(model.macros().front().steps.back().settingsPatch.value(QStringLiteral("startMode")) == QStringLiteral("pause"));

  // Exercise runtime translation without initialize()/setLanguage(), which
  // would write the actual user's language preference through QSettings.
  languageManager.languageChanged(vtrpon::UiLanguage::Japanese);
  CHECK(find<QPushButton>(window, "macroNewButton")->text() == QStringLiteral("新規"));
  CHECK(find<QPushButton>(window, "macroRunButton")->text() == QStringLiteral("実行"));
  CHECK(find<QComboBox>(window, "macroSourcePlaylistCombo")->currentText() ==
        QStringLiteral("プレイリスト %1").arg(secondPlaylist));
  languageManager.languageChanged(vtrpon::UiLanguage::English);
  CHECK(find<QPushButton>(window, "macroNewButton")->text() == QStringLiteral("New"));
  CHECK(find<QPushButton>(window, "macroRunButton")->text() == QStringLiteral("Run"));
  if (qEnvironmentVariable("VTRPON_MACRO_LANGUAGE") == QStringLiteral("ja"))
    languageManager.languageChanged(vtrpon::UiLanguage::Japanese);
  table = find<QTableView>(window, "macroStepTable");

  // Capture the actual widget layout after source/step construction, if requested.
  const QString screenshot = qEnvironmentVariable("VTRPON_MACRO_SCREENSHOT");
  if (!screenshot.isEmpty()) {
    application.processEvents();
    CHECK(window.grab().save(screenshot));
  }

  MacroDefinition delayed;
  delayed.name = QStringLiteral("Delay test");
  MacroStep delay;
  delay.type = MacroStepType::Delay;
  delay.durationMs = 10000;
  delayed.steps = {delay};
  CHECK(model.setMacros({delayed}));
  CHECK(runner.start(delayed));
  application.processEvents();
  CHECK(runner.isActive());
  CHECK(!find<QPushButton>(window, "macroAddStepButton")->isEnabled());
  CHECK(!find<QPushButton>(window, "macroAddFilesButton")->isEnabled());
  CHECK(!table->model()->setData(table->model()->index(0, 1), Qt::Unchecked, Qt::CheckStateRole));
  window.close();
  CHECK(!window.isVisible());
  CHECK(runner.isActive());
  runner.stop();
  CHECK(!runner.isActive());
  CHECK(model.cueItemId() == first);
  return 0;
}
