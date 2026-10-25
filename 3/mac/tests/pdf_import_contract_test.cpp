#include <QtGlobal>
#ifdef Q_OS_MACOS
#include "mac_media_runtime.h"
#endif
#include "document_add_method_dialog.h"
#include "document_video_encoder.h"
#include "pdf_document_backend.h"
#include "pdf_import_service.h"
#include "pdf_source_session_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QDoubleSpinBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRadioButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <cmath>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool configureGStreamerDllSearchPath() {
#ifdef _WIN32
  const auto hasRuntime = [](const std::wstring &binDirectory) {
    const std::wstring libraryPath =
        binDirectory + L"/gstreamer-1.0-0.dll";
    return GetFileAttributesW(libraryPath.c_str()) !=
           INVALID_FILE_ATTRIBUTES;
  };

  std::wstring binDirectory;
  wchar_t environmentRoot[32768] = {};
  const DWORD length = GetEnvironmentVariableW(
      L"GSTREAMER_1_0_ROOT_MSVC_X86_64", environmentRoot,
      static_cast<DWORD>(std::size(environmentRoot)));
  if (length > 0 && length < std::size(environmentRoot)) {
    binDirectory = std::wstring(environmentRoot, length) + L"/bin";
  }
  if (!hasRuntime(binDirectory)) {
    binDirectory = VTRPON_GSTREAMER_BIN_DIR;
  }
  return hasRuntime(binDirectory) &&
         SetDllDirectoryW(binDirectory.c_str()) != FALSE;
#else
  return true;
#endif
}

bool check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool createTwoPagePdf(const QString &filePath) {
  bool wrotePages = false;
  {
    QPdfWriter writer(filePath);
    writer.setResolution(72);
    const QPageSize squarePage(QSizeF(400.0, 400.0), QPageSize::Point,
                               QStringLiteral("VTR-PON Test Square"),
                               QPageSize::ExactMatch);
    const QPageLayout pageLayout(squarePage, QPageLayout::Portrait,
                                 QMarginsF(), QPageLayout::Point);
    if (!writer.setPageLayout(pageLayout)) {
      return false;
    }

    QPainter painter(&writer);
    if (!painter.isActive() || writer.width() <= 0 || writer.height() <= 0) {
      return false;
    }
    const QRect pageRect(0, 0, writer.width(), writer.height());
    painter.fillRect(pageRect, Qt::white);
    painter.fillRect(QRect(writer.width() / 4, writer.height() / 4,
                           writer.width() / 2, writer.height() / 2),
                     QColor(230, 32, 32));

    if (!writer.newPage()) {
      painter.end();
      return false;
    }
    painter.fillRect(pageRect, QColor(245, 245, 245));
    painter.fillRect(QRect(writer.width() / 4, writer.height() / 4,
                           writer.width() / 2, writer.height() / 2),
                     QColor(32, 80, 230));
    wrotePages = painter.end();
  }
  return wrotePages && QFileInfo(filePath).isFile() &&
         QFileInfo(filePath).size() > 0;
}

bool createPortraitPdfWithImplicitPaper(const QString &filePath) {
  bool wrotePage = false;
  {
    QPdfWriter writer(filePath);
    writer.setResolution(72);
    const QPageLayout pageLayout(QPageSize(QPageSize::A4),
                                 QPageLayout::Portrait, QMarginsF(),
                                 QPageLayout::Point);
    if (!writer.setPageLayout(pageLayout)) {
      return false;
    }

    QPainter painter(&writer);
    if (!painter.isActive() || writer.width() <= 0 || writer.height() <= 0) {
      return false;
    }
    painter.fillRect(QRect(writer.width() / 4, writer.height() / 4,
                           writer.width() / 2, writer.height() / 2),
                     Qt::black);
    wrotePage = painter.end();
  }
  return wrotePage && QFileInfo(filePath).isFile() &&
         QFileInfo(filePath).size() > 0;
}

bool createPageLimitPdf(const QString &filePath, int pageCount) {
  if (pageCount <= 0) {
    return false;
  }
  bool wrotePages = false;
  {
    QPdfWriter writer(filePath);
    writer.setResolution(72);
    const QPageSize smallPage(QSizeF(72.0, 72.0), QPageSize::Point,
                              QStringLiteral("VTR-PON Page Limit"),
                              QPageSize::ExactMatch);
    if (!writer.setPageLayout(QPageLayout(
            smallPage, QPageLayout::Portrait, QMarginsF(),
            QPageLayout::Point))) {
      return false;
    }
    QPainter painter(&writer);
    if (!painter.isActive()) {
      return false;
    }
    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
      painter.fillRect(QRect(0, 0, 2, 2),
                       pageIndex % 2 == 0 ? Qt::black : Qt::white);
      if (pageIndex + 1 < pageCount && !writer.newPage()) {
        painter.end();
        return false;
      }
    }
    wrotePages = painter.end();
  }
  return wrotePages && QFileInfo(filePath).isFile() &&
         QFileInfo(filePath).size() > 0;
}

