#pragma once

// Stable translation IDs and deferred user-facing text.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace vtrpon {

QString uiText(const char *id, int n = -1);
QStringList knownUiTextIds();

class LocalizedText final {
public:
  LocalizedText() = default;
  explicit LocalizedText(const char *id);
  static LocalizedText fromRaw(const QString &text);

  LocalizedText arg(const QString &value) const;
  LocalizedText arg(qint64 value) const;
  LocalizedText arg(int value) const;
  QString resolve() const;
  bool isEmpty() const;

  bool operator==(const LocalizedText &) const = default;

private:
  QByteArray id_;
  QStringList arguments_;
  QString rawText_;
};

} // namespace vtrpon
