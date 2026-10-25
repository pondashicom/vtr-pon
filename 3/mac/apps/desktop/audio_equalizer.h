#pragma once

#include "equalizer_profile.h"
#include <gst/gst.h>
#include <memory>

namespace vtrpon {
// Shared EQ DSP for the standalone element and application effect strips.
// This processor has no knowledge of VST slots or application routing.
class AudioEqualizerProcessor final {
public:
    AudioEqualizerProcessor();
    ~AudioEqualizerProcessor();
    AudioEqualizerProcessor(const AudioEqualizerProcessor &) = delete;
    AudioEqualizerProcessor &operator=(const AudioEqualizerProcessor &) = delete;

    void setGain(unsigned band, double gain);
    double gain(unsigned band) const;
    void reset();
    GstFlowReturn process(GstBuffer *buffer);

private:
    struct State;
    std::unique_ptr<State> state_;
};

bool registerAudioEqualizer();
double safeEqGain(double gain);
void setAudioEqualizer(GstElement *equalizer, double lowDb, double midDb, double highDb);
} // namespace vtrpon
