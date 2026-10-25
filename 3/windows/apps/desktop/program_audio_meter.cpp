#include "program_audio_meter.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMeterFloorDb = -60.0;

bool readLevelChannels(const GValue *values, double &left, double &right)
{
    if (!values) {
        return false;
    }

    guint channelCount = 0;
    const GValue *leftValue = nullptr;
    const GValue *rightValue = nullptr;
    const GType valueType = G_VALUE_TYPE(values);
    static const GType glibArrayType = g_type_from_name("GValueArray");
    static const GType gstArrayType = g_type_from_name("GstValueArray");
    static const GType listType = g_type_from_name("GstValueList");
    if (glibArrayType != G_TYPE_INVALID
        && g_type_is_a(valueType, glibArrayType)) {
        const auto *array = static_cast<const GValueArray *>(
            g_value_get_boxed(values));
        if (array && array->n_values > 0) {
            channelCount = array->n_values;
            leftValue = &array->values[0];
            rightValue = channelCount > 1 ? &array->values[1] : leftValue;
        }
    } else if (gstArrayType != G_TYPE_INVALID
               && g_type_is_a(valueType, gstArrayType)) {
        channelCount = gst_value_array_get_size(values);
        if (channelCount > 0) {
            leftValue = gst_value_array_get_value(values, 0);
            rightValue = channelCount > 1
                ? gst_value_array_get_value(values, 1) : leftValue;
        }
    } else if (listType != G_TYPE_INVALID
               && g_type_is_a(valueType, listType)) {
        channelCount = gst_value_list_get_size(values);
        if (channelCount > 0) {
            leftValue = gst_value_list_get_value(values, 0);
            rightValue = channelCount > 1
                ? gst_value_list_get_value(values, 1) : leftValue;
        }
    }
    if (!leftValue || !rightValue
        || !G_VALUE_HOLDS_DOUBLE(leftValue)
        || !G_VALUE_HOLDS_DOUBLE(rightValue)) {
        return false;
    }
    left = g_value_get_double(leftValue);
    right = g_value_get_double(rightValue);
    return true;
}

double displayDb(double value)
{
    return std::isfinite(value) ? std::clamp(value, kMeterFloorDb, 0.0)
                                : kMeterFloorDb;
}

} // namespace

ProgramAudioMeter::~ProgramAudioMeter()
{
    if (bgmPostFaderAudioLevel_) gst_object_unref(bgmPostFaderAudioLevel_);
    releaseAudioLevelRoute(audioLevelRoute_);
    if (masterAudioLevel_) {
        gst_object_unref(masterAudioLevel_);
    }
    if (bgmInputAudioLevel_) {
        gst_object_unref(bgmInputAudioLevel_);
    }
}

void ProgramAudioMeter::setOutputSources(GstElement *master, GstElement *bgmInput,
                                        GstElement *bgmPostFader)
{
    if (bgmPostFader) gst_object_ref(bgmPostFader);
    if (bgmPostFaderAudioLevel_) gst_object_unref(bgmPostFaderAudioLevel_);
    bgmPostFaderAudioLevel_ = bgmPostFader;
    if (master) {
        gst_object_ref(master);
    }
    if (bgmInput) {
        gst_object_ref(bgmInput);
    }
    if (masterAudioLevel_) {
        gst_object_unref(masterAudioLevel_);
    }
    if (bgmInputAudioLevel_) {
        gst_object_unref(bgmInputAudioLevel_);
    }
    masterAudioLevel_ = master;
    bgmInputAudioLevel_ = bgmInput;
}

void ProgramAudioMeter::releaseAudioLevelRoute(AudioLevelRoute &route)
{
    if (route.inputAudioLevel) {
        gst_object_unref(route.inputAudioLevel);
        route.inputAudioLevel = nullptr;
    }
    if (route.itemAudioLevel) {
        gst_object_unref(route.itemAudioLevel);
        route.itemAudioLevel = nullptr;
    }
}

