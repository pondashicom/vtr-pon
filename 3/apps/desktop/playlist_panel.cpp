#include "network_video_source.h"
#include "playlist_panel.h"
#include "controller_button_focus.h"
#include <QMenu>
#include <QAction>
#include <QScopedValueRollback>

#include "playlist_thumbnail_store.h"
#include "playlist_item_delegate.h"
#include "playlist_input_widgets.h"
#include "playlist_item_operations.h"
#include "playlist_context_menu.h"
#include "network_time_clock.h"
#include "time_settings.h"
#include "playlist_item_display.h"
#include "playlist_model.h"
#include "language_text.h"
#include "user_info.h"
#include "controller_scaling.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QModelIndex>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSizePolicy>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace playlist_view;

namespace {

struct SpinBoxEditorWidgets {
    QWidget *control = nullptr;
    QToolButton *increaseButton = nullptr;
    QToolButton *decreaseButton = nullptr;
};

SpinBoxEditorWidgets createSpinBoxEditor(QAbstractSpinBox *spinBox,
                                         const QString &objectNamePrefix,
                                         QWidget *parent)
{
    auto *control = new QWidget(parent);
    control->setObjectName(objectNamePrefix + QStringLiteral("Control"));
    auto *layout = new QHBoxLayout(control);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(vtrpon::scalePx(1));

    spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    layout->addWidget(spinBox);

    auto *buttonColumn = new QWidget(control);
    auto *buttonColumnLayout = new QVBoxLayout(buttonColumn);
    buttonColumnLayout->setContentsMargins(0, 0, 0, 0);
    buttonColumnLayout->setSpacing(0);
    const auto createStepButton = [buttonColumn](Qt::ArrowType arrowType,
                                                  const QString &objectName) {
        auto *button = new QToolButton(buttonColumn);
        button->setObjectName(objectName);
        button->setProperty("fadeDurationStepButton", true);
        button->setArrowType(arrowType);
        button->setAutoRepeat(true);
        button->setAutoRepeatDelay(400);
        button->setAutoRepeatInterval(80);
        button->setFixedSize(vtrpon::scaleSize(
            kCrossfadeDurationStepButtonWidth,
            kCrossfadeDurationStepButtonHeight));
        button->setFocusPolicy(Qt::StrongFocus);
        return button;
    };
    auto *increaseButton = createStepButton(
        Qt::UpArrow, objectNamePrefix + QStringLiteral("IncreaseButton"));
    auto *decreaseButton = createStepButton(
        Qt::DownArrow, objectNamePrefix + QStringLiteral("DecreaseButton"));
    buttonColumnLayout->addWidget(increaseButton);
    buttonColumnLayout->addWidget(decreaseButton);
    layout->addWidget(buttonColumn);

    QObject::connect(increaseButton, &QToolButton::clicked, spinBox,
                     &QAbstractSpinBox::stepUp);
    QObject::connect(decreaseButton, &QToolButton::clicked, spinBox,
                     &QAbstractSpinBox::stepDown);
    return {control, increaseButton, decreaseButton};
}

void retranslateDurationButtons(QDoubleSpinBox *spinBox,
                                QToolButton *increaseButton,
                                QToolButton *decreaseButton)
{
    const QString increaseText =
        vtrpon::uiText("edit.duration.increase.accessible")
            .arg(spinBox->accessibleName());
    const QString decreaseText =
        vtrpon::uiText("edit.duration.decrease.accessible")
            .arg(spinBox->accessibleName());
    increaseButton->setAccessibleName(increaseText);
    increaseButton->setToolTip(increaseText);
    decreaseButton->setAccessibleName(decreaseText);
    decreaseButton->setToolTip(decreaseText);
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

PlaylistPanel::PlaylistPanel(PlaylistModel *model, QWidget *parent,
                             bool thumbnailsEnabled,
                             UserInfoCenter *userInfoCenter,
                             bool synchronizeNetworkTime,
                             ControllerOperations *operations)
    : QWidget(parent)
    , model_(model)
    , playlistNameEdit_(new QLineEdit(this))
    , playlistTabs_(createPlaylistTabBar(this))
    , listViewButton_(new QToolButton(this))
    , tileViewButton_(new QToolButton(this))
    , list_(createPlaylistListWidget(this))
    , itemCountLabel_(new QLabel(this))
    , moveUpButton_(new QPushButton(QStringLiteral("▲"), this))
    , moveDownButton_(new QPushButton(QStringLiteral("▼"), this))
    , removeButton_(new QPushButton(vtrpon::uiText("playlist.delete"), this))
    , bulkMenu_(new QMenu(this))
    , bulkOperationComboBox_(new QComboBox(this))
    , bulkApplyButton_(new QPushButton(this))
    , editButton_(new QPushButton(vtrpon::uiText("info.source.edit"), this))
    , addButton_(new QPushButton(vtrpon::uiText("playlist.add_files"), this))
    , crossfadeCheckBox_(new QCheckBox(vtrpon::uiText("playlist.crossfade"), this))
    , crossfadeDurationSpinBox_(new QDoubleSpinBox(this))
    , onAirButton_(new QPushButton(vtrpon::uiText("playlist.on_air"), this))
    , userInfoCenter_(userInfoCenter)
    , networkTimeClock_(synchronizeNetworkTime
                            ? new vtrpon::NetworkTimeClock(this)
                            : nullptr)
{
    operations_ = operations ? operations : new ControllerOperations(this);
    itemOperations_ = new PlaylistItemOperations(model_, operations_, this);
    thumbnails_ = new PlaylistThumbnailStore(model_, thumbnailsEnabled, this);
    contextMenu_ = new PlaylistContextMenu(model_, itemOperations_, operations_, this);
    createUi();
    bindSelectionOperations();
    connectInputs();
    connectModel();
    connect(itemOperations_, &PlaylistItemOperations::actionBlocked,
            this, &PlaylistPanel::showBlockedAction);
    connect(thumbnails_, &PlaylistThumbnailStore::thumbnailChanged,
            this, &PlaylistPanel::refreshThumbnail);
    rebuildActivePlaylist();
    updateItemCount();
    updateSelectionControls();
    retranslateUi();
    vtrpon::preserveFocusOnButtonClick(this);
    vtrpon::preserveFocusOnComboBoxClick(this);
    qApp->installEventFilter(this);
}

void PlaylistPanel::focusAddFilesButton()
{
    addButton_->setFocus(Qt::OtherFocusReason);
}

bool PlaylistPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::ShortcutOverride &&
        event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);

    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != window() || !isVisible() ||
        !isEnabled() || QApplication::activeModalWidget() ||
        QApplication::activePopupWidget())
        return QWidget::eventFilter(watched, event);

    auto *key = static_cast<QKeyEvent *>(event);
    if (key->modifiers() != Qt::NoModifier ||
        (key->key() != Qt::Key_Up && key->key() != Qt::Key_Down))
        return QWidget::eventFilter(watched, event);

    // Plain arrows belong to playlist selection throughout the Controller.
    // Consume them even at a boundary or with no items, before Qt moves focus.
    event->accept();
    if (event->type() == QEvent::ShortcutOverride)
        return true;

    (void)operations_->executeFrom("Shortcut", ControllerOperations::Action::SelectRelativeItem,
                                   key->key() == Qt::Key_Down ? 1 : -1);
    list_->setFocus(Qt::OtherFocusReason);
    if (auto *selected = itemsById_.value(selectedItemId(), nullptr))
        list_->scrollToItem(selected);
    return true;
}

