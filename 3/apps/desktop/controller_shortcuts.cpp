#include "controller_settings.h"
#include "controller_shortcuts.h"
#include "controller_shortcut_settings.h"

#include "controller_operations.h"
#include "controller_volume_input.h"
#include "playlist_panel.h"
#include "program_panel.h"

#include <QAction>
#include <QHash>
#include <QApplication>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QShortcut>
#include <QString>
#include <QWidget>

#include <functional>

class ControllerShortcuts::Impl final : public QObject {
public:
  Impl(QWidget *controlWindow, PlaylistPanel *playlistPanel,
       ProgramPanel *programPanel,
       ControllerOperations *operations,
       QAction *addFileAction, QAction *exitAction,
       vtrpon::ViewMenuController *) {
    playlist_ = playlistPanel;
    program_ = programPanel;
    QSettings settings = vtrpon::makeControllerSettings();
    bindings_ = vtrpon::shortcutSettings::load(settings);
    using Op = ControllerOperations::Action;
    const QPointer<ControllerOperations> guarded(operations);
    const auto installOperation = [this, controlWindow,
                                   guarded](Action key, Op operation) {
      install(
          key, controlWindow,
          [guarded, operation] {
            if (guarded)
              (void)guarded->execute(operation);
          });
    };
    installOperation(Action::TakeOnAir, Op::TakeSelectedOnAir);
    installOperation(Action::ToggleOnAirPlayback, Op::ProgramTogglePlayback);
    installOperation(Action::LoadEdit, Op::EditLoadSelected);
    installOperation(Action::CopyItemState, Op::CopySelectedState);
    installOperation(Action::PasteItemState, Op::PasteSelectedState);
    installOperation(Action::DocumentNext, Op::DocumentNext);
    installOperation(Action::DocumentPrevious, Op::DocumentPrevious);
    installOperation(Action::ToggleFtb, Op::ToggleFtb);
    installOperation(Action::ToggleDsk, Op::ToggleDsk);
    installOperation(Action::OffAir, Op::OffAir);
    installOperation(Action::MasterFadeIn, Op::MasterFadeIn);
    installOperation(Action::MasterFadeOut, Op::MasterFadeOut);
    installOperation(Action::ItemFadeIn, Op::ItemFadeIn);
    installOperation(Action::ItemFadeOut, Op::ItemFadeOut);
    installOperation(Action::BgmFadeIn, Op::BgmFadeInConfigured);
    installOperation(Action::BgmFadeOut, Op::BgmFadeOutConfigured);
    if (playlistPanel) {
      configureMenuAction(Action::CopyItemState,
          playlistPanel->bulkOperationsMenu()->findChild<QAction *>(QStringLiteral("playlistMenuCopyStateAction")));
      configureMenuAction(Action::PasteItemState,
          playlistPanel->bulkOperationsMenu()->findChild<QAction *>(QStringLiteral("playlistMenuPasteStateAction")));
    }
    const QList<QPair<Action, PlaylistBulkOperation>> bulkBindings{
          {Action::PlaylistPlayOnce, PlaylistBulkOperation::PlayOnce},
          {Action::PlaylistRepeatAll, PlaylistBulkOperation::RepeatAll},
          {Action::PlaylistShuffle, PlaylistBulkOperation::Shuffle},
          {Action::PlaylistBgmMode, PlaylistBulkOperation::BgmMode},
          {Action::PlaylistStandardMode, PlaylistBulkOperation::StandardMode},
          {Action::PlaylistClear, PlaylistBulkOperation::Clear},
          {Action::PlaylistDelete, PlaylistBulkOperation::DeletePlaylist},
          {Action::PlaylistUndo, PlaylistBulkOperation::Undo}
    };
    for (const auto &binding : bulkBindings) {
      install(binding.first, controlWindow, [guarded, operation = binding.second] {
        if (guarded)
          (void)guarded->execute(Op::PlaylistBulk, operation);
      });
      if (playlistPanel) {
        for (auto *menuAction : playlistPanel->bulkOperationsMenu()->actions()) {
          if (!menuAction->data().isValid() ||
              menuAction->data().toInt() != static_cast<int>(binding.second)) continue;
          configureMenuAction(binding.first, menuAction);
          break;
        }
      }
    }
    installOperation(Action::PreviewCapture, Op::PreviewCapture);
    installOperation(Action::ProgramCapture, Op::ProgramCapture);
    installOperation(Action::TogglePfl, Op::TogglePfl);
    installOperation(Action::OperatorTimerStart, Op::OperatorTimerStart);
    installOperation(Action::OperatorTimerPause, Op::OperatorTimerPause);
    installOperation(Action::OperatorTimerReset, Op::OperatorTimerReset);
    installOperation(Action::OperatorTimerSettings, Op::OperatorTimerSettings);
    installOperation(Action::OpenNominalEq, Op::OpenNominalEq);
    installOperation(Action::OpenItemEq, Op::OpenItemEq);
    installOperation(Action::OpenMasterEq, Op::OpenMasterEq);
    if (controlWindow) {
      configureMenuAction(Action::OperatorTimerSettings,
          controlWindow->findChild<QAction *>(QStringLiteral("operatorTimerSettingsAction")));
    }
    configureMenuAction(Action::AddFile, addFileAction);
    installOperation(Action::EditToggleStartMode, Op::EditToggleStartMode);
    installOperation(Action::EditToggleFadeOut, Op::EditToggleFadeOut);
    installOperation(Action::EditReset, Op::EditReset);
    installOperation(Action::EditSetIn, Op::EditSetIn);
    installOperation(Action::EditSetOut, Op::EditSetOut);
    const QList<QPair<Action, PlaylistEndMode>> endModeBindings{
        {Action::EditEndStop, PlaylistEndMode::Stop},
        {Action::EditEndPause, PlaylistEndMode::Pause},
        {Action::EditEndRepeat, PlaylistEndMode::Repeat},
        {Action::EditEndNext, PlaylistEndMode::Next}
    };
    for (const auto &binding : endModeBindings) {
      install(binding.first, controlWindow, [guarded, mode = binding.second] {
        if (guarded)
          (void)guarded->execute(Op::EditEndMode, mode);
      });
    }
    if (controlWindow) {
      for (int index = static_cast<int>(Action::EditToggleStartMode);
           index <= static_cast<int>(Action::EditSetOut); ++index) {
        const auto keyAction = static_cast<Action>(index);
        const auto name = QStringLiteral("editMenu") +
                          vtrpon::shortcutSettings::definitions().at(index).id;
        const QPointer<QAction> menuAction = controlWindow->findChild<QAction *>(name);
        configureMenuAction(keyAction, menuAction);
      }
    }
    if (controlWindow) {
      for (int index = 0; index < vtrpon::shortcutSettings::definitions().size(); ++index) {
        configureMenuAction(static_cast<Action>(index), controlWindow->findChild<QAction *>(
            QStringLiteral("onAirMenu") + vtrpon::shortcutSettings::definitions().at(index).id));
      }
    }
    configureMenuAction(Action::Exit, exitAction);
    installOperation(Action::AddFile, Op::AddFilesDialog);
    installOperation(Action::Exit, Op::Exit);
    installOperation(Action::NextOutputDisplay, Op::NextOutputDisplay);

    const auto installFixedOperation = [this, guarded](const QString &id, Op operation,
                                                      const QVariantList &arguments = {}) {
      handlers_.insert(id, [guarded, operation, arguments] {
        if (guarded) (void)guarded->dispatch(operation, arguments);
      });
    };
    installFixedOperation(QStringLiteral("SelectPreviousItem"), Op::SelectRelativeItem, {-1});
    installFixedOperation(QStringLiteral("SelectNextItem"), Op::SelectRelativeItem, {1});
    installFixedOperation(QStringLiteral("RemoveSelectedItem"), Op::RemoveSelectedItem);
    installFixedOperation(QStringLiteral("OpenSettings"), Op::OpenSettings);
    installFixedOperation(QStringLiteral("IncreaseItemVolume"), Op::AdjustItemVolume, {10});
    installFixedOperation(QStringLiteral("DecreaseItemVolume"), Op::AdjustItemVolume, {-10});
    installFixedOperation(QStringLiteral("IncreaseMasterVolume"), Op::AdjustMasterVolume, {10});
    installFixedOperation(QStringLiteral("DecreaseMasterVolume"), Op::AdjustMasterVolume, {-10});

    if (controlWindow) {
      const QKeySequence settingsKey(Qt::CTRL | Qt::ALT | Qt::Key_S);
      if (auto *settingsAction = controlWindow->findChild<QAction *>(QStringLiteral("settingsAction"))) {
        settingsAction->setShortcut(settingsKey);
        settingsAction->setShortcutContext(Qt::WidgetShortcut);
        settingsAction->setAutoRepeat(false);
      }
      auto *settingsShortcut = new QShortcut(settingsKey, controlWindow);
      settingsShortcut->setContext(Qt::WindowShortcut);
      settingsShortcut->setAutoRepeat(false);
      connect(settingsShortcut, &QShortcut::activated, this, [guarded] {
        if (guarded) (void)guarded->executeFrom("Shortcut", Op::OpenSettings);
      });
      shortcuts_.append(settingsShortcut);
      for (int digit = 1; digit <= 9; ++digit) {
        for (bool playlist : {false, true}) {
          const auto action = static_cast<Action>(18 + (digit - 1) * 2 + (playlist ? 1 : 0));
          install(action, controlWindow, [guarded, digit, playlist] {
            if (guarded)
              (void)guarded->execute(playlist ? Op::ActivatePlaylist : Op::SelectItemNumber, digit);
          });
        }
      }
    }

    if (playlistPanel) {
      playlistPanel->setShortcutLabels(label(Action::LoadEdit),
                                       label(Action::TakeOnAir));
    }
    if (programPanel) {
      programPanel->setPlayPauseShortcutLabel(
          label(Action::ToggleOnAirPlayback));
    }

    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *current) {
              updateEnabledState(current);
            });
    updateEnabledState(QApplication::focusWidget());
    new ControllerVolumeInput(operations, controlWindow, this);
  }

  void reload() {
    QSettings settings = vtrpon::makeControllerSettings();
    bindings_ = vtrpon::shortcutSettings::load(settings);
    for (const auto &entry : installed_) if (entry.second) entry.second->setKeys(keys(entry.first));
    for (const auto &entry : menus_) if (entry.second) entry.second->setShortcuts(keys(entry.first));
    if (playlist_) playlist_->setShortcutLabels(label(Action::LoadEdit), label(Action::TakeOnAir));
    if (program_) program_->setPlayPauseShortcutLabel(label(Action::ToggleOnAirPlayback));
    updateEnabledState(QApplication::focusWidget());
  }
  bool triggerAction(const QString &id, const char *source) {
    const auto handler = handlers_.value(id);
    if (!handler) return false;
    ControllerOperations::InputScope scope(source);
    handler();
    return true;
  }
