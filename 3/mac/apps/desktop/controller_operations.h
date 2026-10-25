#pragma once

#include "playlist_item.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QMetaEnum>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <atomic>

#include <array>
#include <cmath>
#include <functional>
#include <type_traits>
#include <utility>

// Input-independent, synchronous entry point on the Controller thread.
// Widgets and device adapters translate input into these operations; the
// existing controllers remain responsible for media state and runtime guards.
class ControllerOperations final : public QObject {
  Q_OBJECT

public:
  enum class Action {
    AddFilesDialog, AddFiles, Exit,
    TakeSelectedOnAir, OffAir, ProgramPlay, ProgramPause, ProgramTogglePlayback,
    ToggleFtb, ToggleDsk, DskPlay, DskPause,
    DocumentNext, DocumentPrevious, ProgramSeek,
    ProgramPlaybackRate, MasterMute,
    EditLoadSelected, EditPlay, EditPause, EditStop, EditSeek, EditStepFrames,
    EditJumpStart, EditJumpIn, EditJumpOut, EditJumpEnd,
    EditScanBackward, EditScanForward, EditScanStop, EditSetIn, EditSetOut,
    EditInTime, EditOutTime, EditStartMode, EditEndMode, EditGoto,
    EditRepeat, EditFadeIn, EditFadeOut, EditEndCrossfade,
    SetEditVolume, SetItemVolume, SetMasterVolume,
    AdjustEditVolume, AdjustItemVolume, AdjustMasterVolume,
    ResetEditVolume, ResetItemVolume, ResetMasterVolume,
    ItemFadeIn, ItemFadeOut, MasterFadeIn, MasterFadeOut,
    ActivatePlaylist, CreatePlaylist, SelectItem, SelectItemNumber,
    MoveItem, MoveItemToRow, TransferItem, RemoveItem, PlaylistBulk,
    CopySelectedState, PasteSelectedState, CopyItemState, PasteItemState,
    BgmPlay, BgmPause, BgmStop, BgmFadeIn, BgmFadeOut,
    RenamePlaylist, SetItemDisplayName, SetItemColor, SetItemSeparatorBelow,
    SetItemStartMode, SetItemEndMode,
    SetItemFadeInEnabled, SetItemFadeOutEnabled, SetItemEndCrossfadeEnabled,
    EditToggleStartMode, EditToggleFadeOut, EditReset,
    SetBgmVolume, AdjustBgmVolume, ResetBgmVolume,
    MacroCue, MacroTake, CancelMacroRequests, MacroItemSettings,
    MacroRuntimeSetting, MacroTransport,
    NextOutputDisplay, PreviewCapture, ProgramCapture, TogglePfl,
    OperatorTimerStart, OperatorTimerPause, OperatorTimerReset,
    OperatorTimerSettings, OpenNominalEq, OpenItemEq, OpenMasterEq, OpenSettings,
    RemoveSelectedItem, SelectRelativeItem, BgmFadeInConfigured, BgmFadeOutConfigured,
    Count
  };
  Q_ENUM(Action)

  enum class Status { Invoked, Rejected, InvalidArguments, Unavailable, WrongThread };
  Q_ENUM(Status)
  struct Result {
    Status status = Status::Unavailable;
    // Invoked means the guarded implementation was called, not that an
    // asynchronous media operation has finished successfully.
    bool invoked() const { return status == Status::Invoked; }
  };

  class InputScope {
  public:
    explicit InputScope(const char *source) : previous_(inputSource()) {
      inputSource() = source;
    }
    ~InputScope() { inputSource() = previous_; }
    InputScope(const InputScope &) = delete;
    InputScope &operator=(const InputScope &) = delete;

  private:
    const char *previous_;
  };
  void setLogContextProvider(std::function<QString()> provider) {
    logContext_ = std::move(provider);
  }
  explicit ControllerOperations(QObject *parent = nullptr) : QObject(parent) {}
  ~ControllerOperations() override {
    for (auto *timer : continuousTimers_)
      if (timer->isActive()) {
        qInfo().noquote() << timer->property("request").toString()
                          << "(last continuous value at shutdown)";
        qInfo().noquote() << timer->property("result").toString();
      }
  }

