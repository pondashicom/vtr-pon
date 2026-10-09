#include "controller_settings_io.h"
#include "controller_shortcut_settings.h"
#include "midi_control_settings.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QTimeZone>

namespace {
const QJsonObject &defaults() {
  static const QJsonObject values{
      {"files/stillCaptureDirectory", ""},
      {"files/documentConversionDirectory", ""},
      {"shortcuts/bindings", QJsonObject{}},
      {"midi/control", vtrpon::midiControl::defaults()},
      {"appearance/controllerSkin", "hardware"},
      {"privacy/sendActivityNotice", false},
      {"updates/checkAutomatically", true},
      {"accessibility/textSize", 0},
      {"ui/language", "en"},
      {"time/timeZone", "Asia/Tokyo"},
      {"time/remainingTotal", false},
      {"time/fontFamily", ""},
      {"time/fontSize", 12},
      {"operatorMonitor/screen", ""},
      {"operatorMonitor/information", false},
      {"operatorMonitor/remaining", true},
      {"operatorMonitor/file", true},
      {"operatorMonitor/modes", true},
      {"operatorMonitor/rate", true},
      {"operatorMonitor/pages", true},
      {"operatorMonitor/timerEnabled", false},
      {"operatorMonitor/timerDurationSeconds", 300},
      {"operatorMonitor/timerWarning1Minutes", 2},
      {"operatorMonitor/timerWarning2Minutes", 1},
      {"operatorMonitor/timerWarning1Color", "#ffe900"},
      {"operatorMonitor/timerWarning2Color", "#f55700"},
      {"video/gpuAdapterId", ""},
      {"video/softwareDecoding", false},
      {"video/programOutput4k", false},
      {"virtualCamera/enabled", false},
      {"testPatterns/entries", QJsonValue(QJsonValue::Null)},
      {"networkVideo/outputEnabled", false},
      {"networkVideo/inputEnabled", false},
      {"networkVideo/outputName", ""},
      {"networkVideo/compositeEnabled", true},
      {"networkVideo/fillEnabled", false},
      {"networkVideo/keyEnabled", false},
      {"networkVideo/alphaEnabled", false},
      {"networkVideo/fillFormat", "straight"},
      {"deckLink/inputEnabled", true},
      {"deckLink/outputEnabled", false},
      {"deckLink/outputChannel", ""},
      {"deckLink/outputMode", "0"},
      {"deckLink/inputMode", "0"},
      {"deckLink/compositeEnabled", true},
      {"deckLink/fillEnabled", false},
      {"deckLink/keyEnabled", false},
      {"deckLink/fillFormat", "straight"},
      {"deckLink/fillChannel", ""},
      {"deckLink/keyChannel", ""},
      {"deckLink/fillMode", "0"},
      {"deckLink/keyMode", "0"},
      {"player/backgroundColor", "#000000"},
      {"playlist/editActivationMode", 0},
      {"playlist/autoSelectNextItemAfterOnAir", false},
      {"playlist/preferAudioAlbumArt", false},
      {"onAir/allowSameItemRetake", false},
      {"onAir/toggleMode", false},
      {"onAir/showPlaybackRateControls", true},
      {"onAir/disableFtbButton", false},
      {"audio/showBgmPlayerControls", false},
      {"audio/programOutputDeviceId", ""},
      {"audio/editPflOutputDeviceId", ""},
      {"audio/useDefaultWhenMissing", false},
      {"audio/useVstPlugins", false},
      {"audio/vst/edit/path", ""},
      {"audio/vst/edit/uid", ""},
      {"audio/vst/item/path", ""},
      {"audio/vst/item/uid", ""},
      {"audio/vst/master/path", ""},
      {"audio/vst/master/uid", ""},
      {"switcher/rolandV8Hd/enabled", false},
      {"switcher/rolandV8Hd/sourceNumber", 1},
      {"switcher/rolandV8Hd/takeProgramWhenOnAir", false},
      {"switcher/rolandV8Hd/takeOnAirWhenProgramSelected", false},
      {"switcher/rolandV8Hd/ftbFollowsProgram", false},
      {"switcher/atem/enabled", false},
      {"switcher/atem/sourceNumber", 1},
      {"switcher/atem/takeProgramWhenOnAir", false},
      {"switcher/atem/takeOnAirWhenProgramSelected", false},
      {"switcher/atem/ftbFollowsProgram", false}};
  return values;
}

bool isMapping(const QString &key) {
  static const QRegularExpression pattern(
      QStringLiteral("^audio/uvcMappings/[0-9a-f]{64}$"));
  return pattern.match(key).hasMatch();
}

bool validTestPatternEntries(const QJsonValue &value) {
  if (value.isNull()) return true; // No override: use the bundled catalog.
  if (!value.isArray() || value.toArray().size() > 512) return false;
  QSet<QString> seen;
  for (const auto &entryValue : value.toArray()) {
    if (!entryValue.isString()) return false;
    const QString entry = entryValue.toString();
    if (entry.isEmpty() || entry.size() > 4096 || entry.contains(QChar::Null)) return false;
    const bool builtin = entry.startsWith(QLatin1StringView("builtin:"));
    const QString name = builtin ? entry.mid(8) : entry;
    if (builtin ? (name.isEmpty() || QFileInfo(name).fileName() != name)
                : !QDir::isAbsolutePath(name)) return false;
    const QString identity = entry.toCaseFolded();
    if (seen.contains(identity)) return false;
    seen.insert(identity);
  }
  return true;
}

bool valid(const QJsonObject &values) {
  for (auto it = values.begin(); it != values.end(); ++it) {
    const auto expected = defaults().value(it.key());
    if (expected.isUndefined()) {
      if (!isMapping(it.key()) || !it.value().isString()) return false;
    } else if (it.key() == QLatin1StringView("testPatterns/entries")) {
      if (!validTestPatternEntries(it.value())) return false;
    } else if (expected.type() != it.value().type()) return false;
    if (it.key() == vtrpon::shortcutSettings::key() &&
        !vtrpon::shortcutSettings::validationError(it.value().toObject()).isEmpty()) return false;
    if (it.key() == vtrpon::midiControl::settingsKey() &&
        !vtrpon::midiControl::validationError(it.value().toObject()).isEmpty()) return false;
    if (it.value().isString() && it.value().toString().size() > 32768) return false;
    if (it.key().startsWith(QLatin1StringView("files/")) &&
        !it.value().toString().isEmpty() &&
        (!QDir::isAbsolutePath(it.value().toString()) || it.value().toString().contains(QChar::Null))) return false;
    if (it.key() == QLatin1StringView("networkVideo/outputName") &&
        (it.value().toString().size() > 200 || it.value().toString().contains(QChar::Null))) return false;
    if ((it.key() == QLatin1StringView("networkVideo/fillFormat") || it.key() == QLatin1StringView("deckLink/fillFormat")) &&
        !QStringList{"straight", "premultiplied"}.contains(it.value().toString())) return false;
    if ((it.key() == QLatin1StringView("deckLink/outputMode") || it.key() == QLatin1StringView("deckLink/inputMode") || it.key() == QLatin1StringView("deckLink/fillMode") || it.key() == QLatin1StringView("deckLink/keyMode")) &&
        (!QRegularExpression(QStringLiteral("^(0|[1-9][0-9]{0,9})$")).match(it.value().toString()).hasMatch() || it.value().toString().toULongLong() > 0xffffffffULL)) return false;
    if ((it.key() == QLatin1StringView("deckLink/outputChannel") || it.key() == QLatin1StringView("deckLink/fillChannel") || it.key() == QLatin1StringView("deckLink/keyChannel")) && !it.value().toString().isEmpty() &&
        !QRegularExpression(QStringLiteral("^decklink://-?[0-9]+/[0-9]+/[0-9]+/0$")).match(it.value().toString()).hasMatch()) return false;
    if (it.key() == QLatin1StringView("appearance/controllerSkin") &&
        !QStringList{"hardware", "material-dark", "vtr-pon-clasic"}.contains(it.value().toString())) return false;
    if (it.key() == QLatin1StringView("ui/language") &&
        !QStringList{"en", "ja"}.contains(it.value().toString())) return false;
    if ((it.key() == QLatin1StringView("player/backgroundColor") ||
         it.key() == QLatin1StringView("operatorMonitor/timerWarning1Color") ||
         it.key() == QLatin1StringView("operatorMonitor/timerWarning2Color")) &&
        !QRegularExpression(QStringLiteral("^#[0-9a-fA-F]{6}$")).match(it.value().toString()).hasMatch()) return false;
    if (it.key() == QLatin1StringView("time/timeZone") && !it.value().toString().isEmpty()
        && !QTimeZone(it.value().toString().toUtf8()).isValid()) return false;
    if (it.value().isDouble()) {
      const double number = it.value().toDouble();
      const bool timerMinutes = it.key() == QLatin1StringView("operatorMonitor/timerWarning1Minutes") ||
          it.key() == QLatin1StringView("operatorMonitor/timerWarning2Minutes");
      const int maximum = it.key() == QLatin1StringView("accessibility/textSize") ? 2 : it.key() == QLatin1StringView("operatorMonitor/timerDurationSeconds") ? 59999 : timerMinutes ? 999 : it.key() == QLatin1StringView("time/fontSize") ? 48 : it.key().startsWith(QLatin1StringView("playlist/")) ? 1
          : it.key().startsWith(QLatin1StringView("switcher/atem/")) ? 40 : 8;
      const int minimum = it.key() == QLatin1StringView("accessibility/textSize") ? 0 : timerMinutes ? 0 : it.key() == QLatin1StringView("time/fontSize") ? 8 : it.key().startsWith(QLatin1StringView("playlist/")) ? 0 : 1;
      if (number < minimum || number > maximum || number != static_cast<int>(number)) return false;
    }
  }
  if (values.value("operatorMonitor/timerWarning2Minutes").toInt(1) >
      values.value("operatorMonitor/timerWarning1Minutes").toInt(2)) return false;
  return !(values.value("switcher/atem/enabled").toBool() &&
           values.value("switcher/rolandV8Hd/enabled").toBool());
}
}

