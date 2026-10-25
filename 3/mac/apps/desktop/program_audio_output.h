#pragma once

#include <QObject>
#include <QString>
#include <gst/gst.h>

#include <array>
#include <memory>

// Owns the PROGRAM mix/output graph. The player decides when the output is
// idle enough to change devices and owns the bus callback context.
class ProgramAudioOutput final
{
public:
    ProgramAudioOutput() = default;
    ~ProgramAudioOutput();
    ProgramAudioOutput(const ProgramAudioOutput &) = delete;
    ProgramAudioOutput &operator=(const ProgramAudioOutput &) = delete;

    bool initialize(GstBusSyncHandler callback, gpointer userData,
                    const QString &initialDeviceId,
                    const char *sinkFactory = nullptr);

    // Borrowed references remain valid until confirmed shutdown and destruction.
    GstElement *pipeline() const { return programOutput_; }
    GstBus *bus() const { return programBus_; }
    GstElement *itemLevel(int deckIndex) const;
    GstElement *masterLevel() const { return programAudioLevel_; }
    GstElement *bgmPostFaderLevel() const { return bgmPostFaderAudioLevel_; }

    void requestDevice(const QString &id);
    QString appliedDevice() const { return appliedProgramDevice_; }
    QString deviceStatusKey() const;
    void updateDevice(bool mayChange);
    bool deviceChanging() const;
    bool deviceOperationPending() const;
    bool ready() const { return programOutputReady_; }
    void reportFailure();

    void setDeckAudible(int deckIndex, bool audible);
    void setMasterVolume(double volume);
    void setMasterDelayFrames(int frames);
    void setDeckDelayFrames(int deck, int frames);
    void setMasterEq(double lowDb, double midDb, double highDb);
    void setDeckEq(int deck, double lowDb, double midDb, double highDb);
    void setMasterMuted(bool muted);
    void setFtbCoverage(double coverage);
    void setBgmGain(double gain);
    void setBgmManualFadeGain(double gain);
    void setBgmTransportGain(double gain);

    void detachCallbacks();
    // False means references must be retained, including the player's callback
    // context. Never race a device worker or release an unconfirmed pipeline.
    bool stop();

private:
    struct BusCallbacks;
    struct DeviceChange;
    static GstBusSyncReply handleBusSync(GstBus *bus, GstMessage *message,
                                        gpointer userData);
    void updateLatency();
    void applyProgramVolume();
    void drainBus();

    GstElement *programOutput_ = nullptr;
    GstBus *programBus_ = nullptr;
    std::array<GstElement *, 2> takeVolumes_ = {};
    std::array<GstElement *, 2> deckEffects_ = {};
    std::array<GstElement *, 2> programItemAudioLevels_ = {};
    GstElement *programAudioVolume_ = nullptr;
    GstElement *programAudioEffects_ = nullptr;
    std::array<double, 3> masterEqDb_{};
    GstElement *programAudioLevel_ = nullptr;
    GstElement *bgmControlVolume_ = nullptr;
    GstElement *bgmManualFadeVolume_ = nullptr;
    GstElement *bgmTransportVolume_ = nullptr;
    GstElement *bgmPostFaderAudioLevel_ = nullptr;
    QString requestedProgramDevice_;
    QString appliedProgramDevice_;
    std::shared_ptr<DeviceChange> programDeviceChange_;
    std::shared_ptr<BusCallbacks> busCallbacks_;
    QObject callbackContext_;
    bool latencyUpdatePending_ = false;
    bool programDeviceFailed_ = false;
    bool programOutputReady_ = false;
    bool shutdownRequested_ = false;
    bool shutdownConfirmed_ = false;
    double masterVolume_ = 1.0;
    double ftbCoverage_ = 0.0;
    bool masterMuted_ = false;
};
