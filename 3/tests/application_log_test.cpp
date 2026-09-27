#include "application_log.h"
#include "controller_operations.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>
#include <gst/gst.h>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (app.arguments().contains("--pre-runtime-log")) {
    QTemporaryDir earlyDirectory;
    if (!earlyDirectory.isValid())
      return 1;
    vtrpon::startApplicationLog(earlyDirectory.path());
    qWarning("Log before runtime setup");
    vtrpon::stopApplicationLog();
#ifdef _WIN32
    // Match the application's delay-load boundary. This must work even before
    // its GStreamer DLL directory is configured, including the error exit path.
    if (GetModuleHandleW(L"glib-2.0-0.dll") ||
        GetModuleHandleW(L"gstreamer-1.0-0.dll"))
      return 1;
#endif
    return 0;
  }
  gst_init(nullptr, nullptr);
  QTemporaryDir directory;
  if (!directory.isValid())
    return 1;
  bool ok = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s\n", message);
      ok = false;
    }
  };
  QProcess earlyLogProcess;
  earlyLogProcess.start(QCoreApplication::applicationFilePath(),
                        {"--pre-runtime-log"});
  const bool earlyLogFinished = earlyLogProcess.waitForFinished(10000);
  check(earlyLogFinished &&
            earlyLogProcess.exitStatus() == QProcess::NormalExit &&
            earlyLogProcess.exitCode() == 0,
        "Logging before runtime initialization loaded GStreamer or failed");
  if (!earlyLogFinished) {
    earlyLogProcess.kill();
    earlyLogProcess.waitForFinished();
  }
  vtrpon::startApplicationLog(directory.path());
  vtrpon::attachGStreamerLog();
  qDebug("hidden-debug-marker");
  qWarning().noquote() << QString::fromUtf8("日本語の警告");
  std::vector<std::thread> producers;
  for (int i = 0; i < 4; ++i)
    producers.emplace_back([i] {
      for (int j = 0; j < 100; ++j)
        qInfo() << "worker-record" << i << j;
    });
  for (auto &producer : producers)
    producer.join();
  {
    ControllerOperations operations;
    operations.setLogContextProvider(
        [] { return QStringLiteral("selected=42"); });
    operations.bind<int>(ControllerOperations::Action::SetItemVolume,
                         &operations, [](int) {});
    operations.bind<>(ControllerOperations::Action::ProgramPlay, &operations,
                      [] {});
    operations.executeFrom("Shortcut",
                           ControllerOperations::Action::ProgramPlay);
    operations.execute(ControllerOperations::Action::ProgramPlay);
    for (int value = 0; value < 20; ++value)
      operations.execute(ControllerOperations::Action::SetItemVolume, value);
    QEventLoop loop;
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
    const auto records = vtrpon::applicationLogSnapshot().join('\n');
    check(records.contains("source=Shortcut") && records.contains("source=UI"),
          "Operation source missing or leaked");
    check(records.contains("selected=42"), "Operation target missing");
    check(records.count("action=SetItemVolume") == 1 &&
              records.contains("(int, 19)"),
          "Continuous operation was not collapsed to final value");
  }
  QWidget window;
  auto *menu = new QMenuBar(&window);
  vtrpon::installToolsMenu(menu, &window, nullptr);
  auto *show = menu->findChild<QAction *>("showLogAction");
  auto *debug = menu->findChild<QAction *>("debugModeAction");
  check(show && debug, "Tools actions missing");
  if (show && debug) {
    debug->setChecked(true);
    check(vtrpon::diagnosticLogging(), "Debug did not enable");
    qDebug("visible-debug-marker");
    show->trigger();
    auto *viewer = window.findChild<QWidget *>("logViewer");
    auto *text = viewer->findChild<QPlainTextEdit *>("logText");
    check(text->toPlainText().contains("visible-debug-marker"),
          "Viewer missing debug record");
    check(!text->toPlainText().contains("hidden-debug-marker"),
          "Debug leaked while disabled");
    auto *filter = viewer->findChild<QComboBox *>("logLevelFilter");
    filter->setCurrentIndex(1);
    check(text->toPlainText().contains(QString::fromUtf8("日本語の警告")),
          "Unicode warning missing");
    check(!text->toPlainText().contains("worker-record"),
          "Severity filter failed");
    viewer->findChild<QCheckBox *>()->setChecked(false);
    qWarning("after-pause-marker");
    viewer->findChild<QLineEdit *>("logSearch")->setText("after-pause-marker");
    check(text->toPlainText().isEmpty(), "Paused view unexpectedly advanced");
    viewer->close();
    check(!viewer->isVisible(), "Close did not hide viewer");
    qInfo("after-close-marker");
    debug->setChecked(false);
    check(!vtrpon::diagnosticLogging(), "Debug did not disable");
  }
  vtrpon::stopApplicationLog();
  QByteArray saved;
  for (const auto &name :
       QDir(directory.path()).entryList({"*.jsonl"}, QDir::Files)) {
    QFile file(QDir(directory.path()).filePath(name));
    check(file.open(QIODevice::ReadOnly), "Cannot read saved log");
    saved += file.readAll();
  }
  int workerCount = 0;
  for (const auto &line : saved.split('\n')) {
    if (line.isEmpty())
      continue;
    QJsonParseError error;
    const auto record = QJsonDocument::fromJson(line, &error).object();
    check(error.error == QJsonParseError::NoError && record.contains("time"),
          "Invalid JSONL record");
    if (record["message"].toString().contains("worker-record"))
      ++workerCount;
  }
  check(workerCount == 400, "Concurrent records lost");
  check(saved.contains("after-close-marker") &&
            saved.contains("Shutdown complete"),
        "Close/shutdown persistence failed");
  check(!saved.contains("hidden-debug-marker") &&
            saved.contains("visible-debug-marker"),
        "Debug persistence filter failed");

  // Rotation and bounded in-memory history under a sustained burst.
  for (int i = 0; i < 12; ++i) {
    QFile old(
        QDir(directory.path()).filePath(QString("vtrpon-old-%1.jsonl").arg(i)));
    check(old.open(QIODevice::WriteOnly), "Cannot create retention fixture");
    old.write("old log\n");
    old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-1),
                    QFileDevice::FileModificationTime);
  }
  QFile unrelated(QDir(directory.path()).filePath("keep.txt"));
  check(unrelated.open(QIODevice::WriteOnly), "Cannot create unrelated file");
  unrelated.close();
  vtrpon::startApplicationLog(directory.path());
  for (int i = 0; i < 1500; ++i)
    vtrpon::writeApplicationLog("INFO", "test", QString(8192, 'x'));
  check(vtrpon::applicationLogSnapshot().size() <= 2000,
        "History is unbounded");
  QElapsedTimer shutdownElapsed;
  shutdownElapsed.start();
  vtrpon::requestApplicationLogShutdown();
  vtrpon::requestApplicationLogShutdown(); // Idempotent while the writer drains.
  check(shutdownElapsed.elapsed() < 100, "Log shutdown request blocked the UI");
  while (!vtrpon::applicationLogShutdownComplete() && shutdownElapsed.elapsed() < 5000) {
    QEventLoop tick;
    QTimer::singleShot(1, &tick, &QEventLoop::quit);
    tick.exec();
  }
  check(vtrpon::applicationLogShutdownComplete(), "Log writer did not finish asynchronously");
  vtrpon::stopApplicationLog();
  const auto files =
      QDir(directory.path()).entryInfoList({"*.jsonl"}, QDir::Files);
  check(files.size() >= 3 && files.size() <= 10, "Rotation did not occur");
  check(QFile::exists(unrelated.fileName()),
        "Retention removed an unrelated file");
  for (const auto &file : files)
    check(file.size() <= 10 * 1024 * 1024, "Log exceeded size limit");

  QFile blocker(QDir(directory.path()).filePath("not-a-directory"));
  check(blocker.open(QIODevice::WriteOnly), "Cannot prepare failure test");
  blocker.close();
  vtrpon::startApplicationLog(blocker.fileName());
  qWarning("failure-path-marker");
  vtrpon::stopApplicationLog();
  check(!vtrpon::applicationLogError().isEmpty(),
        "Save failure was not surfaced");
  return ok ? 0 : 1;
}
