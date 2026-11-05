#pragma once
#include <memory>
#include <QString>
struct ID3D11RenderTargetView;
class QImage;
namespace vtrpon {
#ifdef _WIN32
bool virtualCameraEnabled();
bool setVirtualCameraEnabled(bool enabled);
bool virtualCameraReturnMonitor();
bool setVirtualCameraReturnMonitor(bool enabled);
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
class VirtualCameraOutput final {
public:
    VirtualCameraOutput();
    ~VirtualCameraOutput();
    void publish(ID3D11RenderTargetView* target);
    void publishReturnMonitor(const QImage &image);
private:
#ifdef VTRPON_VCAM_TEST
    friend struct VirtualCameraOutputTestAccess;
#endif
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#else
inline bool virtualCameraEnabled() { return false; }
inline bool setVirtualCameraEnabled(bool) { return false; }
inline void reloadVirtualCameraEnabled() {}
inline QString virtualCameraStatus() { return QStringLiteral("Virtual camera requires Windows 11."); }
class VirtualCameraRuntime final {};
#endif
}
