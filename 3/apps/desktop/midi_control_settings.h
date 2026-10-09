#pragma once

#include "controller_shortcut_settings.h"
#include "audio_fader_scale.h"
#include "language_text.h"
#include <QSet>
#include <cmath>
#include <algorithm>
#include <optional>

namespace vtrpon::midiControl {
inline QString settingsKey() { return QStringLiteral("midi/control"); }
inline QStringList actionIds() {
  QStringList result;
  for (int index : shortcutSettings::displayOrder())
    result.append(shortcutSettings::definitions().at(index).id);
  for (const auto &definition : shortcutSettings::fixedDefinitions())
    result.append(definition.id);
  result.append({QStringLiteral("SetMasterVolume"), QStringLiteral("SetItemVolume"),
                 QStringLiteral("SetEditVolume"), QStringLiteral("SetBgmVolume")});
  return result;
}
inline bool isVolume(const QString &id) {
  return id == QLatin1StringView("SetMasterVolume") || id == QLatin1StringView("SetItemVolume") ||
         id == QLatin1StringView("SetEditVolume") || id == QLatin1StringView("SetBgmVolume");
}
inline QString actionLabel(const QString &id) {
  for (const auto &definition : shortcutSettings::fixedDefinitions())
    if (definition.id == id) return uiText((QStringLiteral("midi.") + id).toUtf8().constData());
  if (isVolume(id)) return uiText((QStringLiteral("midi.") + id).toUtf8().constData());
  if (id.startsWith(QLatin1StringView("SelectItem"))) return uiText("settings.shortcuts.SelectItem").arg(id.mid(10));
  if (id.startsWith(QLatin1StringView("ActivatePlaylist"))) return uiText("settings.shortcuts.ActivatePlaylist").arg(id.mid(16));
  return uiText((QStringLiteral("settings.shortcuts.") + id).toUtf8().constData());
}
inline QJsonObject defaults() {
  return {{"enabled", false}, {"device", ""}, {"bindings", QJsonObject{}}};
}
inline QStringList clearConflictingBindings(QJsonObject &bindings, const QString &action, const QJsonObject &binding) {
  QStringList cleared;
  for (auto it = bindings.begin(); it != bindings.end();) {
    const auto other = it.value().toObject();
    if (it.key() != action && other.value("type") == binding.value("type") &&
        other.value("channel") == binding.value("channel") && other.value("number") == binding.value("number")) {
      cleared.append(it.key());
      it = bindings.erase(it);
    } else ++it;
  }
  return cleared;
}
inline QJsonObject normalized(QJsonObject value) {
  if (!value.value("bindings").isObject()) return value;
  auto bindings = value.value("bindings").toObject();
  // Remove the withdrawn prototype assignment without losing other controls.
  bindings.remove(QStringLiteral("ManualTransition"));
  for (auto it = bindings.begin(); it != bindings.end(); ++it) {
    if (!isVolume(it.key()) || !it.value().isObject()) continue;
    auto binding = it.value().toObject();
    if (!binding.contains("scale") && binding.value("minimum") == QJsonValue(-600) && binding.value("maximum") == QJsonValue(0)) {
      binding.insert("minimum", audioFaderScale::silence);
      binding.insert("maximum", audioFaderScale::maximum);
    }
    if (!binding.contains("scale")) binding.insert("scale", "fader");
    it.value() = binding;
  }
  value.insert("bindings", bindings);
  return value;
}
inline bool integer(const QJsonValue &value, int minimum, int maximum) {
  return value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum &&
         value.toDouble() == value.toInt();
}
inline QString validationError(const QJsonObject &value) {
  if (value.size() != 3 || !value.value("enabled").isBool() || !value.value("device").isString() ||
      value.value("device").toString().size() > 512 || value.value("device").toString().contains(QChar::Null) ||
      !value.value("bindings").isObject()) return QStringLiteral("configuration");
  const auto ids = actionIds();
  QMap<QString, QString> used;
  const auto bindings = value.value("bindings").toObject();
  for (auto it = bindings.begin(); it != bindings.end(); ++it) {
    if (!ids.contains(it.key()) || !it.value().isObject()) return it.key();
    const auto binding = it.value().toObject();
    const auto type = binding.value("type").toString();
    const QSet<QString> fields{"type", "channel", "number", "mode", "pickup", "reverse", "minimum", "maximum", "scale"};
    for (auto field = binding.begin(); field != binding.end(); ++field)
      if (!fields.contains(field.key())) return it.key();
    if ((type != "note" && type != "cc") ||
        (isVolume(it.key()) && type != "cc") || !integer(binding.value("channel"), 1, 16) ||
        !integer(binding.value("number"), 0, 127)) return it.key();
    if (binding.contains("mode") && (!binding.value("mode").isString() ||
        !QStringList{"absolute", "twos-complement", "binary-offset", "signed-bit", "signed-bit-2"}.contains(binding.value("mode").toString()))) return it.key();
    for (const auto *field : {"pickup", "reverse"})
      if (binding.contains(field) && !binding.value(field).isBool()) return it.key();
    if (binding.contains("scale") && binding.value("scale") != QJsonValue("fader")) return it.key();
    for (const auto *field : {"minimum", "maximum"})
      if (binding.contains(field) && !integer(binding.value(field), -960, 60)) return it.key();
    if (binding.value("minimum").toInt(audioFaderScale::silence) >= binding.value("maximum").toInt(audioFaderScale::maximum)) return it.key();
    if (!isVolume(it.key()) && binding.value("mode").toString("absolute") != "absolute") return it.key();
    const auto identity = QStringLiteral("%1/%2/%3").arg(type).arg(binding.value("channel").toInt()).arg(binding.value("number").toInt());
    if (used.contains(identity)) return it.key() + QStringLiteral(" / ") + used.value(identity);
    used.insert(identity, it.key());
  }
  return {};
}
inline QJsonObject load(const QSettings &settings) {
  if (!settings.contains(settingsKey())) return defaults();
  const auto value = normalized(QJsonObject::fromVariantMap(settings.value(settingsKey()).toMap()));
  return validationError(value).isEmpty() ? value : defaults();
}
struct Message {
  QString type;
  int channel = 0;
  int number = 0;
  int value = 0;
  bool pressed = false;
  int minimumValue = -1, maximumValue = -1;
};
inline Message decode(quint32 packed) {
  const int status = packed & 0xff;
  const int number = (packed >> 8) & 0xff;
  const int value = (packed >> 16) & 0xff;
  const int kind = status & 0xf0;
  if ((kind != 0x80 && kind != 0x90 && kind != 0xb0) || number > 127 || value > 127) return {};
  return {kind == 0xb0 ? QStringLiteral("cc") : QStringLiteral("note"), (status & 15) + 1,
          number, value, kind == 0x90 && value > 0};
}
// Relative encoders use the conventional 7-bit increment/decrement encodings.
inline int relativeDelta(int value, const QString &mode) {
  if (mode == "twos-complement") return value < 64 ? value : value - 128;
  if (mode == "binary-offset") return value - 64;
  const int delta = value < 64 ? value : -(value - 64);
  return mode == "signed-bit-2" ? -delta : delta;
}
class VolumeMapper {
public:
  void reset() { states_.clear(); }
  void forget(const QString &id) { states_.remove(id); }
  std::optional<int> map(const QString &id, int raw, const QJsonObject &binding, int current,
                         int minimumReceived = -1, int maximumReceived = -1) {
    const int minimum = binding.value("minimum").toInt(audioFaderScale::silence);
    const int maximum = binding.value("maximum").toInt(audioFaderScale::maximum);
    const auto mode = binding.value("mode").toString("absolute");
    const bool reverse = binding.value("reverse").toBool();
    if (mode != "absolute") {
      const int delta = relativeDelta(raw, mode) * (reverse ? -1 : 1);
      if (!delta) return std::nullopt;
      if (current <= audioFaderScale::silence) return delta < 0 ? audioFaderScale::silence : std::max(minimum, audioFaderScale::silence + 10);
      if (delta < 0 && current <= minimum) return -960;
      return std::clamp(current + delta * 10, minimum, maximum);
    }
    const int value = reverse ? 127 - raw : raw;
    const double lowerPosition = audioFaderScale::positionForValue(minimum);
    const double upperPosition = audioFaderScale::positionForValue(maximum);
    const auto ccPosition = [&](int softwareValue) {
      if (softwareValue <= audioFaderScale::silence) return 0;
      return std::clamp(1 + static_cast<int>(std::lround(
          (audioFaderScale::positionForValue(softwareValue) - lowerPosition) * 126.0 /
          (upperPosition - lowerPosition))), 1, 127);
    };
    int target = value == 0 ? audioFaderScale::silence : std::clamp(audioFaderScale::valueForPosition(
        lowerPosition + (upperPosition - lowerPosition) * (value - 1) / 126.0), minimum, maximum);
    // The nearest 7-bit position is the same 0 dB detent as the screen fader.
    if (value > 0 && minimum <= 0 && maximum >= 0 && value == ccPosition(0)) target = 0;
    auto &state = states_[id];
    if (state.lastOutput && *state.lastOutput != current) {
      state.captured = false; state.previous.reset(); state.lastOutput.reset();
    }
    const int softwarePosition = ccPosition(current);
    if (binding.value("pickup").toBool(false) && !state.captured) {
      const int low = reverse ? 127 - maximumReceived : minimumReceived;
      const int high = reverse ? 127 - minimumReceived : maximumReceived;
      const bool crossedInBatch = minimumReceived >= 0 && maximumReceived <= 127 &&
          minimumReceived <= maximumReceived && low <= softwarePosition && high >= softwarePosition;
      state.captured = crossedInBatch || std::abs(value - softwarePosition) <= 1 ||
          (state.previous && ((*state.previous <= softwarePosition && value >= softwarePosition) ||
                              (*state.previous >= softwarePosition && value <= softwarePosition)));
      state.previous = value;
      if (!state.captured) return std::nullopt;
    }
    state.previous = value;
    state.lastOutput = target;
    return target;
  }
private:
  struct State { bool captured = false; std::optional<int> previous, lastOutput; };
  QMap<QString, State> states_;
};

// Tracks button edges independently of assignments. Learning/suspended input is
// still tracked, so releasing a learned control cannot execute a new assignment.
class Router {
public:
  void reset() { held_.clear(); lastVolumes_.clear(); }
  void resetContinuous() { lastVolumes_.clear(); }
  void resetButtons(const QString &type) {
    for (auto it = held_.begin(); it != held_.end();)
      if (it->startsWith(type + QLatin1Char('/'))) it = held_.erase(it); else ++it;
  }
  QString process(const Message &message, const QJsonObject &bindings, bool execute) {
    if (message.type.isEmpty()) return {};
    const auto identity = QStringLiteral("%1/%2/%3").arg(message.type).arg(message.channel).arg(message.number);
    const bool pressed = message.type == "note" ? message.pressed : message.value >= 64;
    const bool rising = pressed && !held_.contains(identity);
    if (pressed) held_.insert(identity); else held_.remove(identity);
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
      const auto binding = it.value().toObject();
      if (binding.value("type").toString() != message.type ||
          binding.value("channel").toInt() != message.channel || binding.value("number").toInt() != message.number) continue;
      if (isVolume(it.key())) {
        const bool changed = !lastVolumes_.contains(identity) || lastVolumes_.value(identity) != message.value ||
            (message.minimumValue >= 0 && message.minimumValue != message.maximumValue);
        lastVolumes_.insert(identity, message.value);
        return execute && (changed || binding.value("mode").toString("absolute") != "absolute") ? it.key() : QString{};
      }
      return execute && rising ? it.key() : QString{};
    }
    return {};
  }
private:
  QSet<QString> held_;
  QMap<QString, int> lastVolumes_;
};
} // namespace vtrpon::midiControl
