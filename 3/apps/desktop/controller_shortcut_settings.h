#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QSettings>
#include <QMap>
#include <QStringList>

namespace vtrpon::shortcutSettings {
  enum class Action {
    AddFile,
    Exit,
    TakeOnAir,
    ToggleOnAirPlayback,
    LoadEdit,
    CopyItemState,
    PasteItemState,
    DocumentNext,
    DocumentPrevious,
    ToggleFtb,
    ToggleDsk,
    OffAir,
    MasterFadeIn,
    MasterFadeOut,
    ItemFadeIn,
    ItemFadeOut,
    BgmFadeIn,
    BgmFadeOut,
    // Rows 18-35 are the existing item/playlist digit bindings.
    PlaylistPlayOnce = 36,
    PlaylistRepeatAll,
    PlaylistShuffle,
    PlaylistBgmMode,
    PlaylistStandardMode,
    PlaylistClear,
    PlaylistUndo,
    EditToggleStartMode, EditEndStop, EditEndPause, EditEndRepeat, EditEndNext,
    EditToggleFadeOut, EditReset, EditSetIn, EditSetOut,
    NextOutputDisplay,
    PreviewCapture, ProgramCapture, TogglePfl, PlaylistDelete,
    OperatorTimerStart, OperatorTimerPause, OperatorTimerReset, OperatorTimerSettings,
    OpenNominalEq, OpenItemEq, OpenMasterEq
  };


struct Definition { QString id; QList<QKeySequence> keys; };
// Controller-wide fixed keys are outside the user-editable bindings, but are
// also operations that external controllers can assign.
struct FixedDefinition { QString id; QKeySequence key; };
inline const QList<FixedDefinition> &fixedDefinitions() {
  static const QList<FixedDefinition> result{
    {QStringLiteral("SelectPreviousItem"), QKeySequence(Qt::Key_Up)},
    {QStringLiteral("SelectNextItem"), QKeySequence(Qt::Key_Down)},
    {QStringLiteral("RemoveSelectedItem"), QKeySequence(Qt::Key_Delete)},
    {QStringLiteral("OpenSettings"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S)},
    {QStringLiteral("IncreaseItemVolume"), QKeySequence(Qt::CTRL | Qt::Key_Up)},
    {QStringLiteral("DecreaseItemVolume"), QKeySequence(Qt::CTRL | Qt::Key_Down)},
    {QStringLiteral("IncreaseMasterVolume"), QKeySequence(Qt::SHIFT | Qt::Key_Up)},
    {QStringLiteral("DecreaseMasterVolume"), QKeySequence(Qt::SHIFT | Qt::Key_Down)}
  };
  return result;
}
inline const QList<Definition> &definitions() {
  static const QList<Definition> result = [] {
    QList<Definition> rows{
      {QStringLiteral("AddFile"), {QKeySequence(Qt::CTRL | Qt::Key_F)}},
      {QStringLiteral("Exit"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q)}},
      {QStringLiteral("TakeOnAir"), {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}},
      {QStringLiteral("ToggleOnAirPlayback"), {QKeySequence(Qt::Key_Space)}},
      {QStringLiteral("LoadEdit"), {QKeySequence(Qt::Key_E)}},
      {QStringLiteral("CopyItemState"), {QKeySequence(QKeySequence::Copy)}},
      {QStringLiteral("PasteItemState"), {QKeySequence(QKeySequence::Paste)}},
      {QStringLiteral("DocumentNext"), {QKeySequence(Qt::Key_PageUp), QKeySequence(Qt::Key_Right)}},
      {QStringLiteral("DocumentPrevious"), {QKeySequence(Qt::Key_PageDown), QKeySequence(Qt::Key_Left)}},
      {QStringLiteral("ToggleFtb"), {QKeySequence(Qt::SHIFT | Qt::Key_F)}},
      {QStringLiteral("ToggleDsk"), {QKeySequence(Qt::SHIFT | Qt::Key_D)}},
      {QStringLiteral("OffAir"), {QKeySequence(Qt::Key_Escape)}},
      {QStringLiteral("MasterFadeIn"), {QKeySequence(Qt::CTRL | Qt::Key_Comma)}},
      {QStringLiteral("MasterFadeOut"), {QKeySequence(Qt::CTRL | Qt::Key_Period)}},
      {QStringLiteral("ItemFadeIn"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Comma)}},
      {QStringLiteral("ItemFadeOut"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Period)}},
      {QStringLiteral("BgmFadeIn"), {QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Comma)}},
      {QStringLiteral("BgmFadeOut"), {QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Period)}},
    };
    for (int digit = 1; digit <= 9; ++digit) {
      rows.append({QStringLiteral("SelectItem%1").arg(digit), {QKeySequence(Qt::Key_0 + digit)}});
      rows.append({QStringLiteral("ActivatePlaylist%1").arg(digit), {QKeySequence(Qt::CTRL | (Qt::Key_0 + digit))}});
    }
    rows.append({QStringLiteral("PlaylistPlayOnce"), {QKeySequence(Qt::CTRL | Qt::Key_L)}});
    rows.append({QStringLiteral("PlaylistRepeatAll"), {QKeySequence(Qt::CTRL | Qt::Key_R)}});
    rows.append({QStringLiteral("PlaylistShuffle"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X)}});
    rows.append({QStringLiteral("PlaylistBgmMode"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)}});
    rows.append({QStringLiteral("PlaylistStandardMode"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B)}});
    rows.append({QStringLiteral("PlaylistClear"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)}});
    rows.append({QStringLiteral("PlaylistUndo"), {QKeySequence(Qt::CTRL | Qt::Key_Z)}});
    rows.append({QStringLiteral("EditToggleStartMode"), {QKeySequence(Qt::ALT | Qt::Key_S)}});
    rows.append({QStringLiteral("EditEndStop"), {QKeySequence(Qt::ALT | Qt::Key_O)}});
    rows.append({QStringLiteral("EditEndPause"), {QKeySequence(Qt::ALT | Qt::Key_P)}});
    rows.append({QStringLiteral("EditEndRepeat"), {QKeySequence(Qt::ALT | Qt::Key_R)}});
    rows.append({QStringLiteral("EditEndNext"), {QKeySequence(Qt::ALT | Qt::Key_N)}});
    rows.append({QStringLiteral("EditToggleFadeOut"), {QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_F)}});
    rows.append({QStringLiteral("EditReset"), {QKeySequence(Qt::ALT | Qt::Key_C)}});
    rows.append({QStringLiteral("EditSetIn"), {QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_I)}});
    rows.append({QStringLiteral("EditSetOut"), {QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_O)}});
    rows.append({QStringLiteral("NextOutputDisplay"), {QKeySequence(Qt::ALT | Qt::Key_W)}});
    rows.append({QStringLiteral("PreviewCapture"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)}});
    rows.append({QStringLiteral("ProgramCapture"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G)}});
    rows.append({QStringLiteral("TogglePfl"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_P)}});
    rows.append({QStringLiteral("PlaylistDelete"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Delete)}});
    rows.append({QStringLiteral("OperatorTimerStart"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T)}});
    rows.append({QStringLiteral("OperatorTimerPause"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Y)}});
    rows.append({QStringLiteral("OperatorTimerReset"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R)}});
    rows.append({QStringLiteral("OperatorTimerSettings"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_O)}});
    rows.append({QStringLiteral("OpenNominalEq"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_E)}});
    rows.append({QStringLiteral("OpenItemEq"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)}});
    rows.append({QStringLiteral("OpenMasterEq"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_M)}});
    return rows;
  }();
  return result;
}
// Keep stable action indices for runtime bindings; order the settings list by UI proximity.
inline const QList<int> &displayOrder() {
  static const QList<int> result = [] {
    QList<int> rows;
    const auto append = [&rows](Action action) { rows.append(static_cast<int>(action)); };
    for (int index = 0; index <= static_cast<int>(Action::NextOutputDisplay); ++index) {
      rows.append(index);
      switch (static_cast<Action>(index)) {
      case Action::EditSetOut:
        append(Action::PreviewCapture); append(Action::ProgramCapture); break;
      case Action::LoadEdit:
        append(Action::TogglePfl); append(Action::OpenNominalEq); break;
      case Action::OffAir:
        append(Action::OperatorTimerStart); append(Action::OperatorTimerPause);
        append(Action::OperatorTimerReset); append(Action::OperatorTimerSettings); break;
      case Action::MasterFadeOut: append(Action::OpenMasterEq); break;
      case Action::ItemFadeOut: append(Action::OpenItemEq); break;
      case Action::PlaylistClear: append(Action::PlaylistDelete); break;
      default: break;
      }
    }
    return rows;
  }();
  return result;
}
inline QString key() { return QStringLiteral("shortcuts/bindings"); }
inline QJsonObject defaults() {
  QJsonObject result;
  for (const auto &row : definitions()) {
    QJsonArray keys;
    for (const auto &sequence : row.keys) keys.append(sequence.toString(QKeySequence::PortableText));
    result.insert(row.id, keys);
  }
  return result;
}
// Empty arrays disable actions; missing entries retain their default bindings.
inline QJsonObject resolved(const QJsonObject &overrides) {
  auto result = defaults();
  for (auto it = overrides.begin(); it != overrides.end(); ++it) result.insert(it.key(), it.value());
  // Preserve older custom bindings when a newly introduced default uses their key.
  for (int index = static_cast<int>(Action::PlaylistPlayOnce); index < definitions().size(); ++index) {
    const auto &id = definitions().at(index).id;
    if (overrides.contains(id)) continue;
    QJsonArray available;
    for (const auto &candidate : result.value(id).toArray()) {
      bool occupied = false;
      for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (it.value().toArray().contains(candidate)) { occupied = true; break; }
      }
      if (!occupied) available.append(candidate);
    }
    result.insert(id, available);
  }
  return result;
}
inline QString validationError(const QJsonObject &overrides) {
  const auto base = defaults();
  for (auto it = overrides.begin(); it != overrides.end(); ++it)
    if (!base.contains(it.key())) return it.key();
  const auto values = resolved(overrides);
  QMap<QString, QString> used;
  for (const auto &reserved : {"Ctrl+Alt+S", "Ctrl+Up", "Ctrl+Down", "Shift+Up", "Shift+Down"})
    used.insert(QString::fromLatin1(reserved), QStringLiteral("reserved"));
  for (auto it = values.begin(); it != values.end(); ++it) {
    if (!it.value().isArray() || it.value().toArray().size() > 2) return it.key();
    for (const auto &entry : it.value().toArray()) {
      if (!entry.isString() || entry.toString().isEmpty()) return it.key();
      const auto sequence = QKeySequence::fromString(entry.toString(), QKeySequence::PortableText);
      if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown ||
          sequence[0].key() == 0 || sequence.toString(QKeySequence::PortableText) != entry.toString()) return it.key();
      const auto text = sequence.toString(QKeySequence::PortableText);
      if (used.contains(text)) return it.key() + QStringLiteral(" / ") + used.value(text) + QStringLiteral(" : ") + text;
      used.insert(text, it.key());
    }
  }
  return {};
}
inline QJsonObject load(const QSettings &settings) {
  const auto value = QJsonObject::fromVariantMap(settings.value(key()).toMap());
  return validationError(value).isEmpty() ? resolved(value) : defaults();
}
inline QList<QKeySequence> sequences(const QJsonObject &values, int index) {
  QList<QKeySequence> result;
  for (const auto &value : values.value(definitions().at(index).id).toArray())
    result.append(QKeySequence::fromString(value.toString(), QKeySequence::PortableText));
  return result;
}
}