void ProgramAudioMeter::setDeckRoute(GstElement *input, GstElement *item,
                                    bool collectPreroll)
{
    audioMeterActive_.store(false, std::memory_order_relaxed);
    AudioLevelRoute replacement{input, item};
    if (replacement.inputAudioLevel) {
        gst_object_ref(replacement.inputAudioLevel);
    }
    if (replacement.itemAudioLevel) {
        gst_object_ref(replacement.itemAudioLevel);
    }

    AudioLevelRoute previous;
    {
        std::lock_guard<std::mutex> lock(audioLevelMutex_);
        previous = audioLevelRoute_;
        audioLevelRoute_ = replacement;
        resetLocked(collectPreroll);
    }
    releaseAudioLevelRoute(previous);
}

void ProgramAudioMeter::resetLocked(bool collectPreroll)
{
    audioLevelGeneration_.fetch_add(1, std::memory_order_relaxed);
    deckAudioMeterCollecting_.store(collectPreroll,
                                    std::memory_order_relaxed);
    audioMeterCollecting_.store(
        collectPreroll ||
            bgmAudioMeterActive_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    pendingInputAudioLevels_.clear();
    pendingItemAudioLevels_.clear();
    pendingMasterAudioLevels_.clear();
    displayedInputAudioLevel_ = {};
    displayedItemAudioLevel_ = {};
    displayedMasterAudioLevel_ = {};
}

void ProgramAudioMeter::reset(bool collectPreroll)
{
    audioMeterActive_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(audioLevelMutex_);
    resetLocked(collectPreroll);
}

void ProgramAudioMeter::setDeckCollecting(bool active)
{
    deckAudioMeterCollecting_.store(active, std::memory_order_relaxed);
    audioMeterCollecting_.store(
        active || bgmAudioMeterActive_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    audioMeterActive_.store(active, std::memory_order_relaxed);
}

bool ProgramAudioMeter::active() const
{
    return audioMeterActive_.load(std::memory_order_relaxed);
}

void ProgramAudioMeter::setActive(bool active)
{
    audioMeterActive_.store(active, std::memory_order_relaxed);
}

void ProgramAudioMeter::setBgmActive(bool active)
{
    const bool previous = bgmAudioMeterActive_.exchange(
        active, std::memory_order_relaxed);
    if (previous == active) {
        return;
    }
    std::lock_guard<std::mutex> lock(audioLevelMutex_);
    audioLevelGeneration_.fetch_add(1, std::memory_order_relaxed);
    pendingMasterAudioLevels_.clear();
    displayedMasterAudioLevel_ = {};
    pendingBgmAudioLevels_.clear();
    displayedBgmPostFaderAudioLevel_ = {};
    audioMeterCollecting_.store(
        active || deckAudioMeterCollecting_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

void ProgramAudioMeter::resetBgmSignal()
{
    std::lock_guard<std::mutex> lock(audioLevelMutex_);
    displayedBgmInputAudioLevel_ = {};
    pendingBgmAudioLevels_.clear();
    displayedBgmPostFaderAudioLevel_ = {};
}

ProgramAudioMeter::AudioLevel ProgramAudioMeter::bgmSignalLevel()
{
    std::lock_guard<std::mutex> lock(audioLevelMutex_);
    return displayedBgmInputAudioLevel_;
}

ProgramAudioMeter::AudioLevels ProgramAudioMeter::levels(
    GstElement *inputPipeline, GstElement *programPipeline)
{
    const GstClockTime deckNow = currentRunningTime(inputPipeline);
    const GstClockTime programNow = currentRunningTime(programPipeline);
    std::lock_guard<std::mutex> lock(audioLevelMutex_);
    const auto advanceTo = [](GstClockTime now, auto &pending,
                              auto &displayed) {
        if (!GST_CLOCK_TIME_IS_VALID(now)) {
            return;
        }
        while (!pending.empty() && pending.front().runningTime <= now) {
            displayed = pending.front().level;
            pending.pop_front();
        }
    };
    advanceTo(deckNow, pendingInputAudioLevels_, displayedInputAudioLevel_);
    advanceTo(programNow, pendingItemAudioLevels_, displayedItemAudioLevel_);
    advanceTo(programNow, pendingMasterAudioLevels_, displayedMasterAudioLevel_);
    advanceTo(programNow, pendingBgmAudioLevels_, displayedBgmPostFaderAudioLevel_);
    return {displayedInputAudioLevel_, displayedItemAudioLevel_,
            displayedMasterAudioLevel_, displayedBgmPostFaderAudioLevel_};
}

void ProgramAudioMeter::update(GstMessage *message)
{
    const std::uint64_t generation =
        audioLevelGeneration_.load(std::memory_order_relaxed);
    const GstStructure *structure = gst_message_get_structure(message);
    if (!structure || !gst_structure_has_name(structure, "level")) {
        return;
    }
    GstObject *source = GST_MESSAGE_SRC(message);
    const bool bgmSource = source == GST_OBJECT(bgmInputAudioLevel_);
    if (!bgmSource && !audioMeterCollecting_.load(std::memory_order_relaxed)) {
        return;
    }

    double leftPeak = kMeterFloorDb;
    double rightPeak = kMeterFloorDb;
    if (!readLevelChannels(gst_structure_get_value(structure, "peak"),
                           leftPeak, rightPeak)) {
        return;
    }

    guint64 runningTime = GST_CLOCK_TIME_NONE;
    if (!gst_structure_get_uint64(structure, "running-time", &runningTime)
        || !GST_CLOCK_TIME_IS_VALID(runningTime)) {
        return;
    }

    TimedAudioLevel timedLevel;
    timedLevel.runningTime = runningTime;
    timedLevel.level.leftPeakDb = displayDb(leftPeak);
    timedLevel.level.rightPeakDb = displayDb(rightPeak);
    timedLevel.level.clipped = leftPeak >= 0.0 || rightPeak >= 0.0;

    std::lock_guard<std::mutex> lock(audioLevelMutex_);
    if (bgmSource) {
        displayedBgmInputAudioLevel_ = timedLevel.level;
        return;
    }
    if (!audioMeterCollecting_.load(std::memory_order_relaxed)
        || generation != audioLevelGeneration_.load(std::memory_order_relaxed)) {
        return;
    }

    std::deque<TimedAudioLevel> *pendingLevels = nullptr;
    if (source == GST_OBJECT(masterAudioLevel_)) {
        pendingLevels = &pendingMasterAudioLevels_;
    } else if (bgmPostFaderAudioLevel_ && source == GST_OBJECT(bgmPostFaderAudioLevel_)) {
        if (!bgmAudioMeterActive_.load(std::memory_order_relaxed)) return;
        pendingLevels = &pendingBgmAudioLevels_;
    } else if (audioLevelRoute_.inputAudioLevel
               && source == GST_OBJECT(audioLevelRoute_.inputAudioLevel)) {
        pendingLevels = &pendingInputAudioLevels_;
    } else if (audioLevelRoute_.itemAudioLevel
               && source == GST_OBJECT(audioLevelRoute_.itemAudioLevel)) {
        pendingLevels = &pendingItemAudioLevels_;
    } else {
        return;
    }

    pendingLevels->push_back(timedLevel);
    while (pendingLevels->size() > 2048) {
        pendingLevels->pop_front();
    }
}

GstClockTime ProgramAudioMeter::currentRunningTime(GstElement *pipeline)
{
    if (!pipeline) {
        return GST_CLOCK_TIME_NONE;
    }
    GstClock *clock = gst_element_get_clock(pipeline);
    if (!clock) {
        return GST_CLOCK_TIME_NONE;
    }
    const GstClockTime clockTime = gst_clock_get_time(clock);
    const GstClockTime baseTime = gst_element_get_base_time(pipeline);
    gst_object_unref(clock);
    return clockTime >= baseTime
        ? clockTime - baseTime : GST_CLOCK_TIME_NONE;
}
