#include "pdf_import_service.h"

#include "document_video_encoder.h"
#include "media_duration_probe.h"
#include "product_identity.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

constexpr int kRenderProgressMaximumForVideo = 400;

struct PdfPageRenderResult {
  bool succeeded = false;
  bool canceled = false;
  QStringList pagePaths;
  PdfDocumentError pdfError = PdfDocumentError::None;
  QString errorDetail;
};

QString generatedRootDirectory() {
  const QString documentsDirectory =
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return documentsDirectory.isEmpty()
             ? QString()
             : QDir(documentsDirectory).filePath(
                   vtrpon::currentProductStorageName() +
                   QStringLiteral("/Generated/PDF"));
}

bool isReservedWindowsFileName(const QString &component) {
  const QString baseName = component.section(QLatin1Char('.'), 0, 0).toUpper();
  static const QRegularExpression reserved(
      QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"));
  return reserved.match(baseName).hasMatch();
}

QString safeFileNameComponent(const QString &value, const QString &fallback,
                              int maximumLength) {
  QString component = value.trimmed();
  component.replace(
      QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]+")),
      QStringLiteral("_"));
  component.replace(QRegularExpression(QStringLiteral("\\s+")),
                    QStringLiteral("_"));
  component.remove(QRegularExpression(QStringLiteral("[. ]+$")));
  if (component.isEmpty()) {
    component = fallback;
  }
  component = component.left(maximumLength);
  component.remove(QRegularExpression(QStringLiteral("[. ]+$")));
  if (component.isEmpty()) {
    component = fallback;
  }
  if (isReservedWindowsFileName(component)) {
    component.prepend(QLatin1Char('_'));
  }
  return component;
}

QString safeArtifactStem(const QString &filePath) {
  return safeFileNameComponent(QFileInfo(filePath).completeBaseName(),
                               QStringLiteral("pdf"), 72);
}

QString pageArtifactFileName(const QString &sourceStem,
                             const QString &pageLabel, int pageIndex,
                             int pageCount) {
  const int digits =
      (std::max)(3, static_cast<int>(QString::number(pageCount).size()));
  const QString paddedIndex =
      QStringLiteral("%1").arg(pageIndex + 1, digits, 10, QLatin1Char('0'));
  const QString safeLabel = safeFileNameComponent(
      pageLabel, QString::number(pageIndex + 1), 56);
  return QStringLiteral("%1-page-%2-%3.png")
      .arg(sourceStem, paddedIndex, safeLabel);
}

bool pathIsDirectChildOfGeneratedRoot(const QString &directoryPath, const QString &rootPath) {
  const QFileInfo rootInfo(rootPath);
  const QFileInfo candidateInfo(directoryPath);
#ifdef _WIN32
  constexpr auto pathCaseSensitivity = Qt::CaseInsensitive;
#else
  constexpr auto pathCaseSensitivity = Qt::CaseSensitive;
#endif
  // Never follow a cleanup target that was replaced by a directory symlink.
  if (candidateInfo.isSymLink()) return false;
  const QString rootAbsolute =
      QDir::cleanPath(rootInfo.absoluteFilePath());
  const QString rootCanonical =
      QDir::cleanPath(rootInfo.canonicalFilePath());
  const QString candidateAbsolute =
      QDir::cleanPath(candidateInfo.absoluteFilePath());
  if (rootAbsolute.isEmpty() || rootCanonical.isEmpty() ||
      candidateAbsolute.isEmpty() ||
      candidateAbsolute.compare(rootAbsolute, pathCaseSensitivity) == 0) {
    return false;
  }
  const QString lexicalParent = QFileInfo(candidateAbsolute).absolutePath();
  if (lexicalParent.isEmpty() ||
      lexicalParent.compare(rootAbsolute, pathCaseSensitivity) != 0) {
    return false;
  }
  if (!candidateInfo.exists()) {
    return true;
  }
  if (!candidateInfo.isDir()) {
    return false;
  }
  const QString candidate =
      QDir::cleanPath(candidateInfo.canonicalFilePath());
  const QString canonicalParent = QFileInfo(candidate).absolutePath();
  return !candidate.isEmpty() && !canonicalParent.isEmpty() &&
         canonicalParent.compare(rootCanonical, pathCaseSensitivity) == 0;
}

