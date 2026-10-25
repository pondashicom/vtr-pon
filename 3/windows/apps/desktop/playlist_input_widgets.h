#pragma once

#include <QPoint>
#include <QStringList>
#include <functional>

class QWidget;
class QListWidget;
class QTabBar;

// Item ids, insertion gaps and gesture intent are the boundary to the Panel.
// These widgets do not decide whether a playback/model operation is permitted.
struct PlaylistInputCallbacks {
    std::function<void(quint64, int, bool)> itemDrop;
    std::function<void(const QStringList &)> filesDrop;
    std::function<void(quint64, const QPoint &)> contextMenu;
    std::function<void(quint64)> tileActivate;
    std::function<void(quint64, int, bool)> tabDrop;
};

QListWidget *createPlaylistListWidget(QWidget *parent);
QTabBar *createPlaylistTabBar(QWidget *parent);
void connectPlaylistInputWidgets(QListWidget *list, QTabBar *tabs,
                                 PlaylistInputCallbacks callbacks);
void setPlaylistOnAirTab(QTabBar *tabs, int playlistNumber);
