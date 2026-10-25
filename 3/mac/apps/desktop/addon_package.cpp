#include "platform_capabilities.h"
#include "addon_package.h"
#include "product_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>
#ifdef Q_OS_MACOS
#include <QLocalServer>
#include <QLocalSocket>
#endif

#include <zlib.h>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

namespace vtrpon::addons {
namespace {

thread_local QString stateOverride;

constexpr qsizetype kMaximumArchive = 300 * 1024 * 1024;
constexpr qsizetype kMaximumExpanded = 400 * 1024 * 1024;

quint16 u16(const QByteArray &data, qsizetype offset) {
  const auto *p = reinterpret_cast<const unsigned char *>(data.constData() + offset);
  return quint16(p[0] | (p[1] << 8));
}
quint32 u32(const QByteArray &data, qsizetype offset) {
  const auto *p = reinterpret_cast<const unsigned char *>(data.constData() + offset);
  return quint32(p[0]) | (quint32(p[1]) << 8) |
         (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
QString hash(const QByteArray &data) {
  return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}
QString canonicalRoot(const QString &root) {
  return QFileInfo(root).canonicalFilePath();
}
bool validVersion(const QString &version) {
  static const QRegularExpression re(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
  return re.match(version).hasMatch();
}
bool validSha(const QString &sha) {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
  return re.match(sha).hasMatch();
}
bool displayableDocument(const QByteArray &contents) {
  return !contents.isEmpty() && contents.size() <= 1024 * 1024 &&
         !contents.contains('\0') &&
         QString::fromUtf8(contents).toUtf8() == contents;
}
QJsonArray agreementDigests(const Package &package) {
  QJsonArray result;
  for (const auto &entry : package.agreements)
    result.append(QJsonObject{{QStringLiteral("path"), entry.path},
                              {QStringLiteral("sha256"), entry.sha256}});
  return result;
}
bool validConsent(const Package &package, const QJsonObject &consent) {
  if (package.manifest.value(QStringLiteral("formatVersion")).toInt(-1) != 2 ||
      package.agreements.isEmpty() || consent.size() != 5 ||
      consent.value(QStringLiteral("id")).toString() !=
          package.manifest.value(QStringLiteral("id")).toString() ||
      consent.value(QStringLiteral("version")).toString() !=
          package.manifest.value(QStringLiteral("version")).toString() ||
      consent.value(QStringLiteral("packageSha256")).toString() != package.sha256 ||
      consent.value(QStringLiteral("agreements")).toArray() != agreementDigests(package))
    return false;
  const QString when = consent.value(QStringLiteral("acceptedAtUtc")).toString();
  const QDateTime accepted = QDateTime::fromString(when, Qt::ISODateWithMs);
  return !when.isEmpty() && accepted.isValid() &&
         accepted.timeSpec() == Qt::UTC;
}
bool validPath(const QString &path) {
  if (path.isEmpty() || path.startsWith(QLatin1Char('/')) ||
      path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':')) ||
      path.contains(QChar::Null)) return false;
  const auto parts = path.split(QLatin1Char('/'));
  static const QRegularExpression device(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"),
                                         QRegularExpression::CaseInsensitiveOption);
  for (const auto &part : parts) {
    if (part.isEmpty() || part == QLatin1String(".") ||
        part == QLatin1String("..") || part.endsWith(QLatin1Char(' ')) ||
        part.endsWith(QLatin1Char('.')) || part.contains(QChar(0x7f))) return false;
    for (const auto c : part) if (c.unicode() < 32) return false;
    if (device.match(part.section(QLatin1Char('.'), 0, 0)).hasMatch()) return false;
  }
  return true;
}
bool ownedPath(const QString &id, const QString &relative) {
  const QString base = entryPath(id).section(QLatin1Char('/'), 0, 0);
  return validPath(relative) &&
         relative.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive);
}
QStringList manualPaths(const QString &id) {
  const QString entry = entryPath(id);
  if (entry.isEmpty()) return {};
  QStringList paths{entry};
  if (id != QLatin1String("vtr-pon-vst3-addon")) return paths;
#ifdef Q_OS_MACOS
  // Unknown hand-copied dependencies are never claimed or recursively removed.
  return paths;
#else
  for (const char *name : {
           "VST3-SDK-LICENSE.txt", "Qt6Core.dll", "Qt6Gui.dll",
           "Qt6Network.dll", "Qt6Pdf.dll", "Qt6Svg.dll",
           "Qt6Widgets.dll", "d3dcompiler_47.dll", "dxcompiler.dll",
           "dxil.dll", "opengl32sw.dll", "vc_redist.x64.exe",
           "generic/qtuiotouchplugin.dll", "iconengines/qsvgicon.dll",
           "imageformats/qgif.dll", "imageformats/qico.dll",
           "imageformats/qjpeg.dll", "imageformats/qpdf.dll",
           "imageformats/qsvg.dll", "networkinformation/qnetworklistmanager.dll",
           "platforms/qwindows.dll", "styles/qmodernwindowsstyle.dll",
           "tls/qcertonlybackend.dll", "tls/qschannelbackend.dll"})
    paths.append(QStringLiteral("audio-addons/") + QString::fromLatin1(name));
  return paths;
#endif
}
bool validManualFiles(const QString &id, const QJsonArray &files, QString *error) {
  if (files.isEmpty()) return fail(error, QStringLiteral("No manual add-on files were selected."));
  const auto allowed = manualPaths(id);
  QSet<QString> seen;
  for (const auto &value : files) {
    const auto item = value.toObject();
    const QString path = item.value(QStringLiteral("path")).toString();
    const QString sha = item.value(QStringLiteral("sha256")).toString();
    if (item.size() != 2 || !allowed.contains(path) || !validSha(sha) ||
        seen.contains(path))
      return fail(error, QStringLiteral("Invalid manual add-on file list."));
    seen.insert(path);
  }
  if (!seen.contains(entryPath(id)))
    return fail(error, QStringLiteral("The manually installed add-on is missing."));
  return true;
}
bool inflateEntry(const QByteArray &compressed, quint32 size,
                  QByteArray *expanded) {
  expanded->resize(qsizetype(size));
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
  stream.avail_in = uInt(compressed.size());
  stream.next_out = reinterpret_cast<Bytef *>(expanded->data());
  stream.avail_out = uInt(expanded->size());
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
  const int result = inflate(&stream, Z_FINISH);
  const bool valid = result == Z_STREAM_END && stream.total_out == size &&
                     stream.total_in == uLong(compressed.size());
  inflateEnd(&stream);
  return valid;
}
bool writeBytes(const QString &path, const QByteArray &bytes, QString *error) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return fail(error, QStringLiteral("Could not create the destination directory."));
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit())
    return fail(error, QStringLiteral("Could not write %1.").arg(path));
  return true;
}
bool readJson(const QString &path, QJsonObject *object) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  *object = document.object();
  return true;
}
QString recordPath(const QString &root, const QString &id) {
  return stateDirectory(root) + QLatin1Char('/') + id + QStringLiteral(".json");
}
QString jobPath(const QString &root, const QString &id) {
  return stateDirectory(root) + QLatin1Char('/') + id + QStringLiteral(".job.json");
}
QString backupDirectory(const QString &root, const QString &id) {
  return stateDirectory(root) + QLatin1Char('/') + id + QStringLiteral(".backup");
}
void clearBackups(const QString &root, const QString &id, int count) {
  const QString directory = backupDirectory(root, id);
  for (int i = 0; i < count; ++i)
    QFile::remove(directory + QLatin1Char('/') + QString::number(i));
  QDir().rmdir(directory);
}
bool safeDestination(const QString &root, const QString &relative, QString *error) {
  if (!validPath(relative)) return fail(error, QStringLiteral("Invalid destination path."));
  const QString application = canonicalRoot(root);
  const QString canonical = installationDirectory(application);
  if (application.isEmpty() || !QFileInfo(applicationExecutable(application)).isFile())
    return fail(error, QStringLiteral("The VTR-PON installation was not found."));
#ifdef Q_OS_MACOS
  // Validate existing ancestors before creating the external install directory.
  for (QString ancestor = canonical; !ancestor.isEmpty() && ancestor != QLatin1String("/");
       ancestor = QFileInfo(ancestor).absolutePath())
    if (QFileInfo(ancestor).isSymLink())
      return fail(error, QStringLiteral("The add-on directory uses a link."));
#endif
  QString current = canonical;
  const auto parts = relative.split(QLatin1Char('/'));
  for (const auto &part : parts) {
    current += QLatin1Char('/') + part;
#ifdef Q_OS_WIN
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
      return fail(error, QStringLiteral("A destination uses a reparse point: %1").arg(current));
#else
    if (QFileInfo(current).isSymLink())
      return fail(error, QStringLiteral("A destination uses a link: %1").arg(current));
#endif
  }
  return true;
}
QString fileSha256(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  if (!hasher.addData(&file)) return {};
  return QString::fromLatin1(hasher.result().toHex());
}
bool fileMatches(const QString &path, const QString &expected) {
  return fileSha256(path) == expected;
}
bool restoreJob(const QString &root, const QString &id, QJsonObject *job,
                QString *error) {
  const auto newFiles = job->value(QStringLiteral("newFiles")).toArray();
  const auto backups = job->value(QStringLiteral("backups")).toArray();
  const auto previous = job->value(QStringLiteral("oldRecord")).toObject();
  QMap<QString, QString> oldHashes;
  for (const auto &entry : previous.value(QStringLiteral("files")).toArray()) {
    const auto object = entry.toObject();
    oldHashes.insert(object.value(QStringLiteral("path")).toString(),
                     object.value(QStringLiteral("sha256")).toString());
  }
  if (job->value(QStringLiteral("operation")).toString() ==
      QLatin1String("manual-uninstall")) {
    const auto manualFiles = job->value(QStringLiteral("manualFiles")).toArray();
    if (!previous.isEmpty() || !validManualFiles(id, manualFiles, error)) return false;
    for (const auto &entry : manualFiles) {
      const auto item = entry.toObject();
      oldHashes.insert(item.value(QStringLiteral("path")).toString(),
                       item.value(QStringLiteral("sha256")).toString());
    }
  }
  const QFileInfo backupInfo(backupDirectory(root, id));
  if (backupInfo.isSymLink() || backupInfo.isJunction())
    return fail(error, QStringLiteral("Recovery directory uses a link."));
  for (const auto &value : newFiles) {
    const QString relative = value.toString();
    if (!ownedPath(id, relative) || !safeDestination(root, relative, error)) return false;
    const QString destination = installationDirectory(root) + QLatin1Char('/') + relative;
    if (QFileInfo::exists(destination) && !QFile::remove(destination))
      return fail(error, QStringLiteral("Could not remove a partial add-on file: %1").arg(relative));
  }
  for (int i = 0; i < backups.size(); ++i) {
    const QString relative = backups.at(i).toString();
    if (!ownedPath(id, relative) || !safeDestination(root, relative, error)) return false;
    QFile file(backupDirectory(root, id) + QLatin1Char('/') + QString::number(i));
    if (!file.open(QIODevice::ReadOnly))
      return fail(error, QStringLiteral("Could not restore add-on file: %1").arg(relative));
    const QByteArray contents = file.readAll();
    if (oldHashes.value(relative).isEmpty() || hash(contents) != oldHashes.value(relative) ||
        !writeBytes(installationDirectory(root) + QLatin1Char('/') + relative, contents, error))
      return fail(error, QStringLiteral("Recovery file is invalid: %1").arg(relative));
#ifdef Q_OS_MACOS
    const auto savedPermissions = job->value(QStringLiteral("oldPermissions")).toObject();
    if (savedPermissions.contains(relative) && !QFile::setPermissions(
          installationDirectory(root) + QLatin1Char('/') + relative,
          QFile::Permissions(savedPermissions.value(relative).toInt())))
      return fail(error, QStringLiteral("Could not restore add-on permissions."));
#endif
  }
  if (previous.isEmpty()) {
    if (QFileInfo::exists(recordPath(root, id)) && !QFile::remove(recordPath(root, id)))
      return fail(error, QStringLiteral("Could not restore installed information."));
  } else if (!writeBytes(recordPath(root, id), QJsonDocument(previous).toJson(), error)) {
    return false;
  }
  job->insert(QStringLiteral("stage"), QStringLiteral("prepared"));
  return writeBytes(jobPath(root, id), QJsonDocument(*job).toJson(), error);
}
bool launch(const QString &root, const QString &job, QString *error) {
  const QString helper = managerExecutable(root);
  if (!QFileInfo::exists(helper))
    return fail(error, QStringLiteral("The add-on manager executable is missing."));
  if (!QProcess::startDetached(helper,
          {QStringLiteral("--apply"), job, QStringLiteral("--parent-pid"),
           QString::number(QCoreApplication::applicationPid())}, root))
    return fail(error, QStringLiteral("Could not start the add-on manager."));
  return true;
}
#ifdef Q_OS_WIN
bool elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD length = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                      sizeof(elevation), &length);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated;
}
bool requestElevation(const QString &path, int *result) {
  const QString executable = QCoreApplication::applicationFilePath();
  const QString parameters = QStringLiteral("--apply \"") + path +
                             QStringLiteral("\" --parent-pid 0");
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = reinterpret_cast<LPCWSTR>(executable.utf16());
  info.lpParameters = reinterpret_cast<LPCWSTR>(parameters.utf16());
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) return false;
  if (!info.hProcess) return false;
  DWORD exitCode = 1;
  const bool finished = WaitForSingleObject(info.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                        GetExitCodeProcess(info.hProcess, &exitCode);
  CloseHandle(info.hProcess);
  if (finished) *result = int(exitCode);
  return finished;
}
bool waitForRunningCopies(const QString &root, QString *error,
                          const wchar_t *executableName = L"vtr-pon.exe") {
  for (;;) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
      return fail(error, QStringLiteral("Could not check running VTR-PON copies."));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HANDLE matching = nullptr;
    if (Process32FirstW(snapshot, &entry)) do {
      if (_wcsicmp(entry.szExeFile, executableName) != 0 ||
          entry.th32ProcessID == GetCurrentProcessId()) continue;
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                   FALSE, entry.th32ProcessID);
      if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // It exited after the snapshot.
        CloseHandle(snapshot);
        return fail(error, QStringLiteral("Could not inspect a running VTR-PON process."));
      }
      wchar_t path[32768]{};
      DWORD length = DWORD(std::size(path));
      if (!QueryFullProcessImageNameW(process, 0, path, &length)) {
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
          CloseHandle(process);
          continue;
        }
        CloseHandle(process);
        CloseHandle(snapshot);
        return fail(error, QStringLiteral("Could not inspect a running VTR-PON path."));
      }
      if (canonicalRoot(QFileInfo(QString::fromWCharArray(path)).absolutePath()) == root) {
        matching = process;
        break;
      }
      CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (!matching) return true;
    WaitForSingleObject(matching, INFINITE);
    CloseHandle(matching);
  }
}
#endif

} // namespace

