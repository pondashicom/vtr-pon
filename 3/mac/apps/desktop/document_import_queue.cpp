#include "document_import_queue.h"
#include "file_management_settings.h"

#include "background_io_queue.h"
#include "playlist_model.h"
#include "program_playout_controller.h"
#include "user_info.h"

#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
#include "pdf_import_service.h"
#include "powerpoint_import_service.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <atomic>
#endif

DocumentImportQueue::DocumentImportQueue(
    PlaylistModel *playlistModel, ProgramPlayoutController *programController,
    UserInfoCenter *userInfoCenter, BackgroundIoQueue *fileIo, QObject *parent)
    : QObject(parent), playlistModel_(playlistModel),
      programController_(programController), userInfoCenter_(userInfoCenter),
      fileIo_(fileIo) {}

DocumentImportQueue::~DocumentImportQueue() {
  requestShutdown();
  waitForActiveImport();
}

bool DocumentImportQueue::hasPendingImports() const {
#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
  return !documentImportsByItemId_.isEmpty();
#else
  return false;
#endif
}

bool DocumentImportQueue::contains(quint64 itemId) const {
#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
  return documentImportsByItemId_.contains(itemId);
#else
  Q_UNUSED(itemId);
  return false;
#endif
}

void DocumentImportQueue::requestShutdown() {
  shuttingDown_ = true;
  cancelAll(true);
}

bool DocumentImportQueue::shutdownComplete() const {
#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
  return !activeDocumentImport_ && documentImportQueue_.isEmpty() &&
         documentImportsByItemId_.isEmpty();
#else
  return true;
#endif
}

#if !defined(Q_OS_WIN) && !defined(VTRPON_PDF_SUPPORT)
void DocumentImportQueue::cancel(quint64 itemId, bool removePlaceholder) {
  Q_UNUSED(itemId);
  Q_UNUSED(removePlaceholder);
}

void DocumentImportQueue::startNext() {}
#endif

#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
enum class DocumentImportKind { PowerPoint, Pdf };

struct DocumentImportQueue::PendingDocumentImport {
  DocumentImportKind kind = DocumentImportKind::Pdf;
  quint64 itemId = 0;
  QString filePath;
  QString outputRoot;
  PowerPointAddSettings powerPointSettings;
  PdfAddSettings pdfSettings;
  std::shared_ptr<std::atomic_bool> cancelRequested =
      std::make_shared<std::atomic_bool>(false);
  QThread *thread = nullptr;
  bool isNative() const {
    return kind == DocumentImportKind::PowerPoint
               ? powerPointSettings.method ==
                     PowerPointAddMethod::NativeSlideShow
               : pdfSettings.method == PdfAddMethod::Native;
  }
  bool powerPointPrepared = false;
  bool canceledByRemoval = false;
  PowerPointImportResult powerPointResult;
  PdfImportResult pdfResult;
};

#ifdef Q_OS_WIN
bool DocumentImportQueue::enqueuePowerPoint(
    const QString &filePath, const PowerPointAddSettings &settings,
    int playlistNumber, QString *errorDetail) {
  if (shuttingDown_) {
    if (errorDetail)
      *errorDetail = QStringLiteral("Document import is shutting down.");
    return false;
  }
  PlaylistItemRegistration placeholder;
  placeholder.filePath = filePath;
  placeholder.displayName = QFileInfo(filePath).fileName();
  placeholder.sourceType = PlaylistSourceType::PowerPoint;
  placeholder.mediaType = PlaylistMediaType::Presentation;
  placeholder.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
  placeholder.settings.startMode = PlaylistStartMode::Play;
  placeholder.settings.endMode = PlaylistEndMode::Pause;
  placeholder.playlistNumber = playlistNumber;
  placeholder.conversionState = PlaylistItemConversionState{};
  const bool native = settings.method == PowerPointAddMethod::NativeSlideShow;
  QList<quint64> ids;
  if (native) {
    const quint64 id = playlistModel_->addPendingMediaItem(
        filePath, playlistNumber, PlaylistSourceType::PowerPoint);
    if (id != 0)
      ids.append(id);
  } else {
    ids = playlistModel_->addItemsAtomically({placeholder});
  }
  if (ids.size() != 1) {
    if (errorDetail) {
      *errorDetail = QStringLiteral(
          "The PowerPoint conversion item could not be registered.");
    }
    return false;
  }

  auto import = std::make_shared<PendingDocumentImport>();
  import->kind = DocumentImportKind::PowerPoint;
  import->itemId = ids.constFirst();
  import->filePath = QFileInfo(filePath).absoluteFilePath();
  import->powerPointSettings = settings;
  documentImportQueue_.append(import);
  documentImportsByItemId_.insert(import->itemId, import);
  if (errorDetail) {
    errorDetail->clear();
  }
  return true;
}

