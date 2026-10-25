#pragma once
#include <memory>
#include <cstdint>
#include <QString>
#include "platform_capabilities.h"
#include "language_text.h"
struct ID3D11RenderTargetView;
class QImage;
namespace vtrpon {
#if defined(_WIN32) || defined(Q_OS_MACOS)
bool virtualCameraEnabled();
bool setVirtualCameraEnabled(bool enabled);
void reloadVirtualCameraEnabled();
QString virtualCameraStatus();
class VirtualCameraRuntime final {
public:
    VirtualCameraRuntime();
    ~VirtualCameraRuntime();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#ifdef _WIN32
class VirtualCameraOutput final {
public:
    VirtualCameraOutput();
    ~VirtualCameraOutput();
    void publish(ID3D11RenderTargetView* target);
private:
#ifdef VTRPON_VCAM_TEST
    friend struct VirtualCameraOutputTestAccess;
#endif
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#else
bool virtualCameraFrameDemand();
bool publishVirtualCameraFrame(const QImage &image, uint64_t sequence);
bool activateVirtualCameraExtension();
void openVirtualCameraSettings();
#endif
#else
inline bool virtualCameraEnabled() { return false; }
inline bool setVirtualCameraEnabled(bool) { return false; }
inline void reloadVirtualCameraEnabled() {}
inline QString virtualCameraStatus() { return uiText(platformFeatureUnavailableTextId(PlatformFeature::VirtualCamera)); }
class VirtualCameraRuntime final {};
#endif
}
