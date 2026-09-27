#include "range_marker_slider.h"

#include "controller_scaling.h"
#include "skins/controller_skin.h"
#include "language_text.h"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStylePainter>

#include <algorithm>
#include <limits>

RangeMarkerSlider::RangeMarkerSlider(QWidget *parent)
    : QSlider(Qt::Horizontal, parent) {
  setMinimumHeight(vtrpon::scalePx(22));
  setTracking(true);
  setRange(0, 0);
  clearRangeMarkers();
}

void RangeMarkerSlider::setHardwareSkinMode(bool enabled,
                                            HardwareAccent accent) {
  if (hardwareSkinMode_ == enabled && hardwareAccent_ == accent) {
    return;
  }
  hardwareSkinMode_ = enabled;
  hardwareAccent_ = accent;
  update();
}

void RangeMarkerSlider::setTimelineDuration(qint64 durationMs) {
  timelineDurationMs_ = durationMs;
  if (durationMs < 0) {
    setRange(0, 0);
  } else {
    const qint64 maximum = (std::min)(
        durationMs, static_cast<qint64>((std::numeric_limits<int>::max)()));
    setRange(0, static_cast<int>(maximum));
  }
  update();
}

void RangeMarkerSlider::setTimelinePosition(qint64 positionMs) {
  setValue(sliderValueForTime(positionMs));
}

qint64 RangeMarkerSlider::timelinePosition() const {
  return timeForSliderValue(value());
}

void RangeMarkerSlider::setRangeMarkers(qint64 inPointMs,
                                        std::optional<qint64> outPointMs) {
  inPointMs = (std::max)(inPointMs, qint64{0});
  if (outPointMs) {
    *outPointMs = (std::max)(*outPointMs, qint64{0});
  }
  if (inMarkerMs_ == inPointMs && outMarkerMs_ == outPointMs) {
    return;
  }
  inMarkerMs_ = inPointMs;
  outMarkerMs_ = outPointMs;
  updateAccessibleDescription();
  update();
}

void RangeMarkerSlider::clearRangeMarkers() {
  inMarkerMs_.reset();
  outMarkerMs_.reset();
  updateAccessibleDescription();
  update();
}

void RangeMarkerSlider::changeEvent(QEvent *event) {
  QSlider::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    updateAccessibleDescription();
  }
}

void RangeMarkerSlider::updateAccessibleDescription() {
  setAccessibleDescription(
      inMarkerMs_ ? outPointMarkerDescription(*inMarkerMs_, outMarkerMs_)
                  : vtrpon::uiText("range_markers.none"));
}

