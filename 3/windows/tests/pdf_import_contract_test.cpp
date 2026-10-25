#include "document_add_method_dialog.h"
#include "document_video_encoder.h"
#include "pdf_document_backend.h"
#include "pdf_import_service.h"
#include "pdf_source_session_manager.h"
#include "pdf_windows_test_support.h"

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
#include <QPdfDocument>
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
  const QString atLimit = QDir(temporaryDirectory).filePath("1000-pages.pdf");
  succeeded = check(createPageLimitPdf(atLimit, 1000) && pageLimitBackend.load(atLimit) &&
      pageLimitBackend.pageCount() == 1000 && !pageLimitBackend.renderPage(999, QSize(160, 90)).isNull(),
      "Exactly 1000 pages must still load and render") && succeeded;
  // Authored empty page tree with valid object offsets.
  const QString emptyPath = QDir(temporaryDirectory).filePath("zero-pages.pdf");
  QFile empty(emptyPath);
  QByteArray bytes("%PDF-1.4\n");
  const auto firstOffset = bytes.size();
  bytes += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
  const auto secondOffset = bytes.size();
  bytes += "2 0 obj\n<< /Type /Pages /Count 0 /Kids [] >>\nendobj\n";
  const auto xref = bytes.size();
  bytes += "xref\n0 3\n0000000000 65535 f \n";
  for (const auto offset : {firstOffset, secondOffset})
    bytes += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
  bytes += "trailer\n<< /Size 3 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
  if (!check(empty.open(QIODevice::WriteOnly) && empty.write(bytes) == bytes.size(),
             "Empty PDF fixture could not be written")) return false;
  empty.close();
  succeeded = check(!pageLimitBackend.load(emptyPath) &&
      (pageLimitBackend.error() == PdfDocumentError::InvalidFile ||
       pageLimitBackend.error() == PdfDocumentError::LoadFailed) &&
      !pageLimitBackend.isLoaded() && pageLimitBackend.pageCount() == 0 &&
      pageLimitBackend.filePath().isEmpty(), "Empty PDF must clear prior state") && succeeded;
  const QString encrypted = QDir(QString::fromUtf8(VTRPON_PDF_FIXTURE_DIR))
      .filePath(QStringLiteral("password-required.pdf"));
  PdfDocumentBackend encryptedBackend;
  for (const QString &password : {QString(), QStringLiteral("wrong")})
    succeeded = check(!encryptedBackend.load(encrypted, password) &&
        encryptedBackend.error() == PdfDocumentError::PasswordRequired &&
        !encryptedBackend.isLoaded() && encryptedBackend.filePath().isEmpty(),
        "Encrypted PDF must retain PasswordRequired classification") && succeeded;
  succeeded = check(encryptedBackend.load(encrypted, QStringLiteral("boundary-password")) &&
      encryptedBackend.pageCount() == 1 && !encryptedBackend.renderPage(0, QSize(160, 90)).isNull() &&
      !encryptedBackend.load(encrypted) && encryptedBackend.error() == PdfDocumentError::PasswordRequired,
      "Known-password fixture must render and password must not carry across loads") && succeeded;
  std::cout << "Missing/corrupt/0/1000/1001/password classification passed=" << succeeded << '\n';
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

