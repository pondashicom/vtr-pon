#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace vtrpon {

struct UpdateRelease {
  QString version;
  QString publishedAt;
  QStringList notesJa;
  QStringList notesEn;
  QString platform;
  QUrl downloadPage;
};

struct UpdateCatalog {
  QString stable;
  QString preview;
  QList<UpdateRelease> releases;
  QString platform;
  bool available = true;
};

// Returns -1, 0 or 1 for valid SemVer versions, or std::nullopt for invalid input.
std::optional<int> compareSemVer(const QString &left, const QString &right);
// Legacy root-level releases belong to Windows x64 only. Mac requires an exact
// platforms[platform] channel; a missing channel is valid but unavailable.
bool parseUpdateCatalog(const QByteArray &json, UpdateCatalog *catalog,
                        const QString &platform = QStringLiteral("windows-x64"));
std::optional<UpdateRelease> newerUpdate(const UpdateCatalog &catalog,
                                         const QString &currentVersion);

} // namespace vtrpon