bool createCorruptPdf(const QString &filePath) {
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  static constexpr char kCorruptData[] =
      "%PDF-1.7\nThis is deliberately not a valid PDF document.\n";
  return file.write(kCorruptData, sizeof(kCorruptData) - 1) ==
             sizeof(kCorruptData) - 1 &&
         file.flush();
}

bool isBlack(const QColor &color) {
  return color.alpha() == 255 && color.red() == 0 && color.green() == 0 &&
         color.blue() == 0;
}

bool isWhite(const QColor &color) {
  return color.alpha() == 255 && color.red() == 255 && color.green() == 255 &&
         color.blue() == 255;
}

bool verifyBackendErrors(const QString &temporaryDirectory) {
  bool succeeded = true;

  PdfDocumentBackend missingBackend;
  const QString missingPath =
      QDir(temporaryDirectory).filePath(QStringLiteral("missing.pdf"));
  succeeded =
      check(!missingBackend.load(missingPath),
            "Missing PDF unexpectedly loaded") &&
      succeeded;
  succeeded =
      check(missingBackend.error() == PdfDocumentError::MissingFile,
            "Missing PDF did not report MissingFile") &&
      succeeded;
  succeeded =
      check(!missingBackend.errorDetail().isEmpty(),
            "Missing PDF error detail was empty") &&
      succeeded;

  const QString corruptPath =
      QDir(temporaryDirectory).filePath(QStringLiteral("corrupt.pdf"));
  succeeded = check(createCorruptPdf(corruptPath),
                    "Could not create corrupt PDF fixture") &&
              succeeded;
  PdfDocumentBackend corruptBackend;
  succeeded =
      check(!corruptBackend.load(corruptPath),
            "Corrupt PDF unexpectedly loaded") &&
      succeeded;
  succeeded =
      check(corruptBackend.error() == PdfDocumentError::InvalidFile,
            "Corrupt PDF did not report InvalidFile") &&
      succeeded;
  succeeded =
      check(!corruptBackend.errorDetail().isEmpty(),
            "Corrupt PDF error detail was empty") &&
      succeeded;

  const PdfAddSettings nativeSettings;
  const PdfImportResult corruptImport = PdfImportService::importFile(
      corruptPath, nativeSettings, 1);
  succeeded =
      check(!corruptImport.succeeded && !corruptImport.canceled &&
                corruptImport.items.isEmpty() &&
                corruptImport.generatedDirectory.isEmpty() &&
                corruptImport.pdfError == PdfDocumentError::InvalidFile,
            "Corrupt PDF import did not fail without registrations") &&
      succeeded;

  const QString pageLimitPath = QDir(temporaryDirectory).filePath(
      QStringLiteral("too-many-pages.pdf"));
  succeeded =
      check(createPageLimitPdf(
                pageLimitPath, PdfDocumentBackend::kMaximumPageCount + 1),
            "Could not create over-limit PDF fixture") &&
      succeeded;
  PdfDocumentBackend pageLimitBackend;
  succeeded =
      check(!pageLimitBackend.load(pageLimitPath) &&
                pageLimitBackend.error() == PdfDocumentError::TooManyPages &&
                !pageLimitBackend.errorDetail().isEmpty(),
            "Over-limit PDF did not report TooManyPages") &&
      succeeded;
  return succeeded;
}

bool verifyBackendRendering(const QString &filePath) {
  bool succeeded = true;
  PdfDocumentBackend backend;
  succeeded = check(backend.load(filePath),
                    "Two-page PDF fixture did not load") &&
              succeeded;
  if (!backend.isLoaded()) {
    std::cerr << backend.errorDetail().toStdString() << '\n';
    return false;
  }

  succeeded = check(backend.pageCount() == 2,
                    "PDF fixture page count was not two") &&
              succeeded;
  const QSizeF firstPageSize = backend.pagePointSize(0);
  succeeded =
      check(firstPageSize.isValid() && firstPageSize.width() > 0.0 &&
                firstPageSize.height() > 0.0 &&
                std::abs(firstPageSize.width() / firstPageSize.height() -
                         1.0) < 0.01,
            "PDF page point size was not the expected square") &&
      succeeded;
  succeeded = check(backend.pageLabel(0) == QStringLiteral("1") &&
                        backend.pageLabel(1) == QStringLiteral("2"),
                    "PDF default page labels were not exposed") &&
              succeeded;

  const QImage invalidPage = backend.renderPage(2);
  succeeded =
      check(invalidPage.isNull() &&
                backend.error() == PdfDocumentError::InvalidPage,
            "Out-of-range PDF rendering did not report InvalidPage") &&
      succeeded;
  const QImage invalidCanvas = backend.renderPage(0, QSize(0, 1080));
  succeeded =
      check(invalidCanvas.isNull() &&
                backend.error() == PdfDocumentError::RenderFailed,
            "Invalid PDF canvas did not report RenderFailed") &&
      succeeded;

  const QImage rendered = backend.renderPage(0);
  succeeded =
      check(!rendered.isNull(), "First PDF page did not render") &&
      succeeded;
  if (rendered.isNull()) {
    std::cerr << backend.errorDetail().toStdString() << '\n';
    return false;
  }
  succeeded =
      check(rendered.size() == QSize(1920, 1080),
            "PDF render canvas was not exactly 1920x1080") &&
      succeeded;
  succeeded =
      check(rendered.format() == QImage::Format_ARGB32,
            "PDF render canvas was not ARGB32") &&
      succeeded;
  succeeded =
      check(isBlack(rendered.pixelColor(0, 0)) &&
                isBlack(rendered.pixelColor(rendered.width() - 1,
                                            rendered.height() - 1)),
            "PDF contain margins were not opaque black") &&
      succeeded;

  int firstNonBlack = -1;
  int lastNonBlack = -1;
  const int middleY = rendered.height() / 2;
  for (int x = 0; x < rendered.width(); ++x) {
    if (!isBlack(rendered.pixelColor(x, middleY))) {
      if (firstNonBlack < 0) {
        firstNonBlack = x;
      }
      lastNonBlack = x;
    }
  }
  constexpr int kExpectedLeftPadding = (1920 - 1080) / 2;
  constexpr int kExpectedRightEdge = kExpectedLeftPadding + 1080 - 1;
  succeeded =
      check(std::abs(firstNonBlack - kExpectedLeftPadding) <= 3 &&
                std::abs(lastNonBlack - kExpectedRightEdge) <= 3,
            "Square PDF page was not aspect-contained without a second scale") &&
      succeeded;
  const QColor center = rendered.pixelColor(rendered.width() / 2, middleY);
  succeeded =
      check(center.red() > 160 && center.red() > center.green() + 100 &&
                center.red() > center.blue() + 100,
            "Rendered PDF page content was missing from the canvas center") &&
      succeeded;

  backend.close();
  succeeded = check(!backend.isLoaded() && backend.pageCount() == 0,
                    "PDF backend did not close synchronously") &&
              succeeded;
  return succeeded;
}

