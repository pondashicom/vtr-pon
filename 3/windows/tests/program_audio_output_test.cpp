#include "program_audio_output.h"
#include "audio_equalizer_pcm_checks.h"
#include "audio_delay_pcm_checks.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <gst/gst.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void pump(int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

// Observe actual PCM after the PROGRAM master stage. The sink is always a
// fakesink, so this test never opens the user's audio device.
class PcmMeasurement {
public:
    explicit PcmMeasurement(GstElement *level)
        : pad_(level ? gst_element_get_static_pad(level, "src") : nullptr)
    {
        if (pad_) probe_ = gst_pad_add_probe(pad_, GST_PAD_PROBE_TYPE_BUFFER,
                                           &PcmMeasurement::capture, this, nullptr);
    }
    ~PcmMeasurement()
    {
        if (pad_) {
            if (probe_) gst_pad_remove_probe(pad_, probe_);
            gst_object_unref(pad_);
        }
    }
    bool available() const { return pad_ && probe_; }

    bool expectRms(double expected, const char *message, double tolerance = -1.0)
    {
        // Let already queued buffers leave the master stage before measuring
        // the next control setting.
        pump(100);
        {
            std::lock_guard lock(mutex_);
            sumSquares_ = 0;
            samples_ = 0;
            invalidFormat_ = false;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 1500) {
            {
                std::lock_guard lock(mutex_);
                if (samples_ >= 14400 || invalidFormat_) break;
            }
            pump(5);
        }
        std::lock_guard lock(mutex_);
        const double rms = samples_ ? std::sqrt(sumSquares_ / samples_) : -1.0;
        std::cout << message << ": rms=" << rms << " expected=" << expected
                  << " samples=" << samples_ << '\n';
        if (tolerance < 0.0) tolerance = expected == 0 ? 0.0001 : 0.006 + expected * 0.05;
        return require(!invalidFormat_ && samples_ >= 14400
                           && std::abs(rms - expected) <= tolerance, message);
    }

private:
    static GstPadProbeReturn capture(GstPad *pad, GstPadProbeInfo *info, gpointer data)
    {
        auto *self = static_cast<PcmMeasurement *>(data);
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        const GstStructure *structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const char *format = structure ? gst_structure_get_string(structure, "format") : nullptr;
        const bool supported = format && std::strcmp(format, "F32LE") == 0;
        if (caps) gst_caps_unref(caps);
        if (!supported) {
            std::lock_guard lock(self->mutex_);
            self->invalidFormat_ = true;
            return GST_PAD_PROBE_OK;
        }
        GstMapInfo map{};
        if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            double squares = 0;
            const auto samples = map.size / sizeof(float);
            for (gsize index = 0; index < samples; ++index) {
                float sample;
                std::memcpy(&sample, map.data + index * sizeof(float), sizeof(float));
                squares += static_cast<double>(sample) * sample;
            }
            gst_buffer_unmap(buffer, &map);
            std::lock_guard lock(self->mutex_);
            self->sumSquares_ += squares;
            self->samples_ += samples;
        }
        return GST_PAD_PROBE_OK;
    }

    GstPad *pad_ = nullptr;
    gulong probe_ = 0;
    std::mutex mutex_;
    double sumSquares_ = 0;
    gsize samples_ = 0;
    bool invalidFormat_ = false;
};

class ToneInputs {
public:
    bool start()
    {
        // Different frequencies distinguish independent A/B/BGM inputs while
        // allowing their mixed RMS to be checked without depending on phase.
        GError *error = nullptr;
        pipeline_ = gst_parse_launch(
            "audiotestsrc name=eq-test-tone is-live=true wave=sine freq=400 volume=0.2 samplesperbuffer=480 ! "
            "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
            "vtrponaudiosink channel=deck-a-audio sync=true async=false "
            "audiotestsrc is-live=true wave=sine freq=800 volume=0.4 samplesperbuffer=480 ! "
            "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
            "vtrponaudiosink channel=deck-b-audio sync=true async=false "
            "audiotestsrc is-live=true wave=sine freq=1200 volume=0.1 samplesperbuffer=480 ! "
            "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
            "vtrponaudiosink channel=bgm-audio sync=true async=false", &error);
        if (error) {
            std::cerr << "Tone pipeline: " << error->message << '\n';
            g_error_free(error);
            return false;
        }
        return pipeline_ && gst_element_set_state(pipeline_, GST_STATE_PLAYING)
            != GST_STATE_CHANGE_FAILURE;
    }
    void setDeckAFrequency(double frequency)
    {
        GstElement *tone = gst_bin_get_by_name(GST_BIN(pipeline_), "eq-test-tone");
        g_object_set(tone, "freq", frequency, nullptr);
        gst_object_unref(tone);
    }
    ~ToneInputs()
    {
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_element_get_state(pipeline_, nullptr, nullptr, 2 * GST_SECOND);
            gst_object_unref(pipeline_);
        }
    }

