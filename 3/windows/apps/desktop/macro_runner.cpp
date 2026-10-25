#include "macro_runner.h"
#include "playlist_model.h"

#include <QDebug>
#include <QScopedValueRollback>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 kAllowedLatenessMs = 1000;
constexpr qint64 kRequestTimeoutMs = 30000;
bool usesItem(const MacroStep &step) {
  return step.type == MacroStepType::Cue || step.type == MacroStepType::Take ||
         step.type == MacroStepType::ItemSettings;
}
QDateTime takeDeadline(const MacroDefinition &definition, int index) {
  const auto &step = definition.steps.at(index);
  if (!step.requireReady) return {};
  for (int i = index - 1; i >= 0; --i) {
    const auto &previous = definition.steps.at(i);
    if (!previous.enabled) continue;
    if (previous.type == MacroStepType::Take) break;
    if (previous.type == MacroStepType::WaitUntil) return previous.scheduledAt;
  }
  return step.scheduledAt;
}
}

MacroRunner::MacroRunner(PlaylistModel *model, ControllerOperations *operations,
                         Clock clock, QObject *parent)
    : QObject(parent), model_(model), operations_(operations), clock_(std::move(clock)) {
  elapsed_.start();
  timer_.setInterval(25);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, &MacroRunner::poll);
  connect(operations, &ControllerOperations::operationAboutToExecute,
          this, &MacroRunner::beforeOperation);
  connect(model, &PlaylistModel::itemChanged, this, &MacroRunner::itemChanged);
  connect(model, &PlaylistModel::itemRemoved, this,
          [this](quint64 id, int) { itemChanged(id); });
  connect(model, &PlaylistModel::itemReplaced, this,
          [this](quint64 id) { if (!ownModelChange_ && referencesItem(id))
            interrupt(QStringLiteral("Referenced media was replaced / 参照素材が置換されました")); });
  connect(model, &PlaylistModel::itemMediaAvailabilityChanged, this,
          [this](quint64 id, bool offline) { if (offline && referencesItem(id))
            interrupt(QStringLiteral("Referenced media is offline / 参照素材がオフラインです")); });
  connect(model, &PlaylistModel::modelReset, this, [this] {
    if (isActive()) fail(QStringLiteral("Playlist document changed / プレイリスト文書が変更されました"));
  });
  connect(model, &QObject::destroyed, this, [this] { stop(); });
}

MacroRunner::~MacroRunner() { stop(); }
bool MacroRunner::isActive() const {
  return state_ == State::Running || state_ == State::Waiting || state_ == State::Paused;
}
QDateTime MacroRunner::currentDateTime() const {
  return clock_.wallNow ? clock_.wallNow() : QDateTime::currentDateTime();
}
qint64 MacroRunner::monotonicNow() const {
  return clock_.monotonicNow ? clock_.monotonicNow() : elapsed_.elapsed();
}
qint64 MacroRunner::remainingMs() const {
  if (!isActive() || stepIndex_ < 0 || stepIndex_ >= definition_.steps.size()) return 0;
  const auto &step = definition_.steps.at(stepIndex_);
  if (step.type == MacroStepType::Delay && stepStarted_)
    return state_ == State::Paused ? pausedDelayMs_ : (std::max)(qint64{0}, delayDeadlineMs_ - monotonicNow());
  if (step.type == MacroStepType::WaitUntil)
    return (std::max)(qint64{0}, currentDateTime().msecsTo(step.scheduledAt));
  return 0;
}
QString MacroRunner::statusText() const {
  QString state;
  switch (state_) {
  case State::Stopped: state = QStringLiteral("停止 / Stopped"); break;
  case State::Running: state = QStringLiteral("実行中 / Running"); break;
  case State::Waiting: state = QStringLiteral("待機中 / Waiting"); break;
  case State::Paused: state = QStringLiteral("一時停止 / Paused"); break;
  case State::Completed: state = QStringLiteral("完了 / Completed"); break;
  case State::Error: state = QStringLiteral("エラー / Error"); break;
  }
  return detail_.isEmpty() ? state : state + QStringLiteral(" — ") + detail_;
}
void MacroRunner::setState(State state) {
  if (state_ == state) return;
  state_ = state;
  emit stateChanged();
}
void MacroRunner::appendLog(const QString &result) {
  const auto step = stepIndex_ >= 0 && stepIndex_ < definition_.steps.size()
      ? definition_.steps.at(stepIndex_) : MacroStep{};
  const QString line = QStringLiteral("%1 [%2] #%3 item=%4 request=%5 %6")
      .arg(currentDateTime().toString(Qt::ISODateWithMs), definition_.name)
      .arg(stepIndex_ + 1).arg(step.itemId).arg(pendingRequest_).arg(result);
  qInfo().noquote() << "MACRO" << definition_.id << step.id << line;
  log_.append(line);
  if (log_.size() > 500) log_.removeFirst();
  emit logAdded(line);
}

