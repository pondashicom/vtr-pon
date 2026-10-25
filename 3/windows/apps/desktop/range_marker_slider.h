#pragma once

#include <QSlider>

#include <optional>

class QPaintEvent;
class QPainter;
class QStyleOptionSlider;
class QEvent;

class RangeMarkerSlider final : public QSlider {
public:
  enum class HardwareAccent { Program, Edit };

  explicit RangeMarkerSlider(QWidget *parent = nullptr);

  void setHardwareSkinMode(
      bool enabled, HardwareAccent accent = HardwareAccent::Program);
  void setTimelineDuration(qint64 durationMs);
  void setTimelinePosition(qint64 positionMs);
  qint64 timelinePosition() const;
  void setRangeMarkers(qint64 inPointMs, std::optional<qint64> outPointMs);
  void clearRangeMarkers();

protected:
  void changeEvent(QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  static QString
  outPointMarkerDescription(qint64 inPointMs,
                            const std::optional<qint64> &outPointMs);
  void updateAccessibleDescription();
  int sliderValueForTime(qint64 positionMs) const;
  qint64 timeForSliderValue(int sliderValue) const;
  int markerCenterX(const QStyleOptionSlider &baseOption, int value) const;
  bool hardwareSkinActive() const;
  void drawInactiveRanges(QPainter &painter, const QStyleOptionSlider &option);
  void drawMarker(QPainter &painter, const QStyleOptionSlider &baseOption,
                  int value, bool inMarker);

  qint64 timelineDurationMs_ = -1;
  std::optional<qint64> inMarkerMs_ = std::nullopt;
  std::optional<qint64> outMarkerMs_ = std::nullopt;
  bool hardwareSkinMode_ = false;
  HardwareAccent hardwareAccent_ = HardwareAccent::Program;
};