void RangeMarkerSlider::paintEvent(QPaintEvent *) {
  QStyleOptionSlider option;
  initStyleOption(&option);
  if (hardwareSkinActive()) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool editAccent = hardwareAccent_ == HardwareAccent::Edit;

    const qreal halfStroke = 0.5 * vtrpon::uiScale();
    const QRectF chassis =
        QRectF(rect()).adjusted(halfStroke, vtrpon::scalePx(2) + halfStroke,
                               -halfStroke, -vtrpon::scalePx(2) - halfStroke);
    QLinearGradient chassisGradient(chassis.topLeft(),
                                    chassis.bottomLeft());
    chassisGradient.setColorAt(0.0, QColor(QStringLiteral("#5e6b70")));
    chassisGradient.setColorAt(0.18, QColor(QStringLiteral("#34444a")));
    chassisGradient.setColorAt(0.82, QColor(QStringLiteral("#172126")));
    chassisGradient.setColorAt(1.0, QColor(QStringLiteral("#080c0e")));
    painter.setPen(QPen(QColor(QStringLiteral("#748489")), vtrpon::uiScale()));
    painter.setBrush(chassisGradient);
    painter.drawRoundedRect(chassis, vtrpon::scalePx(4),
                            vtrpon::scalePx(4));

    int trackLeft = vtrpon::scalePx(9);
    int trackRight = width() - vtrpon::scalePx(9) - 1;
    if (maximum() > minimum()) {
      const int minimumX = markerCenterX(option, minimum());
      const int maximumX = markerCenterX(option, maximum());
      trackLeft = (std::min)(minimumX, maximumX);
      trackRight = (std::max)(minimumX, maximumX);
    }
    trackRight = (std::max)(trackLeft + 1, trackRight);
    const int trackHeight = vtrpon::scalePx(7);
    const QRect track(trackLeft, (height() - trackHeight) / 2,
                      trackRight - trackLeft + 1, trackHeight);

    painter.setPen(QPen(QColor(QStringLiteral("#050708")), vtrpon::uiScale()));
    painter.setBrush(QColor(QStringLiteral("#020405")));
    painter.drawRoundedRect(track.adjusted(-vtrpon::scalePx(2),
                                           -vtrpon::scalePx(2),
                                           vtrpon::scalePx(2),
                                           vtrpon::scalePx(2)),
                            vtrpon::scalePx(3), vtrpon::scalePx(3));

    QLinearGradient railGradient(track.topLeft(), track.bottomLeft());
    railGradient.setColorAt(0.0, QColor(QStringLiteral("#111719")));
    railGradient.setColorAt(0.5, QColor(QStringLiteral("#303a3e")));
    railGradient.setColorAt(1.0, QColor(QStringLiteral("#070a0b")));
    painter.setPen(QPen(QColor(QStringLiteral("#566267")), vtrpon::uiScale()));
    painter.setBrush(railGradient);
    painter.drawRoundedRect(track, vtrpon::scalePx(2),
                            vtrpon::scalePx(2));

    if (maximum() > minimum()) {
      const int valueX = markerCenterX(option, value());
      const QRect played(track.left(), track.top(),
                         (std::max)(1, valueX - track.left() + 1),
                         track.height());
      QLinearGradient playedGradient(played.topLeft(), played.bottomLeft());
      playedGradient.setColorAt(
          0.0, QColor(editAccent ? QStringLiteral("#62b66f")
                                 : QStringLiteral("#d4544e")));
      playedGradient.setColorAt(
          0.5, QColor(editAccent ? QStringLiteral("#327842")
                                 : QStringLiteral("#9f2628")));
      playedGradient.setColorAt(
          1.0, QColor(editAccent ? QStringLiteral("#153c22")
                                 : QStringLiteral("#541015")));
      painter.setPen(Qt::NoPen);
      painter.setBrush(playedGradient);
      painter.drawRoundedRect(played, vtrpon::scalePx(2),
                              vtrpon::scalePx(2));
    }

    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QColor(255, 255, 255, 28));
    constexpr int kDivisionCount = 20;
    for (int division = 1; division < kDivisionCount; ++division) {
      const int x = track.left() + division * track.width() / kDivisionCount;
      painter.drawLine(x, track.top() + 1, x, track.bottom() - 1);
    }

    const int handleCenterX = maximum() > minimum()
        ? markerCenterX(option, value()) : track.left();
    const int handleWidth = vtrpon::scalePx(18);
    const int handleHeight = vtrpon::scalePx(14);
    const QRect handle(handleCenterX - handleWidth / 2,
                       (height() - handleHeight) / 2,
                       handleWidth, handleHeight);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 145));
    painter.drawRoundedRect(handle.translated(0, vtrpon::scalePx(2)),
                            vtrpon::scalePx(4), vtrpon::scalePx(4));
    QLinearGradient handleGradient(handle.topLeft(), handle.bottomLeft());
    if (isEnabled()) {
      handleGradient.setColorAt(
          0.0, QColor(editAccent ? QStringLiteral("#78d383")
                                 : QStringLiteral("#ff766b")));
      handleGradient.setColorAt(
          0.2, QColor(editAccent ? QStringLiteral("#429852")
                                 : QStringLiteral("#d53a38")));
      handleGradient.setColorAt(
          0.78, QColor(editAccent ? QStringLiteral("#256334")
                                  : QStringLiteral("#95171c")));
      handleGradient.setColorAt(
          1.0, QColor(editAccent ? QStringLiteral("#12391f")
                                 : QStringLiteral("#5b0b10")));
    } else {
      handleGradient.setColorAt(0.0, QColor(QStringLiteral("#6b7376")));
      handleGradient.setColorAt(1.0, QColor(QStringLiteral("#2d3335")));
    }
    painter.setPen(QPen(isEnabled()
                            ? QColor(editAccent
                                         ? QStringLiteral("#9ae3a2")
                                         : QStringLiteral("#ff9288"))
                            : QColor(QStringLiteral("#747d80")),
                        vtrpon::uiScale()));
    painter.setBrush(handleGradient);
    painter.drawRoundedRect(handle, vtrpon::scalePx(4),
                            vtrpon::scalePx(4));
    painter.setPen(QPen(QColor(QStringLiteral("#fff1e8")), vtrpon::uiScale()));
    painter.drawLine(handle.center().x(),
                     handle.top() + vtrpon::scalePx(3),
                     handle.center().x(),
                     handle.bottom() - vtrpon::scalePx(3));
    if (hasFocus()) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor(QStringLiteral("#90caf9")),
                          vtrpon::scalePx(2)));
      painter.drawRoundedRect(
          QRectF(handle).adjusted(-vtrpon::scalePx(2),
                                  -vtrpon::scalePx(2),
                                  vtrpon::scalePx(2),
                                  vtrpon::scalePx(2)),
          vtrpon::scalePx(5), vtrpon::scalePx(5));
    }
  } else {
    QStylePainter stylePainter(this);
    QStyleOptionSlider grooveOption = option;
    grooveOption.subControls = QStyle::SC_SliderGroove;
    if (tickPosition() != QSlider::NoTicks) {
      grooveOption.subControls |= QStyle::SC_SliderTickmarks;
    }

    if (maximum() > minimum()) {
      const int minimumX = markerCenterX(option, minimum());
      const int maximumX = markerCenterX(option, maximum());
      const int trackLeft = (std::min)(minimumX, maximumX);
      const int trackRight = (std::max)(minimumX, maximumX);
      stylePainter.save();
      stylePainter.setClipRect(
          QRect(trackLeft, 0, trackRight - trackLeft + 1, height()));
      stylePainter.drawComplexControl(QStyle::CC_Slider, grooveOption);
      stylePainter.restore();
    } else {
      stylePainter.drawComplexControl(QStyle::CC_Slider, grooveOption);
    }

    QStyleOptionSlider handleOption = option;
    handleOption.subControls = QStyle::SC_SliderHandle;
    stylePainter.drawComplexControl(QStyle::CC_Slider, handleOption);
  }

  if (maximum() <= minimum() || !inMarkerMs_) {
    return;
  }
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  drawInactiveRanges(painter, option);
  drawMarker(painter, option, sliderValueForTime(*inMarkerMs_), true);
  if (outMarkerMs_) {
    drawMarker(painter, option, sliderValueForTime(*outMarkerMs_), false);
  }
}

