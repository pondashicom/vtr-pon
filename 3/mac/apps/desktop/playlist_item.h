#pragma once

#include "equalizer_profile.h"

#include <QString>
#include <QtGlobal>

#include <optional>
#include <array>
#include <algorithm>
#include <cmath>

enum class PlaylistStartMode { Pause, Play };
enum class PlaylistEndMode { Stop, Pause, Repeat, Next, Goto };
enum class PlaylistRepeatEndMode { Stop, Pause, Next, Goto };
enum class PlaylistCueState { None, Pending, Loading, Ready, Error };
enum class PlaylistMediaType {
  Unknown,
  Video,
  Audio,
  StillImage,
  Presentation,
  Pdf,
};
enum class PlaylistSourceType { File, Uvc, PowerPoint, Pdf };
enum class PlaylistPowerPointMode {
  None,
  NativeSlideShow,
  VideoConversion,
  PngSequence,
};
enum class PlaylistItemColor { Default, Red, Yellow, Blue, Green };
enum class PlaylistItemConversionStage {
  Waiting,
  Preparing,
  Exporting,
  Rendering,
  Encoding,
  AnalysisWaiting,
  Analyzing,
};
enum class PlaylistMode { Standard, Bgm };

struct PlaylistItemConversionState {
  PlaylistItemConversionStage stage = PlaylistItemConversionStage::Waiting;
  int progressPermille = 0;

  bool operator==(const PlaylistItemConversionState &) const = default;
};

inline constexpr qint64 kDefaultItemFadeDurationMs = 1000;
inline constexpr qint64 kMinimumItemFadeDurationMs = 100;
inline constexpr qint64 kMaximumItemFadeDurationMs = 60000;
inline constexpr int kInfiniteItemRepeatCount = 0;
inline constexpr int kMaximumItemRepeatCount = 9999;
inline constexpr int kMinimumItemVolumeTenthsDb = -960;
inline constexpr int kMaximumItemVolumeTenthsDb = 60;
inline constexpr int kDefaultItemVolumeTenthsDb = 0;

struct PlaylistItemSettings {
  qint64 inPointMs = 0;
  std::optional<qint64> outPointMs = std::nullopt;
  PlaylistStartMode startMode = PlaylistStartMode::Pause;
  PlaylistEndMode endMode = PlaylistEndMode::Pause;
  bool fadeInEnabled = false;
  qint64 fadeInDurationMs = kDefaultItemFadeDurationMs;
  bool fadeOutEnabled = false;
  qint64 fadeOutDurationMs = kDefaultItemFadeDurationMs;
  bool endCrossfadeEnabled = false;
  qint64 endCrossfadeDurationMs = kDefaultItemFadeDurationMs;
  quint64 gotoTargetItemId = 0;
  int repeatCount = kInfiniteItemRepeatCount;
  PlaylistRepeatEndMode repeatEndMode = PlaylistRepeatEndMode::Pause;
  int nominalVolumeTenthsDb = kDefaultItemVolumeTenthsDb;
  int nominalAudioDelayFrames = 0; // 0 = OFF, 1 ... 6 at 30 fps.
  std::array<int, 3> nominalEqTenthsDb{}; // LOW, MID, HIGH; -150 ... +150.

  bool operator==(const PlaylistItemSettings &) const = default;
};

struct PlaylistItemRegistration {
  QString filePath;
  QString displayName;
  PlaylistSourceType sourceType = PlaylistSourceType::File;
  PlaylistMediaType mediaType = PlaylistMediaType::Unknown;
  PlaylistPowerPointMode powerPointMode = PlaylistPowerPointMode::None;
  PlaylistItemColor color = PlaylistItemColor::Default;
  bool separatorBelow = false;
  PlaylistItemSettings settings;
  int playlistNumber = 0;
  std::optional<PlaylistItemConversionState> conversionState = std::nullopt;
};

struct PlaylistItemState {
  quint64 id = 0;
  int itemNumber = 0;
  int playlistNumber = 0;
  QString filePath;
  QString displayName;
  PlaylistSourceType sourceType = PlaylistSourceType::File;
  PlaylistMediaType mediaType = PlaylistMediaType::Unknown;
  PlaylistPowerPointMode powerPointMode = PlaylistPowerPointMode::None;
  PlaylistItemColor color = PlaylistItemColor::Default;
  bool separatorBelow = false;
  PlaylistItemSettings settings;

  bool operator==(const PlaylistItemState &) const = default;
};

// A repeated overlap must release the outgoing deck before the following one.
// Durations below the supported fade minimum use a prepared CUT instead.
inline qint64 playlistRepeatCrossfadeDurationMs(const PlaylistItemState &item,
                                               qint64 endPositionMs,
                                               double rate = 1.0) {
  if (item.sourceType != PlaylistSourceType::File ||
      item.mediaType == PlaylistMediaType::Audio ||
      !item.settings.endCrossfadeEnabled ||
      endPositionMs <= item.settings.inPointMs || !std::isfinite(rate) || rate <= 0.0) {
    return 0;
  }
  const double maximumMs = (endPositionMs - item.settings.inPointMs) / rate / 2.0;
  const qint64 duration = static_cast<qint64>((std::min)(
      maximumMs, static_cast<double>(std::clamp(item.settings.endCrossfadeDurationMs,
          kMinimumItemFadeDurationMs, kMaximumItemFadeDurationMs))));
  return duration >= kMinimumItemFadeDurationMs ? duration : 0;
}

struct PlaylistState {
  int number = 0;
  QString name;
  PlaylistMode mode = PlaylistMode::Standard;
  bool manualCrossfadeEnabled = false;

  bool operator==(const PlaylistState &) const = default;
};
