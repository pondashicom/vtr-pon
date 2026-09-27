#pragma once

#include "playlist_item.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <optional>

inline QString newMacroId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

enum class MacroStepType {
  Cue, Take, WaitUntil, Delay, ItemSettings, RuntimeSetting, Transport,
  ManualWait, Note
};

struct MacroStep {
  QString id = newMacroId();
  bool enabled = true;
  MacroStepType type = MacroStepType::Note;
  quint64 itemId = 0;
  QString documentId;
  QString label;
  QDateTime scheduledAt;
  qint64 durationMs = 1000;
  bool requireReady = false;
  bool crossfade = false;
  qint64 crossfadeDurationMs = 1000;
  QJsonObject settingsPatch;
  QString operation;
  double value = 0.0;

  bool operator==(const MacroStep &) const = default;
};

struct MacroDefinition {
  QString id = newMacroId();
  QString name;
  QList<MacroStep> steps;

  bool operator==(const MacroDefinition &) const = default;
};

namespace MacroDocumentIo {
QJsonArray toJson(const QList<MacroDefinition> &definitions);
std::optional<QList<MacroDefinition>> fromJson(const QJsonValue &value,
                                             QString *error = nullptr);
// Structural validation permits incomplete/dangling references in saved drafts.
bool validateDefinitions(const QList<MacroDefinition> &definitions,
                         QString *error = nullptr);
QStringList referenceErrors(const QList<MacroDefinition> &definitions,
                            const QString &documentId,
                            const QSet<quint64> &itemIds);
// Field names follow playlist JSON; fadeIn/fadeOut/endCrossfade are partial
// objects {enabled, durationMs}; outPointMs/repeatCount/gotoTargetItemId allow null.
// On failure neither current nor result is modified.
bool applySettingsPatch(const PlaylistItemSettings &current,
                        const QJsonObject &patch,
                        PlaylistItemSettings *result,
                        QString *error = nullptr);
} // namespace MacroDocumentIo
