#include "mac_midi_input.h"
#include "mac_midi_service.h"
#include <QHash>

#include <CoreMIDI/CoreMIDI.h>
#include <QDebug>
#include <future>
#include <mutex>
#include <thread>

namespace vtrpon {
namespace {
struct Service {
  std::shared_future<std::pair<OSStatus, MIDIClientRef>> ready;
  Service() {
    std::promise<std::pair<OSStatus, MIDIClientRef>> prepared;
    ready = prepared.get_future().share();
    std::thread([prepared = std::move(prepared)]() mutable {
      MIDIClientRef value = 0;
      const auto status = MIDIClientCreate(CFSTR("VTR-PON MIDI control"),
          [](const MIDINotification *, void *) {}, nullptr, &value);
      prepared.set_value({status, value});
      // CoreMIDI's first client anchors setup notifications to this run loop.
      // An enumeration worker exits after a query, so it must not own that
      // first client. Otherwise external sources appear initially but sources
      // added/recreated later never reach the process's cached source list.
      if (status == noErr) CFRunLoopRun();
    }).detach();
  }
  OSStatus client(MIDIClientRef &result) {
    const auto [status, value] = ready.get();
    if (status != noErr) return status;
    result = value;
    return noErr;
  }
};
Service &service() {
  // One process-lifetime client, also used by background enumeration. Apple
  // discourages disposing the last client and creating it again. Keeping its
  // notification subscription alive also observes changes in OTHER processes.
  // CoreMIDI releases the client when the process exits; ports close normally.
  static auto *value = new Service;
  return *value;
}
QString stringProperty(MIDIObjectRef object, CFStringRef key) {
  CFStringRef value = nullptr;
  if (MIDIObjectGetStringProperty(object, key, &value) != noErr || !value) return {};
  const auto result = QString::fromCFString(value);
  CFRelease(value);
  return result;
}
QString endpointId(MIDIEndpointRef endpoint) {
  SInt32 uid = 0;
  if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uid) != noErr || !uid) return {};
  return QStringLiteral("coremidi:%1").arg(uid);
}
bool unavailable(MIDIObjectRef object) {
  SInt32 offline = 0, hidden = 0;
  MIDIObjectGetIntegerProperty(object, kMIDIPropertyOffline, &offline);
  MIDIObjectGetIntegerProperty(object, kMIDIPropertyPrivate, &hidden);
  return offline || hidden;
}
bool availableSource(MIDIEndpointRef endpoint) {
  if (!endpoint || unavailable(endpoint)) return false;
  MIDIEntityRef entity = 0;
  MIDIDeviceRef device = 0;
  // Virtual sources have no entity or device; physical devices can report
  // offline on a parent while keeping their endpoint in the MIDI setup.
  if (MIDIEndpointGetEntity(endpoint, &entity) == noErr && entity) {
    if (unavailable(entity)) return false;
    if (MIDIEntityGetDevice(entity, &device) == noErr && device && unavailable(device)) return false;
  }
  return true;
}
std::mutex reservationMutex;
QHash<MIDIEndpointRef, unsigned> reservations;
struct Session {
  explicit Session(std::shared_ptr<midiControl::MessageQueue> inbox) : queue(std::move(inbox)) {}
  std::shared_ptr<midiControl::MessageQueue> queue;
  std::mutex mutex;
  bool active = true;
  MIDIEndpointRef source = 0;
  void receive(const MIDIEventList *events) {
    std::lock_guard lock(mutex);
    if (!active || macMidi::sourceReserved(source) || !events || events->protocol != kMIDIProtocol_1_0) return;
    const auto *packet = &events->packet[0];
    // UMP message lengths, including reserved types. Skipping the entire
    // message prevents SysEx/utility words from masquerading as a Note or CC.
    static constexpr unsigned lengths[16]{1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    for (UInt32 n = 0; n < events->numPackets; ++n, packet = MIDIEventPacketNext(packet)) {
      for (UInt32 i = 0; i < packet->wordCount;) {
        const auto word = packet->words[i];
        const auto type = word >> 28;
        const auto words = lengths[type];
        if (words > packet->wordCount - i) break;
        if (type == 2) {
          // The requested MIDI 1.0 protocol is converted by CoreMIDI, so the
          // existing 7-bit Note/CC mapping contract also applies to this port.
          queue->push(((word >> 16) & 0xff) | (word & 0xff00) | ((word & 0xff) << 16));
        }
        i += words;
      }
    }
  }
  void deactivate() {
    // Wait only for the small inbox write already in flight, never for Qt or
    // Controller work. Later callbacks keep this session alive but do nothing.
    std::lock_guard lock(mutex);
    active = false;
  }
};
} // namespace

