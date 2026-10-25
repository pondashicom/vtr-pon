#include "async_device_monitor.h"
#include "uvc_device_monitor.h"
#include "uvc_input_guard.h"

#include <gst/gst.h>

#include <algorithm>
#ifdef __APPLE__
#include "mac_capture_input.h"
#endif

namespace {

QString structureString(const GstStructure *structure, const char *field) {
  if (!structure) {
    return QString();
  }
  const char *value = gst_structure_get_string(structure, field);
  return value ? QString::fromUtf8(value) : QString();
}

} // namespace

namespace {
QList<UvcDevice> readDevices(GstDeviceProvider *provider) {
  QList<UvcDevice> refreshed;
  GList *gstDevices = gst_device_provider_get_devices(provider);
  for (GList *node = gstDevices; node; node = node->next) {
    auto *device = GST_DEVICE(node->data);
    GstStructure *properties = gst_device_get_properties(device);
    const QString api = structureString(properties, "device.api");
    const QString path = structureString(properties, "device.path");
    gchar *gstName = gst_device_get_display_name(device);
    gchar *gstClass = gst_device_get_device_class(device);
    const QString name = gstName ? QString::fromUtf8(gstName) : QString();
    const QString deviceClass =
        gstClass ? QString::fromUtf8(gstClass) : QString();
    g_free(gstName);
    g_free(gstClass);
    if (properties) {
      gst_structure_free(properties);
    }
    if (api.compare(QStringLiteral("mediafoundation"), Qt::CaseInsensitive) ==
            0 &&
        deviceClass.contains(QStringLiteral("Source/Video"),
                             Qt::CaseInsensitive) &&
        !path.isEmpty() && !name.isEmpty() && vtrpon::uvcInputAllowed(path)) {
      refreshed.append(UvcDevice{name, path});
    }
  }
  g_list_free_full(gstDevices,
                   reinterpret_cast<GDestroyNotify>(gst_object_unref));
  std::sort(refreshed.begin(), refreshed.end(),
            [](const UvcDevice &left, const UvcDevice &right) {
              const int nameOrder = QString::localeAwareCompare(
                  left.displayName, right.displayName);
              return nameOrder != 0 ? nameOrder < 0
                                    : left.devicePath < right.devicePath;
            });
  return refreshed;
}
} // namespace

UvcDeviceMonitor::UvcDeviceMonitor(QObject *parent) : QObject(parent) {
#ifdef __APPLE__
  worker_ = AsyncDeviceMonitor<UvcDevice>::startPolling([] {
    AsyncDeviceMonitor<UvcDevice>::Snapshot value;
    value.devices = vtrpon::macCameraDevices(&value.error);
    value.available = value.error.isEmpty();
    return value;
  });
#elif defined(_WIN32)
  worker_ = AsyncDeviceMonitor<UvcDevice>::start(
      "mfdeviceprovider", QStringLiteral("Could not create the Media Foundation device provider."),
      QStringLiteral("Could not start monitoring UVC devices."), readDevices);
#endif
#if defined(_WIN32) || defined(__APPLE__)
  refreshTimer_.setInterval(50);
  connect(&refreshTimer_, &QTimer::timeout, this, &UvcDeviceMonitor::refreshDevices);
  refreshTimer_.start();
#else
  errorDetail_ = QStringLiteral("UVC input is available only on Windows.");
#endif
}

UvcDeviceMonitor::~UvcDeviceMonitor() { requestShutdown(); }

QList<UvcDevice> UvcDeviceMonitor::devices() const { return devices_; }
bool UvcDeviceMonitor::isAvailable() const { return started_; }
QString UvcDeviceMonitor::errorDetail() const { return errorDetail_; }

void UvcDeviceMonitor::requestShutdown() {
  shutdownRequested_ = true;
  refreshTimer_.stop();
  if (worker_) worker_->requestShutdown();
}

bool UvcDeviceMonitor::shutdownComplete() const {
  return !worker_ || worker_->shutdownComplete();
}

void UvcDeviceMonitor::refreshDevices() {
  if (!worker_ || shutdownRequested_) return;
  auto snapshot = worker_->snapshot();
  if (snapshot.devices == devices_ && snapshot.available == started_ &&
      snapshot.error == errorDetail_) return;
  devices_ = std::move(snapshot.devices);
  started_ = snapshot.available;
  errorDetail_ = std::move(snapshot.error);
  emit devicesChanged();
}