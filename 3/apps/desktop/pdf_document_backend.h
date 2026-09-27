#pragma once

#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <memory>

class QPdfDocument;

enum class PdfDocumentError {
  None,
  MissingFile,
  InvalidFile,
  PasswordRequired,
  UnsupportedSecurity,
  TooManyPages,
  LoadFailed,
  InvalidPage,
  RenderFailed,
};

class PdfDocumentBackend final {
public:
  static constexpr int kMaximumPageCount = 1000;
  static constexpr int kDefaultCanvasWidth = 1920;
  static constexpr int kDefaultCanvasHeight = 1080;

  PdfDocumentBackend();
  ~PdfDocumentBackend();

  PdfDocumentBackend(const PdfDocumentBackend &) = delete;
  PdfDocumentBackend &operator=(const PdfDocumentBackend &) = delete;

  bool load(const QString &filePath, const QString &password = {});
  void close();

  bool isLoaded() const;
  QString filePath() const;
  int pageCount() const;
  QSizeF pagePointSize(int pageIndex) const;
  QString pageLabel(int pageIndex) const;

  QImage renderPage(
      int pageIndex,
      const QSize &canvasSize =
          QSize(kDefaultCanvasWidth, kDefaultCanvasHeight));

  PdfDocumentError error() const;
  QString errorDetail() const;

private:
  void setError(PdfDocumentError error, const QString &detail);

  std::unique_ptr<QPdfDocument> document_;
  QString filePath_;
  PdfDocumentError error_ = PdfDocumentError::None;
  QString errorDetail_;
};
