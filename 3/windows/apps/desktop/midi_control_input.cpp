#include "midi_control_input.h"
#include "controller_operations.h"
#include "controller_settings.h"
#include "midi_message_queue.h"
#include "midi_device_discovery.h"
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace vtrpon {
QList<MidiControlInput::Device> MidiControlInput::devices() {
  QList<Device> result;
  QElapsedTimer elapsed;
  elapsed.start();
#ifdef _WIN32
  QMap<QString, int> occurrences;
  const auto count = midiInGetNumDevs();
  for (UINT index = 0; index < count; ++index) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
    const auto name = QString::fromWCharArray(caps.szPname);
    const auto id = QString::fromUtf8(QJsonDocument(QJsonArray{name, occurrences[name]++}).toJson(QJsonDocument::Compact));
    result.append({id, name, index});
  }
#endif
  if (elapsed.elapsed() >= 250) qInfo() << "MIDI device enumeration elapsedMs=" << elapsed.elapsed() << "count=" << result.size();
  return result;
}

class MidiControlInput::Impl {
public:
  Impl(MidiControlInput *owner, ControllerOperations *operations,
       std::function<void(const QString &)> buttonAction)
      : owner(owner), operations(operations), buttonAction(std::move(buttonAction)) {
    drain.setInterval(10);
    QObject::connect(&drain, &QTimer::timeout, owner, [this] { poll(); });
    reconnect.setInterval(1500);
    QObject::connect(&reconnect, &QTimer::timeout, owner, [this] { refresh(); });
    drain.start();
    reconnect.start();
  }
  ~Impl() { close(); }
  void setStatus(const QString &key, const QString &detail = {}) {
    if (status == key && error == detail) return;
    status = key; error = detail;
    qInfo().noquote() << "MIDI connection" << status << error;
    emit owner->connectionChanged();
  }
  void close() {
#ifdef _WIN32
    if (handle) {
      midiInStop(handle);
      midiInReset(handle);
      midiInClose(handle);
      handle = nullptr;
    }
#endif
    openedId.clear();
    clearQueue();
    router.reset(); volumes.reset();
  }
  void clearQueue() {
    // Discard actions during a mode/settings change, but still account for
    // releases so a learned button does not remain held after closing Settings.
    const auto pending = queue.take();
    if (pending.noteOverflow) router.resetButtons(QStringLiteral("note"));
    if (pending.buttonOverflow || pending.relativeOverflow) router.resetButtons(QStringLiteral("cc"));
    const QJsonObject unassigned;
    const auto track = [&](quint32 packed) { (void)router.process(midiControl::decode(packed), unassigned, false); };
    for (size_t i = 0; i < pending.noteCount; ++i) track(pending.notes[i]);
    for (size_t i = 0; i < pending.buttonCount; ++i) track(pending.buttons[i]);
    for (size_t i = 0; i < pending.relativeCount; ++i) track(pending.relative[i]);
    for (size_t i = 0; i < pending.absoluteCount; ++i) track(pending.absolute[i].packed);
  }
  void configureQueue() {
    midiControl::MessageQueue::Modes modes{};
    const auto bindings = configuration.value("bindings").toObject();
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
      const auto binding = it.value().toObject();
      if (binding.value("type").toString() != "cc") continue;
      const auto key = static_cast<size_t>((binding.value("channel").toInt() - 1) * 128 + binding.value("number").toInt());
      if (!midiControl::isVolume(it.key())) modes[key] = midiControl::MessageQueue::CcMode::Button;
      else if (binding.value("mode").toString("absolute") != "absolute") modes[key] = midiControl::MessageQueue::CcMode::Relative;
    }
    queue.setModes(modes);
  }
  QString requestedDevice() const {
    return configurationMode ? monitoredId : (configuration.value("enabled").toBool() ? configuration.value("device").toString() : QString{});
  }
  void refresh() {
    const auto requested = requestedDevice();
    if (stopped || requested.isEmpty()) {
      close();
      setStatus(configurationMode ? QStringLiteral("midi.select_device") : QStringLiteral("midi.disabled"));
      return;
    }
    if (!openedId.isEmpty() && openedId != requested) close();
    discovery.request();
    if (status == QStringLiteral("midi.disabled") || status == QStringLiteral("midi.select_device"))
      setStatus(QStringLiteral("midi.searching"));
  }
  void refreshConnection(const QList<MidiControlInput::Device> &available) {
    const auto requested = requestedDevice();
    if (stopped || requested.isEmpty()) return;
    const auto found = std::find_if(available.begin(), available.end(), [&](const auto &device) { return device.id == requested; });
    if (found == available.end()) { close(); setStatus(QStringLiteral("midi.disconnected")); return; }
#ifdef _WIN32
    if (handle && openedId == requested && openedIndex == found->index) return;
    close();
    auto result = midiInOpen(&handle, found->index, reinterpret_cast<DWORD_PTR>(&callback), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (result == MMSYSERR_NOERROR) result = midiInStart(handle);
    if (result != MMSYSERR_NOERROR) {
      wchar_t text[MAXERRORLENGTH]{};
      midiInGetErrorTextW(result, text, MAXERRORLENGTH);
      close(); setStatus(QStringLiteral("midi.open_failed"), QString::fromWCharArray(text)); return;
    }
    openedId = requested; openedIndex = found->index;
    setStatus(QStringLiteral("midi.connected"));
#else
    setStatus(QStringLiteral("midi.unsupported"));
#endif
  }
  void poll() {
    const QPointer<MidiControlInput> ownerGuard(owner);
    if (auto available = discovery.takeResult()) refreshConnection(*available);
    if (!ownerGuard) return;
    const auto pending = queue.take();
    if (pending.noteOverflow) router.resetButtons(QStringLiteral("note"));
    if (pending.buttonOverflow || pending.relativeOverflow) router.resetButtons(QStringLiteral("cc"));
    if (pending.noteOverflow || pending.buttonOverflow || pending.relativeOverflow) setStatus(QStringLiteral("midi.overflow"));
    if (!ownerGuard) return;
    trafficReceived += pending.received;
    trafficDispatched += pending.noteCount + pending.buttonCount + pending.relativeCount + pending.absoluteCount;
    if (!trafficTimer.isValid()) trafficTimer.start();
    if (trafficReceived && trafficTimer.elapsed() >= 1000) {
      qInfo() << "MIDI_TRAFFIC received=" << trafficReceived << "dispatched=" << trafficDispatched
              << "intervalMs=" << trafficTimer.elapsed();
      trafficReceived = trafficDispatched = 0;
      trafficTimer.restart();
    }
    const auto generationAtStart = generation;
    const auto dispatch = [&](const auto &packets, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        if (!ownerGuard || generationAtStart != generation || stopped) return false;
        processMessage(midiControl::decode(packets[i]));
      }
      return bool(ownerGuard);
    };
    // Button presses and releases are never queued behind continuous CC.
    if (!dispatch(pending.notes, pending.noteCount) || !dispatch(pending.buttons, pending.buttonCount) ||
        !dispatch(pending.relative, pending.relativeCount)) return;
    for (size_t i = 0; i < pending.absoluteCount; ++i) {
      if (!ownerGuard || generationAtStart != generation || stopped) return;
      auto message = midiControl::decode(pending.absolute[i].packed);
      message.minimumValue = pending.absolute[i].minimum;
      message.maximumValue = pending.absolute[i].maximum;
      processMessage(message);
    }
  }
  void traceMessage(const midiControl::Message &message, const QString &action, const char *result) {
    auto &timer = message.type == "note" ? noteTraceTimer : traceTimer;
    if (timer.isValid() && timer.elapsed() < (message.type == "note" ? 50 : 1000)) return;
    timer.start();
    qInfo().noquote() << "MIDI input" << message.type << "channel=" << message.channel
                     << "number=" << message.number << "value=" << message.value
                     << "action=" << action << "result=" << result;
  }
  void processMessage(const midiControl::Message &message) {
    if (stopped || message.type.isEmpty()) return;
    const QPointer<MidiControlInput> ownerGuard(owner);
    const auto generationAtStart = generation;
    const bool modal = QApplication::activeModalWidget();
    const bool enabled = configuration.value("enabled").toBool();
    const auto action = router.process(message, configuration.value("bindings").toObject(),
                                      !configurationMode && !modal && enabled);
    emit owner->received(message.type, message.channel, message.number, message.value, message.pressed);
    if (!ownerGuard || generationAtStart != generation || stopped) return;
    if (action.isEmpty() || !operations) {
      traceMessage(message, action, configurationMode ? "settings" : modal ? "modal" : !enabled ? "disabled" : "unassigned-or-repeat");
      return;
    }
    if (midiControl::isVolume(action)) {
      if (!volumeValue) { traceMessage(message, action, "no-volume-provider"); return; }
      const auto value = volumes.map(action, message.value, configuration.value("bindings").toObject().value(action).toObject(), volumeValue(action), message.minimumValue, message.maximumValue);
      if (!value) { traceMessage(message, action, "pickup-wait"); return; }
      using A = ControllerOperations::Action;
      const auto operation = action == "SetMasterVolume" ? A::SetMasterVolume : action == "SetItemVolume" ? A::SetItemVolume : action == "SetEditVolume" ? A::SetEditVolume : A::SetBgmVolume;
      const bool invoked = operations->executeFrom("MIDI", operation, *value).invoked();
      if (!ownerGuard) return;
      traceMessage(message, action, invoked ? "invoked" : "unavailable");
      if (!invoked) volumes.forget(action);
    } else {
      const auto callback = buttonAction;
      traceMessage(message, action, callback ? "button" : "no-button-handler");
      if (callback) callback(action);
    }
  }