QStringList ids() {
#ifdef Q_OS_MACOS
  return {QStringLiteral("vtr-pon-atem-addon"), QStringLiteral("vtr-pon-network-video-addon"),
          QStringLiteral("vtr-pon-vst3-addon"), QStringLiteral("vtr-pon-decklink-addon")};
#else
  return {QStringLiteral("vtr-pon-atem-addon"),
          QStringLiteral("vtr-pon-network-video-addon"),
          QStringLiteral("vtr-pon-vst3-addon"), QStringLiteral("vtr-pon-decklink-addon")};
#endif
}
QString entryPath(const QString &id) {
#ifdef Q_OS_MACOS
  if (id == QLatin1String("vtr-pon-decklink-addon")) return QStringLiteral("decklink-addons/vtr-pon-decklink-addon.dylib");
  if (id == QLatin1String("vtr-pon-atem-addon")) return QStringLiteral("switcher-addons/vtr-pon-atem-addon.dylib");
  if (id == QLatin1String("vtr-pon-network-video-addon")) return QStringLiteral("network-video-addons/vtr-pon-network-video-addon.dylib");
  if (id == QLatin1String("vtr-pon-vst3-addon")) return QStringLiteral("audio-addons/vtr-pon-vst3-addon.app/Contents/MacOS/vtr-pon-vst3-addon");
  return {};
#else
  if (id == QLatin1String("vtr-pon-atem-addon"))
    return QStringLiteral("switcher-addons/vtr-pon-atem-addon.dll");
  if (id == QLatin1String("vtr-pon-network-video-addon"))
    return QStringLiteral("network-video-addons/vtr-pon-network-video-addon.dll");
  if (id == QLatin1String("vtr-pon-decklink-addon"))
    return QStringLiteral("decklink-addons/vtr-pon-decklink-addon.dll");
  if (id == QLatin1String("vtr-pon-vst3-addon"))
    return QStringLiteral("audio-addons/vtr-pon-vst3-addon.exe");
  return {};
#endif
}
QString contract(const QString &id) {
  if (id == QLatin1String("vtr-pon-atem-addon")) return QStringLiteral("switcher-addon-abi-v1");
  if (id == QLatin1String("vtr-pon-network-video-addon")) return QStringLiteral("network-video-abi-v2");
  if (id == QLatin1String("vtr-pon-vst3-addon")) return QStringLiteral("vst-protocol-v2");
  if (id == QLatin1String("vtr-pon-decklink-addon")) return QStringLiteral("decklink-media-abi-v1");
  return {};
}
int compareVersions(const QString &left, const QString &right) {
  const auto a = left.split(QLatin1Char('.'));
  const auto b = right.split(QLatin1Char('.'));
  if (a.size() != 3 || b.size() != 3) return 0;
  for (int i = 0; i < 3; ++i) {
    if (a.at(i).size() != b.at(i).size())
      return a.at(i).size() < b.at(i).size() ? -1 : 1;
    const int order = QString::compare(a.at(i), b.at(i), Qt::CaseSensitive);
    if (order != 0) return order < 0 ? -1 : 1;
  }
  return 0;
}
bool hasUniqueJsonKeys(const QByteArray &json) {
  QVector<QSet<QString>> objects;
  for (qsizetype index = 0; index < json.size(); ++index) {
    const char c = json.at(index);
    if (c == '{') objects.append(QSet<QString>{});
    else if (c == '}') {
      if (objects.isEmpty()) return false;
      objects.removeLast();
    } else if (c == '"') {
      const qsizetype start = index;
      ++index;
      while (index < json.size()) {
        if (json.at(index) == '\\') { index += 2; continue; }
        if (json.at(index) == '"') break;
        ++index;
      }
      if (index >= json.size()) return false;
      qsizetype next = index + 1;
      while (next < json.size() &&
             (json.at(next) == ' ' || json.at(next) == '\t' ||
              json.at(next) == '\r' || json.at(next) == '\n')) ++next;
      if (next < json.size() && json.at(next) == ':' && !objects.isEmpty()) {
        const auto parsed = QJsonDocument::fromJson(
            QByteArrayLiteral("[") + json.mid(start, index - start + 1) +
            QByteArrayLiteral("]"));
        if (!parsed.isArray() || parsed.array().size() != 1) return false;
        const QString key = parsed.array().first().toString();
        if (objects.last().contains(key)) return false;
        objects.last().insert(key);
      }
    }
  }
  return objects.isEmpty();
}
QString stateDirectory(const QString &applicationRoot) {
  if (!stateOverride.isEmpty()) return stateOverride;
  const QString root = canonicalRoot(applicationRoot);
  const QString key = rootKey(root);
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
      .filePath(currentProductStorageName() +
                QStringLiteral("/addon-manager/") + key);
}
QStringList pendingJobs(const QString &applicationRoot) {
  QStringList jobs;
  for (const auto &id : ids()) {
    const QString path = jobPath(applicationRoot, id);
    if (QFileInfo::exists(path)) jobs.append(path);
  }
  return jobs;
}
bool restartInProgress(const QString &applicationRoot) {
  const QString path = stateDirectory(applicationRoot) + QStringLiteral("/restart.lock");
  if (!QFileInfo::exists(path)) return false;
  QLockFile lock(path);
  lock.setStaleLockTime(0);
  if (lock.tryLock(0)) { lock.unlock(); return false; }
  return lock.error() == QLockFile::LockFailedError;
}
bool requestRestart(const QString &applicationRoot, const QStringList &arguments,
                    const QString &workingDirectory, QString *error) {
  if (!platformFeatureImplemented(PlatformFeature::AddonManagement))
    return fail(error, QStringLiteral("Add-on restart is not implemented on this platform."));
  const QString root = canonicalRoot(applicationRoot);
  const QString helper = managerExecutable(root);
  if (root.isEmpty() || !QFileInfo(applicationExecutable(root)).isFile() ||
      !QFileInfo::exists(helper))
    return fail(error, QStringLiteral("The add-on manager executable is missing."));
  if (!QDir().mkpath(stateDirectory(root)))
    return fail(error, QStringLiteral("Could not create manager data."));
  QStringList pendingIds;
  for (const auto &job : pendingJobs(root))
    pendingIds.append(QFileInfo(job).fileName().chopped(QStringLiteral(".job.json").size()));
  QStringList helperArguments{QStringLiteral("--restart"), QStringLiteral("--parent-pid"),
      QString::number(QCoreApplication::applicationPid()),
      QStringLiteral("--pending-ids=") + pendingIds.join(QLatin1Char(','))};
#ifdef Q_OS_WIN
  const QString readyName = QStringLiteral("Local\\VTR-PON-AddonRestart-") +
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE,
      reinterpret_cast<LPCWSTR>(readyName.utf16()));
  if (!ready) return fail(error, QStringLiteral("Could not prepare the restart handshake."));
  helperArguments.append(QStringLiteral("--ready-event=") + readyName);
