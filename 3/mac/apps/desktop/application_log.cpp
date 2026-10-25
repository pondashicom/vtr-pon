#include "controller_settings.h"
#include "application_log.h"
#include "product_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>
#include <QtLogging>
#include <gst/gst.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#endif

namespace {
constexpr size_t kQueueLimit = 4096;
constexpr qsizetype kHistoryLimit = 2000;
constexpr qint64 kFileLimit = 10 * 1024 * 1024;
struct LogState {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<QByteArray> pending;
  QStringList history;
  QString directory, session, error;
  std::thread writer;
  std::atomic_bool writerFinished = true;
  bool shutdownRequested = false;
  bool running = false, stopping = false, gstAttached = false,
       defaultGstRemoved = false;
  quint64 dropped = 0;
  std::atomic_bool debug{false};
  QtMessageHandler previousQt = nullptr;
  GPrintFunc previousPrint = nullptr, previousError = nullptr;
};
LogState &state() {
  // Callbacks from shutdown paths must never access a destroyed static object.
  static auto *value = new LogState;
  return *value;
}
void setError(const QString &error) {
  std::lock_guard lock(state().mutex);
  state().error = error;
}
void writerLoop() {
  auto &s = state();
  QFile file;
  int part = 0;
  const auto openPart = [&] {
    file.close();
    QDir dir(s.directory);
    if (!dir.mkpath(".")) {
      setError(
          QStringLiteral("Cannot create log directory: %1").arg(s.directory));
      return false;
    }
    file.setFileName(dir.filePath(
        s.session +
        QStringLiteral("-%1.jsonl").arg(part++, 4, 10, QLatin1Char('0'))));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      setError(file.errorString());
      return false;
    }
    // Only our own log filename pattern is eligible for retention cleanup.
    const auto entries = dir.entryInfoList({QStringLiteral("vtrpon-*.jsonl")},
                                           QDir::Files, QDir::Time);
    for (qsizetype i = 10; i < entries.size(); ++i)
      QFile::remove(entries[i].absoluteFilePath());
    return true;
  };
  bool writable = openPart();
  for (;;) {
    std::deque<QByteArray> batch;
    bool stopping;
    {
      std::unique_lock lock(s.mutex);
      s.ready.wait_for(lock, std::chrono::milliseconds(200),
                       [&] { return s.stopping || !s.pending.empty(); });
      batch.swap(s.pending);
      stopping = s.stopping;
    }
    for (const auto &line : batch) {
      if (writable && file.size() + line.size() > kFileLimit)
        writable = openPart();
      if (writable && file.write(line) != line.size()) {
        setError(file.errorString());
        writable = false;
      }
    }
    if (writable && !batch.empty() && !file.flush()) {
      setError(file.errorString());
      writable = false;
    }
    if (stopping)
      break;
  }
  file.close();
  s.writerFinished.store(true, std::memory_order_release);
}
void qtMessage(QtMsgType type, const QMessageLogContext &context,
               const QString &message) {
  const auto level = type == QtDebugMsg     ? "DEBUG"
                     : type == QtInfoMsg    ? "INFO"
                     : type == QtWarningMsg ? "WARN"
                                            : "ERROR";
  vtrpon::writeApplicationLog(
      QString::fromLatin1(level),
      QString::fromUtf8(context.category ? context.category : "Qt"), message);
}
void glibPrint(const gchar *text) {
  vtrpon::writeApplicationLog("INFO", "GLib", QString::fromUtf8(text));
}
void glibError(const gchar *text) {
  vtrpon::writeApplicationLog("WARN", "GLib", QString::fromUtf8(text));
}
void gstMessage(GstDebugCategory *category, GstDebugLevel level,
                const gchar *file, const gchar *, gint line, GObject *,
                GstDebugMessage *message, gpointer) {
  if (level > GST_LEVEL_WARNING && !state().debug.load())
    return;
  vtrpon::writeApplicationLog(
      level <= GST_LEVEL_ERROR     ? "ERROR"
      : level <= GST_LEVEL_WARNING ? "WARN"
                                   : "DEBUG",
      QStringLiteral("GST/%1").arg(
          QString::fromUtf8(gst_debug_category_get_name(category))),
      QStringLiteral("%1 (%2:%3)")
          .arg(QString::fromUtf8(gst_debug_message_get(message)),
               QString::fromUtf8(file ? file : ""))
          .arg(line));
}
} // namespace

