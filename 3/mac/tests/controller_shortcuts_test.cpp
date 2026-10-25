#include "controller_operations.h"
#include "controller_settings.h"
#include "controller_shortcut_settings.h"
#include "controller_shortcuts.h"
#include "playlist_item_operations.h"
#include "playlist_panel.h"
#include "playlist_model.h"
#include <QInputMethodEvent>
#include <QListWidget>
#include <QTemporaryFile>

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMetaEnum>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>

namespace {
using Op = ControllerOperations::Action;

struct ExpectedOperation {
  Op action;
  QVariantList arguments;
};

ExpectedOperation expectedOperation(const QString &id) {
  const QHash<QString, Op> aliases{
      {QStringLiteral("AddFile"), Op::AddFilesDialog},
      {QStringLiteral("TakeOnAir"), Op::TakeSelectedOnAir},
      {QStringLiteral("ToggleOnAirPlayback"), Op::ProgramTogglePlayback},
      {QStringLiteral("LoadEdit"), Op::EditLoadSelected},
      {QStringLiteral("CopyItemState"), Op::CopySelectedState},
      {QStringLiteral("PasteItemState"), Op::PasteSelectedState},
      {QStringLiteral("BgmFadeIn"), Op::BgmFadeInConfigured},
      {QStringLiteral("BgmFadeOut"), Op::BgmFadeOutConfigured}
  };
  if (aliases.contains(id)) return {aliases.value(id), {}};
  const QHash<QString, ExpectedOperation> fixed{
      {QStringLiteral("SelectPreviousItem"), {Op::SelectRelativeItem, {-1}}},
      {QStringLiteral("SelectNextItem"), {Op::SelectRelativeItem, {1}}},
      {QStringLiteral("RemoveSelectedItem"), {Op::RemoveSelectedItem, {}}},
      {QStringLiteral("OpenSettings"), {Op::OpenSettings, {}}},
      {QStringLiteral("IncreaseItemVolume"), {Op::AdjustItemVolume, {10}}},
      {QStringLiteral("DecreaseItemVolume"), {Op::AdjustItemVolume, {-10}}},
      {QStringLiteral("IncreaseMasterVolume"), {Op::AdjustMasterVolume, {10}}},
      {QStringLiteral("DecreaseMasterVolume"), {Op::AdjustMasterVolume, {-10}}}
  };
  if (fixed.contains(id)) return fixed.value(id);
  for (const auto &prefix : {QStringLiteral("SelectItem"), QStringLiteral("ActivatePlaylist")}) {
    if (id.startsWith(prefix)) {
      return {prefix == QStringLiteral("SelectItem") ? Op::SelectItemNumber : Op::ActivatePlaylist,
              {id.mid(prefix.size()).toInt()}};
    }
  }
  const QHash<QString, PlaylistBulkOperation> bulk{
      {QStringLiteral("PlaylistPlayOnce"), PlaylistBulkOperation::PlayOnce},
      {QStringLiteral("PlaylistRepeatAll"), PlaylistBulkOperation::RepeatAll},
      {QStringLiteral("PlaylistShuffle"), PlaylistBulkOperation::Shuffle},
      {QStringLiteral("PlaylistBgmMode"), PlaylistBulkOperation::BgmMode},
      {QStringLiteral("PlaylistStandardMode"), PlaylistBulkOperation::StandardMode},
      {QStringLiteral("PlaylistClear"), PlaylistBulkOperation::Clear},
      {QStringLiteral("PlaylistDelete"), PlaylistBulkOperation::DeletePlaylist},
      {QStringLiteral("PlaylistUndo"), PlaylistBulkOperation::Undo}
  };
  if (bulk.contains(id)) return {Op::PlaylistBulk, {QVariant::fromValue(bulk.value(id))}};
  const QHash<QString, PlaylistEndMode> endModes{
      {QStringLiteral("EditEndStop"), PlaylistEndMode::Stop},
      {QStringLiteral("EditEndPause"), PlaylistEndMode::Pause},
      {QStringLiteral("EditEndRepeat"), PlaylistEndMode::Repeat},
      {QStringLiteral("EditEndNext"), PlaylistEndMode::Next}
  };
  if (endModes.contains(id)) return {Op::EditEndMode, {QVariant::fromValue(endModes.value(id))}};
  const int value = QMetaEnum::fromType<Op>().keyToValue(id.toUtf8().constData());
  return {value < 0 ? Op::Count : static_cast<Op>(value), {}};
}

bool check(bool condition, const char *expression, int line) {
  if (!condition) std::fprintf(stderr, "shortcut regression line %d: %s\n", line, expression);
  return condition;
}
#define SHORTCUT_CHECK(expression) \
  do { if (!check((expression), #expression, __LINE__)) return false; } while (false)
}

bool runControllerShortcutsRegression() {
  namespace shortcuts = vtrpon::shortcutSettings;
  QSettings settings = vtrpon::makeControllerSettings();
  const bool hadBindings = settings.contains(shortcuts::key());
  const QVariant originalBindings = settings.value(shortcuts::key());
  const auto restore = qScopeGuard([&] {
    if (hadBindings) settings.setValue(shortcuts::key(), originalBindings);
    else settings.remove(shortcuts::key());
  });
  settings.remove(shortcuts::key());
  ControllerOperations operations;
  QWidget window;
  auto *layout = new QVBoxLayout(&window);
  auto *focusButton = new QPushButton(QStringLiteral("Keyboard target"), &window);
  auto *textInput = new QLineEdit(&window);
  layout->addWidget(focusButton);
  layout->addWidget(textInput);
  int widgetInvocations = 0;
  // Old adapter targets deliberately exist but cannot execute an operation.
  for (const auto *name : {"previewCaptureButton", "programCaptureButton", "editPflButton",
                           "operatorTimerStart", "operatorTimerPause", "operatorTimerReset",
                           "operatorTimerSettingsButton", "editNominalEqButton",
                           "programItemEqButton", "programMasterEqButton"}) {
    auto *button = new QPushButton(&window);
    button->setObjectName(QString::fromLatin1(name));
    button->setEnabled(false);
    button->hide();
    QObject::connect(button, &QPushButton::clicked, &window, [&] { ++widgetInvocations; });
  }
  for (const auto &definition : shortcuts::definitions()) {
    for (const auto &prefix : {QStringLiteral("editMenu"), QStringLiteral("onAirMenu")}) {
      auto *action = new QAction(&window);
      action->setObjectName(prefix + definition.id);
      action->setEnabled(false);
      QObject::connect(action, &QAction::triggered, &window, [&] { ++widgetInvocations; });
    }
  }
  auto *settingsAction = new QAction(&window);
  settingsAction->setObjectName(QStringLiteral("settingsAction"));
  settingsAction->setEnabled(false);
  QObject::connect(settingsAction, &QAction::triggered, &window, [&] { ++widgetInvocations; });
  int handlerCalls = 0;
  QSet<Op> bound;
  for (const auto &definition : shortcuts::definitions()) {
    const auto expected = expectedOperation(definition.id);
    SHORTCUT_CHECK(expected.action != Op::Count);
    if (bound.contains(expected.action)) continue;
    bound.insert(expected.action);
    if (expected.action == Op::PlaylistBulk)
      operations.bind<PlaylistBulkOperation>(expected.action, &window, [&](PlaylistBulkOperation) { ++handlerCalls; });
    else if (expected.action == Op::EditEndMode)
      operations.bind<PlaylistEndMode>(expected.action, &window, [&](PlaylistEndMode) { ++handlerCalls; });
    else if (expected.action == Op::SelectItemNumber || expected.action == Op::ActivatePlaylist)
      operations.bind<int>(expected.action, &window, [&](int) { ++handlerCalls; });
    else operations.bind<>(expected.action, &window, [&] { ++handlerCalls; });
  }
  operations.bind<>(Op::OpenSettings, &window, [&] { ++handlerCalls; });
  operations.bind<int>(Op::AdjustItemVolume, &window, [&](int) { ++handlerCalls; });
  operations.bind<int>(Op::AdjustMasterVolume, &window, [&](int) { ++handlerCalls; });
  operations.bind<int>(Op::SelectRelativeItem, &window, [&](int) { ++handlerCalls; });
  operations.bind<>(Op::RemoveSelectedItem, &window, [&] { ++handlerCalls; });
  int notified = 0;
  Op lastAction = Op::Count;
  QVariantList lastArguments;
  QString lastSource;
  QObject::connect(&operations, &ControllerOperations::operationAboutToExecute, &window,
      [&](Op action, const QVariantList &arguments, const QString &source) {
        ++notified; lastAction = action; lastArguments = arguments; lastSource = source;
      });
  // Neither panel nor ViewMenuController is needed to translate keyboard input.
  ControllerShortcuts routes(&window, nullptr, nullptr, &operations, nullptr, nullptr, nullptr);
  const auto installed = window.findChildren<QShortcut *>();
  SHORTCUT_CHECK(installed.size() == shortcuts::definitions().size() + 1);
  const auto findShortcut = [&](const QKeySequence &key) -> QShortcut * {
    QShortcut *found = nullptr;
    for (auto *shortcut : installed) {
      if (!shortcut->keys().contains(key)) continue;
      if (found) return nullptr;
      found = shortcut;
    }
    return found;
  };
  for (const auto &definition : shortcuts::definitions()) {
    auto *shortcut = findShortcut(definition.keys.first());
    SHORTCUT_CHECK(shortcut && shortcut->keys() == definition.keys);
    SHORTCUT_CHECK(!shortcut->autoRepeat() && shortcut->context() == Qt::WindowShortcut);
    const auto expected = expectedOperation(definition.id);
    const int before = handlerCalls;
    SHORTCUT_CHECK(QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection));
    SHORTCUT_CHECK(handlerCalls == before + 1 && notified == handlerCalls);
    SHORTCUT_CHECK(lastAction == expected.action && lastArguments == expected.arguments);
    SHORTCUT_CHECK(lastSource == QStringLiteral("Shortcut"));
  }
  const QKeySequence settingsKey(Qt::CTRL | Qt::ALT | Qt::Key_S);
  for (const auto &definition : shortcuts::definitions()) {
    const auto expected = expectedOperation(definition.id);
    const int before = handlerCalls;
    SHORTCUT_CHECK(routes.triggerAction(definition.id, "MIDI"));
    SHORTCUT_CHECK(handlerCalls == before + 1 && notified == handlerCalls);
    SHORTCUT_CHECK(lastAction == expected.action && lastArguments == expected.arguments && lastSource == "MIDI");
  }
  SHORTCUT_CHECK(!routes.triggerAction(QStringLiteral("InvalidMidiAction"), "MIDI"));
  for (const auto &definition : shortcuts::fixedDefinitions()) {
    const auto expected = expectedOperation(definition.id);
    const int before = handlerCalls;
    SHORTCUT_CHECK(routes.triggerAction(definition.id, "MIDI"));
    SHORTCUT_CHECK(handlerCalls == before + 1 && notified == handlerCalls);
    SHORTCUT_CHECK(lastAction == expected.action && lastArguments == expected.arguments && lastSource == "MIDI");
  }
  auto *settingsShortcut = findShortcut(settingsKey);
  SHORTCUT_CHECK(settingsShortcut && settingsAction->shortcut() == settingsKey);
  SHORTCUT_CHECK(settingsAction->shortcutContext() == Qt::WidgetShortcut);
  SHORTCUT_CHECK(QMetaObject::invokeMethod(settingsShortcut, "activated", Qt::DirectConnection));
  SHORTCUT_CHECK(lastAction == Op::OpenSettings && lastSource == QStringLiteral("Shortcut"));
  SHORTCUT_CHECK(widgetInvocations == 0 && notified == handlerCalls);

  bool ftbAllowed = false;
  operations.setGuard<>(Op::ToggleFtb, &window, [&] { return ftbAllowed; });
  auto *ftbShortcut = findShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F));
  SHORTCUT_CHECK(ftbShortcut);
  const int beforeBlocked = handlerCalls;
  SHORTCUT_CHECK(QMetaObject::invokeMethod(ftbShortcut, "activated", Qt::DirectConnection));
  SHORTCUT_CHECK(handlerCalls == beforeBlocked && notified == beforeBlocked);
  ftbAllowed = true;
  SHORTCUT_CHECK(QMetaObject::invokeMethod(ftbShortcut, "activated", Qt::DirectConnection));
  SHORTCUT_CHECK(handlerCalls == beforeBlocked + 1 && notified == handlerCalls);