#endif
#ifdef Q_OS_MACOS
  QLocalServer ready;
  ready.setSocketOptions(QLocalServer::UserAccessOption);
  // macOS AF_UNIX paths are short; the per-user temp directory can exceed the limit.
  const QString readyName = QStringLiteral("/private/tmp/vtrpon-restart-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!ready.listen(readyName)) return fail(error, QStringLiteral("Could not prepare the restart handshake: %1").arg(ready.errorString()));
  helperArguments.append(QStringLiteral("--ready-socket=") + readyName);
#endif
  helperArguments.append(QStringLiteral("--"));
  for (const auto &argument : arguments) {
    if (argument != QLatin1String("--addon-restart-complete") &&
        !argument.startsWith(QLatin1String("--addon-restart-error=")))
      helperArguments.append(argument);
  }
  qint64 helperPid = 0;
  const bool launched = QProcess::startDetached(helper, helperArguments, workingDirectory, &helperPid);
#ifdef Q_OS_WIN
  // Process creation alone does not prove that the installed helper understands
  // the restart request or even reached main(). Keep the Controller open if not.
  HANDLE process = launched ? OpenProcess(SYNCHRONIZE, FALSE, DWORD(helperPid)) : nullptr;
  HANDLE handles[] = {ready, process};
  const bool acknowledged = launched &&
      WaitForMultipleObjects(process ? 2 : 1, handles, FALSE, 5000) == WAIT_OBJECT_0;
  if (process) CloseHandle(process);
  CloseHandle(ready);
  if (!acknowledged)
    return fail(error, QStringLiteral("The add-on restart manager did not become ready. VTR-PON has been kept open."));
#endif
#ifdef Q_OS_MACOS
  bool acknowledged = false;
  if (launched && ready.waitForNewConnection(5000)) {
    const auto socket = ready.nextPendingConnection();
    if (socket && (socket->bytesAvailable() || socket->waitForReadyRead(1000)))
      acknowledged = socket->readAll() == QByteArrayLiteral("ready");
    if (socket) socket->deleteLater();
  }
  if (!acknowledged) return fail(error, QStringLiteral("The add-on restart manager did not become ready. VTR-PON has been kept open."));
#endif
  if (!launched)
    return fail(error, QStringLiteral("Could not start the add-on restart manager."));
  return true;
}
bool finishPendingOperations(const QString &applicationRoot, qint64 parentPid,
                             const QStringList &pendingIds, QString *error) {
  const QString root = canonicalRoot(applicationRoot);
  if (root.isEmpty()) return fail(error, QStringLiteral("The VTR-PON installation was not found."));
  QStringList originalJobs = pendingJobs(root);
  for (const auto &id : pendingIds) {
    if (!ids().contains(id)) return fail(error, QStringLiteral("Invalid pending add-on ID."));
    if (!originalJobs.contains(jobPath(root, id))) originalJobs.append(jobPath(root, id));
  }
#ifdef Q_OS_WIN
  if (parentPid > 0) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, DWORD(parentPid));
    if (parent) { WaitForSingleObject(parent, INFINITE); CloseHandle(parent); }
  }
  // Existing workers may still be waiting for this Controller to release DLLs.
  // Their elevated children also finish before the unelevated worker exits.
  if (!waitForRunningCopies(root, error) ||
      !waitForRunningCopies(root, error, L"vtr-pon-addon-manager.exe")) return false;
