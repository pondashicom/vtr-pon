#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <memory>

typedef struct _GstContext GstContext;
typedef struct _GstElement GstElement;

enum class PdfCommandResult { Completed, Busy, Boundary, Failed };

struct PdfNavigationState {
  bool active = false;
  bool busy = false;
  bool canGoPrevious = false;
  bool canGoNext = false;
  int currentPage = 0; // One-based while active, matching Program UI text.
  int pageCount = 0;
  QString pageLabel;
  QString errorDetail;
};

struct PdfCommandResponse {
  PdfCommandResult result = PdfCommandResult::Failed;
  QString errorDetail;
};

class PdfSourceSessionManager final {
public:
  struct ProgramSource {
    GstElement *pipeline = nullptr;
    GstElement *appSink = nullptr;

    bool isValid() const { return pipeline && appSink; }
  };

  PdfSourceSessionManager(int outputWidth, int outputHeight);
  ~PdfSourceSessionManager();

  PdfSourceSessionManager(const PdfSourceSessionManager &) = delete;
  PdfSourceSessionManager &
  operator=(const PdfSourceSessionManager &) = delete;

  void setProgramContext(GstContext *context);
  // Queues preparation; programSource() publishes the input when ready.
  bool bindProgramDeck(int deckIndex, const QString &filePath,
                       ProgramSource &source,
                       QString *errorDetail = nullptr);
  // Borrowed input references remain valid until releaseProgramDeck().
  ProgramSource programSource(int deckIndex) const;
  void requestShutdown();
  bool shutdownComplete() const;
  void suspendProgramDeck(int deckIndex);
  bool tryReleaseProgramDeck(int deckIndex);
  // Detachment is immediate; the worker retains resources until NULL is confirmed.
  bool releaseProgramDeck(int deckIndex);
  bool sourceIsPlaying(int deckIndex) const;
  bool sourceErrorAfter(int deckIndex, quint64 previousGeneration,
                        quint64 &currentGeneration,
                        QString &errorDetail);
  PdfNavigationState navigationState(int deckIndex);
  // Busy while queued/running; repeat to collect, cancelNavigation() invalidates.
  PdfCommandResponse next(int deckIndex);
  PdfCommandResponse previous(int deckIndex);
  void cancelNavigation(int deckIndex);
  bool shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

QImage generatePdfThumbnail(const QString &filePath,
                            const QSize &targetSize,
                            QString *errorDetail = nullptr);

QString validatePdfSource(const QString &filePath);
