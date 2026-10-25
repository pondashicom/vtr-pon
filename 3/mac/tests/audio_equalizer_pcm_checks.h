#pragma once

#include "audio_equalizer.h"
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

// Exercise the actual streaming element, not just its coefficient generator.
inline bool checkAudioEqualizerPcm() {
    if (!vtrpon::registerAudioEqualizer()) return false;
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=input format=time caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
        "vtrponaudioequalizer name=eq ! appsink name=output sync=false async=false", &error);
    if (error || !pipeline) {
        if (error) { std::cerr << error->message << '\n'; g_error_free(error); }
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    GstElement *input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    GstElement *output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    GstElement *eq = gst_bin_get_by_name(GST_BIN(pipeline), "eq");
    bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    guint64 offset = 0;
    auto transfer = [&](const std::vector<float> &pcm, bool discont, bool gap = false) {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, pcm.size() * sizeof(float), nullptr);
        gst_buffer_fill(buffer, 0, pcm.data(), pcm.size() * sizeof(float));
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(offset, GST_SECOND, 48000);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(pcm.size() / 2, GST_SECOND, 48000);
        offset += pcm.size() / 2;
        if (discont) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        if (gap) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_GAP);
        GstFlowReturn flow;
        g_signal_emit_by_name(input, "push-buffer", buffer, &flow);
        gst_buffer_unref(buffer);
        GstSample *sample = nullptr;
        if (flow == GST_FLOW_OK)
            g_signal_emit_by_name(output, "try-pull-sample", guint64(2 * GST_SECOND), &sample);
        std::vector<float> result;
        if (sample) {
            GstBuffer *received = gst_sample_get_buffer(sample);
            result.resize(gst_buffer_get_size(received) / sizeof(float));
            gst_buffer_extract(received, 0, result.data(), result.size() * sizeof(float));
            gst_sample_unref(sample);
        }
        return result;
    };
    auto measure = [&](int band, double gain, double frequency) {
        std::array<double, 3> gains{};
        gains[band] = gain;
        vtrpon::setAudioEqualizer(eq, gains[0], gains[1], gains[2]);
        constexpr int frames = 48000;
        std::vector<float> pcm(frames * 2);
        for (int n = 0; n < frames; ++n)
            pcm[n * 2] = static_cast<float>(0.05 * std::sin(2 * std::acos(-1.0) * frequency * n / 48000));
        const auto actual = transfer(pcm, true);
        if (actual.size() != pcm.size()) { ok = false; return 0.0; }
        double inPower = 0, outPower = 0;
        for (int n = frames / 2; n < frames; ++n) {
            inPower += double(pcm[n * 2]) * pcm[n * 2];
            outPower += double(actual[n * 2]) * actual[n * 2];
            ok &= actual[n * 2 + 1] == 0; // No L/R cross-feed.
            ok &= std::isfinite(actual[n * 2]);
        }
        if (gain == 0) ok &= actual == pcm; // Bit-exact flat path.
        return 10 * std::log10(outPower / inPower);
    };
    auto expect = [&](int band, double gain, double frequency, double expected, double tolerance) {
        const double actual = measure(band, gain, frequency);
        const bool passed = std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
        ok &= passed;
        if (!passed) std::cerr << "EQ band " << band << ", " << gain << " dB at "
            << frequency << " Hz: " << actual << " dB, expected " << expected << '\n';
    };
    for (double gain : {-15.0, 15.0}) {
        // Shelves reach the gain at the outer edge and half gain at the corner.
        expect(0, gain, 20, gain, 0.1);
        expect(0, gain, 100, gain / 2, 0.02);
        expect(0, gain, 2500, 0, 0.02);
        expect(2, gain, 20000, gain, 0.1);
        expect(2, gain, 10000, gain / 2, 0.02);
        expect(2, gain, 100, 0, 0.02);
        expect(1, gain, 2500, gain, 0.02);
        expect(1, gain, 20, 0, 0.02);
        expect(1, gain, 20000, 0, 0.2);
        const double lower = std::abs(measure(1, gain, 1250));
        const double upper = std::abs(measure(1, gain, 5000));
        ok &= lower > 4 && lower < 10 && upper > 4 && upper < 10;
    }
    for (double frequency : {20., 100., 1100., 2500., 10000., 20000.})
        expect(1, 0, frequency, 0, 0.00001);
    expect(1, 100, 2500, 15, 0.02); // Clamp before writing GObject properties.
    expect(1, -100, 2500, -15, 0.02);

    // Stream discontinuities and seeks must discard filter history.
    vtrpon::setAudioEqualizer(eq, 15, 15, 15);
    std::vector<float> pulse(64 * 2, 0), zeros(1024 * 2, 0);
    pulse[pulse.size() - 2] = 0.1F;
    transfer(pulse, true);
    ok &= transfer(zeros, true) == zeros;
    transfer(pulse, true);
    GstPad *src = gst_element_get_static_pad(input, "src");
    ok &= gst_pad_push_event(src, gst_event_new_flush_start()) != FALSE;
    ok &= gst_pad_push_event(src, gst_event_new_flush_stop(TRUE)) != FALSE;
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    ok &= gst_pad_push_event(src, gst_event_new_segment(&segment)) != FALSE;
    gst_object_unref(src);
    ok &= transfer(zeros, false) == zeros;
    // GAP payload is ignored even if it contains nonzero bytes.
    ok &= transfer(std::vector<float>(zeros.size(), 0.5F), true, true) == zeros;
    vtrpon::setAudioEqualizer(eq, 0, 0, 0);
    ok &= transfer(zeros, false) == zeros;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(eq);
    gst_object_unref(input);
    gst_object_unref(output);
    gst_object_unref(pipeline);
    std::cout << "MG-style EQ PCM response/reset/stereo: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}
