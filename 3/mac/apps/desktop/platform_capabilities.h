#pragma once
#include <QtGlobal>

namespace vtrpon {
// Compile-time implementation availability. Add-on/runtime presence is checked
// separately by each backend; saved preferences never grant a capability.
enum class PlatformFeature { PowerPoint, OperatorMonitor, VirtualCamera,
                             GpuPreferences, Output4k, Switcher, Atem, AddonManagement, NetworkVideo, Vst3, ExternalAddons };
inline constexpr bool platformFeatureImplemented(PlatformFeature feature) {
  switch (feature) {
  case PlatformFeature::VirtualCamera:
  case PlatformFeature::OperatorMonitor:
  case PlatformFeature::GpuPreferences: case PlatformFeature::Output4k:
  case PlatformFeature::AddonManagement:
  case PlatformFeature::NetworkVideo:
  case PlatformFeature::Vst3:
  case PlatformFeature::Switcher:
  case PlatformFeature::Atem:
  case PlatformFeature::ExternalAddons:
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
  case PlatformFeature::PowerPoint:
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
  }
  return false;
}
inline constexpr const char *platformFeatureUnavailableTextId(PlatformFeature feature) {
  switch (feature) {
  case PlatformFeature::PowerPoint: return "platform.powerpoint.unavailable";
  case PlatformFeature::OperatorMonitor: return "platform.operator_monitor.unavailable";
  case PlatformFeature::VirtualCamera: return "platform.virtual_camera.unavailable";
  case PlatformFeature::GpuPreferences: return "platform.gpu.unavailable";
  case PlatformFeature::Output4k: return "platform.output4k.unavailable";
  case PlatformFeature::Atem:
  case PlatformFeature::Switcher: return "platform.switcher.unavailable";
  case PlatformFeature::AddonManagement: return "platform.addons.unavailable";
  case PlatformFeature::Vst3:
  case PlatformFeature::NetworkVideo: return "platform.external_addons.unavailable";
  case PlatformFeature::ExternalAddons: return "platform.external_addons.unavailable";
  }
  return "platform.addons.unavailable";
}
}