#endif

bool DocumentImportQueue::enqueuePdf(const QString &filePath,
                                     const PdfAddSettings &settings,
                                     int playlistNumber, QString *errorDetail) {
  if (shuttingDown_) {
    if (errorDetail)
      *errorDetail = QStringLiteral("Document import is shutting down.");
    return false;
  }
  PlaylistItemRegistration placeholder;
  placeholder.filePath = filePath;
  placeholder.displayName = QFileInfo(filePath).completeBaseName();
  placeholder.sourceType = PlaylistSourceType::Pdf;
  placeholder.mediaType = PlaylistMediaType::Pdf;
  placeholder.settings.startMode = PlaylistStartMode::Play;
  placeholder.settings.endMode = PlaylistEndMode::Pause;
  placeholder.playlistNumber = playlistNumber;
  placeholder.conversionState = PlaylistItemConversionState{};
  const bool native = settings.method == PdfAddMethod::Native;
  QList<quint64> ids;
  if (native) {
    const quint64 id = playlistModel_->addPendingMediaItem(
        filePath, playlistNumber, PlaylistSourceType::Pdf);
    if (id != 0)
      ids.append(id);
  } else {
    ids = playlistModel_->addItemsAtomically({placeholder});
  }
  if (ids.size() != 1) {
    if (errorDetail) {
      *errorDetail =
          QStringLiteral("The PDF conversion item could not be registered.");
    }
    return false;
  }

  auto import = std::make_shared<PendingDocumentImport>();
  import->kind = DocumentImportKind::Pdf;
  import->itemId = ids.constFirst();
  import->filePath = QFileInfo(filePath).absoluteFilePath();
  import->pdfSettings = settings;
  documentImportQueue_.append(import);
  documentImportsByItemId_.insert(import->itemId, import);
  if (errorDetail) {
    errorDetail->clear();
  }
  return true;
}

void DocumentImportQueue::updateProgress(quint64 itemId,
                                         PlaylistItemConversionStage stage,
                                         int progressPermille) {
  if (!documentImportsByItemId_.contains(itemId) || !playlistModel_) {
    return;
  }
  (void)playlistModel_->setItemConversionProgress(itemId, stage,
                                                  progressPermille);
}

