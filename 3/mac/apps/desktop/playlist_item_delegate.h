#pragma once

#include "playlist_item.h"
#include <QIcon>
#include <QPixmap>
#include <QString>
#include "playlist_view_roles.h"

class QObject;
class QAbstractItemDelegate;
enum class PlaylistMapStopReason;

namespace playlist_view {
QAbstractItemDelegate *createPlaylistItemDelegate(QObject *parent);
QString conversionStageText(PlaylistItemConversionStage stage);
QString conversionStatusText(const PlaylistItemConversionState &state);
QString mediaTypeName(PlaylistMediaType mediaType);
PlaylistPowerPointMode effectivePowerPointMode(const PlaylistItemState &state);
QString powerPointModeBadgeLabel(PlaylistPowerPointMode mode);
QString playlistItemColorName(PlaylistItemColor color);
QIcon playlistItemColorIcon(PlaylistItemColor color);
QPixmap createThumbnailPlaceholder(const PlaylistItemState &state);
QString formatPlaylistPoint(qint64 pointMs);
QString formatItemPoints(const PlaylistItemSettings &settings);
QString formatItemStartMode(const PlaylistItemSettings &settings);
QString formatItemVolume(const PlaylistItemSettings &settings);
QString formatItemModes(const PlaylistItemSettings &settings, int currentPlayCount);
QString playlistMapStopReasonText(PlaylistMapStopReason reason);
const QIcon &statusIcon(StatusIconState state);
QIcon createPlaylistViewModeIcon(bool tileView);
} // namespace playlist_view
