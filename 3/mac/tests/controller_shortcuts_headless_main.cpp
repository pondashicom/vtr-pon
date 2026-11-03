#include "controller_settings.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QTranslator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
int runMacAddonReadinessRegression(QApplication &app);
bool runControllerShortcutsRegression();
bool runPlatformCapabilitiesRegression();
bool runSwitcherReportRegression();
int runMacReturnOutputSettingsRegression();
int runMacRolandMidiRegression();
int runMacRolandProfilesRegression();
bool runMacAtemRegression(const QString &fixture);
int main(int argc, char **argv) {
  // Disposable JSON probe, used only by the add-on readiness regression.
  if (argc == 5 && QByteArray(argv[1]) == "--kind" && QByteArray(argv[3]) == "--addon-file") {
    QCoreApplication probe(argc, argv);
    QFile file(QString::fromLocal8Bit(argv[4]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto data = file.readAll();
    const auto status = QJsonDocument::fromJson(data).object().value("status").toString();
    if (status == "slow") QThread::msleep(15000);
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    return 0;
  }
  QApplication app(argc, argv);
  vtrpon::initializeProductIdentity();
  QTemporaryDir settings;
  if (!settings.isValid()) return 1;
  app.setProperty("vtrpon.settingsFile", settings.filePath("settings.ini"));
  QTranslator translator;
  const QString language = app.arguments().contains("--japanese") ? "ja" : "en";
  if (!translator.load(QCoreApplication::applicationDirPath() + "/vtrpon_" + language + ".qm")) return 2;
  app.installTranslator(&translator);
  if (app.arguments().contains("--addon-readiness-only")) return runMacAddonReadinessRegression(app);
  if (app.arguments().contains("--return-output-settings-only")) return runMacReturnOutputSettingsRegression();
  if (app.arguments().contains("--switcher-report")) return runSwitcherReportRegression() ? 0 : 1;
  const int atem = app.arguments().indexOf("--atem");
  if (atem > 0 && atem + 1 < argc) return runMacAtemRegression(app.arguments().at(atem + 1)) ? 0 : 1;
  if (app.arguments().contains("--roland-profiles")) return runMacRolandProfilesRegression();
  if (app.arguments().contains("--roland")) return runMacRolandMidiRegression();
  if (app.arguments().contains("--capabilities")) return runPlatformCapabilitiesRegression() ? 0 : 1;
  return runControllerShortcutsRegression() ? 0 : 1;
}
