#include "test_pattern_store.h"
#include <QTemporaryDir>
#include <QDebug>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temp;
  if (!temp.isValid()) return 1;
  const auto file = temp.filePath(QStringLiteral("preferences.ini"));
  QSettings settings(file, QSettings::IniFormat);
  const auto bundled = vtrpon::testPatterns::defaults();
  if (bundled.size() != 13 || vtrpon::testPatterns::load(settings) != bundled) return 2;
  for (const auto &entry : bundled)
    if (!QFileInfo::exists(vtrpon::testPatterns::path(entry))) return 3;
  const auto custom = temp.filePath(QStringLiteral("User & Pattern.PNG"));
  QFile media(custom);
  if (!media.open(QIODevice::WriteOnly)) return 4;
  media.write("original");
  media.close();
  QStringList entries{custom, bundled.last(), bundled.first()};
  if (!vtrpon::testPatterns::save(settings, entries)) return 5;
  QSettings reopened(file, QSettings::IniFormat);
  if (vtrpon::testPatterns::load(reopened) != entries) return 6;
  if (!vtrpon::testPatterns::save(settings, {})) return 7;
  reopened.sync();
  if (!vtrpon::testPatterns::load(reopened).isEmpty()) return 8;
  if (!QFileInfo::exists(custom)) return 9;
  if (!vtrpon::testPatterns::supported(custom) ||
      vtrpon::testPatterns::supported(QStringLiteral("invalid.exe"))) return 10;
  qInfo() << "Catalog, order, persistence, empty list and non-destructive removal passed";
  return 0;
}
