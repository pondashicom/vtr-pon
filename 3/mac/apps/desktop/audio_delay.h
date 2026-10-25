#pragma once

#include <gst/gst.h>

namespace vtrpon {
// Video frames use a fixed 30 fps timebase, independent of media/playback rate.
inline constexpr int kMaximumAudioDelayFrames = 6;
inline constexpr int kAudioDelaySamplesPerFrame = 1600; // 48 kHz / 30 fps
bool registerAudioDelay();
bool setAudioDelayFrames(GstElement *bin, const char *name, int frames);
void clearAudioDelay(GstElement *bin, const char *name);
}
