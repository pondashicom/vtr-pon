#pragma once

#include "dual_deck_player.h"

#include <gst/gst.h>

// Owns BGM material playback up to its timestamped PCM output. PROGRAM mixing,
// device readiness, and meter presentation remain with their respective owners.
class BgmPlayback final
{
public:
    using Status = DualDeckPlayer::BgmStatus;
    using Event = DualDeckPlayer::BgmEvent;
    enum class StopResult { Failed, AlreadyEmpty, Stopped };

    BgmPlayback() = default;
    ~BgmPlayback();
    BgmPlayback(const BgmPlayback &) = delete;
    BgmPlayback &operator=(const BgmPlayback &) = delete;

    bool initialize(GstBusSyncHandler callback, gpointer userData);
    // Borrowed references are valid while this owner retains its resources.
    GstElement *pipeline() const { return bgmPlaybin_; }
    GstElement *inputLevel() const { return bgmInputAudioLevel_; }
    void detachCallbacks();
    // False requires the aggregate owner to retain callback storage and all
    // unconfirmed resources. Destruction never releases a running pipeline.
    bool shutdown();

    bool load(const QString &filePath, qint64 inPointMs, qint64 outPointMs);
    bool play();
    bool pause();
    StopResult stop();
    bool clear();
    bool seek(qint64 positionMs);
    qint64 positionMs() const;
    qint64 durationMs() const;
    Status status();
    Status cachedStatus() const { return bgmStatus_; }
    void refreshStatus();
    void pollBus();
    QString errorDetail() const { return bgmErrorDetail_; }
    Event takeEvent(QString &detail);
    void setItemVolume(double volume);
    void setItemDelayFrames(int frames);
    void setItemEq(double lowDb, double midDb, double highDb);

private:
    void drainBus();

    GstElement *bgmPlaybin_ = nullptr;
    GstElement *bgmInputAudioLevel_ = nullptr;
    GstElement *bgmItemAudioVolume_ = nullptr;
    GstBus *bgmBus_ = nullptr;
    Status bgmStatus_ = Status::Empty;
    Event pendingBgmEvent_ = Event::None;
    QString bgmErrorDetail_;
    QString pendingBgmEventDetail_;
    qint64 bgmInPointMs_ = 0;
    qint64 bgmOutPointMs_ = -1;
    bool bgmRangeSeekIssued_ = false;
    bool bgmHasAudio_ = false;
    bool shutdownConfirmed_ = false;
};
