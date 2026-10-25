#include "playlist_input_widgets.h"
#include "playlist_view_roles.h"
#include "controller_scaling.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <utility>

using namespace playlist_view;
namespace {

bool hasPlaylistItemMime(const QMimeData *mimeData)
{
    return mimeData && mimeData->hasFormat(kPlaylistItemMimeType);
}

quint64 playlistItemIdFromMime(const QMimeData *mimeData)
{
    if (!hasPlaylistItemMime(mimeData)) {
        return 0;
    }
    bool valid = false;
    const quint64 itemId =
        mimeData->data(kPlaylistItemMimeType).toULongLong(&valid);
    return valid ? itemId : 0;
}

class PlaylistTabBar final : public QTabBar
{
public:
    using ItemDropCallback = std::function<void(quint64, int, bool)>;
    using QTabBar::QTabBar;

    void setItemDropCallback(ItemDropCallback callback)
    {
        itemDropCallback_ = std::move(callback);
    }

    void setOnAirPlaylistNumber(int playlistNumber)
    {
        if (onAirPlaylistNumber_ == playlistNumber) {
            return;
        }
        onAirPlaylistNumber_ = playlistNumber;
        setProperty("onAirPlaylistNumber", playlistNumber);
        update();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        pressedPlaylistNumber_ = event->button() == Qt::LeftButton
            ? tabData(tabAt(event->position().toPoint())).toInt() : 0;
        tabPressPosition_ = event->position().toPoint();
        tabDragging_ = false;
        tabDragTarget_ = -1;
        QTabBar::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (pressedPlaylistNumber_ > 0
            && event->buttons().testFlag(Qt::LeftButton)) {
            if (!tabDragging_ && (event->position().toPoint() - tabPressPosition_)
                    .manhattanLength() >= QApplication::startDragDistance()) {
                const int source = tabIndexForPlaylist(pressedPlaylistNumber_);
                if (source < 0) {
                    return;
                }
                tabDragPixmap_ = grab(tabRect(source));
                tabDragOffset_ = tabPressPosition_ - tabRect(source).topLeft();
                tabDragging_ = true;
                setCursor(Qt::ClosedHandCursor);
            }
            if (tabDragging_) {
                tabDragPosition_ = event->position().toPoint();
                tabDragTarget_ = numberedTabAt(tabDragPosition_);
                update();
                event->accept();
                return;
            }
        }
        QTabBar::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int from = tabIndexForPlaylist(pressedPlaylistNumber_);
        const int to = numberedTabAt(event->position().toPoint());
        const bool commit = tabDragging_ && event->button() == Qt::LeftButton
            && from >= 0 && to >= 0 && from != to;
        clearTabDrag();
        QTabBar::mouseReleaseEvent(event);
        if (commit) {
            moveTab(from, to);
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTabBar::paintEvent(event);
        paintOnAirTab();
        paintTabDrag();
    }

private:
    void clearTabDrag()
    {
        pressedPlaylistNumber_ = 0;
        tabDragging_ = false;
        tabDragTarget_ = -1;
        tabDragPixmap_ = QPixmap();
        unsetCursor();
        update();
    }

    void paintTabDrag()
    {
        const int from = tabIndexForPlaylist(pressedPlaylistNumber_);
        if (!tabDragging_ || from < 0) {
            return;
        }
        QPainter painter(this);
        painter.setOpacity(kDragPixmapOpacity);
        painter.drawPixmap(tabDragPosition_ - tabDragOffset_, tabDragPixmap_);
        painter.setOpacity(1.0);
        if (tabDragTarget_ < 0 || tabDragTarget_ == from) {
            return;
        }
        const QRect target = tabRect(tabDragTarget_);
        const bool after = tabDragTarget_ > from;
        const int x = (after != isRightToLeft()) ? target.right() : target.left();
        const int y1 = target.top() + vtrpon::scalePx(3);
        const int y2 = target.bottom() - vtrpon::scalePx(3);
        const int cap = vtrpon::scalePx(4);
        painter.setPen(QPen(QColor(0, 0, 0, 150), vtrpon::scalePx(5)));
        painter.drawLine(x, y1, x, y2);
        painter.setPen(QPen(QColor(QStringLiteral("#0096ff")), vtrpon::scalePx(3)));
        painter.drawLine(x, y1, x, y2);
        painter.drawLine(x - cap, y1, x + cap, y1);
        painter.drawLine(x - cap, y2, x + cap, y2);
    }

    void paintOnAirTab()
    {
        if (onAirPlaylistNumber_ <= 0) {
            return;
        }
        const int tabIndex = tabIndexForPlaylist(onAirPlaylistNumber_);
        if (tabIndex < 0) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect tab = tabRect(tabIndex).adjusted(1, 1, -2, -1);
        painter.setPen(QPen(QColor(QStringLiteral("#E7A0A5")), 1));
        painter.setBrush(QColor(QStringLiteral("#8C4A50")));
        painter.drawRoundedRect(tab, vtrpon::scalePx(4),
                                vtrpon::scalePx(4));

        QFont labelFont = font();
        labelFont.setBold(tabIndex == currentIndex());
        painter.setFont(labelFont);
        painter.setPen(QColor(QStringLiteral("#FFF4F4")));
        painter.drawText(tab, Qt::AlignCenter, tabText(tabIndex));
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        updateDragHover(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        updateDragHover(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event || !hasPlaylistItemMime(event->mimeData())) {
            if (event) {
                event->ignore();
            }
            return;
        }
        acceptDrag(event);
        const int tabIndex = numberedTabAt(event->position().toPoint());
        const int playlistNumber = tabIndex >= 0
            ? tabData(tabIndex).toInt()
            : 0;
        const quint64 itemId = playlistItemIdFromMime(event->mimeData());
        if (playlistNumber <= 0 || itemId == 0 || !itemDropCallback_) {
            event->ignore();
            return;
        }
        if (currentIndex() != tabIndex) {
            setCurrentIndex(tabIndex);
        }
        itemDropCallback_(
            itemId, playlistNumber,
            event->modifiers().testFlag(Qt::ControlModifier));
    }

private:
    int tabIndexForPlaylist(int playlistNumber) const
    {
        for (int tabIndex = 0; tabIndex < count(); ++tabIndex) {
            if (tabData(tabIndex).toInt() == playlistNumber) {
                return tabIndex;
            }
        }
        return -1;
    }

    template <typename DragEvent>
    static void acceptDrag(DragEvent *event)
    {
        event->setDropAction(event->modifiers().testFlag(Qt::ControlModifier)
                                 ? Qt::CopyAction
                                 : Qt::MoveAction);
        event->accept();
    }

    int numberedTabAt(const QPoint &position) const
    {
        const int tabIndex = tabAt(position);
        return tabIndex >= 0 && tabData(tabIndex).toInt() > 0
            ? tabIndex
            : -1;
    }

    void scheduleTabSwitch(int tabIndex)
    {
        if (tabIndex < 0) {
            return;
        }
        pendingTabIndex_ = tabIndex;
        if (tabSwitchScheduled_) {
            return;
        }
        tabSwitchScheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            tabSwitchScheduled_ = false;
            const int tabIndex = std::exchange(pendingTabIndex_, -1);
            if (tabIndex >= 0 && tabIndex < count() &&
                tabData(tabIndex).toInt() > 0 && currentIndex() != tabIndex) {
                setCurrentIndex(tabIndex);
            }
        });
    }