void clearGeneratedDirectoryReadOnlyAttribute(const QString &directoryPath) {
#ifdef _WIN32
  const std::wstring nativePath =
      QDir::toNativeSeparators(directoryPath).toStdWString();
  const DWORD attributes = GetFileAttributesW(nativePath.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
    (void)SetFileAttributesW(nativePath.c_str(),
                             attributes & ~FILE_ATTRIBUTE_READONLY);
  }
#else
  Q_UNUSED(directoryPath);
#endif
}

DocumentVideoSettings documentVideoSettings(const PdfAddSettings &settings) {
  DocumentVideoSettings videoSettings;
  videoSettings.pageDurationSeconds = settings.pageDurationSeconds;
  videoSettings.crossDissolveEnabled = settings.crossDissolveEnabled;
  videoSettings.crossDissolveDurationSeconds =
      settings.crossDissolveDurationSeconds;
  return videoSettings;
}

PlaylistItemRegistration nativeRegistration(const QString &filePath,
                                            int playlistNumber) {
  PlaylistItemRegistration item;
  item.filePath = filePath;
  item.displayName = QFileInfo(filePath).completeBaseName();
  item.sourceType = PlaylistSourceType::Pdf;
  item.mediaType = PlaylistMediaType::Pdf;
  item.settings.startMode = PlaylistStartMode::Play;
  item.settings.endMode = PlaylistEndMode::Pause;
  item.settings.outPointMs = std::nullopt;
  item.playlistNumber = playlistNumber;
  return item;
}

} // namespace