bool verifyPortraitRendering(const QString &filePath) {
  PdfDocumentBackend backend;
  bool succeeded =
      check(backend.load(filePath), "Portrait PDF fixture did not load");
  if (!backend.isLoaded()) {
    std::cerr << backend.errorDetail().toStdString() << '\n';
    return false;
  }

  const QSizeF pageSize = backend.pagePointSize(0);
  succeeded =
      check(pageSize.isValid() && pageSize.height() > pageSize.width(),
            "Portrait PDF fixture did not expose a portrait page size") &&
      succeeded;

  const QImage rendered = backend.renderPage(0);
  succeeded =
      check(!rendered.isNull() && rendered.size() == QSize(1920, 1080),
            "Portrait PDF did not render to the 16:9 canvas") &&
      succeeded;
  if (rendered.isNull()) {
    std::cerr << backend.errorDetail().toStdString() << '\n';
    return false;
  }

  const int expectedPageWidth = static_cast<int>(
      std::floor(pageSize.width() * 1080.0 / pageSize.height()));
  const int expectedLeft = (rendered.width() - expectedPageWidth) / 2;
  const int expectedRight = expectedLeft + expectedPageWidth - 1;
  const int centerX = rendered.width() / 2;
  succeeded =
      check(isBlack(rendered.pixelColor(expectedLeft - 1,
                                        rendered.height() / 2)) &&
                isWhite(rendered.pixelColor(expectedLeft, 0)) &&
                isWhite(rendered.pixelColor(centerX, 0)) &&
                isWhite(rendered.pixelColor(centerX,
                                            rendered.height() - 1)) &&
                isWhite(rendered.pixelColor(expectedRight,
                                            rendered.height() - 1)) &&
                isBlack(rendered.pixelColor(expectedRight + 1,
                                            rendered.height() / 2)),
            "Portrait PDF was not height-contained with opaque white paper") &&
      succeeded;
  succeeded =
      check(isBlack(rendered.pixelColor(centerX, rendered.height() / 2)),
            "Portrait PDF page content was missing") &&
      succeeded;
  return succeeded;
}

bool verifyFramePlans() {
  bool succeeded = true;
  DocumentVideoSettings settings;
  settings.pageDurationSeconds = 2.0;
  settings.crossDissolveEnabled = true;
  settings.crossDissolveDurationSeconds = 0.5;
  QString errorDetail;
  const auto plan = makeDocumentVideoFramePlan(2, settings, &errorDetail);
  succeeded =
      check(plan.has_value() && errorDetail.isEmpty(),
            "Valid document video frame plan was rejected") &&
      succeeded;
  if (plan) {
    succeeded =
        check(plan->framesPerSecond == 30 && plan->framesPerPage == 60 &&
                  plan->transitionFrames == 15 && plan->totalFrames == 120 &&
                  plan->durationMs == 4000,
              "Document video frame plan values were incorrect") &&
        succeeded;
  }

  succeeded =
      check(!makeDocumentVideoFramePlan(0, settings, &errorDetail) &&
                !errorDetail.isEmpty(),
            "Zero-page document video frame plan was accepted") &&
      succeeded;
  settings.pageDurationSeconds = 0.05;
  succeeded =
      check(!makeDocumentVideoFramePlan(2, settings, &errorDetail) &&
                !errorDetail.isEmpty(),
            "Invalid page duration was accepted") &&
      succeeded;
  settings.pageDurationSeconds = 1.0;
  settings.crossDissolveDurationSeconds = 1.1;
  succeeded =
      check(!makeDocumentVideoFramePlan(2, settings, &errorDetail) &&
                !errorDetail.isEmpty(),
            "Cross Dissolve longer than a page was accepted") &&
      succeeded;
  return succeeded;
}

