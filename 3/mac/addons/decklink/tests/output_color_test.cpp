#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include "DeckLinkAPI.h"
using Microsoft::WRL::ComPtr;
#define SDK_CALL STDMETHODCALLTYPE
using BufferSize = ULONGLONG;
inline bool sameIid(REFIID a, REFIID b) { return IsEqualIID(a, b); }
inline bool isUnknown(REFIID iid) { return sameIid(iid, IID_IUnknown); }
#else
#include "decklink_platform.h"
#define SDK_CALL
using BufferSize = uint64_t;
inline bool isUnknown(REFIID iid) { return sameIid(iid, IUnknownUUID); }
#endif
#include "output_color.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(expression) do { if (!(expression)) { std::cerr << "Failed line " << __LINE__ << ": " #expression << '\n'; std::exit(1); } } while (false)
using namespace vtrpon::decklink;

// No device enumeration or output: exercise the same SDK conversion helper as
// pump() with caller-owned BGRA pixels and the Runtime's allocated YUV frame.
class BgraFrame final : public IDeckLinkVideoFrame, public IDeckLinkVideoBuffer {
    std::atomic<ULONG> references_{1};
    long width_, height_;
public:
    std::vector<uint8_t> pixels;
    BgraFrame(long width, long height) : width_(width), height_(height),
        pixels(static_cast<size_t>(width) * height * 4) {}
    HRESULT SDK_CALL QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (isUnknown(iid) || sameIid(iid, IID_IDeckLinkVideoFrame))
            *out = static_cast<IDeckLinkVideoFrame *>(this);
        else if (sameIid(iid, IID_IDeckLinkVideoBuffer))
            *out = static_cast<IDeckLinkVideoBuffer *>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG SDK_CALL AddRef() override { return ++references_; }
    ULONG SDK_CALL Release() override { const auto n = --references_; if (!n) delete this; return n; }
    long SDK_CALL GetWidth() override { return width_; }
    long SDK_CALL GetHeight() override { return height_; }
    long SDK_CALL GetRowBytes() override { return width_ * 4; }
    BMDPixelFormat SDK_CALL GetPixelFormat() override { return bmdFormat8BitBGRA; }
    BMDFrameFlags SDK_CALL GetFlags() override { return bmdFrameFlagDefault; }
    HRESULT SDK_CALL GetTimecode(BMDTimecodeFormat, IDeckLinkTimecode **out) override {
        if (out) *out = nullptr; return E_NOTIMPL;
    }
    HRESULT SDK_CALL GetAncillaryData(IDeckLinkVideoFrameAncillary **out) override {
        if (out) *out = nullptr; return E_NOTIMPL;
    }
    HRESULT SDK_CALL GetBytes(void **out) override { if (!out) return E_INVALIDARG; *out = pixels.data(); return S_OK; }
    HRESULT SDK_CALL GetSize(BufferSize *out) override { if (!out) return E_INVALIDARG; *out = pixels.size(); return S_OK; }
    HRESULT SDK_CALL StartAccess(BMDBufferAccessFlags) override { return S_OK; }
    HRESULT SDK_CALL EndAccess(BMDBufferAccessFlags) override { return S_OK; }
};

struct Configuration {
    OutputConfigFlag rgb = false;
    HRESULT setResult = S_OK, getResult = S_OK;
    bool ignoreSet = false;
    int sets = 0, gets = 0;
    HRESULT SetFlag(BMDDeckLinkConfigurationID id, OutputConfigFlag value) {
        CHECK(id == bmdDeckLinkConfig444SDIVideoOutput && !value);
        ++sets;
        if (setResult == S_OK && !ignoreSet) rgb = value;
        return setResult;
    }
    HRESULT GetFlag(BMDDeckLinkConfigurationID id, OutputConfigFlag *value) {
        CHECK(id == bmdDeckLinkConfig444SDIVideoOutput);
        ++gets;
        if (getResult == S_OK) *value = rgb;
        return getResult;
    }
};
void configurationChecks() {
    Configuration c;
    for (int cycle = 0; cycle < 20; ++cycle) {
        // Both a saved YCbCr setting and an externally selected RGB setting.
        c.rgb = (cycle % 2) != 0;
        CHECK(configureOutputColor(&c) == S_OK && !c.rgb);
    }
    CHECK(c.sets == 20 && c.gets == 20);
    c = {}; c.setResult = E_ACCESSDENIED;
    CHECK(FAILED(configureOutputColor(&c)) && c.gets == 0);
    c = {}; c.getResult = E_FAIL;
    CHECK(FAILED(configureOutputColor(&c)));
    c = {}; c.rgb = true; c.ignoreSet = true;
    CHECK(FAILED(configureOutputColor(&c))); // A successful write is not enough.
    c.setResult = E_NOTIMPL;
    CHECK(FAILED(configureOutputColor(&c))); // Readable RGB cannot be ignored.
    c.rgb = false;
    CHECK(configureOutputColor(&c) == S_OK); // Read-only fixed 4:2:2.
    c.getResult = E_NOTIMPL;
    CHECK(configureOutputColor(&c) == S_OK); // No SDI color control (e.g. HDMI).
}

