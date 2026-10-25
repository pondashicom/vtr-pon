#include "program_audio_meter.h"

#include <QCoreApplication>
#include <gst/gst.h>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool isLevel(const ProgramAudioMeter::AudioLevel &level, double left,
             double right, bool clipped = false)
{
    return std::abs(level.leftPeakDb - left) < 0.000001
        && std::abs(level.rightPeakDb - right) < 0.000001
        && level.clipped == clipped;
}

class Element {
public:
    explicit Element(const char *name)
        : value(gst_element_factory_make("level", name))
    {
        if (value) gst_object_ref_sink(value);
    }
    ~Element() { if (value) gst_object_unref(value); }
    Element(const Element &) = delete;
    Element &operator=(const Element &) = delete;
    GstElement *value;
};

// The pipelines need only real Gst clocks and independent base times. No
// playback, device, asynchronous wait, or wall-clock timing assertion is needed.
class PipelineClock {
public:
    PipelineClock()
        : pipeline(gst_pipeline_new(nullptr)), clock_(gst_system_clock_obtain())
    {
        gst_element_set_clock(pipeline, clock_);
        setRunningTime(5 * GST_SECOND);
    }
    ~PipelineClock()
    {
        gst_object_unref(pipeline);
        gst_object_unref(clock_);
    }
    void setRunningTime(GstClockTime time)
    {
        gst_element_set_base_time(pipeline, gst_clock_get_time(clock_) - time);
    }
    GstElement *pipeline;

private:
    GstClock *clock_;
};

void sendLevel(ProgramAudioMeter &meter, GstElement *source, double left,
               double right, GstClockTime time, bool mono = false,
               bool useArray = false)
{
    GValue channels = G_VALUE_INIT;
    g_value_init(&channels, useArray ? GST_TYPE_ARRAY : GST_TYPE_LIST);
    GValue peak = G_VALUE_INIT;
    g_value_init(&peak, G_TYPE_DOUBLE);
    const auto append = [&](double value) {
        g_value_set_double(&peak, value);
        if (useArray) gst_value_array_append_value(&channels, &peak);
        else gst_value_list_append_value(&channels, &peak);
    };
    append(left);
    if (!mono) append(right);
    GstStructure *structure = gst_structure_new("level", "running-time",
                                               G_TYPE_UINT64, time, nullptr);
    gst_structure_set_value(structure, "peak", &channels);
    GstMessage *message = gst_message_new_element(GST_OBJECT(source), structure);
    meter.update(message);
    // update borrows the message; the bus owner remains responsible for it.
    gst_message_unref(message);
    g_value_unset(&peak);
    g_value_unset(&channels);
}

