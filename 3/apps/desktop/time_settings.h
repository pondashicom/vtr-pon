#pragma once
#include "controller_settings.h"
#include <QFontDatabase>
#include <QTimeZone>
#include <algorithm>

namespace vtrpon {
inline QTimeZone savedClockTimeZone() {
  const auto id = makeControllerSettings().value("time/timeZone", "Asia/Tokyo").toByteArray();
  const QTimeZone zone(id);
  return zone.isValid() ? zone : QTimeZone::systemTimeZone();
}
inline QFont savedClockFont() {
  auto settings = makeControllerSettings();
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  const auto family = settings.value("time/fontFamily").toString();
  if (!family.isEmpty()) font.setFamily(family);
  font.setPointSize(std::clamp(settings.value("time/fontSize", 12).toInt(), 8, 48));
  font.setWeight(QFont::DemiBold);
  return font;
}
inline bool savedRemainingTotal() {
  return makeControllerSettings().value("time/remainingTotal", false).toBool();
}
}
