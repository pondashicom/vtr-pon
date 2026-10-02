#include "settings_style.h"

#include "controller_scaling.h"
#include <QDebug>
#include <QFile>

namespace vtrpon {

QString settingsStyleSheet(const QString &dialogObjectName) {
  QFile file(QStringLiteral(":/vtrpon/apps/desktop/settings.qss"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Cannot load settings stylesheet:" << file.errorString();
    return {};
  }
  QString styleSheet = QString::fromUtf8(file.readAll());
  styleSheet.replace(QStringLiteral("QDialog#controllerSettingsDialog"),
                     QStringLiteral("QDialog#") + dialogObjectName);
  // Re-scale the original resource on each text-size change, never a scaled copy.
  return scaledStyleSheet(styleSheet);
}

} // namespace vtrpon
