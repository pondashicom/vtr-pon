#pragma once

#include "dual_deck_player.h"

#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

// Collects level messages from streaming threads and advances their display
// using each pipeline's running time. The owner must stop/detach callbacks
// before destruction, retaining this object if shutdown cannot be confirmed.
class ProgramAudioMeter final
{
public:
    using AudioLevel = DualDeckPlayer::AudioLevel;
    using AudioLevels = DualDeckPlayer::AudioLevels;

    ProgramAudioMeter() = default;
    ~ProgramAudioMeter();
    ProgramAudioMeter(const ProgramAudioMeter &) = delete;
    ProgramAudioMeter &operator=(const ProgramAudioMeter &) = delete;

    // Set once before streaming callbacks begin. All source references are
    // retained so source identity cannot outlive the corresponding elements.
    void setOutputSources(GstElement *master, GstElement *bgmInput,
                          GstElement *bgmPostFader = nullptr);
    void setDeckRoute(GstElement *input, GstElement *item,
                      bool collectPreroll = false);
    void reset(bool collectPreroll = false);
    void setDeckCollecting(bool active);
    bool active() const;
    void setActive(bool active);
    void setBgmActive(bool active);
    void resetBgmSignal();
    AudioLevel bgmSignalLevel();
    // Pipelines are borrowed only for this call; source elements above are
    // retained independently from these clock/position lookups.
    AudioLevels levels(GstElement *inputPipeline, GstElement *programPipeline);
    // Does not consume or unref the message.
    void update(GstMessage *message);

private:
    struct TimedAudioLevel {
        GstClockTime runningTime = GST_CLOCK_TIME_NONE;
        AudioLevel level;
    };
    struct AudioLevelRoute {
        GstElement *inputAudioLevel = nullptr;
        GstElement *itemAudioLevel = nullptr;
    };

    static void releaseAudioLevelRoute(AudioLevelRoute &route);
    static GstClockTime currentRunningTime(GstElement *pipeline);
    void resetLocked(bool collectPreroll);

    // Output sources are immutable while callbacks run; the deck route changes
    // under the same mutex that protects message routing and display queues.
    GstElement *masterAudioLevel_ = nullptr;
    GstElement *bgmInputAudioLevel_ = nullptr;
    GstElement *bgmPostFaderAudioLevel_ = nullptr;
    std::atomic_bool audioMeterActive_ = {false};
    std::atomic_bool deckAudioMeterCollecting_ = {false};
    std::atomic_bool bgmAudioMeterActive_ = {false};
    std::atomic_bool audioMeterCollecting_ = {false};
    std::atomic<std::uint64_t> audioLevelGeneration_ = {0};
    std::mutex audioLevelMutex_;
    AudioLevelRoute audioLevelRoute_;
    std::deque<TimedAudioLevel> pendingInputAudioLevels_;
    std::deque<TimedAudioLevel> pendingItemAudioLevels_;
    std::deque<TimedAudioLevel> pendingMasterAudioLevels_;
    std::deque<TimedAudioLevel> pendingBgmAudioLevels_;
    AudioLevel displayedInputAudioLevel_;
    AudioLevel displayedItemAudioLevel_;
    AudioLevel displayedMasterAudioLevel_;
    AudioLevel displayedBgmInputAudioLevel_;
    AudioLevel displayedBgmPostFaderAudioLevel_;
};
