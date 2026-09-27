#include "application_shutdown_deadline.h"
#include "background_io_queue.h"
#include "playlist_document_io.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
constexpr auto kChildDeadline = std::chrono::milliseconds(500);
constexpr int kUnexpectedChildExit = 90;

[[noreturn]] void blockForever() {
  for (;;) QThread::msleep(1000);
}

int runQueueChecks() {
  bool ok = true;
  const auto check = [&ok](bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ok = false; }
  };
  QTemporaryDir directory;
  PlaylistModel model;
  const auto snapshot = model.snapshot();
  const auto path = directory.filePath("session.json");
  BackgroundIoQueue queue;
  const auto uiThread = QThread::currentThreadId();
  std::atomic_bool onWorker = false;
  int completions = 0;
  int ticks = 0;
  QTimer timer;
  timer.setInterval(10);
  QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; });
  timer.start();
  check(queue.enqueue([&onWorker, uiThread, path, snapshot] {
    onWorker.store(QThread::currentThreadId() != uiThread);
    QThread::msleep(150);
    QString error;
    if (!PlaylistDocumentIo::writeFile(path, snapshot, &error)) throw std::runtime_error(error.toStdString());
  }, [&](const QString &error) { check(error.isEmpty(), "save finishes successfully"); ++completions; }), "save accepted");
  model.createPlaylist(); // Must not change the already captured save.
  queue.requestShutdown();
  check(!queue.enqueue([] {}), "new work rejected during shutdown");
  QElapsedTimer elapsed;
  elapsed.start();
  while (!queue.shutdownComplete() && elapsed.elapsed() < 3000) {
    QCoreApplication::processEvents(); QThread::msleep(2);
  }
  check(queue.shutdownComplete() && completions == 1 && ticks >= 5 && onWorker.load(),
        "shutdown drains accepted save while the UI event loop continues");
  const auto saved = PlaylistDocumentIo::readForRestore(path);
  check(saved && saved->snapshot.playlists.size() == snapshot.playlists.size(), "saved snapshot is immutable");
  if (saved) check(model.restoreSnapshot(saved->snapshot, nullptr, &saved->offlineItems), "prepared restore applies on owner thread");
  std::atomic_bool cancel = true;
  check(!PlaylistDocumentIo::readForRestore(path, nullptr, &cancel), "cancelled restore does not read/apply");
  if (ok) std::cout << "save drained normally" << std::endl;
  return ok ? 0 : 1;
}

int runChild(const QString &mode) {
  if (mode == QStringLiteral("normal-save-drain")) {
    vtrpon::armApplicationShutdownDeadline(std::chrono::milliseconds(1500));
    return runQueueChecks();
  }
  if (mode == QStringLiteral("blocked-queue")) {
    BackgroundIoQueue queue;
    const auto workerStarted = std::make_shared<std::atomic_bool>(false);
    if (!queue.enqueue([workerStarted] {
          workerStarted->store(true);
          blockForever();
        })) return kUnexpectedChildExit;
    vtrpon::armApplicationShutdownDeadline(kChildDeadline);
    queue.requestShutdown();
    int incompletePolls = 0;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] {
      if (queue.shutdownComplete()) {
        QCoreApplication::exit(kUnexpectedChildExit);
      } else if (workerStarted->load() && ++incompletePolls == 3) {
        std::cout << "UI still polls incomplete queue shutdown" << std::endl;
      }
    });
    poll.start();
    QTimer::singleShot(1800, [] { QCoreApplication::exit(kUnexpectedChildExit); });
    return QCoreApplication::exec();
  }
  if (mode == QStringLiteral("blocked-main-thread")) {
    vtrpon::armApplicationShutdownDeadline(kChildDeadline);
    std::cout << "main thread blocked without an event loop" << std::endl;
    blockForever();
  }
  if (mode == QStringLiteral("repeated-arm")) {
    vtrpon::armApplicationShutdownDeadline(kChildDeadline);
    int rearms = 0;
    QTimer rearm;
    rearm.setInterval(100);
    QObject::connect(&rearm, &QTimer::timeout, [&] {
      vtrpon::armApplicationShutdownDeadline(kChildDeadline);
      if (++rearms == 3) std::cout << "shutdown deadline rearmed three times" << std::endl;
    });
    rearm.start();
    QTimer::singleShot(1800, [] { QCoreApplication::exit(kUnexpectedChildExit); });
    return QCoreApplication::exec();
  }
  if (mode == QStringLiteral("blocked-destructor")) {
    struct StuckDestructor {
      ~StuckDestructor() {
        std::cout << "destructor entered and blocked" << std::endl;
        std::thread worker(blockForever);
        worker.join();
      }
    };
    vtrpon::armApplicationShutdownDeadline(kChildDeadline);
    { StuckDestructor stuck; }
    return kUnexpectedChildExit;
  }
  std::cerr << "Unknown child mode: " << mode.toStdString() << '\n';
  return kUnexpectedChildExit;
}

bool checkChild(const char *mode, int expectedExitCode, const char *requiredOutput) {
  QProcess child;
  QElapsedTimer elapsed;
  elapsed.start();
  child.start(QCoreApplication::applicationFilePath(),
              {QStringLiteral("--shutdown-deadline-child"), QString::fromLatin1(mode)});
  const bool started = child.waitForStarted(1000);
  const bool finished = started && child.waitForFinished(2500);
  if (!finished && child.state() != QProcess::NotRunning) {
    child.kill();
    child.waitForFinished(1000);
  }
  const auto output = child.readAllStandardOutput();
  const auto errors = child.readAllStandardError();
  if (!finished || child.exitStatus() != QProcess::NormalExit ||
      child.exitCode() != expectedExitCode || !output.contains(requiredOutput)) {
    std::cerr << "FAIL: " << mode << " (started=" << started << ", finished=" << finished
              << ", exit status=" << static_cast<int>(child.exitStatus())
              << ", exit code=" << child.exitCode() << ", elapsed=" << elapsed.elapsed()
              << " ms, process error=" << child.errorString().toStdString() << ")\n"
              << "stdout: " << output.toStdString() << "\nstderr: " << errors.toStdString() << '\n';
    return false;
  }
  std::cout << "PASS: " << mode << '\n';
  return true;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto arguments = app.arguments();
  if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--shutdown-deadline-child"))
    return runChild(arguments.at(2));

  // Stop after a failed child so even a missing watchdog stays within the CTest timeout.
  if (!checkChild("normal-save-drain", 0, "save drained normally")) return 1;
  if (!checkChild("blocked-queue", vtrpon::kApplicationShutdownTimeoutExitCode,
                  "UI still polls incomplete queue shutdown")) return 1;
  if (!checkChild("blocked-main-thread", vtrpon::kApplicationShutdownTimeoutExitCode,
                  "main thread blocked without an event loop")) return 1;
  if (!checkChild("repeated-arm", vtrpon::kApplicationShutdownTimeoutExitCode,
                  "shutdown deadline rearmed three times")) return 1;
  if (!checkChild("blocked-destructor", vtrpon::kApplicationShutdownTimeoutExitCode,
                  "destructor entered and blocked")) return 1;
  return 0;
}