bool verifyPdfAddDialog() {
  DocumentAddMethodDialog dialog(
      QStringLiteral("pdf.add"), QStringLiteral("pdf"),
      QStringLiteral("page"));
  bool succeeded = true;
  succeeded =
      check(dialog.objectName() == QStringLiteral("pdfAddMethodDialog"),
            "PDF add dialog object name changed") &&
      succeeded;
  auto *nativeButton =
      dialog.findChild<QRadioButton *>(QStringLiteral("pdfNativeMethod"));
  auto *videoButton =
      dialog.findChild<QRadioButton *>(QStringLiteral("pdfVideoMethod"));
  auto *pngButton =
      dialog.findChild<QRadioButton *>(QStringLiteral("pdfPngMethod"));
  auto *pageDuration =
      dialog.findChild<QDoubleSpinBox *>(QStringLiteral("pdfPageDuration"));
  auto *crossDissolve =
      dialog.findChild<QCheckBox *>(QStringLiteral("pdfCrossDissolve"));
  auto *crossDissolveDuration = dialog.findChild<QDoubleSpinBox *>(
      QStringLiteral("pdfCrossDissolveDuration"));
  auto *remember = dialog.findChild<QCheckBox *>(
      QStringLiteral("pdfRememberForSession"));
  succeeded =
      check(nativeButton && videoButton && pngButton && pageDuration &&
                crossDissolve && crossDissolveDuration && remember,
            "PDF add dialog controls were incomplete") &&
      succeeded;
  if (!nativeButton || !videoButton || !pngButton || !pageDuration ||
      !crossDissolve || !crossDissolveDuration || !remember) {
    return false;
  }

  const DocumentAddSettings defaults = dialog.documentSettings();
  succeeded =
      check(defaults.method == DocumentAddMethod::Native &&
                std::abs(defaults.pageDurationSeconds - 5.0) < 0.001 &&
                defaults.crossDissolveEnabled &&
                std::abs(defaults.crossDissolveDurationSeconds - 1.0) <
                    0.001 &&
                !defaults.rememberForSession,
            "PDF add dialog defaults changed") &&
      succeeded;
  succeeded =
      check(!pageDuration->isEnabled() && !crossDissolve->isEnabled() &&
                !crossDissolveDuration->isEnabled(),
            "Native PDF controls were not disabled") &&
      succeeded;

  videoButton->setChecked(true);
  pageDuration->setValue(3.5);
  crossDissolve->setChecked(false);
  crossDissolveDuration->setValue(0.7);
  remember->setChecked(true);
  const DocumentAddSettings selected = dialog.documentSettings();
  succeeded =
      check(selected.method == DocumentAddMethod::VideoConversion &&
                std::abs(selected.pageDurationSeconds - 3.5) < 0.001 &&
                !selected.crossDissolveEnabled &&
                std::abs(selected.crossDissolveDurationSeconds - 0.7) <
                    0.001 &&
                selected.rememberForSession,
            "PDF add dialog did not preserve the selected settings") &&
      succeeded;
  succeeded =
      check(pageDuration->isEnabled() && crossDissolve->isEnabled() &&
                !crossDissolveDuration->isEnabled(),
            "PDF video controls had an incorrect enabled state") &&
      succeeded;

  pngButton->setChecked(true);
  const DocumentAddSettings pngSelected = dialog.documentSettings();
  succeeded =
      check(pngSelected.method == DocumentAddMethod::PngSequence &&
                std::abs(pngSelected.pageDurationSeconds - 3.5) < 0.001,
            "PDF PNG settings did not preserve page duration") &&
      succeeded;
  succeeded =
      check(pageDuration->isEnabled() && !crossDissolve->isEnabled() &&
                !crossDissolveDuration->isEnabled(),
            "PDF PNG controls had an incorrect enabled state") &&
      succeeded;
  return succeeded;
}

bool verifyNativeRegistration(const QString &filePath) {
  PdfAddSettings settings;
  settings.method = PdfAddMethod::Native;
  const PdfImportResult result =
      PdfImportService::importFile(filePath, settings, 7);
  bool succeeded = true;
  succeeded =
      check(result.succeeded && !result.canceled &&
                result.errorDetail.isEmpty() && result.items.size() == 1 &&
                result.generatedDirectory.isEmpty(),
            "Native PDF import did not return one direct registration") &&
      succeeded;
  if (result.items.size() == 1) {
    const PlaylistItemRegistration &item = result.items.constFirst();
    succeeded =
        check(item.filePath == QFileInfo(filePath).absoluteFilePath() &&
                  item.sourceType == PlaylistSourceType::Pdf &&
                  item.mediaType == PlaylistMediaType::Pdf &&
                  item.powerPointMode == PlaylistPowerPointMode::None &&
                  !item.settings.outPointMs &&
                  item.settings.startMode == PlaylistStartMode::Play &&
                  item.settings.endMode == PlaylistEndMode::Pause &&
                  item.playlistNumber == 7,
              "Native PDF registration fields were incorrect") &&
        succeeded;
  }
  return succeeded;
}

