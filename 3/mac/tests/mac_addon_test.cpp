#include "addon_package.h"
#include "product_identity.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <zlib.h>
#include <cstdio>
#include <functional>
using namespace vtrpon::addons;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s; %s\n", __LINE__, #x, qPrintable(error)); return 1; } } while (false)
namespace {
void u16(QByteArray *out, quint16 value) {
  out->append(char(value & 255)); out->append(char((value >> 8) & 255));
}
void u32(QByteArray *out, quint32 value) {
  u16(out, quint16(value & 65535)); u16(out, quint16(value >> 16));
}
bool write(const QString &path, const QByteArray &data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray archive(const QList<QPair<QByteArray, QByteArray>> &entries) {
  QByteArray data, central;
  for (const auto &[name, contents] : entries) {
    const auto offset = quint32(data.size());
    const auto crc = crc32(0, reinterpret_cast<const Bytef *>(contents.constData()),
                           uInt(contents.size()));
    u32(&data, 0x04034b50); u16(&data, 20); u16(&data, 0); u16(&data, 0);
    u16(&data, 0); u16(&data, 0); u32(&data, crc);
    u32(&data, contents.size()); u32(&data, contents.size());
    u16(&data, name.size()); u16(&data, 0); data += name; data += contents;
    u32(&central, 0x02014b50); u16(&central, 20); u16(&central, 20);
    u16(&central, 0); u16(&central, 0); u16(&central, 0); u16(&central, 0);
    u32(&central, crc); u32(&central, contents.size()); u32(&central, contents.size());
    u16(&central, name.size()); u16(&central, 0); u16(&central, 0);
    u16(&central, 0); u16(&central, 0); u32(&central, 0); u32(&central, offset);
    central += name;
  }
  const auto start = quint32(data.size());
  data += central;
  u32(&data, 0x06054b50); u16(&data, 0); u16(&data, 0);
  u16(&data, entries.size()); u16(&data, entries.size());
  u32(&data, central.size()); u32(&data, start); u16(&data, 0);
  return data;
}

QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QString digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QByteArray package(const QByteArray &binary, const QString &v = "1.0.0", const QString &platform = platformId(), const QString &path = {}, const QString &id = "vtr-pon-atem-addon") {
  const QByteArray agreement("Fixture agreement.\n");
  const QJsonObject manifest{{"formatVersion", 2}, {"platform", platform}, {"id", id},
    {"version", v}, {"contract", contract(id)}, {"runtimeStatus", 1},
    {"agreements", QJsonArray{QJsonObject{{"path", "licenses/AGREEMENT.txt"}, {"sha256", digest(agreement)}}}}};
  return archive({{"manifest.json", QJsonDocument(manifest).toJson()}, {"README.md", "Fixture only"},
    {"licenses/AGREEMENT.txt", agreement}, {(path.isEmpty() ? "payload/" + entryPath(id) : path).toUtf8(), binary}});
}
bool stage(const QString &root, const QString &path, const QString &operation, QString *error, const QString &id = "vtr-pon-atem-addon") {
  QDir().mkpath(stateDirectory(root));
  QJsonObject job{{"root", QFileInfo(root).canonicalFilePath()}, {"id", id}, {"operation", operation}};
  if (operation == "install") {
    Package parsed;
    if (!readPackage(path, &parsed, error)) return false;
    const QString staged = stateDirectory(root) + '/' + id + ".vtrponaddon";
    if (!write(staged, read(path))) return false;
    job.insert("package", staged); job.insert("sha256", parsed.sha256); job.insert("consent", consentReceipt(parsed));
  }
  return write(stateDirectory(root) + '/' + id + ".job.json", QJsonDocument(job).toJson());
}
int apply(const QString &root, QString *error, const QString &id = "vtr-pon-atem-addon") { return applyJob(stateDirectory(root) + '/' + id + ".job.json", 0, error); }
bool wait(const std::function<bool()> &predicate, int timeout = 10000) {
  QElapsedTimer elapsed; elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(20); }
  return predicate();
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv); vtrpon::initializeProductIdentity();
  const auto args = app.arguments();
  QString error;
  if (args.value(1) == "--verify-package") {
    CHECK(args.size() == 3);
    QStandardPaths::setTestModeEnabled(true);
    Package parsed; CHECK(readPackage(args.at(2), &parsed, &error));
    const QString id = parsed.manifest.value("id").toString();
    QTemporaryDir temporary("/private/tmp/vtrpon-real-package-XXXXXX"); CHECK(temporary.isValid());
    const QString root = temporary.path();
    const auto cleanup = qScopeGuard([&]{ QDir(stateDirectory(root)).removeRecursively(); QDir(installationDirectory(root)).removeRecursively(); });
    CHECK(QFile::copy(app.applicationFilePath(), applicationExecutable(root)));
    CHECK(stage(root, args.at(2), "install", &error, id) && apply(root, &error, id) == 0);
    CHECK(verifyInstalledBinary(addonFilePath(root, id), &error));
    CHECK(stage(root, args.at(2), "uninstall", &error, id) && apply(root, &error, id) == 0);
    CHECK(!QFileInfo::exists(addonFilePath(root, id)));
    std::printf("Real package native payload/signature/install/remove PASS: %s %s\n", qPrintable(id), qPrintable(parsed.sha256));
    return 0;
  }
  if (args.value(1) == "--restart") return 2; // Old helper fixture: no ready acknowledgement.
  if (args.value(1) == "--restart-marker") {
    CHECK(write(args.at(2), QJsonDocument(QJsonObject{{"arguments", QJsonArray::fromStringList(args.mid(1))}, {"cwd", QDir::currentPath()},
      {"installed", !installed(app.applicationDirPath(), "vtr-pon-atem-addon").isEmpty()}}).toJson())); return 0;
  }
  if (args.value(1) == "--hold-copy") {
    CHECK(write(args.at(2), "holding"));
    CHECK(wait([&]{ return QFileInfo::exists(args.at(3)); })); return 0;
  }
  const bool processes = args.value(1) == "--processes";
  QStandardPaths::setTestModeEnabled(!processes && args.value(1) != "--restart-worker");
  if (args.value(1) == "--queue-worker" || args.value(1) == "--restart-worker") {
    const QString root = args.at(2), pkg = args.at(3); Package parsed;
    CHECK(readPackage(pkg, &parsed, &error));
    CHECK(queueInstall(root, pkg, {}, {}, {}, consentReceipt(parsed), &error));
    CHECK(write(root + "/queued", QByteArray::number(app.applicationPid())));
    if (args.value(1) == "--restart-worker") {
      CHECK(requestRestart(root, {"--restart-marker", root + "/restarted", "argument with spaces", "--addon-restart-error=obsolete"}, root, &error));
      CHECK(requestRestart(root, {"--restart-marker", root + "/restarted"}, root, &error));
      CHECK(!QFileInfo::exists(root + "/restarted"));
    }
    CHECK(wait([&]{ return QFileInfo::exists(root + "/release"); })); return 0;
  }
  CHECK(args.size() == 5);
  const QString fixture = args.at(2), probe = args.at(3), manager = args.at(4);
  const QByteArray binary = read(fixture); CHECK(!binary.isEmpty());
  QTemporaryDir temporary("/private/tmp/vtrpon-mac-addon-XXXXXX"); CHECK(temporary.isValid());
  const QString root = temporary.path();
  CHECK(QFile::copy(app.applicationFilePath(), applicationExecutable(root)) && QFile::copy(manager, managerExecutable(root)));
  const auto cleanup = qScopeGuard([&]{ QDir(stateDirectory(root)).removeRecursively(); QDir(installationDirectory(root)).removeRecursively(); });
  const QString pkg = root + "/fixture.vtrponaddon";
  const QString id("vtr-pon-atem-addon"), target = addonFilePath(root, id);
  if (processes) {
    // The worker runs from the copied Controller so process-path matching is exercised.
    QProcess worker, otherCopy;
    otherCopy.start(applicationExecutable(root), {"--hold-copy", root + "/other-holding", root + "/release-other"});
    CHECK(otherCopy.waitForStarted() && wait([&]{ return QFileInfo::exists(root + "/other-holding"); }));
    CHECK(write(pkg, package(binary)));
    worker.start(applicationExecutable(root), {"--restart-worker", root, pkg});
    CHECK(worker.waitForStarted() && wait([&]{ return QFileInfo::exists(root + "/queued"); }));
    QThread::msleep(250); QCoreApplication::processEvents();
    CHECK(!QFileInfo::exists(target) && !QFileInfo::exists(root + "/restarted") && pendingJobs(root).size() == 1);
    CHECK(write(root + "/release", "exit")); CHECK(worker.waitForFinished(10000) && worker.exitCode() == 0);
    QThread::msleep(250);
    CHECK(!QFileInfo::exists(target) && !QFileInfo::exists(root + "/restarted"));
    CHECK(write(root + "/release-other", "exit") && otherCopy.waitForFinished(10000) && otherCopy.exitCode() == 0);
    CHECK(wait([&]{ return QFileInfo::exists(root + "/restarted"); }));
    const auto restarted = QJsonDocument::fromJson(read(root + "/restarted")).object();
    CHECK(restarted.value("installed").toBool() && restarted.value("cwd").toString() == root);
    CHECK(restarted.value("arguments").toArray().contains("argument with spaces"));
    CHECK(!restarted.value("arguments").toArray().contains("--addon-restart-error=obsolete"));
    CHECK(pendingJobs(root).isEmpty() && verifyInstalledBinary(target, &error));
    // An older helper must leave the caller running instead of restarting later.
    CHECK(wait([&]{ return !restartInProgress(root); }));
    CHECK(QFile::remove(managerExecutable(root)) && QFile::copy(app.applicationFilePath(), managerExecutable(root)));
    CHECK(!requestRestart(root, {}, root, &error));
    std::puts("Mac add-on process wait/restart/handshake: PASS"); return 0;
  }
  CHECK(ids().size() == 4 && ids().contains("vtr-pon-decklink-addon"));
  const QString decklinkId = "vtr-pon-decklink-addon";
  CHECK(entryPath(decklinkId) == "decklink-addons/vtr-pon-decklink-addon.dylib");
  CHECK(contract(decklinkId) == "decklink-media-abi-v1");
  Package decklinkPackage;
  CHECK(write(pkg, package(binary, "0.3.0", platformId(), {}, decklinkId)) && readPackage(pkg, &decklinkPackage, &error));
  CHECK(stage(root, pkg, "install", &error, decklinkId) && apply(root, &error, decklinkId) == 0);
  CHECK(read(addonFilePath(root, decklinkId)) == binary);
  CHECK(stage(root, pkg, "uninstall", &error, decklinkId) && apply(root, &error, decklinkId) == 0);
  CHECK(!QFileInfo::exists(addonFilePath(root, decklinkId)));

