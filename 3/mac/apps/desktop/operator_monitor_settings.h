#pragma once
#include <QSettings>
#include <QString>
#include <QColor>
#include <array>

namespace vtrpon {
inline constexpr std::array<const char *, 5> operatorMonitorFields{
    "remaining", "file", "modes", "rate", "pages"};
struct OperatorMonitorSettings {
  QString screen;
  bool information = false;
  std::array<bool, 5> fields{true, true, true, true, true};
  bool timerEnabled = false;
  int timerDurationSeconds = 300;
  int timerWarning1Minutes = 2;
  int timerWarning2Minutes = 1;
  QColor timerWarning1Color{QStringLiteral("#ffe900")};
  QColor timerWarning2Color{QStringLiteral("#f55700")};
};
inline OperatorMonitorSettings loadOperatorMonitorSettings(const QSettings &settings) {
  OperatorMonitorSettings value;
  value.screen = settings.value("operatorMonitor/screen", "").toString();
  value.information = settings.value("operatorMonitor/information", false).toBool();
  value.timerEnabled = settings.value("operatorMonitor/timerEnabled", false).toBool();
  value.timerDurationSeconds = qBound(1, settings.value("operatorMonitor/timerDurationSeconds", 300).toInt(), 59999);
  value.timerWarning1Minutes = qBound(0, settings.value("operatorMonitor/timerWarning1Minutes", 2).toInt(), 999);
  value.timerWarning2Minutes = qBound(0, settings.value("operatorMonitor/timerWarning2Minutes", 1).toInt(), value.timerWarning1Minutes);
  const QColor first(settings.value("operatorMonitor/timerWarning1Color", "#ffe900").toString());
  const QColor second(settings.value("operatorMonitor/timerWarning2Color", "#f55700").toString());
  if (first.isValid()) value.timerWarning1Color = first;
  if (second.isValid()) value.timerWarning2Color = second;
  for (size_t i = 0; i < value.fields.size(); ++i)
    value.fields[i] = settings.value(QStringLiteral("operatorMonitor/") +
        QString::fromLatin1(operatorMonitorFields[i]), true).toBool();
  return value;
}
inline bool saveOperatorMonitorSettings(QSettings &settings, const OperatorMonitorSettings &value) {
  settings.setValue("operatorMonitor/screen", value.screen);
  settings.setValue("operatorMonitor/information", value.information);
  settings.setValue("operatorMonitor/timerEnabled", value.timerEnabled);
  settings.setValue("operatorMonitor/timerDurationSeconds", value.timerDurationSeconds);
  settings.setValue("operatorMonitor/timerWarning1Minutes", value.timerWarning1Minutes);
  settings.setValue("operatorMonitor/timerWarning2Minutes", value.timerWarning2Minutes);
  settings.setValue("operatorMonitor/timerWarning1Color", value.timerWarning1Color.name());
  settings.setValue("operatorMonitor/timerWarning2Color", value.timerWarning2Color.name());
  for (size_t i = 0; i < value.fields.size(); ++i)
    settings.setValue(QStringLiteral("operatorMonitor/") +
        QString::fromLatin1(operatorMonitorFields[i]), value.fields[i]);
  settings.sync();
  return settings.status() == QSettings::NoError;
}
}