PdfImportResult PdfImportService::importFile(
    const QString &filePath, const PdfAddSettings &settings,
    int playlistNumber, QWidget *parent,
    std::atomic_bool *externalCancelRequested,
    const PdfImportProgress &progress, const QString &outputRoot) {
  Q_UNUSED(parent);
  PdfImportResult result;
  if (settings.method != PdfAddMethod::Native &&
      settings.method != PdfAddMethod::VideoConversion &&
      settings.method != PdfAddMethod::PngSequence) {
    result.errorDetail = QStringLiteral("The PDF add method is invalid.");
    return result;
  }
  if (settings.method == PdfAddMethod::PngSequence &&
      (!std::isfinite(settings.pageDurationSeconds) ||
       settings.pageDurationSeconds < 0.1 ||
       settings.pageDurationSeconds > 600.0)) {
    result.errorDetail = QStringLiteral(
        "The PDF PNG-sequence page duration must be between 0.1 and 600.0 "
        "seconds.");
    return result;
  }
  if (externalCancelRequested &&
      externalCancelRequested->load(std::memory_order_acquire)) {
    result.canceled = true;
    return result;
  }

  PdfDocumentBackend validationBackend;
  if (!validationBackend.load(filePath)) {
    result.pdfError = validationBackend.error();
    result.errorDetail = validationBackend.errorDetail();
    return result;
  }
  const QString absoluteSourcePath = validationBackend.filePath();
  const int pageCount = validationBackend.pageCount();
  if (settings.method == PdfAddMethod::Native) {
    if (externalCancelRequested &&
        externalCancelRequested->load(std::memory_order_acquire)) {
      result.canceled = true;
      return result;
    }
    result.items.append(
        nativeRegistration(absoluteSourcePath, playlistNumber));
    result.succeeded = true;
    return result;
  }
  validationBackend.close();

  const DocumentVideoSettings videoSettings =
      documentVideoSettings(settings);
  if (settings.method == PdfAddMethod::VideoConversion) {
    QString planError;
    if (!makeDocumentVideoFramePlan(pageCount, videoSettings, &planError)) {
      result.errorDetail = planError;
      return result;
    }
  }

  const QString rootPath = outputRoot.isEmpty() ? generatedRootDirectory() : outputRoot;
  result.generatedRoot = rootPath;
  if (rootPath.isEmpty() || !QDir().mkpath(rootPath)) {
    result.errorDetail =
        QStringLiteral("The PDF generated-media directory is unavailable.");
    return result;
  }
  const QString sourceStem = safeArtifactStem(absoluteSourcePath);
  const QString uniqueId =
      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
  const QString finalName =
      QStringLiteral("%1-%2-%3")
          .arg(sourceStem,
               QDateTime::currentDateTimeUtc().toString(
                   QStringLiteral("yyyyMMdd-HHmmsszzz")),
               uniqueId);
  const QString stagingName = finalName + QStringLiteral(".staging");
  const QString stagingPath = QDir(rootPath).filePath(stagingName);
  const QString finalPath = QDir(rootPath).filePath(finalName);
  if (!QDir().mkpath(stagingPath)) {
    result.errorDetail =
        QStringLiteral("Could not create the PDF staging directory.");
    return result;
  }

  const auto discardOrReport = [&result, &rootPath](const QString &directoryPath) {
    if (PdfImportService::discardGeneratedArtifacts(directoryPath, rootPath)) {
      return true;
    }
    const QString residualPath =
        QFileInfo(directoryPath).absoluteFilePath();
    const QString cleanupDetail = QStringLiteral(
        "Generated PDF artifacts could not be removed: %1")
                                      .arg(QDir::toNativeSeparators(
                                          residualPath));
    if (!result.errorDetail.isEmpty()) {
      result.errorDetail.append(QLatin1Char(' '));
    }
    result.errorDetail.append(cleanupDetail);
    result.generatedDirectory = residualPath;
    return false;
  };

  std::atomic_bool localCancelRequested{false};
  std::atomic_bool *const cancelRequested =
      externalCancelRequested ? externalCancelRequested
                              : &localCancelRequested;
  PdfPageRenderResult renderResult;
  DocumentVideoEncodeResult encodeResult;
  if (progress) {
    progress(PdfImportPhase::Preparing, 0);
  }
  PdfDocumentBackend backend;
  if (!backend.load(absoluteSourcePath)) {
    renderResult.pdfError = backend.error();
    renderResult.errorDetail = backend.errorDetail();
  } else {
    renderResult.pagePaths.reserve(backend.pageCount());
    for (int pageIndex = 0; pageIndex < backend.pageCount(); ++pageIndex) {
      if (cancelRequested->load(std::memory_order_acquire)) {
        renderResult.canceled = true;
        break;
      }
      const QString pageLabel = backend.pageLabel(pageIndex);
      const QImage pageImage = backend.renderPage(pageIndex);
      if (pageImage.isNull()) {
        renderResult.pdfError = backend.error();
        renderResult.errorDetail = backend.errorDetail();
        break;
      }
      const QString outputPath = QDir(stagingPath).filePath(
          pageArtifactFileName(sourceStem, pageLabel, pageIndex,
                               backend.pageCount()));
      if (!pageImage.save(outputPath, "PNG")) {
        renderResult.pdfError = PdfDocumentError::RenderFailed;
        renderResult.errorDetail = QStringLiteral(
            "A rendered PDF page could not be saved as PNG.");
        break;
      }
      renderResult.pagePaths.append(QFileInfo(outputPath).absoluteFilePath());

      const int renderMaximum =
          settings.method == PdfAddMethod::VideoConversion
              ? kRenderProgressMaximumForVideo
              : 1000;
      const int value =
          (pageIndex + 1) * renderMaximum / backend.pageCount();
      if (progress) {
        progress(PdfImportPhase::Rendering, value);
      }
    }
    if (!renderResult.canceled &&
        renderResult.pdfError == PdfDocumentError::None &&
        !cancelRequested->load(std::memory_order_acquire)) {
      renderResult.succeeded = !renderResult.pagePaths.isEmpty();
    }
  }
  if (renderResult.succeeded &&
      settings.method == PdfAddMethod::VideoConversion) {
    const QString videoPath = QDir(stagingPath).filePath(
        sourceStem + QStringLiteral("-converted.mp4"));
    encodeResult = encodeDocumentVideo(
        renderResult.pagePaths, videoPath, videoSettings, cancelRequested,
        [&](qint64 completed, qint64 total) {
          const int value =
              total > 0
                  ? kRenderProgressMaximumForVideo +
                        static_cast<int>(
                            completed *
                            (1000 - kRenderProgressMaximumForVideo) / total)
                  : kRenderProgressMaximumForVideo;
          if (progress) {
            progress(PdfImportPhase::Encoding, value);
          }
        });
  }

  result.canceled = cancelRequested->load(std::memory_order_acquire) ||
                    renderResult.canceled || encodeResult.canceled;
  if (result.canceled) {
    if (!discardOrReport(stagingPath)) {
      result.canceled = false;
    }
    return result;
  }
  if (!renderResult.succeeded) {
    result.pdfError = renderResult.pdfError;
    result.errorDetail =
        renderResult.errorDetail.isEmpty()
            ? QStringLiteral("PDF conversion produced no rendered pages.")
            : renderResult.errorDetail;
    (void)discardOrReport(stagingPath);
    return result;
  }
  if (settings.method == PdfAddMethod::VideoConversion &&
      !encodeResult.succeeded) {
    result.errorDetail = encodeResult.errorDetail;
    (void)discardOrReport(stagingPath);
    return result;
  }

  qint64 videoDurationMs = 0;
  if (settings.method == PdfAddMethod::VideoConversion) {
    const MediaDurationProbeResult probe =
        probeMediaDuration(encodeResult.outputPath);
    if (!probe.succeeded || probe.mediaType != PlaylistMediaType::Video ||
        !probe.durationMs || *probe.durationMs <= 0) {
      result.errorDetail =
          probe.errorDetail.isEmpty()
              ? QStringLiteral(
                    "The generated PDF video could not be validated.")
              : probe.errorDetail;
      (void)discardOrReport(stagingPath);
      return result;
    }
    videoDurationMs = *probe.durationMs;
    for (const QString &pagePath : renderResult.pagePaths) {
      if (QFile::exists(pagePath) && !QFile::remove(pagePath)) {
        result.errorDetail =
            QStringLiteral("Could not remove a temporary PDF page image.");
        (void)discardOrReport(stagingPath);
        return result;
      }
    }
  }

  if (!QDir(rootPath).rename(stagingName, finalName)) {
    result.errorDetail = QStringLiteral(
        "Could not commit the generated PDF media directory.");
    (void)discardOrReport(stagingPath);
    return result;
  }

  result.generatedDirectory = QFileInfo(finalPath).absoluteFilePath();
  if (settings.method == PdfAddMethod::PngSequence) {
    result.items.reserve(renderResult.pagePaths.size());
    for (const QString &stagingPagePath : renderResult.pagePaths) {
      PlaylistItemRegistration item;
      item.filePath = QDir(finalPath).filePath(
          QFileInfo(stagingPagePath).fileName());
      item.displayName = QFileInfo(item.filePath).completeBaseName();
      item.sourceType = PlaylistSourceType::File;
      item.mediaType = PlaylistMediaType::StillImage;
      item.settings.outPointMs =
          qRound64(settings.pageDurationSeconds * 1000.0);
      item.playlistNumber = playlistNumber;
      result.items.append(item);
    }
  } else {
    PlaylistItemRegistration item;
    item.filePath = QDir(finalPath).filePath(
        QFileInfo(encodeResult.outputPath).fileName());
    item.displayName = QFileInfo(item.filePath).completeBaseName();
    item.sourceType = PlaylistSourceType::File;
    item.mediaType = PlaylistMediaType::Video;
    item.settings.outPointMs = videoDurationMs;
    item.playlistNumber = playlistNumber;
    result.items.append(item);
  }

  result.succeeded = !result.items.isEmpty();
  if (!result.succeeded) {
    result.errorDetail =
        QStringLiteral("PDF conversion produced no playlist items.");
    if (discardOrReport(result.generatedDirectory)) {
      result.generatedDirectory.clear();
    }
  }
  return result;
}

bool PdfImportService::discardGeneratedArtifacts(
    const QString &directoryPath, const QString &outputRoot) {
  const QString rootPath = outputRoot.isEmpty() ? generatedRootDirectory() : outputRoot;
  if (!pathIsDirectChildOfGeneratedRoot(directoryPath, rootPath)) {
    return false;
  }
  for (int attempt = 0; attempt < 40; ++attempt) {
    QDir directory(directoryPath);
    if (!directory.exists()) {
      return true;
    }
    clearGeneratedDirectoryReadOnlyAttribute(directoryPath);
    if (directory.removeRecursively()) {
      return true;
    }
    if (directory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
            .isEmpty() &&
        QDir().rmdir(directoryPath)) {
      return true;
    }
    QThread::msleep(50);
  }
  return !QDir(directoryPath).exists();
}
