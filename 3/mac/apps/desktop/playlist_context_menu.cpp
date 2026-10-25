#include "playlist_context_menu.h"
#include "playlist_item_operations.h"
#include "controller_operations.h"
#include "playlist_model.h"
#include "playlist_item_delegate.h"
#include "playlist_item_display.h"
#include "controller_scaling.h"
#include "language_text.h"
#include <QDialog>
#include <QActionGroup>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTableWidget>
#include <QVBoxLayout>
#include <array>

using namespace playlist_view;

PlaylistContextMenu::PlaylistContextMenu(PlaylistModel *model,
                                         PlaylistItemOperations *itemOperations,
                                         ControllerOperations *operations,
                                         QWidget *parent)
    : QObject(parent), model_(model), itemOperations_(itemOperations),
      operations_(operations), parentWidget_(parent)
{
}

void PlaylistContextMenu::showItemContextMenu(quint64 itemId,
                                        const QPoint &globalPosition)
{
    const auto state = model_->itemState(itemId);
    if (!state) {
        return;
    }

    QMenu menu(parentWidget_);
    menu.setObjectName(QStringLiteral("playlistItemContextMenu"));

    QMenu *startMenu = menu.addMenu(vtrpon::uiText("playlist.map.start_mode"));
    startMenu->setObjectName(QStringLiteral("playlistItemStartModeMenu"));
    const bool modesEditable = !model_->itemSettingsLocked(itemId) &&
                               !model_->itemConversionInProgress(itemId);
    const bool live = state->sourceType == PlaylistSourceType::Uvc;
    startMenu->setEnabled(modesEditable);
    auto *startGroup = new QActionGroup(startMenu);
    for (const auto mode : {PlaylistStartMode::Pause, PlaylistStartMode::Play}) {
        auto *action = startMenu->addAction(vtrpon::uiText(
            mode == PlaylistStartMode::Pause ? "common.pause" : "common.play"));
        action->setObjectName(QStringLiteral("playlistItemStartModeAction%1")
                                  .arg(static_cast<int>(mode)));
        action->setCheckable(true);
        action->setChecked(state->settings.startMode == mode);
        action->setEnabled(!live);
        startGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, itemId, mode] {
            (void)operations_->execute(ControllerOperations::Action::SetItemStartMode,
                                       itemId, mode);
        });
    }
    QMenu *endMenu = menu.addMenu(vtrpon::uiText("playlist.map.end_mode"));
    endMenu->setObjectName(QStringLiteral("playlistItemEndModeMenu"));
    endMenu->setEnabled(modesEditable && !live);
    auto *endGroup = new QActionGroup(endMenu);
    for (const auto mode : {PlaylistEndMode::Stop, PlaylistEndMode::Pause,
                            PlaylistEndMode::Repeat, PlaylistEndMode::Next}) {
        auto *action = endMenu->addAction(vtrpon::playlistEndModeName(mode));
        action->setObjectName(QStringLiteral("playlistItemEndModeAction%1")
                                  .arg(static_cast<int>(mode)));
        action->setCheckable(true);
        action->setChecked(state->settings.endMode == mode);
        endGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, itemId, mode] {
            (void)operations_->execute(ControllerOperations::Action::SetItemEndMode,
                                       itemId, mode);
        });
    }
    const auto addFadeAction = [this, itemId](QMenu *submenu, const char *textId,
                                            const char *name, bool enabled,
                                            ControllerOperations::Action operation) {
        auto *action = submenu->addAction(vtrpon::uiText(textId));
        action->setObjectName(QString::fromLatin1(name));
        action->setCheckable(true);
        action->setChecked(enabled);
        connect(action, &QAction::triggered, this,
                [this, itemId, operation](bool checked) {
            (void)operations_->execute(operation, itemId, checked);
        });
    };
    startMenu->addSeparator();
    addFadeAction(startMenu, "edit.fade_in", "playlistItemFadeInAction",
                  state->settings.fadeInEnabled,
                  ControllerOperations::Action::SetItemFadeInEnabled);
    endMenu->addSeparator();
    addFadeAction(endMenu, "edit.fade_out", "playlistItemFadeOutAction",
                  state->settings.fadeOutEnabled,
                  ControllerOperations::Action::SetItemFadeOutEnabled);
    addFadeAction(endMenu, "edit.end_crossfade", "playlistItemEndCrossfadeAction",
                  state->settings.endCrossfadeEnabled,
                  ControllerOperations::Action::SetItemEndCrossfadeEnabled);
    menu.addSeparator();

    QAction *copyStateAction = menu.addAction(
        vtrpon::uiText("playlist.context.copy_state"));
    copyStateAction->setObjectName(
        QStringLiteral("playlistItemCopyStateAction"));
    connect(copyStateAction, &QAction::triggered, this,
            [this, itemId] { (void)operations_->execute(ControllerOperations::Action::CopyItemState, itemId); });

    QAction *pasteStateAction = menu.addAction(
        vtrpon::uiText("playlist.context.paste_state"));
    pasteStateAction->setObjectName(
        QStringLiteral("playlistItemPasteStateAction"));
    pasteStateAction->setEnabled(itemOperations_->hasCopiedState() &&
                                  !model_->itemSettingsLocked(itemId) && !live);
    connect(pasteStateAction, &QAction::triggered, this,
            [this, itemId] { (void)operations_->execute(ControllerOperations::Action::PasteItemState, itemId); });

    menu.addSeparator();
    QAction *renameAction = menu.addAction(
        vtrpon::uiText("playlist.context.rename"));
    renameAction->setObjectName(QStringLiteral("playlistItemRenameAction"));

    QMenu *colorMenu = menu.addMenu(
        vtrpon::uiText("playlist.context.color"));
    colorMenu->setObjectName(QStringLiteral("playlistItemColorMenu"));
    constexpr std::array<PlaylistItemColor, 5> colors{
        PlaylistItemColor::Default,
        PlaylistItemColor::Red,
        PlaylistItemColor::Yellow,
        PlaylistItemColor::Blue,
        PlaylistItemColor::Green,
    };
    for (const PlaylistItemColor color : colors) {
        QAction *colorAction = colorMenu->addAction(
            playlistItemColorIcon(color), playlistItemColorName(color));
        colorAction->setObjectName(
            QStringLiteral("playlistItemColorAction%1")
                .arg(static_cast<int>(color)));
        colorAction->setCheckable(true);
        colorAction->setChecked(state->color == color);
        colorAction->setProperty("playlistItemColor",
                                 static_cast<int>(color));
    }

    menu.addSeparator();
    QAction *separatorAction = menu.addAction(
        vtrpon::uiText("playlist.context.separator_below"));
    separatorAction->setObjectName(
        QStringLiteral("playlistItemSeparatorBelowAction"));
    separatorAction->setCheckable(true);
    separatorAction->setChecked(state->separatorBelow);

    menu.addSeparator();
    QAction *mapAction = menu.addAction(
        vtrpon::uiText("playlist.context.map"));
    mapAction->setObjectName(QStringLiteral("playlistItemMapAction"));

    QAction *selectedAction = menu.exec(globalPosition);
    if (!selectedAction) {
        return;
    }
    if (selectedAction == renameAction) {
        renameItemDisplayName(itemId);
        return;
    }
    if (selectedAction == separatorAction) {
        (void)operations_->execute(ControllerOperations::Action::SetItemSeparatorBelow,
                                   itemId, selectedAction->isChecked());
        return;
    }
    if (selectedAction == mapAction) {
        showPlaylistMap(itemId);
        return;
    }
    const QVariant colorValue =
        selectedAction->property("playlistItemColor");
    if (colorValue.isValid()) {
        (void)operations_->execute(ControllerOperations::Action::SetItemColor,
            itemId, static_cast<PlaylistItemColor>(colorValue.toInt()));
    }
}

