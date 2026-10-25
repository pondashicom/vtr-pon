#pragma once

#include "controller_scaling.h"

#include <QPainter>
#include <QPainterPath>
#include <array>
#include <utility>

namespace vtrpon {

inline void drawMonitorNeonOutline(QPainter &painter, const QRectF &monitor,
                                   QColor color) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(Qt::NoBrush);
  // Match the playlist light falloff, keeping the monitor surface opaque.
  const qreal margin = scalePx(6);
  QPainterPath outer;
  outer.addRect(monitor.adjusted(-margin * 2, -margin * 2,
                                 margin * 2, margin * 2));
  QPainterPath surface;
  surface.addRect(monitor);
  painter.setClipPath(outer.subtracted(surface), Qt::IntersectClip);
  for (const auto &[width, alpha] :
       std::array<std::pair<qreal, qreal>, 7>{{
           {12.0, 0.025}, {10.0, 0.035}, {8.0, 0.05},
           {6.0, 0.075}, {4.0, 0.11}, {2.5, 0.18}, {1.0, 0.28}}}) {
    color.setAlphaF(alpha);
    painter.setPen(QPen(color, width * margin / 5.0));
    painter.drawRoundedRect(monitor, scalePx(3), scalePx(3));
  }
  painter.restore();
}

} // namespace vtrpon
