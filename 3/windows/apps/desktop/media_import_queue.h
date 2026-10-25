#pragma once

#include "media_duration_probe.h"
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <functional>
#include <memory>

class PlaylistModel;
class QTimer;
struct PendingMediaImport;

// GUI-owned queue; only copied paths/results cross the worker boundary.
class MediaImportQueue final : public QObject {
  Q_OBJECT
public:
  using Probe = std::function<MediaDurationProbeResult(const QString &)>;
  explicit MediaImportQueue(PlaylistModel *model, QObject *parent = nullptr,
                            Probe probe = probeMediaDuration);
  ~MediaImportQueue() override;
  quint64 enqueue(const QString &path, int playlistNumber,
                  std::optional<qint64> imageDurationMs = std::nullopt);
  bool hasPendingImports() const;
  void cancelAll();
  void requestShutdown();
  bool shutdownComplete() const;

signals:
  void importsFailed(const QStringList &details);

private:
  void poll();
  QPointer<PlaylistModel> model_;
  Probe probe_;
  QTimer *timer_;
  QQueue<std::shared_ptr<PendingMediaImport>> queued_;
  std::shared_ptr<PendingMediaImport> active_;
  QStringList failures_;
  bool shuttingDown_ = false;
};
