#include "addon_package.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2) return 2;
  const QDir directory(app.arguments()[1]);
  QFile catalog(directory.filePath("catalog-macos.json"));
  QVector<vtrpon::addons::CatalogEntry> entries;
  QString error;
  if (!catalog.open(QIODevice::ReadOnly) ||
      !vtrpon::addons::readCatalog(catalog.readAll(), &entries, &error) || entries.size()!=3) {
    std::fprintf(stderr, "Catalog: %s\n", qPrintable(error)); return 1;
  }
  for (const auto &entry: entries) {
    vtrpon::addons::Package package;
    const QString name=entry.id+'-'+entry.version+"-macos-arm64.vtrponaddon";
    if (!vtrpon::addons::readPackage(directory.filePath(name), &package, &error) ||
        package.sha256!=entry.sha256 || package.manifest.value("id").toString()!=entry.id ||
        package.manifest.value("version").toString()!=entry.version) {
      std::fprintf(stderr, "%s: %s\n", qPrintable(name), qPrintable(error)); return 1;
    }
    QTemporaryDir extracted;
    if (!extracted.isValid()) return 1;
    for (const auto &file: package.files) {
      const QString destination=extracted.filePath(file.path);
      QDir().mkpath(QFileInfo(destination).absolutePath());
      QFile output(destination);
      if (!output.open(QIODevice::WriteOnly) || output.write(file.contents)!=file.contents.size()) return 1;
      output.close();
    }
    // The installer writes regular files and restores execute bits for Mach-O.
    // Exercise that representation, rather than verifying only the staging app.
    for (const auto &file: package.files) {
      if (file.contents.left(4)!=QByteArray::fromHex("cffaedfe")) continue;
      const QString path=extracted.filePath(file.path);
      QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther);
      if (!vtrpon::addons::verifyInstalledBinary(path,&error)) {
        std::fprintf(stderr,"Installed signature %s: %s\n",qPrintable(file.path),qPrintable(error));return 1;
      }
    }
    std::printf("PASS %s: %lld payload files\n",qPrintable(name),static_cast<long long>(package.files.size()));
  }
}
