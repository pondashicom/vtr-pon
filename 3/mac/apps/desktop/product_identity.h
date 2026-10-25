#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace vtrpon {

inline constexpr auto kDistributionMarker = "vtr-pon-distribution.txt";

inline bool isDistributionDirectory(const QString &directory) {
  QString markerPath = QDir(directory).filePath(QString::fromLatin1(kDistributionMarker));
#ifdef Q_OS_MACOS
  if (directory.endsWith(QLatin1String("/Contents/MacOS")))
    markerPath = QDir(directory).filePath(QStringLiteral("../Resources/") + QString::fromLatin1(kDistributionMarker));
#endif
  QFile marker(markerPath);
  return marker.open(QIODevice::ReadOnly) &&
         marker.readAll().trimmed() == QByteArrayLiteral("VTR-PON3");
}

inline QString productStorageName(const QString &directory) {
  return isDistributionDirectory(directory) ? QStringLiteral("VTR-PON3")
                                            : QStringLiteral("VTR-PON3-Dev");
}

inline QString currentProductStorageName() {
  return productStorageName(QCoreApplication::applicationDirPath());
}

inline void initializeProductIdentity() {
  QCoreApplication::setOrganizationName(QString());
  QCoreApplication::setApplicationName(currentProductStorageName());
}

inline QString productLocalDataDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

inline QString productCacheDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

} // namespace vtrpon
