#pragma once

// Factories replaced only inside the contract-test process. Keep the production
// names so the tests exercise the normal platform-specific initialization path.
namespace PlayoutTestPlatform {
#ifdef __APPLE__
inline constexpr const char *audioSink = "osxaudiosink";
inline constexpr const char *videoSource = "vtrponavfvideosrc";
inline constexpr const char *audioSource = "vtrponcoreaudiosrc";
#else
inline constexpr const char *audioSink = "wasapi2sink";
inline constexpr const char *videoSource = "mfvideosrc";
inline constexpr const char *audioSource = "vtrponwasapirawsrc";
#endif
} // namespace PlayoutTestPlatform