QString RangeMarkerSlider::outPointMarkerDescription(
    qint64 inPointMs, const std::optional<qint64> &outPointMs) {
  if (!outPointMs) {
    return vtrpon::uiText("range_markers.in_only")
        .arg(inPointMs);
  }
  return vtrpon::uiText("range_markers.both")
      .arg(inPointMs)
      .arg(*outPointMs);
}

int RangeMarkerSlider::sliderValueForTime(qint64 positionMs) const {
  if (timelineDurationMs_ <= 0 || maximum() <= 0) {
    return 0;
  }
  const qint64 clamped = std::clamp(positionMs, qint64{0}, timelineDurationMs_);
  if (timelineDurationMs_ <= (std::numeric_limits<int>::max)()) {
    return static_cast<int>(clamped);
  }
  const long double ratio = static_cast<long double>(clamped) /
                            static_cast<long double>(timelineDurationMs_);
  return static_cast<int>(ratio * maximum());
}

qint64 RangeMarkerSlider::timeForSliderValue(int sliderValue) const {
  if (timelineDurationMs_ <= 0 || maximum() <= 0) {
    return 0;
  }
  if (timelineDurationMs_ <= (std::numeric_limits<int>::max)()) {
    return std::clamp(sliderValue, minimum(), maximum());
  }
  const long double ratio =
      static_cast<long double>(std::clamp(sliderValue, minimum(), maximum())) /
      static_cast<long double>(maximum());
  return static_cast<qint64>(ratio * timelineDurationMs_);
}

