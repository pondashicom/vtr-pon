#pragma once

#include "playlist_item.h"

#include <QString>
#include <QtGlobal>

#include <optional>

struct MediaDurationProbeResult {
  bool succeeded = false;
  bool isStillImage = false;
  PlaylistMediaType mediaType = PlaylistMediaType::Unknown;
  std::optional<qint64> durationMs = std::nullopt;
  QString errorDetail;
};

MediaDurationProbeResult probeMediaDuration(const QString &filePath);
