#pragma once
#include <QString>
#include <QByteArray>
#include <memory>
#include <cstdint>

struct ID3D11RenderTargetView;
namespace vtrpon {
QString operatorMonitorChannelName();
void setOperatorMonitorEnabled(bool enabled);

// UI-owned metadata has its own mailbox; it never shares the video/GPU lock.
class OperatorMonitorControlFeed final {
public:
  OperatorMonitorControlFeed();
  ~OperatorMonitorControlFeed();
  void publish(const QByteArray &json);
  bool available() const;
#ifdef __APPLE__
  int sharedDescriptor() const;
#endif
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class OperatorMonitorOutput final {
public:
  OperatorMonitorOutput();
  ~OperatorMonitorOutput();
  void publish(ID3D11RenderTargetView *target);
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}

#ifdef __APPLE__
class QImage;
namespace vtrpon {
bool operatorMonitorEnabled();
// Called only by the auxiliary readback worker, never the render producer.
bool publishOperatorMonitorFrame(const QImage &image, std::uint64_t sourceSequence);
}
#endif
