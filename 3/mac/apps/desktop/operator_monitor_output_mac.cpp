#include "operator_monitor_output.h"
#include "operator_monitor_transport_mac.h"
#include "operator_monitor_renderer.h"
#include "network_video.h"
#include "decklink.h"
#include "virtual_camera/output.h"
#include <QJsonDocument>
#include <QImage>
#include <QUuid>
#include <QDebug>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <new>

namespace vtrpon {
namespace {
struct Mapping {
  int descriptor = -1;
  MacOperatorMonitorShared *shared = nullptr;
  Mapping() {
    const auto name = (QStringLiteral("/vtrom-") + QUuid::createUuid().toString(QUuid::Id128).left(20)).toUtf8();
    descriptor = shm_open(name.constData(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (descriptor < 0) return;
    if (shm_unlink(name.constData()) != 0 || fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
        ftruncate(descriptor, sizeof(MacOperatorMonitorShared)) != 0) return;
    void *memory = mmap(nullptr, sizeof(MacOperatorMonitorShared), PROT_READ | PROT_WRITE,
                        MAP_SHARED, descriptor, 0);
    if (memory == MAP_FAILED) return;
    auto *value = new (memory) MacOperatorMonitorShared;
    pthread_mutexattr_t attributes;
    bool ready = pthread_mutexattr_init(&attributes) == 0;
    if (ready) {
      ready = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) == 0 &&
              pthread_mutex_init(&value->videoMutex, &attributes) == 0 &&
              pthread_mutex_init(&value->infoMutex, &attributes) == 0;
      pthread_mutexattr_destroy(&attributes);
    }
    if (!ready) { munmap(memory, sizeof(MacOperatorMonitorShared)); return; }
    shared = value;
  }
  ~Mapping() {
    // No pthread_mutex_destroy: an exited/paused reader can still own a lock.
    if (shared) munmap(shared, sizeof(MacOperatorMonitorShared));
    if (descriptor >= 0) close(descriptor);
  }
};
std::atomic_bool enabled{false};
std::mutex presentationMutex;
QJsonObject presentation;
std::mutex activeMutex;
std::weak_ptr<Mapping> active;
std::shared_ptr<Mapping> activeMapping() {
  std::unique_lock lock(activeMutex, std::try_to_lock);
  return lock ? active.lock() : nullptr;
}
}
QString operatorMonitorChannelName() { return QStringLiteral("fd:0"); }
void setOperatorMonitorEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
bool operatorMonitorEnabled() { return enabled.load(std::memory_order_relaxed); }
bool operatorMonitorReadbackNeeded() {
  return operatorMonitorEnabled() || (networkVideoOutputMask() & 16u) ||
      (deckLinkOutputMask() & 8u) || (virtualCameraReturnMonitor() && virtualCameraFrameDemand());
}
class OperatorMonitorControlFeed::Impl {
public:
  std::shared_ptr<Mapping> mapping = std::make_shared<Mapping>();
  Impl() { std::lock_guard lock(activeMutex); active = mapping; }
};
OperatorMonitorControlFeed::OperatorMonitorControlFeed() : impl_(std::make_unique<Impl>()) {
  if (!available()) qWarning("Mac return monitor shared memory is unavailable; ON AIR is unaffected.");
}
OperatorMonitorControlFeed::~OperatorMonitorControlFeed() = default;
bool OperatorMonitorControlFeed::available() const { return impl_->mapping->shared != nullptr; }
int OperatorMonitorControlFeed::sharedDescriptor() const { return available() ? impl_->mapping->descriptor : -1; }
void OperatorMonitorControlFeed::publish(const QByteArray &json) {
  if (json.size() > sizeof(MacOperatorMonitorShared::presentation.json)) return;
  {
    std::unique_lock lock(presentationMutex, std::try_to_lock);
    if (lock) presentation = QJsonDocument::fromJson(json).object();
  }
  auto *shared = impl_->mapping->shared;
  if (!shared || json.size() > sizeof(shared->presentation.json) ||
      pthread_mutex_trylock(&shared->infoMutex) != 0) return;
  std::memcpy(shared->presentation.json, json.constData(), json.size());
  shared->presentation.size = unsigned(json.size());
  ++shared->presentation.serial;
  pthread_mutex_unlock(&shared->infoMutex);
}
bool publishOperatorMonitorFrame(const QImage &image, std::uint64_t sourceSequence,
    std::uint64_t ndiGeneration, std::uint64_t deckLinkGeneration, std::int64_t timecode,
    std::uint64_t cameraGeneration) {
  if (image.isNull() || image.width() > int(operatorMonitorMaxWidth) ||
      image.height() > int(operatorMonitorMaxHeight) ||
      (image.format() != QImage::Format_ARGB32 && image.format() != QImage::Format_RGB32)) return false;
  bool published = false;
  if (operatorMonitorEnabled()) {
    auto mapping = activeMapping();
    auto *shared = mapping ? mapping->shared : nullptr;
    if (shared && pthread_mutex_trylock(&shared->videoMutex) == 0) {
      auto &frame = shared->frame;
      for (int y = 0; y < image.height(); ++y)
        std::memcpy(frame.pixels + size_t(y) * image.width() * 4, image.constScanLine(y), size_t(image.width()) * 4);
      frame.width = unsigned(image.width()); frame.height = unsigned(image.height());
      frame.publishedAt = operatorMonitorNowMs(); shared->sourceSequence = sourceSequence;
      ++frame.serial; published = true;
      pthread_mutex_unlock(&shared->videoMutex);
    }
  }
  const bool ndi = (networkVideoOutputMask() & 16u) && ndiGeneration;
  const bool deck = (deckLinkOutputMask() & 8u) && deckLinkGeneration;
  const bool camera = cameraGeneration && virtualCameraReturnMonitor() && virtualCameraFrameDemand();
  if (ndi || deck || camera) {
    QJsonObject state;
    {
      std::unique_lock lock(presentationMutex, std::try_to_lock);
      if (!lock) return published;
      state = presentation;
    }
    // This is the auxiliary readback worker, never the GUI/render producer.
    QImage composed(operatorMonitorMaxWidth, operatorMonitorMaxHeight, QImage::Format_ARGB32);
    { QPainter painter(&composed); paintOperatorMonitor(painter, composed.size(), image, state); }
    const QByteArray pixels(reinterpret_cast<const char *>(composed.constBits()), composed.sizeInBytes());
    if (ndi) publishNetworkReturnMonitor(pixels, composed.width(), composed.height(), ndiGeneration, timecode);
    if (deck) publishDeckLinkReturnMonitor(pixels, composed.width(), composed.height(), deckLinkGeneration, timecode);
    if (camera) publishVirtualCameraFrame(composed, sourceSequence, true, cameraGeneration);
  }
  return published;
}
}
