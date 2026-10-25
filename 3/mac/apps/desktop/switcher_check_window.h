#pragma once
#include <QJsonObject>
#include <QUrl>
class QWidget;
namespace vtrpon {
class SwitcherController;
class UiLanguageManager;
QUrl switcherCheckReportMailUrl(const QJsonObject &report, const QString &fileName);
void showSwitcherCheckWindow(SwitcherController *controller, UiLanguageManager *language, QWidget *parent);
}
