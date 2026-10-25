#include "pdf_boundary_fixtures.h"
#include "pdf_document_backend.h"
#include "pdf_source_session_manager.h"

#include <QCoreApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPdfDocument>
#include <QStringList>
#include <QThread>

#include <cmath>
#include <functional>
#include <sys/stat.h>

namespace {

bool check(bool condition, const char *label) {
  qInfo() << (condition ? "PASS" : "FAIL") << label;
  return condition;
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 10000) {
  QElapsedTimer elapsed;
  elapsed.start();
  do {
    QCoreApplication::processEvents();
    if (condition()) return true;
    QThread::msleep(2);
  } while (elapsed.elapsed() < timeoutMs);
  return false;
}

bool near(const QImage &image, QPoint point, QColor expected) {
  if (image.isNull() || !image.rect().contains(point)) return false;
  const QColor actual = image.pixelColor(point);
  return actual.alpha() == 255 && std::abs(actual.red() - expected.red()) <= 4 &&
         std::abs(actual.green() - expected.green()) <= 4 &&
         std::abs(actual.blue() - expected.blue()) <= 4;
}

class FileModeRestore final {
public:
  explicit FileModeRestore(const QString &path) : path_(QFile::encodeName(path)) {
    struct stat info {};
    valid_ = ::stat(path_.constData(), &info) == 0;
    if (valid_) mode_ = info.st_mode & 07777;
  }
  ~FileModeRestore() {
    if (valid_) ::chmod(path_.constData(), mode_);
  }
  bool set(mode_t mode) const {
    return valid_ && ::chmod(path_.constData(), mode) == 0;
  }
  bool restore() const { return set(mode_); }

private:
  QByteArray path_;
  mode_t mode_ = 0;
  bool valid_ = false;
};

bool encryptedChecks(const QString &plainPath) {
  const QString encrypted = PdfBoundaryFixtures::encryptedFixturePath();
  QPdfDocument fixtureProof;
  fixtureProof.setPassword(PdfBoundaryFixtures::password());
  bool ok = check(fixtureProof.load(encrypted) == QPdfDocument::Error::None &&
                      fixtureProof.status() == QPdfDocument::Status::Ready &&
                      fixtureProof.pageCount() == 1,
                  "encrypted fixture is a valid one-page PDF with its known password");
  fixtureProof.close();
  PdfDocumentBackend backend;
  for (const QString &password : {QString(), QStringLiteral("wrong-password")}) {
    ok &= check(!backend.load(encrypted, password) &&
                    backend.error() == PdfDocumentError::PasswordRequired &&
                    !backend.errorDetail().isEmpty() && !backend.isLoaded() &&
                    backend.pageCount() == 0 && backend.filePath().isEmpty(),
                "encrypted PDF with absent or incorrect password fails without retained document state");
  }
  ok &= check(backend.load(encrypted, PdfBoundaryFixtures::password()) &&
                  backend.pageCount() == 1 &&
                  near(backend.renderPage(0, {320, 180}), {160, 90}, {230, 32, 32}),
              "backend explicitly supplied correct password renders the authored encrypted page");
  ok &= check(!backend.load(encrypted) &&
                  backend.error() == PdfDocumentError::PasswordRequired,
              "a previous successful password is not retained for a later load");
  ok &= check(backend.load(plainPath) && backend.error() == PdfDocumentError::None &&
                  backend.errorDetail().isEmpty(),
              "unencrypted PDF recovers after password failures");
  QString error;
  ok &= check(generatePdfThumbnail(encrypted, {160, 90}, &error).isNull() &&
                  !error.isEmpty() && !validatePdfSource(encrypted).isEmpty(),
              "thumbnail and native validation reject password-protected input with a reason");
  ok &= check(!generatePdfThumbnail(plainPath, {160, 90}, &error).isNull() &&
                  error.isEmpty() && validatePdfSource(plainPath).isEmpty(),
              "thumbnail and validation recover with ordinary input");
  return ok;
}

bool pageCountChecks(const QString &directory, const QString &plainPath) {
  const QString atLimit = QDir(directory).filePath(QStringLiteral("1000-pages.pdf"));
  const QString overLimit = QDir(directory).filePath(QStringLiteral("1001-pages.pdf"));
  const QString zeroPages = QDir(directory).filePath(QStringLiteral("zero-pages.pdf"));
  if (!check(PdfBoundaryFixtures::writePdf(atLimit, 1000) &&
                 PdfBoundaryFixtures::writePdf(overLimit, 1001) &&
                 PdfBoundaryFixtures::writePdf(zeroPages, 0),
             "authored fixtures cover zero, exactly 1000, and 1001 pages")) return false;
  PdfDocumentBackend backend;
  bool ok = check(backend.load(atLimit) && backend.pageCount() == 1000,
                  "exactly 1000 PDF pages are accepted");
  ok &= check(near(backend.renderPage(0, {160, 90}), {80, 45}, {230, 32, 32}) &&
                  near(backend.renderPage(999, {160, 90}), {80, 45}, {32, 80, 230}),
              "first and last pages at the page limit render with the expected content");
  for (const int page : {-1, 1000}) {
    ok &= check(backend.renderPage(page, {160, 90}).isNull() &&
                    backend.error() == PdfDocumentError::InvalidPage,
                "negative and one-past-last page indices are rejected");
  }
  ok &= check(!backend.renderPage(999, {160, 90}).isNull() &&
                  backend.error() == PdfDocumentError::None,
              "valid rendering recovers after an invalid page index");
  ok &= check(!backend.load(overLimit) &&
                  backend.error() == PdfDocumentError::TooManyPages &&
                  !backend.errorDetail().isEmpty() && !backend.isLoaded() &&
                  backend.pageCount() == 0 && backend.filePath().isEmpty(),
              "1001-page rejection clears the previously loaded document");
  // PDFium can reject an empty page tree before Qt reaches Ready; both
  // parser rejection and the backend's explicit empty-document branch apply.
  ok &= check(!backend.load(zeroPages) &&
                  (backend.error() == PdfDocumentError::InvalidFile ||
                   backend.error() == PdfDocumentError::LoadFailed) &&
                  !backend.errorDetail().isEmpty() && backend.filePath().isEmpty() &&
                  !backend.isLoaded() && backend.pageCount() == 0,
              "a PDF page tree containing no pages is rejected");
  ok &= check(backend.load(plainPath) && !backend.renderPage(0, {160, 90}).isNull(),
              "ordinary PDF reload succeeds after page-count failures");
  return ok;
}

bool permissionsChecks(const QString &directory, const QString &plainPath) {
  const QString unreadable = QDir(directory).filePath(QStringLiteral("unreadable-source.pdf"));
  if (!check(QFile::copy(plainPath, unreadable), "unreadable-source fixture is isolated"))
    return false;
  FileModeRestore modes(unreadable);
  if (!check(modes.set(0000), "source permissions can be removed for the test")) return false;
  QFile directRead(unreadable);
  if (!check(!directRead.open(QIODevice::ReadOnly),
             "OS actually denies reading the chmod-000 PDF")) return false;
  PdfDocumentBackend backend;
  bool ok = check(backend.load(plainPath), "a document is loaded before permission failure");
  ok &= check(!backend.load(unreadable) && backend.error() == PdfDocumentError::LoadFailed &&
                  !backend.errorDetail().isEmpty() && !backend.isLoaded() &&
                  backend.pageCount() == 0 && backend.filePath().isEmpty(),
              "an existing unreadable PDF reports load failure rather than missing-file status");
  QString error;
  ok &= check(generatePdfThumbnail(unreadable, {160, 90}, &error).isNull() &&
                  !error.isEmpty() && !validatePdfSource(unreadable).isEmpty(),
              "unreadable source is rejected by thumbnail and native validation");
  ok &= check(modes.restore() && backend.load(unreadable) &&
                  !backend.renderPage(0, {160, 90}).isNull() &&
                  backend.error() == PdfDocumentError::None,
              "restored source permissions allow the same path to load and render");
  return ok;
}

bool renderingChecks(const QString &directory) {
  const QString complex = QDir(directory).filePath(QStringLiteral("vectors-alpha-rotation.pdf"));
  const QString huge = QDir(directory).filePath(QStringLiteral("huge-page-box.pdf"));
  const QString large = QDir(directory).filePath(QStringLiteral("64MiB-referenced-image.pdf"));
  if (!check(PdfBoundaryFixtures::writePdf(complex, 2, 0, true) &&
                 PdfBoundaryFixtures::writePdf(huge, 1, 0, false, true) &&
                 PdfBoundaryFixtures::writePdf(large, 1, 64LL * 1024 * 1024),
             "complex, huge-page-box, and 64 MiB image PDFs are authored locally")) return false;
  PdfDocumentBackend backend;
  bool ok = check(backend.load(complex), "vector/transparency/rotation PDF loads");
  const QImage vectorPage = backend.renderPage(0, {640, 360});
  ok &= check(vectorPage.size() == QSize(640, 360) &&
                  near(vectorPage, {320, 180}, {115, 16, 143}) &&
                  near(vectorPage, {600, 180}, {230, 32, 32}),
              "2000 vector curves and alpha compositing render with opaque expected colors");
  const auto rotatedSize = backend.pagePointSize(1);
  const QImage rotated = backend.renderPage(1, {320, 180});
  ok &= check(rotatedSize.height() > rotatedSize.width() &&
                  rotated.size() == QSize(320, 180) &&
                  near(rotated, {5, 90}, Qt::black) &&
                  near(rotated, {160, 90}, {16, 40, 243}),
              "rotated portrait page fits the canvas with black borders and correct transparency");
  ok &= check(backend.load(huge), "huge page box loads without using its dimensions as pixels");
  const QImage bounded = backend.renderPage(0, {160, 90});
  ok &= check(bounded.size() == QSize(160, 90) && bounded.sizeInBytes() == 160 * 90 * 4 &&
                  near(bounded, {80, 45}, {230, 32, 32}) && near(bounded, {5, 45}, Qt::black),
              "million-point square page renders into a bounded 160x90 image");
  for (const QSize canvas : {QSize(0, 90), QSize(-1, 90), QSize(160, 0),
                             QSize(1921, 1080), QSize(1920, 1081)}) {
    ok &= check(backend.renderPage(0, canvas).isNull() &&
                    backend.error() == PdfDocumentError::RenderFailed,
                "zero, negative, and one-pixel-over-limit canvas sizes are rejected");
  }
  QElapsedTimer elapsed;
  elapsed.start();
  ok &= check(QFileInfo(large).size() >= 64LL * 1024 * 1024 && backend.load(large) &&
                  backend.pageCount() == 1,
              "64 MiB referenced uncompressed image PDF is accepted");
  const QImage imagePage = backend.renderPage(0, {320, 180});
  ok &= check(imagePage.size() == QSize(320, 180) &&
                  near(imagePage, {160, 90}, {230, 32, 32}),
              "large PDF decodes referenced image content into the bounded output canvas");
  qInfo() << "PDF 64 MiB load/render observation:" << elapsed.elapsed() << "ms; file bytes:"
          << QFileInfo(large).size();
  backend.close();
  ok &= check(!backend.isLoaded() && backend.filePath().isEmpty() && backend.pageCount() == 0 &&
                  backend.error() == PdfDocumentError::None && backend.load(complex),
              "closing the large PDF clears state and allows another document to load");
  return ok;
}

bool nativeFailureRecoveryChecks(const QString &directory, const QString &plainPath) {
  PdfSourceSessionManager manager(320, 180);
  PdfSourceSessionManager::ProgramSource source;
  QString detail;
  bool ok = check(manager.bindProgramDeck(0, plainPath, source, &detail) &&
                      waitFor([&] { return manager.programSource(0).isValid(); }),
                  "ordinary native PDF source becomes ready without a display");
  const auto liveSource = manager.programSource(0);
  const auto liveState = manager.navigationState(0);
  const QStringList rejected = {
      PdfBoundaryFixtures::encryptedFixturePath(),
      QDir(directory).filePath(QStringLiteral("1001-pages.pdf")),
      QDir(directory).filePath(QStringLiteral("absent-native.pdf"))};
  for (const QString &path : rejected) {
    quint64 generation = 0;
    detail.clear();
    ok &= check(manager.bindProgramDeck(1, path, source, &detail) &&
                    waitFor([&] { return manager.sourceErrorAfter(1, 0, generation, detail); }) &&
                    !detail.isEmpty() && generation != 0 && !manager.programSource(1).isValid(),
                "native worker rejects encrypted, over-limit, or missing standby document asynchronously");
    const auto retained = manager.programSource(0);
    const auto retainedState = manager.navigationState(0);
    ok &= check(retained.pipeline == liveSource.pipeline && retained.appSink == liveSource.appSink &&
                    retainedState.currentPage == liveState.currentPage &&
                    retainedState.pageCount == liveState.pageCount,
                "failed standby leaves the other native source and page selection unchanged");
    QString duplicate;
    ok &= check(!manager.sourceErrorAfter(1, generation, generation, duplicate) &&
                    manager.releaseProgramDeck(1) && !manager.navigationState(1).active,
                "failed native session reports one error generation and releases ownership");
  }
  ok &= check(manager.bindProgramDeck(1, plainPath, source, &detail) &&
                  waitFor([&] { return manager.programSource(1).isValid(); }) &&
                  manager.navigationState(1).currentPage == 1,
              "native deck can prepare ordinary input after repeated failed documents");
  manager.requestShutdown();
  ok &= check(waitFor([&] { return manager.shutdownComplete(); }) &&
                  !manager.navigationState(0).active && !manager.navigationState(1).active,
              "native failure/recovery worker confirms shutdown and releases both decks");
  return ok;
}

} // namespace

bool runPdfBackendBoundaryChecks(const QString &directory) {
  const QString root = QDir(directory).filePath(QStringLiteral("backend-boundaries"));
  const QString plain = QDir(root).filePath(QStringLiteral("ordinary-two-pages.pdf"));
  if (!check(QDir().mkpath(root) && PdfBoundaryFixtures::writePdf(plain, 2),
             "backend boundary fixtures use an isolated temporary directory")) return false;
  bool ok = encryptedChecks(plain);
  ok = pageCountChecks(root, plain) && ok;
  ok = permissionsChecks(root, plain) && ok;
  ok = renderingChecks(root) && ok;
  ok = nativeFailureRecoveryChecks(root, plain) && ok;
  return ok;
}
