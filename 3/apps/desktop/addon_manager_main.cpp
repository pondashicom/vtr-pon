#include "addon_package.h"
#include "product_identity.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  vtrpon::initializeProductIdentity();
  const auto args = app.arguments();
  const bool apply = args.size() == 5 && args.at(1) == QLatin1String("--apply") &&
                     args.at(3) == QLatin1String("--parent-pid");
  const bool resumeAll = args.size() == 4 && args.at(1) == QLatin1String("--resume-all") &&
                         args.at(2) == QLatin1String("--parent-pid");
  if (!apply && !resumeAll) return 2;
  bool ok = false;
  const qint64 pid = args.at(apply ? 4 : 3).toLongLong(&ok);
  if (!ok || pid < 0) return 2;
  const QString root = QCoreApplication::applicationDirPath();
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
