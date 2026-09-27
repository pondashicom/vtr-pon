#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace vtrpon {

inline constexpr auto kDistributionMarker = "vtr-pon-distribution.txt";

inline bool isDistributionDirectory(const QString &directory) {
  QFile marker(QDir(directory).filePath(QString::fromLatin1(kDistributionMarker)));
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
