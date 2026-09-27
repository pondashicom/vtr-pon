#pragma once

#include "controller_settings.h"

namespace vtrpon {

inline bool load4kProgramOutput(const QSettings &settings) {
  return settings.value(QStringLiteral("video/programOutput4k"), false).toBool();
}

inline bool save4kProgramOutput(QSettings &settings, bool enabled) {
  settings.sync();
  if (settings.status() != QSettings::NoError || !settings.isWritable()) return false;
  const bool previous = load4kProgramOutput(settings);
  settings.setValue(QStringLiteral("video/programOutput4k"), enabled);
  settings.sync();
  if (settings.status() == QSettings::NoError) return true;
  settings.setValue(QStringLiteral("video/programOutput4k"), previous);
  settings.sync();
  return false;
}

// Freeze before creating playback/output resources. Saving a preference later
// must not renegotiate live textures or interrupt Program playback.
inline bool initializeProgramOutputResolution(bool commandLine4k = false) {
  auto *app = QCoreApplication::instance();
  if (!app) return false;
  const auto active = app->property("video.startupProgramOutput4k");
  if (active.isValid()) return active.toBool();
#ifdef Q_OS_WIN
  const auto settings = makeControllerSettings();
  const bool enabled = commandLine4k || load4kProgramOutput(settings);
#else
  Q_UNUSED(commandLine4k);
  const bool enabled = false;
#endif
  app->setProperty("video.startupProgramOutput4k", enabled);
  return enabled;
}

} // namespace vtrpon
