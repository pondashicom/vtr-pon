#include "settings_style.h"

#include "controller_scaling.h"
#include <QDebug>
#include <QFile>

namespace vtrpon {

QString settingsStyleSheet() {
  QFile file(QStringLiteral(":/vtrpon/apps/desktop/settings.qss"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Cannot load settings stylesheet:" << file.errorString();
    return {};
  }
  // Re-scale the original resource on each text-size change, never a scaled copy.
  return scaledStyleSheet(QString::fromUtf8(file.readAll()));
}

} // namespace vtrpon
