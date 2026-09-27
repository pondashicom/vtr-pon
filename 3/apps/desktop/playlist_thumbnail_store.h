#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QSize>
#include <optional>

class QImage;
class PlaylistModel;
class MediaThumbnailer;

// Owns the cache and request lifetime for every playlist, including hidden tabs.
// MediaThumbnailer remains responsible for worker cancellation and request identity.
class PlaylistThumbnailStore final : public QObject
{
    Q_OBJECT
public:
    PlaylistThumbnailStore(PlaylistModel *model, bool enabled,
                           QObject *parent = nullptr);
    bool contains(quint64 itemId) const;
    QPixmap thumbnail(quint64 itemId) const;
    QString errorDetail(quint64 itemId) const;
    QSize sourceSize(quint64 itemId) const;
    std::optional<qint64> sourceDurationMs(quint64 itemId) const;
    QString sourceCodec(quint64 itemId) const;
    void request(quint64 itemId);
    void setThumbnail(quint64 itemId, const QImage &image,
                      const QString &errorDetail);

signals:
    void thumbnailChanged(quint64 itemId);

private:
    void invalidate(quint64 itemId);
    PlaylistModel *model_;
    MediaThumbnailer *thumbnailer_;
    QHash<quint64, QPixmap> thumbnailsById_;
    QHash<quint64, QString> errorsById_;
    QHash<quint64, QSize> sourceSizesById_;
    QHash<quint64, qint64> sourceDurationsById_;
    QHash<quint64, QString> sourceCodecsById_;
};
