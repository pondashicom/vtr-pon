#pragma once

#include "dual_deck_player.h"
#include "async_pipeline_stop.h"
#include <QDeadlineTimer>
#include <vector>
#include <optional>

#include <gst/gst.h>

class ProgramGpuCompositor;
class UvcSourceSessionManager;
namespace vtrpon { class NetworkVideoReceiver; }

// Owns the media resources of one physical deck. PROGRAM/CUE selection,
// compositing layers and output gains remain the player's responsibility.
class DeckSourceSession final
{
public:
    enum class Type { None, File, Uvc, PowerPoint, Pdf };
    using PreparationState = DualDeckPlayer::CueStatus;
    struct PreparationResult {
        PreparationState state = PreparationState::Empty;
        QString errorDetail;
        // Only a failed File PAUSED transition asks the coordinator to prefer
        // a queued File bus error over the state-operation fallback detail.
        bool fileStateChangeFailed = false;
    };
    struct ReleaseResult {
        bool succeeded = false;
        bool detached = false;
        bool ownershipReleased = false;
        // Source stop confirmation is independent of detachment/ownership.
        // Releasing a shared UVC binding does not confirm its producer stopped.
        bool stopConfirmed = false;
    };

    DeckSourceSession() = default;
    ~DeckSourceSession();
    DeckSourceSession(const DeckSourceSession &) = delete;
    DeckSourceSession &operator=(const DeckSourceSession &) = delete;

    bool initialize(int deckIndex, int width, int height,
                    GstBusSyncHandler callback = nullptr,
                    gpointer userData = nullptr,
                    guintptr videoWindow = 0, guintptr monitorWindow = 0);
    void attachSourceManagers(UvcSourceSessionManager *uvc,
                              PowerPointSourceSessionManager *powerPoint,
                              PdfSourceSessionManager *pdf,
                              ProgramGpuCompositor *compositor);
    PreparationResult prepare(const DualDeckPlayer::DeckSource &source,
                              double itemVolume = 1.0,
                              GstClock *audioClock = nullptr);
    PreparationResult refreshPreparation();
    ReleaseResult clearForReplacement();
    // Retire a loading/failed File graph without ever reusing its playbin.
    bool replaceFilePipeline();
    ReleaseResult standbyAfterTake();
    ReleaseResult clearAfterOffAir();

    bool setFileState(GstState state);
    bool seek(qint64 positionMs);
    bool setRate(double rate);
    qint64 positionMs() const;
    qint64 durationMs() const;
    bool sourceIsPlaying() const;
    bool uvcProgramPaused() const { return uvcProgramPaused_; }
    void setLiveFrameUpdates(bool enabled);
    void setItemVolume(double volume);
    bool takeSourceError(QString &detail);
    PowerPointNavigationState powerPointNavigationState();
    PowerPointCommandResponse nextPowerPointSlide();
    PowerPointCommandResponse previousPowerPointSlide();
    PdfNavigationState pdfNavigationState();
    PdfCommandResponse nextPdfPage();
    PdfCommandResponse previousPdfPage();

    Type sourceType() const { return sourceType_; }
    bool hasVideo() const { return hasVideo_; }
    bool hasAudio() const { return hasAudio_; }
    qint64 inPointMs() const { return inPointMs_; }
    qint64 outPointMs() const { return outPointMs_; }
    // Borrowed references: valid while this deck retains its resources.
    GstElement *pipeline() const { return playbin_; }
    GstBus *bus() const { return bus_; }
    GstElement *inputAudioLevel() const;
    GstElement *audioPipeline() const;
    GstBus *mappedAudioBus() const;
    GstElement *appSink() const;
    GstElement *videoSink() const;
    GstElement *monitorSink() const;
    guintptr videoWindowHandle() const { return videoWindowHandle_; }
    guintptr monitorWindowHandle() const { return monitorWindowHandle_; }
    void setNativeWindowHandles(guintptr videoWindow, guintptr monitorWindow);
    void detachCallbacks();
    bool stopPipelinesForShutdown();
    bool retirementPending() const;
    // Call only after aggregate producer/compositor shutdown is confirmed.
    void releaseResourcesAfterConfirmedShutdown();

private:
    friend class DeckSourceSessionTest;
    struct BusCallbacks;
    static GstBusSyncReply handleFileBusSync(GstBus *, GstMessage *, gpointer);
    bool prepareFile(const QString &path, qint64 inPointMs, qint64 outPointMs,
                     double playbackRate);
    bool prepareUvc(const QString &path, const QString &audioDeviceId,
                    double itemVolume, GstClock *audioClock);
    bool preparePowerPoint(const QString &path);
    bool preparePdf(const QString &path);
    bool beginSource(Type type);
    void refreshPreparationState();
    bool releasePdfAfterDetach(const QString &operation, bool nonBlocking = false);
    bool releaseUvcAudio();
    bool startUvcAudio(const QString &audioDeviceId, double itemVolume,
                       GstClock *audioClock, QString *errorDetail);
    GstElement *makeVideoOutput(const QString &deckName);
    static void applyWindowHandle(GstObject *object, guintptr windowHandle);
    static bool stopPipeline(GstElement *pipeline, const char *description);
    static void drainBus(GstBus *bus);
    bool fileStopConfirmed() const;

