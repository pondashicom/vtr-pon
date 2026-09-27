#pragma once

#include <gst/gst.h>

namespace vtrpon {

// Register the in-process, timestamp-preserving PROGRAM PCM transport.
// Call after gst_init() and before constructing playback pipelines.
bool registerProgramAudioBridge();

// Disconnect a producer and invalidate queued/in-flight PCM before async stop.
void disconnectProgramAudioProducer(GstElement *sink);

} // namespace vtrpon