bool verifyReadDenied(const QString &pdf, const QString &root) {
  const QString deniedPath = QDir(root).filePath("read-denied.pdf");
  if (!check(QFile::copy(pdf, deniedPath), "Read-denied fixture copy failed")) return false;
  const QByteArray sourceHash = pdf_test::sha256(pdf);
  PdfDocumentBackend backend;
  if (!check(backend.load(pdf), "Prior PDF did not load")) return false;
  pdf_test::ReadDataDeny deny(deniedPath);
  if (!check(deny.apply(), "Read-data deny DACL setup failed")) return false;
  QFile readable(deniedPath);
  if (!check(QFileInfo(deniedPath).isFile() && !readable.open(QIODevice::ReadOnly),
             "DACL did not preserve metadata and deny actual open")) return false;
  QPdfDocument qtDocument;
  const auto qtError = qtDocument.load(deniedPath);
  qtDocument.close();
  const bool loaded = backend.load(deniedPath);
  std::cout << "Read deny: Qt error=" << static_cast<int>(qtError)
            << " backend=" << static_cast<int>(backend.error())
            << " reason=" << backend.errorDetail().toStdString() << '\n';
  bool passed = check(!loaded && backend.error() == PdfDocumentError::LoadFailed &&
      !backend.errorDetail().isEmpty() && !backend.isLoaded() &&
      backend.pageCount() == 0 && backend.filePath().isEmpty(),
      "Existing unreadable PDF must report LoadFailed and clear prior state");
  const QString outputRoot = QDir(root).filePath("denied-output");
  for (const auto method : {PdfAddMethod::Native, PdfAddMethod::PngSequence,
                            PdfAddMethod::VideoConversion}) {
    PdfAddSettings settings;
    settings.method = method;
    const auto result = PdfImportService::importFile(
        deniedPath, settings, 1, nullptr, nullptr, {}, outputRoot);
    passed = check(!result.succeeded && !result.canceled && result.items.isEmpty() &&
        result.generatedDirectory.isEmpty() && result.pdfError == PdfDocumentError::LoadFailed &&
        !result.errorDetail.isEmpty() && !QFileInfo::exists(outputRoot),
        "Unreadable PDF import created output or lost its failure reason") && passed;
  }
  QString detail;
  passed = check(generatePdfThumbnail(deniedPath, QSize(160, 90), &detail).isNull() &&
      !detail.isEmpty() && !validatePdfSource(deniedPath).isEmpty(),
      "Unreadable thumbnail/native validation did not report failure") && passed;
  if (!check(deny.restore(), "Read-data deny DACL restoration failed")) return false;
  passed = check(!sourceHash.isEmpty() && pdf_test::sha256(pdf) == sourceHash &&
      pdf_test::sha256(deniedPath) == sourceHash && backend.load(deniedPath) &&
      !backend.renderPage(0).isNull(), "Read-denied PDF did not recover unchanged") && passed;
  PdfAddSettings recoveredSettings;
  recoveredSettings.method = PdfAddMethod::PngSequence;
  const auto recovered = PdfImportService::importFile(
      deniedPath, recoveredSettings, 1, nullptr, nullptr, {}, outputRoot);
  passed = check(recovered.succeeded && recovered.items.size() == 2,
      "Restored PDF did not convert") && passed;
  if (!recovered.generatedDirectory.isEmpty())
    passed = check(PdfImportService::discardGeneratedArtifacts(
        recovered.generatedDirectory, outputRoot), "Recovered output cleanup failed") && passed;
  std::cout << "Read deny recovery SHA256=" << sourceHash.constData()
            << " passed=" << passed << '\n';
  return passed;
}

