#include "async_device_monitor.h"
#include "async_pipeline_stop.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <system_error>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  gst_init(nullptr, nullptr);
  using Worker = AsyncDeviceMonitor<QString>;
  const auto makeWorker = [] {
    return Worker::start("vtrpon-nonexistent-test-device-provider",
                         QStringLiteral("creation failed"),
                         QStringLiteral("start failed"),
                         [](GstDeviceProvider *) { return QList<QString>{}; });
  };
  auto worker = makeWorker();
  QElapsedTimer timer;
  timer.start();
  while (!worker->shutdownComplete() && timer.elapsed() < 5000) QThread::msleep(1);
  const auto snapshot = worker->snapshot();
  if (!worker->shutdownComplete() || snapshot.available ||
      !snapshot.devices.isEmpty() || snapshot.error != QStringLiteral("creation failed")) {
    std::cerr << "Provider initialization failure was not published/completed\n";
    return 1;
  }
  worker->requestShutdown();
  worker->requestShutdown();
  worker.reset();

  // A monitor may be destroyed immediately after construction, before its
  // worker starts. No QObject or GUI lifetime may be needed to finish cleanup.
  for (int i = 0; i < 32; ++i) {
    auto abandoned = makeWorker();
    std::weak_ptr<Worker> lifetime = abandoned;
    abandoned->requestShutdown();
    abandoned.reset();
    timer.restart();
    while (!lifetime.expired() && timer.elapsed() < 5000) QThread::msleep(1);
    if (!lifetime.expired()) {
      std::cerr << "Cancelled device worker retained its owner state\n";
      return 1;
    }
  }
  GstElement *pipeline = gst_pipeline_new("thread-failure-test");
  if (!pipeline) return 1;
  const unsigned int initialReferences = GST_OBJECT_REFCOUNT_VALUE(pipeline);
  bool cleanupCalled = false;
  auto failedStop = AsyncPipelineStop::startWithLauncher(
      pipeline, [&cleanupCalled] { cleanupCalled = true; },
      [](auto) { throw std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again)); });
  if (!failedStop->done() || failedStop->succeeded() || cleanupCalled ||
      GST_OBJECT_REFCOUNT_VALUE(pipeline) != initialReferences + 1) {
    std::cerr << "Failed stop worker did not retain the unconfirmed pipeline\n";
    return 1;
  }
  failedStop.reset();
  gst_object_unref(pipeline); // The failed worker's retained reference.
  gst_object_unref(pipeline); // The test's original reference.
  return 0;
}
