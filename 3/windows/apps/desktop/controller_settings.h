#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "product_identity.h"

namespace vtrpon {

inline QString defaultControllerConfigDirectory() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#else
  const QString config =
      QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  return config.isEmpty() ? QString()
                         : QDir(config).filePath(currentProductStorageName());
#endif
}

inline QString resolveControllerSettingsFile(const QString &normalDirectory,
                                             const QString &executableDirectory,
                                             bool portable) {
  const QString directory = portable
      ? QDir(executableDirectory).filePath(
            QStringLiteral("config/") + productStorageName(executableDirectory))
      : normalDirectory;
  return directory.isEmpty() ? QString()
                            : QDir(directory).absoluteFilePath(QStringLiteral("user.ini"));
}

inline void initializeControllerSettingsLocation() {
  bool portable = false;
#ifdef Q_OS_WIN
  const QDir executable(QCoreApplication::applicationDirPath());
  portable = QCoreApplication::arguments().contains(QStringLiteral("--portable")) ||
      QFileInfo::exists(executable.filePath(QStringLiteral("portable_mode"))) ||
      QFileInfo::exists(executable.filePath(QStringLiteral("portable_mode.txt")));
#endif
  QCoreApplication::instance()->setProperty("vtrpon.settingsFile",
      resolveControllerSettingsFile(defaultControllerConfigDirectory(),
                                    QCoreApplication::applicationDirPath(), portable));
  QCoreApplication::instance()->setProperty("vtrpon.portableMode", portable);
}

inline QString controllerSettingsFilePath() {
  if (auto *app = QCoreApplication::instance()) {
    const QVariant configured = app->property("vtrpon.settingsFile");
    if (configured.isValid()) return configured.toString();
  }
  // Existing tests explicitly select an isolated INI scope with setPath().
  // The desktop entry point always freezes its explicit path above instead.
  if (QSettings::defaultFormat() == QSettings::IniFormat) {
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     currentProductStorageName(),
                     QStringLiteral("VTR-PON Controller")).fileName();
  }
  return resolveControllerSettingsFile(defaultControllerConfigDirectory(),
                                       QCoreApplication::applicationDirPath(), false);
}

inline QSettings makeControllerSettings() {
  // An explicit INI file has no registry or organization/system fallback.
  return QSettings(controllerSettingsFilePath(), QSettings::IniFormat);
}

// Explicit one-time import only: ordinary startup never reads legacy native
// settings. Run the import from the user's normal environment, whose legacy
// values were verified; never merge competing registry views automatically.
inline bool importLegacyControllerSettings(QSettings &source,
                                           const QString &destination,
                                           QString *error) {
  const auto fail = [&](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (destination.isEmpty() || QFileInfo::exists(destination))
    return fail(QStringLiteral("The INI destination is empty or already exists; it was not overwritten."));
  const QStringList keys = source.allKeys();
  if (source.status() != QSettings::NoError)
    return fail(QStringLiteral("Could not read legacy settings."));
  const QString directory = QFileInfo(destination).absolutePath();
  if (!QDir().mkpath(directory))
    return fail(QStringLiteral("Could not create the settings directory: %1").arg(directory));
  QTemporaryDir temporary(QDir(directory).filePath(QStringLiteral(".settings-import-XXXXXX")));
  if (!temporary.isValid()) return fail(temporary.errorString());
  const QString temporaryFile = temporary.filePath(QStringLiteral("user.ini"));
  {
    QSettings target(temporaryFile, QSettings::IniFormat);
    for (const QString &key : keys) target.setValue(key, source.value(key));
    target.sync();
    if (source.status() != QSettings::NoError || target.status() != QSettings::NoError)
      return fail(QStringLiteral("Could not save the imported settings."));
  }
  // QFile::rename refuses an existing destination; the published file is
  // complete, and an existing user's INI cannot be replaced by an import.
  if (!QFile::rename(temporaryFile, destination))
    return fail(QStringLiteral("Could not publish the imported INI; an existing file was not overwritten."));
  if (error) error->clear();
  return true;
}

} // namespace vtrpon
