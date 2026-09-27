#include "macro_document.h"

#include <cmath>

namespace {
constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr qint64 kMaximumDelayMs = 7 * 24 * 60 * 60 * 1000LL;

bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}

bool integer(const QJsonValue &value, qint64 minimum, qint64 maximum,
             qint64 *result, const QString &field, QString *error) {
  const double number = value.toDouble();
  if (!value.isDouble() || !std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum)) {
    return fail(error, QStringLiteral("%1 must be an integer from %2 to %3.")
                           .arg(field).arg(minimum).arg(maximum));
  }
  *result = static_cast<qint64>(number);
  return true;
}

bool itemId(const QJsonValue &value, quint64 *result, const QString &field,
            QString *error, bool allowZero) {
  bool ok = false;
  const QString text = value.toString();
  const quint64 parsed = text.toULongLong(&ok);
  if (!value.isString() || !ok || (!allowZero && parsed == 0) ||
      QString::number(parsed) != text) {
    return fail(error, QStringLiteral("%1 must be an integer ID string.").arg(field));
  }
  *result = parsed;
  return true;
}

QString typeName(MacroStepType type) {
  switch (type) {
  case MacroStepType::Cue: return QStringLiteral("cue");
  case MacroStepType::Take: return QStringLiteral("take");
  case MacroStepType::WaitUntil: return QStringLiteral("waitUntil");
  case MacroStepType::Delay: return QStringLiteral("delay");
  case MacroStepType::ItemSettings: return QStringLiteral("itemSettings");
  case MacroStepType::RuntimeSetting: return QStringLiteral("runtimeSetting");
  case MacroStepType::Transport: return QStringLiteral("transport");
  case MacroStepType::ManualWait: return QStringLiteral("manualWait");
  case MacroStepType::Note: return QStringLiteral("note");
  }
  return {};
}

bool parseType(const QString &name, MacroStepType *result) {
  for (MacroStepType type : {MacroStepType::Cue, MacroStepType::Take,
       MacroStepType::WaitUntil, MacroStepType::Delay, MacroStepType::ItemSettings,
       MacroStepType::RuntimeSetting, MacroStepType::Transport,
       MacroStepType::ManualWait, MacroStepType::Note}) {
    if (typeName(type) == name) { *result = type; return true; }
  }
  return false;
}

