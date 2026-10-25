#pragma once

#include "midi_control_input.h"
#include "midi_message_queue.h"

namespace vtrpon {
// CoreMIDI owns the receive thread. Only this bounded inbox crosses into the
// Controller; the callback never retains or calls a QObject.
class MacMidiInput final {
public:
  explicit MacMidiInput(std::shared_ptr<midiControl::MessageQueue> queue);
  ~MacMidiInput();
  static QList<MidiControlInput::Device> devices();
  QString open(const MidiControlInput::Device &device);
  void close();
  bool isOpen() const;
  bool reserved() const;
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace vtrpon