  Result dispatch(Action action, const QVariantList &arguments = {}) {
    static std::atomic<quint64> sequence{0};
    const auto operationId = ++sequence;
    const auto name =
        QMetaEnum::fromType<Action>().valueToKey(static_cast<int>(action));
    const bool correctThread = QThread::currentThread() == thread();
    const bool continuous =
        correctThread &&
        (action == Action::ProgramSeek || action == Action::EditSeek ||
         action == Action::SetEditVolume || action == Action::SetItemVolume ||
         action == Action::SetMasterVolume || action == Action::SetBgmVolume);
    QString request;
    {
      QDebug out(&request);
      out.nospace()
          << "OP request id=" << operationId
          << " action=" << (name ? name : "Unknown") << " requestedAt="
          << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
          << " source=" << inputSource() << " args=" << arguments << " context="
          << (correctThread && logContext_ ? logContext_() : QString());
    }
    if (continuous)
      qDebug().noquote() << request;
    else
      qInfo().noquote() << request;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto run = [&]() -> Result {
      if (QThread::currentThread() != thread())
        return {Status::WrongThread};
      const auto index = static_cast<size_t>(action);
      if (!enabled_ || index >= handlers_.size() || !handlers_[index])
        return {Status::Unavailable};
      // Invalid or unavailable input is not a manual intervention. Validate
      // before notifying MacroRunner, then recheck after observers have run.
      const auto ready = [&]() -> Result {
        if (QThread::currentThread() != thread()) return {Status::WrongThread};
        if (!enabled_) return {Status::Unavailable};
        const auto validator = validators_[index];
        const auto valid = validator(arguments);
        if (!valid.invoked()) return valid;
        const auto guard = guards_[index];
        return guard ? guard(arguments) : valid;
      };
      const auto before = ready();
      if (!before.invoked()) return before;
      emit operationAboutToExecute(action, arguments, QString::fromUtf8(inputSource()));
      const auto after = ready();
      if (!after.invoked()) return after;
      // Copy before invoking: a dialog may run a nested event loop.
      const auto handler = handlers_[index];
      return handler(arguments);
    };
    const auto result = run();
    QString resultText;
    {
      QDebug out(&resultText);
      out.nospace() << "OP result id=" << operationId << " status="
                    << QMetaEnum::fromType<Status>().valueToKey(
                           static_cast<int>(result.status))
                    << " elapsedMs=" << elapsed.elapsed();
    }
    if (continuous && result.invoked()) {
      qDebug().noquote() << resultText;
      auto *&timer = continuousTimers_[static_cast<int>(action)];
      if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, timer] {
          qInfo().noquote() << timer->property("request").toString()
                            << "(last value in continuous operation)";
          qInfo().noquote() << timer->property("result").toString();
        });
      }
      timer->setProperty("request", request);
      timer->setProperty("result", resultText);
      timer->start(250);
    } else {
      if (continuous)
        qInfo().noquote() << request;
      if (result.invoked())
        qInfo().noquote() << resultText;
      else
        qWarning().noquote() << resultText;
    }
    return result;
  }

  template <typename... Args>
  Result executeFrom(const char *source, Action action, Args... arguments) {
    InputScope scope(source);
    return execute(action, arguments...);
  }

  template <typename... Args> Result execute(Action action, Args... arguments) {
    return dispatch(action, {QVariant::fromValue(arguments)...});
  }

  template <typename... Args, typename F>
  void bind(Action action, QObject *owner, F handler) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(owner && owner->thread() == thread());
    const auto index = static_cast<size_t>(action);
    Q_ASSERT(index < handlers_.size());
    Q_ASSERT(!handlers_[index]);
    if (QThread::currentThread() != thread() || !owner ||
        owner->thread() != thread() || index >= handlers_.size() || handlers_[index]) return;
    const QPointer<QObject> guarded(owner);
    validators_[index] = [guarded](const QVariantList &arguments) -> Result {
      if (!guarded) return {Status::Unavailable};
      if (guarded->thread() != QThread::currentThread()) return {Status::WrongThread};
      return validateArguments<Args...>(arguments, std::index_sequence_for<Args...>{});
    };
    handlers_[index] = [guarded, handler](const QVariantList &arguments) -> Result {
      if (!guarded) return {Status::Unavailable};
      if (guarded->thread() != QThread::currentThread()) return {Status::WrongThread};
      return invoke<Args...>(handler, arguments, std::index_sequence_for<Args...>{});
    };
  }

  // Register independently of bind(), in either order. A guard is a pure
  // availability predicate: it must not mutate state or dispatch operations.
  // It runs before notification and again after observers may have changed
  // the state. False rejects the operation without calling its implementation.
  template <typename... Args, typename F>
  void setGuard(Action action, QObject *owner, F predicate) {
    static_assert(std::is_same_v<std::invoke_result_t<F, Args...>, bool>);
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(owner && owner->thread() == thread());
    const auto index = static_cast<size_t>(action);
    Q_ASSERT(index < guards_.size());
    Q_ASSERT(!guards_[index]);
    if (QThread::currentThread() != thread() || !owner ||
        owner->thread() != thread() || index >= guards_.size() || guards_[index]) return;
    const QPointer<QObject> guarded(owner);
    guards_[index] = [guarded, predicate](const QVariantList &arguments) -> Result {
      if (!guarded) return {Status::Unavailable};
      if (guarded->thread() != QThread::currentThread()) return {Status::WrongThread};
      return invoke<Args...>(predicate, arguments, std::index_sequence_for<Args...>{});
    };
  }

  template <typename T, typename R, typename... Args>
  void bindMember(Action action, T *owner, R (T::*method)(Args...)) {
    bind<std::decay_t<Args>...>(action, owner,
        [owner, method](std::decay_t<Args>... args) -> R {
          return (owner->*method)(args...);
        });
  }

  template <typename T, typename... Args>
  void connectInput(T *sender, void (T::*signal)(Args...), Action action) {
    connect(sender, signal, this, [this, action](Args... args) {
      (void)execute(action, args...);
    });
  }

  void disable() { enabled_ = false; }