    template <typename DragEvent>
    void updateDragHover(DragEvent *event)
    {
        if (!event || !hasPlaylistItemMime(event->mimeData())) {
            if (event) {
                event->ignore();
            }
            return;
        }
        acceptDrag(event);
        scheduleTabSwitch(numberedTabAt(event->position().toPoint()));
    }

    ItemDropCallback itemDropCallback_;
    int pressedPlaylistNumber_ = 0;
    QPoint tabPressPosition_;
    QPoint tabDragPosition_;
    QPoint tabDragOffset_;
    QPixmap tabDragPixmap_;
    int tabDragTarget_ = -1;
    bool tabDragging_ = false;
    int onAirPlaylistNumber_ = 0;
    int pendingTabIndex_ = -1;
    bool tabSwitchScheduled_ = false;
};

class PlaylistListWidget final : public QListWidget
{
public:
    using ItemDropCallback = std::function<void(quint64, int, bool)>;
    using FilesCallback = std::function<void(const QStringList &)>;
    using ContextMenuCallback = std::function<void(quint64, const QPoint &)>;
    using TileActivateCallback = std::function<void(quint64)>;

    explicit PlaylistListWidget(QWidget *parent = nullptr)
        : QListWidget(parent)
    {
        setProperty("dropMarkerGap", -1);
        setProperty("dragPixmapOpacity", kDragPixmapOpacity);
        setProperty("pressedTileItemId", QVariant::fromValue<qulonglong>(0));
        setProperty("tileSelectOnlyGesture", false);
    }

