#pragma once
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace vtrpon {
inline bool isForeignWindowsMediaPath(const QString &path) {
#ifdef Q_OS_WIN
  Q_UNUSED(path);
  return false;
#else
  return (path.size() >= 3 && path[0].isLetter() && path[0].unicode() < 128 &&
          path[1] == QLatin1Char(':') &&
          (path[2] == QLatin1Char('\\') || path[2] == QLatin1Char('/'))) ||
         path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"));
#endif
}
inline QString playlistMediaFileName(QString path) {
  if (isForeignWindowsMediaPath(path)) path.replace(QLatin1Char('\\'), QLatin1Char('/'));
  return QFileInfo(path).fileName();
}
inline QString restoredPlaylistMediaPath(const QString &path, const QString &document = {}) {
  if (isForeignWindowsMediaPath(path)) return path;
#ifndef Q_OS_WIN
  if (!document.isEmpty() && QFileInfo(path).isRelative())
    return QDir(QFileInfo(document).absolutePath()).absoluteFilePath(path);
#endif
  return QFileInfo(path).absoluteFilePath();
}
inline bool playlistMediaFileExists(const QString &path) {
  if (isForeignWindowsMediaPath(path)) return false;
  const QFileInfo file(path);
  return file.exists() && file.isFile();
}
}