namespace vtrpon {
void writeApplicationLog(const QString &level, const QString &source,
                         const QString &message) {
  auto &s = state();
  if (level == "DEBUG" && !s.debug.load())
    return;
  const QJsonObject record{
      {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"level", level},
      {"source", source},
      {"pid", QCoreApplication::applicationPid()},
      {"thread", QString::number(
                     std::hash<std::thread::id>{}(std::this_thread::get_id()))},
      {"message", message.left(8192)}};
  const QByteArray line =
      QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
  std::lock_guard lock(s.mutex);
  if (!s.running || s.stopping)
    return;
  if (s.pending.size() >= kQueueLimit) {
    ++s.dropped;
    return;
  }
  if (s.dropped) {
    const auto notice =
        QJsonDocument(
            QJsonObject{
                {"time", record["time"]},
                {"level", "WARN"},
                {"source", "LOGGER"},
                {"message",
                 QStringLiteral(
                     "Dropped %1 records because the log queue was full")
                     .arg(s.dropped)}})
            .toJson(QJsonDocument::Compact);
    s.pending.push_back(notice + '\n');
    s.history.append(QString::fromUtf8(notice));
    s.dropped = 0;
  }
  s.pending.push_back(line);
  s.history.append(QString::fromUtf8(line).trimmed());
  while (s.history.size() > kHistoryLimit)
    s.history.removeFirst();
  s.ready.notify_one();
}
void startApplicationLog(const QString &directory) {
  auto &s = state();
  s.directory = directory.isEmpty()
                    ? QDir(vtrpon::productLocalDataDirectory())
                          .filePath("logs")
                    : directory;
  s.session = "vtrpon-" +
              QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") +
              "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  s.stopping = false;
  s.gstAttached = false;
  s.defaultGstRemoved = false;
  s.debug.store(false);
  s.history.clear();
  s.pending.clear();
  s.error.clear();
  s.dropped = 0;
  s.running = true;
  s.shutdownRequested = false;
  s.writerFinished.store(false);
  try {
    s.writer = std::thread(writerLoop);
  } catch (const std::system_error &error) {
    s.running = false;
    s.writerFinished.store(true, std::memory_order_release);
    qWarning() << "Could not start application log writer:" << error.what();
    return;
  }
  s.previousQt = qInstallMessageHandler(qtMessage);
  writeApplicationLog(
      "INFO", "SYSTEM",
      QStringLiteral(
          "Started VTR-PON; Qt %1; OS %2; executable %3; build %4 %5")
          .arg(qVersion(), QSysInfo::prettyProductName(),
               QCoreApplication::applicationFilePath(),
               QStringLiteral(__DATE__), QStringLiteral(__TIME__)));
  logStartupSnapshot(QStringLiteral("before-settings-initialization"));
}

void logStartupSnapshot(const QString &phase) {
  const auto record = [&](QJsonObject value) {
    value.insert(QStringLiteral("phase"), phase);
    writeApplicationLog(QStringLiteral("INFO"), QStringLiteral("STARTUP"),
                        QString::fromUtf8(QJsonDocument(value).toJson(
                            QJsonDocument::Compact)));
  };
  if (phase == QLatin1StringView("before-settings-initialization")) {
    const QString executable = QCoreApplication::applicationFilePath();
    QFile file(executable);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const bool hashed = file.open(QIODevice::ReadOnly) && hash.addData(&file);
    record({{"kind", "process"}, {"executable", executable},
            {"executableSha256", hashed ? QString::fromLatin1(hash.result().toHex()) : QString()},
            {"hashSucceeded", hashed}, {"workingDirectory", QDir::currentPath()},
            {"logDirectory", applicationLogDirectory()},
            {"appLocalData", QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)}});
  }
  QSettings settings = vtrpon::makeControllerSettings();
  record({{"kind", "settings-source"}, {"source", settings.fileName()},
          {"format", int(settings.format())}, {"scope", int(settings.scope())},
          {"fallbacksEnabled", settings.fallbacksEnabled()},
          {"portable", QCoreApplication::instance()->property("vtrpon.portableMode").toBool()}});
  QFile settingsFile(settings.fileName());
  QJsonObject fileRecord{{"kind", "settings-file"}, {"path", settings.fileName()},
                         {"exists", QFileInfo::exists(settings.fileName())}};
  const bool settingsOpened = settingsFile.open(QIODevice::ReadOnly);
  fileRecord.insert("opened", settingsOpened);
  if (settingsOpened) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const bool hashed = hash.addData(&settingsFile);
    fileRecord.insert("hashSucceeded", hashed);
    if (hashed) fileRecord.insert("sha256", QString::fromLatin1(hash.result().toHex()));
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(settingsFile.handle()));
    wchar_t finalPath[32768]{};
    const DWORD length = GetFinalPathNameByHandleW(handle, finalPath,
                                                 DWORD(std::size(finalPath)), 0);
    if (length > 0 && length < std::size(finalPath))
      fileRecord.insert("openedFilePath", QString::fromWCharArray(finalPath, int(length)));
    else fileRecord.insert("pathQueryError", int(GetLastError()));
