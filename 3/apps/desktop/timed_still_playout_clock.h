#pragma once

#include <QtGlobal>

#include <optional>

// Logical playout time for a still image. The media pipeline remains paused so
// that the original image (and its alpha channel for DSK) is held unchanged;
// the controller advances this clock only while the item is logically playing.
class TimedStillPlayoutClock final {
public:
  void configure(qint64 startPositionMs,
                 std::optional<qint64> endPositionMs, bool play);
  void reset();

  bool active() const;
  bool playing() const;
  qint64 positionMs() const;
  qint64 durationMs() const;

  void play();
  void pause();
  void seek(qint64 positionMs);
  void restart(bool play);
  void stopAtStart();
  void holdAtEnd();

  // Returns true exactly when this advance reaches the configured duration.
  bool advance(qint64 elapsedMs);

private:
  qint64 startPositionMs_ = 0;
  qint64 durationMs_ = -1;
  qint64 positionMs_ = 0;
  bool playing_ = false;
};