int RangeMarkerSlider::markerCenterX(const QStyleOptionSlider &baseOption,
                                     int value) const {
  QStyleOptionSlider markerOption = baseOption;
  const int markerValue = std::clamp(value, minimum(), maximum());
  markerOption.sliderPosition = markerValue;
  markerOption.sliderValue = markerValue;
  return style()
      ->subControlRect(QStyle::CC_Slider, &markerOption,
                       QStyle::SC_SliderHandle, this)
      .center()
      .x();
}

bool RangeMarkerSlider::hardwareSkinActive() const {
  return hardwareSkinMode_ &&
         vtrpon::currentControllerSkinUsesHardwareEffects();
}

void RangeMarkerSlider::drawInactiveRanges(QPainter &painter,
                                           const QStyleOptionSlider &option) {
  const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderGroove, this);
  if (groove.isEmpty()) {
    return;
  }
  const bool hardwareSkin = hardwareSkinActive();
  const int inactiveTrackHeight = hardwareSkin ? vtrpon::scalePx(7) : 4;
  const int minimumX = markerCenterX(option, minimum());
  const int maximumX = markerCenterX(option, maximum());
  const int trackLeft = (std::min)(minimumX, maximumX);
  const int trackRight = (std::max)(minimumX, maximumX);
  const QRect track(trackLeft,
                    groove.center().y() - inactiveTrackHeight / 2,
                    trackRight - trackLeft + 1, inactiveTrackHeight);
  const int inX = markerCenterX(option, sliderValueForTime(*inMarkerMs_));
  const QColor inactiveRangeColor(hardwareSkin
                                      ? QStringLiteral("#30383b")
                                      : QStringLiteral("#44494b"));
  painter.save();
  painter.setPen(Qt::NoPen);
  painter.setBrush(inactiveRangeColor);
  if (inX > track.left()) {
    painter.drawRect(track.left(), track.top(), inX - track.left(),
                     track.height());
  }
  if (outMarkerMs_) {
    const int outX = markerCenterX(option, sliderValueForTime(*outMarkerMs_));
    if (outX < track.right()) {
      painter.drawRect(outX + 1, track.top(), track.right() - outX,
                       track.height());
    }
  }
  painter.restore();
}

void RangeMarkerSlider::drawMarker(QPainter &painter,
                                   const QStyleOptionSlider &baseOption,
                                   int value, bool inMarker) {
  const QRect groove = style()->subControlRect(QStyle::CC_Slider, &baseOption,
                                               QStyle::SC_SliderGroove, this);
  const int x = markerCenterX(baseOption, value);
  const QColor fill = inMarker ? QColor(QStringLiteral("#66d19e"))
                               : QColor(QStringLiteral("#f2a65a"));
  painter.setPen(QPen(QColor(QStringLiteral("#101112")), vtrpon::uiScale()));
  painter.setBrush(fill);
  const int halfWidth = vtrpon::scalePx(5);
  const int markerHeight = vtrpon::scalePx(7);
  const int gap = vtrpon::scalePx(1);
  if (inMarker) {
    const int top = gap;
    const QPolygon marker(
        {QPoint(x - halfWidth, top), QPoint(x + halfWidth, top),
         QPoint(x, top + markerHeight)});
    painter.drawPolygon(marker);
    painter.setPen(QPen(fill, 2 * vtrpon::uiScale()));
    painter.drawLine(x, top + markerHeight, x, groove.top() - gap);
  } else {
    const int bottom = height() - vtrpon::scalePx(2);
    const QPolygon marker(
        {QPoint(x - halfWidth, bottom), QPoint(x + halfWidth, bottom),
         QPoint(x, bottom - markerHeight)});
    painter.drawPolygon(marker);
    painter.setPen(QPen(fill, 2 * vtrpon::uiScale()));
    painter.drawLine(x, groove.bottom() + gap, x, bottom - markerHeight);
  }
}
