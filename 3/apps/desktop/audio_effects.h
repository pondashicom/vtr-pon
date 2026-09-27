#pragma once

#include <gst/gst.h>

namespace vtrpon {
// Application strip: select either the built-in EQ or the configured VST slot
// once per buffer. Pipelines must explicitly set slot=edit, item, or master.
// Standalone/BGM equalizers use registerAudioEqualizer() instead.
bool registerAudioEffects();
void setAudioEffectEqualizer(GstElement *effects, double lowDb, double midDb, double highDb);
void setAudioEffectAudible(GstElement *effects, bool audible);
} // namespace vtrpon