namespace vtrpon {
QJsonObject controllerSettingsSnapshot(const QSettings &settings) {
  QJsonObject result;
  for (auto it = defaults().begin(); it != defaults().end(); ++it) {
    QVariant value = settings.value(it.key(), it.value().toVariant());
    if (it.key() == QLatin1StringView("testPatterns/entries")) {
      result.insert(it.key(), settings.contains(it.key())
          ? QJsonValue(QJsonArray::fromStringList(value.toStringList()))
          : QJsonValue(QJsonValue::Null));
    } else if (it.key() == midiControl::settingsKey()) result.insert(it.key(), midiControl::normalized(QJsonObject::fromVariantMap(value.toMap())));
    else if (it.value().isObject()) result.insert(it.key(), QJsonObject::fromVariantMap(value.toMap()));
    else if (it.value().isBool()) result.insert(it.key(), value.toBool());
    else if (it.value().isDouble()) result.insert(it.key(), value.toInt());
    else result.insert(it.key(), value.toString());
  }
  for (const auto &key : settings.allKeys()) {
    if (isMapping(key)) result.insert(key, settings.value(key).toString());
  }
  return result;
}

bool replaceControllerSettings(QSettings &settings, const QJsonObject &values) {
  // Older exports may contain the retired fade toggle. The color is now always used.
  auto currentValues = values;
  currentValues.remove(QStringLiteral("player/fadeToBackground"));
  const auto midi = currentValues.value(midiControl::settingsKey());
  if (midi.isObject()) currentValues.insert(midiControl::settingsKey(), midiControl::normalized(midi.toObject()));
  if (!valid(currentValues)) return false;
  settings.sync();
  if (settings.status() != QSettings::NoError || !settings.isWritable()) return false;
  QVariantMap previous;
  for (const auto &key : settings.allKeys()) {
    if (defaults().contains(key) || isMapping(key) || key == QLatin1StringView("player/fadeToBackground")) previous.insert(key, settings.value(key));
  }
  const auto removePreferences = [&] {
    for (const auto &key : settings.allKeys()) {
      if (defaults().contains(key) || isMapping(key) || key == QLatin1StringView("player/fadeToBackground")) settings.remove(key);
    }
  };
  removePreferences();
  for (auto it = currentValues.begin(); it != currentValues.end(); ++it) {
    if (it.key() == QLatin1StringView("testPatterns/entries")) {
      if (!it.value().isNull()) {
        QStringList entries;
        for (const auto &entry : it.value().toArray()) entries.append(entry.toString());
        settings.setValue(it.key(), entries);
      }
    } else settings.setValue(it.key(), it.value().toVariant());
  }
  settings.sync();
  if (settings.status() == QSettings::NoError) return true;
  removePreferences();
  for (auto it = previous.begin(); it != previous.end(); ++it) settings.setValue(it.key(), it.value());
  settings.sync();
  return false;
}

bool exportControllerSettings(const QSettings &settings, const QString &path) {
  const auto values = controllerSettingsSnapshot(settings);
  if (!valid(values) || settings.status() != QSettings::NoError) return false;
  const QByteArray data = QJsonDocument(QJsonObject{
      {"format", "vtr-pon-settings"}, {"version", 1}, {"settings", values}}).toJson();
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

bool importControllerSettings(QSettings &settings, const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return false;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (file.error() != QFileDevice::NoError || error.error != QJsonParseError::NoError || !document.isObject()) return false;
  const auto root = document.object();
  if (root.value("format") != QJsonValue("vtr-pon-settings") ||
      root.value("version") != QJsonValue(1) || !root.value("settings").isObject()) return false;
  return replaceControllerSettings(settings, root.value("settings").toObject());
}
}
