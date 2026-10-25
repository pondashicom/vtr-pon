#pragma once

#include <QWidget>
#include <memory>

class MacroRunner;
class PlaylistModel;
class QCloseEvent;
namespace vtrpon {
class NetworkTimeClock;
class UiLanguageManager;
}

// A modeless, independently selected view of playlist macros. Closing this
// window only hides it; the Controller owns the runner and its lifetime.
class MacroEditorWindow final : public QWidget {
  Q_OBJECT
public:
  explicit MacroEditorWindow(PlaylistModel *model, MacroRunner *runner,
                             QWidget *parent = nullptr);
  ~MacroEditorWindow() override;
  void setLanguageManager(vtrpon::UiLanguageManager *manager);
  void setClock(vtrpon::NetworkTimeClock *clock);

signals:
  void saveRequested();
  void addFilesRequested(int playlistNumber);

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  struct Private;
  std::unique_ptr<Private> d_;
};