bool awaitPdfInput(PdfSourceSessionManager &manager,
                   PdfSourceSessionManager::ProgramSource &source) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 10000) {
    source = manager.programSource(0);
    if (source.isValid()) return true;
    quint64 generation = 0;
    QString detail;
    if (manager.sourceErrorAfter(0, 0, generation, detail)) {
      std::cerr << detail.toStdString() << '\n';
      return false;
    }
    QApplication::processEvents();
    QThread::msleep(10);
  }
  return false;
}

PdfCommandResponse navigatePdf(PdfSourceSessionManager &manager, bool forward) {
  QElapsedTimer timer;
  timer.start();
  PdfCommandResponse response;
  do {
    response = forward ? manager.next(0) : manager.previous(0);
    if (response.result != PdfCommandResult::Busy) return response;
    QApplication::processEvents();
    QThread::msleep(10);
  } while (timer.elapsed() < 10000);
  return response;
}

bool verifyNativeSourceSmoke(const QString &filePath) {
  PdfSourceSessionManager manager(1920, 1080);
  PdfSourceSessionManager::ProgramSource source;
  QString errorDetail;
  bool succeeded =
      check(manager.bindProgramDeck(0, filePath, source, &errorDetail),
            "Native PDF source could not bind a Program deck");
  if (!succeeded) {
    std::cerr << errorDetail.toStdString() << '\n';
    return false;
  }
  succeeded =
      check(awaitPdfInput(manager, source) && manager.sourceIsPlaying(0),
            "Native PDF Program source did not enter PLAYING") &&
      succeeded;
  if (!source.isValid()) return false;
  const PdfNavigationState initialState = manager.navigationState(0);
  succeeded =
      check(initialState.active && initialState.currentPage == 1 &&
                initialState.pageCount == 2 &&
                !initialState.canGoPrevious && initialState.canGoNext,
            "Native PDF initial navigation state was incorrect") &&
      succeeded;

  GstSample *sample = gst_app_sink_try_pull_sample(
      GST_APP_SINK(source.appSink), 5 * GST_SECOND);
  succeeded = check(sample != nullptr,
                    "Native PDF source produced no D3D11 Program frame") &&
              succeeded;
  if (sample) {
    gst_sample_unref(sample);
  }

  const PdfCommandResponse nextResponse = navigatePdf(manager, true);
  const PdfNavigationState nextState = manager.navigationState(0);
  succeeded =
      check(nextResponse.result == PdfCommandResult::Completed &&
                nextState.currentPage == 2 && nextState.canGoPrevious &&
                !nextState.canGoNext,
            "Native PDF NEXT did not commit page two") &&
      succeeded;
  const PdfCommandResponse boundaryResponse = navigatePdf(manager, true);
  succeeded =
      check(boundaryResponse.result == PdfCommandResult::Boundary &&
                manager.navigationState(0).currentPage == 2,
            "Native PDF page boundary changed the Program page") &&
      succeeded;
  const PdfCommandResponse previousResponse = navigatePdf(manager, false);
  succeeded =
      check(previousResponse.result == PdfCommandResult::Completed &&
                manager.navigationState(0).currentPage == 1,
            "Native PDF PREV did not restore page one") &&
      succeeded;

  quint64 errorGeneration = 0;
  QString sourceError;
  succeeded =
      check(!manager.sourceErrorAfter(0, 0, errorGeneration, sourceError),
            "Native PDF source reported a pipeline error") &&
      succeeded;
  manager.suspendProgramDeck(0);
  bool released = false;
  QElapsedTimer releaseTimer;
  releaseTimer.start();
  while (!released && releaseTimer.elapsed() < 1000) {
    released = manager.tryReleaseProgramDeck(0);
    if (!released) {
      QThread::msleep(10);
    }
  }
  succeeded = check(released,
                    "Native PDF deck did not release non-blockingly") &&
              succeeded;
  // A retired missing-file request must not deliver an error into the next CUE.
  QElapsedTimer requestTimer;
  requestTimer.start();
  succeeded = check(manager.bindProgramDeck(0, filePath + ".missing", source, &errorDetail),
                    "Missing-file request was not queued") && succeeded;
  succeeded = check(!source.isValid(), "Queued bind unexpectedly returned a live input") && succeeded;
  manager.releaseProgramDeck(0);
  succeeded = check(manager.bindProgramDeck(0, filePath, source, &errorDetail),
                    "Replacement PDF request failed") && succeeded;
  succeeded = check(requestTimer.elapsed() < 500, "PDF CUE/release blocked the caller") && succeeded;
  if (!awaitPdfInput(manager, source)) return false;
  succeeded = check(manager.navigationState(0).currentPage == 1 &&
                    !manager.sourceErrorAfter(0, 0, errorGeneration, sourceError),
                    "Retired PDF results leaked into the replacement") && succeeded;
  // A released navigation response must not change the replacement page.
  (void)manager.next(0);
  manager.releaseProgramDeck(0);
  manager.bindProgramDeck(0, filePath, source, &errorDetail);
  if (!awaitPdfInput(manager, source)) return false;
  succeeded = check(manager.navigationState(0).currentPage == 1,
                    "Old navigation affected a replacement PDF") && succeeded;
  // Timeout/cancel must discard a completed response, not consume the next click.
  (void)manager.next(0);
  QElapsedTimer navigationTimer;
  navigationTimer.start();
  while (manager.navigationState(0).currentPage != 2 && navigationTimer.elapsed() < 5000) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  manager.cancelNavigation(0);
  succeeded = check(navigatePdf(manager, false).result == PdfCommandResult::Completed &&
                    manager.navigationState(0).currentPage == 1,
                    "Canceled completion consumed the next navigation request") && succeeded;
  manager.requestShutdown();
  QElapsedTimer shutdownTimer;
  shutdownTimer.start();
  while (!manager.shutdownComplete() && shutdownTimer.elapsed() < 10000) {
    QApplication::processEvents();
    QThread::msleep(10);
  }
  succeeded = check(manager.shutdownComplete(), "PDF async shutdown did not complete") && succeeded;
  succeeded = check(manager.shutdown(),
                    "Native PDF source manager did not shut down cleanly") &&
              succeeded;
  return succeeded;
}

