#pragma once

#include <QObject>
#include <QSet>
#include <QString>

#include <optional>

class AudioOutputDeviceMonitor;
class EditSessionController;
class PlaylistModel;
class ProgramPlayoutController;
class UserInfoCenter;

class AudioOutputCoordinator final : public QObject {
  Q_OBJECT

public:
  AudioOutputCoordinator(PlaylistModel *playlistModel,
                         EditSessionController *editController,
                         ProgramPlayoutController *programController,
                         UserInfoCenter *userInfoCenter,
                         QObject *parent = nullptr);

  void applySavedSettings();
  void programOutputSelectionChanged(const QString &id);
  void pflOutputSelectionChanged(const QString &id);
  void requestShutdown();
  bool shutdownComplete() const;

signals:
  void savedAudioOutputsChanged();

private:
  struct PflRoute {
    QString pflId;
    QString programId;
    QString appliedProgramId;
    QString defaultId;
    bool operator==(const PflRoute &) const = default;
  };

  void cancelPflRequest();
  void requestPfl(bool enabled);
  void audioOutputsRefreshed(bool success);
  void showAudioOutputSelectionChanged(bool program, const QString &id);

  EditSessionController *editController_ = nullptr;
  ProgramPlayoutController *programController_ = nullptr;
  UserInfoCenter *userInfoCenter_ = nullptr;
  AudioOutputDeviceMonitor *audioOutputMonitor_ = nullptr;
  QSet<QString> missingAudioOutputs_;
  QString lastAudioOutputFailure_;
  std::optional<QString> pendingPflDevice_ = std::nullopt;
  std::optional<PflRoute> pendingPflConfirmation_ = std::nullopt;
  std::optional<PflRoute> approvedPflRoute_ = std::nullopt;
  std::optional<QString> pendingPflError_ = std::nullopt;
  std::optional<QString> pendingProgramAudioError_ = std::nullopt;
  QString pendingProgramAudioDevice_;
  bool shuttingDown_ = false;
};