#elif defined(Q_OS_MACOS)
  if (!waitForAddonProcesses(root, parentPid, false, error) ||
      !waitForAddonProcesses(root, 0, true, error)) return false;
#else
  Q_UNUSED(parentPid)
#endif
  bool succeeded = true;
  for (const auto &job : pendingJobs(root)) {
    QString detail;
    if (applyJob(job, 0, &detail) != 0) {
      succeeded = false;
      if (error) *error = detail;
    }
  }
  for (const auto &job : originalJobs) {
    QFile failure(job.chopped(QStringLiteral(".job.json").size()) + QStringLiteral(".error.txt"));
    if (failure.open(QIODevice::ReadOnly)) {
      succeeded = false;
      if (error) *error = QString::fromUtf8(failure.readAll());
    }
  }
  return succeeded && pendingJobs(root).isEmpty();
}
QJsonObject installed(const QString &applicationRoot, const QString &id) {
  QJsonObject object;
  if (!ids().contains(id) || !readJson(recordPath(applicationRoot, id), &object) ||
      object.value(QStringLiteral("root")).toString() != canonicalRoot(applicationRoot) ||
      object.value(QStringLiteral("manifest")).toObject().value(QStringLiteral("id")).toString() != id)
    return {};
  return object;
}

bool readPackage(const QString &path, Package *package, QString *error) {
  if (!package) return fail(error, QStringLiteral("No package destination."));
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumArchive)
    return fail(error, QStringLiteral("The add-on package is missing or too large."));
  const QByteArray zip = file.readAll();
  if (zip.size() < 22) return fail(error, QStringLiteral("The ZIP is incomplete."));
  qsizetype eocd = -1;
  for (qsizetype pos = zip.size() - 22;
       pos >= qMax<qsizetype>(0, zip.size() - 65557); --pos) {
    if (u32(zip, pos) == 0x06054b50 && pos + 22 + u16(zip, pos + 20) == zip.size()) {
      eocd = pos;
      break;
    }
  }
  if (eocd < 0 || u16(zip, eocd + 4) != 0 || u16(zip, eocd + 6) != 0 ||
      u16(zip, eocd + 8) != u16(zip, eocd + 10) ||
      u16(zip, eocd + 10) > 1000 ||
      quint64(u32(zip, eocd + 16)) + u32(zip, eocd + 12) != quint64(eocd))
    return fail(error, QStringLiteral("Unsupported ZIP structure."));
  qsizetype cursor = u32(zip, eocd + 16);
  const qsizetype centralEnd = eocd;
  QSet<QString> names;
  QSet<QString> directories;
  QMap<QString, QByteArray> entries;
  qsizetype expandedTotal = 0;
  for (int index = 0; index < u16(zip, eocd + 10); ++index) {
    if (cursor + 46 > centralEnd || u32(zip, cursor) != 0x02014b50)
      return fail(error, QStringLiteral("Invalid ZIP directory."));
    const quint16 flags = u16(zip, cursor + 8);
    const quint16 method = u16(zip, cursor + 10);
    const quint32 crc = u32(zip, cursor + 16);
    const quint32 packed = u32(zip, cursor + 20);
    const quint32 unpacked = u32(zip, cursor + 24);
    const quint16 nameLength = u16(zip, cursor + 28);
    const quint16 extraLength = u16(zip, cursor + 30);
    const quint16 commentLength = u16(zip, cursor + 32);
    const quint32 external = u32(zip, cursor + 38);
    const quint32 localOffset = u32(zip, cursor + 42);
    if (quint64(cursor) + 46 + nameLength + extraLength + commentLength > quint64(centralEnd) ||
        flags & 1 || (method != 0 && method != 8) ||
        ((external >> 16) & 0170000) == 0120000 ||
        packed > kMaximumArchive || unpacked > kMaximumExpanded ||
        expandedTotal + unpacked > kMaximumExpanded)
      return fail(error, QStringLiteral("Unsupported or unsafe ZIP entry."));
    const QByteArray rawName = zip.mid(cursor + 46, nameLength);
    const QString name = QString::fromUtf8(rawName);
    if (name.toUtf8() != rawName)
      return fail(error, QStringLiteral("Invalid UTF-8 ZIP path."));
    const bool directory = name.endsWith(QLatin1Char('/'));
    const QString relative = directory ? name.left(name.size() - 1) : name;
    if (!validPath(relative)) return fail(error, QStringLiteral("Unsafe ZIP path: %1").arg(name));
    const QString folded = relative.toCaseFolded();
    if (names.contains(folded)) return fail(error, QStringLiteral("Duplicate ZIP path: %1").arg(name));
    names.insert(folded);
    if (directory) directories.insert(folded);
    const qsizetype local = localOffset;
    if (local + 30 > cursor || u32(zip, local) != 0x04034b50 ||
        u16(zip, local + 8) != method ||
        zip.mid(local + 30, u16(zip, local + 26)) != rawName)
      return fail(error, QStringLiteral("Invalid local ZIP entry."));
    const quint64 dataStart = quint64(local) + 30 + u16(zip, local + 26) + u16(zip, local + 28);
    if (dataStart + packed > quint64(cursor))
      return fail(error, QStringLiteral("Overlapping ZIP entry."));
    QByteArray contents;
    const QByteArray compressed = zip.mid(qsizetype(dataStart), packed);
    if (method == 0) contents = compressed;
    else if (!inflateEntry(compressed, unpacked, &contents))
      return fail(error, QStringLiteral("Could not decompress ZIP entry."));
    if (contents.size() != unpacked ||
        crc32(0, reinterpret_cast<const Bytef *>(contents.constData()), uInt(contents.size())) != crc ||
        (directory && !contents.isEmpty()))
      return fail(error, QStringLiteral("ZIP entry checksum or size mismatch."));
    if (!directory) entries.insert(relative, contents);
    expandedTotal += unpacked;
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  if (cursor != centralEnd) return fail(error, QStringLiteral("Unexpected ZIP directory data."));
  for (const auto &name : names) {
    QString parent = name;
    while (parent.contains(QLatin1Char('/'))) {
      parent = parent.left(parent.lastIndexOf(QLatin1Char('/')));
      if (names.contains(parent) && !directories.contains(parent))
        return fail(error, QStringLiteral("ZIP file and directory collide."));
    }
  }
  if (!entries.contains(QStringLiteral("manifest.json")) ||
      !entries.contains(QStringLiteral("README.md")))
    return fail(error, QStringLiteral("The package needs a manifest and README."));
  const QByteArray manifestBytes = entries.take(QStringLiteral("manifest.json"));
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(manifestBytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
      !hasUniqueJsonKeys(manifestBytes))
    return fail(error, QStringLiteral("Invalid manifest.json."));
  const QJsonObject manifest = document.object();
  const QString id = manifest.value(QStringLiteral("id")).toString();
  const int formatVersion = manifest.value(QStringLiteral("formatVersion")).toInt(-1);
  const bool hasPlatform = manifest.contains(QStringLiteral("platform"));
#ifdef Q_OS_MACOS
  if (formatVersion != 2 || !hasPlatform || manifest.value(QStringLiteral("platform")).toString() != platformId())
    return fail(error, QStringLiteral("This package is not for this Mac's OS and CPU."));
#endif
  if ((formatVersion != 1 && formatVersion != 2) ||
      !ids().contains(id) || !validVersion(manifest.value(QStringLiteral("version")).toString()) ||
      manifest.value(QStringLiteral("contract")).toString() != contract(id) ||
      (manifest.contains(QStringLiteral("runtimeStatus")) &&
       (id == QLatin1String("vtr-pon-vst3-addon") ||
        manifest.value(QStringLiteral("runtimeStatus")).toInt(-1) != 1)) ||
      manifest.size() != 4 + int(manifest.contains(QStringLiteral("runtimeStatus"))) +
                             int(formatVersion == 2) + int(hasPlatform) ||
      (hasPlatform && manifest.value(QStringLiteral("platform")).toString() != platformId()) ||
      (formatVersion == 2 && !manifest.value(QStringLiteral("agreements")).isArray()))
    return fail(error, QStringLiteral("Unsupported add-on manifest."));
  Package result;
  result.manifest = manifest;
  result.sha256 = hash(zip);
  result.readme = entries.take(QStringLiteral("README.md"));
  const QString folder = entryPath(id).section(QLatin1Char('/'), 0, 0);
  const QString payloadFolder = QStringLiteral("payload/") + folder;
  for (const auto &directory : directories) {
    if (directory != QLatin1String("payload") &&
        directory != QLatin1String("licenses") &&
        !directory.startsWith(QStringLiteral("licenses/")) &&
        directory != payloadFolder &&
        !directory.startsWith(payloadFolder + QLatin1Char('/')))
      return fail(error, QStringLiteral("Unexpected package directory: %1").arg(directory));
  }
  for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
    if (it.key().startsWith(QStringLiteral("licenses/"))) {
      if (formatVersion == 2 &&
          (!displayableDocument(it.value()) ||
           (!it.key().endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive) &&
            !it.key().endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))))
        return fail(error, QStringLiteral("License documents must be UTF-8 text: %1").arg(it.key()));
      result.notices.append({it.key(), it.value(), hash(it.value())});
    } else if (it.key().startsWith(QStringLiteral("payload/") + folder + QLatin1Char('/'))) {
      const QString relative = it.key().mid(8);
      if (!validatePlatformPayload(relative, it.value(), error)) return false;
      result.files.append({relative, it.value(), hash(it.value())});
    } else {
      return fail(error, QStringLiteral("Unexpected package file: %1").arg(it.key()));
    }
  }
  if (formatVersion == 2) {
    const QJsonArray declared = manifest.value(QStringLiteral("agreements")).toArray();
    if (declared.isEmpty() || declared.size() > 16)
      return fail(error, QStringLiteral("The package needs an agreement document."));
    QSet<QString> seenAgreements;
    for (const auto &item : declared) {
      if (!item.isObject()) return fail(error, QStringLiteral("Invalid agreement declaration."));
      const QJsonObject object = item.toObject();
      const QString path = object.value(QStringLiteral("path")).toString();
      const QString sha = object.value(QStringLiteral("sha256")).toString();
      if (object.size() != 2 || !path.startsWith(QStringLiteral("licenses/")) ||
          !validPath(path) || !validSha(sha) || seenAgreements.contains(path.toCaseFolded()))
        return fail(error, QStringLiteral("Invalid agreement declaration."));
      seenAgreements.insert(path.toCaseFolded());
      const auto notice = std::find_if(result.notices.cbegin(), result.notices.cend(),
          [&path](const File &entry) { return entry.path == path; });
      if (notice == result.notices.cend() || notice->sha256 != sha)
        return fail(error, QStringLiteral("An agreement document is missing or changed: %1").arg(path));
      result.agreements.append(*notice);
    }
    result.notices.erase(std::remove_if(result.notices.begin(), result.notices.end(),
        [&seenAgreements](const File &entry) {
          return seenAgreements.contains(entry.path.toCaseFolded());
        }), result.notices.end());
  }
  bool hasEntry = false;
  for (const auto &entry : result.files) if (entry.path == entryPath(id)) hasEntry = true;
  if (!hasEntry) return fail(error, QStringLiteral("The add-on executable is missing."));
  *package = std::move(result);
  return true;
}