bool verifyPngSequenceImport(const QString &filePath, const QString &outputRoot = {}) {
  PdfAddSettings settings;
  settings.method = PdfAddMethod::PngSequence;
  settings.pageDurationSeconds = 3.75;
  const PdfImportResult result =
      PdfImportService::importFile(filePath, settings, 3, nullptr, nullptr, {}, outputRoot);
  bool succeeded = check(outputRoot.isEmpty() || result.generatedRoot == outputRoot, "Custom conversion root was ignored");
  succeeded =
      check(result.succeeded && !result.canceled &&
                result.errorDetail.isEmpty() && result.items.size() == 2 &&
                !result.generatedDirectory.isEmpty() &&
                QDir(result.generatedDirectory).exists(),
            "PNG-sequence PDF import did not commit two registrations") &&
      succeeded;
  succeeded =
      check(!QFileInfo(result.generatedDirectory)
                 .fileName()
                 .endsWith(QStringLiteral(".staging")),
            "PNG-sequence PDF import exposed a staging directory") &&
      succeeded;

  for (int pageIndex = 0; pageIndex < result.items.size(); ++pageIndex) {
    const PlaylistItemRegistration &item = result.items.at(pageIndex);
    const QString expectedIndex =
        QStringLiteral("page-%1").arg(pageIndex + 1, 3, 10,
                                      QLatin1Char('0'));
    const QString expectedLabel = QStringLiteral("-%1.png").arg(pageIndex + 1);
    const QFileInfo itemInfo(item.filePath);
    succeeded =
        check(item.sourceType == PlaylistSourceType::File &&
                  item.mediaType == PlaylistMediaType::StillImage &&
                  item.powerPointMode == PlaylistPowerPointMode::None &&
                  item.settings.outPointMs &&
                  *item.settings.outPointMs == 3750 &&
                  item.settings.startMode == PlaylistStartMode::Pause &&
                  item.settings.endMode == PlaylistEndMode::Pause &&
                  item.playlistNumber == 3 && itemInfo.isFile() &&
                  itemInfo.absolutePath().compare(result.generatedDirectory,
                                                  Qt::CaseInsensitive) == 0 &&
                  itemInfo.fileName().contains(QStringLiteral("broadcast_source")) &&
                  itemInfo.fileName().contains(expectedIndex) &&
                  itemInfo.fileName().endsWith(expectedLabel),
              "Generated PDF page registration or filename was incorrect") &&
        succeeded;
    const QImage pageImage(item.filePath);
    succeeded =
        check(!pageImage.isNull() && pageImage.size() == QSize(1920, 1080),
              "Generated PDF page PNG was not a valid 1920x1080 image") &&
        succeeded;
  }

  if (!result.generatedDirectory.isEmpty()) {
    const QString generatedRoot =
        QFileInfo(result.generatedDirectory).absolutePath();
    const QString nestedDirectory = QDir(result.generatedDirectory)
                                        .filePath(QStringLiteral("nested"));
    succeeded =
        check(QDir().mkpath(nestedDirectory),
              "Could not create cleanup-scope fixture") &&
        succeeded;
    succeeded =
        check(!PdfImportService::discardGeneratedArtifacts(generatedRoot, result.generatedRoot) &&
                  !PdfImportService::discardGeneratedArtifacts(
                      nestedDirectory, result.generatedRoot) &&
                  !PdfImportService::discardGeneratedArtifacts(
                      QFileInfo(filePath).absolutePath(), result.generatedRoot) &&
                  QDir(result.generatedDirectory).exists(),
              "PDF cleanup accepted a root, nested, or unrelated path") &&
        succeeded;
    succeeded =
        check(PdfImportService::discardGeneratedArtifacts(
                  result.generatedDirectory, result.generatedRoot),
              "Generated PDF page directory could not be discarded") &&
        succeeded;
    succeeded =
        check(!QDir(result.generatedDirectory).exists(),
              "Generated PDF page directory remained after cleanup") &&
        succeeded;
  }
  return succeeded;
}

