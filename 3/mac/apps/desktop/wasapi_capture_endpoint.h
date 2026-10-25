#pragma once

#ifdef _WIN32
#include <mmdeviceapi.h>

namespace vtrpon {
// GStreamer's Default capture entry stores DEVINTERFACE_AUDIO_CAPTURE, which
// represents the console default rather than an IMMDevice endpoint ID.
inline HRESULT resolveWasapiCaptureEndpoint(IMMDeviceEnumerator *enumerator,
                                            LPCWSTR selectedId, IMMDevice **device) {
    if (!device) return E_POINTER;
    *device = nullptr;
    if (!enumerator || !selectedId || !*selectedId) return E_INVALIDARG;
    // SDK DEVINTERFACE_AUDIO_CAPTURE value, kept local to avoid an external
    // GUID definition dependency in the Windows desktop host and its tests.
    constexpr GUID defaultCapture{0x2eef81be, 0x33fa, 0x4800,
        {0x96, 0x70, 0x1c, 0xd4, 0x74, 0x97, 0x2c, 0x3f}};
    GUID identifier{};
    if (SUCCEEDED(IIDFromString(selectedId, &identifier))
        && IsEqualGUID(identifier, defaultCapture))
        return enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device);
    // A missing explicit endpoint must fail, never substitute a default device.
    return enumerator->GetDevice(selectedId, device);
}
}
#endif
