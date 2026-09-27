#include "pdf_document_backend.h"

#include <QFileInfo>
#include <QPainter>
#include <QPdfDocument>

#include <algorithm>
#include <cmath>

namespace {

struct MappedPdfError {
  PdfDocumentError error = PdfDocumentError::LoadFailed;
  QString detail;
};

MappedPdfError mapLoadError(QPdfDocument::Error error) {
  switch (error) {
  case QPdfDocument::Error::FileNotFound:
    return {PdfDocumentError::MissingFile,
            QStringLiteral("The PDF file could not be found.")};
  case QPdfDocument::Error::InvalidFileFormat:
    return {PdfDocumentError::InvalidFile,
            QStringLiteral("The selected file is not a valid PDF document.")};
  case QPdfDocument::Error::IncorrectPassword:
    return {PdfDocumentError::PasswordRequired,
            QStringLiteral(
                "The PDF document requires the correct password.")};
  case QPdfDocument::Error::UnsupportedSecurityScheme:
    return {PdfDocumentError::UnsupportedSecurity,
            QStringLiteral(
                "The PDF document uses an unsupported security scheme.")};
  case QPdfDocument::Error::DataNotYetAvailable:
    return {PdfDocumentError::LoadFailed,
            QStringLiteral("The PDF document did not finish loading.")};
  case QPdfDocument::Error::Unknown:
    return {PdfDocumentError::LoadFailed,
            QStringLiteral("The PDF document could not be loaded.")};
  case QPdfDocument::Error::None:
    break;
  }
  return {PdfDocumentError::LoadFailed,
          QStringLiteral("The PDF document could not be loaded.")};
}

bool validPageIndex(const QPdfDocument &document, int pageIndex) {
  return document.status() == QPdfDocument::Status::Ready && pageIndex >= 0 &&
         pageIndex < document.pageCount();
}

QSize containedRenderSize(const QSizeF &pageSize, const QSize &canvasSize) {
  if (!pageSize.isValid() || pageSize.isEmpty() || !canvasSize.isValid() ||
      canvasSize.isEmpty() || !std::isfinite(pageSize.width()) ||
      !std::isfinite(pageSize.height())) {
    return {};
  }
  const qreal scale =
      (std::min)(static_cast<qreal>(canvasSize.width()) / pageSize.width(),
                 static_cast<qreal>(canvasSize.height()) / pageSize.height());
  if (!std::isfinite(scale) || scale <= 0.0) {
    return {};
  }
  return {
      (std::max)(1, static_cast<int>(std::floor(pageSize.width() * scale))),
      (std::max)(1, static_cast<int>(std::floor(pageSize.height() * scale)))};
}

} // namespace

PdfDocumentBackend::PdfDocumentBackend()
    : document_(std::make_unique<QPdfDocument>(nullptr)) {}

PdfDocumentBackend::~PdfDocumentBackend() = default;

bool PdfDocumentBackend::load(const QString &filePath,
                              const QString &password) {
  close();
  const QFileInfo sourceInfo(filePath);
  if (!sourceInfo.exists() || !sourceInfo.isFile()) {
    setError(PdfDocumentError::MissingFile,
             QStringLiteral("The PDF file could not be found."));
    return false;
  }

  document_->setPassword(password);
  const QPdfDocument::Error loadError =
      document_->load(sourceInfo.absoluteFilePath());
  if (loadError != QPdfDocument::Error::None) {
    const MappedPdfError mapped = mapLoadError(loadError);
    document_->close();
    setError(mapped.error, mapped.detail);
    return false;
  }
  if (document_->status() != QPdfDocument::Status::Ready) {
    const MappedPdfError mapped = mapLoadError(document_->error());
    document_->close();
    setError(mapped.error, mapped.detail);
    return false;
  }
  if (document_->pageCount() <= 0) {
    document_->close();
    setError(PdfDocumentError::InvalidFile,
             QStringLiteral("The PDF document contains no pages."));
    return false;
  }
  if (document_->pageCount() > kMaximumPageCount) {
    document_->close();
    setError(PdfDocumentError::TooManyPages,
             QStringLiteral("The PDF document exceeds the 1000-page limit."));
    return false;
  }

  filePath_ = sourceInfo.absoluteFilePath();
  setError(PdfDocumentError::None, {});
  return true;
}