bool verifyInvalidPngSequenceDurations(const QString &filePath) {
  bool succeeded = true;
  const double invalidDurations[] = {
      0.09,
      600.01,
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
  };
  for (const double duration : invalidDurations) {
    PdfAddSettings settings;
    settings.method = PdfAddMethod::PngSequence;
    settings.pageDurationSeconds = duration;
    const PdfImportResult result =
        PdfImportService::importFile(filePath, settings, 3);
    succeeded =
        check(!result.succeeded && !result.canceled &&
                  result.items.isEmpty() &&
                  result.generatedDirectory.isEmpty() &&
                  !result.errorDetail.isEmpty(),
              "Invalid PNG-sequence PDF duration was accepted") &&
        succeeded;
  }
  return succeeded;
}

bool verifyPreCanceledImport(const QString &filePath) {
  PdfAddSettings settings;
  settings.method = PdfAddMethod::PngSequence;
  std::atomic_bool cancelRequested{true};
  const PdfImportResult result = PdfImportService::importFile(
      filePath, settings, 4, nullptr, &cancelRequested);
  return check(result.canceled && !result.succeeded &&
                   result.items.isEmpty() &&
                   result.generatedDirectory.isEmpty() &&
                   result.errorDetail.isEmpty(),
               "Pre-canceled PDF import produced items or artifacts");
}

bool verifyPngCancellation(const QString &filePath, const QString &directory) {
  const QString root = QDir(directory).filePath(QStringLiteral("cancel-output"));
  QDir().mkpath(root);
  QFile sentinel(QDir(root).filePath(QStringLiteral("keep.txt")));
  if (!sentinel.open(QIODevice::WriteOnly)) return false;
  sentinel.write("unrelated file"); sentinel.close();
  QFile source(filePath);
  if (!source.open(QIODevice::ReadOnly)) return false;
  const auto original = source.readAll(); source.close();
  bool succeeded = true;
  for (int cancelAt : {0, 500, 1000}) {
    PdfAddSettings settings; settings.method = PdfAddMethod::PngSequence;
    std::atomic_bool cancel{false};
    bool reported = false;
    const auto result = PdfImportService::importFile(filePath, settings, 1, nullptr, &cancel,
        [&](PdfImportPhase, int value) {
          reported = true;
          if (value >= cancelAt) cancel.store(true);
        }, root);
    succeeded = check(reported && result.canceled && !result.succeeded && result.items.isEmpty() &&
        result.generatedDirectory.isEmpty() && QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == QStringList{QStringLiteral("keep.txt")},
        "PNG cancellation left partial output or removed an unrelated file") && succeeded;
  }
  if (!source.open(QIODevice::ReadOnly)) return false;
  succeeded = check(source.readAll() == original, "PNG cancellation changed the source PDF") && succeeded;
#ifdef __APPLE__
  const auto target = QDir(root).filePath(QStringLiteral("generated"));
  const auto alias = QDir(root).filePath(QStringLiteral("alias"));
  succeeded = check(QDir().mkpath(target) && QFile::link(target, alias), "Could not create symlink cleanup fixture") && succeeded;
  succeeded = check(!PdfImportService::discardGeneratedArtifacts(alias, root) && QDir(target).exists(),
                    "PDF cleanup accepted a symlink target") && succeeded;
#endif
  return succeeded;
}

