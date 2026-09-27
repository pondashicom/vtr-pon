#include "powerpoint_import_service.h"

#include "language_text.h"
#include "media_duration_probe.h"
#include "powerpoint_source_session_manager.h"
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

#include <atomic>
#include <cmath>

namespace {

QString generatedRootDirectory() {
  const QString documentsDirectory =
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return documentsDirectory.isEmpty()
             ? QString()
             : QDir(documentsDirectory).filePath(
                   vtrpon::currentProductStorageName() +
                   QStringLiteral("/Generated/PowerPoint"));
}

QString safeArtifactStem(const QString &filePath) {
  QString stem = QFileInfo(filePath).completeBaseName().trimmed();
  stem.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]+")),
               QStringLiteral("_"));
  stem.replace(QRegularExpression(QStringLiteral("\\s+$")), QString());
  if (stem.isEmpty()) {
    stem = QStringLiteral("powerpoint");
  }
  return stem.left(80);
}

bool pathIsDirectChildOfGeneratedRoot(const QString &directoryPath, const QString &rootPath) {
  const QString root = QDir::cleanPath(
      QFileInfo(rootPath).canonicalFilePath());
  const QString candidate = QDir::cleanPath(
      QFileInfo(directoryPath).canonicalFilePath());
  if (root.isEmpty() || candidate.isEmpty() || candidate == root) {
    return false;
  }
  const QString candidateParent =
      QDir(QFileInfo(candidate).absolutePath()).canonicalPath();
  return !candidateParent.isEmpty() &&
         candidateParent.compare(root, Qt::CaseInsensitive) == 0;
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

DocumentVideoSettings documentVideoSettings(
    const PowerPointAddSettings &settings) {
  DocumentVideoSettings result;
  result.pageDurationSeconds = settings.slideDurationSeconds;
  result.crossDissolveEnabled = settings.crossDissolveEnabled;
  result.crossDissolveDurationSeconds =
      settings.crossDissolveDurationSeconds;
  return result;
}

PlaylistItemRegistration nativeRegistration(const QString &filePath,
                                            int playlistNumber) {
  PlaylistItemRegistration item;
  item.filePath = filePath;
  item.sourceType = PlaylistSourceType::PowerPoint;
  item.mediaType = PlaylistMediaType::Presentation;
  item.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
  item.settings.startMode = PlaylistStartMode::Play;
  item.settings.endMode = PlaylistEndMode::Pause;
  item.settings.outPointMs = std::nullopt;
  item.playlistNumber = playlistNumber;
  return item;
}

} // namespace

std::optional<PowerPointVideoFramePlan> makePowerPointVideoFramePlan(
    int slideCount, const PowerPointAddSettings &settings,
    QString *errorDetail) {
  QString documentError;
  const auto documentPlan = makeDocumentVideoFramePlan(
      slideCount, documentVideoSettings(settings), &documentError);
  if (!documentPlan) {
    if (errorDetail) {
      if (slideCount <= 0) {
        *errorDetail = QStringLiteral("The slide duration is invalid.");
      } else {
        documentError.replace(QStringLiteral("page duration"),
                              QStringLiteral("slide duration"));
        *errorDetail = documentError;
      }
    }
    return std::nullopt;
  }
  if (errorDetail) {
    errorDetail->clear();
  }
  PowerPointVideoFramePlan plan;
  plan.framesPerSecond = documentPlan->framesPerSecond;
  plan.framesPerSlide = documentPlan->framesPerPage;
  plan.transitionFrames = documentPlan->transitionFrames;
  plan.totalFrames = documentPlan->totalFrames;
  plan.durationMs = documentPlan->durationMs;
  return plan;
}

std::optional<PlaylistItemSettings> makePowerPointPngSequenceItemSettings(
    const PowerPointAddSettings &settings, QString *errorDetail) {
  if (!std::isfinite(settings.slideDurationSeconds) ||
      settings.slideDurationSeconds < 0.1 ||
      settings.slideDurationSeconds > 600.0) {
    if (errorDetail) {
      *errorDetail = QStringLiteral(
          "The slide duration must be between 0.1 and 600.0 seconds.");
    }
    return std::nullopt;
  }

  if (errorDetail) {
    errorDetail->clear();
  }
  PlaylistItemSettings itemSettings;
  itemSettings.outPointMs = qRound64(settings.slideDurationSeconds * 1000.0);
  return itemSettings;
}

