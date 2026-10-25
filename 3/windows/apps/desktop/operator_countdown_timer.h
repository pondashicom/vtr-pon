#pragma once

#include "operator_monitor_settings.h"
#include <QJsonObject>

namespace vtrpon {
inline QString operatorTimerText(qint64 seconds) {
  const auto absolute = qAbs(seconds);
  return QStringLiteral("%1%2:%3")
      .arg(seconds < 0 ? QStringLiteral("−") : QString())
      .arg(absolute / 60, 2, 10, QLatin1Char('0'))
      .arg(absolute % 60, 2, 10, QLatin1Char('0'));
}

// The controller supplies monotonic milliseconds. Receiver visibility and media
// playback never own or advance this clock. Only configuration is persisted.
class OperatorCountdownTimer {
public:
  void configure(const OperatorMonitorSettings &settings, qint64 now) {
    const bool resetNeeded = settings.timerEnabled != settings_.timerEnabled ||
        settings.timerDurationSeconds != settings_.timerDurationSeconds;
    settings_ = settings;
    if (resetNeeded) reset(now);
  }
  void start(qint64 now) {
    if (!settings_.timerEnabled || running_) return;
    anchorMs_ = now;
    running_ = true;
    started_ = true;
  }
  void pause(qint64 now) {
    if (!running_) return;
    remainingMs_ = remainingMilliseconds(now);
    running_ = false;
  }
  void reset(qint64 now) {
    remainingMs_ = qint64(settings_.timerDurationSeconds) * 1000;
    anchorMs_ = now;
    running_ = false;
    started_ = false;
  }
  QJsonObject presentation(qint64 now) const {
    const qint64 milliseconds = remainingMilliseconds(now);
    // ceil for remaining time; integer division already rounds negatives to zero.
    const qint64 seconds = milliseconds > 0 ? (milliseconds + 999) / 1000 : milliseconds / 1000;
    int warning = 0;
    if (settings_.timerEnabled && started_) {
      if (milliseconds <= qint64(settings_.timerWarning2Minutes) * 60000) warning = 2;
      else if (milliseconds <= qint64(settings_.timerWarning1Minutes) * 60000) warning = 1;
    }
    const QColor warningColor = warning == 2 ? settings_.timerWarning2Color : settings_.timerWarning1Color;
    return {{"enabled", settings_.timerEnabled}, {"running", running_},
        {"started", started_}, {"seconds", seconds}, {"text", operatorTimerText(seconds)},
        {"warning", warning}, {"warningColor", warning ? warningColor.name() : QString()}};
  }
private:
  qint64 remainingMilliseconds(qint64 now) const {
    return remainingMs_ - (running_ ? qMax(qint64(0), now - anchorMs_) : 0);
  }
  OperatorMonitorSettings settings_;
  qint64 remainingMs_ = 300000;
  qint64 anchorMs_ = 0;
  bool running_ = false;
  bool started_ = false;
};
}
