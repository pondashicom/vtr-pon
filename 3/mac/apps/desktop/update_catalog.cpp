#include "update_catalog.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace {

struct Version {
  QStringList core;
  QStringList pre;
};

std::optional<Version> parseVersion(const QString &text) {
  if (text.size() > 128) return std::nullopt;
  static const QRegularExpression pattern(QStringLiteral(
      R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)"));
  const auto match = pattern.match(text);
  if (!match.hasMatch()) return std::nullopt;
  Version version{{match.captured(1), match.captured(2), match.captured(3)},
                  match.captured(4).isEmpty() ? QStringList{} : match.captured(4).split(QLatin1Char('.'))};
  for (const QString &identifier : version.pre) {
    if (identifier.size() > 32) return std::nullopt;
    if (identifier.size() > 1 && identifier.startsWith(QLatin1Char('0')) &&
        std::all_of(identifier.cbegin(), identifier.cend(),
                    [](QChar c) { return c.isDigit(); })) return std::nullopt;
  }
  return version;
}

int compareDigits(QString left, QString right) {
  while (left.size() > 1 && left.startsWith(QLatin1Char('0'))) left.remove(0, 1);
  while (right.size() > 1 && right.startsWith(QLatin1Char('0'))) right.remove(0, 1);
  if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
  return QString::compare(left, right, Qt::CaseSensitive);
}

bool readNotes(const QJsonValue &value, QStringList *notes) {
  if (!value.isArray()) return false;
  const auto array = value.toArray();
  if (array.isEmpty() || array.size() > 30) return false;
  for (const auto &item : array) {
    if (!item.isString() || item.toString().trimmed().isEmpty() ||
        item.toString().size() > 1000) return false;
    notes->append(item.toString().trimmed());
  }
  return true;
}

} // namespace

namespace vtrpon {

std::optional<int> compareSemVer(const QString &left, const QString &right) {
  const auto a = parseVersion(left);
  const auto b = parseVersion(right);
  if (!a || !b) return std::nullopt;
  for (int i = 0; i < 3; ++i) {
    const int order = compareDigits(a->core[i], b->core[i]);
    if (order) return order < 0 ? -1 : 1;
  }
  if (a->pre.isEmpty() || b->pre.isEmpty()) {
    if (a->pre.isEmpty() == b->pre.isEmpty()) return 0;
    return a->pre.isEmpty() ? 1 : -1;
  }
  const int count = std::min(a->pre.size(), b->pre.size());
  for (int i = 0; i < count; ++i) {
    const QString &x = a->pre[i];
    const QString &y = b->pre[i];
    const bool xNumeric = std::all_of(x.cbegin(), x.cend(), [](QChar c) { return c.isDigit(); });
    const bool yNumeric = std::all_of(y.cbegin(), y.cend(), [](QChar c) { return c.isDigit(); });
    if (xNumeric != yNumeric) return xNumeric ? -1 : 1;
    const int order = xNumeric ? compareDigits(x, y) : QString::compare(x, y, Qt::CaseSensitive);
    if (order) return order < 0 ? -1 : 1;
  }
  if (a->pre.size() == b->pre.size()) return 0;
  return a->pre.size() < b->pre.size() ? -1 : 1;
}

bool parseUpdateCatalog(const QByteArray &json, UpdateCatalog *catalog, const QString &platform) {
  if (!catalog || json.size() > 256 * 1024) return false;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  const auto root = document.object();
  if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != 1) return false;
  UpdateCatalog result;
  result.platform = platform;
  QJsonObject channel = root;
  const auto platforms = root.value(QStringLiteral("platforms"));
  if (!platforms.isUndefined() && !platforms.isObject()) return false;
  if (platforms.isObject() && platforms.toObject().contains(platform)) {
    const auto selected = platforms.toObject().value(platform);
    if (!selected.isObject()) return false;
    channel = selected.toObject();
  } else if (platform != QStringLiteral("windows-x64")) {
    result.available = false;
    *catalog = std::move(result);
    return true;
  }
  const auto stable = channel.value(QStringLiteral("stable"));
  const auto preview = channel.value(QStringLiteral("preview"));
  if ((!stable.isUndefined() && !stable.isString()) ||
      (!preview.isUndefined() && !preview.isString())) return false;
  result.stable = stable.toString();
  result.preview = preview.toString();
  if (result.stable.isEmpty() && result.preview.isEmpty()) return false;
  if ((!result.stable.isEmpty() && (!parseVersion(result.stable) ||
                                  !parseVersion(result.stable)->pre.isEmpty())) ||
      (!result.preview.isEmpty() && (!parseVersion(result.preview) ||
                                    parseVersion(result.preview)->pre.isEmpty()))) return false;
  const auto releases = channel.value(QStringLiteral("releases"));
  if (!releases.isArray() || releases.toArray().size() > 100) return false;
  QSet<QString> seen;
  for (const auto &value : releases.toArray()) {
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    UpdateRelease release;
    release.platform = platform;
    release.version = object.value(QStringLiteral("version")).toString();
    release.publishedAt = object.value(QStringLiteral("publishedAt")).toString();
    const auto notes = object.value(QStringLiteral("notes")).toObject();
    if (!parseVersion(release.version) || seen.contains(release.version) ||
        !QDate::fromString(release.publishedAt, Qt::ISODate).isValid() ||
        !readNotes(notes.value(QStringLiteral("ja")), &release.notesJa) ||
        !readNotes(notes.value(QStringLiteral("en")), &release.notesEn)) return false;
    seen.insert(release.version);
    result.releases.append(release);
  }
  if ((!result.stable.isEmpty() && !seen.contains(result.stable)) ||
      (!result.preview.isEmpty() && !seen.contains(result.preview))) return false;
  *catalog = std::move(result);
  return true;
}

std::optional<UpdateRelease> newerUpdate(const UpdateCatalog &catalog,
                                         const QString &currentVersion) {
  const auto current = parseVersion(currentVersion);
  if (!catalog.available || !current) return std::nullopt;
  QString best = catalog.stable;
  if (!current->pre.isEmpty() && !catalog.preview.isEmpty() &&
      (best.isEmpty() || compareSemVer(catalog.preview, best).value_or(-1) > 0))
    best = catalog.preview;
  if (best.isEmpty() || compareSemVer(best, currentVersion).value_or(0) <= 0)
    return std::nullopt;
  for (const auto &release : catalog.releases) {
    if (release.version == best) return release;
  }
  return std::nullopt;
}

} // namespace vtrpon