#ifdef _WIN32
  static void CALLBACK callback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR packed, DWORD_PTR) {
    if (message != MIM_DATA) return;
    auto *self = reinterpret_cast<Impl *>(instance);
    // No Qt, multimedia calls or Controller operations on the driver thread.
    self->queue.push(static_cast<quint32>(packed));
  }
  HMIDIIN handle = nullptr;
#endif
  MidiControlInput *owner;
  QPointer<ControllerOperations> operations;
  std::function<void(const QString &)> buttonAction;
  std::function<int(const QString &)> volumeValue;
  QTimer drain, reconnect;
  QElapsedTimer traceTimer;
  QElapsedTimer noteTraceTimer;
  QElapsedTimer trafficTimer;
  size_t trafficReceived = 0, trafficDispatched = 0;
  QJsonObject configuration;
  midiControl::Router router;
  midiControl::VolumeMapper volumes;
  QString monitoredId, openedId, status = QStringLiteral("midi.disabled"), error;
  unsigned int openedIndex = 0;
  bool configurationMode = false, stopped = false;
  quint64 generation = 0;
  midiControl::MessageQueue queue;
  MidiDeviceDiscovery discovery;
};
MidiControlInput::MidiControlInput(ControllerOperations *operations, std::function<void(const QString &)> buttonAction, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, operations, std::move(buttonAction))) { reload(); }
MidiControlInput::~MidiControlInput() = default;
void MidiControlInput::reload() {
  auto settings = makeControllerSettings();
  const auto previousDevice = impl_->openedId;
  impl_->configuration = midiControl::load(settings);
  qInfo().noquote() << "MIDI settings enabled=" << impl_->configuration.value("enabled").toBool()
                   << "device=" << impl_->configuration.value("device").toString()
                   << "actions=" << impl_->configuration.value("bindings").toObject().keys();
  ++impl_->generation; impl_->clearQueue();
  auto router = impl_->router;
  router.resetContinuous();
  impl_->configureQueue();
  impl_->volumes.reset(); impl_->refresh();
  if (previousDevice == impl_->openedId) impl_->router = std::move(router);
}
void MidiControlInput::setConfigurationMode(bool active) {
  if (impl_->configurationMode == active) return;
  impl_->configurationMode = active;
  if (!active) {
    // Preserve the device session when returning from Settings. Only the CC
    // history and Pickup state need to restart; reopening WinMM here would
    // unnecessarily block the dialog close on the driver.
    reload();
    return;
  }
  if (impl_->monitoredId.isEmpty()) impl_->monitoredId = impl_->configuration.value("device").toString();
  ++impl_->generation; impl_->clearQueue(); impl_->volumes.reset(); impl_->refresh();
}
void MidiControlInput::processMessage(const midiControl::Message &message) { impl_->processMessage(message); }
void MidiControlInput::enqueueMessage(quint32 packed) { impl_->queue.push(packed); }
void MidiControlInput::pollMessages() { impl_->poll(); }
void MidiControlInput::monitorDevice(const QString &id) {
  if (impl_->monitoredId == id) return;
  impl_->monitoredId = id; ++impl_->generation; impl_->refresh();
}
QString MidiControlInput::statusKey() const { return impl_->status; }
QString MidiControlInput::errorDetail() const { return impl_->error; }
void MidiControlInput::setVolumeValueProvider(std::function<int(const QString &)> provider) { impl_->volumeValue = std::move(provider); }
void MidiControlInput::stop() {
  impl_->stopped = true; ++impl_->generation;
  impl_->drain.stop(); impl_->reconnect.stop(); impl_->close();
}
} // namespace vtrpon
