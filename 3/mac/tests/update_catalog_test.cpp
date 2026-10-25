#include "update_catalog.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; \
    return 1; \
  } \
} while (false)

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc == 3) {
    QFile file(QString::fromLocal8Bit(argv[1]));
    vtrpon::UpdateCatalog siteCatalog;
    if (!file.open(QIODevice::ReadOnly) ||
        !vtrpon::parseUpdateCatalog(file.readAll(), &siteCatalog)) {
      std::cerr << "Invalid update catalog file\n";
      return 1;
    }
    const QString installedVersion = QString::fromLocal8Bit(argv[2]);
    if (siteCatalog.preview != installedVersion ||
        vtrpon::newerUpdate(siteCatalog, installedVersion)) {
      std::cerr << "Catalog does not report the installed preview as current\n";
      return 1;
    }
    std::cout << "CURRENT " << installedVersion.toStdString() << '\n';
    return 0;
  }
  if (argc != 1) return 2;
  using vtrpon::compareSemVer;
  CHECK(compareSemVer(QStringLiteral("3.0.0-beta.2"),
                      QStringLiteral("3.0.0-beta.11")) == -1);
  CHECK(compareSemVer(QStringLiteral("3.0.0-beta.11"),
                      QStringLiteral("3.0.0-rc.1")) == -1);
  CHECK(compareSemVer(QStringLiteral("3.0.0-rc.1"),
                      QStringLiteral("3.0.0")) == -1);
  CHECK(compareSemVer(QStringLiteral("3.0.0+build.2"),
                      QStringLiteral("3.0.0+build.1")) == 0);
  CHECK(compareSemVer(QStringLiteral("3.0.0-1"),
                      QStringLiteral("3.0.0-alpha")) == -1);
  CHECK(!compareSemVer(QStringLiteral("3.0.0-beta.01"),
                       QStringLiteral("3.0.0")));
  CHECK(!compareSemVer(QStringLiteral("3.0.0-beta"),
                       QStringLiteral("3.0.0-beta.01")));

  const QByteArray json = R"({
    "schemaVersion": 1,
    "stable": "3.0.0",
    "preview": "3.1.0-beta.2",
    "releases": [
      {"version":"3.1.0-beta.2","publishedAt":"2026-09-25",
       "notes":{"ja":["ベータの更新"],"en":["Preview update"]}},
      {"version":"3.0.0","publishedAt":"2026-09-20",
       "notes":{"ja":["正式版の更新"],"en":["Stable update"]}}
    ]
  })";
  vtrpon::UpdateCatalog catalog;
  CHECK(vtrpon::parseUpdateCatalog(json, &catalog));
  CHECK(vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0-beta.1"))->version ==
        QStringLiteral("3.1.0-beta.2"));
  CHECK(vtrpon::newerUpdate(catalog, QStringLiteral("2.9.0"))->version ==
        QStringLiteral("3.0.0"));
  CHECK(!vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0")));
  CHECK(!vtrpon::newerUpdate(catalog, QStringLiteral("3.1.0-beta.2")));
  CHECK(!vtrpon::parseUpdateCatalog(
      QByteArrayLiteral("{\"schemaVersion\":2,\"stable\":\"3.0.0\"}"), &catalog));
  CHECK(!vtrpon::parseUpdateCatalog(
      QByteArrayLiteral("{\"schemaVersion\":1,\"preview\":\"3.0.0\",\"releases\":[]}"),
      &catalog));
  CHECK(!vtrpon::parseUpdateCatalog(
      QByteArrayLiteral("{\"schemaVersion\":1,\"stable\":\"3.0.0\",\"releases\":[]}"),
      &catalog));
  // The old root-level channel is Windows-only. Neither Mac CPU may use it.
  for (const auto &target : {QStringLiteral("macos-arm64"), QStringLiteral("macos-x64")}) {
    CHECK(vtrpon::parseUpdateCatalog(json, &catalog, target));
    CHECK(!catalog.available && catalog.platform == target && catalog.releases.isEmpty());
    CHECK(!vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0-beta.1")));
  }
  const auto macChannel = [](const QString &version) {
    return QJsonDocument::fromJson((QStringLiteral(R"({"preview":"%1","releases":[{"version":"%1","publishedAt":"2026-10-19","notes":{"ja":["Mac向け"],"en":["For Mac"]}}]})")
        .arg(version)).toUtf8()).object();
  };
  auto mixed = QJsonDocument::fromJson(json).object();
  mixed.insert(QStringLiteral("platforms"), QJsonObject{
      {QStringLiteral("macos-arm64"), macChannel(QStringLiteral("3.0.0-beta.2"))},
      {QStringLiteral("macos-x64"), macChannel(QStringLiteral("3.0.0-beta.3"))}});
  const auto bytes = QJsonDocument(mixed).toJson();
  CHECK(vtrpon::parseUpdateCatalog(bytes, &catalog, QStringLiteral("macos-arm64")));
  auto arm = vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0-beta.1"));
  CHECK(catalog.available && arm && arm->version == QStringLiteral("3.0.0-beta.2") && arm->platform == QStringLiteral("macos-arm64"));
  CHECK(!vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0-beta.2")));
  CHECK(!vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0"))); // Stable installations ignore preview.
  CHECK(vtrpon::parseUpdateCatalog(bytes, &catalog, QStringLiteral("macos-x64")));
  CHECK(vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0-beta.1"))->version == QStringLiteral("3.0.0-beta.3"));
  CHECK(vtrpon::parseUpdateCatalog(bytes, &catalog)); // Existing Windows root channel remains compatible.
  CHECK(vtrpon::newerUpdate(catalog, QStringLiteral("3.0.0-beta.1"))->version == QStringLiteral("3.1.0-beta.2"));
  CHECK(!vtrpon::parseUpdateCatalog(QByteArrayLiteral("{\"schemaVersion\":1,\"platforms\":[]}"), &catalog, QStringLiteral("macos-arm64")));
  CHECK(!vtrpon::parseUpdateCatalog(QByteArrayLiteral("{\"schemaVersion\":1,\"platforms\":{\"macos-arm64\":null}}"), &catalog, QStringLiteral("macos-arm64")));
  CHECK(!vtrpon::parseUpdateCatalog(QByteArrayLiteral("{\"schemaVersion\":1,\"platforms\":{\"macos-arm64\":{\"preview\":\"3.0.0-beta.2\",\"releases\":[]}}}"), &catalog, QStringLiteral("macos-arm64")));
  return 0;
}
