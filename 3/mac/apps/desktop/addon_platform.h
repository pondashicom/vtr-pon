#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

namespace vtrpon::addons {
struct CatalogEntry { QString id, version, sha256, url; };
QString platformId();
QString rootKey(const QString &applicationRoot);
QString applicationExecutable(const QString &applicationRoot);
QString managerExecutable(const QString &applicationRoot);
QString installationDirectory(const QString &applicationRoot);
QString addonFilePath(const QString &applicationRoot, const QString &id);
bool readCatalog(const QByteArray &data, QVector<CatalogEntry> *entries, QString *error);
bool validatePlatformPayload(const QString &path, const QByteArray &bytes, QString *error);
bool verifyInstalledBinary(const QString &path, QString *error);
#ifdef Q_OS_MACOS
// Worker-only: never called from the Controller's playout or UI callbacks.
bool waitForAddonProcesses(const QString &root, qint64 parentPid,
                           bool managers, QString *error);
#endif
}
