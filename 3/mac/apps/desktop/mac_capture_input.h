#pragma once

#include "uvc_audio_mapping.h"
#include "uvc_device_monitor.h"

typedef struct _GstCaps GstCaps;
typedef struct _GstElement GstElement;

namespace vtrpon {
// Persist native identities, never enumeration indices or display names.
inline QString macCameraPath(const QString &uid) { return QStringLiteral("avfoundation:") + uid; }
inline QString macAudioInputId(const QString &uid) { return QStringLiteral("coreaudio-input:") + uid; }
QList<UvcDevice> macCameraDevices(QString *error);
QList<AudioInputDevice> macAudioInputDevices(QString *error);
// The video source opens AVCaptureDevice by uniqueID. The audio source checks
// the selected CoreAudio input and opens osxaudiosrc by unique-id. Neither
// source substitutes another device. Only opening a source requests access.
bool registerMacCaptureSources();
// Caller owns the caps; shared by the native callback and frame-flow checks.
GstCaps *macCameraFrameCaps(int width, int height);
// Map native device-clock audio onto the shared pipeline clock, retaining PCM cadence.
// Installed on osxaudiosrc; exposed so regression fixtures use the same mapping.
bool installMacAudioClockMapping(GstElement *source);
}
