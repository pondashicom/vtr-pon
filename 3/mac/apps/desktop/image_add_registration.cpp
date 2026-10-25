#include "image_add_registration.h"

#include <QFileInfo>

bool isPngOrJpegImagePath(const QString &filePath) {
  const QString suffix = QFileInfo(filePath).suffix();
  return suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0 ||
         suffix.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0 ||
         suffix.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0;
}

std::optional<PlaylistItemRegistration> makeImagePlaylistRegistration(
    const QString &filePath, qint64 displayDurationMs,
    int playlistNumber) {
  const QFileInfo fileInfo(filePath);
  if (!isPngOrJpegImagePath(filePath) || !fileInfo.exists() ||
      !fileInfo.isFile() || displayDurationMs <= 0 || playlistNumber <= 0) {
    return std::nullopt;
  }

  PlaylistItemRegistration item;
  item.filePath = filePath;
  item.sourceType = PlaylistSourceType::File;
  item.mediaType = PlaylistMediaType::StillImage;
  item.settings.outPointMs = displayDurationMs;
  item.playlistNumber = playlistNumber;
  return item;
}
