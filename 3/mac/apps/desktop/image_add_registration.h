#pragma once

#include "playlist_item.h"

#include <QString>

#include <optional>

bool isPngOrJpegImagePath(const QString &filePath);

std::optional<PlaylistItemRegistration> makeImagePlaylistRegistration(
    const QString &filePath, qint64 displayDurationMs,
    int playlistNumber);
