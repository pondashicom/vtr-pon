#pragma once

#include <QWidget>
#include <QElapsedTimer>

class QThread;
class QLabel;

class SystemLoadWidget final : public QWidget {
public:
  explicit SystemLoadWidget(QWidget *parent = nullptr);
  ~SystemLoadWidget() override;
  void setFullscreenOutput(quint64 presentedFrames, const QSize &resolution);

private:
  QThread *sampler_ = nullptr;
  QLabel *fullscreenOutput_ = nullptr;
  QElapsedTimer fullscreenClock_;
  quint64 previousPresentedFrames_ = 0;
  QSize fullscreenResolution_;
};