bool MacroRunner::checkTarget(const MacroStep &step, QString *error, bool checkSettings) const {
  const auto reject = [error](const QString &message) { if (error) *error = message; return false; };
  if (!model_) return reject(QStringLiteral("Playlist unavailable / プレイリストを利用できません"));
  if (!usesItem(step)) return true;
  if (step.documentId != model_->documentId())
    return reject(QStringLiteral("Different playlist document / 別のプレイリスト文書を参照しています"));
  const auto item = model_->itemState(step.itemId);
  if (!item) return reject(QStringLiteral("Missing target item / 対象項目がありません"));
  if (model_->itemMediaOffline(step.itemId) || model_->itemConversionInProgress(step.itemId))
    return reject(QStringLiteral("Target is offline or importing / 対象がオフラインまたは読込中です"));
  if (item->sourceType != PlaylistSourceType::File)
    return reject(QStringLiteral("Unsupported source / この素材形式はマクロ未対応です"));
  if (step.type != MacroStepType::ItemSettings &&
      (model_->playlistMode(item->playlistNumber) != PlaylistMode::Standard ||
       (item->mediaType != PlaylistMediaType::Video && item->mediaType != PlaylistMediaType::Audio &&
        item->mediaType != PlaylistMediaType::StillImage)))
    return reject(QStringLiteral("Select a standard video, audio or still item / 通常の動画・音声・静止画を選択してください"));
  if (step.type == MacroStepType::ItemSettings) {
    if (model_->itemSettingsLocked(step.itemId))
      return reject(QStringLiteral("Item settings are locked / 項目設定がロックされています"));
    if (!checkSettings) return true;
    PlaylistItemSettings settings;
    if (!MacroDocumentIo::applySettingsPatch(item->settings, step.settingsPatch, &settings, error)) return false;
    if (model_->itemEditPointsLocked(step.itemId) &&
        (settings.inPointMs != item->settings.inPointMs || settings.outPointMs != item->settings.outPointMs))
      return reject(QStringLiteral("In/Out are locked during playout / 送出中のIn/Outは変更できません"));
    if (settings.gotoTargetItemId && !model_->itemState(settings.gotoTargetItemId))
      return reject(QStringLiteral("GOTO target is missing / GOTO先がありません"));
    if (!model_->itemSettingsWithinKnownDuration(step.itemId, settings))
      return reject(QStringLiteral("In/Out exceeds the known media duration / In/Outが素材尺を超えています"));
  }
  return true;
}

QStringList MacroRunner::validate(const MacroDefinition &definition) const {
  QStringList errors;
  QString structural;
  if (!MacroDocumentIo::validateDefinitions({definition}, &structural)) errors.append(structural);
  int enabled = 0;
  QDateTime precedingTime;
  QHash<quint64, PlaylistItemSettings> predictedSettings;
  for (int i = 0; i < definition.steps.size(); ++i) {
    const auto &step = definition.steps.at(i);
    if (!step.enabled) continue;
    ++enabled;
    QString error;
    if (!checkTarget(step, &error, false)) errors.append(QStringLiteral("#%1: %2").arg(i + 1).arg(error));
    else if (step.type == MacroStepType::ItemSettings) {
      const auto current = predictedSettings.value(step.itemId, model_->itemState(step.itemId)->settings);
      PlaylistItemSettings predicted;
      if (!MacroDocumentIo::applySettingsPatch(current, step.settingsPatch, &predicted, &error))
        errors.append(QStringLiteral("#%1: %2").arg(i + 1).arg(error));
      else if (model_->itemEditPointsLocked(step.itemId) &&
               (predicted.inPointMs != current.inPointMs || predicted.outPointMs != current.outPointMs))
        errors.append(QStringLiteral("#%1: 送出中のIn/Outは変更できません / In/Out are locked").arg(i + 1));
      else if (predicted.gotoTargetItemId && !model_->itemState(predicted.gotoTargetItemId))
        errors.append(QStringLiteral("#%1: GOTO先がありません / GOTO target missing").arg(i + 1));
      else if (!model_->itemSettingsWithinKnownDuration(step.itemId, predicted))
        errors.append(QStringLiteral("#%1: In/Outが素材尺を超えています / In/Out exceeds the known media duration").arg(i + 1));
      else predictedSettings.insert(step.itemId, predicted);
    }
    if (step.type == MacroStepType::Take) {
      const auto deadline = takeDeadline(definition, i);
      if (deadline.isValid() && deadline < currentDateTime())
        errors.append(QStringLiteral("#%1: 送出時刻が過去です / TAKE time is in the past").arg(i + 1));
    }
    if (step.type == MacroStepType::WaitUntil) {
      if (!step.scheduledAt.isValid() || step.scheduledAt < currentDateTime())
        errors.append(QStringLiteral("#%1: 時刻が未設定または過去です / Scheduled time is invalid or past").arg(i + 1));
      if (precedingTime.isValid() && step.scheduledAt < precedingTime)
        errors.append(QStringLiteral("#%1: 予約時刻の順序が逆です / Schedule runs backwards").arg(i + 1));
      precedingTime = step.scheduledAt;
    }
  }
  if (!enabled) errors.append(QStringLiteral("有効なステップがありません / No enabled steps"));
  return errors;
}

