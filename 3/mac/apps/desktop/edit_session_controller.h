#pragma once

#include "edit_player.h"
#include "playlist_item.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <optional>
#include <functional>
#include <memory>
#include <vector>

class EditPanel;
class PlaylistModel;
class PlaylistPanel;
class EditPflOutput;
class UvcSourceSessionManager;
class UvcAudioMappingStore;
class LiveEditAudio;

class EditSessionController final : public QObject {
  Q_OBJECT

public:
  EditSessionController(EditPanel *panel, PlaylistPanel *playlistPanel,
                        PlaylistModel *playlistModel,
                        UvcSourceSessionManager *uvcSessions,
                        UvcAudioMappingStore *uvcAudioMappings,
                        QObject *parent = nullptr);
  ~EditSessionController() override;

  VideoFrameCapture captureStillFrame() const { return player_.captureStillFrame(); }
  bool isAvailable() const;
  bool prepareForItemRemoval(quint64 itemId);
  bool deferItemRemoval(quint64 itemId, std::function<void()> continuation);
  void refreshWindowHandle();
  void shutdown();
  void requestShutdown();
  bool shutdownComplete() const;
  void setPflOutputDeviceId(const QString &id);
  void setPflEnabled(bool enabled);

signals:
  void pflRequested(bool enabled);
  void pflOutputError(const QString &message);

public slots:
  void toggleStartMode();
  void toggleFadeOut();
  bool resetEditor();
  void loadSelectedItem();
  void play();
  void pause();
  void stopPlayback();
  void jumpToStart();
  void jumpToIn();
  void jumpToOut();
  void jumpToEnd();
  void startScanBackward();
  void startScanForward();
  void stopScan();
  void setInPoint();
  void setOutPoint();
  void setInPointTime(qint64 positionMs);
  void setOutPointTime(qint64 positionMs);
  void setStartMode(PlaylistStartMode mode);
  void setEndMode(PlaylistEndMode mode);
  void setGotoTarget(quint64 itemId);
  void setRepeatOptions(int count, PlaylistRepeatEndMode endMode);
  void setFadeIn(bool enabled, qint64 durationMs);
  void setFadeOut(bool enabled, qint64 durationMs);
  void setEndCrossfade(bool enabled, qint64 durationMs);
  void setNominalVolume(int volumeTenthsDb);
  void setNominalDelayFrames(int frames);
  void setNominalEq(int lowTenthsDb, int midTenthsDb, int highTenthsDb);
  void seek(qint64 positionMs);
  void stepFrames(int frameCount);

private:
  void loadItem(quint64 itemId, const QString &path);
  void loadLiveItem(const PlaylistItemState &item);
  void updateEditorState();
  void setSelectedEditPoint(
      bool setInPoint,
      std::optional<qint64> requestedPositionMs = std::nullopt);
  void requestSeek(qint64 positionMs, const QString &name);
  void startScan(int direction);
  bool stopScanInternal(bool boundaryReached = false);
  void runTransport(const QString &name, bool succeeded);
  void resetAudioPresentation();
  void updateInputSignalIndicator(const EditPlayer::AudioLevel &inputLevel);
  void updateUi();

  QPointer<EditPanel> panel_;
  QPointer<PlaylistPanel> playlistPanel_;
  QPointer<PlaylistModel> playlistModel_;
  QPointer<UvcSourceSessionManager> uvcSessions_;
  QPointer<UvcAudioMappingStore> uvcAudioMappings_;
  EditPlayer player_;
  std::unique_ptr<LiveEditAudio> liveAudio_;
  QString liveDevicePath_;
  bool liveAudioNeedsStart_ = false;
  bool livePreviewReady_ = false;
  EditPflOutput *pflOutput_ = nullptr;
  QTimer timer_;
  std::optional<qint64> pendingSeekMs_ = std::nullopt;
  bool pendingSeekAccurate_ = false;
  std::optional<qint64> seekLockMs_ = std::nullopt;
  std::optional<qint64> reverseScanPositionMs_ = std::nullopt;
  QElapsedTimer reverseScanElapsed_;
  QElapsedTimer inputSignalClock_;
  qint64 inputSignalUntilMs_ = 0;
  qint64 inputClipUntilMs_ = 0;
  qint64 reverseScanBasePositionMs_ = 0;
  int seekLockTicks_ = 0;
  qint64 seekLockToleranceMs_ = 1000;
  int suppressScanEosTicks_ = 0;
  quint64 editItemId_ = 0;
  QString editFileName_;
  int scanDirection_ = 0;
  bool resumeAfterScan_ = false;
  bool shutdown_ = false;
  bool loadPending_ = false;
  quint64 removalPendingId_ = 0;
  std::vector<std::function<void()>> removalContinuations_;
};