  CHECK(!installationDirectory(root).startsWith(root + '/') && !entryPath(id).endsWith(".dll"));
  Package parsed;
  CHECK(write(pkg, package(binary)) && readPackage(pkg, &parsed, &error));
  CHECK(!queueInstall(root, pkg, {}, {}, {}, {}, &error) && pendingJobs(root).isEmpty());
  for (const auto &platform : {QString("windows-x64"), platformId() == "macos-arm64" ? QString("macos-x64") : QString("macos-arm64")})
    CHECK(write(pkg, package(binary, "1.0.0", platform)) && !readPackage(pkg, &parsed, &error));
  CHECK(write(pkg, package("MZ Windows binary")) && !readPackage(pkg, &parsed, &error));
  CHECK(write(pkg, package(binary, "1.0.0", platformId(), "payload/../escape")) && !readPackage(pkg, &parsed, &error));
  auto corrupt = package(binary); corrupt[50] = char(corrupt.at(50) ^ 1);
  CHECK(write(pkg, corrupt) && !readPackage(pkg, &parsed, &error));
  const auto entry = [&](const QString &platform) { return QJsonObject{{"id", id}, {"version", "1.0.0"}, {"platform", platform},
    {"sha256", digest(binary)}, {"url", "https://pondashi.com/vtr-pon/addon/download/" + id + "-1.0.0-" + platform + ".vtrponaddon"}}; };
  QVector<CatalogEntry> selected;
  const auto catalog = [&](const QJsonArray &entries) { return QJsonDocument(QJsonObject{{"formatVersion", 2}, {"packages", entries}}).toJson(); };
  CHECK(readCatalog(catalog({entry("windows-x64"), entry(platformId())}), &selected, &error) && selected.size() == 1);
  CHECK(!readCatalog(catalog({entry(platformId()), entry(platformId())}), &selected, &error) && selected.isEmpty());
  auto decklinkCatalog = entry(platformId()); decklinkCatalog.insert("id", decklinkId);
  decklinkCatalog.insert("url", "https://pondashi.com/vtr-pon/addon/download/" + decklinkId + "-1.0.0-" + platformId() + ".vtrponaddon");
  CHECK(readCatalog(catalog({decklinkCatalog}), &selected, &error) && selected.size() == 1 && selected.front().id == decklinkId);