bool routingAndTimeContract()
{
    Element input("test-input"), item("test-item"), master("test-master"),
        bgm("test-bgm"), nextInput("test-next-input"), nextItem("test-next-item");
    PipelineClock inputClock, programClock;
    ProgramAudioMeter meter;
    meter.setOutputSources(master.value, bgm.value);
    meter.setDeckRoute(input.value, item.value);
    bool ok = require(!meter.active(), "a new route is initially inactive");
    sendLevel(meter, input.value, -12, -14, 0);
    ok &= require(isLevel(meter.levels(inputClock.pipeline, programClock.pipeline).input, -60, -60),
                  "inactive non-preroll input messages are not collected");

    meter.setDeckRoute(input.value, item.value, true);
    sendLevel(meter, input.value, -12, -14, 10 * GST_SECOND);
    sendLevel(meter, item.value, -18, -20, 10 * GST_SECOND);
    sendLevel(meter, master.value, -24, -26, 10 * GST_SECOND);
    auto levels = meter.levels(nullptr, nullptr);
    ok &= require(isLevel(levels.input, -60, -60)
                      && isLevel(levels.itemPostFader, -60, -60)
                      && isLevel(levels.masterOutput, -60, -60),
                  "missing pipeline clocks do not expose queued future readings");
    inputClock.setRunningTime(15 * GST_SECOND);
    levels = meter.levels(inputClock.pipeline, programClock.pipeline);
    ok &= require(isLevel(levels.input, -12, -14)
                      && isLevel(levels.itemPostFader, -60, -60)
                      && isLevel(levels.masterOutput, -60, -60),
                  "INPUT follows the source clock while ITEM and MASTER await the output clock");
    programClock.setRunningTime(15 * GST_SECOND);
    levels = meter.levels(inputClock.pipeline, programClock.pipeline);
    ok &= require(!meter.active() && isLevel(levels.itemPostFader, -18, -20)
                      && isLevel(levels.masterOutput, -24, -26),
                  "preroll collection preserves output-clock readings before active playback");

    sendLevel(meter, input.value, 3, -90, 20 * GST_SECOND, false, true);
    sendLevel(meter, item.value, -30, -30, 20 * GST_SECOND, true);
    levels = meter.levels(inputClock.pipeline, programClock.pipeline);
    ok &= require(isLevel(levels.input, -12, -14) && isLevel(levels.itemPostFader, -18, -20),
                  "later measurements remain queued until their own running time");
    programClock.setRunningTime(25 * GST_SECOND);
    levels = meter.levels(inputClock.pipeline, programClock.pipeline);
    ok &= require(isLevel(levels.input, -12, -14) && isLevel(levels.itemPostFader, -30, -30),
                  "advancing the output clock does not advance INPUT and mono peaks duplicate channels");
    inputClock.setRunningTime(25 * GST_SECOND);
    levels = meter.levels(inputClock.pipeline, programClock.pipeline);
    ok &= require(isLevel(levels.input, 0, -60, true),
                  "array peaks retain clipping information while display values clamp to the meter range");

    sendLevel(meter, input.value, -2, -2, 100 * GST_SECOND);
    sendLevel(meter, master.value, -1, -1, 100 * GST_SECOND);
    meter.setActive(true);
    meter.setDeckRoute(nextInput.value, nextItem.value, true);
    sendLevel(meter, input.value, -3, -3, 0);
    sendLevel(meter, item.value, -4, -4, 0);
    sendLevel(meter, nextInput.value, -35, -37, 0);
    sendLevel(meter, nextItem.value, -40, -42, 0);
    inputClock.setRunningTime(150 * GST_SECOND);
    programClock.setRunningTime(150 * GST_SECOND);
    levels = meter.levels(inputClock.pipeline, programClock.pipeline);
    ok &= require(!meter.active() && isLevel(levels.input, -35, -37)
                      && isLevel(levels.itemPostFader, -40, -42)
                      && isLevel(levels.masterOutput, -60, -60),
                  "TAKE route replacement clears old queues and rejects late messages from the previous source");
    sendLevel(meter, nextInput.value, -2, -2, GST_CLOCK_TIME_NONE);
    GstMessage *irrelevant = gst_message_new_element(GST_OBJECT(nextInput.value),
        gst_structure_new("not-a-level", "peak", G_TYPE_DOUBLE, 0.0, nullptr));
    meter.update(irrelevant);
    gst_message_unref(irrelevant);
    ok &= require(isLevel(meter.levels(inputClock.pipeline, programClock.pipeline).input, -35, -37),
                  "invalid running time and unrelated element messages do not overwrite valid levels");
    meter.reset(false);
    sendLevel(meter, nextInput.value, -1, -1, 0);
    ok &= require(isLevel(meter.levels(inputClock.pipeline, programClock.pipeline).input, -60, -60),
                  "stop reset clears displayed levels and disables further deck collection");
    return ok;
}

bool bgmContract()
{
    Element master("bgm-test-master"), bgm("bgm-test-signal"), post("bgm-test-post");
    PipelineClock clock;
    ProgramAudioMeter meter;
    meter.setOutputSources(master.value, bgm.value, post.value);
    meter.setDeckRoute(nullptr, nullptr);
    sendLevel(meter, bgm.value, -9, -11, 0);
    bool ok = require(isLevel(meter.bgmSignalLevel(), -9, -11),
                      "BGM input signal is available independently of PROGRAM deck playback");
    meter.setBgmActive(true);
    sendLevel(meter, post.value, -21, -23, 10 * GST_SECOND);
    ok &= require(isLevel(meter.levels(nullptr, clock.pipeline).bgmPostFader, -60, -60),
                  "BGM post-fader waits for the output clock");
    clock.setRunningTime(15 * GST_SECOND);
    ok &= require(isLevel(meter.levels(nullptr, clock.pipeline).bgmPostFader, -21, -23)
                      && isLevel(meter.bgmSignalLevel(), -9, -11),
                  "BGM post-fader and input signal are independent");
    meter.setDeckRoute(nullptr, nullptr);
    ok &= require(isLevel(meter.levels(nullptr, clock.pipeline).bgmPostFader, -21, -23),
                  "deck route changes preserve the BGM meter");
    sendLevel(meter, master.value, -15, -17, 0);
    auto levels = meter.levels(nullptr, clock.pipeline);
    ok &= require(isLevel(levels.masterOutput, -15, -17)
                      && isLevel(levels.input, -60, -60)
                      && isLevel(levels.itemPostFader, -60, -60),
                  "BGM-only playback drives MASTER with no active deck meter route");
    meter.setBgmActive(false);
    sendLevel(meter, post.value, -1, -1, 0);
    ok &= require(isLevel(meter.levels(nullptr, clock.pipeline).bgmPostFader, -60, -60),
                  "BGM stop clears post-fader readings and rejects late levels");
    sendLevel(meter, master.value, -2, -2, 0);
    ok &= require(isLevel(meter.levels(nullptr, clock.pipeline).masterOutput, -60, -60),
                  "stopping BGM clears MASTER and closes collection when no deck is collecting");
    meter.resetBgmSignal();
    ok &= require(isLevel(meter.bgmSignalLevel(), -60, -60), "BGM source reset clears its signal reading");
    meter.setDeckCollecting(true);
    meter.setBgmActive(true);
    meter.setBgmActive(false);
    sendLevel(meter, master.value, -22, -23, 0);
    ok &= require(isLevel(meter.levels(nullptr, clock.pipeline).masterOutput, -22, -23),
                  "stopping BGM retains MASTER collection when the deck is still collecting");
    return ok;
}

