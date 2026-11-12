# VTR-PON selected-camera capture plug-in

This shared module is adapted from `sys/winks` in gst-plugins-bad 1.28.5.
The upstream archive has SHA-256 `d8af55faef2958c1a8663751475ee46f5164877cf4d8c5913ea906ef180aeb71`.
See the copyright headers and `COPYING`. [Upstream source](https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-1.28.5.tar.xz)

The VTR-PON modification of 2026-11-12 opens only the supplied camera interface in
`gst_ks_video_src_open_device`. It omits device ranking, clock-device discovery
and device-provider registration. The element and its GObject types have distinct
names so the standard winks plug-in can coexist. Legacy driver quirks default to off.
Capture startup errors include the Windows error code so the host can recognize
an insufficient USB bandwidth reservation.

Build the module from the Windows project with `cmake --build build --target gstvtrponks`.
The host loads `gstvtrponks.dll` beside its executable when recovering a selected USB camera.