void PlaylistContextMenu::showPlaylistMap(quint64 itemId)
{
    if (!model_) {
        return;
    }
    const PlaylistMapAnalysis analysis =
        model_->analyzePlaySequence(itemId);

    QDialog dialog(parentWidget_);
    dialog.setObjectName(QStringLiteral("playlistMapDialog"));
    dialog.setWindowTitle(vtrpon::uiText("playlist.map.title"));
    dialog.setModal(true);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                               vtrpon::scalePx(12), vtrpon::scalePx(12));
    layout->setSpacing(vtrpon::scalePx(8));

    auto *table = new QTableWidget(analysis.rows.size(), 7, &dialog);
    table->setObjectName(QStringLiteral("playlistMapTable"));
    table->setAccessibleName(vtrpon::uiText("playlist.map.table.accessible"));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setHorizontalHeaderLabels({
        vtrpon::uiText("playlist.map.sequence"),
        vtrpon::uiText("playlist.map.playlist_number"),
        vtrpon::uiText("playlist.map.item_number"),
        vtrpon::uiText("playlist.map.item_name"),
        vtrpon::uiText("playlist.map.start_mode"),
        vtrpon::uiText("playlist.map.end_mode"),
        vtrpon::uiText("playlist.map.duration"),
    });
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch);

    const bool hasFiniteEndpoint =
        !analysis.infinite && analysis.totalDurationMs &&
        !analysis.rows.isEmpty();
    for (int rowIndex = 0; rowIndex < analysis.rows.size(); ++rowIndex) {
        const PlaylistMapRow &row = analysis.rows.at(rowIndex);
        const QString duration = row.infiniteDuration
            ? QString(QChar(0x221e))
            : formatPlaylistPoint(row.durationMs);
        const QStringList values{
            QString::number(row.sequenceNumber),
            QString::number(row.playlistNumber),
            QString::number(row.itemNumber),
            row.itemName,
            formatItemStartMode(row.settings),
            vtrpon::formatPlaylistItemEndMode(row.settings, 0),
            duration,
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *cell = new QTableWidgetItem(values.at(column));
            if (hasFiniteEndpoint &&
                rowIndex == analysis.rows.size() - 1) {
                QFont endpointFont = cell->font();
                endpointFont.setBold(true);
                cell->setFont(endpointFont);
                cell->setToolTip(
                    vtrpon::uiText("playlist.map.endpoint.tooltip"));
            }
            table->setItem(rowIndex, column, cell);
        }
    }
    layout->addWidget(table, 1);

    const QString note = playlistMapStopReasonText(analysis.stopReason);
    if (!note.isEmpty()) {
        auto *noteLabel = new QLabel(note, &dialog);
        noteLabel->setObjectName(QStringLiteral("playlistMapNote"));
        noteLabel->setWordWrap(true);
        layout->addWidget(noteLabel);
    }
    if (hasFiniteEndpoint) {
        const PlaylistMapRow &endpoint = analysis.rows.constLast();
        auto *endpointLabel = new QLabel(
            vtrpon::uiText("playlist.map.endpoint")
                .arg(endpoint.playlistNumber)
                .arg(endpoint.itemNumber)
                .arg(endpoint.itemName),
            &dialog);
        endpointLabel->setObjectName(
            QStringLiteral("playlistMapEndpoint"));
        endpointLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(endpointLabel);
    }

    const QString total = analysis.infinite
        ? QString(QChar(0x221e))
        : analysis.totalDurationMs
              ? formatPlaylistPoint(*analysis.totalDurationMs)
              : QString(QChar(0x2014));
    auto *totalLabel = new QLabel(
        vtrpon::uiText("playlist.map.total").arg(total), &dialog);
    totalLabel->setObjectName(QStringLiteral("playlistMapTotal"));
    QFont totalFont = totalLabel->font();
    totalFont.setBold(true);
    totalLabel->setFont(totalFont);
    totalLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(totalLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->setObjectName(QStringLiteral("playlistMapButtons"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(vtrpon::scaleSize(900, 500));
    dialog.exec();
}

void PlaylistContextMenu::renameItemDisplayName(quint64 itemId)
{
    const auto state = model_->itemState(itemId);
    if (!state) {
        return;
    }
    bool accepted = false;
    const QString displayName = QInputDialog::getText(
        parentWidget_, vtrpon::uiText("playlist.rename.title"),
        vtrpon::uiText("playlist.rename.label"), QLineEdit::Normal,
        state->displayName, &accepted);
    if (accepted) {
        (void)operations_->execute(ControllerOperations::Action::SetItemDisplayName, itemId, displayName);
    }
}