private:
  using Action = vtrpon::shortcutSettings::Action;
  QList<QKeySequence> keys(Action action) const {
    return vtrpon::shortcutSettings::sequences(bindings_, static_cast<int>(action));
  }
  QString label(Action action) const {
    QStringList labels;
    for (const auto &key : keys(action)) labels.append(key.toString(QKeySequence::NativeText));
    return labels.join(QStringLiteral(" / "));
  }

  void configureMenuAction(Action action, QAction *menuAction) {
    if (!menuAction) {
      return;
    }
    menuAction->setShortcuts(keys(action));
    menus_.append({action, menuAction});
    menuAction->setShortcutContext(Qt::WidgetShortcut);
    menuAction->setAutoRepeat(false);
  }

  void install(Action action, QWidget *controlWindow,
               const std::function<void()> &handler) {
    const auto id = vtrpon::shortcutSettings::definitions().at(static_cast<int>(action)).id;
    handlers_.insert(id, handler);
    if (!controlWindow) {
      return;
    }
    {
      auto *shortcut = new QShortcut(controlWindow);
      shortcut->setKeys(keys(action));
      installed_.append({action, shortcut});
      shortcut->setContext(Qt::WindowShortcut);
      shortcut->setAutoRepeat(false);
      connect(shortcut, &QShortcut::activated, this, [this, id] { triggerAction(id, "Shortcut"); });
      shortcuts_.append(shortcut);
    }
  }

  void updateEnabledState(QWidget *focusWidget) {
    const bool textInputActive =
        focusWidget &&
        (focusWidget->testAttribute(Qt::WA_InputMethodEnabled) ||
         focusWidget->inputMethodQuery(Qt::ImEnabled).toBool());
    for (const QPointer<QShortcut> &shortcut : shortcuts_) {
      if (shortcut) {
        shortcut->setEnabled(!textInputActive);
      }
    }
  }

  QJsonObject bindings_;
  QPointer<PlaylistPanel> playlist_;
  QPointer<ProgramPanel> program_;
  QList<QPair<Action, QPointer<QShortcut>>> installed_;
  QList<QPair<Action, QPointer<QAction>>> menus_;
  QList<QPointer<QShortcut>> shortcuts_;
  QHash<QString, std::function<void()>> handlers_;
};

ControllerShortcuts::ControllerShortcuts(
    QWidget *controlWindow, PlaylistPanel *playlistPanel,
    ProgramPanel *programPanel, ControllerOperations *operations, QAction *addFileAction,
    QAction *exitAction, vtrpon::ViewMenuController *viewMenuController)
    : impl_(std::make_unique<Impl>(controlWindow, playlistPanel,
                                  programPanel, operations, addFileAction,
                                  exitAction, viewMenuController)) {}

ControllerShortcuts::~ControllerShortcuts() = default;

void ControllerShortcuts::reloadSavedSettings() { impl_->reload(); }
bool ControllerShortcuts::triggerAction(const QString &id, const char *source) { return impl_->triggerAction(id, source); }
