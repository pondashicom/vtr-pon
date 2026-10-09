#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace vtrpon::midiControl {
// Driver callbacks only touch this bounded, Qt-free inbox. Continuous absolute
// CC must never occupy the button queues, including while learning a new input.
class MessageQueue {
public:
  static constexpr std::size_t controls = 16 * 128;
  static constexpr std::size_t capacity = 1024;
  enum class CcMode : std::uint8_t { Absolute, Button, Relative };
  using Modes = std::array<CcMode, controls>;
  struct Absolute {
    std::uint32_t packed = 0;
    int minimum = 0, maximum = 0;
  };
  struct Batch {
    std::array<std::uint32_t, capacity> notes{}, buttons{}, relative{};
    std::array<Absolute, controls> absolute{};
    std::size_t noteCount = 0, buttonCount = 0, relativeCount = 0, absoluteCount = 0;
    std::size_t received = 0;
    bool noteOverflow = false, buttonOverflow = false, relativeOverflow = false;
  };
  MessageQueue() { buttonStates_.fill(-1); }
  void setModes(const Modes &modes) {
    std::lock_guard lock(mutex_);
    modes_ = modes;
    buttonStates_.fill(-1);
  }
  void push(std::uint32_t packed) {
    const int status = packed & 0xff;
    const int kind = status & 0xf0;
    const int number = (packed >> 8) & 0xff;
    const int value = (packed >> 16) & 0xff;
    if ((kind != 0x80 && kind != 0x90 && kind != 0xb0) || number > 127 || value > 127) return;
    std::lock_guard lock(mutex_);
    ++received_;
    if (kind != 0xb0) {
      append(notes_, noteCount_, noteOverflow_, packed);
      return;
    }
    const auto key = static_cast<std::size_t>((status & 15) * 128 + number);
    if (modes_[key] == CcMode::Button) {
      const int pressed = value >= 64;
      if (buttonStates_[key] == pressed) return;
      buttonStates_[key] = static_cast<std::int8_t>(pressed);
      append(buttons_, buttonCount_, buttonOverflow_, packed);
    } else if (modes_[key] == CcMode::Relative) {
      // Every relative encoder packet is a step, even repeated equal values.
      append(relative_, relativeCount_, relativeOverflow_, packed);
    } else {
      auto &latest = absolute_[key];
      if (!dirty_[key]) {
        dirty_[key] = true;
        dirtyKeys_[absoluteCount_++] = key;
        latest.minimum = latest.maximum = value;
      } else {
        latest.minimum = std::min(latest.minimum, value);
        latest.maximum = std::max(latest.maximum, value);
      }
      latest.packed = packed;
    }
  }
  Batch take() {
    std::lock_guard lock(mutex_);
    Batch batch;
    batch.noteOverflow = noteOverflow_;
    batch.buttonOverflow = buttonOverflow_;
    batch.relativeOverflow = relativeOverflow_;
    batch.noteCount = noteOverflow_ ? 0 : noteCount_;
    batch.buttonCount = buttonOverflow_ ? 0 : buttonCount_;
    batch.relativeCount = relativeOverflow_ ? 0 : relativeCount_;
    std::copy_n(notes_.begin(), batch.noteCount, batch.notes.begin());
    std::copy_n(buttons_.begin(), batch.buttonCount, batch.buttons.begin());
    std::copy_n(relative_.begin(), batch.relativeCount, batch.relative.begin());
    batch.absoluteCount = absoluteCount_;
    for (std::size_t index = 0; index < absoluteCount_; ++index) {
      const auto key = dirtyKeys_[index];
      batch.absolute[index] = absolute_[key];
      dirty_[key] = false;
    }
    batch.received = received_;
    noteCount_ = buttonCount_ = relativeCount_ = absoluteCount_ = received_ = 0;
    noteOverflow_ = buttonOverflow_ = relativeOverflow_ = false;
    return batch;
  }
private:
  static void append(std::array<std::uint32_t, capacity> &queue, std::size_t &count,
                     bool &overflow, std::uint32_t packed) {
    if (count == queue.size()) { overflow = true; return; }
    queue[count++] = packed;
  }
  std::mutex mutex_;
  Modes modes_{};
  std::array<std::int8_t, controls> buttonStates_{};
  std::array<std::uint32_t, capacity> notes_{}, buttons_{}, relative_{};
  std::array<Absolute, controls> absolute_{};
  std::array<bool, controls> dirty_{};
  std::array<std::size_t, controls> dirtyKeys_{};
  std::size_t noteCount_ = 0, buttonCount_ = 0, relativeCount_ = 0, absoluteCount_ = 0, received_ = 0;
  bool noteOverflow_ = false, buttonOverflow_ = false, relativeOverflow_ = false;
};
} // namespace vtrpon::midiControl