bool patchSettings(const PlaylistItemSettings &current, const QJsonObject &patch,
                   PlaylistItemSettings *result, QString *error,
                   bool checkCombinations) {
  PlaylistItemSettings edited = current;
  for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
    const QString &key = it.key();
    const QJsonValue value = it.value();
    qint64 number = 0;
    if (key == QStringLiteral("inPointMs") || key == QStringLiteral("outPointMs")) {
      if (key == QStringLiteral("outPointMs") && value.isNull()) {
        edited.outPointMs.reset();
      } else {
        if (!integer(value, 0, kMaximumExactJsonInteger, &number, key, error)) return false;
        if (key == QStringLiteral("inPointMs")) edited.inPointMs = number;
        else edited.outPointMs = number;
      }
    } else if (key == QStringLiteral("startMode")) {
      if (value == QJsonValue(QStringLiteral("pause"))) edited.startMode = PlaylistStartMode::Pause;
      else if (value == QJsonValue(QStringLiteral("play"))) edited.startMode = PlaylistStartMode::Play;
      else return fail(error, QStringLiteral("startMode must be pause or play."));
    } else if (key == QStringLiteral("endMode")) {
      const QString mode = value.toString();
      if (mode == QStringLiteral("stop")) edited.endMode = PlaylistEndMode::Stop;
      else if (mode == QStringLiteral("pause")) edited.endMode = PlaylistEndMode::Pause;
      else if (mode == QStringLiteral("repeat")) edited.endMode = PlaylistEndMode::Repeat;
      else if (mode == QStringLiteral("next")) edited.endMode = PlaylistEndMode::Next;
      else if (mode == QStringLiteral("goto")) edited.endMode = PlaylistEndMode::Goto;
      else return fail(error, QStringLiteral("Unsupported endMode."));
    } else if (key == QStringLiteral("repeatEndMode")) {
      const QString mode = value.toString();
      if (mode == QStringLiteral("stop")) edited.repeatEndMode = PlaylistRepeatEndMode::Stop;
      else if (mode == QStringLiteral("pause")) edited.repeatEndMode = PlaylistRepeatEndMode::Pause;
      else if (mode == QStringLiteral("next")) edited.repeatEndMode = PlaylistRepeatEndMode::Next;
      else if (mode == QStringLiteral("goto")) edited.repeatEndMode = PlaylistRepeatEndMode::Goto;
      else return fail(error, QStringLiteral("Unsupported repeatEndMode."));
    } else if (key == QStringLiteral("repeatCount")) {
      if (value.isNull()) edited.repeatCount = kInfiniteItemRepeatCount;
      else {
        if (!integer(value, 1, kMaximumItemRepeatCount, &number, key, error)) return false;
        edited.repeatCount = static_cast<int>(number);
      }
    } else if (key == QStringLiteral("gotoTargetItemId")) {
      if (value.isNull()) edited.gotoTargetItemId = 0;
      else if (!itemId(value, &edited.gotoTargetItemId, key, error, false)) return false;
    } else if (key == QStringLiteral("fadeIn") || key == QStringLiteral("fadeOut") ||
               key == QStringLiteral("endCrossfade")) {
      if (!value.isObject()) return fail(error, key + QStringLiteral(" must be an object."));
      bool *enabled = key == QStringLiteral("fadeIn") ? &edited.fadeInEnabled
          : key == QStringLiteral("fadeOut") ? &edited.fadeOutEnabled : &edited.endCrossfadeEnabled;
      qint64 *duration = key == QStringLiteral("fadeIn") ? &edited.fadeInDurationMs
          : key == QStringLiteral("fadeOut") ? &edited.fadeOutDurationMs : &edited.endCrossfadeDurationMs;
      const QJsonObject fade = value.toObject();
      for (auto field = fade.constBegin(); field != fade.constEnd(); ++field) {
        if (field.key() == QStringLiteral("enabled")) {
          if (!field.value().isBool()) return fail(error, key + QStringLiteral(".enabled must be boolean."));
          *enabled = field.value().toBool();
        } else if (field.key() == QStringLiteral("durationMs")) {
          if (!integer(field.value(), kMinimumItemFadeDurationMs, kMaximumItemFadeDurationMs,
                       duration, key + QStringLiteral(".durationMs"), error)) return false;
        } else return fail(error, QStringLiteral("Unknown setting %1.%2.").arg(key, field.key()));
      }
    } else if (key == QStringLiteral("nominalVolumeTenthsDb")) {
      if (!integer(value, kMinimumItemVolumeTenthsDb, kMaximumItemVolumeTenthsDb,
                   &number, key, error)) return false;
      edited.nominalVolumeTenthsDb = static_cast<int>(number);
    } else if (key == QStringLiteral("nominalAudioDelayFrames")) {
      if (!integer(value, 0, 6, &number, key, error)) return false;
      edited.nominalAudioDelayFrames = static_cast<int>(number);
    } else if (key == QStringLiteral("nominalEqLowTenthsDb") ||
               key == QStringLiteral("nominalEqMidTenthsDb") ||
               key == QStringLiteral("nominalEqHighTenthsDb")) {
      if (!integer(value, -vtrpon::kEqMaximumTenthsDb, vtrpon::kEqMaximumTenthsDb,
                   &number, key, error)) return false;
      const int band = key == QStringLiteral("nominalEqLowTenthsDb") ? 0
          : key == QStringLiteral("nominalEqMidTenthsDb") ? 1 : 2;
      edited.nominalEqTenthsDb[band] = static_cast<int>(number);
    } else return fail(error, QStringLiteral("Unknown item setting: %1.").arg(key));
  }
  if (checkCombinations) {
    if (edited.outPointMs && *edited.outPointMs <= edited.inPointMs)
      return fail(error, QStringLiteral("OUT must be after IN."));
    if ((edited.endMode == PlaylistEndMode::Goto ||
         (edited.endMode == PlaylistEndMode::Repeat && edited.repeatCount != kInfiniteItemRepeatCount &&
          edited.repeatEndMode == PlaylistRepeatEndMode::Goto)) && edited.gotoTargetItemId == 0)
      return fail(error, QStringLiteral("GOTO requires a target item."));
  }
  *result = edited;
  if (error) error->clear();
  return true;
}

bool validRuntime(const MacroStep &step, QString *error) {
  if (!std::isfinite(step.value)) return fail(error, QStringLiteral("Runtime value must be finite."));
  if (step.operation == QStringLiteral("masterVolume") || step.operation == QStringLiteral("itemVolume")) {
    qint64 parsed = 0;
    return integer(step.value, kMinimumItemVolumeTenthsDb, kMaximumItemVolumeTenthsDb,
                   &parsed, step.operation, error);
  }
  if (step.operation == QStringLiteral("mute")) {
    if (step.value == 0.0 || step.value == 1.0) return true;
    return fail(error, QStringLiteral("mute must be 0 or 1."));
  }
  if (step.operation == QStringLiteral("playbackRate")) {
    for (double rate : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0})
      if (step.value == rate) return true;
    return fail(error, QStringLiteral("Unsupported playback rate."));
  }
  return fail(error, QStringLiteral("Unknown runtime operation: %1.").arg(step.operation));
}
} // namespace

