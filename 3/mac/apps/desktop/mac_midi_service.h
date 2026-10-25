#pragma once
#include <CoreMIDI/CoreMIDI.h>
#include <QString>

namespace vtrpon::macMidi {
OSStatus client(MIDIClientRef &result);
bool endpointAvailable(MIDIEndpointRef endpoint);
QString endpointId(MIDIEndpointRef endpoint);
bool sourceReserved(MIDIEndpointRef source);
void reserveSource(MIDIEndpointRef source, bool reserved);
}
