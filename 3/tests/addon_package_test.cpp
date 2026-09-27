#include "addon_package.h"
#include "product_identity.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <zlib.h>
#include <cstdio>

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
QByteArray manifest(const QString &contract) {
  const QByteArray agreement = QByteArrayLiteral("Test agreement for this add-on.\n");
  const QString digest = QString::fromLatin1(
      QCryptographicHash::hash(agreement, QCryptographicHash::Sha256).toHex());
  return QJsonDocument(QJsonObject{
      {QStringLiteral("formatVersion"), 2},
      {QStringLiteral("id"), QStringLiteral("vtr-pon-atem-addon")},
      {QStringLiteral("version"), QStringLiteral("1.0.0")},
      {QStringLiteral("contract"), contract},
      {QStringLiteral("agreements"), QJsonArray{QJsonObject{
          {QStringLiteral("path"), QStringLiteral("licenses/AGREEMENT.txt")},
          {QStringLiteral("sha256"), digest}}}},
      {QStringLiteral("runtimeStatus"), 1}}).toJson();
}
QByteArray package(const QString &entryName, const QString &contract) {
  return archive({{QByteArrayLiteral("manifest.json"), manifest(contract)},
                  {QByteArrayLiteral("README.md"), QByteArrayLiteral("Test package")},
                  {QByteArrayLiteral("licenses/AGREEMENT.txt"),
                   QByteArrayLiteral("Test agreement for this add-on.\n")},
                  {entryName.toUtf8(), QByteArrayLiteral("dummy dll")}});
}
bool check(bool result, const char *message) {
  if (!result) qCritical("%s", message);
  return result;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QStandardPaths::setTestModeEnabled(true);
  if (app.arguments().size() == 3 &&
      app.arguments().at(1) == QLatin1String("--install-smoke")) {
    vtrpon::addons::Package parsed;
    QString error;
    if (!check(vtrpon::addons::readPackage(app.arguments().at(2), &parsed, &error),
               "package parse failed")) return 1;
    QTemporaryDir temporary;
    if (!check(temporary.isValid() &&
               write(temporary.path() + QStringLiteral("/vtr-pon.exe"), {}),
               "temporary install root")) return 1;
    const QString root = temporary.path();
    const QString id = parsed.manifest.value(QStringLiteral("id")).toString();
    const QString state = vtrpon::addons::stateDirectory(root);
    if (!check(QDir().mkpath(state), "manager state directory")) return 1;
    const QString staged = state + QLatin1Char('/') + id + QStringLiteral(".vtrponaddon");
    const QString job = state + QLatin1Char('/') + id + QStringLiteral(".job.json");
    const QJsonObject installJob{{QStringLiteral("root"), QFileInfo(root).canonicalFilePath()},
                                 {QStringLiteral("id"), id},
                                 {QStringLiteral("operation"), QStringLiteral("install")},
                                 {QStringLiteral("package"), staged},
                                 {QStringLiteral("sha256"), parsed.sha256},
                                 {QStringLiteral("consent"),
                                  vtrpon::addons::consentReceipt(parsed)}};
    if (!check(QFile::copy(app.arguments().at(2), staged) &&
               write(job, QJsonDocument(installJob).toJson()) &&
               vtrpon::addons::applyJob(job, 0, &error) == 0,
               "package apply failed")) return 1;
    const QJsonObject installed = vtrpon::addons::installed(root, id);
    if (!check(installed.value(QStringLiteral("sha256")).toString() == parsed.sha256 &&
               installed.value(QStringLiteral("agreements")).toArray().size() ==
                   parsed.agreements.size(),
               "installed record is incomplete")) return 1;
    for (const auto &entry : parsed.files) {
      QFile deployed(root + QLatin1Char('/') + entry.path);
      if (!check(deployed.open(QIODevice::ReadOnly) &&
                 QString::fromLatin1(QCryptographicHash::hash(
                     deployed.readAll(), QCryptographicHash::Sha256).toHex()) == entry.sha256,
                 "deployed file does not match package")) return 1;
    }
    const QJsonObject uninstallJob{{QStringLiteral("root"), QFileInfo(root).canonicalFilePath()},
                                   {QStringLiteral("id"), id},
                                   {QStringLiteral("operation"), QStringLiteral("uninstall")}};
    if (!check(write(job, QJsonDocument(uninstallJob).toJson()) &&
               vtrpon::addons::applyJob(job, 0, &error) == 0 &&
               vtrpon::addons::installed(root, id).isEmpty(),
               "package uninstall failed")) return 1;
    for (const auto &entry : parsed.files)
      if (!check(!QFileInfo::exists(root + QLatin1Char('/') + entry.path),
                 "deployed file remains after uninstall")) return 1;
    const QString expectedStateBase = QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).filePath(
            vtrpon::currentProductStorageName() + QStringLiteral("/addon-manager"));
    const QString resolvedBase = QFileInfo(expectedStateBase).canonicalFilePath();
    const QString resolvedState = QFileInfo(state).canonicalFilePath();
    if (!check(!resolvedBase.isEmpty() &&
               resolvedState.startsWith(resolvedBase + QLatin1Char('/')) &&
               QDir(state).removeRecursively(), "manager state cleanup")) return 1;
    return 0;
  }
  if (app.arguments().size() == 2) {
    vtrpon::addons::Package parsed;
    QString error;
    if (!vtrpon::addons::readPackage(app.arguments().at(1), &parsed, &error)) {
      std::fprintf(stderr, "%s\n", error.toUtf8().constData());
      return 1;
    }
    return 0;
  }
  QTemporaryDir temporary;
  if (!check(temporary.isValid(), "temporary directory")) return 1;
  const QString root = temporary.path();
  if (!check(write(root + QStringLiteral("/vtr-pon.exe"), {}), "fake executable")) return 1;
  const QString zipPath = root + QStringLiteral("/package.vtrponaddon");
  const auto valid = package(QStringLiteral("payload/switcher-addons/vtr-pon-atem-addon.dll"),
                             QStringLiteral("switcher-addon-abi-v1"));
  if (!check(write(zipPath, valid), "write package")) return 1;
  QString error;
  vtrpon::addons::Package parsed;
  if (!check(vtrpon::addons::readPackage(zipPath, &parsed, &error),
             "valid package rejected")) return 1;
  if (!check(parsed.agreements.size() == 1 && parsed.notices.isEmpty(),
             "agreement document was not classified")) return 1;
  if (!check(!vtrpon::addons::queueInstall(root, zipPath, {}, {}, {}, {}, &error) &&
             error.contains(QStringLiteral("Agreement")),
             "install without consent was accepted")) return 1;
  const auto state = vtrpon::addons::stateDirectory(root);
  if (!check(QDir().mkpath(state), "state directory")) return 1;
  const QString job = state + QStringLiteral("/vtr-pon-atem-addon.job.json");
  const QJsonObject installJob{{QStringLiteral("root"), QFileInfo(root).canonicalFilePath()},
                               {QStringLiteral("id"), QStringLiteral("vtr-pon-atem-addon")},
                               {QStringLiteral("operation"), QStringLiteral("install")},
                               {QStringLiteral("package"), zipPath},
                               {QStringLiteral("sha256"), parsed.sha256},
                               {QStringLiteral("consent"),
                                vtrpon::addons::consentReceipt(parsed)}};
  const QString rejectedPackage = root + QStringLiteral("/rejected.vtrponaddon");
  QJsonObject rejectedJob = installJob;
  QJsonObject wrongConsent = vtrpon::addons::consentReceipt(parsed);
  wrongConsent.insert(QStringLiteral("packageSha256"), QString(64, QLatin1Char('0')));
  rejectedJob.insert(QStringLiteral("package"), rejectedPackage);
  rejectedJob.insert(QStringLiteral("consent"), wrongConsent);
  if (!check(write(rejectedPackage, valid) &&
             write(job, QJsonDocument(rejectedJob).toJson()) &&
             vtrpon::addons::applyJob(job, 0, &error) == 1 &&
             !QFileInfo::exists(root + QStringLiteral("/switcher-addons/vtr-pon-atem-addon.dll")),
             "mismatched consent was applied")) return 1;
  if (!check(write(job, QJsonDocument(installJob).toJson()), "install job")) return 1;
  if (!check(vtrpon::addons::applyJob(job, 0, &error) == 0, "install failed")) return 1;
  const QString dll = root + QStringLiteral("/switcher-addons/vtr-pon-atem-addon.dll");
  const auto installedRecord = vtrpon::addons::installed(root, QStringLiteral("vtr-pon-atem-addon"));
  if (!check(QFileInfo::exists(dll) && !installedRecord.isEmpty() &&
             installedRecord.value(QStringLiteral("consent")).toObject() ==
                 installJob.value(QStringLiteral("consent")).toObject() &&
             installedRecord.value(QStringLiteral("agreements")).toArray().size() == 1,
             "install state missing")) return 1;
  QJsonObject nextManifest{
      {QStringLiteral("formatVersion"), 2},
      {QStringLiteral("id"), QStringLiteral("vtr-pon-atem-addon")},
      {QStringLiteral("version"), QStringLiteral("1.0.1")},
      {QStringLiteral("contract"), QStringLiteral("switcher-addon-abi-v1")},
      {QStringLiteral("agreements"), QJsonArray{QJsonObject{
          {QStringLiteral("path"), QStringLiteral("licenses/AGREEMENT.txt")},
          {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(
              QByteArrayLiteral("Test agreement for this add-on.\n"),
              QCryptographicHash::Sha256).toHex())}}}},
      {QStringLiteral("runtimeStatus"), 1}};
  const QString note = root + QStringLiteral("/switcher-addons/docs/vtr-pon-atem-addon/info.txt");
  const QByteArray updated = archive({
      {QByteArrayLiteral("manifest.json"), QJsonDocument(nextManifest).toJson()},
      {QByteArrayLiteral("README.md"), QByteArrayLiteral("Updated package")},
      {QByteArrayLiteral("licenses/AGREEMENT.txt"),
       QByteArrayLiteral("Test agreement for this add-on.\n")},
      {QByteArrayLiteral("payload/switcher-addons/vtr-pon-atem-addon.dll"),
       QByteArrayLiteral("dummy dll v2")},
      {QByteArrayLiteral("payload/switcher-addons/docs/vtr-pon-atem-addon/info.txt"),
       QByteArrayLiteral("owned note")}});
  if (!check(write(zipPath, updated), "update package")) return 1;
  const QString updatedSha = QString::fromLatin1(
      QCryptographicHash::hash(updated, QCryptographicHash::Sha256).toHex());
  QJsonObject updateJob = installJob;
  updateJob.insert(QStringLiteral("sha256"), updatedSha);
  if (!check(vtrpon::addons::readPackage(zipPath, &parsed, &error),
             "updated package rejected")) return 1;
  updateJob.insert(QStringLiteral("consent"), vtrpon::addons::consentReceipt(parsed));
  if (!check(write(job, QJsonDocument(updateJob).toJson()), "update job")) return 1;
  if (!check(vtrpon::addons::applyJob(job, 0, &error) == 0, "update failed")) return 1;
  QFile updatedDll(dll);
  if (!check(updatedDll.open(QIODevice::ReadOnly) &&
             updatedDll.readAll() == QByteArrayLiteral("dummy dll v2") &&
             QFileInfo::exists(note), "update files missing")) return 1;
  updatedDll.close();
  const auto beforeInterruptedUpdate = vtrpon::addons::installed(
      root, QStringLiteral("vtr-pon-atem-addon"));
  const QString backupDirectory = state + QStringLiteral("/vtr-pon-atem-addon.backup");
  if (!check(QDir().mkpath(backupDirectory) &&
             write(backupDirectory + QStringLiteral("/0"), QByteArrayLiteral("dummy dll v2")) &&
             write(backupDirectory + QStringLiteral("/1"), QByteArrayLiteral("owned note")),
             "recovery backup")) return 1;
  QJsonObject interruptedJob = updateJob;
  interruptedJob.insert(QStringLiteral("stage"), QStringLiteral("applying"));
  interruptedJob.insert(QStringLiteral("oldRecord"), beforeInterruptedUpdate);
  interruptedJob.insert(QStringLiteral("backups"), QJsonArray{
      QStringLiteral("switcher-addons/vtr-pon-atem-addon.dll"),
      QStringLiteral("switcher-addons/docs/vtr-pon-atem-addon/info.txt")});
  interruptedJob.insert(QStringLiteral("newFiles"), QJsonArray{
      QStringLiteral("switcher-addons/vtr-pon-atem-addon.dll"),
      QStringLiteral("switcher-addons/docs/vtr-pon-atem-addon/info.txt")});
  if (!check(write(dll, QByteArrayLiteral("partial update")) &&
             write(job, QJsonDocument(interruptedJob).toJson()),
             "interrupted update fixture")) return 1;
  if (!check(vtrpon::addons::applyJob(job, 0, &error) == 1,
             "interrupted update should recover then reject same version")) return 1;
  QFile restoredDll(dll);
  if (!check(restoredDll.open(QIODevice::ReadOnly) &&
             restoredDll.readAll() == QByteArrayLiteral("dummy dll v2"),
             "interrupted update was not restored")) return 1;
  restoredDll.close();
  const QJsonObject uninstallJob{{QStringLiteral("root"), QFileInfo(root).canonicalFilePath()},
                                 {QStringLiteral("id"), QStringLiteral("vtr-pon-atem-addon")},
                                 {QStringLiteral("operation"), QStringLiteral("uninstall")}};
  if (!check(write(note, QByteArrayLiteral("changed by user")), "modify owned file")) return 1;
  if (!check(write(job, QJsonDocument(uninstallJob).toJson()), "guard job")) return 1;
  if (!check(vtrpon::addons::applyJob(job, 0, &error) == 1 &&
             QFileInfo::exists(dll) && QFileInfo::exists(note),
             "modified owned file was deleted")) return 1;
  if (!check(write(note, QByteArrayLiteral("owned note")), "restore owned file")) return 1;
  if (!check(write(job, QJsonDocument(uninstallJob).toJson()), "uninstall job")) return 1;
  if (!check(vtrpon::addons::applyJob(job, 0, &error) == 0, "uninstall failed")) return 1;
  if (!check(!QFileInfo::exists(dll) && !QFileInfo::exists(note) &&
             vtrpon::addons::installed(root, QStringLiteral("vtr-pon-atem-addon")).isEmpty(),
             "uninstall state remains")) return 1;
  const auto sha = [](const QByteArray &contents) {
    return QString::fromLatin1(
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
  };
  const QByteArray manualDll = QByteArrayLiteral("manual ATEM add-on");
  const QString unrelated = root + QStringLiteral("/switcher-addons/user-notes.txt");
  if (!check(write(dll, manualDll) && write(unrelated, QByteArrayLiteral("keep")),
             "manual ATEM fixture")) return 1;
  if (!check(!vtrpon::addons::queueUninstall(
                 root, QStringLiteral("vtr-pon-atem-addon"), &error) &&
             error == QStringLiteral("The add-on manager executable is missing.") &&
             !QFileInfo::exists(job),
             "manual ATEM uninstall was not queued")) return 1;
  const QJsonArray manualAtEmFiles{QJsonObject{
      {QStringLiteral("path"), QStringLiteral("switcher-addons/vtr-pon-atem-addon.dll")},
      {QStringLiteral("sha256"), sha(manualDll)}}};
  const QJsonObject manualAtEmJob{
      {QStringLiteral("root"), QFileInfo(root).canonicalFilePath()},
      {QStringLiteral("id"), QStringLiteral("vtr-pon-atem-addon")},
      {QStringLiteral("operation"), QStringLiteral("manual-uninstall")},
      {QStringLiteral("manualFiles"), manualAtEmFiles}};
  if (!check(write(job, QJsonDocument(manualAtEmJob).toJson()) &&
             write(dll, QByteArrayLiteral("changed after queue")),
             "manual ATEM change fixture")) return 1;
  if (!check(vtrpon::addons::applyJob(job, 0, &error) == 1 &&
             QFileInfo::exists(dll) && QFileInfo::exists(unrelated),
             "changed manual add-on was deleted")) return 1;
  if (!check(write(dll, manualDll) &&
             write(job, QJsonDocument(manualAtEmJob).toJson()) &&
             vtrpon::addons::applyJob(job, 0, &error) == 0 &&
             !QFileInfo::exists(dll) && QFileInfo::exists(unrelated),
             "manual ATEM uninstall changed unrelated file")) return 1;

  const QString vstExe = root + QStringLiteral("/audio-addons/vtr-pon-vst3-addon.exe");
  const QString vstQt = root + QStringLiteral("/audio-addons/Qt6Core.dll");
  const QString vstUser = root + QStringLiteral("/audio-addons/my-effect.vst3");
  const QByteArray vstExeBytes = QByteArrayLiteral("manual VST host");
  const QByteArray vstQtBytes = QByteArrayLiteral("manual Qt runtime");
  if (!check(QDir().mkpath(root + QStringLiteral("/audio-addons")) &&
             write(vstExe, vstExeBytes) && write(vstQt, vstQtBytes) &&
             write(vstUser, QByteArrayLiteral("user plug-in")),
             "manual VST fixture")) return 1;
  const QString vstJob = state + QStringLiteral("/vtr-pon-vst3-addon.job.json");
  const QJsonArray manualVstFiles{
      QJsonObject{{QStringLiteral("path"),
                   QStringLiteral("audio-addons/vtr-pon-vst3-addon.exe")},
                  {QStringLiteral("sha256"), sha(vstExeBytes)}},
      QJsonObject{{QStringLiteral("path"), QStringLiteral("audio-addons/Qt6Core.dll")},
                  {QStringLiteral("sha256"), sha(vstQtBytes)}}};
  QJsonObject manualVstJob{
      {QStringLiteral("root"), QFileInfo(root).canonicalFilePath()},
      {QStringLiteral("id"), QStringLiteral("vtr-pon-vst3-addon")},
      {QStringLiteral("operation"), QStringLiteral("manual-uninstall")},
      {QStringLiteral("manualFiles"), manualVstFiles}};
  QJsonArray unsafeManual = manualVstFiles;
  unsafeManual.append(QJsonObject{
      {QStringLiteral("path"), QStringLiteral("audio-addons/my-effect.vst3")},
      {QStringLiteral("sha256"), sha(QByteArrayLiteral("user plug-in"))}});
  manualVstJob.insert(QStringLiteral("manualFiles"), unsafeManual);
  if (!check(write(vstJob, QJsonDocument(manualVstJob).toJson()) &&
             vtrpon::addons::applyJob(vstJob, 0, &error) == 1 &&
             QFileInfo::exists(vstExe) && QFileInfo::exists(vstUser),
             "unlisted manual file was accepted")) return 1;
  manualVstJob.insert(QStringLiteral("manualFiles"), manualVstFiles);
  const QString vstBackup = state + QStringLiteral("/vtr-pon-vst3-addon.backup");
  if (!check(QDir().mkpath(vstBackup) &&
             write(vstBackup + QStringLiteral("/0"), vstQtBytes) &&
             write(vstBackup + QStringLiteral("/1"), vstExeBytes),
             "manual VST recovery backup")) return 1;
  manualVstJob.insert(QStringLiteral("stage"), QStringLiteral("applying"));
  manualVstJob.insert(QStringLiteral("oldRecord"), QJsonObject{});
  manualVstJob.insert(QStringLiteral("backups"), QJsonArray{
      QStringLiteral("audio-addons/Qt6Core.dll"),
      QStringLiteral("audio-addons/vtr-pon-vst3-addon.exe")});
  manualVstJob.insert(QStringLiteral("newFiles"), QJsonArray{});
  if (!check(QFile::remove(vstExe) &&
             write(vstJob, QJsonDocument(manualVstJob).toJson()),
             "manual VST interrupted fixture")) return 1;
  if (!check(vtrpon::addons::applyJob(vstJob, 0, &error) == 0 &&
             !QFileInfo::exists(vstExe) && !QFileInfo::exists(vstQt) &&
             QFileInfo::exists(vstUser),
             "manual VST recovery or removal failed")) return 1;
  const QByteArray oldFormat = archive({
      {QByteArrayLiteral("manifest.json"), QJsonDocument(QJsonObject{
          {QStringLiteral("formatVersion"), 1},
          {QStringLiteral("id"), QStringLiteral("vtr-pon-atem-addon")},
          {QStringLiteral("version"), QStringLiteral("0.1.0")},
          {QStringLiteral("contract"), QStringLiteral("switcher-addon-abi-v1")}}).toJson()},
      {QByteArrayLiteral("README.md"), QByteArrayLiteral("Old development package")},
      {QByteArrayLiteral("payload/switcher-addons/vtr-pon-atem-addon.dll"),
       QByteArrayLiteral("old binary")}});
  write(zipPath, oldFormat);
  if (!check(vtrpon::addons::readPackage(zipPath, &parsed, &error) &&
             parsed.agreements.isEmpty() &&
             !vtrpon::addons::queueInstall(root, zipPath, {}, {}, {}, {}, &error),
             "v1 package installed without an agreement")) return 1;
  const auto wrongContract = package(QStringLiteral("payload/switcher-addons/vtr-pon-atem-addon.dll"),
                                      QStringLiteral("switcher-addon-abi-v2"));
  write(zipPath, wrongContract);
  if (!check(!vtrpon::addons::readPackage(zipPath, &parsed, &error),
             "wrong ABI accepted")) return 1;
  const auto unsafe = package(QStringLiteral("payload/switcher-addons/../vtr-pon-atem-addon.dll"),
                               QStringLiteral("switcher-addon-abi-v1"));
  write(zipPath, unsafe);
  if (!check(!vtrpon::addons::readPackage(zipPath, &parsed, &error),
             "path traversal accepted")) return 1;
  const QByteArray changedAgreement = archive({
      {QByteArrayLiteral("manifest.json"), manifest(QStringLiteral("switcher-addon-abi-v1"))},
      {QByteArrayLiteral("README.md"), QByteArrayLiteral("Test package")},
      {QByteArrayLiteral("licenses/AGREEMENT.txt"), QByteArrayLiteral("Different terms\n")},
      {QByteArrayLiteral("payload/switcher-addons/vtr-pon-atem-addon.dll"),
       QByteArrayLiteral("dummy dll")}});
  write(zipPath, changedAgreement);
  if (!check(!vtrpon::addons::readPackage(zipPath, &parsed, &error),
             "changed agreement document accepted")) return 1;
  const QByteArray duplicate = archive({
      {QByteArrayLiteral("manifest.json"), QByteArrayLiteral(
          "{\"formatVersion\":1,\"id\":\"vtr-pon-atem-addon\","
          "\"version\":\"1.0.0\",\"version\":\"2.0.0\","
          "\"contract\":\"switcher-addon-abi-v1\"}")},
      {QByteArrayLiteral("README.md"), QByteArrayLiteral("Test package")},
      {QByteArrayLiteral("payload/switcher-addons/vtr-pon-atem-addon.dll"),
       QByteArrayLiteral("dummy dll")}});
  write(zipPath, duplicate);
  if (!check(!vtrpon::addons::readPackage(zipPath, &parsed, &error),
             "duplicate manifest key accepted")) return 1;
  if (!check(!vtrpon::addons::hasUniqueJsonKeys(QByteArrayLiteral(
                 "{\"packages\":[{\"id\":1,\"id\":2}]}")),
             "duplicate catalog key accepted")) return 1;
  QDir(state).removeRecursively();
  return 0;
}