bool verifyVideoSmoke(const QString &filePath) {
  PdfAddSettings settings;
  settings.method = PdfAddMethod::VideoConversion;
  settings.pageDurationSeconds = 0.2;
  settings.crossDissolveEnabled = true;
  settings.crossDissolveDurationSeconds = 0.1;
  const PdfImportResult result =
      PdfImportService::importFile(filePath, settings, 5);
  bool succeeded = true;
  succeeded =
      check(result.succeeded && !result.canceled &&
                result.errorDetail.isEmpty() && result.items.size() == 1 &&
                !result.generatedDirectory.isEmpty(),
            "PDF video conversion smoke failed") &&
      succeeded;
  if (!result.succeeded && !result.errorDetail.isEmpty()) {
    std::cerr << result.errorDetail.toStdString() << '\n';
  }
  if (result.items.size() == 1) {
    const PlaylistItemRegistration &item = result.items.constFirst();
    succeeded =
        check(item.sourceType == PlaylistSourceType::File &&
                  item.mediaType == PlaylistMediaType::Video &&
                  item.powerPointMode == PlaylistPowerPointMode::None &&
                  item.settings.outPointMs && *item.settings.outPointMs > 0 &&
                  QFileInfo(item.filePath).isFile(),
              "Converted PDF video registration was incorrect") &&
        succeeded;
  }

  if (!result.generatedDirectory.isEmpty()) {
    const QString generatedRoot =
        QFileInfo(result.generatedDirectory).absolutePath();
    const QStringList entriesBeforeCancel =
        QDir(generatedRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                      QDir::Name);
    PdfAddSettings cancelSettings;
    cancelSettings.method = PdfAddMethod::VideoConversion;
    cancelSettings.pageDurationSeconds = 10.0;
    cancelSettings.crossDissolveEnabled = false;
    std::atomic_bool cancelRequested{false};
    bool progressReported = false;
    const PdfImportResult canceledResult = PdfImportService::importFile(
        filePath, cancelSettings, 6, nullptr, &cancelRequested,
        [&cancelRequested, &progressReported](PdfImportPhase, int) {
          progressReported = true;
          cancelRequested.store(true, std::memory_order_release);
        });
    const QStringList entriesAfterCancel =
        QDir(generatedRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                      QDir::Name);
    succeeded =
        check(canceledResult.canceled && !canceledResult.succeeded &&
                  canceledResult.items.isEmpty() &&
                  canceledResult.generatedDirectory.isEmpty() &&
                  canceledResult.errorDetail.isEmpty() && progressReported &&
                  entriesAfterCancel == entriesBeforeCancel,
              "Canceled PDF video conversion left items or staging files") &&
        succeeded;
  }
  if (!result.generatedDirectory.isEmpty()) {
    succeeded =
        check(PdfImportService::discardGeneratedArtifacts(
                  result.generatedDirectory),
              "Converted PDF video directory could not be discarded") &&
        succeeded;
  }
  return succeeded;
}

} // namespace

int main(int argc, char *argv[]) {
  bool videoSmokeRequested = false;
  bool nativeSmokeRequested = false;
  for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
    if (QString::fromLocal8Bit(argv[argumentIndex]) ==
        QStringLiteral("--video-smoke")) {
      videoSmokeRequested = true;
    } else if (QString::fromLocal8Bit(argv[argumentIndex]) ==
               QStringLiteral("--native-smoke")) {
      nativeSmokeRequested = true;
    }
  }
  if (videoSmokeRequested || nativeSmokeRequested) {
    if (!configureGStreamerDllSearchPath()) {
      std::cerr << "The GStreamer runtime could not be found.\n";
      return 2;
    }
#ifndef Q_OS_MACOS
    gst_init(nullptr, nullptr);
#endif
  }
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef _WIN32
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows"));
#else
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#endif
  }
  QStandardPaths::setTestModeEnabled(true);
  QApplication application(argc, argv);
#ifdef Q_OS_MACOS
  if (videoSmokeRequested || nativeSmokeRequested) {
    if (!configureMacBundledMediaRuntime()) return 2;
    gst_init(nullptr, nullptr);
  }
#endif
  QCoreApplication::setOrganizationName(QStringLiteral("VTR-PON"));
  QCoreApplication::setApplicationName(QStringLiteral("pdf-import-contract"));

  const QStringList arguments = application.arguments();
  if (arguments.size() > 2 ||
      (arguments.size() == 2 &&
       arguments.constLast() != QStringLiteral("--video-smoke") &&
       arguments.constLast() != QStringLiteral("--native-smoke"))) {
    std::cerr << "Usage: pdf-import-contract-test "
                 "[--video-smoke|--native-smoke]\n";
    return 2;
  }

  QTemporaryDir temporaryDirectory;
  if (!check(temporaryDirectory.isValid(),
             "Could not create PDF test directory")) {
    return 1;
  }
  const QString pdfPath = temporaryDirectory.filePath(
      QStringLiteral("broadcast source.pdf"));
  if (!check(createTwoPagePdf(pdfPath),
             "Could not create two-page PDF fixture")) {
    return 1;
  }
  const QString portraitPdfPath = temporaryDirectory.filePath(
      QStringLiteral("portrait implicit paper.pdf"));
  if (!check(createPortraitPdfWithImplicitPaper(portraitPdfPath),
             "Could not create portrait PDF fixture")) {
    return 1;
  }

  if (videoSmokeRequested) {
    return verifyVideoSmoke(pdfPath) ? 0 : 1;
  }
  if (nativeSmokeRequested) {
    return verifyNativeSourceSmoke(pdfPath) ? 0 : 1;
  }
  bool succeeded = verifyBackendErrors(temporaryDirectory.path());
  succeeded = verifyBackendRendering(pdfPath) && succeeded;
  succeeded = verifyPortraitRendering(portraitPdfPath) && succeeded;
  succeeded = verifyFramePlans() && succeeded;
  succeeded = verifyPdfAddDialog() && succeeded;
  succeeded = verifyNativeRegistration(pdfPath) && succeeded;
  succeeded = verifyPngSequenceImport(pdfPath) && succeeded;
  succeeded = verifyPngSequenceImport(pdfPath, temporaryDirectory.filePath("custom/PDF")) && succeeded;
  succeeded = verifyInvalidPngSequenceDurations(pdfPath) && succeeded;
  succeeded = verifyPreCanceledImport(pdfPath) && succeeded;
  succeeded = verifyPngCancellation(pdfPath, temporaryDirectory.path()) && succeeded;
  return succeeded ? 0 : 1;
}