signals:
  void operationAboutToExecute(ControllerOperations::Action action,
                              const QVariantList &arguments,
                              const QString &source);

private:
  static const char *&inputSource() {
    static thread_local const char *value = "UI";
    return value;
  }
  std::function<QString()> logContext_;
  template <typename T> static bool validValue(T value) {
    if constexpr (std::is_same_v<T, double>) return std::isfinite(value);
    else if constexpr (std::is_same_v<T, qint64>) return value >= 0;
    else if constexpr (std::is_same_v<T, PlaylistStartMode>)
      return value == PlaylistStartMode::Pause || value == PlaylistStartMode::Play;
    else if constexpr (std::is_same_v<T, PlaylistEndMode>)
      return value >= PlaylistEndMode::Stop && value <= PlaylistEndMode::Goto;
    else if constexpr (std::is_same_v<T, PlaylistRepeatEndMode>)
      return value >= PlaylistRepeatEndMode::Stop && value <= PlaylistRepeatEndMode::Goto;
    else return true;
  }

  template <typename... Args, size_t... I>
  static Result validateArguments(const QVariantList &args, std::index_sequence<I...>) {
    if (args.size() != sizeof...(Args)) return {Status::InvalidArguments};
    if (!(... && (args[I].metaType() == QMetaType::fromType<Args>())))
      return {Status::InvalidArguments};
    if (!(... && validValue(args[I].value<Args>())))
      return {Status::InvalidArguments};
    return {Status::Invoked};
  }

  template <typename... Args, typename F, size_t... I>
  static Result invoke(const F &handler, const QVariantList &args,
                       std::index_sequence<I...> indexes) {
    const auto valid = validateArguments<Args...>(args, indexes);
    if (!valid.invoked()) return valid;
    using Return = std::invoke_result_t<F, Args...>;
    if constexpr (std::is_same_v<Return, bool>) {
      return {handler(args[I].value<Args>()...) ? Status::Invoked : Status::Rejected};
    } else if constexpr (std::is_same_v<Return, Result>) {
      return handler(args[I].value<Args>()...);
    } else {
      static_assert(std::is_void_v<Return>);
      handler(args[I].value<Args>()...);
      return {Status::Invoked};
    }
  }

  std::array<std::function<Result(const QVariantList &)>,
             static_cast<size_t>(Action::Count)> handlers_;
  std::array<std::function<Result(const QVariantList &)>,
             static_cast<size_t>(Action::Count)> validators_;
  std::array<std::function<Result(const QVariantList &)>,
             static_cast<size_t>(Action::Count)> guards_;
  QHash<int, QTimer *> continuousTimers_;
  bool enabled_ = true;
};