  QJsonObject custom;
  custom.insert(QStringLiteral("PreviewCapture"), QJsonArray{QStringLiteral("F6")});
  custom.insert(QStringLiteral("TogglePfl"), QJsonArray{});
  settings.setValue(shortcuts::key(), custom.toVariantMap());
  routes.reloadSavedSettings();
  SHORTCUT_CHECK(!findShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)));
  SHORTCUT_CHECK(!findShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_P)));
  auto *customCapture = findShortcut(QKeySequence(Qt::Key_F6));
  SHORTCUT_CHECK(customCapture);
  SHORTCUT_CHECK(QMetaObject::invokeMethod(customCapture, "activated", Qt::DirectConnection));
  SHORTCUT_CHECK(lastAction == Op::PreviewCapture && lastSource == QStringLiteral("Shortcut"));
  SHORTCUT_CHECK(findShortcut(settingsKey) == settingsShortcut);

  window.show();
  window.activateWindow();
  textInput->setFocus();
  QApplication::processEvents();
  SHORTCUT_CHECK(QApplication::focusWidget() == textInput);
  for (auto *shortcut : installed) SHORTCUT_CHECK(!shortcut->isEnabled());
  const int beforeText = handlerCalls;
  QKeyEvent textVolume(QEvent::KeyPress, Qt::Key_Up, Qt::ControlModifier);
  QApplication::sendEvent(textInput, &textVolume);
  SHORTCUT_CHECK(handlerCalls == beforeText);
  focusButton->setFocus();
  QApplication::processEvents();
  for (auto *shortcut : installed) SHORTCUT_CHECK(shortcut->isEnabled());
  for (const auto modifiers : {Qt::ControlModifier, Qt::ShiftModifier}) {
    QKeyEvent volume(QEvent::KeyPress, Qt::Key_Up, modifiers);
    QApplication::sendEvent(focusButton, &volume);
    SHORTCUT_CHECK(lastAction == (modifiers == Qt::ControlModifier ? Op::AdjustItemVolume : Op::AdjustMasterVolume));
    SHORTCUT_CHECK(lastArguments == QVariantList{10} && lastSource == QStringLiteral("Shortcut"));
    QWheelEvent wheel(QPointF(1, 1), QPointF(1, 1), QPoint(), QPoint(0, -120),
                      Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(focusButton, &wheel);
    SHORTCUT_CHECK(lastArguments == QVariantList{-10} && lastSource == QStringLiteral("Shortcut"));
  }
  SHORTCUT_CHECK(handlerCalls == beforeText + 4 && notified == handlerCalls && widgetInvocations == 0);
#ifdef Q_OS_MACOS
  window.hide();
  PlaylistModel model;
  ControllerOperations panelOperations;
  PlaylistPanel panel(&model, nullptr, false, nullptr, false, &panelOperations);
  QTemporaryFile media(QDir::tempPath() + QStringLiteral("/mac-input-XXXXXX.mp4"));
  SHORTCUT_CHECK(media.open());
  PlaylistItemRegistration registration;
  registration.filePath = media.fileName();
  const auto ids = model.addItems({registration, registration});
  SHORTCUT_CHECK(ids.size() == 2);
  panelOperations.bind<quint64>(Op::RemoveItem, &panel,
      [&](quint64 id) { model.removeItem(id); });
  auto *list = panel.findChild<QListWidget *>(QStringLiteral("playlistList"));
  auto *name = panel.findChild<QLineEdit *>(QStringLiteral("playlistNameEdit"));
  SHORTCUT_CHECK(list && name);
  const auto press = [](QWidget *target, int key) {
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &down);
    QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(target, &up);
  };
  panel.show();
  panel.activateWindow();
  name->setFocus();
  name->setText(QStringLiteral("abc"));
  QApplication::processEvents();
  press(name, Qt::Key_Down);
  press(name, Qt::Key_Up);
  SHORTCUT_CHECK(panel.focusWidget() == name && panel.selectedItemId() == 0);
  QInputMethodEvent preedit(QStringLiteral("にほんご"), {});
  QApplication::sendEvent(name, &preedit);
  press(name, Qt::Key_Down);
  SHORTCUT_CHECK(panel.focusWidget() == name && panel.selectedItemId() == 0);
  QInputMethodEvent commit;
  commit.setCommitString(QStringLiteral("日本語"));
  QApplication::sendEvent(name, &commit);
  SHORTCUT_CHECK(name->text() == QStringLiteral("abc日本語"));
  press(name, Qt::Key_Backspace);
  SHORTCUT_CHECK(name->text() == QStringLiteral("abc日本") && model.itemCount() == 2);
  list->setCurrentRow(0);
  list->setFocus();
  QApplication::processEvents();
  press(list, Qt::Key_Down);
  SHORTCUT_CHECK(panel.selectedItemId() == ids[1]);
  press(list, Qt::Key_Up);
  SHORTCUT_CHECK(panel.selectedItemId() == ids[0]);
  model.setOnAirItemId(ids[0]);
  press(list, Qt::Key_Backspace);
  SHORTCUT_CHECK(model.itemCount() == 2);
  model.clearOnAirItem();
  press(list, Qt::Key_Backspace);
  SHORTCUT_CHECK(model.itemCount() == 1);
  press(list, Qt::Key_Delete);
  SHORTCUT_CHECK(model.itemCount() == 0);
#endif
  return true;
}