// RGB bars are constant over pixel pairs: chroma subsampling must not blend
// across edges at the sampled centers. Values are independently calculated
// from BT.601 / BT.709 video-range YCbCr, not copied from SDK output.
struct Bar { int r, g, b; };
constexpr std::array<Bar, 8> bars{{
    {0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0},
    {0, 0, 255}, {64, 64, 64}, {128, 128, 128}, {192, 192, 192}
}};
void conversionChecks(IDeckLinkVideoConversion *converter, long width, long height) {
    ComPtr<BgraFrame> source;
    source.Attach(new BgraFrame(width, height));
    for (long y = 0; y < height; ++y) for (long x = 0; x < width; ++x) {
        const auto color = bars[static_cast<size_t>(x * 8 / width)];
        auto *p = source->pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
        p[0] = static_cast<uint8_t>(color.b); p[1] = static_cast<uint8_t>(color.g);
        p[2] = static_cast<uint8_t>(color.r); p[3] = 255;
    }
    ComPtr<IDeckLinkVideoFrame> converted;
    const auto result = convertOutputFrame(converter, source.Get(), converted.GetAddressOf());
    CHECK(result == S_OK && converted);
    CHECK(converted->GetWidth() == width && converted->GetHeight() == height);
    CHECK(converted->GetPixelFormat() == bmdFormat8BitYUV && converted->GetRowBytes() >= width * 2);
    ComPtr<IDeckLinkVideoBuffer> buffer;
    CHECK(converted.As(std::addressof(buffer)) == S_OK);
    CHECK(buffer->StartAccess(bmdBufferAccessRead) == S_OK);
    void *data = nullptr; BufferSize size = 0;
    CHECK(buffer->GetBytes(&data) == S_OK && data && buffer->GetSize(&size) == S_OK);
    CHECK(size >= static_cast<BufferSize>(converted->GetRowBytes()) * height);
    const bool sd = height <= 576;
    const double kr = sd ? 0.299 : 0.2126, kb = sd ? 0.114 : 0.0722;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto color = bars[i];
        const double luma = kr * color.r + (1 - kr - kb) * color.g + kb * color.b;
        const auto y = std::lround(16 + 219 * luma / 255);
        const auto u = std::lround(128 + 112 * (color.b - luma) / ((1 - kb) * 255));
        const auto v = std::lround(128 + 112 * (color.r - luma) / ((1 - kr) * 255));
        const long x = (static_cast<long>((2 * i + 1) * width / 16)) & ~1L;
        for (const long row : {0L, height / 2, height - 1}) {
            const auto *p = static_cast<uint8_t *>(data) + row * converted->GetRowBytes() + x * 2;
            const int tolerance = i < 2 ? 0 : 2;
            if (std::abs(p[0] - u) > tolerance || std::abs(p[1] - y) > tolerance ||
                std::abs(p[2] - v) > tolerance || std::abs(p[3] - y) > tolerance) {
                std::cerr << width << 'x' << height << " bar " << i << " expected UYVY "
                    << u << ',' << y << ',' << v << ',' << y << " got "
                    << int(p[0]) << ',' << int(p[1]) << ',' << int(p[2]) << ',' << int(p[3]) << '\n';
                CHECK(false);
            }
        }
    }
    CHECK(buffer->EndAccess(bmdBufferAccessRead) == S_OK);
    std::cout << width << 'x' << height << " YCbCr 4:2:2 " << (sd ? "BT.601" : "BT.709")
        << ": color bars, black/white and key gray levels passed.\n";
}
int main() {
    configurationChecks();
    std::cout << "Output color configuration cycles/failures passed.\n";
    ComPtr<IDeckLinkVideoConversion> converter;
#if defined(_WIN32)
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CHECK(SUCCEEDED(initialized));
    const auto created = CoCreateInstance(CLSID_CDeckLinkVideoConversion, nullptr, CLSCTX_INPROC_SERVER,
        IID_IDeckLinkVideoConversion, reinterpret_cast<void **>(converter.GetAddressOf()));
    if (created == REGDB_E_CLASSNOTREG) { CoUninitialize(); return 77; }
    CHECK(created == S_OK && converter);
#else
    converter.Attach(CreateVideoConversionInstance());
    if (!converter) return 77;
#endif
    for (const auto dimensions : {std::array<long, 2>{720, 486}, {720, 576}, {1280, 720},
            {1920, 1080}, {3840, 2160}, {4096, 2160}})
        conversionChecks(converter.Get(), dimensions[0], dimensions[1]);
    converter.Reset();
#if defined(_WIN32)
    CoUninitialize();
#endif
}
