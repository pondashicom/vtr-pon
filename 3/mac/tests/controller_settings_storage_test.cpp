#include "controller_settings.h"
#include "file_management_settings.h"
#include "video_output_settings.h"

#include <QJsonObject>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  vtrpon::initializeProductIdentity();
  if (app.arguments().size() == 5 && app.arguments()[1] == "--output-resolution") {
    app.setProperty("vtrpon.settingsFile", app.arguments()[2]);
    const bool active = vtrpon::initializeProgramOutputResolution(app.arguments()[4] == "1");
    return active == (app.arguments()[3] == "1") ? 0 : 1;
  }
  if (app.arguments().size() == 4 && app.arguments()[1] == "--read-child") {
    app.setProperty("vtrpon.settingsFile", app.arguments()[2]);
    auto settings = vtrpon::makeControllerSettings();
    return settings.format() == QSettings::IniFormat &&
                   settings.value("ui/language").toString() == app.arguments()[3]
               ? 0 : 1;
  }
  QTemporaryDir directory;
  if (!directory.isValid()) return 1;
  bool ok = true;
  const auto check = [&](bool result, const char *message) {
    if (!result) { std::fprintf(stderr, "%s\n", message); ok = false; }
  };
  const QString target = directory.filePath("config/user.ini");
  QSettings source(directory.filePath("legacy.ini"), QSettings::IniFormat);
  source.setValue("ui/language", "ja");
  source.setValue("appearance/controllerSkin", "vtr-pon-clasic");
  source.setValue("audio/programOutputDeviceId", "test-device");
  source.setValue("audio/showBgmPlayerControls", true);
  source.setValue("playlist/editActivationMode", 1);
  source.setValue("testPatterns/entries", QStringList{});
  source.setValue("fileDialog/lastMediaDirectory", QString::fromUtf8("G:/素材"));
  source.setValue("shortcuts/bindings", QVariantMap{{"LoadEdit", QStringList{"Ctrl+E"}}});
  source.sync();
  QString error;
  const bool imported = vtrpon::importLegacyControllerSettings(source, target, &error);
  if (!imported) std::fprintf(stderr, "%s\n", error.toUtf8().constData());
  check(imported, "Import failed");
  app.setProperty("vtrpon.settingsFile", target);
  {
    auto settings = vtrpon::makeControllerSettings();
    check(settings.format() == QSettings::IniFormat, "Storage is not INI");
    check(settings.allKeys() == source.allKeys(), "Import dropped keys");
    check(settings.value("ui/language") == source.value("ui/language"), "Language lost");
    check(settings.value("audio/showBgmPlayerControls").toBool(), "Boolean lost");
    check(settings.value("playlist/editActivationMode").toInt() == 1, "Integer lost");
    check(settings.contains("testPatterns/entries") &&
              settings.value("testPatterns/entries").toStringList().isEmpty(), "Empty list lost");
    check(settings.value("shortcuts/bindings").toMap() ==
              source.value("shortcuts/bindings").toMap(), "Shortcut map lost");
    check(settings.value("fileDialog/lastMediaDirectory") ==
              source.value("fileDialog/lastMediaDirectory"), "Unicode path lost");
    settings.setValue("ui/language", "en");
    settings.sync();
    check(settings.status() == QSettings::NoError, "Save failed");
  }
  check(!vtrpon::importLegacyControllerSettings(source, target, &error), "Import overwrote an existing INI");
  QProcess child;
  child.setWorkingDirectory(directory.path());
  child.start(QCoreApplication::applicationFilePath(), {"--read-child", target, "en"});
  const bool finished = child.waitForFinished(10000);
  check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "A separate process did not read the saved INI");
  if (!finished) { child.kill(); child.waitForFinished(); }
  check(source.value("ui/language").toString() == "ja", "Import changed legacy settings");
  {
    auto settings = vtrpon::makeControllerSettings();
    check(vtrpon::stillCaptureDirectory() == vtrpon::defaultStillCaptureDirectory(), "Still default changed");
    const auto custom = directory.filePath("custom");
    settings.setValue("files/stillCaptureDirectory", custom);
    settings.setValue("files/documentConversionDirectory", custom);
    settings.sync();
    check(vtrpon::stillCaptureDirectory() == custom, "Custom still directory ignored");
    check(vtrpon::documentConversionDirectory("PDF") == QDir(custom).filePath("PDF"), "PDF directory ignored");
    check(vtrpon::documentConversionDirectory("PowerPoint") == QDir(custom).filePath("PowerPoint"), "PPTX directory ignored");
    settings.setValue("files/documentConversionDirectory", "");
    settings.sync();
    check(vtrpon::documentConversionDirectory("PDF") == QDir(vtrpon::defaultGeneratedMediaDirectory()).filePath("PDF"),
          "Blank directory did not restore the original default");
  }
  const QString normal = directory.filePath("normal");
  const QString executable = directory.filePath("application");
  check(vtrpon::resolveControllerSettingsFile(normal, executable, false) ==
            QDir(normal).absoluteFilePath("user.ini"), "Normal settings path is incorrect");
  const QString portable = vtrpon::resolveControllerSettingsFile(normal, executable, true);
  check(portable == QDir(executable).absoluteFilePath("config/VTR-PON3-Dev/user.ini"),
        "Portable settings path is incorrect");
  check(portable != target, "Portable settings overlap normal settings");
  check(vtrpon::defaultControllerConfigDirectory().endsWith("VTR-PON3-Dev"),
        "Development settings are not isolated");
  {
    QDir().mkpath(executable);
    QFile marker(QDir(executable).filePath(QString::fromLatin1(vtrpon::kDistributionMarker)));
    check(marker.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              marker.write("VTR-PON3\n") == 9,
          "Could not create distribution marker");
    marker.close();
    check(vtrpon::productStorageName(executable) == QStringLiteral("VTR-PON3"),
          "Distribution identity is incorrect");
    check(vtrpon::resolveControllerSettingsFile(normal, executable, true) ==
              QDir(executable).absoluteFilePath("config/VTR-PON3/user.ini"),
          "Distribution portable settings path is incorrect");
  }
#ifdef Q_OS_WIN
  {
    auto settings = vtrpon::makeControllerSettings();
    check(!vtrpon::load4kProgramOutput(settings), "Output must default to 1080p");
    check(!vtrpon::initializeProgramOutputResolution(), "Default startup must use 1080p");
    check(vtrpon::save4kProgramOutput(settings, true), "4K preference save failed");
    check(!vtrpon::initializeProgramOutputResolution(), "Saving 4K changed the live output");
    const auto verifyStartup = [&](bool expected, bool force4k) {
      QProcess outputChild;
      outputChild.start(QCoreApplication::applicationFilePath(),
          {"--output-resolution", target, expected ? "1" : "0", force4k ? "1" : "0"});
      const bool ended = outputChild.waitForFinished(10000);
      check(ended && outputChild.exitStatus() == QProcess::NormalExit && outputChild.exitCode() == 0,
            "Restart did not apply the saved resolution or command-line override");
      if (!ended) { outputChild.kill(); outputChild.waitForFinished(); }
    };
    verifyStartup(true, false);
    check(vtrpon::save4kProgramOutput(settings, false), "1080p preference save failed");
    verifyStartup(false, false);
    verifyStartup(true, true);
    check(!vtrpon::load4kProgramOutput(settings), "Command-line override changed the saved preference");
  }
#endif
  return ok ? 0 : 1;
}
