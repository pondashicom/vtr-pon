#include "playlist_thumbnail_store.h"
#include "media_thumbnailer.h"
#include "playlist_model.h"

PlaylistThumbnailStore::PlaylistThumbnailStore(PlaylistModel *model, bool enabled,
                                               QObject *parent)
    : QObject(parent), model_(model),
      thumbnailer_(enabled ? new MediaThumbnailer(this) : nullptr)
{
    if (thumbnailer_) {
        connect(thumbnailer_, &MediaThumbnailer::thumbnailReady, this,
                [this](quint64 id, const MediaThumbnailResult &result) {
            if (!model_->itemState(id)) return;
            sourceSizesById_.insert(id, result.sourceSize);
            sourceCodecsById_.insert(id, result.sourceCodec);
            if (result.sourceDurationMs) {
                sourceDurationsById_.insert(id, *result.sourceDurationMs);
            } else {
                sourceDurationsById_.remove(id);
            }
            setThumbnail(id, result.image,
                         result.succeeded ? QString() : result.errorDetail);
        });
    }
    connect(model_, &PlaylistModel::itemRemoved, this,
            [this](quint64 id, int) { invalidate(id); });
    connect(model_, &PlaylistModel::itemReplaced, this, [this](quint64 id) {
        invalidate(id);
        emit thumbnailChanged(id);
    });
    connect(model_, &PlaylistModel::itemMediaAvailabilityChanged, this,
            [this](quint64 id, bool offline) {
        if (offline) {
            invalidate(id);
        } else {
            request(id);
        }
        emit thumbnailChanged(id);
    });
    connect(model_, &PlaylistModel::modelReset, this, [this] {
        if (thumbnailer_) {
            thumbnailer_->cancelAllThumbnails();
        }
        thumbnailsById_.clear();
        errorsById_.clear();
        sourceSizesById_.clear();
        sourceDurationsById_.clear();
        sourceCodecsById_.clear();
    });
}

bool PlaylistThumbnailStore::contains(quint64 itemId) const
{
    return thumbnailsById_.contains(itemId);
}

QPixmap PlaylistThumbnailStore::thumbnail(quint64 itemId) const
{
    return thumbnailsById_.value(itemId);
}

QString PlaylistThumbnailStore::errorDetail(quint64 itemId) const
{
    return errorsById_.value(itemId);
}

QSize PlaylistThumbnailStore::sourceSize(quint64 itemId) const
{
    return sourceSizesById_.value(itemId);
}

std::optional<qint64> PlaylistThumbnailStore::sourceDurationMs(quint64 itemId) const
{
    const auto it = sourceDurationsById_.constFind(itemId);
    return it == sourceDurationsById_.cend() ? std::nullopt
                                            : std::optional<qint64>(*it);
}

QString PlaylistThumbnailStore::sourceCodec(quint64 itemId) const
{
    return sourceCodecsById_.value(itemId);
}

void PlaylistThumbnailStore::request(quint64 itemId)
{
    if (!thumbnailer_ || contains(itemId) || model_->itemMediaOffline(itemId)
        || model_->itemConversionInProgress(itemId)) {
        return;
    }
    const auto state = model_->itemState(itemId);
    if (state && state->sourceType != PlaylistSourceType::Uvc) {
        thumbnailer_->requestThumbnail(itemId, state->filePath, state->mediaType,
                                       state->settings.outPointMs);
    }
}

void PlaylistThumbnailStore::setThumbnail(quint64 itemId, const QImage &image,
                                          const QString &errorDetail)
{
    if (itemId == 0 || image.isNull() || !model_->itemState(itemId)) {
        return;
    }
    thumbnailsById_.insert(itemId, QPixmap::fromImage(image));
    errorsById_.insert(itemId, errorDetail);
    emit thumbnailChanged(itemId);
}

void PlaylistThumbnailStore::invalidate(quint64 itemId)
{
    if (thumbnailer_) {
        thumbnailer_->cancelThumbnail(itemId);
    }
    thumbnailsById_.remove(itemId);
    errorsById_.remove(itemId);
    sourceSizesById_.remove(itemId);
    sourceDurationsById_.remove(itemId);
    sourceCodecsById_.remove(itemId);
}