void PlaylistPanel::createUi()
{
    setObjectName(QStringLiteral("playlistPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(vtrpon::scalePx(kPlaylistPanelWidth));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(vtrpon::uiText("playlist.title"), this);
    title->setObjectName(QStringLiteral("playlistTitle"));
    itemCountLabel_->setObjectName(QStringLiteral("playlistItemCount"));
    itemCountLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    titleRow->addWidget(title);
    titleRow->addStretch();
    titleRow->addWidget(itemCountLabel_);

    auto *playlistNameLabel = new QLabel(vtrpon::uiText("playlist.name.label"), this);
    playlistNameLabel->setObjectName(QStringLiteral("playlistNameLabel"));
    playlistNameEdit_->setObjectName(QStringLiteral("playlistNameEdit"));
    playlistNameEdit_->setAccessibleName(vtrpon::uiText("playlist.name.accessible"));
    playlistNameEdit_->setPlaceholderText(vtrpon::uiText("playlist.name.placeholder"));
    playlistNameEdit_->setClearButtonEnabled(true);
    auto *playlistNameRow = new QHBoxLayout;
    playlistNameRow->setContentsMargins(0, 0, 0, 0);
    playlistNameRow->setSpacing(vtrpon::scalePx(6));
    playlistNameRow->addWidget(playlistNameLabel);
    playlistNameRow->addWidget(playlistNameEdit_, 1);

    playlistTabs_->setObjectName(QStringLiteral("playlistTabs"));
    playlistTabs_->setAccessibleName(vtrpon::uiText("playlist.tabs.accessible"));
    playlistTabs_->setDocumentMode(true);
    playlistTabs_->setDrawBase(false);
    playlistTabs_->setExpanding(false);
    playlistTabs_->setUsesScrollButtons(true);
    playlistTabs_->setElideMode(Qt::ElideNone);
    playlistTabs_->setAcceptDrops(true);
    playlistTabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Resolve the scaled style before QTabBar caches its first tab sizes.
    playlistTabs_->ensurePolished();
    rebuildPlaylistTabs();

    listViewButton_->setObjectName(QStringLiteral("playlistListViewButton"));
    listViewButton_->setIcon(createPlaylistViewModeIcon(false));
    tileViewButton_->setObjectName(QStringLiteral("playlistTileViewButton"));
    tileViewButton_->setIcon(createPlaylistViewModeIcon(true));
    for (QToolButton *button : {listViewButton_, tileViewButton_}) {
        button->setProperty("playlistViewModeButton", true);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setAutoRaise(false);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setFixedSize(vtrpon::scaleSize(30, 23));
        button->setIconSize(vtrpon::scaleSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    listViewButton_->setChecked(true);

    auto *playlistTabsRow = new QHBoxLayout;
    playlistTabsRow->setContentsMargins(0, 0, 0, 0);
    playlistTabsRow->setSpacing(vtrpon::scalePx(4));
    playlistTabsRow->addWidget(playlistTabs_, 1);
    playlistTabsRow->addWidget(listViewButton_);
    playlistTabsRow->addWidget(tileViewButton_);

    list_->setObjectName(QStringLiteral("playlistList"));
    list_->setAccessibleName(vtrpon::uiText("playlist.items.accessible"));
    list_->setAccessibleDescription(vtrpon::uiText("playlist.list.description"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setDragEnabled(true);
    list_->setAcceptDrops(true);
    list_->viewport()->setAcceptDrops(true);
    list_->setDropIndicatorShown(false);
    list_->setDragDropMode(QAbstractItemView::DragDrop);
    list_->setDefaultDropAction(Qt::MoveAction);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setIconSize(QSize(12, 12));
    list_->setSpacing(3);
    list_->setItemDelegate(createPlaylistItemDelegate(list_));
    setTileViewEnabled(false);
    moveUpButton_->setObjectName(QStringLiteral("playlistMoveUpButton"));
    moveUpButton_->setAccessibleName(
        vtrpon::uiText("playlist.move_up.accessible"));
    moveUpButton_->setToolTip(vtrpon::uiText("playlist.move_up.tooltip"));
    moveDownButton_->setObjectName(QStringLiteral("playlistMoveDownButton"));
    moveDownButton_->setAccessibleName(
        vtrpon::uiText("playlist.move_down.accessible"));
    moveDownButton_->setToolTip(vtrpon::uiText("playlist.move_down.tooltip"));
    removeButton_->setObjectName(QStringLiteral("playlistDeleteButton"));
    removeButton_->setAccessibleName(
        vtrpon::uiText("playlist.delete.accessible"));
    removeButton_->setToolTip(vtrpon::uiText("playlist.delete.tooltip"));
    for (QPushButton *button
         : {moveUpButton_, moveDownButton_, removeButton_}) {
        button->setFixedSize(vtrpon::scaleSize(30, 22));
        button->setAutoDefault(false);
        button->setDefault(false);
    }
    bulkMenu_->setObjectName(QStringLiteral("playlistMenu"));
    bulkOperationComboBox_->setObjectName(QStringLiteral("playlistBulkOperationComboBox"));
    bulkOperationComboBox_->setFixedSize(vtrpon::scaleSize(kBulkOperationComboBoxWidth, 24));
    bulkOperationComboBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    bulkApplyButton_->setObjectName(QStringLiteral("playlistBulkApplyButton"));
    bulkApplyButton_->setFixedSize(vtrpon::scaleSize(kBulkApplyButtonWidth, 20));
    bulkApplyButton_->setAutoDefault(false);
    bulkApplyButton_->setDefault(false);
    connect(bulkOperationComboBox_, &QComboBox::currentIndexChanged,
            this, &PlaylistPanel::updateBulkControls);
    connect(bulkApplyButton_, &QPushButton::clicked, this, [this] {
        const auto operation = bulkOperationComboBox_->currentData();
        if (!operation.isValid()) return;
        for (auto *action : bulkMenu_->actions()) {
            if (action->data() == operation && action->isEnabled()) {
                action->trigger();
                break;
            }
        }
        bulkOperationComboBox_->setCurrentIndex(0);
    });
    auto *copyStateAction = bulkMenu_->addAction(QString());
    copyStateAction->setObjectName(QStringLiteral("playlistMenuCopyStateAction"));
    connect(copyStateAction, &QAction::triggered, this, [this] {
        (void)operations_->executeFrom("Menu", ControllerOperations::Action::CopySelectedState);
    });
    auto *pasteStateAction = bulkMenu_->addAction(QString());
    pasteStateAction->setObjectName(QStringLiteral("playlistMenuPasteStateAction"));
    connect(pasteStateAction, &QAction::triggered, this, [this] {
        (void)operations_->executeFrom("Menu", ControllerOperations::Action::PasteSelectedState);
    });
    connect(bulkMenu_, &QMenu::aboutToShow, this, &PlaylistPanel::updateBulkControls);
    bulkMenu_->addSeparator();

    for (auto operation : {PlaylistBulkOperation::PlayOnce, PlaylistBulkOperation::RepeatAll,
                           PlaylistBulkOperation::Shuffle, PlaylistBulkOperation::BgmMode,
                           PlaylistBulkOperation::StandardMode, PlaylistBulkOperation::Clear,
                           PlaylistBulkOperation::DeletePlaylist, PlaylistBulkOperation::Undo}) {
        if (operation == PlaylistBulkOperation::BgmMode || operation == PlaylistBulkOperation::Clear ||
            operation == PlaylistBulkOperation::Undo) bulkMenu_->addSeparator();
        auto *action = bulkMenu_->addAction(QString());
        action->setData(static_cast<int>(operation));
        connect(action, &QAction::triggered, this, [this, operation] {
            ControllerOperations::InputScope inputScope("Menu");
            emit bulkOperationRequested(operation);
        });
    }


    editButton_->setObjectName(QStringLiteral("playlistEditButton"));
    editButton_->setAccessibleName(
        vtrpon::uiText("playlist.edit.accessible"));
    editButton_->setToolTip(vtrpon::uiText("playlist.edit.tooltip"));
    addButton_->setObjectName(QStringLiteral("playlistAddButton"));
    addButton_->setAccessibleName(vtrpon::uiText("playlist.add.accessible"));
    addButton_->setToolTip(
        vtrpon::uiText("playlist.add.tooltip"));
    onAirButton_->setObjectName(QStringLiteral("playlistOnAirButton"));
    onAirButton_->setAccessibleName(vtrpon::uiText("playlist.on_air.accessible"));
    onAirButton_->setToolTip(vtrpon::uiText("playlist.on_air.tooltip"));
    crossfadeCheckBox_->setObjectName(QStringLiteral("playlistCrossfadeCheckBox"));
    crossfadeCheckBox_->setAccessibleName(vtrpon::uiText("playlist.crossfade.accessible"));
    crossfadeCheckBox_->setToolTip(vtrpon::uiText("playlist.crossfade.tooltip"));
    crossfadeDurationSpinBox_->setObjectName(
        QStringLiteral("playlistCrossfadeDurationSpinBox"));
    crossfadeDurationSpinBox_->setAccessibleName(
        vtrpon::uiText("playlist.crossfade_duration.accessible"));
    crossfadeDurationSpinBox_->setDecimals(1);
    crossfadeDurationSpinBox_->setRange(
        kMinimumItemFadeDurationMs / 1000.0,
        kMaximumItemFadeDurationMs / 1000.0);
    crossfadeDurationSpinBox_->setSingleStep(0.1);
    crossfadeDurationSpinBox_->setValue(
        model_->manualCrossfadeDurationMs() / 1000.0);
    crossfadeDurationSpinBox_->setSuffix(QStringLiteral(" s"));
    crossfadeDurationSpinBox_->setFixedSize(
        vtrpon::scaleSize(kCrossfadeDurationWidth, 24));
    crossfadeDurationSpinBox_->setKeyboardTracking(false);
    const SpinBoxEditorWidgets crossfadeDurationEditor = createSpinBoxEditor(
        crossfadeDurationSpinBox_, QStringLiteral("playlistCrossfadeDuration"),
        this);
    crossfadeDurationControl_ = crossfadeDurationEditor.control;
    crossfadeDurationIncreaseButton_ =
        crossfadeDurationEditor.increaseButton;
    crossfadeDurationDecreaseButton_ =
        crossfadeDurationEditor.decreaseButton;
    crossfadeDurationControl_->setEnabled(false);
    for (QPushButton *button : {editButton_, addButton_, onAirButton_}) {
        button->setFixedSize(vtrpon::scaleSize(100, 24));
    }

    auto *actionPanel = new QWidget(this);
    actionPanel->setObjectName(QStringLiteral("playlistActionPanel"));
    actionPanel->setAccessibleName(vtrpon::uiText("playlist.actions.accessible"));
    actionPanel->setAttribute(Qt::WA_StyledBackground, true);
    actionPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *buttonRow = new QHBoxLayout(actionPanel);
    buttonRow->setContentsMargins(vtrpon::scalePx(10),
                                  vtrpon::scalePx(12),
                                  vtrpon::scalePx(10),
                                  vtrpon::scalePx(12));
    buttonRow->setSpacing(vtrpon::scalePx(8));
    buttonRow->addWidget(editButton_);
    buttonRow->addWidget(addButton_);
    buttonRow->addStretch();
    buttonRow->addWidget(crossfadeCheckBox_);
    buttonRow->addWidget(crossfadeDurationControl_);
    powerPointCueStatusLabel_ = new QLabel(actionPanel);
    powerPointCueStatusLabel_->setObjectName(QStringLiteral("playlistPowerPointCueStatus"));
    powerPointCueStatusLabel_->hide();
    buttonRow->addWidget(powerPointCueStatusLabel_);
    buttonRow->addWidget(onAirButton_);

    setTabOrder(editButton_, addButton_);
    setTabOrder(addButton_, crossfadeCheckBox_);
    setTabOrder(crossfadeCheckBox_, crossfadeDurationSpinBox_);
    setTabOrder(crossfadeDurationSpinBox_, crossfadeDurationIncreaseButton_);
    setTabOrder(crossfadeDurationIncreaseButton_,
                crossfadeDurationDecreaseButton_);
    setTabOrder(crossfadeDurationDecreaseButton_, onAirButton_);

    auto *itemControlRow = new QHBoxLayout;
    itemControlRow->setContentsMargins(0, 0, 0, 0);
    itemControlRow->setSpacing(vtrpon::scalePx(3));
    itemControlRow->addWidget(moveUpButton_);
    itemControlRow->addWidget(moveDownButton_);
    itemControlRow->addWidget(removeButton_);
    itemControlRow->addStretch();
    itemControlRow->addWidget(bulkOperationComboBox_);
    itemControlRow->addWidget(bulkApplyButton_);

    setTabOrder(moveUpButton_, moveDownButton_);
    setTabOrder(moveDownButton_, removeButton_);
    setTabOrder(removeButton_, bulkOperationComboBox_);
    setTabOrder(bulkOperationComboBox_, bulkApplyButton_);
    setTabOrder(bulkApplyButton_, editButton_);

    clockRow_ = new QWidget(this);
    clockRow_->setObjectName(QStringLiteral("playlistClockRow"));
    clockRow_->setAttribute(Qt::WA_StyledBackground, true);
    clockRow_->setFixedHeight(vtrpon::scalePx(22));
    clockRow_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *clockLayout = new QHBoxLayout(clockRow_);
    clockLayout->setContentsMargins(vtrpon::scalePx(8), 0,
                                    vtrpon::scalePx(8), 0);
    clockLayout->setSpacing(vtrpon::scalePx(7));
    clockLayout->addStretch(1);

    clockTitleLabel_ = new QLabel(clockRow_);
    clockTitleLabel_->setObjectName(QStringLiteral("playlistClockTitle"));
    clockTitleLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    clockTitleLabel_->setFixedWidth(vtrpon::scalePx(96));
    clockTitleLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    clockLayout->addWidget(clockTitleLabel_);

    clockValueLabel_ = new QLabel(clockRow_);
    clockValueLabel_->setObjectName(QStringLiteral("playlistCurrentTime"));
    clockValueLabel_->setAlignment(Qt::AlignCenter);
    clockValueLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont clockFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    clockFont.setPointSizeF(12.0 * vtrpon::uiScale());
    clockFont.setWeight(QFont::DemiBold);
    clockValueLabel_->setFont(clockFont);
    clockValueLabel_->setMinimumWidth(
        clockValueLabel_->fontMetrics().horizontalAdvance(
            QStringLiteral("88:88:88")));
    clockValueLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    clockLayout->addWidget(clockValueLabel_);
    clockLayout->addSpacing(vtrpon::scalePx(96 + 7));
    clockLayout->addStretch(1);

    applyTimeSettings();
    clockTimer_ = new QTimer(this);
    clockTimer_->setInterval(250);
    QObject::connect(clockTimer_, &QTimer::timeout, this,
                     &PlaylistPanel::updateCurrentTime);
    clockTimer_->start();
    updateCurrentTime();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                               vtrpon::scalePx(12), vtrpon::scalePx(12));
    layout->setSpacing(vtrpon::scalePx(6));
    layout->addLayout(titleRow);
    layout->addLayout(playlistNameRow);
    layout->addLayout(playlistTabsRow);
    layout->addWidget(list_, 1);
    layout->addLayout(itemControlRow);
    if (userInfoCenter_) {
        layout->addWidget(new UserInfoBar(userInfoCenter_, this));
    }
    layout->addWidget(clockRow_);
    layout->addWidget(actionPanel);

    setTabOrder(playlistNameEdit_, playlistTabs_);
    setTabOrder(playlistTabs_, listViewButton_);
    setTabOrder(listViewButton_, tileViewButton_);
    setTabOrder(tileViewButton_, list_);
    setTabOrder(list_, moveUpButton_);
}

void PlaylistPanel::bindSelectionOperations()
{
    using A = ControllerOperations::Action;
    operations_->setGuard<quint64>(A::SelectItem, this, [this](quint64 id) {
        const auto state = model_->itemState(id);
        return state && state->playlistNumber == model_->activePlaylistNumber();
    });
    operations_->bind<quint64>(A::SelectItem, this, [this](quint64 id) {
        auto *item = itemsById_.value(id, nullptr);
        if (!item) return false;
        const auto state = itemState(id);
        const bool retryPowerPoint = selectedItemId() == id && state &&
            state->sourceType == PlaylistSourceType::PowerPoint &&
            model_->cueItemId() == id && model_->cueState() == PlaylistCueState::Error;
        QScopedValueRollback<bool> selecting(selectingThroughOperation_, true);
        list_->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
        if (retryPowerPoint) emit selectionChanged();
        return selectedItemId() == id;
    });
    operations_->setGuard<int>(A::SelectItemNumber, this, [this](int number) {
        return number > 0 && number <= model_->itemCount();
    });
    operations_->bind<int>(A::SelectItemNumber, this, [this](int number) {
        return operations_->execute(A::SelectItem,
                                    model_->itemIdAt(model_->activePlaylistNumber(), number - 1));
    });
    operations_->setGuard<int>(A::SelectRelativeItem, this, [this](int offset) {
        const quint64 targetId = relativeSelectionTarget(offset);
        return targetId != 0 && targetId != selectedItemId();
    });
    operations_->bind<int>(A::SelectRelativeItem, this, [this](int offset) {
        return operations_->execute(A::SelectItem, relativeSelectionTarget(offset));
    });
    operations_->setGuard<>(A::RemoveSelectedItem, this, [this] {
        return canRemoveSelectedItem();
    });
    operations_->bind<>(A::RemoveSelectedItem, this, [this] {
        return operations_->execute(A::RemoveItem, selectedItemId());
    });
    operations_->setGuard<PlaylistBulkOperation>(A::PlaylistBulk, this,
        [this](PlaylistBulkOperation operation) { return canApplyBulkOperation(operation); });
    operations_->bindMember(A::CopySelectedState, this, &PlaylistPanel::copySelectedItemState);
    operations_->bindMember(A::PasteSelectedState, this, &PlaylistPanel::pasteSelectedItemState);
}

void PlaylistPanel::connectInputs()
{
    connectPlaylistInputWidgets(list_, playlistTabs_, {
        [this](quint64 itemId, int targetInsertionRow, bool copy) {
            emit itemDropRequested(itemId, model_->activePlaylistNumber(),
                                   targetInsertionRow, copy);
        },
        [this](const QStringList &paths) { emit filesDropped(paths); },
        [this](quint64 itemId, const QPoint &globalPosition) {
            contextMenu_->showItemContextMenu(itemId, globalPosition);
        },
        [this](quint64 itemId) {
            QListWidgetItem *item = itemsById_.value(itemId, nullptr);
            if (!item) {
                return;
            }
            list_->setCurrentItem(item);
            if (selectedItemId() == itemId) {
                emit onAirRequested();
            }
        },
        [this](quint64 itemId, int targetPlaylistNumber, bool copy) {
            emit itemDropRequested(
                itemId, targetPlaylistNumber,
                model_->itemCount(targetPlaylistNumber), copy);
        }
    });
    QObject::connect(list_, &QListWidget::itemSelectionChanged, this,
                     [this] {
        if (!selectingThroughOperation_ && selectedItemId() != 0) {
            (void)operations_->execute(ControllerOperations::Action::SelectItem, selectedItemId());
        }
        selectedItemByPlaylist_[model_->activePlaylistNumber()] =
            selectedItemId();
        updateUserInfo();
        updateSelectionControls();
        emit selectionChanged();
        const auto selectedState = selectedItemState();
        if (!list_->property("tileSelectOnlyGesture").toBool()
            && editActivationMode_ == PlaylistEditActivationMode::Selection &&
            selectedState &&
            (selectedState->sourceType == PlaylistSourceType::File ||
             selectedState->sourceType == PlaylistSourceType::Uvc)) {
            selectionEditRequestedId_ = selectedState->id;
            if (!model_->itemConversionInProgress(selectedState->id))
                emit editRequested();
        }
    });
    QObject::connect(list_, &QListWidget::itemClicked, this, [this] {
        const auto selected = selectedItemState();
        // Tab restoration can already select this row without loading EDIT.
        // Clicking that row must still activate it in selection mode.
        if (!list_->property("tileSelectOnlyGesture").toBool() &&
            editActivationMode_ == PlaylistEditActivationMode::Selection &&
            selected && (selected->sourceType == PlaylistSourceType::File ||
                         selected->sourceType == PlaylistSourceType::Uvc) &&
            selected->id != selectionEditRequestedId_) {
            selectionEditRequestedId_ = selected->id;
            if (!model_->itemConversionInProgress(selected->id))
                emit editRequested();
        }
        if (selected && selected->sourceType == PlaylistSourceType::PowerPoint &&
            selected->id == model_->cueItemId() &&
            model_->cueState() == PlaylistCueState::Error) {
            // A retry must also be possible by selecting the same failed row.
            emit selectionChanged();
        }
    });
    QObject::connect(editButton_, &QPushButton::clicked, this,
                     &PlaylistPanel::editRequested);
    QObject::connect(listViewButton_, &QToolButton::clicked, this,
                     [this] { setTileViewEnabled(false); });
    QObject::connect(tileViewButton_, &QToolButton::clicked, this,
                     [this] { setTileViewEnabled(true); });
    QObject::connect(moveUpButton_, &QPushButton::clicked, this,
                     [this] {
        const quint64 itemId = selectedItemId();
        if (itemId != 0) {
            emit moveUpRequested(itemId);
        }
    });
    QObject::connect(moveDownButton_, &QPushButton::clicked, this,
                     [this] {
        const quint64 itemId = selectedItemId();
        if (itemId != 0) {
            emit moveDownRequested(itemId);
        }
    });
    QObject::connect(removeButton_, &QPushButton::clicked, this,
                     [this] {
        const quint64 itemId = selectedItemId();
        if (itemId != 0) {
            emit removeRequested(itemId);
        }
    });
    auto *deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), list_);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    deleteShortcut->setAutoRepeat(false);
    QObject::connect(deleteShortcut, &QShortcut::activated,
                     this, [this] {
        (void)operations_->executeFrom("Shortcut", ControllerOperations::Action::RemoveSelectedItem);
    });
    QObject::connect(addButton_, &QPushButton::clicked, this,
                     &PlaylistPanel::addRequested);
    QObject::connect(onAirButton_, &QPushButton::clicked, this,
                     [this] {
                         if (onAirToggleMode_ && model_->onAirItemId() != 0)
                             emit offAirRequested();
                         else
                             emit onAirRequested();
                     });
    QObject::connect(crossfadeCheckBox_, &QCheckBox::toggled,
                     crossfadeDurationControl_, &QWidget::setEnabled);
    QObject::connect(crossfadeCheckBox_, &QCheckBox::toggled, this,
                     [this](bool enabled) {
        model_->setManualCrossfadeEnabled(model_->activePlaylistNumber(), enabled);
    });
    QObject::connect(crossfadeDurationSpinBox_, &QDoubleSpinBox::valueChanged,
                     this, [this](double seconds) {
        model_->setManualCrossfadeDurationMs(qRound64(seconds * 1000.0));
    });
    QObject::connect(playlistNameEdit_, &QLineEdit::textEdited, this,
                     [this](const QString &name) {
        (void)operations_->execute(ControllerOperations::Action::RenamePlaylist,
                                   model_->activePlaylistNumber(), name);
    });
    QObject::connect(playlistTabs_, &QTabBar::tabMoved, this,
                     [this](int, int destinationIndex) {
        model_->movePlaylist(playlistTabs_->tabData(destinationIndex).toInt(),
                             destinationIndex);
    });
    QObject::connect(playlistTabs_, &QTabBar::currentChanged, this,
                     [this](int tabIndex) {
        if (tabIndex < 0) {
            return;
        }
        const int playlistNumber =
            playlistTabs_->tabData(tabIndex).toInt();
        if (playlistNumber == 0) {
            (void)operations_->execute(ControllerOperations::Action::CreatePlaylist);
            return;
        }
        (void)operations_->execute(ControllerOperations::Action::ActivatePlaylist, playlistNumber);
    });
}

void PlaylistPanel::connectModel()
{
    QObject::connect(model_, &PlaylistModel::playlistOrderChanged, this, [this] {
        const QSignalBlocker blocker(playlistTabs_);
        const auto playlists = model_->playlists();
        for (int index = 0; index < playlists.size(); ++index) {
            const int previousIndex = tabIndexForPlaylist(playlists.at(index).number);
            if (previousIndex >= 0 && previousIndex != index) {
                playlistTabs_->moveTab(previousIndex, index);
            }
        }
    });
    QObject::connect(model_, &PlaylistModel::itemAdded, this,
                     [this](quint64 itemId) {
        const auto state = itemState(itemId);
        if (!state || state->playlistNumber !=
                          model_->activePlaylistNumber()) {
            return;
        }
        addVisibleItem(itemId);
        updateItemOrder(state->itemNumber - 1);
        updateSelectionControls();
    });
    QObject::connect(model_, &PlaylistModel::itemRemoved, this,
                     [this](quint64 itemId, int previousRow) {
        const quint64 previousSelectedId = selectedItemId();
        QListWidgetItem *item = itemsById_.take(itemId);
        if (!item) {
            return;
        }
        const int row = list_->row(item);
        {
            const QSignalBlocker blocker(list_);
            delete list_->takeItem(row);
            if (previousSelectedId == itemId) {
                if (list_->count() > 0) {
                    list_->setCurrentItem(list_->item((std::min)(
                        previousRow, list_->count() - 1)));
                } else {
                    list_->clearSelection();
                }
            }
        }
        updateItemOrder(previousRow);
        updateUserInfo();
        updateSelectionControls();
        if (selectedItemId() != previousSelectedId) {
            emit selectionChanged();
        }
    });
    QObject::connect(model_, &PlaylistModel::itemMoved, this,
                     [this](quint64 itemId, int previousRow, int currentRow) {
        QListWidgetItem *item = itemsById_.value(itemId, nullptr);
        if (!item) {
            return;
        }
        const quint64 selectedId = selectedItemId();
        {
            const QSignalBlocker blocker(list_);
            const int displayedRow = list_->row(item);
            if (displayedRow != currentRow) {
                QListWidgetItem *movedItem = list_->takeItem(displayedRow);
                list_->insertItem(currentRow, movedItem);
            }
            if (QListWidgetItem *selectedItem =
                    itemsById_.value(selectedId, nullptr)) {
                list_->setCurrentItem(selectedItem);
            }
        }
        updateItemOrder((std::min)(previousRow, currentRow),
                        (std::max)(previousRow, currentRow));
        updateSelectionControls();
        list_->scrollToItem(item);
    });
    QObject::connect(
        model_, &PlaylistModel::itemMovedToPlaylist, this,
        [this](quint64 itemId, int previousPlaylistNumber, int,
               int currentPlaylistNumber, int) {
            if (itemId == model_->onAirItemId()) {
                updateOnAirPlaylistTab();
            }
            const int activePlaylistNumber = model_->activePlaylistNumber();
            if (activePlaylistNumber != previousPlaylistNumber &&
                activePlaylistNumber != currentPlaylistNumber) {
                return;
            }
            rebuildActivePlaylist();
            if (activePlaylistNumber == currentPlaylistNumber) {
                if (QListWidgetItem *item = itemsById_.value(itemId, nullptr)) {
                    list_->setCurrentItem(item);
                    list_->scrollToItem(item);
                }
            }
        });
    QObject::connect(
        model_, &PlaylistModel::itemCopiedToPlaylist, this,
        [this](quint64, quint64 copiedItemId, int playlistNumber, int) {
            if (model_->activePlaylistNumber() != playlistNumber) {
                return;
            }
            if (QListWidgetItem *item =
                    itemsById_.value(copiedItemId, nullptr)) {
                list_->setCurrentItem(item);
                list_->scrollToItem(item);
            }
        });
    QObject::connect(model_, &PlaylistModel::itemChanged, this,
                     [this](quint64 itemId) {
        updateItemDisplay(itemId);
        updateItemVisual(itemId);
    });
    QObject::connect(model_, &PlaylistModel::itemConversionStateChanged, this,
                     [this](quint64 itemId) {
        updateItemDisplay(itemId);
        updateItemVisual(itemId);
        updateSelectionControls();
        if (QListWidgetItem *item = itemsById_.value(itemId, nullptr)) {
            list_->viewport()->update(list_->visualItemRect(item));
        }
        if (!model_->itemConversionInProgress(itemId) &&
            editActivationMode_ == PlaylistEditActivationMode::Selection &&
            selectionEditRequestedId_ == itemId && selectedItemId() == itemId) {
            // Selection may have requested EDIT while media analysis was still
            // pending. Retry after the replacement signals finish propagating.
            QTimer::singleShot(0, this, [this, itemId] {
                const auto selected = selectedItemState();
                if (editActivationMode_ == PlaylistEditActivationMode::Selection &&
                    selectionEditRequestedId_ == itemId && selected &&
                    selected->id == itemId &&
                     (selected->sourceType == PlaylistSourceType::File ||
                      selected->sourceType == PlaylistSourceType::Uvc) &&
                    !model_->itemConversionInProgress(itemId))
                    emit editRequested();
            });
        }
    });
    QObject::connect(model_, &PlaylistModel::countChanged, this,
                     [this] { updateItemCount(); });
    QObject::connect(model_, &PlaylistModel::onAirStateChanged, this,
                     [this](quint64 previousItemId,
                            quint64 currentItemId) {
         if (previousItemId != currentItemId) {
             setOnAirHighlightOpacity(previousItemId, 0.0);
             setOnAirHighlightOpacity(currentItemId, 1.0);
         }
         updateItemVisual(previousItemId);
         updateItemVisual(currentItemId);
         updateOnAirPlaylistTab();
         updateUserInfo();
         updateSelectionControls();
     });
    QObject::connect(model_, &PlaylistModel::cueStateChanged, this,
                     [this](quint64 previousItemId,
                            quint64 currentItemId) {
        updateItemVisual(previousItemId);
        updateItemVisual(currentItemId);
        updateUserInfo();
        updateSelectionControls();
    });
    QObject::connect(model_, &PlaylistModel::dskStateChanged, this,
                     [this](quint64 previousItemId,
                            quint64 currentItemId) {
        updateItemVisual(previousItemId);
        updateItemVisual(currentItemId);
    });
    QObject::connect(model_, &PlaylistModel::editStateChanged, this,
                     [this](quint64 previousItemId,
                            quint64 currentItemId) {
        updateItemVisual(previousItemId);
        updateItemVisual(currentItemId);
    });
    QObject::connect(model_, &PlaylistModel::bgmStateChanged, this,
                     [this](quint64 previousItemId,
                            quint64 currentItemId) {
        updateItemVisual(previousItemId);
        updateItemVisual(currentItemId);
        updateSelectionControls();
    });
    QObject::connect(model_, &PlaylistModel::playlistAdded, this,
                     [this] {
        rebuildPlaylistTabs();
        updateBulkControls();
    });
    QObject::connect(model_, &PlaylistModel::playlistRemoved, this,
                     [this](int playlistNumber) {
        selectedItemByPlaylist_.remove(playlistNumber);
        rebuildPlaylistTabs();
        updateBulkControls();
    });
    QObject::connect(model_, &PlaylistModel::playlistNameChanged, this,
                     [this](int playlistNumber, const QString &name) {
        const int tabIndex = tabIndexForPlaylist(playlistNumber);
        if (tabIndex >= 0) {
            playlistTabs_->setTabToolTip(
                tabIndex, name.isEmpty()
                              ? vtrpon::uiText("playlist.default_name")
                                    .arg(playlistNumber)
                              : name);
        }
        if (playlistNumber == model_->activePlaylistNumber() &&
            playlistNameEdit_->text() != name) {
            playlistNameEdit_->setText(name);
        }
    });
    QObject::connect(model_, &PlaylistModel::playlistModeChanged, this,
                     [this](int playlistNumber, PlaylistMode mode) {
        const int tabIndex = tabIndexForPlaylist(playlistNumber);
        if (tabIndex >= 0) {
            playlistTabs_->setTabText(
                tabIndex,
                mode == PlaylistMode::Bgm
                    ? QStringLiteral("%1  BGM").arg(playlistNumber)
                    : QString::number(playlistNumber));
        }
        if (playlistNumber == model_->activePlaylistNumber()) {
            for (auto item = itemsById_.cbegin(); item != itemsById_.cend();
                 ++item) {
                updateItemDisplay(item.key());
                updateItemVisual(item.key());
            }
        }
    });
    QObject::connect(model_, &PlaylistModel::activePlaylistChanged, this,
                     [this](int previousPlaylistNumber,
                            int currentPlaylistNumber) {
        selectedItemByPlaylist_[previousPlaylistNumber] = selectedItemId();
        const int tabIndex = tabIndexForPlaylist(currentPlaylistNumber);
        if (tabIndex >= 0 && playlistTabs_->currentIndex() != tabIndex) {
            const QSignalBlocker blocker(playlistTabs_);
            playlistTabs_->setCurrentIndex(tabIndex);
        }
        rebuildActivePlaylist();
    });
    QObject::connect(model_, &PlaylistModel::modelReset, this, [this] {
        selectedItemByPlaylist_.clear();
        rebuildPlaylistTabs();
        rebuildActivePlaylist();
    });
}

void PlaylistPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void PlaylistPanel::retranslateUi()
{
    findChild<QLabel *>(QStringLiteral("playlistTitle"))
        ->setText(vtrpon::uiText("playlist.title"));
    findChild<QLabel *>(QStringLiteral("playlistNameLabel"))
        ->setText(vtrpon::uiText("playlist.name.label"));
    playlistNameEdit_->setAccessibleName(
        vtrpon::uiText("playlist.name.accessible"));
    playlistNameEdit_->setPlaceholderText(
        vtrpon::uiText("playlist.name.placeholder"));
    playlistTabs_->setAccessibleName(
        vtrpon::uiText("playlist.tabs.accessible"));
    listViewButton_->setAccessibleName(
        vtrpon::uiText("playlist.view.list"));
    listViewButton_->setToolTip(vtrpon::uiText("playlist.view.list"));
    tileViewButton_->setAccessibleName(
        vtrpon::uiText("playlist.view.tiles"));
    tileViewButton_->setToolTip(
        vtrpon::uiText("playlist.view.tiles.tooltip"));
    list_->setAccessibleName(vtrpon::uiText("playlist.items.accessible"));
    list_->setAccessibleDescription(vtrpon::uiText(
        list_->viewMode() == QListView::IconMode
            ? "playlist.tiles.description"
            : "playlist.list.description"));
    moveUpButton_->setAccessibleName(
        vtrpon::uiText("playlist.move_up.accessible"));
    moveUpButton_->setToolTip(vtrpon::uiText("playlist.move_up.tooltip"));
    moveDownButton_->setAccessibleName(
        vtrpon::uiText("playlist.move_down.accessible"));
    moveDownButton_->setToolTip(vtrpon::uiText("playlist.move_down.tooltip"));
    removeButton_->setText(vtrpon::uiText("playlist.delete"));
    removeButton_->setAccessibleName(
        vtrpon::uiText("playlist.delete.accessible"));
    removeButton_->setToolTip(vtrpon::uiText("playlist.delete.tooltip"));
    bulkMenu_->setTitle(vtrpon::uiText("menu.playlist"));
    bulkMenu_->findChild<QAction *>(QStringLiteral("playlistMenuCopyStateAction"))->setText(
        vtrpon::uiText("playlist.context.copy_state"));
    bulkMenu_->findChild<QAction *>(QStringLiteral("playlistMenuPasteStateAction"))->setText(
        vtrpon::uiText("playlist.context.paste_state"));
    const char *textIds[] = {"playlist.bulk.play_once", "playlist.bulk.repeat_all",
        "playlist.bulk.shuffle", "playlist.bulk.bgm_mode", "playlist.bulk.standard_mode",
        "playlist.bulk.clear", "playlist.bulk.delete", "playlist.bulk.undo"};
    int index = 0;
    for (auto *action : bulkMenu_->actions()) {
        if (action->data().isValid()) action->setText(vtrpon::uiText(textIds[index++]));
    }
    {
        const auto selected = bulkOperationComboBox_->currentData();
        const QSignalBlocker blocker(bulkOperationComboBox_);
        bulkOperationComboBox_->clear();
        bulkOperationComboBox_->addItem(vtrpon::uiText("playlist.bulk.title"));
        bulkOperationComboBox_->insertSeparator(1);
        for (auto *action : bulkMenu_->actions()) {
            if (action->data().isValid())
                bulkOperationComboBox_->addItem(action->text(), action->data());
        }
        const int selectedIndex = selected.isValid() ? bulkOperationComboBox_->findData(selected) : 0;
        bulkOperationComboBox_->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    }
    bulkOperationComboBox_->setAccessibleName(vtrpon::uiText("playlist.bulk.accessible"));
    bulkOperationComboBox_->setToolTip(vtrpon::uiText("playlist.bulk.tooltip"));
    bulkApplyButton_->setText(vtrpon::uiText("common.apply"));
    bulkApplyButton_->setAccessibleName(vtrpon::uiText("playlist.bulk.apply.accessible"));
    bulkApplyButton_->setToolTip(vtrpon::uiText("playlist.bulk.apply.tooltip"));
    updateBulkControls();

    editButton_->setText(vtrpon::uiText("info.source.edit"));
    editButton_->setAccessibleName(
        vtrpon::uiText("playlist.edit.accessible"));
    addButton_->setText(vtrpon::uiText("playlist.add_files"));
    addButton_->setAccessibleName(vtrpon::uiText("playlist.add.accessible"));
    addButton_->setToolTip(vtrpon::uiText("playlist.add.tooltip"));
    onAirButton_->setText(vtrpon::uiText("playlist.on_air"));
    onAirButton_->setAccessibleName(
        vtrpon::uiText("playlist.on_air.accessible"));
    crossfadeCheckBox_->setText(vtrpon::uiText("playlist.crossfade"));
    crossfadeCheckBox_->setAccessibleName(
        vtrpon::uiText("playlist.crossfade.accessible"));
    crossfadeCheckBox_->setToolTip(
        vtrpon::uiText("playlist.crossfade.tooltip"));
    crossfadeDurationSpinBox_->setAccessibleName(
        vtrpon::uiText("playlist.crossfade_duration.accessible"));
    retranslateDurationButtons(crossfadeDurationSpinBox_,
                               crossfadeDurationIncreaseButton_,
                               crossfadeDurationDecreaseButton_);
    findChild<QWidget *>(QStringLiteral("playlistActionPanel"))
        ->setAccessibleName(vtrpon::uiText("playlist.actions.accessible"));
    clockTitleLabel_->setText(vtrpon::uiText("playlist.current_time"));
    updateCurrentTime();
    if (!editShortcut_.isEmpty() || !onAirShortcut_.isEmpty()) {
        setShortcutLabels(editShortcut_, onAirShortcut_);
    } else {
        editButton_->setToolTip(vtrpon::uiText("playlist.edit.tooltip"));
        onAirButton_->setToolTip(vtrpon::uiText("playlist.on_air.tooltip"));
    }
    rebuildPlaylistTabs();
    updateItemCount();
    updateSelectionControls();
    for (auto item = itemsById_.cbegin(); item != itemsById_.cend(); ++item) {
        const auto state = itemState(item.key());
        if (state && !thumbnails_->contains(item.key())) {
            item.value()->setData(kThumbnailRole,
                                  createThumbnailPlaceholder(*state));
        }
        updateItemDisplay(item.key());
        updateItemVisual(item.key());
    }
    list_->viewport()->update();
}

vtrpon::NetworkTimeClock *PlaylistPanel::networkTimeClock() const
{
    return networkTimeClock_;
}

void PlaylistPanel::applyTimeSettings()
{
    if (networkTimeClock_) networkTimeClock_->setTimeZone(vtrpon::savedClockTimeZone());
    QFont font = vtrpon::savedClockFont();
    font.setPointSizeF(font.pointSizeF() * vtrpon::uiScale());
    clockValueLabel_->setFont(font);
    clockValueLabel_->setMinimumWidth(clockValueLabel_->fontMetrics().horizontalAdvance(QStringLiteral("88:88:88")) + vtrpon::scalePx(12));
    clockRow_->setFixedHeight(std::max(vtrpon::scalePx(22), clockValueLabel_->fontMetrics().height() + vtrpon::scalePx(2)));
    updateCurrentTime();
}

void PlaylistPanel::updateCurrentTime()
{
    const QDateTime currentDateTime = networkTimeClock_
        ? networkTimeClock_->currentLocalDateTime()
        : QDateTime::currentDateTimeUtc().toTimeZone(vtrpon::savedClockTimeZone());
    const QString timeText = QLocale().toString(
        currentDateTime.time(), QStringLiteral("HH:mm:ss"));
    if (clockValueLabel_->text() != timeText) {
        clockValueLabel_->setText(timeText);
    }

    const QString accessibleName =
        vtrpon::uiText("playlist.current_time.accessible").arg(timeText);
    if (clockRow_->accessibleName() != accessibleName) {
        clockRow_->setAccessibleName(accessibleName);
    }
    const QString toolTip = networkTimeClock_
        && networkTimeClock_->isNetworkSynchronized()
        ? vtrpon::uiText("playlist.current_time.ntp_tooltip")
        : vtrpon::uiText("playlist.current_time.local_tooltip");
    if (clockRow_->toolTip() != toolTip) {
        clockRow_->setToolTip(toolTip);
        clockRow_->setAccessibleDescription(toolTip);
        clockTitleLabel_->setToolTip(toolTip);
        clockValueLabel_->setToolTip(toolTip);
    }
}

void PlaylistPanel::addVisibleItem(quint64 itemId)
{
    const auto state = itemState(itemId);
    if (!state || state->playlistNumber != model_->activePlaylistNumber()
        || itemsById_.contains(itemId)) {
        return;
    }
    auto *item = new QListWidgetItem(
        statusIcon(StatusIconState::None), QString());
    list_->insertItem(std::clamp(state->itemNumber - 1, 0, list_->count()),
                      item);
    item->setData(kItemIdRole, QVariant::fromValue<qulonglong>(itemId));
    item->setData(kStatusIconStateRole,
                  static_cast<int>(StatusIconState::None));
    item->setData(kOnAirHighlightOpacityRole,
                  itemId == model_->onAirItemId() ? 1.0 : 0.0);
    itemsById_.insert(itemId, item);
    refreshThumbnail(itemId);
    updateItemVisual(itemId);
}

void PlaylistPanel::rebuildActivePlaylist()
{
    selectionEditRequestedId_ = 0;
    const int playlistNumber = model_->activePlaylistNumber();
    crossfadeCheckBox_->setChecked(
        model_->manualCrossfadeEnabled(playlistNumber));
    crossfadeDurationSpinBox_->setValue(model_->manualCrossfadeDurationMs() / 1000.0);
    crossfadeDurationControl_->setEnabled(crossfadeCheckBox_->isChecked());
    const quint64 selectedId =
        selectedItemByPlaylist_.value(playlistNumber, 0);
    {
        const QSignalBlocker blocker(list_);
        list_->clear();
        itemsById_.clear();
        for (int row = 0; row < model_->itemCount(); ++row) {
            addVisibleItem(model_->itemIdAt(row));
        }
        if (QListWidgetItem *selectedItem =
                itemsById_.value(selectedId, nullptr)) {
            list_->setCurrentItem(selectedItem);
        } else {
            list_->clearSelection();
        }
    }
    updatePlaylistNameEditor();
    updateItemCount();
    updateUserInfo();
    updateSelectionControls();
    emit selectionChanged();
}

void PlaylistPanel::rebuildPlaylistTabs()
{
    const QSignalBlocker blocker(playlistTabs_);
    while (playlistTabs_->count() > 0) {
        playlistTabs_->removeTab(playlistTabs_->count() - 1);
    }
    for (const PlaylistState &playlist : model_->playlists()) {
        addPlaylistTab(playlist.number);
    }
    const int addTabIndex = playlistTabs_->addTab(QStringLiteral("+"));
    playlistTabs_->setTabData(addTabIndex, 0);
    playlistTabs_->setTabToolTip(addTabIndex,
                                 vtrpon::uiText("playlist.add_tab.tooltip"));
    playlistTabs_->setCurrentIndex(
        tabIndexForPlaylist(model_->activePlaylistNumber()));
    updateOnAirPlaylistTab();
}

void PlaylistPanel::addPlaylistTab(int playlistNumber)
{
    if (playlistNumber <= 0 || tabIndexForPlaylist(playlistNumber) >= 0) {
        return;
    }
    int insertIndex = playlistTabs_->count();
    if (insertIndex > 0
        && playlistTabs_->tabData(insertIndex - 1).toInt() == 0) {
        --insertIndex;
    }
    const QString tabText =
        model_->playlistMode(playlistNumber) == PlaylistMode::Bgm
            ? QStringLiteral("%1  BGM").arg(playlistNumber)
            : QString::number(playlistNumber);
    playlistTabs_->insertTab(insertIndex, tabText);
    playlistTabs_->setTabData(insertIndex, playlistNumber);
    const QString name = model_->playlistName(playlistNumber);
    playlistTabs_->setTabToolTip(
        insertIndex, name.isEmpty()
                         ? vtrpon::uiText("playlist.default_name").arg(playlistNumber)
                         : name);
}

int PlaylistPanel::tabIndexForPlaylist(int playlistNumber) const
{
    for (int tabIndex = 0; tabIndex < playlistTabs_->count(); ++tabIndex) {
        if (playlistTabs_->tabData(tabIndex).toInt() == playlistNumber) {
            return tabIndex;
        }
    }
    return -1;
}

void PlaylistPanel::setTileViewEnabled(bool enabled)
{
    const quint64 selectedId = selectedItemId();
    {
        const QSignalBlocker listViewBlocker(listViewButton_);
        const QSignalBlocker tileViewBlocker(tileViewButton_);
        listViewButton_->setChecked(!enabled);
        tileViewButton_->setChecked(enabled);
    }

    list_->setUpdatesEnabled(false);
    list_->setViewMode(enabled ? QListView::IconMode : QListView::ListMode);
    list_->setFlow(enabled ? QListView::LeftToRight : QListView::TopToBottom);
    list_->setWrapping(enabled);
    list_->setResizeMode(QListView::Adjust);
    list_->setDragEnabled(true);
    list_->setAcceptDrops(true);
    list_->setDragDropMode(QAbstractItemView::DragDrop);
    list_->setDefaultDropAction(Qt::MoveAction);
    list_->setDropIndicatorShown(false);
    list_->setGridSize(enabled
                           ? vtrpon::scaleSize(kPlaylistTileWidth,
                                              kPlaylistTileHeight)
                           : QSize());
    list_->setSpacing(vtrpon::scalePx(enabled ? 4 : 3));
    list_->setUniformItemSizes(enabled);
    list_->setProperty("tileView", enabled);
    list_->viewport()->setCursor(enabled ? Qt::PointingHandCursor
                                        : Qt::ArrowCursor);
    list_->setAccessibleDescription(vtrpon::uiText(
        enabled ? "playlist.tiles.description"
                : "playlist.list.description"));
    list_->setUpdatesEnabled(true);
    list_->doItemsLayout();
    list_->viewport()->update();

    if (selectedId != 0) {
        QTimer::singleShot(0, this, [this, selectedId] {
            if (QListWidgetItem *selected =
                    itemsById_.value(selectedId, nullptr)) {
                list_->scrollToItem(selected,
                                    QAbstractItemView::EnsureVisible);
            }
        });
    }
}

void PlaylistPanel::updateOnAirPlaylistTab()
{
    int playlistNumber = 0;
    const auto onAirItem = model_->itemState(model_->onAirItemId());
    if (onAirItem) {
        playlistNumber = onAirItem->playlistNumber;
    }
    setPlaylistOnAirTab(playlistTabs_, playlistNumber);
}

void PlaylistPanel::updatePlaylistNameEditor()
{
    const QString name =
        model_->playlistName(model_->activePlaylistNumber());
    if (playlistNameEdit_->text() != name) {
        playlistNameEdit_->setText(name);
    }
}

void PlaylistPanel::setShortcutLabels(const QString &editShortcut,
                                      const QString &onAirShortcut)
{
    editShortcut_ = editShortcut;
    onAirShortcut_ = onAirShortcut;
    editButton_->setToolTip(
        vtrpon::uiText("playlist.edit.shortcut_tooltip")
            .arg(editShortcut));
    onAirButton_->setToolTip(
        vtrpon::uiText("playlist.on_air.shortcut_tooltip")
            .arg(onAirShortcut));
    updateSelectionControls();
}

void PlaylistPanel::setEditActivationMode(PlaylistEditActivationMode mode)
{
    editActivationMode_ = mode;
    editButton_->setVisible(mode == PlaylistEditActivationMode::EditButton);
}

PlaylistEditActivationMode PlaylistPanel::editActivationMode() const
{
    return editActivationMode_;
}

bool PlaylistPanel::selectNextItemAfter(quint64 itemId)
{
    if (!model_ || itemId == 0) {
        return false;
    }
    const quint64 nextItemId = model_->nextItemIdForPlayout(itemId);
    const auto nextItem = model_->itemState(nextItemId);
    if (!nextItem || nextItemId == itemId) {
        return false;
    }

    selectedItemByPlaylist_[nextItem->playlistNumber] = nextItemId;
    if (model_->activePlaylistNumber() != nextItem->playlistNumber) {
        return model_->setActivePlaylistNumber(nextItem->playlistNumber);
    }
    QListWidgetItem *visibleItem = itemsById_.value(nextItemId, nullptr);
    if (!visibleItem) {
        return false;
    }
    list_->setCurrentItem(visibleItem);
    list_->scrollToItem(visibleItem);
    return selectedItemId() == nextItemId;
}

void PlaylistPanel::updateItemOrder(int firstRow, int lastRow)
{
    if (list_->count() == 0) {
        return;
    }
    firstRow = std::clamp(firstRow, 0, list_->count() - 1);
    if (lastRow < 0) {
        lastRow = list_->count() - 1;
    } else {
        lastRow = std::clamp(lastRow, firstRow, list_->count() - 1);
    }

    for (int rowIndex = firstRow; rowIndex <= lastRow; ++rowIndex) {
        QListWidgetItem *item = list_->item(rowIndex);
        updateItemDisplay(item->data(kItemIdRole).toULongLong());
    }
}

void PlaylistPanel::updateSelectionControls()
{
    const quint64 selectedId = selectedItemId();
    const int selectedRow = selectedId == 0
        ? -1 : list_->row(itemsById_.value(selectedId, nullptr));
    moveUpButton_->setEnabled(selectedRow > 0);
    moveDownButton_->setEnabled(
        selectedRow >= 0 && selectedRow + 1 < list_->count());
    removeButton_->setEnabled(canRemoveSelectedItem());
    const auto selectedState = itemState(selectedId);
    const bool conversionInProgress =
        model_->itemConversionInProgress(selectedId);
    editButton_->setEnabled(
        selectedRow >= 0 && selectedState &&
         (selectedState->sourceType == PlaylistSourceType::File ||
          selectedState->sourceType == PlaylistSourceType::Uvc) &&
        !conversionInProgress);
    const bool onAirActive = model_->onAirItemId() != 0;
    onAirButton_->setEnabled(selectedRow >= 0 || (onAirToggleMode_ && onAirActive));
    onAirButton_->setText(vtrpon::uiText("playlist.on_air"));
    onAirButton_->setAccessibleName(vtrpon::uiText(
        onAirToggleMode_ && onAirActive ? "program.off_air.accessible"
                                       : "playlist.on_air.accessible"));
    onAirButton_->setToolTip(onAirToggleMode_
        ? vtrpon::uiText(onAirActive ? "program.off_air.accessible"
                                    : "playlist.on_air.tooltip")
        : (onAirShortcut_.isEmpty() ? vtrpon::uiText("playlist.on_air.tooltip")
            : vtrpon::uiText("playlist.on_air.shortcut_tooltip").arg(onAirShortcut_)));
    if (onAirButton_->property("onAirActive").toBool() != onAirActive) {
        onAirButton_->setProperty("onAirActive", onAirActive);
        repolish(onAirButton_);
    }
    onAirButton_->setAccessibleDescription(
        onAirActive ? vtrpon::uiText("playlist.program.on_air")
                    : vtrpon::uiText("playlist.program.off_air"));
    const bool showPowerPointCue = selectedState &&
        selectedState->sourceType == PlaylistSourceType::PowerPoint &&
        selectedId != model_->onAirItemId();
    powerPointCueStatusLabel_->setVisible(showPowerPointCue);
    if (showPowerPointCue) {
        const auto cueState = selectedId == model_->cueItemId()
            ? model_->cueState() : PlaylistCueState::None;
        const bool ready = cueState == PlaylistCueState::Ready;
        const bool error = cueState == PlaylistCueState::Error;
        const char *textId = ready ? "playlist.powerpoint.ready"
            : error ? "playlist.powerpoint.failed" : "playlist.powerpoint.preparing";
        const QString text = vtrpon::uiText(textId);
        const QString detail = error ? model_->cueDetail()
            : vtrpon::uiText(ready ? "playlist.state.cue_ready"
                                  : "playlist.powerpoint.preparing_detail");
        powerPointCueStatusLabel_->setText(text);
        powerPointCueStatusLabel_->setAccessibleName(text);
        powerPointCueStatusLabel_->setToolTip(detail);
        powerPointCueStatusLabel_->setAccessibleDescription(detail);
        const QString color = ready ? QStringLiteral("#6fd99b")
            : error ? QStringLiteral("#ff8585") : QStringLiteral("#ffd166");
        powerPointCueStatusLabel_->setStyleSheet(
            QStringLiteral("color: %1; font-weight: 600;").arg(color));
    }
    updateBulkControls();
}

void PlaylistPanel::updateBulkControls()
{
    bulkApplyButton_->setEnabled(false);
    const auto selectedId = selectedItemId();
    const bool selected = model_->itemState(selectedId).has_value();
    bulkMenu_->findChild<QAction *>(QStringLiteral("playlistMenuCopyStateAction"))->setEnabled(selected);
    bulkMenu_->findChild<QAction *>(QStringLiteral("playlistMenuPasteStateAction"))->setEnabled(
        selected && itemOperations_->hasCopiedState() && !model_->itemSettingsLocked(selectedId) &&
        model_->itemState(selectedId)->sourceType == PlaylistSourceType::File);
    for (auto *action : bulkMenu_->actions()) {
        if (!action->data().isValid()) continue;
        const auto operation = static_cast<PlaylistBulkOperation>(action->data().toInt());
        const bool available = canApplyBulkOperation(operation);
        action->setEnabled(available);
        if (action->data() == bulkOperationComboBox_->currentData())
            bulkApplyButton_->setEnabled(available);
    }
}

bool PlaylistPanel::canRemoveSelectedItem() const
{
    const quint64 id = selectedItemId();
    return model_->itemState(id).has_value() && id != model_->onAirItemId() &&
        id != model_->bgmItemId();
}

quint64 PlaylistPanel::relativeSelectionTarget(int offset) const
{
    const int playlistNumber = model_->activePlaylistNumber();
    const int count = model_->itemCount(playlistNumber);
    if (count == 0 || (offset != -1 && offset != 1)) return 0;
    const quint64 selectedId = selectedItemId();
    const auto selected = model_->itemState(selectedId);
    const int row = selected && selected->playlistNumber == playlistNumber
        ? std::clamp(model_->indexOf(selectedId) + offset, 0, count - 1) : 0;
    return model_->itemIdAt(playlistNumber, row);
}

bool PlaylistPanel::canApplyBulkOperation(PlaylistBulkOperation operation) const
{
    switch (operation) {
    case PlaylistBulkOperation::Undo: return bulkUndoAvailable_;
    case PlaylistBulkOperation::DeletePlaylist: return model_->playlists().size() > 1;
    case PlaylistBulkOperation::BgmMode:
    case PlaylistBulkOperation::StandardMode: return true;
    case PlaylistBulkOperation::PlayOnce:
    case PlaylistBulkOperation::RepeatAll:
    case PlaylistBulkOperation::Shuffle:
    case PlaylistBulkOperation::Clear: return model_->itemCount() > 0;
    }
    return false;
}

int PlaylistPanel::itemCount() const
{
    return model_->itemCount();
}

void PlaylistPanel::setBulkUndoAvailable(bool available)
{
    if (bulkUndoAvailable_ == available) {
        return;
    }
    bulkUndoAvailable_ = available;
    updateBulkControls();
}

quint64 PlaylistPanel::selectedItemId() const
{
    const QList<QListWidgetItem *> selectedItems = list_->selectedItems();
    const QListWidgetItem *item = selectedItems.isEmpty() ? nullptr
                                                          : selectedItems.first();
    return item ? item->data(kItemIdRole).toULongLong() : 0;
}

QString PlaylistPanel::selectedFilePath() const
{
    const auto state = selectedItemState();
    return state ? state->filePath : QString();
}

std::optional<PlaylistItemState> PlaylistPanel::selectedItemState() const
{
    return itemState(selectedItemId());
}

std::optional<PlaylistItemState> PlaylistPanel::itemState(quint64 itemId) const
{
    return model_->itemState(itemId);
}

bool PlaylistPanel::allowItemAction(quint64 itemId, PlaylistItemAction action)
{
    return itemOperations_->allowItemAction(itemId, action);
}

void PlaylistPanel::showBlockedAction(PlaylistItemAction action)
{
    QString actionName;
    switch (action) {
    case PlaylistItemAction::OnAir:
        actionName = vtrpon::uiText("playlist.on_air");
        break;
    case PlaylistItemAction::Edit:
        actionName = vtrpon::uiText("info.source.edit");
        break;
    case PlaylistItemAction::Dsk:
        actionName = QStringLiteral("DSK");
        break;
    case PlaylistItemAction::CopyItem:
        actionName = vtrpon::uiText("playlist.conversion.copy_item_action");
        break;
    case PlaylistItemAction::CopyState:
        actionName = vtrpon::uiText("playlist.context.copy_state");
        break;
    case PlaylistItemAction::PasteState:
        actionName = vtrpon::uiText("playlist.context.paste_state");
        break;
    }
    if (userInfoCenter_) {
        userInfoCenter_->showWarning(
            QStringLiteral("PLAYLIST"),
            vtrpon::LocalizedText("playlist.conversion.action_unavailable")
                .arg(actionName));
    }
}

bool PlaylistPanel::copySelectedItemState()
{
    return itemOperations_->copyItemState(selectedItemId());
}

bool PlaylistPanel::pasteSelectedItemState()
{
    return itemOperations_->pasteItemState(selectedItemId());
}

quint64 PlaylistPanel::onAirItemId() const
{
    return model_->onAirItemId();
}

void PlaylistPanel::setOnAirToggleMode(bool enabled)
{
    onAirToggleMode_ = enabled;
    updateSelectionControls();
}

bool PlaylistPanel::crossfadeEnabled() const
{
    return crossfadeCheckBox_ && crossfadeCheckBox_->isChecked();
}

qint64 PlaylistPanel::crossfadeDurationMs() const
{
    return crossfadeDurationSpinBox_
        ? qRound64(crossfadeDurationSpinBox_->value() * 1000.0)
        : kDefaultItemFadeDurationMs;
}

void PlaylistPanel::setOnAirHighlightOpacity(quint64 itemId, qreal opacity)
{
    QListWidgetItem *item = itemsById_.value(itemId, nullptr);
    if (!item) {
        return;
    }
    const qreal boundedOpacity = std::clamp(opacity, 0.0, 1.0);
    if (!qFuzzyCompare(item->data(kOnAirHighlightOpacityRole).toReal(),
                       boundedOpacity)) {
        item->setData(kOnAirHighlightOpacityRole, boundedOpacity);
    }
}

void PlaylistPanel::setItemThumbnail(quint64 itemId, const QImage &image,
                                     const QString &errorDetail)
{
    thumbnails_->setThumbnail(itemId, image, errorDetail);
}

void PlaylistPanel::refreshThumbnail(quint64 itemId)
{
    QListWidgetItem *item = itemsById_.value(itemId, nullptr);
    const auto state = itemState(itemId);
    if (!item || !state) {
        return;
    }
    item->setData(kThumbnailRole, thumbnails_->contains(itemId)
                      ? thumbnails_->thumbnail(itemId)
                      : createThumbnailPlaceholder(*state));
    item->setData(kThumbnailErrorRole, thumbnails_->errorDetail(itemId));
    thumbnails_->request(itemId);
    updateItemDisplay(itemId);
    list_->viewport()->update(list_->visualItemRect(item));
}

void PlaylistPanel::updateItemCount()
{
    itemCountLabel_->setText(
        vtrpon::uiText("playlist.item_count").arg(model_->itemCount()));
}

void PlaylistPanel::updateItemDisplay(quint64 itemId)
{
    QListWidgetItem *item = itemsById_.value(itemId, nullptr);
    const auto state = itemState(itemId);
    if (!item || !state) {
        return;
    }

    const QString fileName = state->displayName.isEmpty()
        ? QFileInfo(state->filePath).fileName() : state->displayName;
    const bool powerPoint =
        state->sourceType == PlaylistSourceType::PowerPoint;
    const bool pdf = state->sourceType == PlaylistSourceType::Pdf;
    const auto conversion = model_->itemConversionState(itemId);
    const bool live = !conversion &&
        state->sourceType != PlaylistSourceType::File;
    const bool controlsUnavailable = live || conversion.has_value();
    const bool mediaOffline = model_->itemMediaOffline(itemId);
    const PlaylistPowerPointMode powerPointMode =
        effectivePowerPointMode(*state);
    const QString powerPointModeLabel =
        powerPointModeBadgeLabel(powerPointMode);
    const int currentPlayCount = model_->itemCurrentPlayCount(itemId);
    const QString unavailable(QChar(0x2014));
    const QString points = conversion
        ? unavailable
        : live ? vtrpon::uiText("playlist.item.live")
               : formatItemPoints(state->settings);
    const QString modes = conversion
        ? conversionStatusText(*conversion)
        : powerPoint
              ? vtrpon::uiText("playlist.item.powerpoint_controls_disabled")
              : pdf ? vtrpon::uiText("playlist.item.pdf_controls_disabled")
                     : live
                          ? formatItemStartMode(state->settings) + QStringLiteral(" | ")
                                + formatItemVolume(state->settings)
                          : formatItemModes(state->settings,
                                            currentPlayCount);
    const QString displayText = QStringLiteral("%1  %2\n%3\n%4")
        .arg(state->itemNumber)
        .arg(fileName, points, modes);
    item->setText(displayText);
    item->setData(kItemNumberRole, state->itemNumber);
    item->setData(kItemNameRole, fileName);
    item->setData(kItemInPointRole,
                  conversion
                      ? unavailable
                      : live ? vtrpon::uiText("playlist.item.live")
                             : formatPlaylistPoint(
                                   state->settings.inPointMs));
    item->setData(kItemOutPointRole,
                  controlsUnavailable
                      ? unavailable
                      : formatPlaylistPoint(
                            state->settings.outPointMs.value_or(0)));
    const QString extension = QFileInfo(state->filePath).suffix().toUpper();
    const QString codec = controlsUnavailable ? QString() : thumbnails_->sourceCodec(itemId);
    const QString fileType = extension.isEmpty() ? unavailable : extension;
    item->setData(kItemMediaTypeRole,
                  state->sourceType == PlaylistSourceType::Uvc
                      ? vtrpon::uiText(vtrpon::isNetworkVideoSource(state->filePath)
                            ? "playlist.item.network_live" : "playlist.item.uvc_live")
                      : codec.isEmpty() ? fileType
                                        : QStringLiteral("%1 (%2)").arg(fileType, codec));
    // File attributes describe the source, independently of the editable OUT.
    const QSize sourceSize = thumbnails_->sourceSize(itemId);
    const auto sourceDuration = thumbnails_->sourceDurationMs(itemId);
    const auto outPoint = state->settings.outPointMs.has_value()
        ? state->settings.outPointMs : sourceDuration;
    item->setData(kItemResolutionRole,
                  !controlsUnavailable && !sourceSize.isEmpty()
                      ? QStringLiteral("%1x%2").arg(sourceSize.width()).arg(sourceSize.height())
                      : unavailable);
    item->setData(kItemDurationRole,
                  !controlsUnavailable && outPoint
                      ? formatPlaylistPoint((std::max)(qint64{0},
                            *outPoint - state->settings.inPointMs)) : unavailable);
    item->setData(kItemVolumeRole,
                  (controlsUnavailable && state->sourceType != PlaylistSourceType::Uvc) ? unavailable
                      : QStringLiteral("%1%").arg(
                          state->settings.nominalVolumeTenthsDb == kMinimumItemVolumeTenthsDb
                              ? 0 : qRound(100.0 * std::pow(10.0,
                                  state->settings.nominalVolumeTenthsDb / 200.0))));
    item->setData(kItemStartModeRole,
                  (controlsUnavailable && state->sourceType != PlaylistSourceType::Uvc)
                      ? unavailable
                      : formatItemStartMode(state->settings).toUpper());
    item->setData(kItemEndModeRole,
                  controlsUnavailable
                      ? unavailable
                      : vtrpon::formatPlaylistItemEndMode(
                            state->settings, currentPlayCount).toUpper());
    item->setData(kItemLiveRole, controlsUnavailable);
    item->setData(kItemColorRole, static_cast<int>(state->color));
    item->setData(kItemSeparatorBelowRole, state->separatorBelow);
    item->setData(kItemMediaOfflineRole, mediaOffline);
    item->setData(kPowerPointModeRole, static_cast<int>(powerPointMode));
    item->setData(kItemConversionActiveRole, conversion.has_value());
    item->setData(kItemConversionProgressRole,
                  conversion ? (conversion->stage == PlaylistItemConversionStage::AnalysisWaiting ||
                                conversion->stage == PlaylistItemConversionStage::Analyzing
                                    ? -1 : conversion->progressPermille) : 0);
    item->setData(kItemConversionStageRole,
                  conversion ? conversionStageText(conversion->stage)
                             : QString());
    const bool bgmMode =
        model_->playlistMode(state->playlistNumber) == PlaylistMode::Bgm;
    item->setData(kItemBgmModeRole, bgmMode);
    const QString powerPointModeSummary = powerPointModeLabel.isEmpty()
        ? QString()
        : vtrpon::uiText("playlist.item.powerpoint_mode")
              .arg(powerPointModeLabel);
    QString accessibleText = displayText;
    accessibleText += QStringLiteral("\nRES %1  DUR %2  VOL %3  TYPE %4")
        .arg(item->data(kItemResolutionRole).toString(),
             item->data(kItemDurationRole).toString(),
             item->data(kItemVolumeRole).toString(),
             item->data(kItemMediaTypeRole).toString());
    if (!powerPointModeSummary.isEmpty()) {
        accessibleText += QLatin1Char('\n') + powerPointModeSummary;
    }
    if (mediaOffline) {
        accessibleText += QStringLiteral("\nMEDIA OFFLINE");
    }
    if (bgmMode) {
        accessibleText += QLatin1Char('\n') +
                          vtrpon::uiText("playlist.item.bgm_mode");
    }
    item->setData(Qt::AccessibleTextRole, accessibleText);
}

void PlaylistPanel::updateItemVisual(quint64 itemId)
{
    QListWidgetItem *item = itemsById_.value(itemId, nullptr);
    if (!item) {
        return;
    }
    const bool onAir = itemId == model_->onAirItemId();
    const bool dsk = itemId == model_->dskItemId();
    const bool edit = itemId == model_->editItemId();
    int visualState = ItemVisualNone;
    if (onAir) {
        visualState |= ItemVisualOnAir;
    }
    if (dsk) {
        visualState |= ItemVisualDsk;
    }
    if (edit) {
      visualState |= ItemVisualEdit;
    }
    if (itemId == model_->bgmItemId()) {
      visualState |= ItemVisualBgmPlay;
    }
    if (item->data(kItemVisualStateRole).toInt() != visualState) {
        item->setData(kItemVisualStateRole, visualState);
    }
    StatusIconState iconState = StatusIconState::None;
    if (onAir) {
        iconState = StatusIconState::OnAir;
    } else if (itemId == model_->cueItemId()) {
        if (model_->cueState() == PlaylistCueState::Ready) {
            iconState = StatusIconState::CueReady;
        } else if (model_->cueState() == PlaylistCueState::Error) {
            iconState = StatusIconState::CueError;
        } else if (model_->cueState() == PlaylistCueState::Pending
                   || model_->cueState() == PlaylistCueState::Loading) {
            iconState = StatusIconState::CuePending;
        }
    }
    if (item->data(kStatusIconStateRole).toInt()
        != static_cast<int>(iconState)) {
        item->setData(kStatusIconStateRole, static_cast<int>(iconState));
        item->setIcon(statusIcon(iconState));
    }
    QFont font = item->font();
    font.setBold(onAir || dsk || edit
                 || iconState == StatusIconState::CueReady);
    item->setFont(font);
    const auto state = itemState(itemId);
    const bool powerPoint =
        state && state->sourceType == PlaylistSourceType::PowerPoint;
    const bool pdf = state && state->sourceType == PlaylistSourceType::Pdf;
    const auto conversion = model_->itemConversionState(itemId);
    const bool live = state && !conversion &&
        state->sourceType != PlaylistSourceType::File;
    QString description = conversion
        ? conversionStatusText(*conversion)
        : powerPoint
        ? vtrpon::uiText("playlist.item.powerpoint_description")
        : pdf ? vtrpon::uiText("playlist.item.pdf_description")
        : live ? vtrpon::uiText("playlist.item.uvc_description")
               : vtrpon::uiText("playlist.item.description");
    if (onAir) {
        description = vtrpon::uiText("playlist.state.on_air");
    } else if (iconState == StatusIconState::CueReady) {
        description = vtrpon::uiText("playlist.state.cue_ready");
    } else if (iconState == StatusIconState::CuePending) {
        description = vtrpon::uiText("playlist.state.cue_pending");
    } else if (iconState == StatusIconState::CueError) {
        description = vtrpon::uiText("playlist.state.cue_failed");
    }
    QStringList activeStates;
    if (onAir) {
        activeStates.append(vtrpon::uiText("playlist.on_air"));
    }
    if (dsk) {
        activeStates.append(QStringLiteral("DSK"));
    }
    if (edit) {
        activeStates.append(QStringLiteral("EDIT"));
    }
    if (!activeStates.isEmpty()) {
        description += vtrpon::uiText("playlist.item.state_suffix").arg(
            activeStates.join(QStringLiteral(", ")));
    }
    if (state) {
        const QString powerPointModeLabel = powerPointModeBadgeLabel(
            effectivePowerPointMode(*state));
        if (!powerPointModeLabel.isEmpty()) {
            description += QLatin1Char(' ')
                + vtrpon::uiText("playlist.item.powerpoint_mode")
                      .arg(powerPointModeLabel);
        }
        description += conversion
            ? QString()
            : live
            ? QStringLiteral(" LIVE")
            : QLatin1Char(' ') + formatItemPoints(state->settings)
                  + QLatin1Char(' ') + formatItemModes(
                        state->settings,
                        model_->itemCurrentPlayCount(itemId));
        if (state->color != PlaylistItemColor::Default) {
            description += vtrpon::uiText("playlist.item.color_suffix")
                               .arg(playlistItemColorName(state->color));
        }
        if (state->separatorBelow) {
            description +=
                vtrpon::uiText("playlist.item.separator_below_suffix");
        }
    }
    item->setData(Qt::AccessibleDescriptionRole, description);
}

void PlaylistPanel::updateUserInfo()
{
    if (!userInfoCenter_) {
        return;
    }

    const quint64 selectedId = selectedItemId();
    const char *statusTextId = nullptr;
    QString detail;
    if (selectedId != 0 && selectedId == model_->onAirItemId()) {
        detail = model_->onAirErrorDetail();
        if (!detail.isEmpty()) {
            statusTextId = "playlist.error.on_air";
        }
    } else if (model_->cueItemId() != 0
               && model_->cueItemId() == selectedId) {
        detail = model_->cueDetail();
        if (model_->cueState() == PlaylistCueState::Error) {
            statusTextId = "playlist.error.cue";
        }
    }

    if (!statusTextId) {
        publishedUserInfoKey_.clear();
        return;
    }
    const QString publishedKey =
        QStringLiteral("%1\n%2\n%3")
            .arg(selectedId)
            .arg(QString::fromLatin1(statusTextId), detail);
    if (publishedUserInfoKey_ == publishedKey) {
        return;
    }
    publishedUserInfoKey_ = publishedKey;
    userInfoCenter_->showError(QStringLiteral("PLAYLIST"),
                               vtrpon::LocalizedText(statusTextId),
                               detail);
}
