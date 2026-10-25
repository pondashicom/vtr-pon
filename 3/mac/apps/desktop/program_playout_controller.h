#pragma once

#include "dual_deck_player.h"
#include "language_text.h"
#include "playlist_item.h"
#include "playlist_item_playout_state_machine.h"
#include "program_audio_controller.h"
#include "timed_still_playout_clock.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantAnimation>

#include <atomic>
#include <memory>
#include <optional>

class PlaylistModel;
class PlaylistPanel;
class ProgramPanel;
class BgmPlayoutController;
class UserInfoCenter;
class UvcAudioMappingStore;
class UvcSourceSessionManager;
class QWidget;

class ProgramPlayoutController final : public QObject {
  Q_OBJECT

public:
  ProgramPlayoutController(QWidget *videoWindow, ProgramPanel *programPanel,
                           PlaylistPanel *playlistPanel,
                           PlaylistModel *playlistModel,
                           UserInfoCenter *userInfoCenter,
                           bool use4kProgramOutput,
                           UvcSourceSessionManager *uvcSourceSessions,
                           UvcAudioMappingStore *uvcAudioMappingStore,
                           QObject *parent = nullptr);
  ~ProgramPlayoutController() override;

  void setBackgroundColor(const QColor &color);
  VideoFrameCapture captureStillFrame() const { return player_.captureStillFrame(); }
  bool isAvailable() const;
  DualDeckPlayer::FullscreenOutputStats fullscreenOutputStats() const;
  bool canRemoveItem(quint64 itemId);
  bool prepareForItemRemoval(quint64 itemId);
  bool beginPowerPointConversion(
      std::atomic_bool *cancelRequested,
      QString *errorDetail = nullptr,
      vtrpon::LocalizedText *errorMessage = nullptr);
  void finishPowerPointConversion();
  void refreshWindowHandles();
  bool prepareNativeDocumentShutdown();
  void shutdown();
  void requestShutdown();
  bool shutdownComplete();
  void setAutoSelectNextItemAfterOnAir(bool enabled);
  void setAllowSameItemOnAirRetake(bool enabled);
  void setBgmControlsVisible(bool visible);
  bool bgmIsActive() const;
  int bgmPlaylistNumber() const;
  void requestProgramOutputDevice(const QString &id);
  QString appliedProgramOutputDevice() const;
  QString programOutputDeviceStatusKey() const;
  bool programAudioOutputReady() const;
  void reportPlaybackError(const QString &detail);
  bool validateMacroTarget(quint64 itemId, QString *error = nullptr) const;
  bool requestMacroCue(quint64 requestId, quint64 itemId,
                       QString *error = nullptr);
  bool requestMacroTake(quint64 requestId, quint64 itemId, bool requireReady,
                        bool crossfade, qint64 crossfadeDurationMs,
                        QString *error = nullptr);
  void cancelMacroRequests();
  bool executeMacroTransport(const QString &operation, QString *error = nullptr);
  bool requestMacroTransport(quint64 requestId, const QString &operation,
                             QString *error = nullptr);
  bool setMacroPlaybackRate(double rate, QString *error = nullptr);

public slots:
  void clearOnAir();
  void playOnAir();
  void pauseOnAir();
  void setPlaybackRate(double rate);
  void togglePlayback();
  void toggleFtb();
  void toggleDsk();
  void playDsk();
  void pauseDsk();
  void takeSelectedPlaylistItemOnAir();
  void nextPowerPointSlide();
  void previousPowerPointSlide();

signals:
  void programAudioOutputError(const QString &detail);
  void onAirChanged(bool onAir);
  void ftbChanged(bool active);
  void macroRequestFinished(quint64 requestId, bool success, const QString &detail);
  void macroIntervention(const QString &reason);
  void macroReservationFailed(const QString &detail);

private:
  friend class ProgramPlayoutTransitionTest;

  // Media preparation key. Start/End modes are resolved by item ID when the
  // corresponding transition fires, so mode-only edits do not rebuild CUE.
  struct CueRequest {
    quint64 itemId = 0;
    QString path;
    QString displayName;
    PlaylistSourceType sourceType = PlaylistSourceType::File;
    QString audioDeviceId;
    qint64 inPointMs = 0;
    std::optional<qint64> outPointMs = std::nullopt;
    double playbackRate = 1.0;

    bool operator==(const CueRequest &) const = default;
  };

  enum class PlayoutTarget { Program, Dsk };