namespace MacroDocumentIo {
bool applySettingsPatch(const PlaylistItemSettings &current, const QJsonObject &patch,
                        PlaylistItemSettings *result, QString *error) {
  if (!result) return fail(error, QStringLiteral("No settings output was supplied."));
  return patchSettings(current, patch, result, error, true);
}

bool validateDefinitions(const QList<MacroDefinition> &definitions, QString *error) {
  if (definitions.size() > 1000) return fail(error, QStringLiteral("Too many macros."));
  QSet<QString> ids;
  for (const auto &macro : definitions) {
    if (QUuid(macro.id).isNull() || ids.contains(macro.id))
      return fail(error, QStringLiteral("Macro ID is invalid or duplicated: %1.").arg(macro.id));
    ids.insert(macro.id);
    if (macro.steps.size() > 10000) return fail(error, QStringLiteral("Too many macro steps."));
    for (qsizetype row = 0; row < macro.steps.size(); ++row) {
      const auto &step = macro.steps[row];
      const QString context = QStringLiteral("Macro %1, step %2: ").arg(macro.name).arg(row + 1);
      QString detail;
      if (QUuid(step.id).isNull() || ids.contains(step.id)) detail = QStringLiteral("Invalid or duplicate step ID.");
      else if (typeName(step.type).isEmpty()) detail = QStringLiteral("Unknown step type.");
      else if (!step.documentId.isEmpty() && QUuid(step.documentId).isNull()) detail = QStringLiteral("Invalid document ID.");
      else if (step.durationMs < 0 || step.durationMs > kMaximumDelayMs) detail = QStringLiteral("Delay must be between 0 and 7 days.");
      else if (step.crossfadeDurationMs < kMinimumItemFadeDurationMs ||
               step.crossfadeDurationMs > kMaximumItemFadeDurationMs) detail = QStringLiteral("Invalid crossfade duration.");
      else if (!std::isfinite(step.value)) detail = QStringLiteral("Value must be finite.");
      else if (step.type == MacroStepType::RuntimeSetting) validRuntime(step, &detail);
      else if (step.type == MacroStepType::Transport && step.operation != QStringLiteral("play") &&
               step.operation != QStringLiteral("pause") && step.operation != QStringLiteral("offAir"))
        detail = QStringLiteral("Unknown transport operation.");
      PlaylistItemSettings ignored;
      if (detail.isEmpty()) patchSettings({}, step.settingsPatch, &ignored, &detail, false);
      if (!detail.isEmpty()) return fail(error, context + detail);
      ids.insert(step.id);
    }
  }
  if (error) error->clear();
  return true;
}

QStringList referenceErrors(const QList<MacroDefinition> &definitions,
                            const QString &documentId, const QSet<quint64> &itemIds) {
  QStringList errors;
  for (const auto &macro : definitions) {
    for (qsizetype row = 0; row < macro.steps.size(); ++row) {
      const auto &step = macro.steps[row];
      if (!step.enabled) continue;
      const QString context = QStringLiteral("Macro %1, step %2: ").arg(macro.name).arg(row + 1);
      if (step.type == MacroStepType::Cue || step.type == MacroStepType::Take ||
          step.type == MacroStepType::ItemSettings) {
        if (step.documentId != documentId) errors.append(context + QStringLiteral("Target belongs to a different or unassigned document."));
        else if (step.itemId == 0 || !itemIds.contains(step.itemId))
          errors.append(context + QStringLiteral("Target item %1 is missing.").arg(step.itemId));
      }
      const QJsonValue target = step.settingsPatch.value(QStringLiteral("gotoTargetItemId"));
      if (step.type == MacroStepType::ItemSettings && target.isString() &&
          (step.documentId != documentId || !itemIds.contains(target.toString().toULongLong())))
        errors.append(context + QStringLiteral("GOTO target is missing or belongs to another document."));
    }
  }
  return errors;
}

QJsonArray toJson(const QList<MacroDefinition> &definitions) {
  QJsonArray macros;
  for (const auto &macro : definitions) {
    QJsonArray steps;
    for (const auto &step : macro.steps) {
      steps.append(QJsonObject{
          {QStringLiteral("id"), step.id}, {QStringLiteral("enabled"), step.enabled},
          {QStringLiteral("type"), typeName(step.type)},
          {QStringLiteral("itemId"), QString::number(step.itemId)},
          {QStringLiteral("documentId"), step.documentId}, {QStringLiteral("label"), step.label},
          {QStringLiteral("scheduledAt"), step.scheduledAt.isValid()
               ? QJsonValue(step.scheduledAt.toOffsetFromUtc(step.scheduledAt.offsetFromUtc()).toString(Qt::ISODateWithMs))
               : QJsonValue(QJsonValue::Null)},
          {QStringLiteral("durationMs"), step.durationMs}, {QStringLiteral("requireReady"), step.requireReady},
          {QStringLiteral("crossfade"), step.crossfade},
          {QStringLiteral("crossfadeDurationMs"), step.crossfadeDurationMs},
          {QStringLiteral("settingsPatch"), step.settingsPatch},
          {QStringLiteral("operation"), step.operation}, {QStringLiteral("value"), step.value}});
    }
    macros.append(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("id"), macro.id},
        {QStringLiteral("name"), macro.name}, {QStringLiteral("steps"), steps}});
  }
  return macros;
}

