#pragma once

#include "playlist_item_playout_state_machine.h"
#include "program_panel.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantAnimation>

class DualDeckPlayer;
class PlaylistModel;
class PlaylistPanel;
class ProgramAudioController;
class UserInfoCenter;

class BgmPlayoutController final : public QObject {
  Q_OBJECT

public:
  BgmPlayoutController(DualDeckPlayer *player, ProgramPanel *programPanel,
                       PlaylistPanel *playlistPanel,
                       PlaylistModel *playlistModel,
                       ProgramAudioController *audioController,
                       UserInfoCenter *userInfoCenter,
                       QObject *parent = nullptr);

  bool isActive() const;
  int activePlaylistNumber() const;
  void setControlsVisibleRequested(bool visible);
  void update();
  void shutdown();
  void requestShutdown();

public slots:
  void stop();
  void play();
  void pause();
  void fadeIn(qint64 durationMs);
  void fadeOut(qint64 durationMs);

private:
  friend class ProgramPlayoutTransitionTest;
  enum class PendingStart { None, Play, Pause };
  enum class PendingTransportAction { None, Pause, Stop };

  bool itemIsEligible(const PlaylistItemState &item) const;
  quint64 startingItemId() const;
  quint64 nextEligibleItemId(quint64 currentItemId) const;
  bool loadItem(quint64 itemId, PendingStart pendingStart);
  bool startLoadedItem(PendingStart pendingStart);
  bool tryPlayBgm(bool &outputBlocked);
  void handleLoadFailure(const QString &detail);
  void handleCompletion();
  void applyItemVolume();
  void startItemFadeIn(const PlaylistItemSettings &settings);
  void maybeStartItemFadeOut(qint64 positionMs, qint64 endPositionMs);
  void resetItemFade();
  bool canManualFade(bool fadeIn) const;
  void startManualFade(qreal target, qint64 durationMs);
  void cancelManualFade();
  void startTransportFade(qreal target, PendingTransportAction action);
  void cancelTransportFade();
  void completeStop();
  void setTransportGain(qreal gain);
  void foldTransportGainIntoManualFade();
  void setPresentation(ProgramPanel::BgmPresentationState state,
                       const QString &detail = {});
  void updateSignalIndicator();
  void showWarning(const QString &text, const QString &detail = {});

  DualDeckPlayer *player_ = nullptr;
  QPointer<ProgramPanel> programPanel_;
  QPointer<PlaylistPanel> playlistPanel_;
  QPointer<PlaylistModel> playlistModel_;
  ProgramAudioController *audioController_ = nullptr;
  UserInfoCenter *userInfoCenter_ = nullptr;
  PlaylistItemPlayoutStateMachine stateMachine_;
  QVariantAnimation manualFadeAnimation_;
  QVariantAnimation itemFadeAnimation_;
  QVariantAnimation transportFadeAnimation_;
  QElapsedTimer signalClock_;
  QSet<quint64> failedItemIds_;
  PendingStart pendingStart_ = PendingStart::None;
  int activePlaylistNumber_ = 0;
  qreal manualFadeGain_ = 1.0;
  qreal itemFadeGain_ = 1.0;
  qreal transportGain_ = 1.0;
  qint64 signalUntilMs_ = 0;
  qint64 clipUntilMs_ = 0;
  PendingTransportAction pendingTransportAction_ =
      PendingTransportAction::None;
  bool itemFadeOutStarted_ = false;
  // START PAUSE defers the item fade until the first successful PLAY.
  bool firstPlayPending_ = false;
  // Keep the muted item owned until STOP can reset the pipeline successfully.
  bool stopFailed_ = false;
  bool controlsVisibleRequested_ = false;
  bool hideWhenStopped_ = false;
  bool shutdown_ = false;
};
