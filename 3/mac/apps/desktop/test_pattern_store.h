#pragma once

#include "controller_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStringList>

namespace vtrpon::testPatterns {
inline QString directory() {
#ifdef Q_OS_MACOS
  const auto resources=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../Resources/assets/testpatterns"));
  if(QFileInfo::exists(QDir(resources).filePath(QStringLiteral("catalog.json")))) return QDir::cleanPath(resources);
#endif
  return QDir(QCoreApplication::applicationDirPath()).filePath(
      QStringLiteral("assets/testpatterns"));
}
inline QStringList defaults() {
  QFile file(QDir(directory()).filePath(QStringLiteral("catalog.json")));
  const bool opened = file.open(QIODevice::ReadOnly);
  qInfo().noquote() << "READ_SOURCE test-pattern-catalog"
                    << file.fileName() << "opened=" << opened;
  if (!opened) return {};
  QStringList result;
  for (const auto &entry : QJsonDocument::fromJson(file.readAll()).array()) {
    const QString name = entry.toString();
    if (!name.isEmpty() && QFileInfo(name).fileName() == name)
      result.append(QStringLiteral("builtin:") + name);
  }
  return result;
}
inline QString path(const QString &entry) {
  return entry.startsWith(QStringLiteral("builtin:"))
      ? QDir(directory()).filePath(QStringLiteral("media/") + entry.mid(8))
      : entry;
}
inline QStringList load(const QSettings &settings) {
  return settings.contains(QStringLiteral("testPatterns/entries"))
      ? settings.value(QStringLiteral("testPatterns/entries")).toStringList()
      : defaults();
}
inline QStringList load() {
  QSettings settings = vtrpon::makeControllerSettings();
  return load(settings);
}
inline bool save(QSettings &settings, const QStringList &entries) {
  if (!settings.isWritable()) return false;
  const bool existed = settings.contains(QStringLiteral("testPatterns/entries"));
  const QVariant previous = settings.value(QStringLiteral("testPatterns/entries"));
  settings.setValue(QStringLiteral("testPatterns/entries"), entries);
  settings.sync();
  if (settings.status() == QSettings::NoError) return true;
  if (existed) settings.setValue(QStringLiteral("testPatterns/entries"), previous);
  else settings.remove(QStringLiteral("testPatterns/entries"));
  settings.sync();
  return false;
}
inline bool save(const QStringList &entries) {
  QSettings settings = vtrpon::makeControllerSettings();
  return save(settings, entries);
}
inline bool supported(const QString &path) {
  static const QStringList extensions{
      "mp4", "mov", "mkv", "webm", "avi", "mxf", "m4v", "mpg", "mpeg", "ts",
      "png", "jpg", "jpeg", "wav", "mp3", "flac", "aac", "m4a", "ogg", "aif", "aiff"};
  return extensions.contains(QFileInfo(path).suffix().toLower());
}
}
