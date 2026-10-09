#pragma once

#include <memory>

class ControllerOperations;
class PlaylistPanel;
class ProgramPanel;
class QAction;
class QWidget;
class QString;
namespace vtrpon { class ViewMenuController; }

class ControllerShortcuts final {
public:
  ControllerShortcuts(QWidget *controlWindow, PlaylistPanel *playlistPanel,
                      ProgramPanel *programPanel,
                      ControllerOperations *operations,
                      QAction *addFileAction, QAction *exitAction,
                      vtrpon::ViewMenuController *viewMenuController);
  ~ControllerShortcuts();
  void reloadSavedSettings();
  bool triggerAction(const QString &id, const char *source);

  ControllerShortcuts(const ControllerShortcuts &) = delete;
  ControllerShortcuts &operator=(const ControllerShortcuts &) = delete;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
