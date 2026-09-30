#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>

typedef struct _GstContext GstContext;
typedef struct _GstElement GstElement;

enum class PowerPointCommandResult { Completed, Busy, Boundary, Failed };

struct PowerPointNavigationState {
  bool active = false;
  bool busy = false;
  bool canGoPrevious = false;
  bool canGoNext = false;
  int currentSlide = 0;
  int slideCount = 0;
  int clickIndex = 0;
  int clickCount = 0;
  QString errorDetail;
};

struct PowerPointCommandResponse {
  PowerPointCommandResult result = PowerPointCommandResult::Failed;
  QString errorDetail;
};

struct PowerPointSlideExportResult {
  bool succeeded = false;
  bool canceled = false;
  QStringList slidePaths;
  QString errorDetail;
};

using PowerPointSlideExportProgress =
    std::function<void(int completedSlides, int totalSlides)>;

class PowerPointSourceSessionManager final {
public:
  struct ProgramSource {
    GstElement *pipeline = nullptr;
    GstElement *appSink = nullptr;

    bool isValid() const { return pipeline && appSink; }
  };

  PowerPointSourceSessionManager(int outputWidth, int outputHeight);
  ~PowerPointSourceSessionManager();

  PowerPointSourceSessionManager(const PowerPointSourceSessionManager &) =
      delete;
  PowerPointSourceSessionManager &
  operator=(const PowerPointSourceSessionManager &) = delete;

  void setProgramContext(GstContext *context);
  // Starts Office on the STA without opening a slide show or playing media.
  // The application stays alive for subsequent CUE requests until shutdown.
  void requestApplicationWarmup();
  bool applicationIsWarm() const;
  // Queues input and Office preparation on the STA; poll programSource().
  bool bindProgramDeck(int deckIndex, const QString &filePath,
                       ProgramSource &source,
                       QString *errorDetail = nullptr);
  ProgramSource programSource(int deckIndex) const;
  void requestShutdown();
  bool shutdownComplete() const;
  // Detaches immediately. The STA retains the source until cleanup completes.
  bool releaseProgramDeck(int deckIndex);
  bool sourceIsPlaying(int deckIndex) const;
  // Nonblocking snapshot; true after the worker confirms a valid first slide.
  bool sourceIsReady(int deckIndex);
  bool sourceErrorAfter(int deckIndex, quint64 previousGeneration,
                        quint64 &currentGeneration,
                        QString &errorDetail);
  PowerPointNavigationState navigationState(int deckIndex);
  // Busy while queued/running; retry the same command to collect its result.
  PowerPointCommandResponse next(int deckIndex);
  PowerPointCommandResponse previous(int deckIndex);
  void cancelNavigation(int deckIndex);
  bool shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

QImage generatePowerPointThumbnail(const QString &filePath,
                                   const QSize &targetSize,
                                   QString *errorDetail = nullptr);

bool isPowerPointNativeSourceActive();
// COM may dispatch native window messages while an interop operation holds
// its lock. A close handler must defer teardown until that operation returns.
bool isPowerPointInteropInProgressOnCurrentThread();
bool isPowerPointNativeSourceActiveError(const QString &errorDetail);

QString validatePowerPointSource(const QString &filePath);
bool powerPointComRegistered();

PowerPointSlideExportResult exportPowerPointSlides(
    const QString &filePath, const QString &outputDirectory,
    const std::atomic_bool *cancelRequested = nullptr,
    const PowerPointSlideExportProgress &progress = {});
