#pragma once

#include "image_add_registration.h"
#include "video_frame_capture.h"
#include <QDateTime>
#include <QDir>
#include <QSaveFile>
#include <QUuid>

struct StillCaptureResult {
  std::optional<PlaylistItemRegistration> item;
  QString path;
  QString errorKey;
  QString detail;
};

// No widgets/model/settings are accessed here; all inputs belong to this job.
inline StillCaptureResult saveStillCapture(const VideoFrameCapture &capture,
                                          const QString &directory, bool program,
                                          qint64 durationMs, int playlistNumber) {
  StillCaptureResult result;
  const QImage image = capture ? capture() : QImage{};
  if (image.isNull()) {
    result.errorKey = QStringLiteral("input.still.no_frame");
    return result;
  }
  if (directory.isEmpty() || !QDir::isAbsolutePath(directory) || !QDir().mkpath(directory)) {
    result.errorKey = QStringLiteral("input.still.save_failed");
    result.detail = directory;
    return result;
  }
  result.path = QDir(directory).filePath(QStringLiteral("%1-%2-%3.png")
      .arg(program ? QStringLiteral("program") : QStringLiteral("preview"),
           QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")),
           QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QSaveFile file(result.path);
  if (!file.open(QIODevice::WriteOnly) ||
      !image.scaled(1920, 1080, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(&file, "PNG") ||
      !file.commit()) {
    result.errorKey = QStringLiteral("input.still.save_failed");
    result.detail = result.path + QStringLiteral("\n") + file.errorString();
    return result;
  }
  result.item = makeImagePlaylistRegistration(result.path, durationMs, playlistNumber);
  if (!result.item) {
    result.errorKey = QStringLiteral("input.still.add_failed");
    result.detail = result.path;
  }
  return result;
}