bool MacroRunner::start(const MacroDefinition &definition) {
  if (isActive()) return false;
  const auto errors = validate(definition);
  if (!errors.isEmpty()) {
    definition_ = definition;
    detail_ = errors.join(QLatin1Char('\n'));
    setState(State::Error);
    emit progressChanged();
    return false;
  }
  definition_ = definition;
  stepIndex_ = 0;
  stepStarted_ = false;
  manualWait_ = false;
  detail_.clear();
  log_.clear();
  refreshObservedItems();
  lastWall_ = currentDateTime();
  lastMono_ = monotonicNow();
  setState(State::Running);
  appendLog(QStringLiteral("開始 / Started"));
  timer_.start();
  emit progressChanged();
  return true;
}

void MacroRunner::cancelPending() {
  pendingRequest_ = 0; // Invalidate before cancellation may synchronously report completion.
  ++sequence_;
  if (operations_) (void)operations_->executeFrom("Macro", ControllerOperations::Action::CancelMacroRequests);
}
void MacroRunner::stop() {
  timer_.stop();
  const bool wasActive = isActive();
  setState(State::Stopped);
  cancelPending();
  stepStarted_ = false;
  manualWait_ = false;
  detail_.clear();
  if (wasActive) appendLog(QStringLiteral("停止 / Stopped"));
  emit progressChanged();
}
void MacroRunner::pause() { interrupt(QStringLiteral("操作による一時停止 / Paused by operator")); }
void MacroRunner::interrupt(const QString &reason) {
  if (!isActive() || state_ == State::Paused) return;
  pausedFrom_ = state_;
  pausedDelayMs_ = remainingMs();
  const bool awaitingOperation = pendingRequest_ != 0;
  detail_ = reason;
  setState(State::Paused);
  timer_.stop();
  cancelPending();
  const auto type = stepIndex_ >= 0 && stepIndex_ < definition_.steps.size()
      ? definition_.steps.at(stepIndex_).type : MacroStepType::Note;
  if (awaitingOperation || (type != MacroStepType::Delay && type != MacroStepType::WaitUntil &&
                           type != MacroStepType::ManualWait)) stepStarted_ = false;
  appendLog(reason);
  emit progressChanged();
}
void MacroRunner::resume() {
  if (state_ != State::Paused) return;
  for (int i = stepIndex_; i < definition_.steps.size(); ++i) {
    const auto &step = definition_.steps.at(i);
    if (step.enabled && step.type == MacroStepType::WaitUntil && step.scheduledAt < currentDateTime()) {
      fail(QStringLiteral("予約時刻を過ぎました。日時を設定し直してください / Reschedule expired cue"));
      return;
    }
  }
  QString error;
  if (stepIndex_ >= 0 && stepIndex_ < definition_.steps.size() &&
      !checkTarget(definition_.steps.at(stepIndex_), &error)) { fail(error); return; }
  // A resume is an explicit acknowledgment of intervening edits; take a fresh
  // comparison baseline, while retaining the immutable macro definition.
  refreshObservedItems();
  detail_.clear();
  lastWall_ = currentDateTime();
  lastMono_ = monotonicNow();
  delayDeadlineMs_ = lastMono_ + pausedDelayMs_;
  setState(pausedFrom_ == State::Waiting ? State::Waiting : State::Running);
  if (manualWait_) { manualWait_ = false; advance(); }
  appendLog(QStringLiteral("再開 / Resumed"));
  timer_.start();
  emit progressChanged();
}
void MacroRunner::fail(const QString &reason) {
  timer_.stop();
  detail_ = reason;
  setState(State::Error);
  cancelPending();
  appendLog(QStringLiteral("失敗 / Failed: ") + reason);
  emit progressChanged();
}
void MacroRunner::advance() {
  appendLog(QStringLiteral("ステップ完了 / Step completed"));
  pendingRequest_ = 0;
  stepStarted_ = false;
  ++stepIndex_;
  detail_.clear();
  setState(State::Running);
  emit progressChanged();
}
void MacroRunner::finishRequest(quint64 requestId, bool success, const QString &detail) {
  if (!isActive() || state_ == State::Paused || !pendingRequest_ || pendingRequest_ != requestId) return;
  if (!success) { fail(detail); return; }
  advance();
}