void DocumentImportQueue::startNext() {
  if (shuttingDown_ || activeDocumentImport_) {
    return;
  }
  while (!documentImportQueue_.isEmpty()) {
    const auto import = documentImportQueue_.takeFirst();
    if (!documentImportsByItemId_.contains(import->itemId) ||
        !playlistModel_->itemState(import->itemId)) {
      continue;
    }
    activeDocumentImport_ = import;
    updateProgress(import->itemId,
                   import->isNative() ? PlaylistItemConversionStage::Analyzing
                                      : PlaylistItemConversionStage::Preparing,
                   0);

#ifdef Q_OS_WIN
    if (import->kind == DocumentImportKind::PowerPoint && !import->isNative()) {
      QString preparationError;
      vtrpon::LocalizedText preparationMessage;
      if (!programController_->beginPowerPointConversion(
              import->cancelRequested.get(), &preparationError,
              &preparationMessage)) {
        documentImportsByItemId_.remove(import->itemId);
        activeDocumentImport_.reset();
        (void)playlistModel_->removeItem(import->itemId);
        userInfoCenter_->showWarning(QStringLiteral("PLAYLIST"),
                                     preparationMessage, preparationError);
        QTimer::singleShot(0, this, [this] { startNext(); });
        return;
      }
      import->powerPointPrepared = true;
    }

#endif
    import->outputRoot = vtrpon::documentConversionDirectory(
        import->kind == DocumentImportKind::PowerPoint ? QStringLiteral("PowerPoint") : QStringLiteral("PDF"));
    if (import->isNative()) {
      import->thread = QThread::create([import] {
#ifdef Q_OS_WIN
        if (import->kind == DocumentImportKind::PowerPoint) {
          import->powerPointResult = PowerPointImportService::importFile(
              import->filePath, import->powerPointSettings, 1, nullptr,
              import->cancelRequested.get());
        } else
#endif
        {
          import->pdfResult = PdfImportService::importFile(
              import->filePath, import->pdfSettings, 1, nullptr,
              import->cancelRequested.get());
        }
      });
    } else {
      import->thread = QThread::create([this, import] {
#ifdef Q_OS_WIN
        if (import->kind == DocumentImportKind::PowerPoint) {
          import->powerPointResult = PowerPointImportService::importFile(
              import->filePath, import->powerPointSettings, 1, nullptr,
              import->cancelRequested.get(),
              [this, itemId = import->itemId](PowerPointImportPhase phase,
                                              int progressPermille) {
                const PlaylistItemConversionStage stage =
                    phase == PowerPointImportPhase::Exporting
                        ? PlaylistItemConversionStage::Exporting
                    : phase == PowerPointImportPhase::Encoding
                        ? PlaylistItemConversionStage::Encoding
                        : PlaylistItemConversionStage::Preparing;
                QMetaObject::invokeMethod(
                    this,
                    [this, itemId, stage, progressPermille] {
                      updateProgress(itemId, stage, progressPermille);
                    },
                    Qt::QueuedConnection);
              }, import->outputRoot);
          return;
        }
#endif
        import->pdfResult = PdfImportService::importFile(
            import->filePath, import->pdfSettings, 1, nullptr,
            import->cancelRequested.get(),
            [this, itemId = import->itemId](PdfImportPhase phase,
                                            int progressPermille) {
              const PlaylistItemConversionStage stage =
                  phase == PdfImportPhase::Rendering
                      ? PlaylistItemConversionStage::Rendering
                  : phase == PdfImportPhase::Encoding
                      ? PlaylistItemConversionStage::Encoding
                      : PlaylistItemConversionStage::Preparing;
              QMetaObject::invokeMethod(
                  this,
                  [this, itemId, stage, progressPermille] {
                    updateProgress(itemId, stage, progressPermille);
                  },
                  Qt::QueuedConnection);
            }, import->outputRoot);
      });
    }
    QThread *const worker = import->thread;
    worker->setParent(this);
    connect(worker, &QThread::finished, this, [this, import, worker] {
      import->thread = nullptr;
      finishImport(import);
      worker->deleteLater();
    });
    worker->start();
    return;
  }
}

