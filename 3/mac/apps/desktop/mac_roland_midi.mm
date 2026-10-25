#include "mac_roland_midi.h"
#include "mac_midi_service.h"
#include <QElapsedTimer>
#include <QDeadlineTimer>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>

namespace vtrpon {
namespace {
bool fail(QString *error, const QString &detail) {
  if (error) *error = detail;
  return false;
}
QString property(MIDIObjectRef object, CFStringRef key) {
  CFStringRef value = nullptr;
  if (MIDIObjectGetStringProperty(object, key, &value) != noErr || !value) return {};
  const auto text = QString::fromCFString(value);
  CFRelease(value);
  return text;
}
bool rolandName(const QString &name, const QString &model) {
  const auto normalized = [](QString text) {
    text = text.toUpper(); text.remove('-'); text.remove(' '); text.replace("MKII", "MK2"); return text;
  };
  return normalized(name).contains(normalized(model));
}
bool rolandEndpoint(MIDIEndpointRef endpoint, const QString &model) {
  if (!macMidi::endpointAvailable(endpoint)) return false;
  if (rolandName(property(endpoint, kMIDIPropertyDisplayName), model) ||
      rolandName(property(endpoint, kMIDIPropertyName), model)) return true;
  MIDIEntityRef entity = 0;
  MIDIDeviceRef device = 0;
  MIDIEndpointGetEntity(endpoint, &entity);
  if (entity) MIDIEntityGetDevice(entity, &device);
  return device && rolandName(property(device, kMIDIPropertyName), model);
}
struct Inbox {
  std::mutex mutex;
  bool active = true, overflow = false;
  QByteArray sysex;
  unsigned status = 0, remaining = 0;
  QList<QByteArray> messages;
  void append(const QByteArray &message) {
    if (messages.size() >= 128) { overflow = true; return; }
    messages.append(message);
  }
  void receive(const MIDIPacketList *list) {
    std::lock_guard lock(mutex);
    if (!active || !list) return;
    auto *packet = &list->packet[0];
    for (UInt32 n = 0; n < list->numPackets; ++n, packet = MIDIPacketNext(packet)) {
      for (unsigned i = 0; i < packet->length; ++i) {
        const auto byte = packet->data[i];
        if (byte >= 0xf8) continue; // Realtime may occur inside any MIDI message.
        if (byte == 0xf0) { sysex = QByteArray(1, char(byte)); status = 0; remaining = 0; continue; }
        if (!sysex.isEmpty()) {
          if (sysex.size() >= 512) { sysex.clear(); overflow = true; status = 0; continue; }
          if (byte == 0xf7) { sysex.append(char(byte)); append(sysex); sysex.clear(); continue; }
          if (byte < 0x80) { sysex.append(char(byte)); continue; }
          sysex.clear(); // Resynchronise after a malformed/incomplete SysEx.
        }
        if (byte & 0x80) {
          status = byte;
          remaining = byte < 0xf0 ? (((byte & 0xe0) == 0xc0) ? 1 : 2)
                      : byte == 0xf2 ? 2 : (byte == 0xf1 || byte == 0xf3) ? 1 : 0;
          continue;
        }
        if ((status & 0xf0) == 0xc0) append(QByteArray(1, char(status)) + char(byte));
        if (remaining && --remaining == 0) {
          if (status >= 0xf0) status = 0;
          else remaining = ((status & 0xe0) == 0xc0) ? 1 : 2;
        }
      }
    }
  }
};
struct Transmission {
  QByteArray bytes;
  MIDISysexSendRequest request{};
  std::atomic_bool done{false};
  QElapsedTimer started;
  static void complete(MIDISysexSendRequest *request) {
    auto *holder = static_cast<std::shared_ptr<Transmission> *>(request->completionRefCon);
    (*holder)->done.store(true, std::memory_order_release);
    delete holder; // Own the request and bytes until CoreMIDI releases them.
  }
};
}

class MacRolandMidi::Impl {
public:
  explicit Impl(SysExSender sender) : sender(sender) {}
  SysExSender sender;
  MIDIEndpointRef source = 0, destination = 0;
  MIDIPortRef input = 0, output = 0;
  QString sourceId, destinationId, failure;
  std::shared_ptr<Inbox> inbox;
  std::shared_ptr<Transmission> transmission;
  std::deque<QByteArray> queue;
  QElapsedTimer health;
  int sendIntervalMs = 0;
  QDeadlineTimer nextSend;
  QList<Diagnostic> diagnostics;
  void record(const QString &kind, const QString &detail) {
    if (diagnostics.size() >= 128) diagnostics.removeFirst();
    diagnostics.append({kind, detail});
  }
  bool pump(QString *error) {
    if (!failure.isEmpty()) return fail(error, failure);
    if (transmission) {
      if (!transmission->done.load(std::memory_order_acquire)) {
        if (transmission->started.elapsed() < 2000) return true;
        failure = QStringLiteral("Roland MIDI output completion timed out. Reconnecting the switcher.");
        return fail(error, failure);
      }
      record("DRIVER", QStringLiteral("CoreMIDI SysEx completed (not a device acknowledgement)."));
      transmission.reset();
      nextSend.setRemainingTime(sendIntervalMs);
    }
    if (!nextSend.hasExpired()) return true;
    while (!queue.empty()) {
      auto bytes = std::move(queue.front()); queue.pop_front();
      OSStatus status = noErr;
      if (static_cast<unsigned char>(bytes[0]) == 0xf0) {
        auto current = std::make_shared<Transmission>();
        current->bytes = std::move(bytes);
        current->request.destination = destination;
        current->request.data = reinterpret_cast<const Byte *>(current->bytes.constData());
        current->request.bytesToSend = current->bytes.size();
        current->request.completionProc = Transmission::complete;
        auto *holder = new std::shared_ptr<Transmission>(current);
        current->request.completionRefCon = holder;
        current->started.start();
        transmission = current;
        status = sender(&current->request);
        if (status == noErr) {
          record("TX", QString::fromLatin1(current->bytes.toHex(' ')));
          return true;
        }
        delete holder; transmission.reset();
      } else {
        alignas(MIDIPacketList) std::array<char, 256> storage{};
        auto *list = reinterpret_cast<MIDIPacketList *>(storage.data());
        auto *packet = MIDIPacketListInit(list);
        packet = MIDIPacketListAdd(list, storage.size(), packet, 0, bytes.size(),
                                   reinterpret_cast<const Byte *>(bytes.constData()));
        status = packet ? MIDISend(output, destination, list) : kMIDIMessageSendErr;
      }
      if (status == noErr) record("TX", QString::fromLatin1(bytes.toHex(' ')));
      if (status != noErr) {
        failure = QStringLiteral("Roland CoreMIDI send failed (OSStatus %1).").arg(status);
        return fail(error, failure);
      }
    }
    return true;
  }
};
MacRolandMidi::MacRolandMidi(SysExSender sender) : impl_(std::make_unique<Impl>(sender)) {}
MacRolandMidi::~MacRolandMidi() { close(); }
bool MacRolandMidi::isOpen() const { return impl_->output != 0; }
void MacRolandMidi::close() {
  auto &p = *impl_;
  if (p.inbox) { std::lock_guard lock(p.inbox->mutex); p.inbox->active = false; }
  if (p.input) {
    MIDIPortDisconnectSource(p.input, p.source);
    MIDIPortDispose(p.input);
  }
  macMidi::reserveSource(p.source, false);
  // Cancellation completes asynchronously. Its self-owned request can outlive
  // this backend; close/destruction never waits on the driver or the GUI.
  if (p.transmission) p.transmission->request.complete = true;
  p.transmission.reset();
  if (p.output) MIDIPortDispose(p.output);
  p.input = p.output = 0;
  p.source = p.destination = 0;
  p.inbox.reset(); p.queue.clear(); p.failure.clear(); p.diagnostics.clear();
}
bool MacRolandMidi::open(bool receive, QString *error, const QString &modelName, int sendIntervalMs) {
  close();
  auto &p = *impl_;
  p.sendIntervalMs = qMax(0, sendIntervalMs); p.nextSend = QDeadlineTimer();
  MIDIClientRef client = 0;
  auto status = macMidi::client(client);
  if (status != noErr) return fail(error, QStringLiteral("CoreMIDI startup failed (%1).").arg(status));
  QList<MIDIEndpointRef> destinations, sources;
  for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
    const auto endpoint = MIDIGetDestination(i);
    if (rolandEndpoint(endpoint, modelName) && (p.destinationId.isEmpty() || macMidi::endpointId(endpoint) == p.destinationId))
      destinations.append(endpoint);
  }
  if (destinations.size() != 1) return fail(error, destinations.isEmpty()
      ? QStringLiteral("Roland %1 MIDI output was not found.").arg(modelName)
      : QStringLiteral("Multiple Roland %1 MIDI outputs were found. Connect one switcher.").arg(modelName));
  p.destination = destinations.first();
  MIDIEntityRef outputEntity = 0;
  MIDIEndpointGetEntity(p.destination, &outputEntity);
  for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
    const auto endpoint = MIDIGetSource(i);
    MIDIEntityRef entity = 0;
    MIDIEndpointGetEntity(endpoint, &entity);
    if (rolandEndpoint(endpoint, modelName) && (!outputEntity || entity == outputEntity) &&
        (p.sourceId.isEmpty() || macMidi::endpointId(endpoint) == p.sourceId)) sources.append(endpoint);
  }
  if ((receive && sources.size() != 1) || sources.size() > 1) {
    close();
    return fail(error, QStringLiteral("A unique Roland %1 MIDI input was not found.").arg(modelName));
  }
  if (!sources.isEmpty()) {
    p.source = sources.first();
    if (macMidi::sourceReserved(p.source)) { p.source = 0; close(); return fail(error, QStringLiteral("Roland MIDI input is already in use.")); }
    macMidi::reserveSource(p.source, true);
  }
  status = MIDIOutputPortCreate(client, CFSTR("VTR-PON Roland output"), &p.output);
  if (status == noErr && receive) {
    p.inbox = std::make_shared<Inbox>();
    const auto inbox = p.inbox;
    status = MIDIInputPortCreateWithBlock(client, CFSTR("VTR-PON Roland input"), &p.input,
        ^(const MIDIPacketList *packets, void *) { inbox->receive(packets); });
    if (status == noErr) status = MIDIPortConnectSource(p.input, p.source, nullptr);
  }
  if (status != noErr) { close(); return fail(error, QStringLiteral("Roland CoreMIDI open failed (%1).").arg(status)); }
  p.destinationId = macMidi::endpointId(p.destination);
  if (p.source) p.sourceId = macMidi::endpointId(p.source);
  if (p.destinationId.isEmpty() || (p.source && p.sourceId.isEmpty())) {
    close(); return fail(error, QStringLiteral("Roland MIDI endpoint has no persistent UniqueID."));
  }
  p.health.start();
  return true;
}
QList<MacRolandMidi::Diagnostic> MacRolandMidi::takeDiagnostics() {
  return std::exchange(impl_->diagnostics, {});
}
bool MacRolandMidi::send(const QByteArray &message, QString *error) {
  if (!isOpen() || message.isEmpty()) return fail(error, QStringLiteral("Roland MIDI output is not connected."));
  if (impl_->queue.size() >= 128) {
    impl_->failure = QStringLiteral("Roland MIDI output queue is full. Reconnecting the switcher.");
    return fail(error, impl_->failure);
  }
  impl_->queue.push_back(message);
  return impl_->pump(error);
}
bool MacRolandMidi::poll(QList<QByteArray> &messages, QString *error) {
  auto &p = *impl_;
  if (!isOpen()) return fail(error, QStringLiteral("Roland MIDI output is not connected."));
  if (p.health.elapsed() >= 250) {
    p.health.restart();
    if (!macMidi::endpointAvailable(p.destination) || macMidi::endpointId(p.destination) != p.destinationId ||
        (p.source && (!macMidi::endpointAvailable(p.source) || macMidi::endpointId(p.source) != p.sourceId)))
      return fail(error, QStringLiteral("Roland V-8HD MIDI device disconnected. Waiting for the same device."));
  }
  if (!p.pump(error)) return false;
  if (p.inbox) {
    std::lock_guard lock(p.inbox->mutex);
    if (p.inbox->overflow) return fail(error, QStringLiteral("Roland MIDI input overflow. Reconnecting the switcher."));
    messages = std::exchange(p.inbox->messages, {});
  }
  return true;
}
}