bool verifyCleanupDenied(const QString &pdf, const QString &root) {
  const QByteArray sourceHash = pdf_test::sha256(pdf);
  const QString outputRoot = QDir(root).filePath("cleanup-output");
  const QString sentinel = QDir(outputRoot).filePath("existing/keep.txt");
  if (!QDir().mkpath(QFileInfo(sentinel).absolutePath())) return false;
  QFile keep(sentinel);
  if (!keep.open(QIODevice::WriteOnly) || keep.write("existing output") != 15) return false;
  keep.close();
  const auto keepHash = pdf_test::sha256(sentinel);
  PdfAddSettings settings;
  settings.method = PdfAddMethod::PngSequence;
  for (const bool blockDelete : {true, false}) {
    pdf_test::DeleteDeny held;
    std::atomic_bool cancel = false;
    bool reached = false;
    bool denied = false;
    const auto result = PdfImportService::importFile(pdf, settings, 1, nullptr, &cancel,
        [&](PdfImportPhase phase, int) {
          if (phase != PdfImportPhase::Rendering || reached) return;
          reached = true;
          denied = blockDelete && held.hold(pdf_test::firstPng(outputRoot));
          cancel.store(true, std::memory_order_release);
        }, outputRoot);
    bool passed = check(reached && !result.succeeded && result.items.isEmpty(),
                        "Cancellation did not occur after rendering");
    if (blockDelete) {
      passed = check(denied && !result.canceled && !result.generatedDirectory.isEmpty() &&
          result.errorDetail.contains(QDir::toNativeSeparators(result.generatedDirectory)) &&
          QFileInfo::exists(result.generatedDirectory),
          "Failed cleanup must return residual path and must not be normal cancellation") && passed;
      std::cout << "Service residual=" << result.generatedDirectory.toStdString() << '\n';
      held.release();
      passed = check(PdfImportService::discardGeneratedArtifacts(
          result.generatedDirectory, outputRoot), "Residual cleanup after CloseHandle failed") && passed;
    } else {
      passed = check(result.canceled && result.errorDetail.isEmpty() &&
          result.generatedDirectory.isEmpty(), "Normal cancel left an error or output") && passed;
    }
    passed = check(pdf_test::sha256(pdf) == sourceHash && pdf_test::sha256(sentinel) == keepHash,
        "Cancellation changed the source or unrelated output") && passed;
    if (!passed) return false;
  }
  const auto recovered = PdfImportService::importFile(pdf, settings, 1, nullptr, nullptr, {}, outputRoot);
  const bool passed = recovered.succeeded && recovered.items.size() == 2 &&
      PdfImportService::discardGeneratedArtifacts(recovered.generatedDirectory, outputRoot) &&
      pdf_test::sha256(pdf) == sourceHash && pdf_test::sha256(sentinel) == keepHash;
  std::cout << "Service cleanup recovery passed=" << passed << '\n';
  return check(passed, "Service cleanup retry failed or changed unrelated files");
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

QByteArray bluePageFrameHash(GstElement *sink) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 5000) {
    QApplication::processEvents();
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
    if (!sample) continue;
    GstMapInfo mapped = GST_MAP_INFO_INIT;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    QByteArray digest;
    if (gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
      const size_t center = (540 * 1920 + 960) * 4;
      if (mapped.size >= 1920 * 1080 * 4 && mapped.data[center + 2] > 160 &&
          mapped.data[center + 2] > mapped.data[center] + 100) {
        digest = QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(mapped.data), mapped.size),
            QCryptographicHash::Sha256).toHex();
      }
      gst_buffer_unmap(buffer, &mapped);
    }
    gst_sample_unref(sample);
    if (!digest.isEmpty()) return digest;
  }
  return {};
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
  // A failed request on the other deck must leave page two and its real frame intact.
  const QString deniedPath = filePath + QStringLiteral(".denied.pdf");
  if (!check(QFile::copy(filePath, deniedPath), "Native denied fixture copy failed")) return false;
  pdf_test::ReadDataDeny deny(deniedPath);
  if (!check(deny.apply(), "Native read-data denial setup failed")) return false;
  QFile deniedOpen(deniedPath);
  if (!check(!deniedOpen.open(QIODevice::ReadOnly), "Native read denial is ineffective")) return false;
  const QByteArray beforeFrame = bluePageFrameHash(source.appSink);
  PdfSourceSessionManager::ProgramSource failedSource;
  if (!check(manager.bindProgramDeck(1, deniedPath, failedSource, &errorDetail),
             "Other-deck denied load was not queued")) return false;
  quint64 failedGeneration = 0;
  QString failedDetail;
  QElapsedTimer failureTimer;
  failureTimer.start();
  bool reported = false;
  while (!reported && failureTimer.elapsed() < 5000) {
    QApplication::processEvents();
    reported = manager.sourceErrorAfter(1, 0, failedGeneration, failedDetail);
    if (!reported) QThread::msleep(1);
  }
  const QByteArray afterFrame = bluePageFrameHash(source.appSink);
  succeeded = check(reported && !failedDetail.isEmpty() && !manager.programSource(1).isValid() &&
      manager.programSource(0).pipeline == source.pipeline && manager.sourceIsPlaying(0) &&
      manager.navigationState(0).currentPage == 2 && !beforeFrame.isEmpty() && beforeFrame == afterFrame,
      "Other-deck read failure changed the on-air page/frame or lost its reason") && succeeded;
  std::cout << "Other-deck read denial frame SHA256=" << afterFrame.constData() << '\n';
  manager.releaseProgramDeck(1);
  if (!check(deny.restore(), "Native DACL restoration failed")) return false;
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
    gst_init(nullptr, nullptr);
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
  succeeded = verifyReadDenied(pdfPath, temporaryDirectory.path()) && succeeded;
  succeeded = verifyCleanupDenied(pdfPath, temporaryDirectory.path()) && succeeded;
  succeeded = verifyBackendRendering(pdfPath) && succeeded;
  succeeded = verifyPortraitRendering(portraitPdfPath) && succeeded;
  succeeded = verifyFramePlans() && succeeded;
  succeeded = verifyPdfAddDialog() && succeeded;
  succeeded = verifyNativeRegistration(pdfPath) && succeeded;
  succeeded = verifyPngSequenceImport(pdfPath) && succeeded;
  succeeded = verifyPngSequenceImport(pdfPath, temporaryDirectory.filePath("custom/PDF")) && succeeded;
  succeeded = verifyInvalidPngSequenceDurations(pdfPath) && succeeded;
  succeeded = verifyPreCanceledImport(pdfPath) && succeeded;
  return succeeded ? 0 : 1;
}
