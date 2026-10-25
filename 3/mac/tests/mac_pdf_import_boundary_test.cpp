#include "pdf_boundary_fixtures.h"
#include "pdf_import_service.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <atomic>
#include <memory>
#include <utility>

namespace {
bool check(bool ok, const char *label) {
  qInfo() << (ok ? "PASS" : "FAIL") << label;
  return ok;
}

QByteArray hashFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) return {};
  return hash.result();
}

class PermissionRestore final {
public:
  explicit PermissionRestore(QString path)
      : path_(std::move(path)), original_(QFile::permissions(path_)) {}
  ~PermissionRestore() { (void)restore(); }
  bool set(QFile::Permissions permissions) {
    changed_ = true;
    return QFile::setPermissions(path_, permissions);
  }
  bool restore() {
    if (!changed_) return true;
    const bool restored = !QFileInfo::exists(path_) ||
                          QFile::setPermissions(path_, original_);
    if (restored) changed_ = false;
    return restored;
  }
private:
  QString path_;
  QFile::Permissions original_;
  bool changed_ = false;
};

bool writeSentinel(const QString &root) {
  if (!QDir().mkpath(root)) return false;
  QFile sentinel(QDir(root).filePath(QStringLiteral("keep.txt")));
  if (!sentinel.open(QIODevice::WriteOnly)) return false;
  return sentinel.write("existing conversion stays intact\n") == 33 && sentinel.flush();
}

bool sentinelIntact(const QString &root) {
  QFile sentinel(QDir(root).filePath(QStringLiteral("keep.txt")));
  return sentinel.open(QIODevice::ReadOnly) &&
         sentinel.readAll() == QByteArrayLiteral("existing conversion stays intact\n");
}

bool onlySentinel(const QString &root) {
  return sentinelIntact(root) &&
         QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                              QDir::Name) == QStringList{QStringLiteral("keep.txt")};
}

