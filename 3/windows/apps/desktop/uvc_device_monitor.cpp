#include "uvc_device_monitor.h"
#include "uvc_input_guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

namespace {
// Keep all COM/MF objects on this worker. Read descriptions only; do not
// activate media sources or use the GStreamer provider's capability probe.
class MediaFoundationScope final {
public:
  MediaFoundationScope() {
    com_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    status_ = SUCCEEDED(com_) ? MFStartup(MF_VERSION) : com_;
  }
  ~MediaFoundationScope() {
    if (SUCCEEDED(status_)) MFShutdown();
    if (SUCCEEDED(com_)) CoUninitialize();
  }
  HRESULT status() const { return status_; }
private:
  HRESULT com_ = E_FAIL;
  HRESULT status_ = E_FAIL;
};

struct DeviceDescriptions {
  IMFActivate **values = nullptr;
  UINT32 count = 0;
  ~DeviceDescriptions() {
    if (values) {
      for (UINT32 i = 0; i < count; ++i) if (values[i]) values[i]->Release();
      CoTaskMemFree(values);
    }
  }
};

QString descriptionString(IMFAttributes &description, REFGUID key) {
  wchar_t *value = nullptr;
  UINT32 length = 0;
  const HRESULT result = description.GetAllocatedString(key, &value, &length);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(value, CoTaskMemFree);
  const QString text = SUCCEEDED(result) && value
      ? QString::fromWCharArray(value, static_cast<int>(length)) : QString();
  return text;
}

HRESULT enumerateDescriptions(QList<UvcDevice> &devices) {
  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  HRESULT result = MFCreateAttributes(&attributes, 1);
  if (FAILED(result)) return result;
  result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(result)) return result;
  DeviceDescriptions descriptions;
  result = MFEnumDeviceSources(attributes.Get(), &descriptions.values, &descriptions.count);
  if (FAILED(result)) return result;
  for (UINT32 i = 0; i < descriptions.count; ++i) {
    if (!descriptions.values[i]) continue;
    IMFAttributes &description = *descriptions.values[i];
    const QString name = descriptionString(description, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    const QString path = descriptionString(description, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    if (name.isEmpty() || path.isEmpty()) continue;
    const bool duplicate = std::any_of(devices.cbegin(), devices.cend(), [&](const UvcDevice &device) {
      return device.devicePath.compare(path, Qt::CaseInsensitive) == 0;
    });
    if (!duplicate) devices.append({name, path});
  }
  std::sort(devices.begin(), devices.end(), [](const UvcDevice &left, const UvcDevice &right) {
    const int nameOrder = QString::localeAwareCompare(left.displayName, right.displayName);
    return nameOrder != 0 ? nameOrder < 0 : left.devicePath < right.devicePath;
  });
  return S_OK;
}
} // namespace
#endif

class UvcDeviceMonitor::Discovery final {
public:
  struct Snapshot {
    QList<UvcDevice> devices;
    QString error;
    bool available = false;
  };
  static std::shared_ptr<Discovery> start() {
    auto state = std::make_shared<Discovery>();
    try {
      std::thread([state] {
        try {
#ifdef _WIN32
          {
            MediaFoundationScope runtime;
            if (FAILED(runtime.status())) {
              state->publishFailure(runtime.status());
            } else {
              while (!state->stopping()) {
                Snapshot next;
                const HRESULT result = enumerateDescriptions(next.devices);
                if (state->stopping()) break;
                if (SUCCEEDED(result)) {
                  next.available = true;
                  state->publish(std::move(next));
                } else {
                  // A failed query does not mean that existing devices vanished.
                  state->publishFailure(result);
                }
                std::unique_lock lock(state->mutex_);
                state->wake_.wait_for(lock, std::chrono::seconds(1),
                                     [&] { return state->stopRequested_; });
              }
            }
          } // Complete MF/COM teardown before announcing completion.
#else
          state->publish({{}, QStringLiteral("UVC input is available only on Windows."), false});
#endif
        } catch (...) {
          auto failed = state->snapshot();
          failed.error = QStringLiteral("UVC device enumeration failed.");
          failed.available = false;
          state->publish(std::move(failed));
        }
        state->finished_.store(true, std::memory_order_release);
      }).detach();
    } catch (...) {
      state->publish({{}, QStringLiteral("Could not start the UVC device monitor worker."), false});
      state->finished_.store(true, std::memory_order_release);
    }
    return state;
  }
  void requestShutdown() {
    { std::lock_guard lock(mutex_); stopRequested_ = true; }
    wake_.notify_one();
  }
  bool shutdownComplete() const { return finished_.load(std::memory_order_acquire); }
  Snapshot snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
private:
#ifdef _WIN32
  void publishFailure(HRESULT result) {
    auto failed = snapshot();
    failed.available = false;
    failed.error = QStringLiteral("Could not enumerate UVC devices (0x%1).")
        .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    publish(std::move(failed));
  }
#endif
  bool stopping() const { std::lock_guard lock(mutex_); return stopRequested_; }
  void publish(Snapshot snapshot) {
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(snapshot);
  }
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  Snapshot snapshot_;
  bool stopRequested_ = false;
  std::atomic_bool finished_{false};
};

UvcDeviceMonitor::UvcDeviceMonitor(QObject *parent) : QObject(parent) {
  worker_ = Discovery::start();
  refreshTimer_.setInterval(50);
  connect(&refreshTimer_, &QTimer::timeout, this, &UvcDeviceMonitor::refreshDevices);
  refreshTimer_.start();
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
  // Do not publish a disappearance while identifying our own output camera.
  {
    auto &guard = vtrpon::uvcInputGuardDetail::state();
    std::lock_guard lock(guard.mutex);
    if (guard.registering) return;
  }
  auto snapshot = worker_->snapshot();
  snapshot.devices.removeIf([](const UvcDevice &device) {
    return !vtrpon::uvcInputAllowed(device.devicePath);
  });
  if (snapshot.devices == devices_ && snapshot.available == started_ &&
      snapshot.error == errorDetail_) return;
  devices_ = std::move(snapshot.devices);
  started_ = snapshot.available;
  errorDetail_ = std::move(snapshot.error);
  emit devicesChanged();
}