namespace macMidi {
OSStatus client(MIDIClientRef &result) { return service().client(result); }
bool endpointAvailable(MIDIEndpointRef endpoint) { return availableSource(endpoint); }
QString endpointId(MIDIEndpointRef endpoint) { return ::vtrpon::endpointId(endpoint); }
bool sourceReserved(MIDIEndpointRef source) {
  std::lock_guard lock(reservationMutex);
  return reservations.contains(source);
}
void reserveSource(MIDIEndpointRef source, bool reserved) {
  if (!source) return;
  std::lock_guard lock(reservationMutex);
  if (reserved) ++reservations[source];
  else if (auto it = reservations.find(source); it != reservations.end() && --it.value() == 0) reservations.erase(it);
}
}

QList<MidiControlInput::Device> MacMidiInput::devices() {
  MIDIClientRef client = 0;
  const auto status = service().client(client);
  if (status != noErr) { qWarning() << "MIDI CoreMIDI discovery failed status=" << status; return {}; }
  QList<MidiControlInput::Device> devices;
  const auto count = MIDIGetNumberOfSources();
  for (ItemCount index = 0; index < count; ++index) {
    const auto endpoint = MIDIGetSource(index);
    if (!availableSource(endpoint)) continue;
    const auto id = endpointId(endpoint);
    if (id.isEmpty()) continue;
    auto name = stringProperty(endpoint, kMIDIPropertyDisplayName);
    if (name.isEmpty()) name = stringProperty(endpoint, kMIDIPropertyName);
    devices.append({id, name.isEmpty() ? id : name, endpoint});
  }
  return devices;
}

class MacMidiInput::Impl {
public:
  explicit Impl(std::shared_ptr<midiControl::MessageQueue> queue) : queue(std::move(queue)) {}
  std::shared_ptr<midiControl::MessageQueue> queue;
  std::shared_ptr<Session> session;
  MIDIPortRef port = 0;
  MIDIEndpointRef source = 0;
};
MacMidiInput::MacMidiInput(std::shared_ptr<midiControl::MessageQueue> queue)
    : impl_(std::make_unique<Impl>(std::move(queue))) {}
MacMidiInput::~MacMidiInput() { close(); }
bool MacMidiInput::isOpen() const { return impl_->port != 0; }
bool MacMidiInput::reserved() const { return macMidi::sourceReserved(impl_->source); }
void MacMidiInput::close() {
  if (impl_->session) impl_->session->deactivate();
  if (impl_->port) {
    if (impl_->source) MIDIPortDisconnectSource(impl_->port, impl_->source);
    MIDIPortDispose(impl_->port);
  }
  impl_->port = 0; impl_->source = 0; impl_->session.reset();
}
QString MacMidiInput::open(const MidiControlInput::Device &device) {
  close();
  MIDIClientRef client = 0;
  OSStatus result = service().client(client);
  if (result == noErr && (!availableSource(device.index) || endpointId(device.index) != device.id))
    result = kMIDIObjectNotFound;
  if (result == noErr && macMidi::sourceReserved(device.index))
    return QStringLiteral("This MIDI input is reserved for Roland V-8HD control.");
  if (result == noErr) {
    impl_->session = std::make_shared<Session>(impl_->queue);
    const auto session = impl_->session;
    session->source = device.index;
    result = MIDIInputPortCreateWithProtocol(client, CFSTR("VTR-PON control input"),
        kMIDIProtocol_1_0, &impl_->port, ^(const MIDIEventList *events, void *) { session->receive(events); });
  }
  if (result == noErr) {
    impl_->source = device.index;
    result = MIDIPortConnectSource(impl_->port, impl_->source, nullptr);
  }
  if (result != noErr) { close(); return QStringLiteral("CoreMIDI OSStatus %1").arg(result); }
  qInfo() << "MIDI CoreMIDI opened uid=" << device.id << "source=" << device.index << "name=" << device.name;
  return {};
}
} // namespace vtrpon