  struct NextTransitionState {
    std::optional<quint64> itemId;
    std::optional<PlaylistEndMode> mode;
    std::optional<CueRequest> request;
    bool preparationAttempted = false;
    bool due = false;
    bool paused = false;
  };

  struct MacroRequest {
    quint64 requestId = 0;
    bool take = false;
    bool crossfade = false;
    qint64 crossfadeDurationMs = kDefaultItemFadeDurationMs;
  };
  struct MacroTransportRequest {
    quint64 requestId = 0;
    quint64 itemId = 0;
  };

  void updateMacroRequest();
  void updateMacroTransportRequest();
  void finishMacroTransportRequest(bool success, const QString &detail = {});
  void finishMacroRequest(bool success, const QString &detail = {});
  void interruptMacroReservation(const QString &reason);
  void failMacroReservation(const QString &detail);

  CueRequest cueRequestFor(const PlaylistItemState &item) const;
  bool takeCueOnAir(const PlaylistItemState &item);
  bool takeCueOnAir(const PlaylistItemState &item, bool crossfade,
                    qint64 crossfadeDurationMs);
  void startCrossfade(qint64 durationMs);
  void startItemFadeIn(const PlaylistItemSettings &settings);
  void maybeStartItemFadeOut(qint64 positionMs, qint64 endPositionMs);
  void finishItemFadeForEnd(bool fadeOutEnabled);
  void revealItemAfterRepeat();
  void applyItemFadeProgress();
  void applyOnAirHighlightProgress(qreal itemProgress);
  void finishOnAirHighlightCrossfade();
  void resetItemFadeState();
  void scheduleSelectedItemCue();
  void scheduleCueRequest(std::optional<CueRequest> requestedCue, bool retryFailed = false);
  bool cueReplacementMustWait();
  void applyPendingCue();
  void beginPendingCue();
  void updateCueUi();
  void maybePrepareNextItem(qint64 positionMs, qint64 endPositionMs);
  bool maybePrepareNextItem(PlayoutTarget target, qint64 positionMs,
                            qint64 endPositionMs);
  std::optional<PlaylistItemState> preparedNextItem(PlayoutTarget target) const;
  bool takePreparedNextItem();
  bool usesAlternatingRepeat() const;
  bool ownsAutomaticTransitionCue(const CueRequest &request) const;
  qint64 repeatCrossfadeDurationMs(qint64 endPositionMs) const;
  bool takePreparedRepeatItem(const PlaylistItemState &item);
  void cancelNextTransition(bool restoreSelectedCue,
                            PlayoutTarget target = PlayoutTarget::Program);
  void resetNextTransitionState();
  void failNextTransition(const QString &detail);
  void handleOnAirCompletion(qint64 playbackEnd);
  void updatePlaybackUi();
  void refreshOnAirEndModePresentation();
  CueRequest dskRequestFor(const PlaylistItemState &item) const;
  bool cueSelectedPlaylistItemToDsk();
  bool cuePlaylistItemToDsk(const PlaylistItemState &item, bool pendingOn);
  bool activatePendingDsk();
  bool takePreparedNextDskItem();
  void maybePrepareNextDskItem(qint64 positionMs, qint64 endPositionMs);
  void resetNextDskTransitionState(bool clearCue);
  void handleDskCompletion();
  void startDskCrossfade(qint64 durationMs);
  void startDskFadeIn(const PlaylistItemSettings &settings);
  void maybeStartDskFadeOut(qint64 positionMs, qint64 endPositionMs);
  void applyDskFadeProgress();
  void resetDskFadeState();
  void setDskAnimationsPaused(bool paused);
  void updateDskTransportControls();
  void failDsk(const QString &detail);
  void beginDskOff();
  void finishDskOff();
  void setPendingDskOn(bool pending);
  QString takeDskFailureDetail(const QString &fallback);
  void updateDskUi();
  void updateDskSelectionAvailability();
  enum class PowerPointNavigationCommand { Next, Previous };
  void attemptPowerPointNavigation(PowerPointNavigationCommand command);
  void clearPendingPowerPointNavigation();
  void updatePowerPointNavigationUi();
  void applyFtbCoverage(qreal coverage);
  void resetFtb();
  void showWarning(const QString &text);
  void setFault(const QString &detail);
  void clearFault();
  void synchronizeProgramPosition();
  void updateTotalRemainingTime(qint64 currentItemRemainingMs);
  qint64 playbackEnd(qint64 duration) const;
  qint64 programPositionMs() const;
  qint64 programDurationMs() const;
  bool programIsPlaying() const;
  bool onAirIsLive() const;
  void selectNextItemAfterOnAir(quint64 itemId);

