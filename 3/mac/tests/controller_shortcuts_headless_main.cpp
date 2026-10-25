#include "controller_settings.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QTranslator>
bool runControllerShortcutsRegression();
bool runPlatformCapabilitiesRegression();
bool runSwitcherReportRegression();
int runMacRolandMidiRegression();
int runMacRolandProfilesRegression();
bool runMacAtemRegression(const QString &fixture);
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  vtrpon::initializeProductIdentity();
  QTemporaryDir settings;
  if (!settings.isValid()) return 1;
  app.setProperty("vtrpon.settingsFile", settings.filePath("settings.ini"));
  QTranslator translator;
  const QString language = app.arguments().contains("--japanese") ? "ja" : "en";
  if (!translator.load(QCoreApplication::applicationDirPath() + "/vtrpon_" + language + ".qm")) return 2;
  app.installTranslator(&translator);
  if (app.arguments().contains("--switcher-report")) return runSwitcherReportRegression() ? 0 : 1;
  const int atem = app.arguments().indexOf("--atem");
  if (atem > 0 && atem + 1 < argc) return runMacAtemRegression(app.arguments().at(atem + 1)) ? 0 : 1;
  if (app.arguments().contains("--roland-profiles")) return runMacRolandProfilesRegression();
  if (app.arguments().contains("--roland")) return runMacRolandMidiRegression();
  if (app.arguments().contains("--capabilities")) return runPlatformCapabilitiesRegression() ? 0 : 1;
  return runControllerShortcutsRegression() ? 0 : 1;
}
