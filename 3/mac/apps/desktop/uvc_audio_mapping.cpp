#include "async_device_monitor.h"
#include "controller_settings.h"
#include "uvc_audio_mapping.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QSet>

#include <gst/gst.h>

#include <algorithm>
#ifdef __APPLE__
#include "mac_capture_input.h"
#endif

namespace {

QString structureString(const GstStructure *structure, const char *field) {
  if (!structure) {
    return {};
  }
  const char *value = gst_structure_get_string(structure, field);
  return value ? QString::fromUtf8(value) : QString();
}

bool structureBoolean(const GstStructure *structure, const char *field,
                      bool fallback = false) {
  gboolean value = fallback;
  return structure && gst_structure_get_boolean(structure, field, &value)
             ? value
             : fallback;
}

QSettings controllerSettings() {
  return vtrpon::makeControllerSettings();
}

} // namespace

namespace {
QList<AudioInputDevice> readDevices(GstDeviceProvider *provider) {
  QList<AudioInputDevice> refreshed;
  QSet<QString> seenIds;
  GList *gstDevices = gst_device_provider_get_devices(provider);
  for (GList *node = gstDevices; node; node = node->next) {
    auto *device = GST_DEVICE(node->data);
    GstStructure *properties = gst_device_get_properties(device);
    gchar *gstName = gst_device_get_display_name(device);
    gchar *gstClass = gst_device_get_device_class(device);
    const QString name = gstName ? QString::fromUtf8(gstName) : QString();
    const QString deviceClass =
        gstClass ? QString::fromUtf8(gstClass) : QString();
    const QString api = structureString(properties, "device.api");
    const QString deviceId = structureString(properties, "device.id");
    const bool loopback =
        structureBoolean(properties, "wasapi2.device.loopback");
    const bool isDefault = structureBoolean(properties, "device.default");
    const QString actualName =
        structureString(properties, "device.actual-name");
    g_free(gstName);
    g_free(gstClass);
    if (properties) {
      gst_structure_free(properties);
    }

    if (api.compare(QStringLiteral("wasapi2"), Qt::CaseInsensitive) != 0 ||
        !deviceClass.contains(QStringLiteral("Audio/Source"),
                              Qt::CaseInsensitive) ||
        loopback || deviceId.isEmpty() || name.isEmpty() ||
        seenIds.contains(deviceId)) {
      continue;
    }
    seenIds.insert(deviceId);
    const QString visibleName = isDefault && !actualName.isEmpty()
                                    ? QStringLiteral("Default - %1")
                                          .arg(actualName)
                                    : name;
    refreshed.append(AudioInputDevice{visibleName, deviceId, isDefault});
  }
  g_list_free_full(gstDevices,
                   reinterpret_cast<GDestroyNotify>(gst_object_unref));

  std::sort(refreshed.begin(), refreshed.end(),
            [](const AudioInputDevice &left,
               const AudioInputDevice &right) {
              if (left.isDefault != right.isDefault) {
                return left.isDefault;
              }
              const int nameOrder = QString::localeAwareCompare(
                  left.displayName, right.displayName);
              return nameOrder != 0 ? nameOrder < 0
                                    : left.deviceId < right.deviceId;
            });
  return refreshed;
}
} // namespace

AudioInputDeviceMonitor::AudioInputDeviceMonitor(QObject *parent) : QObject(parent) {
#ifdef __APPLE__
  worker_ = AsyncDeviceMonitor<AudioInputDevice>::startPolling([] {
    AsyncDeviceMonitor<AudioInputDevice>::Snapshot value;
    value.devices = vtrpon::macAudioInputDevices(&value.error);
    value.available = value.error.isEmpty();
    return value;
  });
#elif defined(_WIN32)
  worker_ = AsyncDeviceMonitor<AudioInputDevice>::start(
      "wasapi2deviceprovider", QStringLiteral("Could not create the WASAPI audio device provider."),
      QStringLiteral("Could not start monitoring audio input devices."), readDevices);
#endif
#if defined(_WIN32) || defined(__APPLE__)
  refreshTimer_.setInterval(50);
  connect(&refreshTimer_, &QTimer::timeout, this, &AudioInputDeviceMonitor::refreshDevices);
  refreshTimer_.start();
#else
  errorDetail_ = QStringLiteral("Audio input mapping is available only on Windows.");
#endif
}

AudioInputDeviceMonitor::~AudioInputDeviceMonitor() { requestShutdown(); }

QList<AudioInputDevice> AudioInputDeviceMonitor::devices() const { return devices_; }
bool AudioInputDeviceMonitor::isAvailable() const { return started_; }
QString AudioInputDeviceMonitor::errorDetail() const { return errorDetail_; }

void AudioInputDeviceMonitor::requestShutdown() {
  shutdownRequested_ = true;
  refreshTimer_.stop();
  if (worker_) worker_->requestShutdown();
}

bool AudioInputDeviceMonitor::shutdownComplete() const {
  return !worker_ || worker_->shutdownComplete();
}

void AudioInputDeviceMonitor::refreshDevices() {
  if (!worker_ || shutdownRequested_) return;
  auto snapshot = worker_->snapshot();
  if (snapshot.devices == devices_ && snapshot.available == started_ &&
      snapshot.error == errorDetail_) return;
  devices_ = std::move(snapshot.devices);
  started_ = snapshot.available;
  errorDetail_ = std::move(snapshot.error);
  emit devicesChanged();
}
UvcAudioMappingStore::UvcAudioMappingStore(QObject *parent)
    : QObject(parent) {}

QString UvcAudioMappingStore::audioDeviceIdFor(
    const QString &uvcDevicePath) const {
  if (uvcDevicePath.isEmpty()) {
    return {};
  }
  QSettings settings = controllerSettings();
  return settings.value(settingKey(uvcDevicePath)).toString();
}

bool UvcAudioMappingStore::setAudioDeviceIdFor(
    const QString &uvcDevicePath, const QString &audioDeviceId,
    QString *errorDetail) {
  if (errorDetail) {
    errorDetail->clear();
  }
  if (uvcDevicePath.isEmpty()) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("The UVC device path is empty.");
    }
    return false;
  }
  if (audioDeviceIdFor(uvcDevicePath) == audioDeviceId) {
    return true;
  }

  QSettings settings = controllerSettings();
  const QString key = settingKey(uvcDevicePath);
  if (audioDeviceId.isEmpty()) {
    settings.remove(key);
  } else {
    settings.setValue(key, audioDeviceId);
  }
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    if (errorDetail) {
      *errorDetail = QStringLiteral(
          "The UVC audio mapping could not be saved.");
    }
    return false;
  }
  emit mappingChanged(uvcDevicePath, audioDeviceId);
  return true;
}

QString UvcAudioMappingStore::settingKey(const QString &uvcDevicePath) {
  const QByteArray digest = QCryptographicHash::hash(
      uvcDevicePath.toUtf8(), QCryptographicHash::Sha256);
  return QStringLiteral("audio/uvcMappings/%1")
      .arg(QString::fromLatin1(digest.toHex()));
}
