#pragma once

#if defined(_WIN32) || defined(__APPLE__)

#include "dual_deck_player.h"
#include "async_pipeline_stop.h"
#include "async_file_preparation.h"
#include <gst/gst.h>
#include <array>

class ProgramGpuCompositor;

// Owns the two video-only DSK producers and their playout state. The compositor
// is borrowed; the owner orders compositor detachment before aggregate shutdown.
class DskPlayback final
{
public:
    using DskStatus = DualDeckPlayer::DskStatus;
    using DskEvent = DualDeckPlayer::DskEvent;
    DskPlayback() = default;
    ~DskPlayback();
    DskPlayback(const DskPlayback &) = delete;
    DskPlayback &operator=(const DskPlayback &) = delete;

    bool initialize();
    GstElement *pipeline(int index) const;
    GstElement *appSink(int index) const;
    void attachCompositor(ProgramGpuCompositor *compositor);
    void detachCallbacks();
    // False retains producer references; never destroys the borrowed compositor.
    bool shutdown();
    bool retirementPending() const;

    bool cue(const QString &filePath, qint64 inPointMs = 0, qint64 outPointMs = -1);
    // Caller polls before its availability guard, matching the public facade.
    bool takeCue(bool playOnShow, bool crossfade, float videoOpacity);
    bool crossfadeActive() const;
    bool setCrossfadeProgress(float progress);
    bool setVideoOpacity(float opacity);
    bool clearCue();
    bool clear();
    bool play();
    bool pause();
    bool isPlaying() const;
    bool pauseAtEnd();
    bool restartFromIn(bool play);
    DskStatus status();
    DskStatus cueStatus();
    qint64 positionMs() const;
    qint64 durationMs() const;
    QString errorDetail() const;
    bool cueMediaOffline() const;
    bool mediaOffline() const;
    DskEvent takeEvent(QString &detail);
    void pollBus();

private:
    struct Dsk {
        std::shared_ptr<AsyncPipelineStop> stopJob;
        QString pendingPath;
        QDeadlineTimer preparationDeadline{QDeadlineTimer::Forever};
        std::shared_ptr<AsyncFilePreparation> preparation;
        bool mediaOffline = false;
        GstElement *playbin = nullptr;
        GstElement *appSink = nullptr;
        GstBus *bus = nullptr;
        DskStatus status = DskStatus::Empty;
        QString errorDetail;
        bool isStillImage = false;
        bool playing = false;
        bool rangeSeekIssued = false;
        qint64 inPointMs = 0;
        qint64 outPointMs = -1;
    };
    bool initializeDsk(int index);
    GstElement *makeDskVideoOutput(int index);
    bool enforceDskAudioIsolation(GstElement *playbin);
    void releaseDsk(Dsk &dsk);
    void setDskError(int dskIndex, const QString &detail, bool cueError);
    bool seekDskRange(int dskIndex, GstSeekFlags flags);
    qint64 queryDskTime(int dskIndex, GstQueryType type) const;
    bool applyDskLayers();
    bool resetDskDeck(int dskIndex);
    bool startPendingCue(int dskIndex);
    void finishRetirements();
    void refreshDskStatus();
    static bool stopPipeline(GstElement *pipeline, const char *description);
    static void drainBus(GstBus *bus);

    ProgramGpuCompositor *programGpuCompositor_ = nullptr;
    std::array<Dsk, 2> dsks_;
    bool initialized_ = false;
    bool shutdownRequested_ = false;
    bool shutdownConfirmed_ = false;
    int dskActiveDeck_ = -1;
    int dskCueDeck_ = -1;
    bool dskCrossfadeActive_ = false;
    int dskCrossfadeIncomingDeck_ = -1;
    int dskCrossfadeOutgoingDeck_ = -1;
    float dskCrossfadeProgress_ = 1.0F;
    float dskCrossfadeOutgoingStartOpacity_ = 0.0F;
    float dskCurrentOpacity_ = 0.0F;
    DskEvent pendingDskEvent_ = DskEvent::None;
    QString pendingDskEventDetail_;
};

#endif