    void setItemDropCallback(ItemDropCallback callback)
    {
        itemDropCallback_ = std::move(callback);
    }

    void setFilesCallback(FilesCallback callback)
    {
        filesCallback_ = std::move(callback);
    }

    void setContextMenuCallback(ContextMenuCallback callback)
    {
        contextMenuCallback_ = std::move(callback);
    }

    void setTileActivateCallback(TileActivateCallback callback)
    {
        tileActivateCallback_ = std::move(callback);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        clearPressedTile();
        tilePressEligible_ = false;
        const bool tilePress = event
            && viewMode() == QListView::IconMode
            && event->button() == Qt::LeftButton;
        QListWidgetItem *target = tilePress
            ? itemAt(event->position().toPoint()) : nullptr;
        const quint64 itemId = target
            ? target->data(kItemIdRole).toULongLong() : 0;
        if (itemId != 0) {
            pressedTileItemId_ = itemId;
            tilePressPosition_ = event->position().toPoint();
            tilePressEligible_ = true;
            setProperty("pressedTileItemId",
                        QVariant::fromValue<qulonglong>(itemId));
            setProperty(
                "tileSelectOnlyGesture",
                event->modifiers().testFlag(Qt::ControlModifier));
            viewport()->update(visualItemRect(target));
        }
        QListWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (tilePressEligible_ && event
            && (event->position().toPoint() - tilePressPosition_)
                   .manhattanLength() >= QApplication::startDragDistance()) {
            tilePressEligible_ = false;
            clearPressedTile();
        }
        QListWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool leftRelease = event && event->button() == Qt::LeftButton;
        QListWidgetItem *target = leftRelease
            ? itemAt(event->position().toPoint()) : nullptr;
        const quint64 targetId = target
            ? target->data(kItemIdRole).toULongLong() : 0;
        const bool selectOnly = leftRelease
            && event->modifiers().testFlag(Qt::ControlModifier);
        const bool activate = tilePressEligible_
            && viewMode() == QListView::IconMode
            && targetId != 0 && targetId == pressedTileItemId_
            && !selectOnly;

        QListWidget::mouseReleaseEvent(event);
        if (selectOnly && target) {
            setCurrentItem(target, QItemSelectionModel::ClearAndSelect);
        }
        clearPressedTile();
        setProperty("tileSelectOnlyGesture", false);
        tilePressEligible_ = false;
        if (activate && tileActivateCallback_) {
            tileActivateCallback_(targetId);
        }
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!event || !contextMenuCallback_) {
            QListWidget::contextMenuEvent(event);
            return;
        }
        QListWidgetItem *target = itemAt(event->pos());
        if (!target && event->reason() == QContextMenuEvent::Keyboard) {
            target = currentItem();
        }
        const quint64 itemId = target
            ? target->data(kItemIdRole).toULongLong() : 0;
        if (!target || itemId == 0) {
            event->ignore();
            return;
        }
        setCurrentItem(target);
        contextMenuCallback_(itemId, event->globalPos());
        event->accept();
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)
        tilePressEligible_ = false;
        clearPressedTile();
        setProperty("tileSelectOnlyGesture", false);
        QListWidgetItem *draggedItem = currentItem();
        const quint64 itemId = draggedItem
            ? draggedItem->data(kItemIdRole).toULongLong()
            : 0;
        if (!draggedItem || itemId == 0) {
            return;
        }

        const QRect itemRect = visualItemRect(draggedItem);
        const QPixmap sourcePixmap = viewport()->grab(itemRect);
        QPixmap dragPixmap(sourcePixmap.size());
        dragPixmap.setDevicePixelRatio(sourcePixmap.devicePixelRatio());
        dragPixmap.fill(Qt::transparent);
        {
            QPainter painter(&dragPixmap);
            painter.setOpacity(kDragPixmapOpacity);
            painter.drawPixmap(0, 0, sourcePixmap);
        }