QJsonObject consentReceipt(const Package &package) {
  if (package.manifest.value(QStringLiteral("formatVersion")).toInt(-1) != 2 ||
      package.agreements.isEmpty()) return {};
  return QJsonObject{{QStringLiteral("id"), package.manifest.value(QStringLiteral("id"))},
                     {QStringLiteral("version"), package.manifest.value(QStringLiteral("version"))},
                     {QStringLiteral("packageSha256"), package.sha256},
                     {QStringLiteral("agreements"), agreementDigests(package)},
                     {QStringLiteral("acceptedAtUtc"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}

bool queueInstall(const QString &applicationRoot, const QString &path,
                  const QString &expectedId, const QString &expectedVersion,
                  const QString &expectedSha256, const QJsonObject &consent,
                  QString *error) {
  if (!platformFeatureImplemented(PlatformFeature::AddonManagement))
    return fail(error, QStringLiteral("Add-on installation is not implemented on this platform."));
  const QString root = canonicalRoot(applicationRoot);
  Package package;
  if (!readPackage(path, &package, error)) return false;
  const QString id = package.manifest.value(QStringLiteral("id")).toString();
  if ((!expectedId.isEmpty() && expectedId != id) ||
      (!expectedVersion.isEmpty() && expectedVersion != package.manifest.value(QStringLiteral("version")).toString()) ||
      (!expectedSha256.isEmpty() && expectedSha256 != package.sha256))
    return fail(error, QStringLiteral("The package does not match the catalog."));
  if (!validConsent(package, consent))
    return fail(error, QStringLiteral("Agreement to this package is required before installation."));
  if (QFileInfo::exists(jobPath(root, id)))
    return fail(error, QStringLiteral("An operation is already waiting for this add-on."));
  const auto old = installed(root, id);
  if (!old.isEmpty() &&
      compareVersions(old.value(QStringLiteral("manifest")).toObject()
                          .value(QStringLiteral("version")).toString(),
                      package.manifest.value(QStringLiteral("version")).toString()) >= 0)
    return fail(error, QStringLiteral("The installed version is already current or newer."));
  QSet<QString> owned;
  for (const auto &item : old.value(QStringLiteral("files")).toArray())
    owned.insert(item.toObject().value(QStringLiteral("path")).toString().toCaseFolded());
  for (const auto &entry : package.files) {
    if (!safeDestination(root, entry.path, error)) return false;
    if (QFileInfo::exists(installationDirectory(root) + QLatin1Char('/') + entry.path) &&
        !owned.contains(entry.path.toCaseFolded()))
      return fail(error, QStringLiteral("An unmanaged file already exists: %1").arg(entry.path));
  }
  for (const auto &item : old.value(QStringLiteral("files")).toArray()) {
    const auto object = item.toObject();
    const QString relative = object.value(QStringLiteral("path")).toString();
    if (!ownedPath(id, relative) || !safeDestination(root, relative, error) ||
        !fileMatches(installationDirectory(root) + QLatin1Char('/') + relative,
                     object.value(QStringLiteral("sha256")).toString()))
      return fail(error, QStringLiteral("An installed file was changed: %1").arg(relative));
  }
  const QString folder = stateDirectory(root);
  if (!QDir().mkpath(folder)) return fail(error, QStringLiteral("Could not create manager data."));
  const QString staged = folder + QLatin1Char('/') + id + QStringLiteral(".vtrponaddon");
  QFile::remove(staged);
  if (!QFile::copy(path, staged)) return fail(error, QStringLiteral("Could not stage the package."));
  const QJsonObject job{{QStringLiteral("root"), root}, {QStringLiteral("id"), id},
                        {QStringLiteral("operation"), QStringLiteral("install")},
                        {QStringLiteral("package"), staged},
                        {QStringLiteral("sha256"), package.sha256},
                        {QStringLiteral("consent"), consent}};
  if (!writeBytes(jobPath(root, id), QJsonDocument(job).toJson(), error) ||
      !launch(root, jobPath(root, id), error)) {
    QFile::remove(jobPath(root, id));
    QFile::remove(staged);
    return false;
  }
  QFile::remove(folder + QLatin1Char('/') + id + QStringLiteral(".error.txt"));
  return true;
}

bool queueUninstall(const QString &applicationRoot, const QString &id,
                    QString *error) {
  if (!platformFeatureImplemented(PlatformFeature::AddonManagement))
    return fail(error, QStringLiteral("Add-on removal is not implemented on this platform."));
  const QString root = canonicalRoot(applicationRoot);
  if (!ids().contains(id) || root.isEmpty())
    return fail(error, QStringLiteral("Invalid add-on target."));
  if (QFileInfo::exists(jobPath(root, id)))
    return fail(error, QStringLiteral("An operation is already waiting for this add-on."));
  const bool managed = !installed(root, id).isEmpty();
  QJsonObject job{{QStringLiteral("root"), root}, {QStringLiteral("id"), id},
                  {QStringLiteral("operation"),
                   managed ? QStringLiteral("uninstall") :
                             QStringLiteral("manual-uninstall")}};
  if (!managed) {
    if (QFileInfo::exists(recordPath(root, id)))
      return fail(error, QStringLiteral("The add-on management record is invalid."));
    QJsonArray files;
    for (const auto &relative : manualPaths(id)) {
      if (!safeDestination(root, relative, error)) return false;
      const QString path = installationDirectory(root) + QLatin1Char('/') + relative;
      if (!QFileInfo::exists(path)) continue;
      if (!QFileInfo(path).isFile())
        return fail(error, QStringLiteral("An add-on path is not a file: %1").arg(relative));
      const QString sha = fileSha256(path);
      if (!validSha(sha))
        return fail(error, QStringLiteral("Could not inspect add-on file: %1").arg(relative));
      files.append(QJsonObject{{QStringLiteral("path"), relative},
                               {QStringLiteral("sha256"), sha}});
    }
    if (!validManualFiles(id, files, error)) return false;
    job.insert(QStringLiteral("manualFiles"), files);
  }
  if (!writeBytes(jobPath(root, id), QJsonDocument(job).toJson(), error) ||
      !launch(root, jobPath(root, id), error)) {
    QFile::remove(jobPath(root, id));
    return false;
  }
  QFile::remove(stateDirectory(root) + QLatin1Char('/') + id +
                QStringLiteral(".error.txt"));
  return true;
}

int applyJobImpl(const QString &path, qint64 parentPid, QString *error,
                 bool *alreadyFinished) {
  QJsonObject job;
  if (!readJson(path, &job)) { fail(error, QStringLiteral("Invalid operation file.")); return 1; }
  const QString root = job.value(QStringLiteral("root")).toString();
  const QString id = job.value(QStringLiteral("id")).toString();
  const QString operation = job.value(QStringLiteral("operation")).toString();
  if (!ids().contains(id) || canonicalRoot(root) != root || path != jobPath(root, id) ||
      (operation != QLatin1String("install") &&
       operation != QLatin1String("uninstall") &&
       operation != QLatin1String("manual-uninstall"))) {
    fail(error, QStringLiteral("Invalid operation target.")); return 1;
  }
#ifdef Q_OS_WIN
  if (parentPid > 0) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, DWORD(parentPid));
    if (parent) { WaitForSingleObject(parent, INFINITE); CloseHandle(parent); }
  }
  if (!waitForRunningCopies(root, error)) return 1;
  QTemporaryFile probe(root + QStringLiteral("/.vtrpon-addon-write-XXXXXX"));
  if (!probe.open()) {
    int elevatedResult = 1;
    if (elevated() || !requestElevation(path, &elevatedResult)) {
      fail(error, QStringLiteral("Write access to the VTR-PON directory was denied."));
      return 1;
    }
    if (elevatedResult != 0) {
      QFile failure(stateDirectory(root) + QLatin1Char('/') + id + QStringLiteral(".error.txt"));
      fail(error, failure.open(QIODevice::ReadOnly) ? QString::fromUtf8(failure.readAll()) :
          QStringLiteral("The elevated add-on operation failed."));
    }
    return elevatedResult;
  }
  probe.close();
#elif defined(Q_OS_MACOS)
  if (!waitForAddonProcesses(root, parentPid, false, error)) return 1;
#else
  Q_UNUSED(parentPid)
#endif
  QLockFile lock(stateDirectory(root) + QStringLiteral("/manager.lock"));
  if (!lock.tryLock(30000)) { fail(error, QStringLiteral("Another add-on operation is active.")); return 2; }
  // A queued worker and a recovery worker may have read the same job before
  // either acquired the lock. Use the current transaction state under the lock.
  if (!QFileInfo::exists(path)) { *alreadyFinished = true; return 0; }
  QJsonObject currentJob;
  if (!readJson(path, &currentJob) ||
      currentJob.value(QStringLiteral("root")).toString() != root ||
      currentJob.value(QStringLiteral("id")).toString() != id ||
      currentJob.value(QStringLiteral("operation")).toString() != operation) {
    fail(error, QStringLiteral("The queued operation changed.")); return 2;
  }
  job = currentJob;
  if (job.value(QStringLiteral("stage")).toString() == QLatin1String("applying") &&
      !restoreJob(root, id, &job, error)) return 2;
  const auto old = installed(root, id);
  if (operation == QLatin1String("uninstall") && old.isEmpty()) {
    fail(error, QStringLiteral("No managed installation was found.")); return 1;
  }
  QJsonArray filesToRemove = old.value(QStringLiteral("files")).toArray();
  if (operation == QLatin1String("manual-uninstall")) {
    filesToRemove = job.value(QStringLiteral("manualFiles")).toArray();
    if (!old.isEmpty() || QFileInfo::exists(recordPath(root, id)) ||
        !validManualFiles(id, filesToRemove, error)) return 1;
  }
  Package package;
  if (operation == QLatin1String("install") &&
      (!readPackage(job.value(QStringLiteral("package")).toString(), &package, error) ||
       package.sha256 != job.value(QStringLiteral("sha256")).toString() ||
       package.manifest.value(QStringLiteral("id")).toString() != id ||
       !validConsent(package, job.value(QStringLiteral("consent")).toObject()))) {
    fail(error, QStringLiteral("The staged package changed or is invalid.")); return 1;
  }
  if (operation == QLatin1String("install") && !old.isEmpty() &&
      compareVersions(old.value(QStringLiteral("manifest")).toObject()
                          .value(QStringLiteral("version")).toString(),
                      package.manifest.value(QStringLiteral("version")).toString()) >= 0) {
    fail(error, QStringLiteral("The installed version is already current or newer.")); return 1;
  }
  QMap<QString, QByteArray> backup;
#ifdef Q_OS_MACOS
  QJsonObject oldPermissions;
#endif
  for (const auto &item : filesToRemove) {
    const auto record = item.toObject();
    const QString relative = record.value(QStringLiteral("path")).toString();
    if (!ownedPath(id, relative) || !safeDestination(root, relative, error) ||
        !fileMatches(installationDirectory(root) + QLatin1Char('/') + relative,
                     record.value(QStringLiteral("sha256")).toString())) {
      fail(error, QStringLiteral("An installed file changed: %1").arg(relative)); return 1;
    }
    QFile file(installationDirectory(root) + QLatin1Char('/') + relative);
    if (!file.open(QIODevice::ReadOnly)) {
      fail(error, QStringLiteral("Could not back up %1.").arg(relative)); return 1;
    }
#ifdef Q_OS_MACOS
    oldPermissions.insert(relative, int(QFileInfo(file).permissions()));
#endif
    backup.insert(relative, file.readAll());
  }
  QSet<QString> newPaths;
  for (const auto &entry : package.files) {
    if (!safeDestination(root, entry.path, error)) return 1;
    newPaths.insert(entry.path);
    if (QFileInfo::exists(installationDirectory(root) + QLatin1Char('/') + entry.path) && !backup.contains(entry.path)) {
      fail(error, QStringLiteral("An unmanaged file appeared: %1").arg(entry.path)); return 1;
    }
  }
  QJsonArray backupNames;
  QJsonArray newNames;
  for (const auto &entry : package.files) newNames.append(entry.path);
  const QFileInfo backupInfo(backupDirectory(root, id));
  if (backupInfo.isSymLink() || backupInfo.isJunction()) {
    fail(error, QStringLiteral("Recovery directory uses a link.")); return 1;
  }
  if (!QDir().mkpath(backupDirectory(root, id))) {
    fail(error, QStringLiteral("Could not prepare recovery files.")); return 1;
  }
  int backupCount = 0;
  for (auto it = backup.cbegin(); it != backup.cend(); ++it) {
    if (!writeBytes(backupDirectory(root, id) + QLatin1Char('/') +
                        QString::number(backupCount), it.value(), error)) {
      clearBackups(root, id, backupCount);
      return 1;
    }
    backupNames.append(it.key());
    ++backupCount;
  }
#ifdef Q_OS_MACOS
  job.insert(QStringLiteral("oldPermissions"), oldPermissions);
#endif
  job.insert(QStringLiteral("stage"), QStringLiteral("applying"));
  job.insert(QStringLiteral("oldRecord"), old);
  job.insert(QStringLiteral("backups"), backupNames);
  job.insert(QStringLiteral("newFiles"), newNames);
  if (!writeBytes(path, QJsonDocument(job).toJson(), error)) {
    clearBackups(root, id, backupCount);
    return 1;
  }
  bool applied = true;
  for (auto it = backup.cbegin(); it != backup.cend(); ++it) {
    if (!QFile::remove(installationDirectory(root) + QLatin1Char('/') + it.key())) { applied = false; break; }
  }
  if (applied) for (const auto &entry : package.files) {
    if (!writeBytes(installationDirectory(root) + QLatin1Char('/') + entry.path, entry.contents, error)) {
      applied = false; break;
    }
  }
#ifdef Q_OS_MACOS
  if (applied) for (const auto &entry : package.files) {
    const QString destination = installationDirectory(root) + QLatin1Char('/') + entry.path;
    const bool binary = entry.path == entryPath(id) || entry.path.endsWith(QLatin1String(".dylib")) ||
        (entry.contents.size() >= 4 && (entry.contents.left(4) == QByteArray::fromHex("cffaedfe") ||
         entry.contents.left(4) == QByteArray::fromHex("cafebabe") || entry.contents.left(4) == QByteArray::fromHex("cafebabf")));
    const auto permissions = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther |
        (binary ? (QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther) : QFile::Permissions{});
    if (!QFile::setPermissions(destination, permissions) || (binary && !verifyInstalledBinary(destination, error))) {
      applied = false; break;
    }
  }
#endif
  if (applied) {
    if (operation == QLatin1String("install")) {
      QJsonArray files;
      for (const auto &entry : package.files)
        files.append(QJsonObject{{QStringLiteral("path"), entry.path},
                                 {QStringLiteral("sha256"), entry.sha256}});
      QJsonArray notices;
      for (const auto &entry : package.notices)
        notices.append(QJsonObject{{QStringLiteral("path"), entry.path},
                                   {QStringLiteral("contentsBase64"),
                                    QString::fromLatin1(entry.contents.toBase64())}});
      QJsonArray agreements;
      for (const auto &entry : package.agreements)
        agreements.append(QJsonObject{{QStringLiteral("path"), entry.path},
                                      {QStringLiteral("sha256"), entry.sha256},
                                      {QStringLiteral("contentsBase64"),
                                       QString::fromLatin1(entry.contents.toBase64())}});
      const QJsonObject record{{QStringLiteral("root"), root},
                               {QStringLiteral("manifest"), package.manifest},
                               {QStringLiteral("sha256"), package.sha256},
                               {QStringLiteral("files"), files},
                               {QStringLiteral("readme"), QString::fromUtf8(package.readme)},
                               {QStringLiteral("agreements"), agreements},
                               {QStringLiteral("consent"), job.value(QStringLiteral("consent"))},
                               {QStringLiteral("notices"), notices}};
      applied = writeBytes(recordPath(root, id), QJsonDocument(record).toJson(), error);
    } else if (operation == QLatin1String("uninstall")) {
      applied = QFile::remove(recordPath(root, id));
    }
  }
  if (!applied) {
    if (!restoreJob(root, id, &job, error)) {
      fail(error, QStringLiteral("Recovery is required before starting VTR-PON."));
      return 2;
    }
    clearBackups(root, id, backupCount);
    return 1;
  }
  // Remove only empty directories below this add-on's placement folder.
  QSet<QString> oldDirectories;
  for (auto it = backup.cbegin(); it != backup.cend(); ++it) {
    QString directory = it.key().section(QLatin1Char('/'), 0, -2);
    const QString placement = entryPath(id).section(QLatin1Char('/'), 0, 0);
    while (directory.startsWith(placement + QLatin1Char('/'))) {
      oldDirectories.insert(directory);
      directory = directory.section(QLatin1Char('/'), 0, -2);
    }
  }
  QStringList directories = oldDirectories.values();
  std::sort(directories.begin(), directories.end(), [](const QString &a, const QString &b) {
    return a.size() > b.size();
  });
  for (const auto &directory : directories) QDir(installationDirectory(root)).rmdir(directory);
  QFile::remove(path);
  if (operation == QLatin1String("install")) QFile::remove(job.value(QStringLiteral("package")).toString());
  clearBackups(root, id, backupCount);
  return 0;
}

int applyJob(const QString &path, qint64 parentPid, QString *error) {
  if (!platformFeatureImplemented(PlatformFeature::AddonManagement)) {
    fail(error, QStringLiteral("Add-on operations are not implemented on this platform."));
    return 1;
  }
  QJsonObject job;
  const bool hadJob = readJson(path, &job);
  if (!hadJob && !QFileInfo::exists(path)) return 0;
  if (hadJob) {
    const QString root = canonicalRoot(job.value(QStringLiteral("root")).toString());
    const QString id = job.value(QStringLiteral("id")).toString();
    const QFileInfo jobInfo(path);
    const QDir stateDir = jobInfo.absoluteDir();
    QDir stateParent = stateDir;
    stateParent.cdUp();
    QDir stateGrandparent = stateParent;
    stateGrandparent.cdUp();
    const QString expectedKey = rootKey(root);
    if (root.isEmpty() || !ids().contains(id) ||
        jobInfo.fileName() != id + QStringLiteral(".job.json") ||
        stateDir.dirName() != expectedKey ||
        QFileInfo(stateDir.absolutePath()).isSymLink() ||
        QFileInfo(stateDir.absolutePath()).isJunction() ||
        QFileInfo(stateParent.absolutePath()).isSymLink() ||
        QFileInfo(stateParent.absolutePath()).isJunction() ||
        stateParent.dirName() != QLatin1String("addon-manager") ||
        stateGrandparent.dirName() != currentProductStorageName() ||
        jobInfo.isSymLink() || jobInfo.isJunction()) {
      fail(error, QStringLiteral("Invalid add-on manager state directory."));
      return 1;
    }
    stateOverride = stateDir.absolutePath();
  }
  bool alreadyFinished = false;
  const int result = applyJobImpl(path, parentPid, error, &alreadyFinished);
  if (alreadyFinished) { stateOverride.clear(); return result; }
  if (!hadJob) return result;
  const QString root = job.value(QStringLiteral("root")).toString();
  const QString id = job.value(QStringLiteral("id")).toString();
  if (!ids().contains(id) || canonicalRoot(root) != root ||
      path != jobPath(root, id)) {
    stateOverride.clear();
    return result;
  }
  const QString status = stateDirectory(root) + QLatin1Char('/') + id +
                         QStringLiteral(".error.txt");
  if (result == 0) {
    QFile::remove(status);
  } else if (result == 1) {
    QJsonObject current;
    readJson(path, &current);
    if (current.value(QStringLiteral("stage")).toString() ==
        QLatin1String("applying")) {
      stateOverride.clear();
      return result;
    }
    writeBytes(status, error ? error->toUtf8() : QByteArray(), nullptr);
    clearBackups(root, id, current.value(QStringLiteral("backups")).toArray().size());
    QFile::remove(path);
    QFile::remove(job.value(QStringLiteral("package")).toString());
  }
  stateOverride.clear();
  return result;
}

} // namespace vtrpon::addons
