#pragma once

#include "playlist_model.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <atomic>

namespace PlaylistDocumentIo {

inline constexpr qsizetype kMaximumJsonBytes = 16 * 1024 * 1024;
inline constexpr qsizetype kMaximumPlaylists = 128;
inline constexpr qsizetype kMaximumItems = 10000;
inline constexpr qsizetype kMaximumStringLength = 4096;
inline constexpr qsizetype kMaximumTotalStringLength = 8 * 1024 * 1024;

struct PreparedRestore {
  PlaylistModelSnapshot snapshot;
  QSet<quint64> offlineItems;
};
// Worker-side read, validation and referenced-file checks.
std::optional<PreparedRestore> readForRestore(const QString &filePath, QString *errorDetail = nullptr,
                                               const std::atomic_bool *cancel = nullptr);

QByteArray toJson(const PlaylistModelSnapshot &snapshot);
std::optional<PlaylistModelSnapshot> fromJson(
    const QByteArray &json, QString *errorDetail = nullptr,
    const std::atomic_bool *cancel = nullptr);
bool writeFile(const QString &filePath, const PlaylistModelSnapshot &snapshot,
               QString *errorDetail = nullptr);
std::optional<PlaylistModelSnapshot> readFile(
    const QString &filePath, QString *errorDetail = nullptr,
    const std::atomic_bool *cancel = nullptr);

} // namespace PlaylistDocumentIo
