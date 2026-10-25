#pragma once

// Call after gst_init, before media discovery or playback is started.
void configureMacAudioDecoders();

// A bundle marker selects only the media runtime inside this .app.
// Development builds without the marker keep their existing environment.
bool configureMacBundledMediaRuntime();
