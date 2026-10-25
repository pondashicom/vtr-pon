#pragma once
#include <QJsonObject>
#include <QUrl>
class QWidget;
namespace vtrpon {
QJsonObject deckLinkCheckSnapshot();
QUrl deckLinkCheckReportMailUrl(const QJsonObject &report, const QString &fileName);
void showDeckLinkCheckWindow(QWidget *parent);
}