  auto foreign = entry(platformId()); foreign.insert("url", "https://example.com/foreign");
  CHECK(!readCatalog(catalog({foreign}), &selected, &error));
  auto legacy = entry("windows-x64"); legacy.remove("platform");
  CHECK(readCatalog(QJsonDocument(QJsonObject{{"formatVersion", 1}, {"packages", QJsonArray{legacy}}}).toJson(), &selected, &error) && selected.isEmpty());
  const auto applicationBefore = digest(read(applicationExecutable(root)));
  CHECK(write(pkg, package(binary)) && stage(root, pkg, "install", &error) && apply(root, &error) == 0);
  CHECK(read(target) == binary && verifyInstalledBinary(target, &error) && QFileInfo(target).isExecutable());
  CHECK(digest(read(applicationExecutable(root))) == applicationBefore);
  const auto oldRecord = installed(root, id); const auto oldMode = QFileInfo(target).permissions();
  // A signature failure occurs after writing the update: retain the previous bytes, receipt and mode.
  auto badSignature = binary; badSignature[4096] = char(badSignature.at(4096) ^ 1);
  CHECK(write(pkg, package(badSignature, "1.1.0")) && stage(root, pkg, "install", &error) && apply(root, &error) != 0);
  CHECK(read(target) == binary && installed(root, id) == oldRecord && QFileInfo(target).permissions() == oldMode);
  CHECK(write(pkg, package(binary, "1.1.0")) && stage(root, pkg, "install", &error));
  // Emulate interruption after removal/writing began; the next helper must restore then retry.
  const QString jobPath = stateDirectory(root) + '/' + id + ".job.json";
  auto interrupted = QJsonDocument::fromJson(read(jobPath)).object();
  CHECK(QDir().mkpath(stateDirectory(root) + '/' + id + ".backup"));
  CHECK(write(stateDirectory(root) + '/' + id + ".backup/0", binary));
  interrupted.insert("stage", "applying"); interrupted.insert("oldRecord", oldRecord);
  interrupted.insert("backups", QJsonArray{entryPath(id)}); interrupted.insert("newFiles", QJsonArray{entryPath(id)});
  interrupted.insert("oldPermissions", QJsonObject{{entryPath(id), int(oldMode)}});
  CHECK(write(target, "interrupted partial file") && write(jobPath, QJsonDocument(interrupted).toJson()));
  CHECK(apply(root, &error) == 0 && verifyInstalledBinary(target, &error));
  CHECK(installed(root, id).value("manifest").toObject().value("version") == "1.1.0");
  QProcess diagnosis; diagnosis.start(probe, {"--kind", "atem", "--addon-file", target});
  CHECK(diagnosis.waitForFinished(10000) && QJsonDocument::fromJson(diagnosis.readAllStandardOutput()).object().value("status") == "ready");
  CHECK(write(target + ".unowned", "keep"));
  CHECK(stage(root, {}, "uninstall", &error) && apply(root, &error) == 0);
  CHECK(!QFileInfo::exists(target) && QFileInfo::exists(target + ".unowned") && installed(root, id).isEmpty());
  for (const auto &otherId : {QString("vtr-pon-network-video-addon"), QString("vtr-pon-vst3-addon")}) {
    auto manifest = parsed.manifest; manifest.insert("id", otherId); manifest.insert("contract", contract(otherId));
    if (otherId == "vtr-pon-vst3-addon") manifest.remove("runtimeStatus");
    QList<QPair<QByteArray,QByteArray>> entries{{"manifest.json", QJsonDocument(manifest).toJson()},
      {"README.md", "Fixture"}, {"licenses/AGREEMENT.txt", "Fixture agreement.\n"}};
    if (otherId == "vtr-pon-vst3-addon") {
      // A Mac executable inside .app must carry its signed bundle metadata.
      // A bare Mach-O moved under Contents/MacOS is not a valid app signature.
      QTemporaryDir bundleRoot; CHECK(bundleRoot.isValid());
      const auto executable = bundleRoot.filePath(entryPath(otherId));
      CHECK(QDir().mkpath(QFileInfo(executable).absolutePath()) && QFile::copy(app.applicationFilePath(),executable));
      const auto bundle = bundleRoot.filePath("audio-addons/vtr-pon-vst3-addon.app");
      CHECK(write(bundle + "/Contents/Info.plist", QByteArrayLiteral(
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>CFBundleExecutable</key><string>vtr-pon-vst3-addon</string>"
        "<key>CFBundleIdentifier</key><string>com.pondashi.vtrpon.test-vst3</string>"
        "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>")));
      QProcess signer; signer.start("/usr/bin/codesign", {"--force","--sign","-","--timestamp=none",bundle});
      CHECK(signer.waitForFinished(10000) && signer.exitCode()==0);
      QDirIterator files(bundleRoot.path(),QDir::Files,QDirIterator::Subdirectories);
      while(files.hasNext()) { const auto file=files.next(); entries.append({("payload/"+QDir(bundleRoot.path()).relativeFilePath(file)).toUtf8(),read(file)}); }
    } else entries.append({("payload/"+entryPath(otherId)).toUtf8(),binary});
    CHECK(write(pkg, archive(entries)));
    CHECK(stage(root, pkg, "install", &error, otherId) && apply(root, &error, otherId) == 0);
    CHECK(QFileInfo(addonFilePath(root, otherId)).isExecutable() && verifyInstalledBinary(addonFilePath(root, otherId), &error));
    CHECK(stage(root, {}, "uninstall", &error, otherId) && apply(root, &error, otherId) == 0);
  }
  QDir(installationDirectory(root)).removeRecursively();
  CHECK(QDir().mkpath(installationDirectory(root)) && QFile::link(root, installationDirectory(root) + "/switcher-addons"));
  CHECK(write(pkg, package(binary)) && !queueInstall(root, pkg, {}, {}, {}, consentReceipt(parsed), &error));
  std::puts("Mac add-on package/catalog/CPU/signature/rollback/removal: PASS"); return 0;
}
