#pragma once

#include "playlist_item.h"

#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>
#include "controller_operations.h"
#include "playlist_item_operations.h"

#include <optional>

class QMenu;
class QLabel;
class QImage;
class QListWidget;
class QListWidgetItem;
class PlaylistThumbnailStore;
class PlaylistContextMenu;
namespace vtrpon {
class NetworkTimeClock;
}
class PlaylistModel;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QTabBar;
class QTimer;
class QToolButton;
class UserInfoCenter;
class QEvent;

enum class PlaylistEditActivationMode : int {
    EditButton = 0,
    Selection = 1,
};

class PlaylistPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit PlaylistPanel(PlaylistModel *model, QWidget *parent = nullptr,
                           bool thumbnailsEnabled = true,
                           UserInfoCenter *userInfoCenter = nullptr,
                           bool synchronizeNetworkTime = false,
                           ControllerOperations *operations = nullptr);

    QMenu *bulkOperationsMenu() const { return bulkMenu_; }
    int itemCount() const;
    quint64 selectedItemId() const;
    QString selectedFilePath() const;
    std::optional<PlaylistItemState> selectedItemState() const;
    std::optional<PlaylistItemState> itemState(quint64 itemId) const;
    quint64 onAirItemId() const;
    void setOnAirToggleMode(bool enabled);
    bool crossfadeEnabled() const;
    qint64 crossfadeDurationMs() const;
    void setOnAirHighlightOpacity(quint64 itemId, qreal opacity);
    void setItemThumbnail(quint64 itemId, const QImage &image,
                          const QString &errorDetail = QString());
    void setShortcutLabels(const QString &editShortcut,
                           const QString &onAirShortcut);
    bool copySelectedItemState();
    bool pasteSelectedItemState();
    void setBulkUndoAvailable(bool available);
    void setEditActivationMode(PlaylistEditActivationMode mode);
    PlaylistEditActivationMode editActivationMode() const;
    bool selectNextItemAfter(quint64 itemId);
    bool allowItemAction(quint64 itemId, PlaylistItemAction action);
    vtrpon::NetworkTimeClock *networkTimeClock() const;
    void applyTimeSettings();
    void focusAddFilesButton();

signals:
    void addRequested();
    void moveUpRequested(quint64 itemId);
    void moveDownRequested(quint64 itemId);
    void moveRequested(quint64 itemId, int targetRow);
    void itemDropRequested(quint64 itemId, int targetPlaylistNumber,
                           int targetInsertionRow, bool copy);
    void filesDropped(const QStringList &paths);
    void removeRequested(quint64 itemId);
    void bulkOperationRequested(PlaylistBulkOperation operation);
    void onAirRequested();
    void offAirRequested();
    void selectionChanged();
    void editRequested();

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    ControllerOperations *operations_ = nullptr;
    bool onAirToggleMode_ = false;
    bool selectingThroughOperation_ = false;
    quint64 selectionEditRequestedId_ = 0;
    void createUi();
    void connectInputs();
    void bindSelectionOperations();
    void connectModel();
    void showBlockedAction(PlaylistItemAction action);
    void refreshThumbnail(quint64 itemId);

    void retranslateUi();
    void addVisibleItem(quint64 itemId);
    void rebuildActivePlaylist();
    void rebuildPlaylistTabs();
    void addPlaylistTab(int playlistNumber);
    int tabIndexForPlaylist(int playlistNumber) const;
    void setTileViewEnabled(bool enabled);
    void updateOnAirPlaylistTab();
    void updatePlaylistNameEditor();
    void updateItemOrder(int firstRow, int lastRow = -1);
    void updateSelectionControls();
    void updateBulkControls();
    bool canRemoveSelectedItem() const;
    quint64 relativeSelectionTarget(int offset) const;
    bool canApplyBulkOperation(PlaylistBulkOperation operation) const;
    void updateItemCount();
    void updateCurrentTime();
    void updateItemDisplay(quint64 itemId);
    void updateItemVisual(quint64 itemId);
    void updateUserInfo();

    PlaylistModel *model_ = nullptr;
    QLineEdit *playlistNameEdit_ = nullptr;
    QTabBar *playlistTabs_ = nullptr;
    QToolButton *listViewButton_ = nullptr;
    QToolButton *tileViewButton_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *itemCountLabel_ = nullptr;
    QPushButton *moveUpButton_ = nullptr;
    QPushButton *moveDownButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QMenu *bulkMenu_ = nullptr;
    QComboBox *bulkOperationComboBox_ = nullptr;
    QPushButton *bulkApplyButton_ = nullptr;
    QPushButton *editButton_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QCheckBox *crossfadeCheckBox_ = nullptr;
    QWidget *crossfadeDurationControl_ = nullptr;
    QDoubleSpinBox *crossfadeDurationSpinBox_ = nullptr;
    QToolButton *crossfadeDurationIncreaseButton_ = nullptr;
    QToolButton *crossfadeDurationDecreaseButton_ = nullptr;
    QPushButton *onAirButton_ = nullptr;
    QLabel *powerPointCueStatusLabel_ = nullptr;
    QHash<quint64, QListWidgetItem *> itemsById_;
    QHash<int, quint64> selectedItemByPlaylist_;
    PlaylistThumbnailStore *thumbnails_ = nullptr;
    PlaylistItemOperations *itemOperations_ = nullptr;
    PlaylistContextMenu *contextMenu_ = nullptr;
    UserInfoCenter *userInfoCenter_ = nullptr;
    vtrpon::NetworkTimeClock *networkTimeClock_ = nullptr;
    QWidget *clockRow_ = nullptr;
    QLabel *clockTitleLabel_ = nullptr;
    QLabel *clockValueLabel_ = nullptr;
    QTimer *clockTimer_ = nullptr;
    QString publishedUserInfoKey_;
    QString editShortcut_;
    QString onAirShortcut_;
    bool bulkUndoAvailable_ = false;
    PlaylistEditActivationMode editActivationMode_ =
        PlaylistEditActivationMode::EditButton;
};