void markReleased(gpointer data, GObject *)
{
    *static_cast<bool *>(data) = true;
}

bool ownershipContract()
{
    bool inputReleased = false, itemReleased = false, masterReleased = false, bgmReleased = false;
    ProgramAudioMeter meter;
    const auto makeTracked = [](bool &released) {
        GstElement *source = gst_element_factory_make("level", nullptr);
        gst_object_ref_sink(source);
        g_object_weak_ref(G_OBJECT(source), &markReleased, &released);
        return source;
    };
    GstElement *input = makeTracked(inputReleased), *item = makeTracked(itemReleased),
        *master = makeTracked(masterReleased), *bgm = makeTracked(bgmReleased);
    meter.setOutputSources(master, bgm);
    meter.setDeckRoute(input, item, true);
    for (GstElement *source : {input, item, master, bgm}) gst_object_unref(source);
    bool ok = require(!inputReleased && !itemReleased && !masterReleased && !bgmReleased,
                      "meter routes retain source elements after the pipeline owner releases its references");
    sendLevel(meter, input, -20, -20, 0);
    sendLevel(meter, master, -25, -25, 0);
    meter.setDeckRoute(nullptr, nullptr);
    ok &= require(inputReleased && itemReleased && !masterReleased && !bgmReleased,
                  "clearing the deck route releases only its retained source elements");
    meter.setOutputSources(nullptr, nullptr);
    ok &= require(masterReleased && bgmReleased, "clearing output sources releases their retained elements");
    return ok;
}

bool concurrentResetContract()
{
    Element input("concurrent-input"), item("concurrent-item"), master("concurrent-master"), bgm("concurrent-bgm");
    PipelineClock clock;
    ProgramAudioMeter meter;
    meter.setOutputSources(master.value, bgm.value);
    meter.setDeckRoute(input.value, item.value, true);
    std::atomic_bool begin{false};
    std::thread producer([&] {
        while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int index = 0; index < 1500; ++index) {
            sendLevel(meter, input.value, -10, -12, 0);
            sendLevel(meter, item.value, -15, -17, 0);
            sendLevel(meter, master.value, -20, -22, 0);
            sendLevel(meter, bgm.value, -25, -27, 0);
        }
    });
    begin.store(true, std::memory_order_release);
    for (int index = 0; index < 1500; ++index) {
        meter.setDeckRoute(index % 2 ? input.value : nullptr,
                           index % 2 ? item.value : nullptr, true);
        meter.setBgmActive(index % 3 == 0);
        meter.reset(index % 2 == 0);
        meter.setDeckCollecting(true);
        (void)meter.levels(clock.pipeline, clock.pipeline);
        (void)meter.bgmSignalLevel();
    }
    producer.join();
    // Assert a deterministic boundary after the concurrent work, never a
    // particular ordering between the producer and the controller thread.
    meter.setBgmActive(false);
    meter.setDeckRoute(input.value, item.value, true);
    sendLevel(meter, input.value, -31, -32, 0);
    sendLevel(meter, item.value, -33, -34, 0);
    sendLevel(meter, master.value, -35, -36, 0);
    const auto levels = meter.levels(clock.pipeline, clock.pipeline);
    return require(isLevel(levels.input, -31, -32) && isLevel(levels.itemPostFader, -33, -34)
                       && isLevel(levels.masterOutput, -35, -36),
                   "concurrent bus updates and route resets preserve a clean subsequent collection boundary");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(nullptr, nullptr);
    GstElementFactory *levelFactory = gst_element_factory_find("level");
    if (!require(levelFactory != nullptr, "GStreamer level element is available")) return 1;
    gst_object_unref(levelFactory);
    bool ok = routingAndTimeContract();
    ok &= bgmContract();
    ok &= ownershipContract();
    ok &= concurrentResetContract();
    std::cout << "PROGRAM audio meter contract: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