void MacroRunner::poll() {
  if (!isActive() || state_ == State::Paused || dispatching_) return;
  const auto now = currentDateTime();
  const auto mono = monotonicNow();
  const auto elapsed = mono - lastMono_;
  const auto wallElapsed = lastWall_.msecsTo(now);
  const bool discontinuity = lastWall_.isValid() &&
      (elapsed < 0 || elapsed > 2000 || std::abs(wallElapsed - elapsed) > 1000 ||
       lastWall_.timeZone() != now.timeZone());
  lastWall_ = now;
  lastMono_ = mono;
  if (discontinuity) { interrupt(QStringLiteral("時計変更または長い中断を検出 / Clock change or long suspension detected")); return; }
  if (pendingRequest_) {
    if (mono - requestStartedMs_ >= kRequestTimeoutMs) fail(QStringLiteral("準備・操作のタイムアウト / Operation timed out"));
    return;
  }
  while (stepIndex_ < definition_.steps.size() && !definition_.steps.at(stepIndex_).enabled) ++stepIndex_;
  if (stepIndex_ >= definition_.steps.size()) {
    timer_.stop();
    setState(State::Completed);
    cancelPending();
    appendLog(QStringLiteral("マクロ完了 / Macro completed"));
    emit progressChanged();
    return;
  }
  const auto &step = definition_.steps.at(stepIndex_);
  if (stepStarted_) {
    if (step.type == MacroStepType::Delay && mono >= delayDeadlineMs_) advance();
    else if (step.type == MacroStepType::WaitUntil && now >= step.scheduledAt) {
      if (step.scheduledAt.msecsTo(now) > kAllowedLatenessMs)
        fail(QStringLiteral("指定時刻に間に合いませんでした / Scheduled time missed"));
      else advance();
    }
    emit progressChanged();
    return;
  }
  executeStep();
}

void MacroRunner::executeStep() {
  const auto step = definition_.steps.at(stepIndex_);
  QString error;
  if (!checkTarget(step, &error)) { fail(error); return; }
  const auto deadline = takeDeadline(definition_, stepIndex_);
  if (step.type == MacroStepType::Take && deadline.isValid() &&
      (currentDateTime() < deadline || deadline.msecsTo(currentDateTime()) > kAllowedLatenessMs)) {
    fail(QStringLiteral("時刻送出の期限外です / Outside scheduled TAKE time")); return;
  }
  stepStarted_ = true;
  appendLog(QStringLiteral("実行 / Dispatch%1").arg(step.scheduledAt.isValid()
      ? QStringLiteral(" scheduled=") + step.scheduledAt.toString(Qt::ISODateWithMs) : QString()));
  using T = MacroStepType;
  using A = ControllerOperations::Action;
  if (step.type == T::Delay) {
    delayDeadlineMs_ = monotonicNow() + step.durationMs;
    setState(State::Waiting);
    return;
  }
  if (step.type == T::WaitUntil) { setState(State::Waiting); return; }
  if (step.type == T::ManualWait) {
    manualWait_ = true;
    interrupt(QStringLiteral("再開ボタンで次へ / Press Resume to continue"));
    return;
  }
  if (step.type == T::Note) { advance(); return; }
  if (!operations_) { fail(QStringLiteral("操作入口がありません / Operations unavailable")); return; }
  QScopedValueRollback<bool> dispatchGuard(dispatching_, true);
  QScopedValueRollback<bool> modelGuard(ownModelChange_, true);
  ControllerOperations::Result result;
  if (step.type == T::Cue || step.type == T::Take || step.type == T::Transport) {
    pendingRequest_ = ++sequence_;
    requestStartedMs_ = monotonicNow();
    appendLog(QStringLiteral("再生要求 / Playout request"));
    setState(State::Waiting);
    if (step.type == T::Transport)
      result = operations_->executeFrom("Macro", A::MacroTransport, pendingRequest_, step.operation);
    else if (step.type == T::Cue)
      result = operations_->executeFrom("Macro", A::MacroCue, pendingRequest_, step.itemId);
    else
      result = operations_->executeFrom("Macro", A::MacroTake, pendingRequest_, step.itemId,
                                        step.requireReady, step.crossfade, step.crossfadeDurationMs);
    if (!result.invoked() && isActive() && state_ != State::Paused)
      fail(QStringLiteral("再生操作が拒否されました / Playout request rejected"));
    return;
  }
  if (step.type == T::ItemSettings)
    result = operations_->executeFrom("Macro", A::MacroItemSettings, step.itemId, step.settingsPatch);
  else if (step.type == T::RuntimeSetting)
    result = operations_->executeFrom("Macro", A::MacroRuntimeSetting, step.operation, step.value);
  if (!isActive() || state_ == State::Paused) return;
  if (!result.invoked()) { fail(QStringLiteral("設定・操作が拒否されました / Setting or operation rejected")); return; }
  if (usesItem(step)) refreshObservedItems();
  advance();
}

