#pragma once

#include <QJsonObject>
#include <QString>

class QSettings;

namespace vtrpon {
// Only preferences exposed by Settings; playlist documents are independent.
QJsonObject controllerSettingsSnapshot(const QSettings &settings);
bool replaceControllerSettings(QSettings &settings, const QJsonObject &values);
bool exportControllerSettings(const QSettings &settings, const QString &path);
bool importControllerSettings(QSettings &settings, const QString &path);
}
