#pragma once
#include "addon_platform.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vtrpon::addons {

struct File {
  QString path;
  QByteArray contents;
  QString sha256;
};

struct Package {
  QJsonObject manifest;
  QString sha256;
  QVector<File> files;
  QByteArray readme;
  QVector<File> agreements;
  QVector<File> notices;
};

QStringList ids();
QString entryPath(const QString &id);
QString contract(const QString &id);
int compareVersions(const QString &left, const QString &right);
bool hasUniqueJsonKeys(const QByteArray &json);
QString stateDirectory(const QString &applicationRoot);
QStringList pendingJobs(const QString &applicationRoot);
bool restartInProgress(const QString &applicationRoot);
bool requestRestart(const QString &applicationRoot, const QStringList &arguments,
                    const QString &workingDirectory, QString *error);
bool finishPendingOperations(const QString &applicationRoot, qint64 parentPid,
                             const QStringList &pendingIds, QString *error);
QJsonObject installed(const QString &applicationRoot, const QString &id);
bool readPackage(const QString &path, Package *package, QString *error);
QJsonObject consentReceipt(const Package &package);
bool queueInstall(const QString &applicationRoot, const QString &path,
                  const QString &expectedId, const QString &expectedVersion,
                  const QString &expectedSha256, const QJsonObject &consent,
                  QString *error);
bool queueUninstall(const QString &applicationRoot, const QString &id,
                    QString *error);
int applyJob(const QString &jobPath, qint64 parentPid, QString *error);

} // namespace vtrpon::addons
