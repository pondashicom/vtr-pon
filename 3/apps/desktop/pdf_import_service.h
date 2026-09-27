#pragma once

#include "pdf_document_backend.h"
#include "playlist_item.h"

#include <QList>
#include <QString>

#include <atomic>
#include <functional>

class QWidget;

enum class PdfAddMethod {
  Native,
  VideoConversion,
  PngSequence,
};

struct PdfAddSettings {
  PdfAddMethod method = PdfAddMethod::Native;
  double pageDurationSeconds = 5.0;
  bool crossDissolveEnabled = true;
  double crossDissolveDurationSeconds = 1.0;
  bool rememberForSession = false;

  bool operator==(const PdfAddSettings &) const = default;
};

struct PdfImportResult {
  bool succeeded = false;
  bool canceled = false;
  QList<PlaylistItemRegistration> items;
  QString generatedDirectory;
  QString generatedRoot;
  PdfDocumentError pdfError = PdfDocumentError::None;
  QString errorDetail;
};

enum class PdfImportPhase { Preparing, Rendering, Encoding };
using PdfImportProgress =
    std::function<void(PdfImportPhase phase, int progressPermille)>;

class PdfImportService final {
public:
  static PdfImportResult importFile(
      const QString &filePath, const PdfAddSettings &settings,
      int playlistNumber, QWidget *parent = nullptr,
      std::atomic_bool *externalCancelRequested = nullptr,
      const PdfImportProgress &progress = {},
      const QString &outputRoot = {});

  static bool discardGeneratedArtifacts(const QString &directoryPath,
                                        const QString &outputRoot = {});
};
