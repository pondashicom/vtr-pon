#pragma once
#include "midi_control_input.h"
#include <mutex>
#include <optional>
#include <thread>

namespace vtrpon {
// WinMM enumeration can wait on a driver for seconds. Only value data crosses
// this boundary: a late result never calls a destroyed QObject, and destruction
// never waits for the driver. At most one query per instance is outstanding.
class MidiDeviceDiscovery final {
public:
  using Devices = QList<MidiControlInput::Device>;
  using Enumerator = std::function<Devices()>;
  explicit MidiDeviceDiscovery(Enumerator enumerate = MidiControlInput::devices)
      : enumerate_(std::move(enumerate)) {}
  void request() {
    const auto state = state_;
    std::lock_guard lock(state->mutex);
    if (state->running || state->result) return;
    state->running = true;
    try {
      std::thread([state, enumerate = enumerate_] {
        Devices devices;
        try { devices = enumerate(); } catch (...) { /* Retry on the next query. */ }
        std::lock_guard done(state->mutex);
        state->result = std::move(devices);
        state->running = false;
      }).detach();
    } catch (...) {
      state->running = false;
      state->result = Devices{};
    }
  }
  std::optional<Devices> takeResult() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->result);
    state_->result.reset();
    return result;
  }
private:
  struct State {
    std::mutex mutex;
    bool running = false;
    std::optional<Devices> result;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
  Enumerator enumerate_;
};
} // namespace vtrpon