#else
    fileRecord.insert("openedFilePath", QFileInfo(settingsFile).canonicalFilePath());
#endif
  } else {
    fileRecord.insert("error", settingsFile.errorString());
  }
  record(fileRecord);
  // One record per key avoids the logger's per-message size limit for a whole
  // settings object. This is a read-only snapshot taken before startup writes.
  for (const QString &key : settings.allKeys()) {
    const QVariant value = settings.value(key);
    record({{"kind", "settings-read"}, {"key", key},
            {"valueType", QString::fromLatin1(value.typeName() ? value.typeName() : "")},
            {"value", QJsonValue::fromVariant(value)}});
  }
  record({{"kind", "settings-read-complete"}, {"status", int(settings.status())}});
#ifdef _WIN32
  const HANDLE snapshot = CreateToolhelp32Snapshot(
      TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snapshot != INVALID_HANDLE_VALUE) {
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
      do {
        record({{"kind", "loaded-module"},
                {"name", QString::fromWCharArray(entry.szModule)},
                {"path", QString::fromWCharArray(entry.szExePath)}});
      } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
  } else {
    record({{"kind", "module-snapshot-error"}, {"error", int(GetLastError())}});
  }
#endif
}
void attachGStreamerLog() {
  auto &s = state();
  // The GUI initializes logging before configuring the delay-loaded runtime.
  // Do not call GLib until the GStreamer DLL search path has been configured.
  s.previousPrint = g_set_print_handler(glibPrint);
  s.previousError = g_set_printerr_handler(glibError);
  gst_debug_add_log_function(gstMessage, nullptr, nullptr);
  if (qEnvironmentVariableIsEmpty("GST_DEBUG_FILE")) {
    gst_debug_remove_log_function(gst_debug_log_default);
    s.defaultGstRemoved = true;
  }
  s.gstAttached = true;
  if (qgetenv("GST_DEBUG").isEmpty())
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
  gchar *version = gst_version_string();
  writeApplicationLog("INFO", "SYSTEM", QString::fromUtf8(version));
  g_free(version);
}
void setDiagnosticLogging(bool enabled) {
  state().debug.store(enabled);
  if (state().gstAttached) {
    const QByteArray thresholds =
#ifdef __APPLE__
        enabled ? QByteArray("*:2,osxaudio*:4,audiobasesrc:4,audioaggregator:5,vtrpon-audio-bridge:6")
#else
        enabled ? QByteArray("*:2,*d3d11*:4,wasapi*:4,playbin*:4")
#endif
                : qgetenv("GST_DEBUG");
    gst_debug_set_threshold_from_string(
        thresholds.isEmpty() ? "*:2" : thresholds.constData(), TRUE);
  }
  writeApplicationLog("INFO", "SYSTEM",
                      enabled ? "Debug mode enabled" : "Debug mode disabled");
}
bool diagnosticLogging() { return state().debug.load(); }
QStringList applicationLogSnapshot() {
  std::lock_guard lock(state().mutex);
  return state().history;
}
QString applicationLogDirectory() { return state().directory; }
QString applicationLogError() {
  std::lock_guard lock(state().mutex);
  return state().error;
}
void requestApplicationLogShutdown() {
  auto &s = state();
  if (s.shutdownRequested) return;
  s.shutdownRequested = true;
  writeApplicationLog("INFO", "SYSTEM", "Shutdown complete");
  if (s.gstAttached)
    gst_debug_remove_log_function(gstMessage);
  if (s.defaultGstRemoved)
    gst_debug_add_log_function(gst_debug_log_default, nullptr, nullptr);
  qInstallMessageHandler(s.previousQt);
  if (s.gstAttached) {
    g_set_print_handler(s.previousPrint);
    g_set_printerr_handler(s.previousError);
  }
  {
    std::lock_guard lock(s.mutex);
    s.stopping = true;
  }
  s.ready.notify_one();
}
bool applicationLogShutdownComplete() {
  return state().writerFinished.load(std::memory_order_acquire);
}
void stopApplicationLog() {
  requestApplicationLogShutdown();
  auto &s = state();
  if (s.writer.joinable()) s.writer.join();
  {
    std::lock_guard lock(s.mutex);
    s.running = false;
  }
}
} // namespace vtrpon