bool MacroRunner::applyItemSettings(quint64 itemId, const QJsonObject &patch, QString *error) {
  if (isActive() || !model_ || !operations_) {
    if (error) *error = QStringLiteral("マクロを停止してから適用してください / Stop macro before editing");
    return false;
  }
  MacroStep step;
  step.type = MacroStepType::ItemSettings;
  step.documentId = model_->documentId(); step.itemId = itemId; step.settingsPatch = patch;
  if (!checkTarget(step, error)) return false;
  const bool result = operations_->executeFrom("MacroEditor", ControllerOperations::Action::MacroItemSettings,
                                                itemId, patch).invoked();
  if (!result && error) *error = QStringLiteral("項目設定が拒否されました / Item settings rejected");
  return result;
}
bool MacroRunner::referencesItem(quint64 id) const {
  return isActive() && observedItems_.contains(id);
}
void MacroRunner::refreshObservedItems() {
  observedItems_.clear();
  if (!model_) return;
  const auto remember = [this](quint64 id) {
    if (const auto item = model_->itemState(id)) observedItems_.insert(id, *item);
  };
  for (const auto &step : definition_.steps) if (step.enabled && usesItem(step)) {
    remember(step.itemId);
    if (const auto item = model_->itemState(step.itemId)) remember(item->settings.gotoTargetItemId);
    remember(step.settingsPatch.value(QStringLiteral("gotoTargetItemId")).toString().toULongLong());
  }
}
void MacroRunner::itemChanged(quint64 id) {
  if (ownModelChange_ || !referencesItem(id) || !model_) return;
  const auto item = model_->itemState(id);
  const auto old = observedItems_.constFind(id);
  if (!item || old == observedItems_.cend() || item->filePath != old->filePath ||
      item->sourceType != old->sourceType || item->mediaType != old->mediaType ||
      item->settings != old->settings)
    interrupt(QStringLiteral("参照項目が変更されました。再開前に確認してください / Referenced item changed"));
}

void MacroRunner::selectionChanged() {
  if (dispatching_ || !isActive()) return;
  // TAKE's normal auto-select-next notification is part of that same request.
  // A real selection during pending TAKE is caught first by Program's owner guard.
  if (pendingRequest_ && stepIndex_ >= 0 && stepIndex_ < definition_.steps.size() &&
      definition_.steps.at(stepIndex_).type == MacroStepType::Take) return;
  interrupt(QStringLiteral("選択に伴うスタンバイを優先して一時停止 / Paused to prepare the selected item"));
}

void MacroRunner::beforeOperation(ControllerOperations::Action action,
                                  const QVariantList &, const QString &source) {
  if (!isActive() || state_ == State::Paused || source == QLatin1String("Macro")) return;
  using A = ControllerOperations::Action;
  switch (action) {
  case A::TakeSelectedOnAir: case A::OffAir: case A::ProgramPlay: case A::ProgramPause:
  case A::ProgramTogglePlayback: case A::ToggleFtb: case A::ToggleDsk:
  case A::ProgramSeek: case A::ProgramPlaybackRate: case A::MasterMute:
  case A::SetItemVolume: case A::SetMasterVolume: case A::AdjustItemVolume:
  case A::AdjustMasterVolume: case A::ResetItemVolume: case A::ResetMasterVolume:
  case A::ItemFadeIn: case A::ItemFadeOut: case A::MasterFadeIn: case A::MasterFadeOut:
  case A::MacroCue: case A::MacroTake: case A::MacroRuntimeSetting: case A::MacroTransport:
    interrupt(QStringLiteral("手動操作を優先して一時停止 / Paused for manual operation"));
    break;
  default: break;
  }
}
