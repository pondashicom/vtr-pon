#include "addon_platform.h"
#include "addon_package.h"
#include "product_identity.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <algorithm>
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif

namespace vtrpon::addons {
namespace {
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
bool version(const QString &text) {
  static const QRegularExpression re(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
  return re.match(text).hasMatch();
}
bool sha(const QString &text) {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
  return re.match(text).hasMatch();
}
#ifdef Q_OS_MACOS
bool thinNative(const QByteArray &bytes) {
  if (bytes.size() < 32 || qFromLittleEndian<quint32>(bytes.constData()) != 0xfeedfacf) return false;
#ifdef Q_PROCESSOR_ARM_64
  constexpr quint32 cpu = 0x0100000c;
#else
  constexpr quint32 cpu = 0x01000007;
#endif
  const quint32 type = qFromLittleEndian<quint32>(bytes.constData() + 12);
  return qFromLittleEndian<quint32>(bytes.constData() + 4) == cpu && (type == 2 || type == 6 || type == 8);
}
bool nativeBinary(const QByteArray &bytes) {
  if (thinNative(bytes)) return true;
  if (bytes.size() < 8) return false;
  const quint32 magic = qFromBigEndian<quint32>(bytes.constData());
  if (magic != 0xcafebabe && magic != 0xcafebabf) return false;
  const quint32 count = qFromBigEndian<quint32>(bytes.constData() + 4);
  const int stride = magic == 0xcafebabf ? 32 : 20;
  if (!count || count > 32 || bytes.size() < 8 + count * stride) return false;
  for (quint32 i = 0; i < count; ++i) {
    const char *arch = bytes.constData() + 8 + i * stride;
    const quint64 offset = stride == 32 ? qFromBigEndian<quint64>(arch + 8) : qFromBigEndian<quint32>(arch + 8);
    const quint64 size = stride == 32 ? qFromBigEndian<quint64>(arch + 16) : qFromBigEndian<quint32>(arch + 12);
    if (offset > quint64(bytes.size()) || size > quint64(bytes.size()) - offset) return false;
    if (thinNative(bytes.mid(qsizetype(offset), qsizetype(size)))) return true;
  }
  return false;
}
bool codesign(const QStringList &args, QByteArray *output = nullptr) {
  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start(QStringLiteral("/usr/bin/codesign"), args);
  if (!process.waitForStarted(1000) || !process.waitForFinished(3000)) {
    process.kill(); process.waitForFinished(1000); return false;
  }
  if (output) *output = process.readAll().left(16384);
  return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
QString team(const QString &file) {
  QByteArray output;
  if (!codesign({QStringLiteral("-dv"), QStringLiteral("--verbose=4"), file}, &output)) return {};
  for (const auto &line : output.split('\n'))
    if (line.startsWith("TeamIdentifier=") && line != "TeamIdentifier=not set")
      return QString::fromLatin1(line.mid(15)).trimmed();
  return {};
}
#endif
}
QString platformId() {
#ifdef Q_OS_MACOS
#ifdef Q_PROCESSOR_ARM_64
  return QStringLiteral("macos-arm64");
#elif defined(Q_PROCESSOR_X86_64)
  return QStringLiteral("macos-x64");
#else
  return QStringLiteral("macos-unsupported");
#endif
#else
  return QStringLiteral("windows-x64");
#endif
}
QString rootKey(const QString &root) {
  QString canonical = QFileInfo(root).canonicalFilePath();
#ifndef Q_OS_MACOS
  canonical = canonical.toCaseFolded();
#endif
  return QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString applicationExecutable(const QString &root) {
#ifdef Q_OS_MACOS
  return QDir(root).filePath(QStringLiteral("vtr-pon"));
#else
  return QDir(root).filePath(QStringLiteral("vtr-pon.exe"));
#endif
}
QString managerExecutable(const QString &root) {
#ifdef Q_OS_MACOS
  return QDir(root).filePath(QStringLiteral("vtr-pon-addon-manager"));
#else
  return QDir(root).filePath(QStringLiteral("vtr-pon-addon-manager.exe"));
#endif
}
QString installationDirectory(const QString &root) {
#ifdef Q_OS_MACOS
  return QDir::cleanPath(QDir(stateDirectory(root)).absoluteFilePath(QStringLiteral("../../addons/") + rootKey(root)));
#else
  return root;
#endif
}
QString addonFilePath(const QString &root, const QString &id) {
  const QString entry = entryPath(id);
  return entry.isEmpty() ? QString() : QDir(installationDirectory(root)).filePath(entry);
}
bool readCatalog(const QByteArray &data, QVector<CatalogEntry> *entries, QString *error) {
  if (!entries) return fail(error, QStringLiteral("No catalog destination."));
  entries->clear();
  const auto doc = QJsonDocument::fromJson(data);
  if (data.size() > 65536 || !doc.isObject() || !hasUniqueJsonKeys(data))
    return fail(error, QStringLiteral("Invalid add-on catalog."));
  const auto catalog = doc.object();
  const int format = catalog.value(QStringLiteral("formatVersion")).toInt(-1);
  const auto packages = catalog.value(QStringLiteral("packages")).toArray();
  if (catalog.size() != 2 || (format != 1 && format != 2) ||
      !catalog.value(QStringLiteral("packages")).isArray() || packages.size() > 32)
    return fail(error, QStringLiteral("Unsupported add-on catalog."));
  const QStringList knownIds{QStringLiteral("vtr-pon-atem-addon"), QStringLiteral("vtr-pon-network-video-addon"),
      QStringLiteral("vtr-pon-vst3-addon"), QStringLiteral("vtr-pon-decklink-addon")};
  const QStringList platforms{QStringLiteral("windows-x64"), QStringLiteral("macos-arm64"), QStringLiteral("macos-x64")};
  QSet<QString> seen;
  QVector<CatalogEntry> selected;
  for (const auto &value : packages) {
    const auto item = value.toObject();
    const QString id = item.value(QStringLiteral("id")).toString();
    const QString v = item.value(QStringLiteral("version")).toString();
    const QString digest = item.value(QStringLiteral("sha256")).toString();
    const QString platform = format == 1 ? QStringLiteral("windows-x64") : item.value(QStringLiteral("platform")).toString();
    const QUrl url(item.value(QStringLiteral("url")).toString());
    const QString key = id + QLatin1Char('/') + platform;
    if (item.size() != (format == 1 ? 4 : 5) || !knownIds.contains(id) ||
        !platforms.contains(platform) || seen.contains(key) || !version(v) || !sha(digest) ||
        !url.isValid() || url.scheme() != QLatin1String("https") || url.host() != QLatin1String("pondashi.com") ||
        url.port(-1) != -1 || !url.userInfo().isEmpty() || !url.query().isEmpty() || !url.fragment().isEmpty() ||
        url.path(QUrl::FullyEncoded) != QStringLiteral("/vtr-pon/addon/download/") + id + QLatin1Char('-') + v +
          QLatin1Char('-') + platform + QStringLiteral(".vtrponaddon") ||
        (id == QLatin1String("vtr-pon-decklink-addon") && platform != QLatin1String("windows-x64") && platform != QLatin1String("macos-arm64")))
      return fail(error, QStringLiteral("Invalid add-on catalog entry."));
    seen.insert(key);
    if (platform == platformId() && ids().contains(id)) selected.append({id, v, digest, url.toString()});
  }
  *entries = selected;
  return true;
}
bool validatePlatformPayload(const QString &path, const QByteArray &bytes, QString *error) {
#ifdef Q_OS_MACOS
  const auto candidates = ids();
  const bool entry = std::any_of(candidates.cbegin(), candidates.cend(), [&path](const QString &id) { return entryPath(id) == path; });
  if (path.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive))
    return fail(error, QStringLiteral("Windows add-on binaries cannot be installed on Mac."));
  const bool mach = bytes.size() >= 4 && (qFromLittleEndian<quint32>(bytes.constData()) == 0xfeedfacf ||
      qFromBigEndian<quint32>(bytes.constData()) == 0xcafebabe || qFromBigEndian<quint32>(bytes.constData()) == 0xcafebabf);
  if ((entry || mach || path.endsWith(QLatin1String(".dylib"))) && !nativeBinary(bytes))
    return fail(error, QStringLiteral("The add-on binary does not support this Mac's CPU."));
#else
  Q_UNUSED(path) Q_UNUSED(bytes) Q_UNUSED(error)
#endif
  return true;
}
bool verifyInstalledBinary(const QString &path, QString *error) {
#ifdef Q_OS_MACOS
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 400 * 1024 * 1024 || !nativeBinary(file.readAll()))
    return fail(error, QStringLiteral("The add-on binary does not support this Mac's CPU."));
  if (!codesign({QStringLiteral("--verify"), QStringLiteral("--strict"), path}))
    return fail(error, QStringLiteral("The add-on code signature is missing or invalid."));
  const QString hostTeam = team(QCoreApplication::applicationFilePath());
  if (!hostTeam.isEmpty() && team(path) != hostTeam)
    return fail(error, QStringLiteral("The add-on must be signed by the VTR-PON publisher."));
#else
  Q_UNUSED(path) Q_UNUSED(error)
#endif
  return true;
}
#ifdef Q_OS_MACOS
bool waitForAddonProcesses(const QString &root, qint64 parentPid, bool managers, QString *error) {
  if (parentPid == QCoreApplication::applicationPid()) return fail(error, QStringLiteral("Cannot wait for the manager itself."));
  const auto pathFor = [](pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE]{};
    return proc_pidpath(pid, path, sizeof(path)) > 0 ? QString::fromUtf8(path) : QString();
  };
  const QString parentPath = parentPid > 0 ? pathFor(pid_t(parentPid)) : QString();
  while (parentPid > 0 && (kill(pid_t(parentPid), 0) == 0 || errno == EPERM)) {
    const QString current = pathFor(pid_t(parentPid));
    if (!parentPath.isEmpty() && !current.isEmpty() && current != parentPath) break;
    QThread::msleep(50);
  }
  QString prefix = QFileInfo(root).canonicalFilePath() + QLatin1Char('/');
  if (root.endsWith(QLatin1String("/Contents/MacOS"))) prefix = QDir(root).absoluteFilePath(QStringLiteral("../"));
  prefix = QDir::cleanPath(prefix) + QLatin1Char('/');
  for (;;) {
    const int bytes = proc_listpids(PROC_UID_ONLY, getuid(), nullptr, 0);
    if (bytes <= 0) return fail(error, QStringLiteral("Could not inspect running VTR-PON processes."));
    QVector<pid_t> pids(bytes / int(sizeof(pid_t)) + 64);
    const int actual = proc_listpids(PROC_UID_ONLY, getuid(), pids.data(), int(pids.size() * sizeof(pid_t)));
    if (actual <= 0) return fail(error, QStringLiteral("Could not inspect running VTR-PON processes."));
    bool running = false;
    for (int i = 0; i < actual / int(sizeof(pid_t)); ++i) {
      if (pids[i] <= 0 || pids[i] == getpid()) continue;
      const QString path = pathFor(pids[i]);
      const bool manager = path == managerExecutable(root);
      if (managers ? manager : (path.startsWith(prefix) && !manager)) { running = true; break; }
    }
    if (!running) return true;
    QThread::msleep(50);
  }
}
#endif
}
