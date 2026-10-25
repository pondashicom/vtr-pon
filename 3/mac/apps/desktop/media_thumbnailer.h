#pragma once

#include "playlist_item.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>

#include <atomic>
#include <memory>
#include <optional>

struct MediaThumbnailResult {
  QImage image;
  PlaylistMediaType mediaType = PlaylistMediaType::Unknown;
  bool succeeded = false;
  QString errorDetail;
  QSize sourceSize;
  std::optional<qint64> sourceDurationMs;
  QString sourceCodec;
};

MediaThumbnailResult generateMediaThumbnail(
    const QString &filePath, PlaylistMediaType mediaType,
    std::optional<qint64> durationMs = std::nullopt,
    const QSize &targetSize = QSize(120, 68),
    bool preferAudioAlbumArt = false);
MediaThumbnailResult generateUvcThumbnail(
    const QString &devicePath, const QSize &targetSize = QSize(120, 68));
QImage generateLiveAudioThumbnail(const QSize &targetSize = QSize(240, 136));

class MediaThumbnailer final : public QObject {
  Q_OBJECT

public:
  explicit MediaThumbnailer(QObject *parent = nullptr);
  ~MediaThumbnailer() override;

  MediaThumbnailer(const MediaThumbnailer &) = delete;
  MediaThumbnailer &operator=(const MediaThumbnailer &) = delete;

  void requestThumbnail(quint64 itemId, const QString &filePath,
                        PlaylistMediaType mediaType,
                        std::optional<qint64> durationMs = std::nullopt,
                        bool preferAudioAlbumArt = false);
  void requestUvcThumbnail(quint64 requestId, const QString &devicePath);
  void cancelThumbnail(quint64 itemId);
  void cancelAllThumbnails();
  void requestShutdown();
  bool shutdownComplete();

signals:
  void thumbnailReady(quint64 itemId, const MediaThumbnailResult &result);

private:
  friend class MediaThumbnailerLifecycleTest;
  struct PendingThumbnail {
    QString filePath;
    PlaylistMediaType mediaType = PlaylistMediaType::Unknown;
    std::optional<qint64> durationMs;
    bool preferAudioAlbumArt = false;
    std::atomic_bool canceled = false;
  };

  // File requests are managed on this QObject's thread. Workers only read
  // their immutable request and its atomic cancellation flag.
  QHash<quint64, std::shared_ptr<PendingThumbnail>> pendingThumbnails_;
  QThreadPool workerPool_;
  std::atomic_bool shuttingDown_ = false;
};
