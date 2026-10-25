#pragma once

#include "controller_settings.h"

namespace vtrpon {
inline QString defaultGeneratedMediaDirectory() {
  const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return documents.isEmpty() ? QString() : QDir(documents).filePath(
      currentProductStorageName() + QStringLiteral("/Generated"));
}
inline QString defaultStillCaptureDirectory() {
  const auto root = defaultGeneratedMediaDirectory();
  return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("StillCapture"));
}
inline QString savedMediaDirectory(const char *key, const QString &fallback) {
  const auto path = makeControllerSettings().value(QLatin1StringView(key)).toString().trimmed();
  return path.isEmpty() ? fallback : QDir::cleanPath(path);
}
inline QString stillCaptureDirectory() {
  return savedMediaDirectory("files/stillCaptureDirectory", defaultStillCaptureDirectory());
}
inline QString documentConversionDirectory(const QString &kind) {
  const auto root = savedMediaDirectory("files/documentConversionDirectory", defaultGeneratedMediaDirectory());
  return root.isEmpty() ? QString() : QDir(root).filePath(kind);
}
} // namespace vtrpon
