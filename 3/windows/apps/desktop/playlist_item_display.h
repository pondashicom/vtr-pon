#pragma once

#include "language_text.h"
#include "playlist_item.h"

#include <QString>

#include <algorithm>

namespace vtrpon {

inline QString playlistEndModeName(PlaylistEndMode mode) {
  switch (mode) {
  case PlaylistEndMode::Stop:
    return uiText("common.stop");
  case PlaylistEndMode::Pause:
    return uiText("common.pause");
  case PlaylistEndMode::Repeat:
    return uiText("common.repeat");
  case PlaylistEndMode::Next:
    return uiText("common.next");
  case PlaylistEndMode::Goto:
    return uiText("common.goto");
  }
  return QString();
}

inline QString playlistRepeatEndModeName(PlaylistRepeatEndMode mode) {
  switch (mode) {
  case PlaylistRepeatEndMode::Stop:
    return uiText("common.stop");
  case PlaylistRepeatEndMode::Pause:
    return uiText("common.pause");
  case PlaylistRepeatEndMode::Next:
    return uiText("common.next");
  case PlaylistRepeatEndMode::Goto:
    return uiText("common.goto");
  }
  return QString();
}

inline QString formatPlaylistItemEndMode(const PlaylistItemSettings &settings,
                                         int currentPlayCount) {
  QString end = playlistEndModeName(settings.endMode);
  if (settings.endMode == PlaylistEndMode::Repeat) {
    const int displayedPlayCount = (std::max)(currentPlayCount, 0);
    if (settings.repeatCount == kInfiniteItemRepeatCount) {
      end += uiText("playlist.item.repeat_infinite")
                 .arg(displayedPlayCount);
    } else {
      end += uiText("playlist.item.repeat_finite")
                 .arg(displayedPlayCount)
                 .arg(settings.repeatCount)
                 .arg(playlistRepeatEndModeName(settings.repeatEndMode));
    }
  }
  if (settings.fadeOutEnabled) {
    end += uiText("playlist.item.fade_out");
  }
  const bool repeatAutomaticTransition = settings.endMode == PlaylistEndMode::Repeat;
  if ((settings.endMode == PlaylistEndMode::Next ||
       settings.endMode == PlaylistEndMode::Goto ||
      repeatAutomaticTransition) &&
      settings.endCrossfadeEnabled) {
    end += uiText("playlist.item.end_crossfade");
  }
  return end;
}

} // namespace vtrpon
