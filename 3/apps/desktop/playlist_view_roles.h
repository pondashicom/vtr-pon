#pragma once

#include <QString>

namespace playlist_view {

constexpr int kItemIdRole = Qt::UserRole;
constexpr int kStatusIconStateRole = Qt::UserRole + 1;
constexpr int kThumbnailRole = Qt::UserRole + 2;
constexpr int kThumbnailErrorRole = Qt::UserRole + 3;
constexpr int kItemVisualStateRole = Qt::UserRole + 4;
constexpr int kOnAirHighlightOpacityRole = Qt::UserRole + 5;
constexpr int kItemNumberRole = Qt::UserRole + 6;
constexpr int kItemNameRole = Qt::UserRole + 7;
constexpr int kItemInPointRole = Qt::UserRole + 8;
constexpr int kItemOutPointRole = Qt::UserRole + 9;
constexpr int kItemMediaTypeRole = Qt::UserRole + 10;
constexpr int kItemVolumeRole = Qt::UserRole + 11;
constexpr int kItemStartModeRole = Qt::UserRole + 12;
constexpr int kItemEndModeRole = Qt::UserRole + 13;
constexpr int kItemLiveRole = Qt::UserRole + 14;
constexpr int kItemColorRole = Qt::UserRole + 15;
constexpr int kItemSeparatorBelowRole = Qt::UserRole + 16;
constexpr int kItemMediaOfflineRole = Qt::UserRole + 17;
constexpr int kPowerPointModeRole = Qt::UserRole + 18;
constexpr int kItemConversionActiveRole = Qt::UserRole + 19;
constexpr int kItemConversionProgressRole = Qt::UserRole + 20;
constexpr int kItemConversionStageRole = Qt::UserRole + 21;
constexpr int kItemBgmModeRole = Qt::UserRole + 22;
constexpr int kItemResolutionRole = Qt::UserRole + 23;
constexpr int kItemDurationRole = Qt::UserRole + 24;
constexpr int kPlaylistPanelWidth = 664;
constexpr int kThumbnailWidth = 120;
constexpr int kThumbnailHeight = 68;
constexpr int kPlaylistItemHeight = 88;
constexpr int kItemNumberWidth = 32;
constexpr int kItemModeColumnWidth = 168;
constexpr int kPlaylistTileWidth = 150;
constexpr int kPlaylistTileHeight = 120;
constexpr int kDropMarkerRadius = 7;
constexpr int kDropMarkerLineWidth = 5;
constexpr int kCrossfadeDurationWidth = 41;
constexpr int kCrossfadeDurationStepButtonWidth = 18;
constexpr int kCrossfadeDurationStepButtonHeight = 12;
constexpr int kBulkOperationComboBoxWidth = 210;
constexpr int kBulkApplyButtonWidth = 56;
constexpr qreal kDragPixmapOpacity = 0.60;
const QString kPlaylistItemMimeType =
    QStringLiteral("application/x-vtrpon-playlist-item");

enum class StatusIconState { None, CuePending, CueReady, CueError, OnAir };

enum ItemVisualState {
    ItemVisualNone = 0,
    ItemVisualOnAir = 1 << 0,
    ItemVisualDsk = 1 << 1,
    ItemVisualEdit = 1 << 2,
    ItemVisualBgmPlay = 1 << 3,
};

} // namespace playlist_view