        auto *mimeData = new QMimeData;
        mimeData->setData(kPlaylistItemMimeType, QByteArray::number(itemId));
        QDrag drag(this);
        drag.setMimeData(mimeData);
        drag.setPixmap(dragPixmap);
        drag.setHotSpot(viewport()->mapFromGlobal(QCursor::pos())
                        - itemRect.topLeft());
        const Qt::DropAction defaultAction =
            QApplication::keyboardModifiers().testFlag(Qt::ControlModifier)
            ? Qt::CopyAction
            : Qt::MoveAction;
        (void)drag.exec(Qt::MoveAction | Qt::CopyAction, defaultAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (isPlaylistReorderDrag(event)) {
            setDropMarkerGap(insertionGapAt(event->position().toPoint()));
            event->setDropAction(
                event->modifiers().testFlag(Qt::ControlModifier)
                    ? Qt::CopyAction
                    : Qt::MoveAction);
            event->accept();
            return;
        }
        clearDropMarker();
        if (hasLocalFiles(event->mimeData())) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (isPlaylistReorderDrag(event)) {
            setDropMarkerGap(insertionGapAt(event->position().toPoint()));
            event->setDropAction(
                event->modifiers().testFlag(Qt::ControlModifier)
                    ? Qt::CopyAction
                    : Qt::MoveAction);
            event->accept();
            return;
        }
        clearDropMarker();
        if (hasLocalFiles(event->mimeData())) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        clearDropMarker();
        QListWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (isPlaylistReorderDrag(event)) {
            const int targetInsertionRow = insertionGapAt(
                event->position().toPoint());
            const quint64 itemId = playlistItemIdFromMime(event->mimeData());
            const bool copy =
                event->modifiers().testFlag(Qt::ControlModifier);
            clearDropMarker();
            if (itemId == 0 || !itemDropCallback_) {
                event->ignore();
                return;
            }
            itemDropCallback_(itemId, targetInsertionRow, copy);
            event->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
            event->accept();
            return;
        }

        const QStringList paths = localFiles(event->mimeData());
        clearDropMarker();
        if (paths.isEmpty()) {
            event->ignore();
            return;
        }
        if (filesCallback_) {
            filesCallback_(paths);
        }
        event->acceptProposedAction();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QListWidget::paintEvent(event);
        if (dropMarkerGap_ < 0 || count() == 0) {
            return;
        }

        const QColor markerColor(QStringLiteral("#0096ff"));
        if (viewMode() == QListView::IconMode) {
            QRect referenceRect;
            int markerX = 0;
            if (dropMarkerGap_ <= 0) {
                referenceRect = visualItemRect(item(0));
                markerX = referenceRect.left();
            } else if (dropMarkerGap_ >= count()) {
                referenceRect = visualItemRect(item(count() - 1));
                markerX = referenceRect.right() + 1;
            } else {
                const QRect before =
                    visualItemRect(item(dropMarkerGap_ - 1));
                const QRect after = visualItemRect(item(dropMarkerGap_));
                referenceRect = after;
                markerX = qAbs(before.center().y() - after.center().y())
                        <= vtrpon::scalePx(2)
                    ? (before.right() + after.left()) / 2
                    : after.left();
            }
            markerX = std::clamp(markerX, kDropMarkerRadius + 1,
                                 viewport()->width() - kDropMarkerRadius - 1);
            const int markerTop = (std::max)(
                kDropMarkerRadius + 1,
                referenceRect.top() + vtrpon::scalePx(5));
            const int markerBottom = (std::min)(
                viewport()->height() - kDropMarkerRadius - 1,
                referenceRect.bottom() - vtrpon::scalePx(5));

            QPainter painter(viewport());
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(0, 0, 0, 150),
                                kDropMarkerLineWidth + 4,
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(markerX, markerTop, markerX, markerBottom);
            painter.setPen(QPen(markerColor, kDropMarkerLineWidth,
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(markerX, markerTop, markerX, markerBottom);
            painter.setPen(QPen(markerColor.lighter(165), 2));
            painter.setBrush(markerColor);
            painter.drawEllipse(QPoint(markerX, markerTop),
                                kDropMarkerRadius, kDropMarkerRadius);
            return;
        }

        const int markerY = dropMarkerY(dropMarkerGap_);
        const int markerLeft = kDropMarkerRadius + 5;
        const int markerRight = viewport()->width() - 8;

        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(0, 0, 0, 150),
                            kDropMarkerLineWidth + 4,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(markerLeft, markerY, markerRight, markerY);
        painter.setPen(QPen(markerColor, kDropMarkerLineWidth,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(markerLeft, markerY, markerRight, markerY);
        painter.setPen(QPen(markerColor.lighter(165), 2));
        painter.setBrush(markerColor);
        painter.drawEllipse(QPoint(kDropMarkerRadius + 2, markerY),
                            kDropMarkerRadius, kDropMarkerRadius);
    }

private:
    void clearPressedTile()
    {
        if (pressedTileItemId_ == 0) {
            return;
        }
        pressedTileItemId_ = 0;
        setProperty("pressedTileItemId", QVariant::fromValue<qulonglong>(0));
        viewport()->update();
    }

    template <typename DragEvent>
    bool isPlaylistReorderDrag(const DragEvent *event) const
    {
        return event &&
            (hasPlaylistItemMime(event->mimeData()) ||
             event->source() == this ||
             (event->mimeData() &&
              event->mimeData()->hasFormat(
                  QStringLiteral("application/x-qabstractitemmodeldatalist"))));
    }

    int insertionGapAt(const QPoint &position) const
    {
        if (viewMode() == QListView::IconMode) {
            for (int row = 0; row < count(); ++row) {
                const QRect itemRect = visualItemRect(item(row));
                if (position.y() < itemRect.top()) {
                    return row;
                }
                if (position.y() <= itemRect.bottom()
                    && position.x() < itemRect.center().x()) {
                    return row;
                }
                if (position.y() <= itemRect.bottom()
                    && position.x() <= itemRect.right()) {
                    return row + 1;
                }
            }
            return count();
        }
        for (int row = 0; row < count(); ++row) {
            const QRect itemRect = visualItemRect(item(row));
            if (position.y() < itemRect.center().y()) {
                return row;
            }
        }
        return count();
    }

    int dropMarkerY(int gap) const
    {
        int markerY = 0;
        if (gap <= 0) {
            markerY = visualItemRect(item(0)).top();
        } else if (gap >= count()) {
            markerY = visualItemRect(item(count() - 1)).bottom() + 1;
        } else {
            const QRect before = visualItemRect(item(gap - 1));
            const QRect after = visualItemRect(item(gap));
            markerY = (before.bottom() + after.top()) / 2;
        }
        return std::clamp(markerY, kDropMarkerRadius + 1,
                          viewport()->height() - kDropMarkerRadius - 1);
    }

    void setDropMarkerGap(int gap)
    {
        const int boundedGap = std::clamp(gap, 0, count());
        if (dropMarkerGap_ == boundedGap) {
            return;
        }
        dropMarkerGap_ = boundedGap;
        setProperty("dropMarkerGap", dropMarkerGap_);
        viewport()->update();
    }

    void clearDropMarker()
    {
        if (dropMarkerGap_ < 0) {
            return;
        }
        dropMarkerGap_ = -1;
        setProperty("dropMarkerGap", -1);
        viewport()->update();
    }

    static QStringList localFiles(const QMimeData *mimeData)
    {
        QStringList paths;
        if (!mimeData || !mimeData->hasUrls()) {
            return paths;
        }
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
        return paths;
    }

    static bool hasLocalFiles(const QMimeData *mimeData)
    {
        return !localFiles(mimeData).isEmpty();
    }

    ItemDropCallback itemDropCallback_;
    FilesCallback filesCallback_;
    ContextMenuCallback contextMenuCallback_;
    TileActivateCallback tileActivateCallback_;
    QPoint tilePressPosition_;
    quint64 pressedTileItemId_ = 0;
    bool tilePressEligible_ = false;
    int dropMarkerGap_ = -1;
};

} // namespace

QListWidget *createPlaylistListWidget(QWidget *parent)
{
    return new PlaylistListWidget(parent);
}

QTabBar *createPlaylistTabBar(QWidget *parent)
{
    return new PlaylistTabBar(parent);
}

void connectPlaylistInputWidgets(QListWidget *list, QTabBar *tabs,
                                 PlaylistInputCallbacks callbacks)
{
    auto *inputList = static_cast<PlaylistListWidget *>(list);
    inputList->setItemDropCallback(std::move(callbacks.itemDrop));
    inputList->setFilesCallback(std::move(callbacks.filesDrop));
    inputList->setContextMenuCallback(std::move(callbacks.contextMenu));
    inputList->setTileActivateCallback(std::move(callbacks.tileActivate));
    static_cast<PlaylistTabBar *>(tabs)->setItemDropCallback(std::move(callbacks.tabDrop));
}

void setPlaylistOnAirTab(QTabBar *tabs, int playlistNumber)
{
    static_cast<PlaylistTabBar *>(tabs)->setOnAirPlaylistNumber(playlistNumber);
}
