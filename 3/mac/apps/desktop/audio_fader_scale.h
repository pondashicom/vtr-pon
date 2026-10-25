#pragma once

#include <algorithm>
#include <cmath>

namespace vtrpon::audioFaderScale {
inline constexpr int silence = -960;
inline constexpr int maximum = 60;
inline constexpr double unityPosition = 0.80;
inline constexpr double taperExponent = 4.0;

inline double positionForValue(int value) {
  if (value <= silence) return 0.0;
  if (value <= 0)
    return unityPosition * std::pow(std::pow(10.0, value / 200.0), 1.0 / taperExponent);
  return std::clamp(unityPosition + (1.0 - unityPosition) * value / maximum, 0.0, 1.0);
}

inline int valueForPosition(double position) {
  position = std::clamp(position, 0.0, 1.0);
  if (position <= 0.0) return silence;
  const double value = position <= unityPosition
      ? 200.0 * taperExponent * std::log10(position / unityPosition)
      : (position - unityPosition) * maximum / (1.0 - unityPosition);
  return std::clamp(static_cast<int>(std::lround(value)), silence, maximum);
}
} // namespace vtrpon::audioFaderScale
