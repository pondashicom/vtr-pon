# Windows release notes

## VTR-PON 3.0.0-beta.3 — distribution candidate (2026-11-05)

- Add return monitor output to NDI, DeckLink and the virtual camera. NDI uses a
  separate source; DeckLink requires a separate hardware channel for concurrent
  ON AIR and return output. The virtual camera selects ON AIR or return monitor
  video and does not carry audio.
- Show add-on runtime readiness, initialization failures and operational status
  in Settings. Preserve vendor diagnostic details and warn at startup when an
  enabled feature needs an unavailable runtime.
- Fix Roland LAN authentication, acknowledgement handling, state polling,
  initial black states and delayed local-control echoes. Correct MIDI tally
  handling and treat V-8HD Program Change as memory recall rather than input state.
- Harden Windows WinMM receive-buffer handling, reject data from old connection
  generations, and avoid combining ports from different devices of the same model.

- Add optional USB MIDI control for V-160HD, V-80HD, V-1-4K, VR-6HD, and
  VR-120HD. Choose LAN or USB MIDI under **Settings → Roland integration →
  Connection**. Existing settings keep LAN selected, and switching to USB retains
  the saved LAN connection settings. USB control requires the model's MIDI input
  and output ports; additional models remain trial support pending hardware checks.
- VR-120HD FTB linking is available over LAN only. Selecting USB MIDI disables
  FTB linking and preserves the saved LAN FTB preference.

The normal Windows RelWithDebInfo build passed all 52 CTest cases and eight
Python release-source tests on 2026-11-05. The installer and ZIP are generated
as a new distribution candidate. Automated tests use simulated devices;
physical Roland, NDI and DeckLink devices, visible
media, physical audio, long-running use and clean-PC installation require
separate acceptance. The beta.2 download record below remains historical.

## VTR-PON 3.0.0-beta.2 — released 2026-10-25

- Move Windows sources to `3/windows`, alongside the Mac source in `3/mac`.
- Add Roland switcher trial profiles, including VR-50HD MK II and VR-120HD LAN
  control, separate ATEM/Roland settings, operation checks and JSON reports.
  Email preparation leaves the final sending action to the user. Additional
  models require physical-device verification; trial support is not a claim
  that every model and operation has passed hardware acceptance.
- Recover VST3 responses completed before the deadline when the host resumes
  late, and defer bridge resource cleanup away from audio processing.
- Classify PDF read-access failures correctly and report failed cleanup after
  cancelled document conversion, including the remaining output directory.
- Fix the MIDI test's source-path dependency and include its Roland profile
  sources so it builds after the directory move.

The installer and ZIP are available from the
[official Windows download page](https://pondashi.com/vtr-pon/download/windows/).
Their SHA-256 values and corresponding source commit are recorded in the
[distribution record](release/distribution-3.0.0-beta.2.json). The Windows update
catalog now points to beta.2. Package generation and automated checks do not
establish clean-PC installation, physical audio or hardware acceptance.

## VTR-PON 3.0.0-beta.1

Initial Windows 11 x64 beta.

- Prepare video, audio, timed stills, PDF, PowerPoint, UVC video, and test
  patterns in a playlist; check and adjust items in EDIT before ON AIR.
- Send items to ON AIR with cut or crossfade transitions, DSK, and fade to
  black. Use PFL, BGM, the return monitor, and Roland V-8HD control.
- Save and restore playlists and settings. The installer and ZIP editions are
  available from the [official Windows download page](https://pondashi.com/vtr-pon/download/windows/).

Native PowerPoint audio plays directly through Windows and is outside the
VTR-PON ITEM, MASTER, MUTE, and FTB controls. Virtual camera output carries
video only. Check both paths with your actual equipment before use. VTR-PON2
settings and playlists cannot be migrated. Alongside this first beta, ATEM,
Network Video (NDI®), VST3, and DeckLink **0.3.0** packages are distributed
separately for VTR-PON 3.0.0-beta.1. They are not bundled with the application.
See the [VTR-PON3 add-on guide](https://pondashi.com/vtr-pon/addon/), check each
package's requirements and SHA-256, and install it through **Settings → Add-ons**.
Read and accept its conditions and notices, then exit and restart VTR-PON as
directed. DeckLink use also requires the official
[Blackmagic Desktop Video](https://www.blackmagicdesign.com/developer/products/capture-and-playback/sdk-and-software)
Windows x64 drivers and API runtime (API 16.0 or newer for the current add-on).
The DeckLink SDK is needed only for add-on development.

See the [installation guide](INSTALL.ja.md) and [report issues](https://github.com/pondashicom/vtr-pon/issues).
