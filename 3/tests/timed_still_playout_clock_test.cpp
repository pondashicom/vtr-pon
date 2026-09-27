#include "timed_still_playout_clock.h"

#include "playlist_item_playout_state_machine.h"
#include "playlist_model.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTemporaryFile>

#include <optional>

namespace {

bool check(bool condition, const char *message) {
  if (!condition) {
    qCritical().noquote() << message;
  }
  return condition;
}

bool verifyInactiveClock() {
  TimedStillPlayoutClock clock;
  clock.configure(0, std::nullopt, true);
  return check(!clock.active(), "untimed still must not activate the clock") &&
         check(!clock.playing(), "untimed still must not report playing") &&
         check(clock.positionMs() == -1,
               "untimed still must not expose a synthetic position") &&
         check(!clock.advance(1000),
               "untimed still must never synthesize completion");
}

bool verifyPlayPauseAndSeek() {
  TimedStillPlayoutClock clock;
  clock.configure(0, 5000, true);
  if (!check(clock.active() && clock.playing(),
             "timed still must start in the requested Play state") ||
      !check(!clock.advance(1250) && clock.positionMs() == 1250,
             "Play must advance by elapsed time")) {
    return false;
  }
  clock.pause();
  if (!check(!clock.advance(2000) && clock.positionMs() == 1250,
             "Pause must hold the current position")) {
    return false;
  }
  clock.seek(4800);
  clock.play();
  return check(clock.advance(250), "crossing DUR must complete once") &&
         check(clock.positionMs() == 5000 && !clock.playing(),
               "completion must clamp to DUR and pause") &&
         check(!clock.advance(1000),
               "a completed clock must not complete repeatedly");
}

bool verifyEndModeClockOperations() {
  TimedStillPlayoutClock clock;
  clock.configure(0, 3000, true);
  (void)clock.advance(3000);

  clock.restart(true);
  if (!check(clock.positionMs() == 0 && clock.playing(),
             "Repeat must reset to zero and resume Play")) {
    return false;
  }
  (void)clock.advance(750);
  clock.stopAtStart();
  if (!check(clock.positionMs() == 0 && !clock.playing(),
             "Stop must reset to zero and hold")) {
    return false;
  }
  clock.play();
  (void)clock.advance(900);
  clock.holdAtEnd();
  return check(clock.positionMs() == 3000 && !clock.playing(),
               "Pause End Mode must hold at DUR");
}

bool verifyNonZeroInPoint() {
  TimedStillPlayoutClock clock;
  clock.configure(2000, 5000, true);
  if (!check(clock.active() && clock.positionMs() == 2000,
             "timed still must start at its configured In point") ||
      !check(!clock.advance(2999) && clock.positionMs() == 4999,
             "non-zero In must use the In-to-Out range duration") ||
      !check(clock.advance(1) && clock.positionMs() == 5000,
             "non-zero In must complete exactly at Out")) {
    return false;
  }
  clock.restart(false);
  return check(clock.positionMs() == 2000 && !clock.playing(),
               "Stop must return a non-zero range to its In point");
}

bool verifyStateMachineEndModes() {
  QTemporaryFile source;
  if (!source.open()) {
    return check(false, "could not create the still-image source fixture");
  }

  const auto verifyMode = [&source](PlaylistEndMode mode) {
    PlaylistModel model;
    PlaylistItemRegistration registration;
    registration.filePath = source.fileName();
    registration.mediaType = PlaylistMediaType::StillImage;
    registration.settings.startMode = PlaylistStartMode::Play;
    registration.settings.endMode = mode;
    registration.settings.outPointMs = 1000;
    const QList<quint64> ids = model.addItems({registration});
    if (ids.size() != 1) {
      return false;
    }
    PlaylistItemPlayoutStateMachine machine(&model);
    TimedStillPlayoutClock clock;
    if (!machine.prepare(ids.first()) ||
        machine.start() != PlaylistStartMode::Play) {
      return false;
    }
    clock.configure(registration.settings.inPointMs,
                    registration.settings.outPointMs, true);
    return clock.advance(1000) && machine.finish() == mode;
  };

  if (!check(verifyMode(PlaylistEndMode::Stop),
             "DUR must dispatch Stop through the state machine") ||
      !check(verifyMode(PlaylistEndMode::Pause),
             "DUR must dispatch Pause through the state machine") ||
      !check(verifyMode(PlaylistEndMode::Next),
             "DUR must dispatch Next through the state machine") ||
      !check(verifyMode(PlaylistEndMode::Goto),
             "DUR must dispatch Goto through the state machine")) {
    return false;
  }

  PlaylistModel repeatModel;
  PlaylistItemRegistration repeatRegistration;
  repeatRegistration.filePath = source.fileName();
  repeatRegistration.mediaType = PlaylistMediaType::StillImage;
  repeatRegistration.settings.startMode = PlaylistStartMode::Play;
  repeatRegistration.settings.endMode = PlaylistEndMode::Repeat;
  repeatRegistration.settings.repeatCount = 2;
  repeatRegistration.settings.repeatEndMode = PlaylistRepeatEndMode::Next;
  repeatRegistration.settings.outPointMs = 1000;
  const QList<quint64> repeatIds = repeatModel.addItems({repeatRegistration});
  PlaylistItemPlayoutStateMachine repeatMachine(&repeatModel);
  TimedStillPlayoutClock repeatClock;
  if (repeatIds.size() != 1 || !repeatMachine.prepare(repeatIds.first()) ||
      repeatMachine.start() != PlaylistStartMode::Play) {
    return check(false, "could not prepare the Repeat still-image fixture");
  }
  repeatClock.configure(repeatRegistration.settings.inPointMs,
                        repeatRegistration.settings.outPointMs, true);
  if (!repeatClock.advance(1000) ||
      repeatMachine.finish() != PlaylistEndMode::Repeat) {
    return check(false, "first DUR must dispatch Repeat");
  }
  repeatClock.restart(true);
  return check(repeatClock.advance(1000) &&
                   repeatMachine.finish() == PlaylistEndMode::Next,
               "final repeated DUR must dispatch Repeat End Mode");
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  return verifyInactiveClock() && verifyPlayPauseAndSeek() &&
                 verifyEndModeClockOperations() && verifyNonZeroInPoint() &&
                 verifyStateMachineEndModes()
             ? 0
             : 1;
}
