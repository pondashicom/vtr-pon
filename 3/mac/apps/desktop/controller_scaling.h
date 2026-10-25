#pragma once

#include <QSize>
#include <QSizeF>
#include <QString>

namespace vtrpon {

// Both sizes are Qt logical pixels (already adjusted for the OS display scale).
// Call before constructing the Controller to establish the display density.
qreal controllerScaleForGeometry(const QSizeF &screenSize,
                                 const QSizeF &availableSize);
void setUiScale(qreal scale);
qreal uiScale();
int scalePx(int value);
void setUiTextSize(int size);
int uiTextSize();
qreal uiTextScale();
int scaleTextPx(int value);
QSize scaleSize(int width, int height);
QString scaledStyleSheet(const QString &styleSheet);
QString scaledStyleSheet(const QString &styleSheet, qreal scale);

} // namespace vtrpon