PowerPointImportResult PowerPointImportService::importFile(
    const QString &filePath, const PowerPointAddSettings &settings,
    int playlistNumber, QWidget *parent,
    std::atomic_bool *externalCancelRequested,
    const PowerPointImportProgress &progress, const QString &outputRoot) {
  Q_UNUSED(parent);
  PowerPointImportResult result;
  if (externalCancelRequested && externalCancelRequested->load(std::memory_order_acquire)) {
    result.canceled = true;
    return result;
  }
  const QString sourceError = validatePowerPointSource(filePath);
  if (!sourceError.isEmpty()) {
    result.errorDetail = sourceError;
    return result;
  }
  if (settings.method == PowerPointAddMethod::NativeSlideShow) {
    if (externalCancelRequested && externalCancelRequested->load(std::memory_order_acquire)) {
      result.canceled = true;
      return result;
    }
    result.items.append(nativeRegistration(filePath, playlistNumber));
    result.succeeded = true;
    return result;
  }
  std::optional<PlaylistItemSettings> pngSequenceItemSettings = std::nullopt;
  if (settings.method == PowerPointAddMethod::PngSequence) {
    pngSequenceItemSettings =
        makePowerPointPngSequenceItemSettings(settings, &result.errorDetail);
    if (!pngSequenceItemSettings) {
      return result;
    }
  }
  if (isPowerPointNativeSourceActive()) {
    result.errorDetail =
        vtrpon::uiText("powerpoint.import.native_source_active");
    return result;
  }

  const QString rootPath = outputRoot.isEmpty() ? generatedRootDirectory() : outputRoot;
  result.generatedRoot = rootPath;
  if (rootPath.isEmpty() || !QDir().mkpath(rootPath)) {
    result.errorDetail = QStringLiteral(
        "The PowerPoint generated-media directory is unavailable.");
    return result;
  }
  const QString stem = safeArtifactStem(filePath);
  const QString uniqueId =
      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
  const QString finalName =
      QStringLiteral("%1-%2-%3")
          .arg(stem,
               QDateTime::currentDateTimeUtc().toString(
                   QStringLiteral("yyyyMMdd-HHmmsszzz")),
               uniqueId);
  const QString stagingName = QStringLiteral("%1.staging").arg(finalName);
  const QString stagingPath = QDir(rootPath).filePath(stagingName);
  const QString finalPath = QDir(rootPath).filePath(finalName);
  if (!QDir().mkpath(stagingPath)) {
    result.errorDetail =
        QStringLiteral("Could not create the PowerPoint staging directory.");
    return result;
  }

  std::atomic_bool localCancelRequested{false};
  std::atomic_bool *const cancelRequested =
      externalCancelRequested ? externalCancelRequested
                              : &localCancelRequested;
  PowerPointSlideExportResult exportResult;
  DocumentVideoEncodeResult encodeResult;
  if (progress) {
    progress(PowerPointImportPhase::Preparing, 0);
  }
  exportResult = exportPowerPointSlides(
      filePath, stagingPath, cancelRequested,
      [&](int completed, int total) {
        const int value =
            total > 0
                ? completed *
                      (settings.method == PowerPointAddMethod::VideoConversion
                           ? 450
                           : 1000) /
                      total
                : 0;
        if (progress) {
          progress(PowerPointImportPhase::Exporting, value);
        }
      });
  if (exportResult.succeeded && !exportResult.canceled &&
      !cancelRequested->load(std::memory_order_acquire) &&
      settings.method == PowerPointAddMethod::VideoConversion) {
    QString settingsError;
    if (!makePowerPointVideoFramePlan(exportResult.slidePaths.size(), settings,
                                      &settingsError)) {
      encodeResult.errorDetail = settingsError;
    } else {
      const QString videoPath = QDir(stagingPath).filePath(
          stem + QStringLiteral("-converted.mp4"));
      encodeResult = encodeDocumentVideo(
          exportResult.slidePaths, videoPath, documentVideoSettings(settings),
          cancelRequested, [&](qint64 completed, qint64 total) {
            const int value = total > 0
                                  ? 450 + static_cast<int>(completed * 550 /
                                                           total)
                                  : 450;
            if (progress) {
              progress(PowerPointImportPhase::Encoding, value);
            }
          });
    }
  }

  result.canceled = cancelRequested->load(std::memory_order_acquire) ||
                    exportResult.canceled || encodeResult.canceled;
  if (result.canceled) {
    (void)discardGeneratedArtifacts(stagingPath, rootPath);
    return result;
  }
  if (!exportResult.succeeded) {
    result.errorDetail = exportResult.errorDetail;
    (void)discardGeneratedArtifacts(stagingPath, rootPath);
    return result;
  }
  if (settings.method == PowerPointAddMethod::VideoConversion &&
      !encodeResult.succeeded) {
    result.errorDetail = encodeResult.errorDetail;
    (void)discardGeneratedArtifacts(stagingPath, rootPath);
    return result;
  }
  if (settings.method == PowerPointAddMethod::VideoConversion) {
    const MediaDurationProbeResult probe =
        probeMediaDuration(encodeResult.outputPath);
    if (!probe.succeeded || probe.mediaType != PlaylistMediaType::Video ||
        !probe.durationMs || *probe.durationMs <= 0) {
      result.errorDetail = probe.errorDetail.isEmpty()
                               ? QStringLiteral(
                                     "The generated MP4 video could not be validated.")
                               : probe.errorDetail;
      (void)discardGeneratedArtifacts(stagingPath, rootPath);
      return result;
    }
    encodeResult.durationMs = *probe.durationMs;
    for (const QString &slidePath : exportResult.slidePaths) {
      if (QFile::exists(slidePath) && !QFile::remove(slidePath)) {
        result.errorDetail = QStringLiteral(
            "Could not remove a temporary PowerPoint slide image.");
        (void)discardGeneratedArtifacts(stagingPath, rootPath);
        return result;
      }
    }
  }
  if (!QDir(rootPath).rename(stagingName, finalName)) {
    result.errorDetail = QStringLiteral(
        "Could not commit the generated PowerPoint media directory.");
    (void)discardGeneratedArtifacts(stagingPath, rootPath);
    return result;
  }

  result.generatedDirectory = QFileInfo(finalPath).absoluteFilePath();
  if (settings.method == PowerPointAddMethod::PngSequence) {
    result.items.reserve(exportResult.slidePaths.size());
    for (const QString &stagingSlidePath : exportResult.slidePaths) {
      PlaylistItemRegistration item;
      item.filePath =
          QDir(finalPath).filePath(QFileInfo(stagingSlidePath).fileName());
      item.sourceType = PlaylistSourceType::File;
      item.mediaType = PlaylistMediaType::StillImage;
      item.powerPointMode = PlaylistPowerPointMode::PngSequence;
      item.settings = *pngSequenceItemSettings;
      item.playlistNumber = playlistNumber;
      result.items.append(item);
    }
  } else {
    PlaylistItemRegistration item;
    item.filePath =
        QDir(finalPath).filePath(QFileInfo(encodeResult.outputPath).fileName());
    item.sourceType = PlaylistSourceType::File;
    item.mediaType = PlaylistMediaType::Video;
    item.powerPointMode = PlaylistPowerPointMode::VideoConversion;
    item.settings.outPointMs = encodeResult.durationMs;
    item.playlistNumber = playlistNumber;
    result.items.append(item);
  }
  result.succeeded = !result.items.isEmpty();
  if (!result.succeeded) {
    result.errorDetail =
        QStringLiteral("PowerPoint conversion produced no playlist items.");
    (void)discardGeneratedArtifacts(result.generatedDirectory, rootPath);
    result.generatedDirectory.clear();
  }
  return result;
}

bool PowerPointImportService::discardGeneratedArtifacts(
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