private:
    GstElement *pipeline_ = nullptr;
};

bool awaitDevice(ProgramAudioOutput &output)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (output.deviceChanging() && elapsed.elapsed() < 5000) {
        pump(5);
        output.updateDevice(true);
    }
    return require(!output.deviceChanging() && output.ready(),
                   "asynchronous output open reaches ready");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(nullptr, nullptr);
    if (!checkAudioDelayPcm() || !checkAudioEqualizerPcm()) return 1;
    ProgramAudioOutput output;
    if (!require(output.initialize(nullptr, nullptr, {}, "fakesink"),
                 "PROGRAM output initializes with no physical audio sink")) return 1;
    if (!require(!output.ready(), "initialization does not open the output")) return 1;
    output.setBgmTransportGain(0.0);
    output.updateDevice(false);
    if (!require(!output.deviceChanging() && !output.ready(),
                 "caller idle policy defers output startup")) return 1;
    output.updateDevice(true);
    if (!require(output.deviceChanging(), "opening blocks starts until the result is polled")
        || !awaitDevice(output)) return 1;
    bool ok = require(output.deviceStatusKey() == QStringLiteral("settings.output.applied"),
                      "successful opening reports the applied state");

    ToneInputs tones;
    if (!require(tones.start(), "three independent timestamped audio inputs start")) return 1;
    {
        PcmMeasurement pcm(output.masterLevel());
        PcmMeasurement bgmPcm(output.bgmPostFaderLevel());
        if (!require(bgmPcm.available(), "BGM post-fader observation is attached")) return 1;
        if (!require(pcm.available(), "master-stage PCM observation is attached")) return 1;
        pump(350);
        constexpr double rootTwo = 1.4142135623730951;
        ok &= pcm.expectRms(0.0, "both deck take gates and BGM transport gate start silent");

        output.setDeckAudible(0, true);
        ok &= pcm.expectRms(0.2 / rootTwo, "deck A reaches PROGRAM while deck B remains in CUE");
        constexpr std::array<double, 3> eqFrequencies{100.0, 2500.0, 10000.0};
        for (int band = 0; band < 3; ++band) {
            tones.setDeckAFrequency(eqFrequencies[band]);
            std::array<double, 3> gains{};
            gains[band] = -15.0;
            output.setMasterEq(gains[0], gains[1], gains[2]);
            const double cutDb = band == 1 ? -15.0 : -7.5;
            ok &= pcm.expectRms(0.2 / rootTwo * std::pow(10.0, cutDb / 20.0),
                                "EQ cut follows MG band response", 0.008);
            gains[band] = 15.0;
            output.setMasterEq(gains[0], gains[1], gains[2]);
            ok &= pcm.expectRms(0.2 / rootTwo * std::pow(10.0, -cutDb / 20.0),
                                "EQ boost reaches +15 dB (shelf midpoint +7.5 dB)", 0.008);
            output.setMasterMuted(true);
            ok &= pcm.expectRms(0.0, "MASTER mute remains downstream of EQ");
            output.setMasterMuted(false);
            output.setMasterEq(0.0, 0.0, 0.0);
            ok &= pcm.expectRms(0.2 / rootTwo, "flat EQ restores nominal PCM");
        }
        tones.setDeckAFrequency(400.0);
        tones.setDeckAFrequency(2500.0);
        output.setDeckEq(0, 0.0, -12.0, 0.0);
        ok &= pcm.expectRms(0.06, "ITEM EQ attenuates only its active deck", 0.04);
        output.setDeckEq(1, 12.0, 12.0, 12.0);
        ok &= pcm.expectRms(0.06, "preparing CUE EQ does not alter the on-air deck", 0.04);
        output.setDeckEq(1, 0.0, 0.0, 0.0);
        output.setDeckAudible(0, false);
        output.setBgmTransportGain(1.0);
        ok &= pcm.expectRms(0.1 / rootTwo, "BGM bypasses the PROGRAM ITEM EQ");
        output.setBgmTransportGain(0.0);
        output.setDeckEq(0, 0.0, 0.0, 0.0);
        tones.setDeckAFrequency(400.0);
        output.setDeckAudible(0, true);
        ok &= pcm.expectRms(0.2 / rootTwo, "next flat ITEM restores nominal PCM");
        output.setDeckAudible(0, false);
        output.setDeckAudible(1, true);
        ok &= pcm.expectRms(0.4 / rootTwo, "TAKE selects deck B and closes deck A");
        output.setDeckAudible(1, false);
        ok &= pcm.expectRms(0.0, "closing both take gates returns PROGRAM to silence");

        output.setBgmTransportGain(1.0);
        ok &= pcm.expectRms(0.1 / rootTwo, "BGM remains independent of closed PROGRAM deck gates");
        output.setBgmGain(0.5);
        ok &= bgmPcm.expectRms(0.05 / rootTwo, "BGM meter observes its fader attenuation");
        ok &= pcm.expectRms(0.05 / rootTwo, "BGM control gain scales BGM PCM");
        output.setBgmManualFadeGain(0.5);
        ok &= bgmPcm.expectRms(0.025 / rootTwo, "BGM meter observes manual fade attenuation");
        ok &= pcm.expectRms(0.025 / rootTwo, "BGM manual fade multiplies its control gain");
        output.setBgmTransportGain(0.0);
        ok &= pcm.expectRms(0.0, "BGM transport gate closes independently of its stored gains");

        output.setBgmGain(1.0);
        output.setBgmManualFadeGain(1.0);
        output.setBgmTransportGain(1.0);
        output.setDeckAudible(0, true);
        const double mixedRms = std::sqrt((0.2 * 0.2 + 0.1 * 0.1) / 2.0);
        ok &= pcm.expectRms(mixedRms, "PROGRAM and BGM are mixed together");
        output.setMasterVolume(0.5);
        ok &= pcm.expectRms(mixedRms * 0.5, "MASTER scales both PROGRAM and BGM");
        output.setFtbCoverage(0.5);
        ok &= pcm.expectRms(mixedRms * 0.25, "FTB multiplies MASTER for the complete mix");
        output.setMasterMuted(true);
        ok &= pcm.expectRms(0.0, "MASTER mute silences PROGRAM and BGM together");
        ok &= bgmPcm.expectRms(0.1 / rootTwo, "BGM meter is before MASTER volume, FTB and mute");
        output.setMasterMuted(false);
        ok &= pcm.expectRms(mixedRms * 0.25, "unmute preserves MASTER and FTB levels");
        output.setFtbCoverage(1.0);
        ok &= pcm.expectRms(0.0, "full FTB silences the complete mix");
        output.setFtbCoverage(0.0);
        output.setMasterVolume(1.0);

        // The fake sink has no device property. This exercises a rejected
        // explicit selection without probing any real endpoint or changing
        // persisted user settings.
        output.requestDevice(QStringLiteral("program-audio-test-unsupported-device"));
        output.updateDevice(false);
        ok &= require(!output.deviceChanging() && output.ready()
                          && output.deviceStatusKey() == QStringLiteral("settings.output.pending"),
                      "busy PROGRAM or BGM leaves a requested device pending");
        ok &= pcm.expectRms(mixedRms, "a deferred device request preserves the current output");
        output.updateDevice(true);
        ok &= require(!output.deviceChanging() && output.ready() && output.appliedDevice().isEmpty()
                          && output.deviceStatusKey() == QStringLiteral("settings.output.failed"),
                      "unsupported device selection fails without replacing the applied device");
        ok &= pcm.expectRms(mixedRms, "a rejected device request preserves current PROGRAM and BGM audio");
        output.requestDevice({});
        output.updateDevice(true);
        ok &= require(output.ready()
                          && output.deviceStatusKey() == QStringLiteral("settings.output.applied"),
                      "requesting the applied device clears a previous failure");

        // Keep the probe's callback context alive until streaming has stopped;
        // removing the probe alone is not our shutdown confirmation boundary.
        output.detachCallbacks();
        ok &= require(output.stop(), "PROGRAM output confirms shutdown before releasing its PCM observer");
    }
    output.detachCallbacks();
    ok &= require(output.stop(), "PROGRAM output shutdown remains idempotent");
    std::cout << "PROGRAM audio output contract: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