  QPointer<QWidget> videoWindow_;
  QPointer<ProgramPanel> programPanel_;
  QPointer<PlaylistPanel> playlistPanel_;
  QPointer<PlaylistModel> playlistModel_;
  UserInfoCenter *userInfoCenter_ = nullptr;
  UvcAudioMappingStore *uvcAudioMappingStore_ = nullptr;
  UvcSourceSessionManager *uvcSourceSessions_ = nullptr;
  PlaylistItemPlayoutStateMachine onAirStateMachine_;
  PlaylistItemPlayoutStateMachine dskStateMachine_;
  DualDeckPlayer player_;
  ProgramAudioController audioController_;
  std::unique_ptr<BgmPlayoutController> bgmController_;
  QTimer busTimer_;
  QTimer ftbBlinkTimer_;
  QTimer powerPointNavigationRetryTimer_;
  QElapsedTimer onAirTimedStillElapsedTimer_;
  QElapsedTimer dskTimedStillElapsedTimer_;
  QVariantAnimation ftbAnimation_;
  QVariantAnimation crossfadeAnimation_;
  QVariantAnimation itemFadeInAnimation_;
  QVariantAnimation itemFadeOutAnimation_;
  QVariantAnimation dskCrossfadeAnimation_;
  QVariantAnimation dskFadeInAnimation_;
  QVariantAnimation dskFadeOutAnimation_;
  std::optional<qint64> pendingSeekMs_ = std::nullopt;
  std::optional<qint64> seekLockMs_ = std::nullopt;
  int seekLockTicks_ = 0;
  std::optional<CueRequest> pendingCue_ = std::nullopt;
  std::optional<CueRequest> cue_ = std::nullopt;
  // Ownership survives a completed CUE so a later timed TAKE cannot silently
  // use a spare deck replaced by selection or automatic END MODE preparation.
  std::optional<CueRequest> macroCueOwner_;
  std::optional<MacroRequest> macroRequest_;
  std::optional<MacroTransportRequest> macroTransportRequest_;
  bool cueReplacementPending_ = false;
  NextTransitionState nextTransition_;
  NextTransitionState nextDskTransition_;
  std::optional<CueRequest> dskCue_ = std::nullopt;
  std::optional<quint64> pendingTakeItemId_ = std::nullopt;
  std::optional<PowerPointNavigationCommand>
      pendingPowerPointNavigation_ = std::nullopt;
  TimedStillPlayoutClock onAirTimedStillClock_;
  TimedStillPlayoutClock dskTimedStillClock_;
  int powerPointNavigationRetryCount_ = 0;
  DualDeckPlayer::CueStatus lastCueStatus_ = DualDeckPlayer::CueStatus::Empty;
  DualDeckPlayer::DskStatus lastDskStatus_ =
      DualDeckPlayer::DskStatus::Empty;
  QString dskFileName_;
  bool pendingDskOn_ = false;
  bool pendingDskOff_ = false;
  bool dskManuallyPaused_ = false;
  bool dskEndOfStreamPending_ = false;
  qreal ftbCoverage_ = 0.0;
  qreal itemFadeInProgress_ = 1.0;
  qreal itemFadeOutProgress_ = 1.0;
  qreal crossfadeProgress_ = 1.0;
  qreal crossfadeOutgoingStartOpacity_ = 0.0;
  qreal dskFadeInProgress_ = 1.0;
  qreal dskFadeOutProgress_ = 1.0;
  quint64 crossfadeOutgoingItemId_ = 0;
  bool itemFadeOutStarted_ = false;
  bool dskFadeOutStarted_ = false;
  bool ftbTargetCovered_ = false;
  bool ftbBlinkRed_ = false;
  bool autoSelectNextItemAfterOnAir_ = false;
  bool internalAutoSelection_ = false;
  bool allowSameItemOnAirRetake_ = false;
  bool powerPointConversionInProgress_ = false;
  bool powerPointNextTransitionDeferred_ = false;
  std::atomic_bool *powerPointConversionCancelRequested_ = nullptr;
  bool shutdown_ = false;
  bool onAirTimedStillRestorePending_ = false;
  bool onAirTimedStillCompletionDue_ = false;
  bool dskTimedStillCompletionDue_ = false;
  double playbackRate_ = 1.0;
};
