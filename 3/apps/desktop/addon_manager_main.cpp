#include "addon_package.h"
#include "product_identity.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QLockFile>
#include <QProcess>
#include <QTextStream>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  vtrpon::initializeProductIdentity();
  const auto args = app.arguments();
  const bool apply = args.size() == 5 && args.at(1) == QLatin1String("--apply") &&
                     args.at(3) == QLatin1String("--parent-pid");
  const bool resumeAll = args.size() == 4 && args.at(1) == QLatin1String("--resume-all") &&
                         args.at(2) == QLatin1String("--parent-pid");
  const bool hasReadyEvent = args.size() >= 7 &&
      args.at(5).startsWith(QLatin1String("--ready-event="));
  const int separator = hasReadyEvent ? 6 : 5;
  const bool restart = args.size() > separator && args.at(1) == QLatin1String("--restart") &&
                       args.at(2) == QLatin1String("--parent-pid") &&
                       args.at(4).startsWith(QLatin1String("--pending-ids=")) &&
                       args.at(separator) == QLatin1String("--");
  if (!apply && !resumeAll && !restart) return 2;
  bool ok = false;
  const qint64 pid = args.at(apply ? 4 : 3).toLongLong(&ok);
  if (!ok || pid < 0) return 2;
  const QString root = QCoreApplication::applicationDirPath();
  if (restart) {
    const auto logRestart = [&root](const QString &message) {
      QFile log(vtrpon::addons::stateDirectory(root) + QStringLiteral("/restart.log"));
      if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        QTextStream(&log) << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                         << ' ' << QCoreApplication::applicationPid() << ' ' << message << '\n';
    };
    logRestart(QStringLiteral("Waiting for parent %1").arg(pid));
    const auto acknowledge = [&] {
#ifdef Q_OS_WIN
      if (!hasReadyEvent) return true;
      const QString name = args.at(5).sliced(QStringLiteral("--ready-event=").size());
      if (!name.startsWith(QLatin1String("Local\\VTR-PON-AddonRestart-"))) return false;
      HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE,
          reinterpret_cast<LPCWSTR>(name.utf16()));
      if (!ready) return false;
      const bool signaled = SetEvent(ready);
      CloseHandle(ready);
      return signaled;
#else
      return !hasReadyEvent;
#endif
    };
    QLockFile restartLock(vtrpon::addons::stateDirectory(root) + QStringLiteral("/restart.lock"));
    restartLock.setStaleLockTime(0);
    // A manual launch during an automatic restart must not start a second copy.
    if (!restartLock.tryLock(0)) {
      logRestart(QStringLiteral("Coordinator already active; lock error %1").arg(restartLock.error()));
      return restartLock.error() == QLockFile::LockFailedError && acknowledge() ? 0 : 1;
    }
    // If the requesting Controller stopped waiting, do not restart it later.
    if (!acknowledge()) return 1;
    QString error;
    const auto pendingIds = args.at(4).sliced(QStringLiteral("--pending-ids=").size())
        .split(QLatin1Char(','), Qt::SkipEmptyParts);
    const bool finished = vtrpon::addons::finishPendingOperations(root, pid, pendingIds, &error);
    logRestart(QStringLiteral("Operations finished=%1: %2").arg(finished).arg(error));
    QStringList restartArguments = args.mid(separator + 1);
    restartArguments.append(QStringLiteral("--addon-restart-complete"));
    if (!finished) {
      if (error.isEmpty()) error = QStringLiteral("An add-on operation needs recovery.");
      restartArguments.append(QStringLiteral("--addon-restart-error=") + error);
    }
    qint64 startedPid = 0;
    if (!QProcess::startDetached(root + QStringLiteral("/vtr-pon.exe"),
                                 restartArguments, QDir::currentPath(), &startedPid)) {
      logRestart(QStringLiteral("Could not launch Controller"));
      return 3;
    }
    logRestart(QStringLiteral("Launched Controller %1").arg(startedPid));
    return finished ? 0 : 1;
  }
  QStringList jobs;
  if (apply) {
    jobs.append(args.at(2));
  } else {
    const QString state = vtrpon::addons::stateDirectory(root);
    for (const auto &id : vtrpon::addons::ids()) {
      const QString path = state + QLatin1Char('/') + id + QStringLiteral(".job.json");
      if (QFileInfo::exists(path)) jobs.append(path);
    }
  }
  int overall = 0;
  for (const auto &job : jobs) {
    QString error;
    const int result = vtrpon::addons::applyJob(job, pid, &error);
    if (result != 0 && result != 5) {
      QFile log(QFileInfo(job).absolutePath() + QStringLiteral("/manager-error.log"));
      if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        QTextStream(&log) << job << ": " << error << '\n';
      overall = result;
    }
  }
  return overall;
}
