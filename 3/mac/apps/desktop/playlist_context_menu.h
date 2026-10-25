#pragma once

#include <QObject>
#include <QPoint>

class QWidget;
class PlaylistModel;
class PlaylistItemOperations;
class ControllerOperations;

class PlaylistContextMenu final : public QObject
{
public:
    PlaylistContextMenu(PlaylistModel *model, PlaylistItemOperations *itemOperations,
                        ControllerOperations *operations, QWidget *parent);
    void showItemContextMenu(quint64 itemId, const QPoint &globalPosition);
private:
    void showPlaylistMap(quint64 itemId);
    void renameItemDisplayName(quint64 itemId);
    PlaylistModel *model_;
    PlaylistItemOperations *itemOperations_;
    ControllerOperations *operations_;
    QWidget *parentWidget_;
};
