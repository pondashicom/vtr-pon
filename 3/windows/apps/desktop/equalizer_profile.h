#pragma once

#include <array>

namespace vtrpon {
// Yamaha MG fixed three-band EQ (MG10/MG12): frequencies, types and gain
// range follow the published specification. Q/shelf slope are our digital
// approximation; Yamaha does not specify them in the public spec sheet.
inline constexpr int kEqMaximumTenthsDb = 150;
inline constexpr double kEqMaximumDb = 15.0;
inline constexpr std::array<double, 3> kEqFrequencies{100.0, 2500.0, 10000.0};
inline constexpr std::array<const char *, 3> kEqFrequencyLabels{"100 Hz", "2.5 kHz", "10 kHz"};
inline constexpr double kEqMidQ = 0.7;
} // namespace vtrpon
