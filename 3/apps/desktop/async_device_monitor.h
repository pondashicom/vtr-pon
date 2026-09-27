#pragma once

#include <QList>
#include <QString>
#include <gst/gst.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Only value snapshots cross the boundary. The worker owns the provider from
// creation through stop/unref and never references the GUI monitor.
template<class Device> class AsyncDeviceMonitor final {
public:
  struct Snapshot {
    QList<Device> devices;
    QString error;
    bool available = false;
  };
  using Reader = std::function<QList<Device>(GstDeviceProvider *)>;

  static std::shared_ptr<AsyncDeviceMonitor> start(
      const char *factory, QString createError, QString startError, Reader read) {
    auto state = std::make_shared<AsyncDeviceMonitor>();
    try {
      std::thread([state, factory, createError = std::move(createError),
                   startError = std::move(startError), read = std::move(read)] {
        GstDeviceProvider *provider = nullptr;
        GstBus *bus = nullptr;
        bool started = false;
        try {
          if (!state->stopping()) {
            provider = gst_device_provider_factory_get_by_name(factory);
            if (provider) {
              bus = gst_device_provider_get_bus(provider);
              started = bus && gst_device_provider_start(provider);
            }
            if (!started) {
              state->publish({{}, provider ? startError : createError, false});
            } else {
              while (!state->stopping()) {
                while (GstMessage *message = gst_bus_pop(bus)) gst_message_unref(message);
                auto devices = read(provider);
                if (state->stopping()) break;
                state->publish({std::move(devices), {}, true});
                std::unique_lock lock(state->mutex_);
                state->wake_.wait_for(lock, std::chrono::seconds(1),
                                     [&] { return state->stopRequested_; });
              }
            }
          }
        } catch (...) {
          state->publish({{}, QStringLiteral("Device monitoring failed."), false});
        }
        if (started) gst_device_provider_stop(provider);
        if (bus) gst_object_unref(bus);
        if (provider) gst_object_unref(provider);
        state->finished_.store(true, std::memory_order_release);
      }).detach();
    } catch (...) {
      state->publish({{}, QStringLiteral("Could not start the device monitor worker."), false});
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
