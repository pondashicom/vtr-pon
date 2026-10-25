#pragma once
#include <CoreMIDI/CoreMIDI.h>
#include <QString>
#include <array>
#include <cstddef>
#include <vector>

// Test-only CoreMIDI source. It never sends to a physical MIDI destination.
class MacMidiSource final {
public:
  MIDIClientRef client = 0;
  MIDIEndpointRef endpoint = 0;
  SInt32 uid = 0;
  QString name;
  explicit MacMidiSource(QString name) : name(std::move(name)) {
    MIDIClientCreateWithBlock(CFSTR("VTR-PON MIDI test source"), &client, nullptr);
    create();
  }
  ~MacMidiSource() { disconnect(); if (client) MIDIClientDispose(client); }
  bool create() {
    if (!client || endpoint) return false;
    const auto label = name.toCFString();
    const auto status = MIDISourceCreateWithProtocol(client, label, kMIDIProtocol_1_0, &endpoint);
    CFRelease(label);
    if (status != noErr) return false;
    if (uid) {
      if (MIDIObjectSetIntegerProperty(endpoint, kMIDIPropertyUniqueID, uid) != noErr) return false;
    } else if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uid) != noErr) return false;
    return true;
  }
  void disconnect() { if (endpoint) MIDIEndpointDispose(endpoint); endpoint = 0; }
  QString id() const { return QStringLiteral("coremidi:%1").arg(uid); }
  bool send(const std::vector<UInt32> &words) {
    alignas(MIDIEventList) std::array<std::byte, 4096> storage{};
    auto *events = reinterpret_cast<MIDIEventList *>(storage.data());
    auto *packet = MIDIEventListInit(events, kMIDIProtocol_1_0);
    packet = MIDIEventListAdd(events, storage.size(), packet, 0, words.size(), words.data());
    return packet && endpoint && MIDIReceivedEventList(endpoint, events) == noErr;
  }
  bool voice(unsigned status, unsigned number, unsigned value) {
    return send({0x20000000U | (status << 16) | (number << 8) | value});
  }
};
