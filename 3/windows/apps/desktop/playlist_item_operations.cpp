#include "playlist_item_operations.h"
#include "controller_operations.h"
#include "playlist_model.h"

PlaylistItemOperations::PlaylistItemOperations(PlaylistModel *model,
                                               ControllerOperations *operations,
                                               QObject *parent)
    : QObject(parent), model_(model)
{
    using A = ControllerOperations::Action;
    operations->bind<int>(A::ActivatePlaylist, this, [this](int number) {
        return model_->setActivePlaylistNumber(number);
    });
    operations->bind<>(A::CreatePlaylist, this, [this] {
        const int number = model_->createPlaylist();
        return number != 0 && model_->setActivePlaylistNumber(number);
    });
    operations->bindMember(A::CopyItemState, this, &PlaylistItemOperations::copyItemState);
    operations->bindMember(A::PasteItemState, this, &PlaylistItemOperations::pasteItemState);
    operations->bind<quint64, PlaylistStartMode>(A::SetItemStartMode, this,
        [this](quint64 id, PlaylistStartMode mode) {
            if (model_->itemConversionInProgress(id)) return false;
            const auto state = model_->itemState(id);
            if (!state) return false;
            auto settings = state->settings;
            settings.startMode = mode;
            return model_->updateItemSettings(id, settings);
        });
    operations->bind<quint64, PlaylistEndMode>(A::SetItemEndMode, this,
        [this](quint64 id, PlaylistEndMode mode) {
            if (model_->itemConversionInProgress(id)) return false;
            if (mode == PlaylistEndMode::Goto)
                return false;
            const auto state = model_->itemState(id);
            if (!state) return false;
            auto settings = state->settings;
            settings.endMode = mode;
            return model_->updateItemSettings(id, settings);
        });
    const auto bindFade = [this, operations](A action, bool PlaylistItemSettings::*field) {
        operations->bind<quint64, bool>(action, this,
            [this, field](quint64 id, bool enabled) {
                if (model_->itemConversionInProgress(id)) return false;
                const auto state = model_->itemState(id);
                if (!state) return false;
                auto settings = state->settings;
                settings.*field = enabled;
                return model_->updateItemSettings(id, settings);
            });
    };
    bindFade(A::SetItemFadeInEnabled, &PlaylistItemSettings::fadeInEnabled);
    bindFade(A::SetItemFadeOutEnabled, &PlaylistItemSettings::fadeOutEnabled);
    bindFade(A::SetItemEndCrossfadeEnabled, &PlaylistItemSettings::endCrossfadeEnabled);
    operations->bind<int, QString>(A::RenamePlaylist, this,
        [this](int number, const QString &name) { return model_->setPlaylistName(number, name); });
    operations->bind<quint64, QString>(A::SetItemDisplayName, this,
        [this](quint64 id, const QString &name) { return model_->setItemDisplayName(id, name); });
    operations->bind<quint64, PlaylistItemColor>(A::SetItemColor, this,
        [this](quint64 id, PlaylistItemColor color) { return model_->setItemColor(id, color); });
    operations->bind<quint64, bool>(A::SetItemSeparatorBelow, this,
        [this](quint64 id, bool enabled) { return model_->setItemSeparatorBelow(id, enabled); });
}

bool PlaylistItemOperations::allowItemAction(quint64 itemId, PlaylistItemAction action)
{
    if (!model_->itemConversionInProgress(itemId)) {
        return true;
    }
    emit actionBlocked(action);
    return false;
}

bool PlaylistItemOperations::copyItemState(quint64 itemId)
{
    if (!allowItemAction(itemId, PlaylistItemAction::CopyState)) {
        return false;
    }
    const auto state = model_->itemState(itemId);
    if (!state) {
        return false;
    }
    copiedItemState_ = CopiedItemState{state->settings.startMode,
                                       state->settings.endMode,
                                       state->settings.fadeInEnabled,
                                       state->settings.fadeOutEnabled,
                                       state->settings.endCrossfadeEnabled};
    return true;
}

bool PlaylistItemOperations::pasteItemState(quint64 itemId)
{
    if (!allowItemAction(itemId, PlaylistItemAction::PasteState)) {
        return false;
    }
    const auto state = model_->itemState(itemId);
    if (!state || !copiedItemState_) {
        return false;
    }

    PlaylistItemSettings settings = state->settings;
    settings.startMode = copiedItemState_->startMode;
    settings.endMode = copiedItemState_->endMode;
    settings.fadeInEnabled = copiedItemState_->fadeInEnabled;
    settings.fadeOutEnabled = copiedItemState_->fadeOutEnabled;
    settings.endCrossfadeEnabled = copiedItemState_->endCrossfadeEnabled;
    return model_->updateItemSettings(itemId, settings);
}
