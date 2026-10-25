#pragma once

#include <QString>
#include "wasapi_raw_audio_source.h"
#ifdef __APPLE__
#include "mac_capture_input.h"
#endif

namespace vtrpon {
inline bool registerLiveCaptureAudio() {
#ifdef __APPLE__
    return registerMacCaptureSources();
#else
    return registerWasapiRawAudioSource();
#endif
}
inline QString liveCaptureAudioInput(const QString &name) {
#ifdef __APPLE__
    return QStringLiteral("vtrponcoreaudiosrc name=%1 ! ").arg(name);
#else
    return QStringLiteral("vtrponwasapirawsrc name=%1 ! ").arg(name);
#endif
}
}