std::optional<QList<MacroDefinition>> fromJson(const QJsonValue &value, QString *error) {
  if (value.isUndefined()) { if (error) error->clear(); return QList<MacroDefinition>{}; }
  if (!value.isArray()) { fail(error, QStringLiteral("macros must be an array.")); return std::nullopt; }
  QList<MacroDefinition> definitions;
  for (const QJsonValue &entry : value.toArray()) {
    const QJsonObject object = entry.toObject();
    if (!entry.isObject() || object.value(QStringLiteral("version")) != QJsonValue(1) ||
        !object.value(QStringLiteral("id")).isString() || !object.value(QStringLiteral("name")).isString() ||
        !object.value(QStringLiteral("steps")).isArray()) {
      fail(error, QStringLiteral("Invalid or unsupported macro document format.")); return std::nullopt;
    }
    MacroDefinition macro;
    macro.id = object.value(QStringLiteral("id")).toString();
    macro.name = object.value(QStringLiteral("name")).toString();
    for (const QJsonValue &entryStep : object.value(QStringLiteral("steps")).toArray()) {
      const QJsonObject json = entryStep.toObject();
      MacroStep step;
      auto invalid = [&]() -> std::optional<QList<MacroDefinition>> {
        fail(error, QStringLiteral("Invalid macro step %1 in %2.").arg(macro.steps.size() + 1).arg(macro.name));
        return std::nullopt;
      };
      if (!entryStep.isObject()) return invalid();
      for (const auto key : {"id", "type", "documentId", "label", "operation"})
        if (!json.value(QLatin1String(key)).isString()) return invalid();
      for (const auto key : {"enabled", "requireReady", "crossfade"})
        if (!json.value(QLatin1String(key)).isBool()) return invalid();
      if (!json.value(QStringLiteral("settingsPatch")).isObject() ||
          !json.value(QStringLiteral("value")).isDouble() ||
          !parseType(json.value(QStringLiteral("type")).toString(), &step.type)) return invalid();
      step.id = json.value(QStringLiteral("id")).toString();
      step.documentId = json.value(QStringLiteral("documentId")).toString();
      step.label = json.value(QStringLiteral("label")).toString();
      step.operation = json.value(QStringLiteral("operation")).toString();
      step.enabled = json.value(QStringLiteral("enabled")).toBool();
      step.requireReady = json.value(QStringLiteral("requireReady")).toBool();
      step.crossfade = json.value(QStringLiteral("crossfade")).toBool();
      step.value = json.value(QStringLiteral("value")).toDouble();
      step.settingsPatch = json.value(QStringLiteral("settingsPatch")).toObject();
      if (!itemId(json.value(QStringLiteral("itemId")), &step.itemId, QStringLiteral("itemId"), error, true) ||
          !integer(json.value(QStringLiteral("durationMs")), 0, kMaximumDelayMs, &step.durationMs,
                   QStringLiteral("durationMs"), error) ||
          !integer(json.value(QStringLiteral("crossfadeDurationMs")), kMinimumItemFadeDurationMs,
                   kMaximumItemFadeDurationMs, &step.crossfadeDurationMs, QStringLiteral("crossfadeDurationMs"), error))
        return std::nullopt;
      const QJsonValue scheduled = json.value(QStringLiteral("scheduledAt"));
      if (!scheduled.isNull()) {
        if (!scheduled.isString()) return invalid();
        step.scheduledAt = QDateTime::fromString(scheduled.toString(), Qt::ISODateWithMs);
        if (!step.scheduledAt.isValid() || step.scheduledAt.timeSpec() == Qt::LocalTime) return invalid();
      }
      macro.steps.append(step);
    }
    definitions.append(macro);
  }
  if (!validateDefinitions(definitions, error)) return std::nullopt;
  return definitions;
}
} // namespace MacroDocumentIo