void DocumentImportQueue::finishImport(
    const std::shared_ptr<PendingDocumentImport> &import) {
#ifdef Q_OS_WIN
  if (import->powerPointPrepared) {
    programController_->finishPowerPointConversion();
    import->powerPointPrepared = false;
  }
#endif
  if (activeDocumentImport_ == import) {
    activeDocumentImport_.reset();
  }
  documentImportsByItemId_.remove(import->itemId);

  const bool powerPoint = import->kind == DocumentImportKind::PowerPoint;
  const bool succeeded = powerPoint ? import->powerPointResult.succeeded
                                    : import->pdfResult.succeeded;
  const bool canceled =
      import->cancelRequested->load(std::memory_order_acquire) ||
      (powerPoint ? import->powerPointResult.canceled
                  : import->pdfResult.canceled);
  const QList<PlaylistItemRegistration> items =
      powerPoint ? import->powerPointResult.items : import->pdfResult.items;
  const QString generatedDirectory =
      powerPoint ? import->powerPointResult.generatedDirectory
                 : import->pdfResult.generatedDirectory;
  const QString errorDetail = powerPoint ? import->powerPointResult.errorDetail
                                         : import->pdfResult.errorDetail;
  const QString generatedRoot = powerPoint ? import->powerPointResult.generatedRoot : import->pdfResult.generatedRoot;
  const auto discardGenerated = [this, powerPoint, generatedDirectory, generatedRoot] {
    if (generatedDirectory.isEmpty())
      return true;
    const auto removed = std::make_shared<bool>(false);
    // Completion runs on the GUI thread even when the item/queue has gone.
    // The worker owns values only; it never accesses the UI or this queue.
    const QPointer<UserInfoCenter> info(userInfoCenter_);
    const auto completed = [info, removed, generatedDirectory](const QString &error) {
      if (*removed && error.isEmpty()) return;
      QString detail = QStringLiteral("Generated conversion artifacts could not be removed: %1")
                           .arg(QDir::toNativeSeparators(generatedDirectory));
      if (!error.isEmpty()) detail += QLatin1Char(' ') + error;
      qWarning().noquote() << detail;
      if (info) {
        info->showWarning(QStringLiteral("PLAYLIST"),
                          vtrpon::LocalizedText("playlist.conversion.failed"), detail);
      }
    };
    if (fileIo_ && fileIo_->enqueue([powerPoint, generatedDirectory, generatedRoot, removed] {
      *removed =
#ifdef Q_OS_WIN
          powerPoint ? PowerPointImportService::discardGeneratedArtifacts(generatedDirectory, generatedRoot) :
#endif
          PdfImportService::discardGeneratedArtifacts(generatedDirectory, generatedRoot);
    }, completed)) return true;
    completed(QStringLiteral("The background file queue is unavailable."));
    return false;
  };

  const bool placeholderExists =
      playlistModel_ && playlistModel_->itemState(import->itemId).has_value();
  if (!placeholderExists) {
    (void)discardGenerated();
    startNext();
    return;
  }
  if (canceled || !succeeded) {
    (void)playlistModel_->removeItem(import->itemId);
    (void)discardGenerated();
    if (!import->canceledByRemoval) {
      userInfoCenter_->showWarning(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText(canceled ? "playlist.conversion.canceled"
                                         : "playlist.conversion.failed"),
          errorDetail);
    }
    startNext();
    return;
  }

  const QList<quint64> replacementIds =
      import->isNative()
          ? (items.size() == 1 && playlistModel_->completePendingNativeDocument(
                                      import->itemId)
                 ? QList<quint64>{import->itemId}
                 : QList<quint64>{})
          : playlistModel_->replaceItemAtomically(import->itemId, items);
  if (replacementIds.size() != items.size()) {
    (void)discardGenerated();
    (void)playlistModel_->removeItem(import->itemId);
    userInfoCenter_->showWarning(
        QStringLiteral("PLAYLIST"),
        vtrpon::LocalizedText("playlist.conversion.failed"),
        QStringLiteral("The converted items could not replace the pending "
                       "playlist item."));
    startNext();
    return;
  }

#ifdef Q_OS_WIN
  if (powerPoint && import->powerPointSettings.rememberForSession) {
    emit powerPointSettingsRemembered(import->powerPointSettings);
  }
#endif
  userInfoCenter_->showSuccess(
      QStringLiteral("PLAYLIST"),
      vtrpon::LocalizedText(powerPoint ? "powerpoint.import.succeeded"
                                       : "pdf.import.succeeded")
          .arg(replacementIds.size()));
  startNext();
}

void DocumentImportQueue::cancel(quint64 itemId, bool removePlaceholder) {
  const auto import = documentImportsByItemId_.value(itemId);
  if (!import) {
    return;
  }
  import->canceledByRemoval = true;
  import->cancelRequested->store(true, std::memory_order_release);
  if (activeDocumentImport_ != import) {
    documentImportQueue_.removeAll(import);
    documentImportsByItemId_.remove(itemId);
  }
  if (removePlaceholder && playlistModel_) {
    (void)playlistModel_->removeItem(itemId);
  }
}
#endif

void DocumentImportQueue::cancelAll(bool removePlaceholders) {
#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
  const auto imports = documentImportsByItemId_.values();
  for (const auto &import : imports) {
    cancel(import->itemId, removePlaceholders);
  }
#else
  Q_UNUSED(removePlaceholders);
#endif
}

void DocumentImportQueue::waitForActiveImport() {
#if defined(Q_OS_WIN) || defined(VTRPON_PDF_SUPPORT)
  const auto import = activeDocumentImport_;
  if (!import) {
    return;
  }
  if (import->thread) {
    import->thread->wait();
  }
#ifdef Q_OS_WIN
  if (import->powerPointPrepared && programController_) {
    programController_->finishPowerPointConversion();
    import->powerPointPrepared = false;
  }
#endif
#endif
}
