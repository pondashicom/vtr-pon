#pragma once

#include "document_video_encoder.h"
#include "playlist_item.h"
#include "powerpoint_add_method_dialog.h"

#include <QList>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <optional>

class QWidget;

struct PowerPointVideoFramePlan {
  int framesPerSecond = 30;
  int framesPerSlide = 0;
  int transitionFrames = 0;
  qint64 totalFrames = 0;
  qint64 durationMs = 0;

  bool operator==(const PowerPointVideoFramePlan &) const = default;
};

std::optional<PowerPointVideoFramePlan> makePowerPointVideoFramePlan(
    int slideCount, const PowerPointAddSettings &settings,
    QString *errorDetail = nullptr);

std::optional<PlaylistItemSettings> makePowerPointPngSequenceItemSettings(
    const PowerPointAddSettings &settings, QString *errorDetail = nullptr);

struct PowerPointImportResult {
  bool succeeded = false;
  bool canceled = false;
  QList<PlaylistItemRegistration> items;
  QString generatedDirectory;
  QString generatedRoot;
  QString errorDetail;
};

enum class PowerPointImportPhase { Preparing, Exporting, Encoding };
using PowerPointImportProgress =
    std::function<void(PowerPointImportPhase phase, int progressPermille)>;

class PowerPointImportService final {
public:
  static PowerPointImportResult importFile(
      const QString &filePath, const PowerPointAddSettings &settings,
      int playlistNumber, QWidget *parent = nullptr,
      std::atomic_bool *externalCancelRequested = nullptr,
      const PowerPointImportProgress &progress = {},
      const QString &outputRoot = {});

  static bool discardGeneratedArtifacts(const QString &directoryPath,
                                        const QString &outputRoot = {});
};
