#include "mac_gpu_device.h"
#include <QCryptographicHash>
#include <IOKit/IOKitLib.h>

namespace vtrpon {
GpuAdapter describeMetalGpu(id<MTLDevice> device) {
  if (!device) return {};
  const QString name = QString::fromUtf8(device.name.UTF8String);
  // Store the hardware's registry path, never its boot-specific registry ID.
  io_string_t path{};
  const auto entry = IOServiceGetMatchingService(kIOMainPortDefault,
      IORegistryEntryIDMatching(device.registryID));
  const bool havePath = entry && IORegistryEntryGetPath(entry, kIOServicePlane, path) == KERN_SUCCESS;
  if (entry) IOObjectRelease(entry);
  const auto identity = name.toUtf8() + ':' + (havePath ? QByteArray(path)
      : QByteArray::number(device.location) + ':' + QByteArray::number(device.locationNumber));
  return {QStringLiteral("metal:") + QString::fromLatin1(
      QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()),
      name, qint64(device.registryID)};
}
QVector<GpuAdapter> availableMetalGpuAdapters() {
  @autoreleasepool {
    QVector<GpuAdapter> result;
    for (id<MTLDevice> device in MTLCopyAllDevices()) result.push_back(describeMetalGpu(device));
    return result;
  }
}
}
