#pragma once

#include <QObject>

#ifdef Q_OS_WIN
#include "powerpoint_add_method_dialog.h"

#include <QHash>
#include <QList>
#include <QString>
#include <memory>
#endif

class BackgroundIoQueue;
class PlaylistModel;
class ProgramPlayoutController;
class UserInfoCenter;
struct PdfAddSettings;
enum class PlaylistItemConversionStage;

// GUI-owned document jobs. Workers only produce results and post progress;
// playlist mutations and PowerPoint playback coordination stay on the GUI
// thread. The model, controllers, info center and file queue must outlive this
// queue.
class DocumentImportQueue final : public QObject {
  Q_OBJECT

public:
  DocumentImportQueue(PlaylistModel *playlistModel,
                      ProgramPlayoutController *programController,
                      UserInfoCenter *userInfoCenter, BackgroundIoQueue *fileIo,
                      QObject *parent = nullptr);
  ~DocumentImportQueue() override;

  bool hasPendingImports() const;
  bool contains(quint64 itemId) const;
  void cancel(quint64 itemId, bool removePlaceholder);
  void cancelAll(bool removePlaceholders);
  // Enqueue a complete batch before starting it, preserving playlist order.
  void startNext();
  void requestShutdown();
  // Completion includes GUI-thread finalization, not just worker termination.
  bool shutdownComplete() const;
  void waitForActiveImport();

#ifdef Q_OS_WIN
  bool enqueuePowerPoint(const QString &filePath,
                         const PowerPointAddSettings &settings,
                         int playlistNumber, QString *errorDetail);
  bool enqueuePdf(const QString &filePath, const PdfAddSettings &settings,
                  int playlistNumber, QString *errorDetail);

signals:
  void powerPointSettingsRemembered(const PowerPointAddSettings &settings);
#endif

private:
#ifdef Q_OS_WIN
  struct PendingDocumentImport;
  void updateProgress(quint64 itemId, PlaylistItemConversionStage stage,
                      int progressPermille);
  void finishImport(const std::shared_ptr<PendingDocumentImport> &import);

  QList<std::shared_ptr<PendingDocumentImport>> documentImportQueue_;
  QHash<quint64, std::shared_ptr<PendingDocumentImport>>
      documentImportsByItemId_;
  std::shared_ptr<PendingDocumentImport> activeDocumentImport_;
#endif
  PlaylistModel *playlistModel_;
  ProgramPlayoutController *programController_;
  UserInfoCenter *userInfoCenter_;
  BackgroundIoQueue *fileIo_;
  bool shuttingDown_ = false;
};
