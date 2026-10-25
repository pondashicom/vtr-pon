#pragma once
#include "audio_delay.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

// Exercise actual negotiated GStreamer buffers across uneven packet sizes.
inline bool checkAudioDelayPcm() {
    if (!vtrpon::registerAudioDelay()) return false;
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=input format=time caps=\"audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved\" ! "
        "vtrponaudiodelay name=item ! vtrponaudiodelay name=master ! "
        "appsink name=output sync=false async=false", &error);
    if (error || !pipeline) {
        if (error) { std::cerr << error->message << '\n'; g_error_free(error); }
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    GstElement *input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    GstElement *output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    guint64 position = 0;
    auto transfer = [&](const std::vector<float> &pcm, bool discontinuity = false, bool gap = false) {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, pcm.size() * sizeof(float), nullptr);
        gst_buffer_fill(buffer, 0, pcm.data(), pcm.size() * sizeof(float));
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(position, GST_SECOND, 48000);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(pcm.size() / 2, GST_SECOND, 48000);
        position += pcm.size() / 2;
        if (discontinuity) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        if (gap) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_GAP);
        GstFlowReturn flow = GST_FLOW_ERROR;
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
    // Different L/R impulses prove exact delay, stereo separation, and no echo.
    for (int frames = 0; ok && frames <= 6; ++frames) {
        ok &= vtrpon::setAudioDelayFrames(pipeline, "item", frames);
        ok &= vtrpon::setAudioDelayFrames(pipeline, "master", 0);
        constexpr int total = 12000;
        const int delay = frames * 1600;
        for (int start = 0; ok && start < total; start += 127) {
            const int count = (std::min)(127, total - start);
            std::vector<float> pcm(count * 2, 0.0F);
            if (start == 0) { pcm[0] = 0.5F; pcm[15] = -0.25F; }
            const auto actual = transfer(pcm, start == 0);
            if (actual.size() != pcm.size()) { ok = false; break; }
            for (int n = 0; n < count; ++n) {
                ok &= actual[n * 2] == (start + n == delay ? 0.5F : 0.0F);
                ok &= actual[n * 2 + 1] == (start + n == delay + 7 ? -0.25F : 0.0F);
            }
        }
        std::cout << "Audio delay " << frames << " F: " << (ok ? "PASS" : "FAIL") << '\n';
    }
    // ITEM + MASTER add, with no rounding drift: 6 + 6 frames = 19200 samples.
    vtrpon::setAudioDelayFrames(pipeline, "item", 6);
    vtrpon::setAudioDelayFrames(pipeline, "master", 6);
    std::vector<float> impulse(20000 * 2, 0.0F);
    impulse[0] = 0.5F;
    auto combined = transfer(impulse, true);
    ok &= combined.size() == impulse.size();
    for (size_t n = 0; n < combined.size(); ++n)
        ok &= combined[n] == (n == 19200 * 2 ? 0.5F : 0.0F);
    // A changed control, DISCONT, or FLUSH may never replay previous audio.
    vtrpon::setAudioDelayFrames(pipeline, "master", 0);
    auto silence = [](const std::vector<float> &v) {
        return !v.empty() && std::all_of(v.begin(), v.end(), [](float f) { return f == 0.0F; });
    };
    std::vector<float> shortPulse(200, 0.5F), zeros(20000, 0.0F);
    transfer(shortPulse, true);
    vtrpon::setAudioDelayFrames(pipeline, "item", 0);
    ok &= silence(transfer(zeros));
    vtrpon::setAudioDelayFrames(pipeline, "item", 6);
    transfer(shortPulse, true);
    ok &= silence(transfer(zeros, true));
    transfer(shortPulse, true);
    GstPad *src = gst_element_get_static_pad(input, "src");
    ok &= gst_pad_push_event(src, gst_event_new_flush_start());
    ok &= gst_pad_push_event(src, gst_event_new_flush_stop(FALSE));
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    ok &= gst_pad_push_event(src, gst_event_new_segment(&segment));
    gst_object_unref(src);
    ok &= silence(transfer(zeros));
    // A GAP input still emits the buffered signal, then silence.
    vtrpon::setAudioDelayFrames(pipeline, "item", 1);
    transfer(shortPulse, true);
    auto tail = transfer(zeros, false, true);
    ok &= tail.size() == zeros.size();
    for (size_t n = 0; n < tail.size(); ++n)
        ok &= tail[n] == (n >= 3000 && n < 3200 ? 0.5F : 0.0F);
    ok &= !vtrpon::setAudioDelayFrames(pipeline, "item", 7);
    ok &= !vtrpon::setAudioDelayFrames(pipeline, "item", -1);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(input);
    gst_object_unref(output);
    gst_object_unref(pipeline);
    std::cout << "Audio delay cascade/reset/GAP: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}
