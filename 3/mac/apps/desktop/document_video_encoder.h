#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <optional>

struct DocumentVideoSettings {
  double pageDurationSeconds = 5.0;
  bool crossDissolveEnabled = true;
  double crossDissolveDurationSeconds = 1.0;

  bool operator==(const DocumentVideoSettings &) const = default;
};

struct DocumentVideoFramePlan {
  int framesPerSecond = 30;
  int framesPerPage = 0;
  int transitionFrames = 0;
  qint64 totalFrames = 0;
  qint64 durationMs = 0;

  bool operator==(const DocumentVideoFramePlan &) const = default;
};

std::optional<DocumentVideoFramePlan> makeDocumentVideoFramePlan(
    int pageCount, const DocumentVideoSettings &settings,
    QString *errorDetail = nullptr);

using DocumentVideoProgress =
    std::function<void(qint64 completedFrames, qint64 totalFrames)>;

struct DocumentVideoEncodeResult {
  bool succeeded = false;
  bool canceled = false;
  qint64 durationMs = 0;
  QString outputPath;
  QString errorDetail;
};

DocumentVideoEncodeResult encodeDocumentVideo(
    const QStringList &paths, const QString &outputPath,
    const DocumentVideoSettings &settings,
    const std::atomic_bool *cancelRequested = nullptr,
    const DocumentVideoProgress &progress = {});