void PdfDocumentBackend::close() {
  document_.reset();
  document_ = std::make_unique<QPdfDocument>(nullptr);
  filePath_.clear();
  setError(PdfDocumentError::None, {});
}

bool PdfDocumentBackend::isLoaded() const {
  return document_ && document_->status() == QPdfDocument::Status::Ready &&
         document_->pageCount() > 0 &&
         document_->pageCount() <= kMaximumPageCount;
}

QString PdfDocumentBackend::filePath() const { return filePath_; }

int PdfDocumentBackend::pageCount() const {
  return isLoaded() ? document_->pageCount() : 0;
}

QSizeF PdfDocumentBackend::pagePointSize(int pageIndex) const {
  return document_ && validPageIndex(*document_, pageIndex)
             ? document_->pagePointSize(pageIndex)
             : QSizeF{};
}

QString PdfDocumentBackend::pageLabel(int pageIndex) const {
  return document_ && validPageIndex(*document_, pageIndex)
             ? document_->pageLabel(pageIndex)
             : QString{};
}

QImage PdfDocumentBackend::renderPage(int pageIndex,
                                      const QSize &canvasSize) {
  if (!document_ || !validPageIndex(*document_, pageIndex)) {
    setError(PdfDocumentError::InvalidPage,
             QStringLiteral("The requested PDF page does not exist."));
    return {};
  }
  if (!canvasSize.isValid() || canvasSize.isEmpty() ||
      canvasSize.width() > kDefaultCanvasWidth ||
      canvasSize.height() > kDefaultCanvasHeight) {
    setError(PdfDocumentError::RenderFailed,
             QStringLiteral(
                 "The PDF render canvas must not exceed 1920x1080."));
    return {};
  }

  const QSize renderSize =
      containedRenderSize(document_->pagePointSize(pageIndex), canvasSize);
  if (!renderSize.isValid() || renderSize.isEmpty()) {
    setError(PdfDocumentError::RenderFailed,
             QStringLiteral("The PDF page size is invalid."));
    return {};
  }

  const QImage renderedPage = document_->render(pageIndex, renderSize);
  if (renderedPage.isNull() || renderedPage.size() != renderSize) {
    setError(PdfDocumentError::RenderFailed,
             QStringLiteral("The PDF page could not be rendered."));
    return {};
  }

  QImage canvas(canvasSize, QImage::Format_ARGB32);
  if (canvas.isNull()) {
    setError(PdfDocumentError::RenderFailed,
             QStringLiteral("The PDF render canvas could not be allocated."));
    return {};
  }
  canvas.fill(Qt::black);
  {
    QPainter painter(&canvas);
    if (!painter.isActive()) {
      setError(PdfDocumentError::RenderFailed,
               QStringLiteral("The PDF render canvas could not be painted."));
      return {};
    }
    const QPoint topLeft((canvas.width() - renderedPage.width()) / 2,
                         (canvas.height() - renderedPage.height()) / 2);
    painter.fillRect(QRect(topLeft, renderedPage.size()), Qt::white);
    painter.drawImage(topLeft, renderedPage);
  }
  setError(PdfDocumentError::None, {});
  return canvas;
}

PdfDocumentError PdfDocumentBackend::error() const { return error_; }

QString PdfDocumentBackend::errorDetail() const { return errorDetail_; }

void PdfDocumentBackend::setError(PdfDocumentError error,
                                  const QString &detail) {
  error_ = error;
  errorDetail_ = detail;
}
