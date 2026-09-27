#pragma once
#include <QString>
#include <QByteArray>
#include <memory>

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