bool writeDenied(const QString &directory) {
  QFile probe(QDir(directory).filePath(QStringLiteral("permission-probe.tmp")));
  if (!probe.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return true;
  probe.close();
  (void)probe.remove();
  return false;
}

QString stagingDirectory(const QString &root) {
  const auto entries = QDir(root).entryList({QStringLiteral("*.staging")},
                                          QDir::Dirs | QDir::NoDotAndDotDot);
  return entries.size() == 1 ? QDir(root).filePath(entries.first()) : QString();
}

PdfAddSettings settingsFor(PdfAddMethod method) {
  PdfAddSettings settings;
  settings.method = method;
  settings.pageDurationSeconds = 0.1;
  settings.crossDissolveEnabled = false;
  return settings;
}

bool rejectedWithoutArtifacts(const QString &path, const QString &root,
                              PdfDocumentError expected) {
  bool ok = true;
  for (const auto method : {PdfAddMethod::Native, PdfAddMethod::PngSequence,
                            PdfAddMethod::VideoConversion}) {
    const auto result = PdfImportService::importFile(
        path, settingsFor(method), 7, nullptr, nullptr, {}, root);
    ok &= check(!result.succeeded && !result.canceled && result.items.isEmpty() &&
                    result.generatedDirectory.isEmpty() &&
                    !result.errorDetail.isEmpty() &&
                    result.pdfError == expected &&
                    !QFileInfo::exists(root),
                "rejected PDF creates no registrations or conversion directory in every mode");
  }
  return ok;
}

bool rejectedInputChecks(const QString &directory, const QString &smallPdf) {
  const QString encrypted = PdfBoundaryFixtures::encryptedFixturePath();
  const QByteArray encryptedHash = hashFile(encrypted);
  bool ok = check(!encryptedHash.isEmpty(), "encrypted import fixture is readable");
  ok &= rejectedWithoutArtifacts(encrypted, QDir(directory).filePath("encrypted-output"),
                                  PdfDocumentError::PasswordRequired);
  ok &= check(hashFile(encrypted) == encryptedHash,
              "rejected encrypted import preserves source bytes");

  const QString oversized = QDir(directory).filePath("1001-pages.pdf");
  if (!check(PdfBoundaryFixtures::writePdf(oversized, 1001),
             "over-limit import fixture is created")) return false;
  const QByteArray oversizedHash = hashFile(oversized);
  ok &= rejectedWithoutArtifacts(oversized, QDir(directory).filePath("over-limit-output"),
                                  PdfDocumentError::TooManyPages);
  ok &= check(!oversizedHash.isEmpty() && hashFile(oversized) == oversizedHash,
              "rejected over-limit import preserves source bytes");

  const QByteArray sourceHash = hashFile(smallPdf);
  PermissionRestore permissions(smallPdf);
  const bool changed = permissions.set(QFile::Permissions{});
  QFile unreadable(smallPdf);
  const bool denied = !unreadable.open(QIODevice::ReadOnly);
  unreadable.close();
  ok &= check(changed && denied, "read permission fixture actually denies PDF access");
  if (changed && denied) {
    ok &= rejectedWithoutArtifacts(smallPdf, QDir(directory).filePath("unreadable-output"),
                                    PdfDocumentError::LoadFailed);
  }
  ok &= check(permissions.restore() && !sourceHash.isEmpty() &&
                  hashFile(smallPdf) == sourceHash,
              "restoring read permission preserves the PDF source");
  return ok;
}

bool pageLimitChecks(const QString &directory) {
  const QString source = QDir(directory).filePath("1000-pages.pdf");
  if (!check(PdfBoundaryFixtures::writePdf(source, 1000),
             "exact-limit import fixture is created")) return false;
  const QByteArray sourceHash = hashFile(source);
  const QString nativeRoot = QDir(directory).filePath("native-limit-output");
  const auto native = PdfImportService::importFile(
      source, settingsFor(PdfAddMethod::Native), 7, nullptr, nullptr, {}, nativeRoot);
  bool ok = check(native.succeeded && !native.canceled && native.items.size() == 1 &&
                      native.items.first().filePath == QFileInfo(source).absoluteFilePath() &&
                      native.items.first().playlistNumber == 7 &&
                      native.generatedDirectory.isEmpty() && !QFileInfo::exists(nativeRoot),
                  "1000-page native import is accepted without generating output");
  const QString root = QDir(directory).filePath("page-limit-cancel");
  if (!check(writeSentinel(root), "large-page cancellation sentinel is created")) return false;
  for (const auto method : {PdfAddMethod::PngSequence, PdfAddMethod::VideoConversion}) {
    int renderedPages = 0;
    std::atomic_bool canceled{false};
    const auto result = PdfImportService::importFile(
        source, settingsFor(method), 7, nullptr, &canceled,
        [&](PdfImportPhase phase, int) {
          if (phase == PdfImportPhase::Rendering && ++renderedPages == 2)
            canceled.store(true, std::memory_order_release);
        }, root);
    ok &= check(renderedPages == 2 && result.canceled && !result.succeeded &&
                    result.items.isEmpty() && result.generatedDirectory.isEmpty() &&
                    result.errorDetail.isEmpty() && onlySentinel(root),
                "1000-page PNG and video imports cancel after two pages and remove partial output");
  }
  ok &= check(!sourceHash.isEmpty() && hashFile(source) == sourceHash,
              "large-page import and cancellation preserve source bytes");
  return ok;
}

bool largeConversionChecks(const QString &directory) {
  const QString source = QDir(directory).filePath("large-conversion-64MiB.pdf");
  const QString root = QDir(directory).filePath("large-conversion-output");
  if (!check(PdfBoundaryFixtures::writePdf(source, 1, 64LL * 1024 * 1024) &&
                 QFileInfo(source).size() >= 64LL * 1024 * 1024 && writeSentinel(root),
             "large conversion fixture contains a referenced 64 MiB image")) return false;
  const auto original = hashFile(source);
  bool ok = true;
  for (const auto method : {PdfAddMethod::PngSequence, PdfAddMethod::VideoConversion}) {
    const auto result = PdfImportService::importFile(source, settingsFor(method), 7,
                                                     nullptr, nullptr, {}, root);
    ok &= check(result.succeeded && !result.canceled && result.items.size() == 1 &&
                    result.errorDetail.isEmpty() && !result.generatedDirectory.isEmpty(),
                "64 MiB PDF converts through full-size PNG and VideoToolbox MP4 paths");
    if (result.items.size() == 1) {
      ok &= check(QFileInfo(result.items.first().filePath).size() > 0,
                  "large-source conversion commits a nonempty output file");
      if (method == PdfAddMethod::PngSequence) {
        const QImage image(result.items.first().filePath);
        ok &= check(image.size() == QSize(1920, 1080) &&
                        image.pixelColor(960, 540).red() > 220 &&
                        image.pixelColor(960, 540).blue() < 45,
                    "large-source saved PNG preserves full canvas and image color");
      }
    }
    ok &= check(!result.generatedDirectory.isEmpty() &&
                    PdfImportService::discardGeneratedArtifacts(result.generatedDirectory, root) &&
                    onlySentinel(root),
                "large-source output cleanup preserves existing sibling material");
  }
  ok &= check(!original.isEmpty() && hashFile(source) == original,
              "large PDF remains byte-identical after PNG and MP4 conversion");
  return ok;
}

bool outputFailureChecks(const QString &directory, const QString &source) {
  const QString root = QDir(directory).filePath("output-failures");
  if (!check(writeSentinel(root), "output failure sentinel is created")) return false;
  const auto settings = settingsFor(PdfAddMethod::PngSequence);
  const QByteArray sourceHash = hashFile(source);
  constexpr auto readExecute = QFile::ReadOwner | QFile::ExeOwner;
  bool ok = true;

  {
    PermissionRestore rootPermissions(root);
    const bool denied = rootPermissions.set(readExecute) && writeDenied(root);
    ok &= check(denied, "output root permission fixture actually denies new files");
    if (denied) {
      const auto result = PdfImportService::importFile(source, settings, 7,
                                                       nullptr, nullptr, {}, root);
      ok &= check(!result.succeeded && !result.canceled && result.items.isEmpty() &&
                      result.generatedDirectory.isEmpty() && !result.errorDetail.isEmpty() &&
                      onlySentinel(root),
                  "unwritable root fails before producing any page or registration");
    }
    ok &= check(rootPermissions.restore(), "output root permissions restore");
  }

  // A directory with no files can still be removed from its writable parent.
  // Deny the first PNG write and verify cleanup succeeds without residual output.
  {
    QString staging;
    std::unique_ptr<PermissionRestore> stagingPermissions;
    bool denied = false;
    const auto result = PdfImportService::importFile(source, settings, 7, nullptr, nullptr,
        [&](PdfImportPhase phase, int) {
          if (phase != PdfImportPhase::Preparing) return;
          staging = stagingDirectory(root);
          if (staging.isEmpty()) return;
          stagingPermissions = std::make_unique<PermissionRestore>(staging);
          denied = stagingPermissions->set(readExecute) && writeDenied(staging);
        }, root);
    ok &= check(denied && !result.succeeded && !result.canceled && result.items.isEmpty() &&
                    result.pdfError == PdfDocumentError::RenderFailed &&
                    !result.errorDetail.isEmpty() && result.generatedDirectory.isEmpty() &&
                    !QFileInfo::exists(staging) && onlySentinel(root),
                "denied first PNG save reports failure and removes empty staging directory");
    if (stagingPermissions)
      ok &= check(stagingPermissions->restore(), "staging permission guard restores safely after removal");
  }

  // Deny a subsequent PNG write after one page exists. The same permission also
  // prevents removing that page, so the residual directory must be reported.
  {
    QString staging;
    std::unique_ptr<PermissionRestore> stagingPermissions;
    int renderedPages = 0;
    bool denied = false;
    const auto result = PdfImportService::importFile(source, settings, 7, nullptr, nullptr,
        [&](PdfImportPhase phase, int) {
          if (phase != PdfImportPhase::Rendering || ++renderedPages != 1) return;
          staging = stagingDirectory(root);
          if (staging.isEmpty()) return;
          stagingPermissions = std::make_unique<PermissionRestore>(staging);
          denied = stagingPermissions->set(readExecute) && writeDenied(staging);
        }, root);
    ok &= check(denied && renderedPages == 1 && !result.succeeded && !result.canceled &&
                    result.items.isEmpty() && result.pdfError == PdfDocumentError::RenderFailed &&
                    result.generatedDirectory == QFileInfo(staging).absoluteFilePath() &&
                    result.errorDetail.contains(result.generatedDirectory) &&
                    QFileInfo::exists(staging) && sentinelIntact(root),
                "failed later PNG save reports undeletable partial output without registering it");
    if (stagingPermissions)
      ok &= check(stagingPermissions->restore(), "partial-output permissions restore");
    ok &= check(!staging.isEmpty() && PdfImportService::discardGeneratedArtifacts(staging, root) &&
                    onlySentinel(root),
                "partial output can be recovered after restoring permissions");
  }

  for (const bool cancel : {false, true}) {
    PermissionRestore rootPermissions(root);
    std::atomic_bool canceled{false};
    QString staging;
    bool denied = false;
    const auto result = PdfImportService::importFile(source, settings, 7, nullptr, &canceled,
        [&](PdfImportPhase phase, int progress) {
          if (phase != PdfImportPhase::Rendering || denied || (!cancel && progress != 1000))
            return;
          staging = stagingDirectory(root);
          denied = !staging.isEmpty() && rootPermissions.set(readExecute) && writeDenied(root);
          if (cancel && denied) canceled.store(true, std::memory_order_release);
        }, root);
    ok &= check(denied && !result.succeeded && !result.canceled && result.items.isEmpty() &&
                    result.generatedDirectory == QFileInfo(staging).absoluteFilePath() &&
                    result.errorDetail.contains(result.generatedDirectory) &&
                    QFileInfo::exists(staging) && sentinelIntact(root),
                cancel ? "cancellation cleanup denial reports a residual path as failure"
                       : "commit and cleanup denial report a residual path without registrations");
    ok &= check(rootPermissions.restore(), "commit/cancel root permissions restore");
    ok &= check(!staging.isEmpty() && PdfImportService::discardGeneratedArtifacts(staging, root) &&
                    onlySentinel(root),
                "commit/cancel residual output is removable after restoring permissions");
  }

  const auto recovered = PdfImportService::importFile(source, settings, 7,
                                                     nullptr, nullptr, {}, root);
  ok &= check(recovered.succeeded && recovered.items.size() == 2 &&
                  recovered.errorDetail.isEmpty() && sentinelIntact(root),
              "normal conversion succeeds after permission failures");
  if (!recovered.generatedDirectory.isEmpty())
    ok &= check(PdfImportService::discardGeneratedArtifacts(recovered.generatedDirectory, root),
                "recovered conversion output is removable");
  ok &= check(onlySentinel(root) && !sourceHash.isEmpty() && hashFile(source) == sourceHash,
              "all output failures preserve source bytes and existing sibling output");
  return ok;
}
} // namespace

bool runPdfImportBoundaryChecks(const QString &directory) {
  const QString root = QDir(directory).filePath("import-boundaries");
  if (!check(QDir().mkpath(root), "import boundary fixture directory is created")) return false;
  const QString source = QDir(root).filePath("source.pdf");
  if (!check(PdfBoundaryFixtures::writePdf(source, 2),
             "two-page import boundary fixture is created")) return false;
  bool ok = rejectedInputChecks(root, source);
  ok = pageLimitChecks(root) && ok;
  ok = largeConversionChecks(root) && ok;
  ok = outputFailureChecks(root, source) && ok;
  return ok;
}
