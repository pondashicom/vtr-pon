#pragma once

#include "playlist_item.h"
#include <QObject>
#include <optional>

class ControllerOperations;
class PlaylistModel;

enum class PlaylistBulkOperation {
    PlayOnce,
    RepeatAll,
    Shuffle,
    BgmMode,
    StandardMode,
    Clear,
    Undo,
    DeletePlaylist,
};

enum class PlaylistItemAction {
    OnAir,
    Edit,
    Dsk,
    CopyItem,
    CopyState,
    PasteState,
};

// No QWidget, selection, mouse event, or dialog dependency.
class PlaylistItemOperations final : public QObject
{
    Q_OBJECT
public:
    PlaylistItemOperations(PlaylistModel *model, ControllerOperations *operations,
                           QObject *parent = nullptr);
    bool allowItemAction(quint64 itemId, PlaylistItemAction action);
    bool copyItemState(quint64 itemId);
    bool pasteItemState(quint64 itemId);
    bool hasCopiedState() const { return copiedItemState_.has_value(); }

signals:
    void actionBlocked(PlaylistItemAction action);

private:
    struct CopiedItemState {
        PlaylistStartMode startMode = PlaylistStartMode::Pause;
        PlaylistEndMode endMode = PlaylistEndMode::Pause;
        bool fadeInEnabled = false;
        bool fadeOutEnabled = false;
        bool endCrossfadeEnabled = false;
    };
    PlaylistModel *model_;
    std::optional<CopiedItemState> copiedItemState_;
};