    int index_ = -1;
    int outputWidth_ = 1920;
    int outputHeight_ = 1080;
    GstBusSyncHandler callback_ = nullptr;
    gpointer callbackData_ = nullptr;
    GstElement *playbin_ = nullptr;
    GstElement *inputAudioLevel_ = nullptr;
    GstElement *itemAudioVolume_ = nullptr;
    GstBus *bus_ = nullptr;
    std::shared_ptr<BusCallbacks> fileBusCallbacks_;
    std::vector<std::shared_ptr<AsyncPipelineStop>> retiredFiles_;
    QDeadlineTimer preparationDeadline_{QDeadlineTimer::Forever};
    guintptr videoWindowHandle_ = 0;
    guintptr monitorWindowHandle_ = 0;
    Type sourceType_ = Type::None;
    bool hasVideo_ = false;
    bool hasAudio_ = false;
    qint64 inPointMs_ = 0;
    qint64 outPointMs_ = -1;
    double playbackRate_ = 1.0;
    PreparationState preparationState_ = PreparationState::Empty;
    QString preparationError_;
    bool rangeSeekIssued_ = false;
    bool fileStateChangeFailed_ = false;
    bool uvcProgramPaused_ = false;
    bool pipelinesStopped_ = false;
#ifdef _WIN32
    GstElement *appSink_ = nullptr;
    QString uvcDevicePath_;
    QString uvcAudioDeviceId_;
    std::shared_ptr<AsyncPipelineStop> uvcAudioRetirement_;
    std::optional<DualDeckPlayer::DeckSource> pendingSource_;
    std::shared_ptr<GstClock> pendingAudioClock_;
    double pendingItemVolume_ = 1.0;
    GstElement *uvcAudioPipeline_ = nullptr;
    GstElement *uvcAudioSource_ = nullptr;
    std::shared_ptr<vtrpon::NetworkVideoReceiver> networkAudioReceiver_;
    GstElement *uvcInputAudioLevel_ = nullptr;
    GstElement *uvcItemAudioVolume_ = nullptr;
    GstBus *uvcAudioBus_ = nullptr;
    quint64 uvcErrorGeneration_ = 0;
    QString powerPointFilePath_;
    quint64 powerPointErrorGeneration_ = 0;
    bool nativeInputAttached_ = false;
    QString pdfFilePath_;
    quint64 pdfErrorGeneration_ = 0;
    UvcSourceSessionManager *uvcSourceSessions_ = nullptr;
    PowerPointSourceSessionManager *powerPointSources_ = nullptr;
    PdfSourceSessionManager *pdfSources_ = nullptr;
    ProgramGpuCompositor *programGpuCompositor_ = nullptr;
#else
    struct MonitorCadenceLimiter {
        MonitorCadenceLimiter();
        void reset(bool resetSegment = false);
        GstSegment segment;
        GstClockTime lastKeptRunningTime = GST_CLOCK_TIME_NONE;
    };
    static GstPadProbeReturn limitMonitorCadence(GstPad *, GstPadProbeInfo *, gpointer);
    GstElement *videoSink_ = nullptr;
    GstElement *monitorSink_ = nullptr;
    GstPad *monitorCadencePad_ = nullptr;
    gulong monitorCadenceProbeId_ = 0;
    MonitorCadenceLimiter monitorCadence_;
#endif
};
