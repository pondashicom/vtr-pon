#pragma once
#include "DeckLinkAPI.h"

namespace vtrpon::decklink {
// The host supplies BGRA pixels. The wire format is a separate choice: always
// convert to video-range YCbCr 4:2:2, including on devices that also accept BGRA.
inline constexpr BMDPixelFormat outputPixelFormat = bmdFormat8BitYUV;
inline BMDColorspace outputColorspace(long height) noexcept {
    return height <= 576 ? bmdColorspaceRec601 : bmdColorspaceRec709;
}
#if defined(_WIN32)
using OutputConfigFlag = BOOL;
#else
using OutputConfigFlag = bool;
#endif

template<class Configuration>
HRESULT configureOutputColor(Configuration *config) {
    auto result = config->SetFlag(bmdDeckLinkConfig444SDIVideoOutput, false);
    if (result != S_OK && result != E_NOTIMPL) return FAILED(result) ? result : E_FAIL;
    // Fixed-format / HDMI-only devices may not implement this SDI control.
    // A readable RGB setting must never be accepted, even if SetFlag succeeded.
    OutputConfigFlag rgb = false;
    result = config->GetFlag(bmdDeckLinkConfig444SDIVideoOutput, &rgb);
    if (result == S_OK) return rgb ? E_FAIL : S_OK;
    return result == E_NOTIMPL ? S_OK : (FAILED(result) ? result : E_FAIL);
}

inline HRESULT convertOutputFrame(IDeckLinkVideoConversion *converter,
    IDeckLinkVideoFrame *source, IDeckLinkVideoFrame **frame) {
    // Specify the matrix as well as the packing. Do not rely on the driver's
    // implicit RGB-to-YUV conversion or an inherited SDI RGB preference.
    return converter->ConvertNewFrame(source, outputPixelFormat,
        outputColorspace(source->GetHeight()), nullptr, frame);
}
}
