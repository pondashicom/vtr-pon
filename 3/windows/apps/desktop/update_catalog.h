#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace vtrpon {

struct UpdateRelease {
  QString version;
  QString publishedAt;
  QStringList notesJa;
  QStringList notesEn;
};

struct UpdateCatalog {
  QString stable;
  QString preview;
  QList<UpdateRelease> releases;
};

// Returns -1, 0 or 1 for valid SemVer versions, or std::nullopt for invalid input.
std::optional<int> compareSemVer(const QString &left, const QString &right);
bool parseUpdateCatalog(const QByteArray &json, UpdateCatalog *catalog);
std::optional<UpdateRelease> newerUpdate(const UpdateCatalog &catalog,
                                         const QString &currentVersion);

} // namespace vtrpon
