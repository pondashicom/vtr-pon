#include "timed_still_playout_clock.h"

#include <algorithm>

void TimedStillPlayoutClock::configure(
    qint64 startPositionMs, std::optional<qint64> endPositionMs,
    bool play) {
  if (startPositionMs < 0 || !endPositionMs ||
      *endPositionMs <= startPositionMs) {
    reset();
    return;
  }
  startPositionMs_ = startPositionMs;
  durationMs_ = *endPositionMs;
  positionMs_ = startPositionMs_;
  playing_ = play;
}

void TimedStillPlayoutClock::reset() {
  startPositionMs_ = 0;
  durationMs_ = -1;
  positionMs_ = 0;
  playing_ = false;
}

bool TimedStillPlayoutClock::active() const { return durationMs_ > 0; }

bool TimedStillPlayoutClock::playing() const {
  return active() && playing_;
}

qint64 TimedStillPlayoutClock::positionMs() const {
  return active() ? positionMs_ : -1;
}

qint64 TimedStillPlayoutClock::durationMs() const { return durationMs_; }

void TimedStillPlayoutClock::play() {
  if (active()) {
    playing_ = true;
  }
}

void TimedStillPlayoutClock::pause() { playing_ = false; }

void TimedStillPlayoutClock::seek(qint64 positionMs) {
  if (!active()) {
    return;
  }
  positionMs_ = std::clamp(positionMs, startPositionMs_, durationMs_);
}

void TimedStillPlayoutClock::restart(bool play) {
  if (!active()) {
    return;
  }
  positionMs_ = startPositionMs_;
  playing_ = play;
}

void TimedStillPlayoutClock::stopAtStart() { restart(false); }

void TimedStillPlayoutClock::holdAtEnd() {
  if (!active()) {
    return;
  }
  positionMs_ = durationMs_;
  playing_ = false;
}

bool TimedStillPlayoutClock::advance(qint64 elapsedMs) {
  if (!playing() || elapsedMs <= 0 || positionMs_ >= durationMs_) {
    return false;
  }
  const qint64 remaining = durationMs_ - positionMs_;
  if (elapsedMs < remaining) {
    positionMs_ += elapsedMs;
    return false;
  }
  holdAtEnd();
  return true;
}
