#include "language_text.h"

#include <QtTranslation>

#include <array>

namespace {

constexpr auto kUiTextIds = std::to_array<const char *>({
    //% "Select output format"
    QT_TRID_NOOP("decklink.select_format"),
    //% "Assign each enabled output to a different hardware channel. Fill and Key use the same video format and source frame. FTB fades them to transparency. Audio is sent to Composite and Fill; Key is silent. Synchronized playback uses a common locked reference when required by the device."
    QT_TRID_NOOP("decklink.fill_key_help"),
    //% "Fill/Key: synchronized playback."
    QT_TRID_NOOP("decklink.pair_synchronized"),
    //% "Fill/Key: independent playback. Hardware timing is not synchronized."
    QT_TRID_NOOP("decklink.pair_unsynchronized"),
    //% "DeckLink"
    QT_TRID_NOOP("decklink.title"),
    //% "Choose the output channel and format for Composite, Fill and Key, then enable each output. DeckLink input channels appear in the INPUT dropdown. Input format changes apply to newly added items."
    QT_TRID_NOOP("decklink.description"),
    //% "Enable DeckLink input"
    QT_TRID_NOOP("decklink.enable_input"),
    //% "Enable DeckLink output"
    QT_TRID_NOOP("decklink.enable_output"),
    //% "Output channel"
    QT_TRID_NOOP("decklink.output_channel"),
    //% "Output format"
    QT_TRID_NOOP("decklink.output_mode"),
    //% "Input format"
    QT_TRID_NOOP("decklink.input_mode"),
    //% "Auto (device detection)"
    QT_TRID_NOOP("decklink.auto_mode"),
    //% "Select output channel"
    QT_TRID_NOOP("decklink.select_channel"),
    //% "Saved selection (currently unavailable)"
    QT_TRID_NOOP("decklink.saved_unavailable"),
    //% "DeckLink output is running."
    QT_TRID_NOOP("decklink.output_running"),
    //% "No DeckLink devices found. Connect the device and check Desktop Video Setup."
    QT_TRID_NOOP("decklink.no_devices"),
    //% "DeckLink devices are available."
    QT_TRID_NOOP("decklink.ready"),
    //% "A DeckLink input error occurred."
    QT_TRID_NOOP("decklink.input_error"),
    //% "The selected DeckLink input channel is unavailable."
    QT_TRID_NOOP("decklink.selected_unavailable"),
    //% "Could not start DeckLink input."
    QT_TRID_NOOP("decklink.start_failed"),
    //% "Could not add DeckLink input to the playlist."
    QT_TRID_NOOP("decklink.add_failed"),
    //% "DeckLink Live"
    QT_TRID_NOOP("decklink.live"),
    //% "UVC / NDI / DeckLink Input"
    QT_TRID_NOOP("decklink.input_title"),
    //% "Select an input device or channel"
    QT_TRID_NOOP("decklink.input_devices"),
    //% "Add selected input to playlist"
    QT_TRID_NOOP("decklink.input_add"),
    //% "Updates and notices"
    QT_TRID_NOOP("settings.updates.title"),
    //% "Choose when VTR-PON checks for updates and whether to send a usage notice."
    QT_TRID_NOOP("settings.updates.description"),
    //% "Update checks"
    QT_TRID_NOOP("settings.updates.check_title"),
    //% "Check for updates at startup"
    QT_TRID_NOOP("settings.updates.check_automatic"),
    //% "When enabled, VTR-PON checks for updates when it starts. You can still check manually from Help when this is off."
    QT_TRID_NOOP("settings.updates.check_description"),
    //% "Usage notice"
    QT_TRID_NOOP("settings.privacy.title"),
    //% "Send a usage notice"
    QT_TRID_NOOP("settings.privacy.activity_consent"),
    //% "When enabled, VTR-PON sends its version to pondashi.com at most once per UTC day during automatic or manual update checks. No install ID or media information is sent. The activity database keeps only daily counts by version; the web host may keep standard access logs. You can turn this off at any time."
    QT_TRID_NOOP("settings.privacy.activity_description"),
    //% "Still Capture"
    QT_TRID_NOOP("input.still.title"),
    //% "EDIT Capture"
    QT_TRID_NOOP("input.still.preview"),
    //% "ON AIR Capture"
    QT_TRID_NOOP("input.still.program"),
    //% "Dur"
    QT_TRID_NOOP("input.still.duration_label"),
    //% "Still capture duration (seconds)"
    QT_TRID_NOOP("input.still.duration"),
    //% "No video frame is available to capture."
    QT_TRID_NOOP("input.still.no_frame"),
    //% "Could not save the still capture. Check the save directory."
    QT_TRID_NOOP("input.still.save_failed"),
    //% "The PNG was saved, but could not be added to the original playlist."
    QT_TRID_NOOP("input.still.add_failed"),
    //% "Still capture added to the playlist."
    QT_TRID_NOOP("input.still.saved"),
    //% "File Management"
    QT_TRID_NOOP("settings.files.title"),
    //% "Directory for saving still captures"
    QT_TRID_NOOP("settings.files.still"),
    //% "Directory for files converted when importing PPTX/PDF"
    QT_TRID_NOOP("settings.files.documents"),
    //% "Leave blank to use the default directory shown in the field. Converted files are placed in PowerPoint and PDF subdirectories. Changes apply to new captures and conversions; existing files are not moved."
    QT_TRID_NOOP("settings.files.description"),
    //% "Browse…"
    QT_TRID_NOOP("settings.files.browse"),
    //% "Enter an absolute directory path, or leave blank for the default."
    QT_TRID_NOOP("settings.files.absolute_path"),
    //% "Could not save the directory setting."
    QT_TRID_NOOP("settings.files.save_failed"),
    //% "Directory setting saved."
    QT_TRID_NOOP("settings.files.saved"),

    //% "Add-ons"
    QT_TRID_NOOP("settings.addons.title"),
    //% "Install and remove add-ons for this VTR-PON copy. Changes are applied after VTR-PON exits; your feature settings are kept. Vendor runtimes are installed separately."
    QT_TRID_NOOP("settings.addons.description"),
    //% "ATEM switcher control"
    QT_TRID_NOOP("settings.addons.atem"),
    //% "NDI video and audio input/output"
    QT_TRID_NOOP("settings.addons.ndi"),
    //% "VST3 audio effects"
    QT_TRID_NOOP("settings.addons.vst3"),
    //% "Check available add-ons"
    QT_TRID_NOOP("settings.addons.refresh"),
    //% "Checking the catalog…"
    QT_TRID_NOOP("settings.addons.checking"),
    //% "Available add-ons are up to date."
    QT_TRID_NOOP("settings.addons.catalog_ready"),
    //% "The catalog is unavailable. You can still uninstall add-ons or install a local package."
    QT_TRID_NOOP("settings.addons.catalog_unavailable"),
    //% "Not installed"
    QT_TRID_NOOP("settings.addons.not_installed"),
    //% "Installed version %1"
    QT_TRID_NOOP("settings.addons.installed"),
    //% "Available version %1"
    QT_TRID_NOOP("settings.addons.available"),
    //% "Manually installed files are present. The version is unknown; only known add-on files can be removed."
    QT_TRID_NOOP("settings.addons.manual"),
    //% "Waiting for VTR-PON to exit"
    QT_TRID_NOOP("settings.addons.waiting"),
    //% "Install"
    QT_TRID_NOOP("settings.addons.install"),
    //% "Update"
    QT_TRID_NOOP("settings.addons.update"),
    //% "Uninstall"
    QT_TRID_NOOP("settings.addons.uninstall"),
    //% "Uninstall %1 after VTR-PON exits? Its feature settings will stay."
    QT_TRID_NOOP("settings.addons.confirm_uninstall"),
    //% "Remove the manually installed %1 add-on after VTR-PON exits? Only known add-on files will be removed. Other files and feature settings will stay."
    QT_TRID_NOOP("settings.addons.confirm_manual_uninstall"),
    //% "Install from package…"
    QT_TRID_NOOP("settings.addons.local"),
    //% "Downloading %1…"
    QT_TRID_NOOP("settings.addons.downloading"),
    //% "The add-on operation is ready and will take effect after restarting VTR-PON."
    QT_TRID_NOOP("settings.addons.pending"),
    //% "Restart VTR-PON"
    QT_TRID_NOOP("settings.addons.restart_title"),
    //% "VTR-PON must restart to enable the add-on.\nRestart now?"
    QT_TRID_NOOP("settings.addons.restart_question"),
    //% "Could not complete the add-on restart: %1"
    QT_TRID_NOOP("settings.addons.restart_failed"),
    //% "Could not prepare the add-on operation: %1"
    QT_TRID_NOOP("settings.addons.operation_failed"),
    //% "Last operation failed: %1"
    QT_TRID_NOOP("settings.addons.last_error"),
    //% "An add-on operation is still pending or needs recovery. Wait for it to finish before starting VTR-PON. Manager data: %1"
    QT_TRID_NOOP("settings.addons.startup_pending"),
    //% "VTR-PON will start automatically after the add-on operation finishes. Close this notice and wait."
    QT_TRID_NOOP("settings.addons.startup_resuming"),
    //% "Add-on license agreement"
    QT_TRID_NOOP("settings.addons.agreement_title"),
    //% "Review the documents for %1, version %2. Installation is prepared only after you agree to the agreement documents."
    QT_TRID_NOOP("settings.addons.agreement_intro"),
    //% "Agreement: "
    QT_TRID_NOOP("settings.addons.agreement_tab"),
    //% "Notice: "
    QT_TRID_NOOP("settings.addons.notice_tab"),
    //% "I have read and agree to the agreement documents shown above."
    QT_TRID_NOOP("settings.addons.agreement_check"),
    //% "Agree and install"
    QT_TRID_NOOP("settings.addons.agreement_accept"),
    //% "This package has no supported agreement document. Ask for a new package."
    QT_TRID_NOOP("settings.addons.agreement_missing"),

    //% "Use VST plug-ins"
    QT_TRID_NOOP("vst.use"),
    //% "VST3 add-on installed. The EQ buttons become VST buttons."
    QT_TRID_NOOP("vst.addon_ready"),
    //% "The VST3 add-on is not installed. Install it in audio-addons."
    QT_TRID_NOOP("vst.addon_missing"),
    //% "Select a plug-in"
    QT_TRID_NOOP("vst.select"),
    //% "VST3 plug-in"
    QT_TRID_NOOP("vst.plugin"),
    //% "Bypass"
    QT_TRID_NOOP("vst.bypass"),
    //% "Scanning..."
    QT_TRID_NOOP("vst.scanning"),
    //% "Rescan"
    QT_TRID_NOOP("vst.rescan"),
    //% "Cancel scan"
    QT_TRID_NOOP("vst.scan_cancel"),
    //% "Searching VST3 folders..."
    QT_TRID_NOOP("vst.scan_discovering"),
    //% "Scanning %1 / %2: %3"
    QT_TRID_NOOP("vst.scan_progress"),
    //% "%1 plug-ins found. %2 files could not be scanned."
    QT_TRID_NOOP("vst.scan_complete"),
    //% "Scan canceled. Plug-ins already found can still be selected."
    QT_TRID_NOOP("vst.scan_canceled"),
    //% "Could not scan VST3 plug-ins: %1"
    QT_TRID_NOOP("vst.scan_failed"),
    //% "Inactive — press VST to enable the selected plug-in."
    QT_TRID_NOOP("vst.inactive"),
    //% "Active"
    QT_TRID_NOOP("vst.active"),
    //% "Bypassed — original audio is passing through."
    QT_TRID_NOOP("vst.bypassed"),
    //% "Loading plug-in... Original audio is passing through."
    QT_TRID_NOOP("vst.loading"),
    //% "VST stopped. Playback continues with original audio. Press VST to retry."
    QT_TRID_NOOP("vst.failed"),
    //% "Stereo audio effects, 48 kHz. Plug-in latency: %1 samples. Selection is saved; plug-in parameters last for this session."
    QT_TRID_NOOP("vst.hint"),
    //% "%1 VST stopped because of a plug-in error. Playback continues with original audio."
    QT_TRID_NOOP("vst.failure_notice"),
    //% "Checking %1..."
    QT_TRID_NOOP("addons.runtime.checking"),
    //% "%1 is available."
    QT_TRID_NOOP("addons.runtime.ready"),
    //% "The add-on is installed, but %1 was not found. Install it from the official download and restart VTR-PON."
    QT_TRID_NOOP("addons.runtime.missing"),
    //% "%1 could not be loaded. Check the supported version and its dependencies, then restart VTR-PON."
    QT_TRID_NOOP("addons.runtime.incompatible"),
    //% "Could not check %1. See the details or try again."
    QT_TRID_NOOP("addons.runtime.error"),
    //% "Checking %1 timed out. You can continue using VTR-PON and try again."
    QT_TRID_NOOP("addons.runtime.timeout"),
    //% "The add-on for %1 is not installed."
    QT_TRID_NOOP("addons.runtime.addon_missing"),
    //% "The add-on for %1 could not be loaded. Check the add-on version and installation."
    QT_TRID_NOOP("addons.runtime.addon_error"),
    //% "Runtime diagnostics for %1 are unavailable. Check that VTR-PON and the add-on are up to date and correctly installed."
    QT_TRID_NOOP("addons.runtime.check_unavailable"),
    //% "Open official download"
    QT_TRID_NOOP("addons.runtime.download"),
    //% "Check again"
    QT_TRID_NOOP("addons.runtime.retry"),
    //% "Could not open the browser. Open this URL manually:"
    QT_TRID_NOOP("addons.runtime.browser_failed"),
    //% "Select the ON AIR outputs to send over NDI. Enable input to find sources on the LAN."
    QT_TRID_NOOP("network_video.description"),
    //% "Output video"
    QT_TRID_NOOP("network_video.outputs"),
    //% "Composite — video with background"
    QT_TRID_NOOP("network_video.composite"),
    //% "Fill — color for external compositing"
    QT_TRID_NOOP("network_video.fill"),
    //% "Key — grayscale alpha"
    QT_TRID_NOOP("network_video.key"),
    //% "Alpha — video with transparency"
    QT_TRID_NOOP("network_video.alpha"),
    //% "Advanced settings"
    QT_TRID_NOOP("network_video.details"),
    //% "Fill format"
    QT_TRID_NOOP("network_video.fill_format"),
    //% "FTB fades Fill/Key and Alpha outputs to transparency. Only Composite includes the player background. Audio is included in every output except Key; select one audio source at the receiver."
    QT_TRID_NOOP("network_video.alpha_help"),
    //% "No output video is selected."
    QT_TRID_NOOP("network_video.no_outputs"),
    //% "Error:"
    QT_TRID_NOOP("network_video.output_error"),
    //% "Enable NDI output"
    QT_TRID_NOOP("network_video.enable_output"),
    //% "Enable NDI input"
    QT_TRID_NOOP("network_video.enable_input"),
    //% "Source name"
    QT_TRID_NOOP("network_video.source_name"),
    //% "NDI output is running."
    QT_TRID_NOOP("network_video.output_running"),
    //% "Starting NDI output..."
    QT_TRID_NOOP("network_video.output_starting"),
    //% "NDI output is disabled."
    QT_TRID_NOOP("network_video.output_disabled"),
    //% "NDI sources found: %1"
    QT_TRID_NOOP("network_video.sources_found"),
    //% "NDI input error."
    QT_TRID_NOOP("network_video.input_error"),
    //% "The selected NDI source is currently unavailable."
    QT_TRID_NOOP("network_video.selected_unavailable"),
    //% "Could not start the NDI input."
    QT_TRID_NOOP("network_video.start_failed"),
    //% "Could not add the NDI input to the playlist."
    QT_TRID_NOOP("network_video.add_failed"),
    //% "UVC/NDI INPUT"
    QT_TRID_NOOP("input.network.title"),
    //% "Available UVC devices and NDI sources"
    QT_TRID_NOOP("input.network.devices_accessible"),
    //% "Add the selected UVC device or NDI source to the playlist"
    QT_TRID_NOOP("input.network.add"),
    //% "NDI LIVE"
    QT_TRID_NOOP("playlist.item.network_live"),
    //% "Nominal audio delay"
    QT_TRID_NOOP("audio.delay.nominal"),
    //% "On-air item audio delay"
    QT_TRID_NOOP("audio.delay.item"),
    //% "Master audio delay"
    QT_TRID_NOOP("audio.delay.master"),
    //% "0 = OFF; 1-6 frames at 30 fps (33.33-200 ms). Changing the value clears buffered audio. Stop/seek discards the delayed tail."
    QT_TRID_NOOP("audio.delay.tooltip"),
    //% "Virtual Camera"
    QT_TRID_NOOP("virtual_camera.title"),
    //% "Enable virtual camera output"
    QT_TRID_NOOP("virtual_camera.enable"),
    //% "1920 x 1080 / 30 fps. Sends ON AIR while enabled, including OFF AIR. Shows the standby text only while disabled. Install the camera DLL once before use."
    QT_TRID_NOOP("virtual_camera.description"),
    //% "Could not save the virtual camera setting. Check settings file write permissions."
    QT_TRID_NOOP("virtual_camera.save_failed"),
    //% "Nominal Item EQ"
    QT_TRID_NOOP("edit.eq.title"),
    //% "On-air Item EQ"
    QT_TRID_NOOP("item.eq.title"),
    //% "Adjusted"
    QT_TRID_NOOP("item.eq.adjusted"),
    //% "Flat (0 dB)"
    QT_TRID_NOOP("item.eq.flat"),
    //% "Master EQ"
    QT_TRID_NOOP("program.eq.title"),
    //% "Master EQ: flat (all bands 0 dB)"
    QT_TRID_NOOP("program.eq.flat"),
    //% "Master EQ: adjusted"
    QT_TRID_NOOP("program.eq.adjusted"),
    //% "Reset to flat"
    QT_TRID_NOOP("program.eq.reset"),
    //% "%1: increase"
    QT_TRID_NOOP("operator_timer.increase"),
    //% "%1: decrease"
    QT_TRID_NOOP("operator_timer.decrease"),
    //% "Return Timer Settings"
    QT_TRID_NOOP("operator_timer.settings"),
    //% "Timer Settings"
    QT_TRID_NOOP("operator_timer.open_settings"),
    //% "Enable countdown timer"
    QT_TRID_NOOP("operator_timer.enable"),
    //% "Duration"
    QT_TRID_NOOP("operator_timer.duration"),
    //% "Set a duration of at least one second."
    QT_TRID_NOOP("operator_timer.duration_positive"),
    //% "First warning (remaining)"
    QT_TRID_NOOP("operator_timer.warning1"),
    //% "Second warning (remaining)"
    QT_TRID_NOOP("operator_timer.warning2"),
    //% " min"
    QT_TRID_NOOP("operator_timer.minutes"),
    //% " sec"
    QT_TRID_NOOP("operator_timer.seconds"),
    //% "Color"
    QT_TRID_NOOP("operator_timer.color"),
    //% "Save settings"
    QT_TRID_NOOP("operator_timer.save"),
    //% "Timer settings saved."
    QT_TRID_NOOP("operator_timer.saved"),
    //% "Set the second warning at or below the first warning time."
    QT_TRID_NOOP("operator_timer.warning_order"),
    //% "Return Timer"
    QT_TRID_NOOP("operator_timer.title"),
    //% "Start"
    QT_TRID_NOOP("operator_timer.start"),
    //% "Reset"
    QT_TRID_NOOP("operator_timer.reset"),
    //% "Roland V-8HD"
    QT_TRID_NOOP("settings.group.roland"),
    //% "Blackmagic ATEM"
    QT_TRID_NOOP("settings.group.atem"),
    //% "NDI"
    QT_TRID_NOOP("settings.group.ndi"),
    //% "VST"
    QT_TRID_NOOP("settings.group.vst"),
    //% "Return Monitor"
    QT_TRID_NOOP("operator_monitor.settings"),
    //% "Return monitor display"
    QT_TRID_NOOP("operator_monitor.display"),
    //% "Show information"
    QT_TRID_NOOP("operator_monitor.information"),
    //% "Remaining time"
    QT_TRID_NOOP("operator_monitor.remaining"),
    //% "File name"
    QT_TRID_NOOP("operator_monitor.file"),
    //% "Start mode / End mode"
    QT_TRID_NOOP("operator_monitor.modes"),
    //% "Playback speed"
    QT_TRID_NOOP("operator_monitor.rate"),
    //% "Pages (total / current)"
    QT_TRID_NOOP("operator_monitor.pages"),
    //% "Message"
    QT_TRID_NOOP("operator_monitor.cue"),
    //% "Send"
    QT_TRID_NOOP("operator_monitor.send"),
    //% "Delete"
    QT_TRID_NOOP("operator_monitor.clear"),
    //% "Window"
    QT_TRID_NOOP("operator_monitor.window"),
    //% "Disconnected: %1"
    QT_TRID_NOOP("operator_monitor.missing_display"),
    //% "Return Monitor"
    QT_TRID_NOOP("menu.view.operator_monitor"),
    //% "The return monitor is unavailable. ON AIR continues independently."
    QT_TRID_NOOP("operator_monitor.unavailable"),
    //% "Time"
    QT_TRID_NOOP("settings.time.title"),
    //% "Language & Time"
    QT_TRID_NOOP("settings.category.time_language"),
    //% "Choose the language used by VTR-PON and configure time displays. Changes are saved automatically."
    QT_TRID_NOOP("settings.time_language.description"),
    //% "Configure the current time display and default remaining timer. Changes are saved automatically."
    QT_TRID_NOOP("settings.time.description"),
    //% "Time zone"
    QT_TRID_NOOP("settings.time.zone"),
    //% "System time zone"
    QT_TRID_NOOP("settings.time.system"),
    //% "Synchronize time"
    QT_TRID_NOOP("settings.time.sync"),
    //% "Default remaining timer"
    QT_TRID_NOOP("settings.time.remaining"),
    //% "Single item"
    QT_TRID_NOOP("settings.time.item"),
    //% "Total"
    QT_TRID_NOOP("settings.time.total"),
    //% "Current time font"
    QT_TRID_NOOP("settings.time.font"),
    //% "Current time font size"
    QT_TRID_NOOP("settings.time.size"),
    //% "Synchronizing time..."
    QT_TRID_NOOP("settings.time.syncing"),
    //% "Could not save time settings."
    QT_TRID_NOOP("settings.time.save_failed"),

    //% "Rendering GPU"
    QT_TRID_NOOP("settings.gpu.device"),
    //% "Automatic (ON AIR display GPU)"
    QT_TRID_NOOP("settings.gpu.auto"),
    //% "Saved GPU is unavailable (automatic fallback)"
    QT_TRID_NOOP("settings.gpu.missing_choice"),
    //% "Use software video decoding"
    QT_TRID_NOOP("settings.gpu.software"),
    //% "Current GPU: %1\nVideo decoding: %2"
    QT_TRID_NOOP("settings.gpu.current"),
    //% "Software (GPU rendering remains enabled)"
    QT_TRID_NOOP("settings.gpu.mode_software"),
    //% "D3D11 on the selected GPU; software fallback for unsupported codecs"
    QT_TRID_NOOP("settings.gpu.mode_hardware"),
    //% "GPU output has not started."
    QT_TRID_NOOP("settings.gpu.not_started"),
    //% "The saved GPU was unavailable at startup. The ON AIR display GPU is in use."
    QT_TRID_NOOP("settings.gpu.fallback"),
    //% "Changes are saved immediately and take effect after restarting VTR-PON. Playback is not interrupted. Automatic follows the ON AIR display at startup."
    QT_TRID_NOOP("settings.gpu.restart_note"),
    //% "Saved. Restart VTR-PON to apply the GPU and decoding settings. Current playback keeps its existing settings."
    QT_TRID_NOOP("settings.gpu.restart_pending"),
    //% "Could not save the GPU settings."
    QT_TRID_NOOP("settings.gpu.save_failed"),
    //% "GPU selection is available on Windows."
    QT_TRID_NOOP("settings.gpu.windows_only"),
    //% "Add Files..."
    QT_TRID_NOOP("test_patterns.add"),
    //% "Remove from List"
    QT_TRID_NOOP("test_patterns.remove"),
    //% "Move Up"
    QT_TRID_NOOP("test_patterns.up"),
    //% "Move Down"
    QT_TRID_NOOP("test_patterns.down"),
    //% "Restore Bundled Items"
    QT_TRID_NOOP("test_patterns.restore"),
    //% "File names appear in the Tools menu in this order. Changes are saved immediately. Files are registered by path; keep them in place. Removing an entry does not delete its file or existing playlist items."
    QT_TRID_NOOP("test_patterns.description"),
    //% "Could not save the test pattern list."
    QT_TRID_NOOP("test_patterns.save_failed"),
    //% "Could not register: %1"
    QT_TRID_NOOP("test_patterns.rejected"),
    //% "No test patterns registered"
    QT_TRID_NOOP("test_patterns.empty"),
    //% "Media (*.mp4 *.mov *.mkv *.webm *.avi *.mxf *.m4v *.mpg *.mpeg *.ts *.png *.jpg *.jpeg *.wav *.mp3 *.flac *.aac *.m4a *.ogg *.aif *.aiff)"
    QT_TRID_NOOP("test_patterns.filter"),
    //% "Test Pattern"
    QT_TRID_NOOP("menu.tools.test_pattern"),
    //% "SMPTE HD Color Bars"
    QT_TRID_NOOP("test_pattern.smpte_hd"),
    //% "On Air"
    QT_TRID_NOOP("menu.on_air"),
    //% "On Air"
    QT_TRID_NOOP("menu.on_air.TakeOnAir"),
    //% "Off Air"
    QT_TRID_NOOP("menu.on_air.OffAir"),
    //% "FTB"
    QT_TRID_NOOP("menu.on_air.ToggleFtb"),
    //% "Play / Pause"
    QT_TRID_NOOP("menu.on_air.ToggleOnAirPlayback"),
    //% "Main Audio Fade In"
    QT_TRID_NOOP("menu.on_air.MasterFadeIn"),
    //% "Main Audio Fade Out"
    QT_TRID_NOOP("menu.on_air.MasterFadeOut"),
    //% "ITEM Audio Fade In"
    QT_TRID_NOOP("menu.on_air.ItemFadeIn"),
    //% "ITEM Audio Fade Out"
    QT_TRID_NOOP("menu.on_air.ItemFadeOut"),
    //% "BGM Audio Fade In"
    QT_TRID_NOOP("menu.on_air.BgmFadeIn"),
    //% "BGM Audio Fade Out"
    QT_TRID_NOOP("menu.on_air.BgmFadeOut"),
    //% "Toggle DSK"
    QT_TRID_NOOP("menu.on_air.ToggleDsk"),
    //% "Edit"
    QT_TRID_NOOP("menu.edit"),
    //% "Toggle Start Mode: PAUSE / PLAY"
    QT_TRID_NOOP("settings.shortcuts.EditToggleStartMode"),
    //% "End Mode: Stop"
    QT_TRID_NOOP("settings.shortcuts.EditEndStop"),
    //% "End Mode: Pause"
    QT_TRID_NOOP("settings.shortcuts.EditEndPause"),
    //% "End Mode: Repeat"
    QT_TRID_NOOP("settings.shortcuts.EditEndRepeat"),
    //% "End Mode: Next"
    QT_TRID_NOOP("settings.shortcuts.EditEndNext"),
    //% "End Mode: Toggle Fade Out"
    QT_TRID_NOOP("settings.shortcuts.EditToggleFadeOut"),
    //% "Reset Edit Area"
    QT_TRID_NOOP("settings.shortcuts.EditReset"),
    //% "Set IN Point"
    QT_TRID_NOOP("settings.shortcuts.EditSetIn"),
    //% "Set OUT Point"
    QT_TRID_NOOP("settings.shortcuts.EditSetOut"),
    QT_TRID_NOOP("settings.shortcuts.NextOutputDisplay"),
    //% "EDIT Capture"
    QT_TRID_NOOP("settings.shortcuts.PreviewCapture"),
    //% "ON AIR Capture"
    QT_TRID_NOOP("settings.shortcuts.ProgramCapture"),
    //% "EDIT: Toggle PFL"
    QT_TRID_NOOP("settings.shortcuts.TogglePfl"),
    //% "Playlist: Delete Playlist"
    QT_TRID_NOOP("settings.shortcuts.PlaylistDelete"),
    //% "Operator Timer: Start"
    QT_TRID_NOOP("settings.shortcuts.OperatorTimerStart"),
    //% "Operator Timer: Pause"
    QT_TRID_NOOP("settings.shortcuts.OperatorTimerPause"),
    //% "Operator Timer: Reset"
    QT_TRID_NOOP("settings.shortcuts.OperatorTimerReset"),
    //% "Operator Timer: Open Settings"
    QT_TRID_NOOP("settings.shortcuts.OperatorTimerSettings"),
    //% "EDIT: Open Nominal EQ"
    QT_TRID_NOOP("settings.shortcuts.OpenNominalEq"),
    //% "ON AIR: Open Item EQ"
    QT_TRID_NOOP("settings.shortcuts.OpenItemEq"),
    //% "ON AIR: Open Master EQ"
    QT_TRID_NOOP("settings.shortcuts.OpenMasterEq"),
    //% "Press a key"
    QT_TRID_NOOP("settings.shortcuts.placeholder"),
    //% "Shortcut Keys"
    QT_TRID_NOOP("settings.category.shortcuts"),
    //% "Assign up to two keys per action. Clear both to disable. Save to apply. Shortcuts are suppressed during text/IME input. Ctrl+Alt+S and Ctrl/Shift+Up/Down are reserved."
    QT_TRID_NOOP("settings.shortcuts.description"),
    //% "Action"
    QT_TRID_NOOP("settings.shortcuts.action"),
    //% "Key 1"
    QT_TRID_NOOP("settings.shortcuts.key1"),
    //% "Key 2"
    QT_TRID_NOOP("settings.shortcuts.key2"),
    //% "Save and Apply"
    QT_TRID_NOOP("settings.shortcuts.save"),
    //% "Restore Defaults"
    QT_TRID_NOOP("settings.shortcuts.defaults"),
    //% "Defaults loaded. Save to apply."
    QT_TRID_NOOP("settings.shortcuts.pending"),
    //% "Duplicate, reserved or invalid key. Not saved: %1"
    QT_TRID_NOOP("settings.shortcuts.conflict"),
    //% "Shortcuts saved and applied."
    QT_TRID_NOOP("settings.shortcuts.saved"),
    //% "Add Files"
    QT_TRID_NOOP("settings.shortcuts.AddFile"),
    //% "Exit"
    QT_TRID_NOOP("settings.shortcuts.Exit"),
    //% "ON AIR selected item"
    QT_TRID_NOOP("settings.shortcuts.TakeOnAir"),
    //% "ON AIR Play / Pause"
    QT_TRID_NOOP("settings.shortcuts.ToggleOnAirPlayback"),
    //% "Load selected item in EDIT"
    QT_TRID_NOOP("settings.shortcuts.LoadEdit"),
    //% "Copy Start / End Mode"
    QT_TRID_NOOP("settings.shortcuts.CopyItemState"),
    //% "Paste Start / End Mode"
    QT_TRID_NOOP("settings.shortcuts.PasteItemState"),
    //% "Document Next Page"
    QT_TRID_NOOP("settings.shortcuts.DocumentNext"),
    //% "Document Previous Page"
    QT_TRID_NOOP("settings.shortcuts.DocumentPrevious"),
    //% "Toggle FTB"
    QT_TRID_NOOP("settings.shortcuts.ToggleFtb"),
    //% "Toggle DSK"
    QT_TRID_NOOP("settings.shortcuts.ToggleDsk"),
    //% "OFF AIR"
    QT_TRID_NOOP("settings.shortcuts.OffAir"),
    //% "MASTER Fade In"
    QT_TRID_NOOP("settings.shortcuts.MasterFadeIn"),
    //% "MASTER Fade Out"
    QT_TRID_NOOP("settings.shortcuts.MasterFadeOut"),
    //% "ITEM Fade In"
    QT_TRID_NOOP("settings.shortcuts.ItemFadeIn"),
    //% "ITEM Fade Out"
    QT_TRID_NOOP("settings.shortcuts.ItemFadeOut"),
    //% "BGM Fade In"
    QT_TRID_NOOP("settings.shortcuts.BgmFadeIn"),
    //% "BGM Fade Out"
    QT_TRID_NOOP("settings.shortcuts.BgmFadeOut"),
    //% "Playlist: Play List Once"
    QT_TRID_NOOP("settings.shortcuts.PlaylistPlayOnce"),
    //% "Playlist: Repeat All"
    QT_TRID_NOOP("settings.shortcuts.PlaylistRepeatAll"),
    //% "Playlist: Shuffle"
    QT_TRID_NOOP("settings.shortcuts.PlaylistShuffle"),
    //% "Playlist: Set BGM Mode"
    QT_TRID_NOOP("settings.shortcuts.PlaylistBgmMode"),
    //% "Playlist: Clear BGM Mode"
    QT_TRID_NOOP("settings.shortcuts.PlaylistStandardMode"),
    //% "Playlist: Clear Playlist"
    QT_TRID_NOOP("settings.shortcuts.PlaylistClear"),
    //% "Playlist: Undo Bulk Operation"
    QT_TRID_NOOP("settings.shortcuts.PlaylistUndo"),
    //% "Select item %1"
    QT_TRID_NOOP("settings.shortcuts.SelectItem"),
    //% "Activate playlist %1"
    QT_TRID_NOOP("settings.shortcuts.ActivatePlaylist"),
    //% "Updated about once per second. PC: system-wide. VTR-PON: main process only, excluding helper processes. CPU: percentage of total CPU capacity. GPU: busiest engine for each scope. RAM: physical memory usage / total RAM. VRAM: local dedicated memory usage / total dedicated capacity across GPUs. PC warnings turn red at 90% and clear below 85%. --%: unavailable."
    QT_TRID_NOOP("system_load.description"),
    //% "Fullscreen %1 FPS %2"
    QT_TRID_NOOP("system_load.fullscreen"),
    //% "Updated about once per second. FPS: frames submitted to fullscreen output, including repeated frames. Resolution: output video dimensions in pixels. --: unavailable or waiting for measurement."
    QT_TRID_NOOP("system_load.fullscreen_description"),
    //% "PC"
    QT_TRID_NOOP("system_load.pc"),
    //% "PC High load"
    QT_TRID_NOOP("system_load.pc_warning"),
    //% "Playlist"
    QT_TRID_NOOP("menu.playlist"),
    //% "Tools"
    QT_TRID_NOOP("menu.tools"),
    //% "Clock Sync"
    QT_TRID_NOOP("clock.sync.action"),
    //% "Clock Sync (Synchronizing...)"
    QT_TRID_NOOP("clock.sync.in_progress"),
    //% "App clock synchronized."
    QT_TRID_NOOP("clock.sync.succeeded"),
    //% "Clock sync failed. Keeping the last synchronized time."
    QT_TRID_NOOP("clock.sync.failed_keep_time"),
    //% "Clock sync failed. Using the local computer time."
    QT_TRID_NOOP("clock.sync.failed_local_time"),
    //% "Show Log"
    QT_TRID_NOOP("logs.show"),
    //% "Debug Mode"
    QT_TRID_NOOP("logs.debug"),
    //% "Search logs"
    QT_TRID_NOOP("logs.search"),
    //% "All"
    QT_TRID_NOOP("logs.all"),
    //% "Warnings / Errors"
    QT_TRID_NOOP("logs.warnings"),
    //% "Live update"
    QT_TRID_NOOP("logs.follow"),
    //% "Save Displayed Log"
    QT_TRID_NOOP("logs.save"),
    //% "Open Log Folder"
    QT_TRID_NOOP("logs.folder"),
    //% "Latest 2,000 records shown. Automatic logs: %1 (10 MB × 10 files)."
    QT_TRID_NOOP("logs.retention"),
    //% "Log save/open failed: %1"
    QT_TRID_NOOP("logs.failed"),
    //% "Saved: %1"
    QT_TRID_NOOP("logs.saved"),

    //% "Use this color for FTB / fade-out"
    //% "Fade to the selected background color on FTB / fade-out"
    //% "Choose Color"
    QT_TRID_NOOP("settings.background.picker"),
    //% "Other Color…"
    QT_TRID_NOOP("settings.background.other"),
    //% "White"
    QT_TRID_NOOP("settings.background.white"),
    //% "Blue"
    QT_TRID_NOOP("settings.background.blue"),
    //% "Digital Green"
    QT_TRID_NOOP("settings.background.green"),
    //% "Black"
    QT_TRID_NOOP("settings.background.black"),
    //% "Background Color"
    QT_TRID_NOOP("settings.background.color"),
    //% "Choose the background color for ON AIR and its monitor. Changes are applied immediately."
    QT_TRID_NOOP("settings.background.description"),
    //% "Player Background Color"
    QT_TRID_NOOP("settings.category.player_background"),
    //% "Video Output"
    QT_TRID_NOOP("settings.category.video_output"),
    //% "Control the ON AIR and Controller windows and fullscreen output resolution. Always on Top requires separate displays for ON AIR and Controller."
    QT_TRID_NOOP("settings.video_output.description"),
    //% "Maximum output resolution"
    QT_TRID_NOOP("settings.video_output.maximum_resolution"),
    //% "Changes are saved immediately and apply after restarting VTR-PON. For 4K fullscreen, also set the output display to 3840 x 2160 in Windows. ON AIR monitor, virtual camera and return monitor resolutions stay unchanged."
    QT_TRID_NOOP("settings.video_output.resolution_note"),
    //% "Current output: %1"
    QT_TRID_NOOP("settings.video_output.resolution_current"),
    //% "Current output: %1. Restart VTR-PON to apply the saved resolution."
    QT_TRID_NOOP("settings.video_output.resolution_pending"),
    //% "Could not save the output resolution."
    QT_TRID_NOOP("settings.video_output.resolution_save_failed"),

    //% "Save / Reset"
    QT_TRID_NOOP("settings.category.save_reset"),
    //% "Export Settings to File"
    QT_TRID_NOOP("settings.export.button"),
    //% "Restore Settings from File"
    QT_TRID_NOOP("settings.import.button"),

    //% "Export Settings"
    QT_TRID_NOOP("menu.file.export_settings"),
    //% "Import Settings"
    QT_TRID_NOOP("menu.file.import_settings"),
    //% "Save / Reset"
    QT_TRID_NOOP("settings.reset.title"),
    //% "Reset"
    QT_TRID_NOOP("settings.reset.button"),
    //% "Restore all Settings preferences to their initial values and save them."
    QT_TRID_NOOP("settings.reset.description"),
    //% "Settings have been reset."
    QT_TRID_NOOP("settings.reset.succeeded"),
    //% "Settings exported."
    QT_TRID_NOOP("settings.export.succeeded"),
    //% "Settings restored and saved."
    QT_TRID_NOOP("settings.import.succeeded"),
    //% "Could not save or restore settings. Check the file format and write permissions."
    QT_TRID_NOOP("settings.manage.failed"),
    //% "VTR-PON Settings (*.json)"
    QT_TRID_NOOP("settings.manage.filter"),

    //% "Audio OUTPUT"
    QT_TRID_NOOP("settings.category.audio_output"),
    //% "Choose separate output devices for ON AIR and EDIT PFL."
    QT_TRID_NOOP("settings.output.description"),
    //% "ON AIR Output Device"
    QT_TRID_NOOP("settings.output.program_device"),
    //% "EDIT PFL Output Device"
    QT_TRID_NOOP("settings.output.pfl_device"),
    //% "System Default"
    QT_TRID_NOOP("settings.output.default"),
    //% "No Device"
    QT_TRID_NOOP("settings.output.none"),
    //% "Unavailable — %1"
    QT_TRID_NOOP("settings.output.unavailable"),
    //% "ON AIR OUTPUT DEVICE and PFL DEVICE are the same."
    QT_TRID_NOOP("settings.output.same_device"),
    //% "Current ON AIR output: %1"
    QT_TRID_NOOP("settings.output.current_program"),
    //% "Waiting until ON AIR and BGM are stopped."
    QT_TRID_NOOP("settings.output.pending"),
    //% "Applying ON AIR output device…"
    QT_TRID_NOOP("settings.output.applying"),
    //% "ON AIR output device applied."
    QT_TRID_NOOP("settings.output.applied"),
    //% "Could not apply the ON AIR output device. Select an available device and try again."
    QT_TRID_NOOP("settings.output.failed"),
    //% "Could not save the output device setting."
    QT_TRID_NOOP("settings.output.save_failed"),
    //% "Choose EDIT PFL Output Device in Settings."
    QT_TRID_NOOP("edit.pfl.no_device"),
    //% "PFL OFF — listen before nominal volume."
    QT_TRID_NOOP("edit.pfl.off"),
    //% "PFL ON — listening before nominal volume."
    QT_TRID_NOOP("edit.pfl.on"),
    //% "Opening the PFL output device…"
    QT_TRID_NOOP("edit.pfl.opening"),
    //% "PFL output is unavailable. Check the selected device, then turn PFL on again."
    QT_TRID_NOOP("edit.pfl.failed"),
    //% "The PFL output component could not be found."
    QT_TRID_NOOP("edit.pfl.helper_missing"),
    //% "Apply"
    QT_TRID_NOOP("common.apply"),
    //% "Close"
    QT_TRID_NOOP("common.close"),
    //% "Cancel"
    QT_TRID_NOOP("common.cancel"),
    //% "Add"
    QT_TRID_NOOP("common.add"),
    //% "Capturing…"
    QT_TRID_NOOP("common.capturing"),
    //% "Play"
    QT_TRID_NOOP("common.play"),
    //% "Pause"
    QT_TRID_NOOP("common.pause"),
    //% "Stop"
    QT_TRID_NOOP("common.stop"),
    //% "Repeat"
    QT_TRID_NOOP("common.repeat"),
    //% "Next"
    QT_TRID_NOOP("common.next"),
    //% "GOTO"
    QT_TRID_NOOP("common.goto"),
    //% "ON"
    QT_TRID_NOOP("common.on"),
    //% "OFF"
    QT_TRID_NOOP("common.off"),
    //% "Live"
    QT_TRID_NOOP("common.live"),
    //% "VIDEO"
    QT_TRID_NOOP("media.video"),
    //% "AUDIO"
    QT_TRID_NOOP("media.audio"),
    //% "IMAGE"
    QT_TRID_NOOP("media.image"),
    //% "POWERPOINT"
    QT_TRID_NOOP("media.powerpoint"),
    //% "PDF"
    QT_TRID_NOOP("media.pdf"),
    //% "MEDIA"
    QT_TRID_NOOP("media.unknown"),
    //% "Failed"
    QT_TRID_NOOP("media.failed"),
    //% "Generating…"
    QT_TRID_NOOP("media.generating"),

    //% "VTR-PON Controller"
    QT_TRID_NOOP("window.controller.title"),
    //% "VTR-PON Player"
    QT_TRID_NOOP("window.player.title"),
    //% "Media playout application for Windows"
    QT_TRID_NOOP("application.description"),
    //% "Switch ON AIR GPU output to 3840x2160."
    QT_TRID_NOOP("application.option.program_output_4k"),

    //% "&File"
    QT_TRID_NOOP("menu.file"),
    //% "Add File"
    QT_TRID_NOOP("menu.file.add"),
    //% "Export Playlist"
    QT_TRID_NOOP("menu.file.export_playlist"),
    //% "Import Playlist"
    QT_TRID_NOOP("menu.file.import_playlist"),
    //% "Restore Previous Session"
    QT_TRID_NOOP("menu.file.restore_previous_session"),
    //% "Settings"
    QT_TRID_NOOP("menu.file.settings"),
    //% "Exit"
    QT_TRID_NOOP("menu.file.exit"),
    //% "&Help"
    QT_TRID_NOOP("menu.help"),
    //% "Check for Updates"
    QT_TRID_NOOP("menu.help.check_updates"),
    //% "Checking for Updates…"
    QT_TRID_NOOP("menu.help.checking"),
    //% "About VTR-PON"
    QT_TRID_NOOP("menu.help.about"),
    //% "Version %1"
    QT_TRID_NOOP("about.version"),
    //% "License"
    QT_TRID_NOOP("about.license"),
    //% "Notices"
    QT_TRID_NOOP("about.notices"),
    //% "Official Website"
    QT_TRID_NOOP("about.website"),
    //% "Could not open: %1"
    QT_TRID_NOOP("about.open_failed"),

    //% "SYSTEM"
    QT_TRID_NOOP("info.source.system"),
    //% "PLAYLIST"
    QT_TRID_NOOP("info.source.playlist"),
    //% "ON AIR"
    QT_TRID_NOOP("info.source.program"),
    //% "EDIT"
    QT_TRID_NOOP("info.source.edit"),
    //% "UVC"
    QT_TRID_NOOP("info.source.uvc"),
    //% "SUCCESS"
    QT_TRID_NOOP("info.severity.success"),
    //% "WARNING"
    QT_TRID_NOOP("info.severity.warning"),
    //% "ERROR"
    QT_TRID_NOOP("info.severity.error"),
    //% "User information: %1, %2, %3"
    QT_TRID_NOOP("info.accessible.summary"),

    //% "The GStreamer runtime could not be found."
    QT_TRID_NOOP("startup.gstreamer_runtime_missing"),
    //% "Could not create the GStreamer playbin."
    QT_TRID_NOOP("startup.playbin_creation_failed"),
    //% "ON AIR audio device not found."
    QT_TRID_NOOP("startup.program_output_missing"),
    //% "PFL audio device not found."
    QT_TRID_NOOP("startup.pfl_output_missing"),
    //% "ON AIR and PFL audio devices not found."
    QT_TRID_NOOP("startup.audio_outputs_missing"),
    //% "Open audio device settings"
    QT_TRID_NOOP("info.action.audio_settings"),
    //% "PFL audio will be sent to the on-air output. Are you sure you want to play it?"
    QT_TRID_NOOP("edit.pfl.same_device_confirm"),
    //% "No PFL audio device is configured."
    QT_TRID_NOOP("edit.pfl.device_not_configured"),
    //% "Could not check the audio devices. Please check the audio device settings."
    QT_TRID_NOOP("audio.outputs.check_failed"),
    //% "Use system default when the selected ON AIR audio device is not found (ON AIR only)"
    QT_TRID_NOOP("settings.output.fallback_default"),
    //% "%1 audio output setting changed to system default because the device was unavailable."
    QT_TRID_NOOP("audio.output.fallback_applied"),
    //% "%1 audio output setting changed to %2."
    QT_TRID_NOOP("audio.output.selection_changed"),
    //% "ON AIR audio output is unavailable. Video playout can continue. Check the audio output settings and device connection."
    QT_TRID_NOOP("audio.output.unavailable"),
    //% "UVC input error."
    QT_TRID_NOOP("uvc.input_error"),
    //% "Add Files to Playlist"
    QT_TRID_NOOP("file_dialog.add_files.title"),
    //% "Media Files (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pptx *.pdf);;PowerPoint Files (*.pptx);;PDF Files (*.pdf);;All Files (*.*)"
    QT_TRID_NOOP("file_dialog.media.filter"),
    //% "Media Files (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pdf);;PDF Files (*.pdf)"
    QT_TRID_NOOP("file_dialog.media.filter_without_powerpoint"),
    //% "Export Playlist"
    QT_TRID_NOOP("file_dialog.export_playlist.title"),
    //% "Import Playlist"
    QT_TRID_NOOP("file_dialog.import_playlist.title"),
    //% "VTR-PON Playlist (*.json);;All Files (*.*)"
    QT_TRID_NOOP("file_dialog.playlist.filter"),
    //% "Playlist export failed."
    QT_TRID_NOOP("playlist.export.failed"),
    //% "Exported %1 playlist(s) and %2 item(s)."
    QT_TRID_NOOP("playlist.export.succeeded"),
    //% "Playlist import failed."
    QT_TRID_NOOP("playlist.import.failed"),
    //% "Turn OFF AIR, turn DSK off, and stop BGM before importing a playlist."
    QT_TRID_NOOP("playlist.import.playout_active"),
    //% "Imported %1 playlist(s) and %2 item(s)."
    QT_TRID_NOOP("playlist.import.succeeded"),
    //% "Previous session restore failed."
    QT_TRID_NOOP("playlist.restore_previous.failed"),
    //% "Turn OFF AIR, turn DSK off, and stop BGM before restoring the previous session."
    QT_TRID_NOOP("playlist.restore_previous.playout_active"),
    //% "Restored %1 playlist(s) and %2 item(s) from the previous session."
    QT_TRID_NOOP("playlist.restore_previous.succeeded"),
    //% "Some files could not be added."
    QT_TRID_NOOP("playlist.add_media.some_failed"),
    //% "Waiting to analyze…"
    QT_TRID_NOOP("playlist.analysis.waiting"),
    //% "Analyzing media…"
    QT_TRID_NOOP("playlist.analysis.analyzing"),
    //% "Waiting to convert…"
    QT_TRID_NOOP("playlist.conversion.waiting"),
    //% "Preparing conversion…"
    QT_TRID_NOOP("playlist.conversion.preparing"),
    //% "Exporting slides…"
    QT_TRID_NOOP("playlist.conversion.exporting"),
    //% "Rendering pages…"
    QT_TRID_NOOP("playlist.conversion.rendering"),
    //% "Encoding video…"
    QT_TRID_NOOP("playlist.conversion.encoding"),
    //% "%1 %2%"
    QT_TRID_NOOP("playlist.conversion.item_status"),
    //% "This item is being analyzed or converted and cannot be used for %1."
    QT_TRID_NOOP("playlist.conversion.action_unavailable"),
    //% "COPY"
    QT_TRID_NOOP("playlist.conversion.copy_item_action"),
    //% "%1 is unavailable while media analysis or document conversion is in progress."
    QT_TRID_NOOP("playlist.conversion.playlist_action_unavailable"),
    //% "Document conversion failed."
    QT_TRID_NOOP("playlist.conversion.failed"),
    //% "Document conversion was canceled."
    QT_TRID_NOOP("playlist.conversion.canceled"),
    //% "Add Image"
    QT_TRID_NOOP("image.add.title"),
    //% "Add the original PNG/JPEG file without conversion and set how long VTR-PON displays it. Transparent PNG data is preserved."
    QT_TRID_NOOP("image.add.description"),
    //% "Image Settings"
    QT_TRID_NOOP("image.add.settings"),
    //% "Display duration"
    QT_TRID_NOOP("image.add.display_duration"),
    //% "Remember this display duration for this session"
    QT_TRID_NOOP("image.add.remember_session"),
    //% "Add PowerPoint"
    QT_TRID_NOOP("powerpoint.add.title"),
    //% "Choose how this PowerPoint presentation should be added to the playlist."
    QT_TRID_NOOP("powerpoint.add.description"),
    //% "Add Method"
    QT_TRID_NOOP("powerpoint.add.method_group"),
    //% "PowerPoint Slide Show (Native)"
    QT_TRID_NOOP("powerpoint.add.native"),
    //% "Open PowerPoint only when preparing an item or taking it ON AIR, and control slides live. Desktop PowerPoint is required."
    QT_TRID_NOOP("powerpoint.add.native.description"),
    //% "Convert to Video"
    QT_TRID_NOOP("powerpoint.add.video"),
    //% "Export the slides and create one 1920 x 1080, 30 fps H.264/MP4 playlist item."
    QT_TRID_NOOP("powerpoint.add.video.description"),
    //% "Convert to PNG Sequence"
    QT_TRID_NOOP("powerpoint.add.png"),
    //% "Export every slide as a 1920 x 1080 PNG and add the files in slide order. The selected duration applies to every item."
    QT_TRID_NOOP("powerpoint.add.png.description"),
    //% "Timing Settings"
    QT_TRID_NOOP("powerpoint.add.video_settings"),
    //% "Duration per slide"
    QT_TRID_NOOP("powerpoint.add.slide_duration"),
    //% "Cross Dissolve"
    QT_TRID_NOOP("powerpoint.add.cross_dissolve"),
    //% "Cross Dissolve duration"
    QT_TRID_NOOP("powerpoint.add.cross_dissolve_duration"),
    //% "Remember this choice for this session"
    QT_TRID_NOOP("powerpoint.add.remember_session"),
    //% "Importing PowerPoint"
    QT_TRID_NOOP("powerpoint.import.title"),
    //% "Preparing PowerPoint import…"
    QT_TRID_NOOP("powerpoint.import.preparing"),
    //% "Exporting slides…"
    QT_TRID_NOOP("powerpoint.import.exporting"),
    //% "Encoding H.264/MP4 video…"
    QT_TRID_NOOP("powerpoint.import.encoding"),
    //% "Added %1 PowerPoint playlist item(s)."
    QT_TRID_NOOP("powerpoint.import.succeeded"),
    //% "PowerPoint conversion cannot start while a native PowerPoint item is being prepared or is ON AIR. The current ON AIR output has priority."
    QT_TRID_NOOP("powerpoint.import.native_source_active"),
    //% "Add PDF"
    QT_TRID_NOOP("pdf.add.title"),
    //% "Choose how this PDF document should be added to the playlist."
    QT_TRID_NOOP("pdf.add.description"),
    //% "Add Method"
    QT_TRID_NOOP("pdf.add.method_group"),
    //% "PDF Pages (Native)"
    QT_TRID_NOOP("pdf.add.native"),
    //% "Render pages only when preparing an item or taking it ON AIR, and control pages live."
    QT_TRID_NOOP("pdf.add.native.description"),
    //% "Convert to Video"
    QT_TRID_NOOP("pdf.add.video"),
    //% "Render the pages and create one 1920 x 1080, 30 fps H.264/MP4 playlist item."
    QT_TRID_NOOP("pdf.add.video.description"),
    //% "Convert to PNG Sequence"
    QT_TRID_NOOP("pdf.add.png"),
    //% "Render every page as a 1920 x 1080 PNG and add the files in page order. The selected duration applies to every item."
    QT_TRID_NOOP("pdf.add.png.description"),
    //% "Timing Settings"
    QT_TRID_NOOP("pdf.add.video_settings"),
    //% "Duration per page"
    QT_TRID_NOOP("pdf.add.page_duration"),
    //% "Cross Dissolve"
    QT_TRID_NOOP("pdf.add.cross_dissolve"),
    //% "Cross Dissolve duration"
    QT_TRID_NOOP("pdf.add.cross_dissolve_duration"),
    //% "Remember this choice for this session"
    QT_TRID_NOOP("pdf.add.remember_session"),
    //% "Importing PDF"
    QT_TRID_NOOP("pdf.import.title"),
    //% "Preparing PDF import…"
    QT_TRID_NOOP("pdf.import.preparing"),
    //% "Rendering pages…"
    QT_TRID_NOOP("pdf.import.rendering"),
    //% "Encoding H.264/MP4 video…"
    QT_TRID_NOOP("pdf.import.encoding"),
    //% "Added %1 PDF playlist item(s)."
    QT_TRID_NOOP("pdf.import.succeeded"),
    //% "The selected UVC device is currently unavailable."
    QT_TRID_NOOP("uvc.selected_unavailable"),
    //% "Could not start the UVC device."
    QT_TRID_NOOP("uvc.start_failed"),
    //% "Could not add the UVC input to the playlist."
    QT_TRID_NOOP("uvc.add_to_playlist_failed"),

    //% "Settings"
    QT_TRID_NOOP("settings.title"),
    //% "Settings categories"
    QT_TRID_NOOP("settings.categories.accessible"),
    //% "Appearance"
    QT_TRID_NOOP("settings.category.appearance"),
    //% "Accessibility"
    QT_TRID_NOOP("settings.category.accessibility"),
    //% "UI text size"
    QT_TRID_NOOP("settings.accessibility.text_size"),
    //% "Adjust UI text size. Changes are applied immediately and saved. Some compact controls, including DELAY, keep their original size."
    QT_TRID_NOOP("settings.accessibility.description"),
    //% "Playlist / Settings\nAdjust the slider to find a comfortable text size."
    QT_TRID_NOOP("settings.accessibility.preview"),
    //% "Small (current size)"
    QT_TRID_NOOP("settings.accessibility.small"),
    //% "Medium"
    QT_TRID_NOOP("settings.accessibility.medium"),
    //% "Large"
    QT_TRID_NOOP("settings.accessibility.large"),
    //% "Could not save the text size. Please try again."
    QT_TRID_NOOP("settings.accessibility.save_failed"),

    //% "Playlist"
    QT_TRID_NOOP("settings.category.playlist"),
    //% "Playlist Settings"
    QT_TRID_NOOP("settings.navigation.playlist"),
    //% "ON AIR"
    QT_TRID_NOOP("settings.category.on_air"),
    //% "ON AIR Settings"
    QT_TRID_NOOP("settings.navigation.on_air"),
    //% "UVC Audio Mapping"
    QT_TRID_NOOP("settings.category.audio"),
    //% "BGM Player"
    QT_TRID_NOOP("settings.category.bgm_player"),
    //% "Switcher Control"
    QT_TRID_NOOP("settings.category.switcher"),
    //% "Language"
    QT_TRID_NOOP("settings.category.language"),
    //% "Choose the Controller's visual style and UI text size."
    QT_TRID_NOOP("settings.appearance.description"),
    //% "Controller skin"
    QT_TRID_NOOP("settings.appearance.skin"),
    //% "Apply the selected Controller skin."
    QT_TRID_NOOP("settings.appearance.apply_accessible"),
    //% "Skin status"
    QT_TRID_NOOP("settings.appearance.status_accessible"),
    //% "Current skin: %1"
    QT_TRID_NOOP("settings.appearance.current_skin"),
    //% "Skin could not be applied: %1"
    QT_TRID_NOOP("settings.appearance.apply_failed"),
    //% "Applied for this session, but the preference could not be saved: %1"
    QT_TRID_NOOP("settings.appearance.save_failed"),
    //% "Studio Hardware"
    QT_TRID_NOOP("settings.appearance.skin.hardware"),
    //% "Material Dark"
    QT_TRID_NOOP("settings.appearance.skin.material_dark"),
    //% "VTR-PON Clasic"
    QT_TRID_NOOP("settings.appearance.skin.vtrpon_clasic"),
    //% "Playlist"
    QT_TRID_NOOP("settings.playlist.title"),
    //% "Configure playlist selection, audio thumbnails, and what happens after ON AIR ends. Changes are saved automatically."
    QT_TRID_NOOP("settings.playlist.description"),
    //% "Open items in EDIT"
    QT_TRID_NOOP("settings.playlist.edit_mode.field"),
    //% "Playlist EDIT activation mode"
    QT_TRID_NOOP("settings.playlist.edit_mode.accessible"),
    //% "When the EDIT button is pressed"
    QT_TRID_NOOP("settings.playlist.edit_mode.button"),
    //% "When an item is selected"
    QT_TRID_NOOP("settings.playlist.edit_mode.selection"),
    //% "Select the next item after ON AIR ends"
    QT_TRID_NOOP("settings.playlist.auto_select_next"),
    //% "Selects the next available item after an End Mode completes or OFF AIR is pressed."
    QT_TRID_NOOP("settings.playlist.auto_select_next.description"),
    //% "Audio file thumbnails"
    QT_TRID_NOOP("settings.playlist.audio_thumbnail.field"),
    //% "Show embedded album art when available. Otherwise, show the waveform."
    QT_TRID_NOOP("settings.playlist.audio_thumbnail.description"),
    //% "Waveform"
    QT_TRID_NOOP("settings.playlist.audio_thumbnail.waveform"),
    //% "Album art (waveform if unavailable)"
    QT_TRID_NOOP("settings.playlist.audio_thumbnail.album_art"),
    //% "Playlist settings status"
    QT_TRID_NOOP("settings.playlist.status_accessible"),
    //% "Playlist settings are saved automatically."
    QT_TRID_NOOP("settings.playlist.saved_automatically"),
    //% "Playlist settings could not be saved."
    QT_TRID_NOOP("settings.playlist.save_failed"),
    //% "ON AIR"
    QT_TRID_NOOP("settings.on_air.title"),
    //% "Configure ON AIR behavior and ON AIR controls. Changes are saved automatically."
    QT_TRID_NOOP("settings.on_air.description"),
    //% "Allow the same item to be taken ON AIR again while it is already ON AIR"
    QT_TRID_NOOP("settings.on_air.allow_same_item_retake"),
    //% "Allows the item currently ON AIR to be prepared and taken ON AIR again."
    QT_TRID_NOOP("settings.on_air.allow_same_item_retake.description"),
    //% "Toggle ON AIR and OFF AIR with one button"
    QT_TRID_NOOP("settings.on_air.toggle_mode"),
    //% "Show playback-rate buttons"
    QT_TRID_NOOP("settings.on_air.show_playback_rate_controls"),
    //% "Shows or hides the entire playback-rate control panel in ON AIR."
    QT_TRID_NOOP("settings.on_air.show_playback_rate_controls.description"),
    //% "Disable the FTB button"
    QT_TRID_NOOP("settings.on_air.disable_ftb_button"),
    //% "Hides the entire FTB panel and expands the OFF AIR, PLAY, and PAUSE panel to the right edge."
    QT_TRID_NOOP("settings.on_air.disable_ftb_button.description"),
    //% "ON AIR settings status"
    QT_TRID_NOOP("settings.on_air.status_accessible"),
    //% "ON AIR settings are saved automatically."
    QT_TRID_NOOP("settings.on_air.saved_automatically"),
    //% "ON AIR settings could not be saved."
    QT_TRID_NOOP("settings.on_air.save_failed"),
    //% "Display Language"
    QT_TRID_NOOP("settings.language.title"),
    //% "Choose the language used by VTR-PON. The change is applied immediately and saved automatically."
    QT_TRID_NOOP("settings.language.description"),
    //% "Display language"
    QT_TRID_NOOP("settings.language.field"),
    //% "VTR-PON display language"
    QT_TRID_NOOP("settings.language.combo_accessible"),
    //% "Language changed to %1."
    QT_TRID_NOOP("settings.language.changed"),
    //% "The language could not be changed: %1"
    QT_TRID_NOOP("settings.language.change_failed"),
    //% "The language was changed for this session, but the preference could not be saved."
    QT_TRID_NOOP("settings.language.save_failed"),
    //% "Unknown display language."
    QT_TRID_NOOP("settings.language.unknown"),
    //% "Could not load the language pack: %1"
    QT_TRID_NOOP("settings.language.load_failed"),
    //% "Audio"
    QT_TRID_NOOP("settings.audio.title"),
    //% "Choose the audio input paired with each available UVC video device."
    QT_TRID_NOOP("settings.audio.description"),
    //% "UVC audio mapping status"
    QT_TRID_NOOP("settings.audio.status_accessible"),
    //% "No UVC video devices are currently available."
    QT_TRID_NOOP("settings.audio.no_uvc_devices"),
    //% "Audio input for %1"
    QT_TRID_NOOP("settings.audio.input_for"),
    //% "No Audio"
    QT_TRID_NOOP("settings.audio.no_audio"),
    //% "Unavailable audio input (%1)"
    QT_TRID_NOOP("settings.audio.unavailable_input"),
    //% "UVC device monitoring is unavailable."
    QT_TRID_NOOP("settings.audio.uvc_monitor_unavailable"),
    //% "Audio input device monitoring is unavailable."
    QT_TRID_NOOP("settings.audio.input_monitor_unavailable"),
    //% "Mappings are saved automatically."
    QT_TRID_NOOP("settings.audio.saved_automatically"),
    //% "BGM Player"
    QT_TRID_NOOP("settings.bgm_player.title"),
    //% "Configure whether the dedicated BGM controls are shown in the ON AIR panel. Changes are saved automatically."
    QT_TRID_NOOP("settings.bgm_player.description"),
    //% "Show BGM Player"
    QT_TRID_NOOP("settings.bgm_player.show"),
    //% "Show the complete BGM control module in the ON AIR panel."
    QT_TRID_NOOP("settings.bgm_player.show.description"),
    //% "BGM Player settings status"
    QT_TRID_NOOP("settings.bgm_player.status_accessible"),
    //% "BGM Player settings are saved automatically."
    QT_TRID_NOOP("settings.bgm_player.saved_automatically"),
    //% "BGM Player settings could not be saved."
    QT_TRID_NOOP("settings.bgm_player.save_failed"),
    //% "Switcher Control"
    QT_TRID_NOOP("settings.switcher.title"),
    //% "Connect VTR-PON to an external production switcher. Only one switcher can be active at a time."
    QT_TRID_NOOP("settings.switcher.description"),
    //% "Roland and ATEM control are mutually exclusive. Enabling one automatically disables the other."
    QT_TRID_NOOP("settings.switcher.exclusive"),
    //% "Enable Roland V-8HD control"
    QT_TRID_NOOP("settings.switcher.roland_v8hd.enable"),
    //% "Automatically connects to a USB MIDI device whose name contains Roland V-8HD."
    QT_TRID_NOOP("settings.switcher.roland_v8hd.enable.description"),
    //% "Roland source number"
    QT_TRID_NOOP("settings.switcher.roland_v8hd.source"),
    //% "Roland V-8HD source number"
    QT_TRID_NOOP("settings.switcher.roland_v8hd.source.accessible"),
    //% "When VTR-PON goes ON AIR, put source %1 on Program Out"
    QT_TRID_NOOP("settings.switcher.roland_v8hd.take_program_when_on_air"),
    //% "Sends a Program select command to the Roland switcher when VTR-PON takes an item ON AIR."
    QT_TRID_NOOP("settings.switcher.roland_v8hd.take_program_when_on_air.description"),
    //% "When source %1 is switched on the Roland, take VTR-PON ON AIR"
    QT_TRID_NOOP("settings.switcher.roland_v8hd.take_on_air_when_program_selected"),
    //% "Takes the currently selected playlist item ON AIR when the Roland reports that source as Program."
    QT_TRID_NOOP("settings.switcher.roland_v8hd.take_on_air_when_program_selected.description"),
    //% "When the FTB key is pressed, also fade the switcher to black"
    QT_TRID_NOOP("settings.switcher.roland_v8hd.ftb_follows"),
    //% "Mirrors VTR-PON ON AIR FTB changes to the Roland V-8HD output fade over MIDI."
    QT_TRID_NOOP("settings.switcher.roland_v8hd.ftb_follows.description"),
    //% "Enable Blackmagic ATEM control"
    QT_TRID_NOOP("settings.switcher.atem.enable"),
    //% "Uses the separately installed ATEM add-on and connects to an ATEM switcher over USB."
    QT_TRID_NOOP("settings.switcher.atem.enable.description"),
    //% "ATEM input number"
    QT_TRID_NOOP("settings.switcher.atem.source"),
    //% "Blackmagic ATEM Program input number"
    QT_TRID_NOOP("settings.switcher.atem.source.accessible"),
    //% "When VTR-PON goes ON AIR, put input %1 on ATEM Program"
    QT_TRID_NOOP("settings.switcher.atem.take_program_when_on_air"),
    //% "Selects the specified ATEM Program input when VTR-PON takes an item ON AIR."
    QT_TRID_NOOP("settings.switcher.atem.take_program_when_on_air.description"),
    //% "When input %1 is switched on the ATEM, take VTR-PON ON AIR"
    QT_TRID_NOOP("settings.switcher.atem.take_on_air_when_program_selected"),
    //% "Takes the currently selected playlist item ON AIR when ATEM reports that input as Program."
    QT_TRID_NOOP("settings.switcher.atem.take_on_air_when_program_selected.description"),
    //% "When the FTB key is pressed, also fade the ATEM to black"
    QT_TRID_NOOP("settings.switcher.atem.ftb_follows"),
    //% "Mirrors VTR-PON ON AIR FTB changes to the ATEM Mix Effect block."
    QT_TRID_NOOP("settings.switcher.atem.ftb_follows.description"),
    //% "ATEM add-on installed"
    QT_TRID_NOOP("settings.switcher.atem.addon_installed"),
    //% "ATEM add-on is not installed"
    QT_TRID_NOOP("settings.switcher.atem.addon_missing"),
    //% "Switcher Control settings status"
    QT_TRID_NOOP("settings.switcher.status_accessible"),
    //% "Ready"
    QT_TRID_NOOP("settings.switcher.connection.ready"),
    //% "No Connection"
    QT_TRID_NOOP("settings.switcher.connection.no_connection"),
    //% "Disabled"
    QT_TRID_NOOP("settings.switcher.connection.disabled"),
    //% "Switcher Control settings are saved automatically."
    QT_TRID_NOOP("settings.switcher.saved_automatically"),
    //% "Switcher Control settings could not be saved."
    QT_TRID_NOOP("settings.switcher.save_failed"),

    //% "Input controls"
    QT_TRID_NOOP("input.accessible"),
    //% "INPUT CONTROL"
    QT_TRID_NOOP("input.title"),
    //% "UVC input controls"
    QT_TRID_NOOP("input.uvc.accessible"),
    //% "UVC INPUT"
    QT_TRID_NOOP("input.uvc.title"),
    //% "Available UVC video devices"
    QT_TRID_NOOP("input.uvc.devices_accessible"),
    //% "Add selected UVC device to playlist"
    QT_TRID_NOOP("input.uvc.add_accessible"),
    //% "Capture one frame from the current camera feed and add it to the playlist as a UVC input"
    QT_TRID_NOOP("input.uvc.add_tooltip"),
    //% "No UVC devices"
    QT_TRID_NOOP("input.uvc.none"),

    //% "Stereo audio level meter"
    QT_TRID_NOOP("audio_meter.stereo_accessible"),
    //% "Selected item nominal volume fader"
    QT_TRID_NOOP("audio_fader.nominal_name"),
    //% "Current item volume fader"
    QT_TRID_NOOP("audio_fader.item_name"),
    //% "ON AIR master volume fader"
    QT_TRID_NOOP("audio_fader.master_name"),
    //% "Selected item's nominal volume: %1 dB. This becomes the ITEM reference level when ON AIR starts. Up/Down changes it by 1 dB, Page Up/Down by 5 dB, and double-click resets it to 0 dB."
    QT_TRID_NOOP("audio_fader.nominal_description"),
    //% "Selected item's nominal volume: %1 dB\nITEM reference level at ON AIR start\nDouble-click to reset to 0 dB"
    QT_TRID_NOOP("audio_fader.nominal_tooltip"),
    //% "Current item volume: %1 dB. This value is not carried over to the next item. Up/Down changes it by 1 dB, Page Up/Down by 5 dB, and double-click resets it to 0 dB."
    QT_TRID_NOOP("audio_fader.item_description"),
    //% "Current item volume: %1 dB\nNot carried over to the next item\nDouble-click to reset to 0 dB"
    QT_TRID_NOOP("audio_fader.item_tooltip"),
    //% "ON AIR master volume: %1 dB. Up/Down changes it by 1 dB, Page Up/Down by 5 dB, and double-click resets it to 0 dB."
    QT_TRID_NOOP("audio_fader.master_description"),
    //% "ON AIR master volume: %1 dB\nDouble-click to reset to 0 dB"
    QT_TRID_NOOP("audio_fader.master_tooltip"),

    //% "EDIT"
    QT_TRID_NOOP("edit.section"),
    //% "Independent EDIT monitor"
    QT_TRID_NOOP("edit.monitor_accessible"),
    //% "EDIT audio level meter"
    QT_TRID_NOOP("edit.meter_accessible"),
    //% "EDIT seek bar"
    QT_TRID_NOOP("edit.seek_accessible"),
    //% "Current EDIT file: none"
    QT_TRID_NOOP("edit.file.none_accessible"),
    //% "Current EDIT file: %1"
    QT_TRID_NOOP("edit.file.current_accessible"),
    //% "EDIT transport controls"
    QT_TRID_NOOP("edit.transport.accessible"),
    //% "IN / OUT"
    QT_TRID_NOOP("edit.points.title"),
    //% "EDIT In and Out point controls"
    QT_TRID_NOOP("edit.points.accessible"),
    //% "START MODE"
    QT_TRID_NOOP("edit.start_mode.title"),
    //% "EDIT playlist item start mode controls"
    QT_TRID_NOOP("edit.start_mode.accessible"),
    //% "END MODE"
    QT_TRID_NOOP("edit.end_mode.title"),
    //% "EDIT playlist item end mode controls"
    QT_TRID_NOOP("edit.end_mode.accessible"),
    //% "Set In"
    QT_TRID_NOOP("edit.set_in"),
    //% "Set Out"
    QT_TRID_NOOP("edit.set_out"),
    //% "Fade In"
    QT_TRID_NOOP("edit.fade_in"),
    //% "Fade Out"
    QT_TRID_NOOP("edit.fade_out"),
    //% "Crossfade"
    QT_TRID_NOOP("edit.end_crossfade"),
    //% "GOTO Destination"
    QT_TRID_NOOP("edit.goto_target"),
    //% "Select destination"
    QT_TRID_NOOP("edit.goto_target.placeholder"),
    //% "P%1-%2  %3"
    QT_TRID_NOOP("edit.goto_target.item"),
    //% "Repeat Option"
    QT_TRID_NOOP("edit.repeat_option"),
    //% "Count"
    QT_TRID_NOOP("edit.repeat_count"),
    //% "After Repeat"
    QT_TRID_NOOP("edit.repeat_end_mode"),
    //% "Nominal Volume"
    QT_TRID_NOOP("edit.nominal_volume"),
    //% "Jump EDIT player to start"
    QT_TRID_NOOP("edit.jump_start.accessible"),
    //% "Jump EDIT player to In point"
    QT_TRID_NOOP("edit.jump_in.accessible"),
    //% "Move EDIT timeline backward while held"
    QT_TRID_NOOP("edit.rewind.accessible"),
    //% "Play EDIT player"
    QT_TRID_NOOP("edit.play.accessible"),
    //% "Pause EDIT player"
    QT_TRID_NOOP("edit.pause.accessible"),
    //% "Scan EDIT player forward while held"
    QT_TRID_NOOP("edit.forward.accessible"),
    //% "Jump EDIT player to Out point"
    QT_TRID_NOOP("edit.jump_out.accessible"),
    //% "Jump EDIT player to end"
    QT_TRID_NOOP("edit.jump_end.accessible"),
    //% "Go to start"
    QT_TRID_NOOP("edit.jump_start.tooltip"),
    //% "Go to In point"
    QT_TRID_NOOP("edit.jump_in.tooltip"),
    //% "Hold to rewind"
    QT_TRID_NOOP("edit.rewind.tooltip"),
    //% "Hold to fast-forward"
    QT_TRID_NOOP("edit.forward.tooltip"),
    //% "Go to Out point"
    QT_TRID_NOOP("edit.jump_out.tooltip"),
    //% "Go to end"
    QT_TRID_NOOP("edit.jump_end.tooltip"),
    //% "Set selected playlist item In point"
    QT_TRID_NOOP("edit.set_in.accessible"),
    //% "Set selected playlist item Out point"
    QT_TRID_NOOP("edit.set_out.accessible"),
    //% "Save the current EDIT position as the selected item's In point"
    QT_TRID_NOOP("edit.set_in.tooltip"),
    //% "Save the current EDIT position as the selected item's Out point"
    QT_TRID_NOOP("edit.set_out.tooltip"),
    //% "Selected playlist item In point time"
    QT_TRID_NOOP("edit.in_time.accessible"),
    //% "Selected playlist item Out point time"
    QT_TRID_NOOP("edit.out_time.accessible"),
    //% "Enter the In point as HH:MM:SS.cc, then press Enter"
    QT_TRID_NOOP("edit.in_time.tooltip"),
    //% "Enter the Out point as HH:MM:SS.cc, then press Enter"
    QT_TRID_NOOP("edit.out_time.tooltip"),
    //% "Set playlist item start mode to pause"
    QT_TRID_NOOP("edit.start_pause.accessible"),
    //% "Set playlist item start mode to play"
    QT_TRID_NOOP("edit.start_play.accessible"),
    //% "Set playlist item end mode to stop"
    QT_TRID_NOOP("edit.end_stop.accessible"),
    //% "Set playlist item end mode to pause"
    QT_TRID_NOOP("edit.end_pause.accessible"),
    //% "Set playlist item end mode to repeat"
    QT_TRID_NOOP("edit.end_repeat.accessible"),
    //% "Set playlist item end mode to next"
    QT_TRID_NOOP("edit.end_next.accessible"),
    //% "Set playlist item end mode to GOTO"
    QT_TRID_NOOP("edit.end_goto.accessible"),
    //% "Pause at the In point when the item goes ON AIR"
    QT_TRID_NOOP("edit.start_pause.tooltip"),
    //% "Play from the In point when the item goes ON AIR"
    QT_TRID_NOOP("edit.start_play.tooltip"),
    //% "Switch OFF AIR at the Out point or end of media"
    QT_TRID_NOOP("edit.end_stop.tooltip"),
    //% "Pause at the Out point or end of media"
    QT_TRID_NOOP("edit.end_pause.tooltip"),
    //% "Return to the In point at the Out point or end of media, then repeat playback"
    QT_TRID_NOOP("edit.end_repeat.tooltip"),
    //% "At the Out point or end of media, take the next playlist item on air; wrap to item 1 and skip the current DSK item"
    QT_TRID_NOOP("edit.end_next.tooltip"),
    //% "At the Out point or end of media, take the selected destination playlist item on air"
    QT_TRID_NOOP("edit.end_goto.tooltip"),
    //% "Enable playlist item fade in"
    QT_TRID_NOOP("edit.fade_in.accessible"),
    //% "Enable playlist item fade out"
    QT_TRID_NOOP("edit.fade_out.accessible"),
    //% "Playlist item fade in duration"
    QT_TRID_NOOP("edit.fade_in_duration.accessible"),
    //% "Playlist item fade out duration"
    QT_TRID_NOOP("edit.fade_out_duration.accessible"),
    //% "Enable crossfade to the destination playlist item"
    QT_TRID_NOOP("edit.end_crossfade.accessible"),
    //% "End crossfade duration"
    QT_TRID_NOOP("edit.end_crossfade_duration.accessible"),
    //% "%1: increase by 0.1 seconds"
    QT_TRID_NOOP("edit.duration.increase.accessible"),
    //% "%1: decrease by 0.1 seconds"
    QT_TRID_NOOP("edit.duration.decrease.accessible"),
    //% "Increase repeat count"
    QT_TRID_NOOP("edit.repeat_count.increase.accessible"),
    //% "Decrease repeat count"
    QT_TRID_NOOP("edit.repeat_count.decrease.accessible"),
    //% "Add a video and audio fade-in to the Start Mode action"
    QT_TRID_NOOP("edit.fade_in.tooltip"),
    //% "Fade out video and audio before the End Mode action"
    QT_TRID_NOOP("edit.fade_out.tooltip"),
    //% "Crossfade between ON AIR repeats, or when End Mode is Next or GOTO"
    QT_TRID_NOOP("edit.end_crossfade.tooltip"),
    //% "GOTO destination playlist item"
    QT_TRID_NOOP("edit.goto_target.accessible"),
    //% "Select the playlist item taken on air at the Out point or end of media"
    QT_TRID_NOOP("edit.goto_target.tooltip"),
    //% "Repeat count"
    QT_TRID_NOOP("edit.repeat_count.accessible"),
    //% "Set the total play count including the first play; infinity repeats without end"
    QT_TRID_NOOP("edit.repeat_count.tooltip"),
    //% "Action after repeat count"
    QT_TRID_NOOP("edit.repeat_end_mode.accessible"),
    //% "Choose what happens after the selected play count"
    QT_TRID_NOOP("edit.repeat_end_mode.tooltip"),
    //% "EDIT nominal volume fader and audio meter"
    QT_TRID_NOOP("edit.nominal_strip.accessible"),
    //% "EDIT pre-fader input signal indicator"
    QT_TRID_NOOP("edit.input_signal.accessible"),
    //% "Use HH:MM:SS.cc with minutes and seconds from 00 to 59."
    QT_TRID_NOOP("edit.time_format.tooltip"),
    //% "Enter a valid In/Out time."
    QT_TRID_NOOP("edit.time_format.invalid"),
    //% "A custom In point is set"
    QT_TRID_NOOP("edit.in_point.custom"),
    //% "In point is at the default 0 seconds"
    QT_TRID_NOOP("edit.in_point.default"),
    //% "A custom Out point is set"
    QT_TRID_NOOP("edit.out_point.custom"),
    //% "Out point is at the default media duration"
    QT_TRID_NOOP("edit.out_point.default"),
    //% "Post-nominal L %1 dB / R %2 dB%3"
    QT_TRID_NOOP("edit.audio_level.accessible"),
    //% " (clipping)"
    QT_TRID_NOOP("audio.clipping_suffix"),
    //% "Pre-fader input clipping"
    QT_TRID_NOOP("audio.input.clipping"),
    //% "Pre-fader input detected"
    QT_TRID_NOOP("audio.input.detected"),
    //% "No pre-fader input"
    QT_TRID_NOOP("audio.input.none"),

    //% "PLAYLIST"
    QT_TRID_NOOP("playlist.title"),
    //% "Playlist Name"
    QT_TRID_NOOP("playlist.name.label"),
    //% "Playlist name"
    QT_TRID_NOOP("playlist.name.accessible"),
    //% "Enter playlist name"
    QT_TRID_NOOP("playlist.name.placeholder"),
    //% "Playlists"
    QT_TRID_NOOP("playlist.tabs.accessible"),
    //% "List view"
    QT_TRID_NOOP("playlist.view.list"),
    //% "Tile view"
    QT_TRID_NOOP("playlist.view.tiles"),
    //% "Tile view\nClick a tile: ON AIR\nCtrl+Click: Select and prepare only"
    QT_TRID_NOOP("playlist.view.tiles.tooltip"),
    //% "Playlist items"
    QT_TRID_NOOP("playlist.items.accessible"),
    //% "Copy State"
    QT_TRID_NOOP("playlist.context.copy_state"),
    //% "Paste State"
    QT_TRID_NOOP("playlist.context.paste_state"),
    //% "Rename Item"
    QT_TRID_NOOP("playlist.context.rename"),
    //% "Item Color"
    QT_TRID_NOOP("playlist.context.color"),
    //% "Default"
    QT_TRID_NOOP("playlist.context.color.default"),
    //% "Red"
    QT_TRID_NOOP("playlist.context.color.red"),
    //% "Yellow"
    QT_TRID_NOOP("playlist.context.color.yellow"),
    //% "Blue"
    QT_TRID_NOOP("playlist.context.color.blue"),
    //% "Green"
    QT_TRID_NOOP("playlist.context.color.green"),
    //% "Separator Below"
    QT_TRID_NOOP("playlist.context.separator_below"),
    //% "Map"
    QT_TRID_NOOP("playlist.context.map"),
    //% "Map"
    QT_TRID_NOOP("playlist.map.title"),
    //% "Playlist playback map"
    QT_TRID_NOOP("playlist.map.table.accessible"),
    //% "Play Sequence No."
    QT_TRID_NOOP("playlist.map.sequence"),
    //% "Playlist No."
    QT_TRID_NOOP("playlist.map.playlist_number"),
    //% "Item No."
    QT_TRID_NOOP("playlist.map.item_number"),
    //% "Item name"
    QT_TRID_NOOP("playlist.map.item_name"),
    //% "Start Mode"
    QT_TRID_NOOP("playlist.map.start_mode"),
    //% "End Mode"
    QT_TRID_NOOP("playlist.map.end_mode"),
    //% "DUR"
    QT_TRID_NOOP("playlist.map.duration"),
    //% "Total: %1"
    QT_TRID_NOOP("playlist.map.total"),
    //% "Arrival: Playlist %1 / Item %2 — %3"
    QT_TRID_NOOP("playlist.map.endpoint"),
    //% "This is the predicted arrival point."
    QT_TRID_NOOP("playlist.map.endpoint.tooltip"),
    //% "The starting item's Start Mode is PAUSE. Mapping stops at this item."
    QT_TRID_NOOP("playlist.map.note.start_pause"),
    //% "The next item's Start Mode is PAUSE. Mapping stops before that item."
    QT_TRID_NOOP("playlist.map.note.destination_pause"),
    //% "A LIVE item or an item with unknown duration was found. Total is unlimited."
    QT_TRID_NOOP("playlist.map.note.live_or_unknown"),
    //% "An infinite repeat was found. Total is unlimited."
    QT_TRID_NOOP("playlist.map.note.infinite_repeat"),
    //% "A playback loop was found. Total is unlimited."
    QT_TRID_NOOP("playlist.map.note.loop"),
    //% "The NEXT or GOTO destination is unavailable. Mapping stops here."
    QT_TRID_NOOP("playlist.map.note.destination_unavailable"),
    //% "The destination is in a BGM playlist and cannot continue in ON AIR. Mapping stops here."
    QT_TRID_NOOP("playlist.map.note.destination_bgm"),
    //% "Rename Playlist Item"
    QT_TRID_NOOP("playlist.rename.title"),
    //% "Display name:"
    QT_TRID_NOOP("playlist.rename.label"),
    //% "DEL"
    QT_TRID_NOOP("playlist.delete"),
    //% "ADD FILES"
    QT_TRID_NOOP("playlist.add_files"),
    //% "CROSSFADE"
    QT_TRID_NOOP("playlist.crossfade"),
    //% "ON AIR"
    QT_TRID_NOOP("playlist.on_air"),
    //% "Move selected playlist item up"
    QT_TRID_NOOP("playlist.move_up.accessible"),
    //% "Move selected playlist item down"
    QT_TRID_NOOP("playlist.move_down.accessible"),
    //% "Move the selected item up one position"
    QT_TRID_NOOP("playlist.move_up.tooltip"),
    //% "Move the selected item down one position"
    QT_TRID_NOOP("playlist.move_down.tooltip"),
    //% "Remove selected playlist item"
    QT_TRID_NOOP("playlist.delete.accessible"),
    //% "Remove the selected item from the playlist (the source file will not be deleted)"
    QT_TRID_NOOP("playlist.delete.tooltip"),
    //% "Playlist Bulk Operations"
    QT_TRID_NOOP("playlist.bulk.title"),
    //% "Play List Once"
    QT_TRID_NOOP("playlist.bulk.play_once"),
    //% "Repeat All"
    QT_TRID_NOOP("playlist.bulk.repeat_all"),
    //% "Shuffle"
    QT_TRID_NOOP("playlist.bulk.shuffle"),
    //% "Set BGM Mode"
    QT_TRID_NOOP("playlist.bulk.bgm_mode"),
    //% "Clear BGM Mode"
    QT_TRID_NOOP("playlist.bulk.standard_mode"),
    //% "Clear Playlist"
    QT_TRID_NOOP("playlist.bulk.clear"),
    //% "Delete Playlist"
    QT_TRID_NOOP("playlist.bulk.delete"),
    //% "Undo"
    QT_TRID_NOOP("playlist.bulk.undo"),
    //% "Playlist bulk operation"
    QT_TRID_NOOP("playlist.bulk.accessible"),
    //% "Select an operation for every item in the current playlist"
    QT_TRID_NOOP("playlist.bulk.tooltip"),
    //% "Apply playlist bulk operation"
    QT_TRID_NOOP("playlist.bulk.apply.accessible"),
    //% "Apply the selected operation to the current playlist"
    QT_TRID_NOOP("playlist.bulk.apply.tooltip"),
    //% "Could not apply the playlist bulk operation."
    QT_TRID_NOOP("playlist.bulk.failed"),
    //% "Wait for media analysis and document imports to finish before applying a playlist bulk operation."
    QT_TRID_NOOP("playlist.bulk.pending_imports"),
    //% "Could not undo the playlist bulk operation."
    QT_TRID_NOOP("playlist.bulk.undo.failed"),
    //% "Undo completed. Later edits and deletions were kept."
    QT_TRID_NOOP("playlist.bulk.undo.kept_later_changes"),
    //% "Turn OFF AIR, turn DSK off, and stop BGM before undoing a playlist bulk operation."
    QT_TRID_NOOP("playlist.bulk.undo.playout_active"),
    //% "Stop BGM before changing or clearing its playlist mode."
    QT_TRID_NOOP("playlist.bulk.bgm_active"),
    //% "BGM mode playlist item"
    QT_TRID_NOOP("playlist.item.bgm_mode"),
    //% "Load selected playlist item into EDIT"
    QT_TRID_NOOP("playlist.edit.accessible"),
    //% "Load the selected item into EDIT"
    QT_TRID_NOOP("playlist.edit.tooltip"),
    //% "Load the selected item into EDIT (%1)"
    QT_TRID_NOOP("playlist.edit.shortcut_tooltip"),
    //% "Add files to playlist"
    QT_TRID_NOOP("playlist.add.accessible"),
    //% "Add video, audio, image, PowerPoint, or PDF files to the playlist"
    QT_TRID_NOOP("playlist.add.tooltip"),
    //% "Take selected playlist item on air"
    QT_TRID_NOOP("playlist.on_air.accessible"),
    //% "Take the selected item ON AIR"
    QT_TRID_NOOP("playlist.on_air.tooltip"),
    //% "Take the selected item ON AIR (%1)"
    QT_TRID_NOOP("playlist.on_air.shortcut_tooltip"),
    //% "Enable manual ON AIR crossfade"
    QT_TRID_NOOP("playlist.crossfade.accessible"),
    //% "Crossfade video using the selected duration when taking the item on air"
    QT_TRID_NOOP("playlist.crossfade.tooltip"),
    //% "Manual ON AIR crossfade duration"
    QT_TRID_NOOP("playlist.crossfade_duration.accessible"),
    //% "Playlist actions"
    QT_TRID_NOOP("playlist.actions.accessible"),
    //% "CURRENT TIME"
    QT_TRID_NOOP("playlist.current_time"),
    //% "Current time: %1"
    QT_TRID_NOOP("playlist.current_time.accessible"),
    //% "Current time synchronized with time.cloudflare.com."
    QT_TRID_NOOP("playlist.current_time.ntp_tooltip"),
    //% "Showing the local computer time because NTP is unavailable."
    QT_TRID_NOOP("playlist.current_time.local_tooltip"),
    //% "Add a new playlist"
    QT_TRID_NOOP("playlist.add_tab.tooltip"),
    //% "Playlist %1"
    QT_TRID_NOOP("playlist.default_name"),
    //% "%1 ITEMS"
    QT_TRID_NOOP("playlist.item_count"),
    //% "IN"
    QT_TRID_NOOP("playlist.item.in_label"),
    //% "OUT"
    QT_TRID_NOOP("playlist.item.out_label"),
    //% "TYPE"
    QT_TRID_NOOP("playlist.item.type_label"),
    //% "VOL"
    QT_TRID_NOOP("playlist.item.volume_label"),
    //% "START"
    QT_TRID_NOOP("playlist.item.start_label"),
    //% "END"
    QT_TRID_NOOP("playlist.item.end_label"),
    //% "Currently ON AIR"
    QT_TRID_NOOP("playlist.program.on_air"),
    //% "Currently OFF AIR"
    QT_TRID_NOOP("playlist.program.off_air"),
    //% "In %1  Out %2"
    QT_TRID_NOOP("playlist.item.points"),
    //% "Start %1  End %2  Nominal Volume %3"
    QT_TRID_NOOP("playlist.item.modes"),
    //% " + FI"
    QT_TRID_NOOP("playlist.item.fade_in"),
    //% " + FO"
    QT_TRID_NOOP("playlist.item.fade_out"),
    //% " + CF"
    QT_TRID_NOOP("playlist.item.end_crossfade"),
    //% "(%1/infinity)"
    QT_TRID_NOOP("playlist.item.repeat_infinite"),
    //% "(%1/%2)-%3"
    QT_TRID_NOOP("playlist.item.repeat_finite"),
    //% "LIVE"
    QT_TRID_NOOP("playlist.item.live"),
    //% "Start/End/EDIT disabled"
    QT_TRID_NOOP("playlist.item.uvc_controls_disabled"),
    //% "UVC LIVE"
    QT_TRID_NOOP("playlist.item.uvc_live"),
    //% "Start/End/EDIT disabled"
    QT_TRID_NOOP("playlist.item.powerpoint_controls_disabled"),
    //% "POWERPOINT LIVE"
    QT_TRID_NOOP("playlist.item.powerpoint_live"),
    //% "Start/End/EDIT disabled"
    QT_TRID_NOOP("playlist.item.pdf_controls_disabled"),
    //% "PDF LIVE"
    QT_TRID_NOOP("playlist.item.pdf_live"),
    //% "PPT NATIVE"
    QT_TRID_NOOP("playlist.thumbnail.powerpoint.native"),
    //% "PPT MP4"
    QT_TRID_NOOP("playlist.thumbnail.powerpoint.mp4"),
    //% "PPT PNG"
    QT_TRID_NOOP("playlist.thumbnail.powerpoint.png"),
    //% "PowerPoint mode: %1"
    QT_TRID_NOOP("playlist.item.powerpoint_mode"),
    //% "Playlist item currently ON AIR"
    QT_TRID_NOOP("playlist.state.on_air"),
    //% "Ready for ON AIR."
    QT_TRID_NOOP("playlist.state.cue_ready"),
    //% "Preparing for ON AIR."
    QT_TRID_NOOP("playlist.state.cue_pending"),
    //% "Could not prepare the item for ON AIR."
    QT_TRID_NOOP("playlist.state.cue_failed"),
    //% "PPT: PREPARING"
    QT_TRID_NOOP("playlist.powerpoint.preparing"),
    //% "PPT: READY"
    QT_TRID_NOOP("playlist.powerpoint.ready"),
    //% "PPT: ERROR"
    QT_TRID_NOOP("playlist.powerpoint.failed"),
    //% "Preparing the selected PowerPoint. Press ON AIR to take it automatically when preparation finishes."
    QT_TRID_NOOP("playlist.powerpoint.preparing_detail"),
    //% "Click an item to select it, then click EDIT to load it. Selection alone does not take the item on air."
    QT_TRID_NOOP("playlist.list.description"),
    //% "Tile view. Click a tile to take it ON AIR. Ctrl+Click selects and prepares it without taking it ON AIR."
    QT_TRID_NOOP("playlist.tiles.description"),
    //% "Playlist item. Select it, then click EDIT to load it. Selection alone does not take the item on air."
    QT_TRID_NOOP("playlist.item.description"),
    //% "UVC live input. EDIT and Start/End modes are disabled. Selection alone does not take the item on air."
    QT_TRID_NOOP("playlist.item.uvc_description"),
    //% "PowerPoint live slide show. EDIT and Start/End modes are disabled. Selection alone does not take the item on air."
    QT_TRID_NOOP("playlist.item.powerpoint_description"),
    //% "Native PDF pages. EDIT and Start/End modes are disabled. Selection alone does not take the item on air."
    QT_TRID_NOOP("playlist.item.pdf_description"),
    //% " State: %1."
    QT_TRID_NOOP("playlist.item.state_suffix"),
    //% " Color: %1."
    QT_TRID_NOOP("playlist.item.color_suffix"),
    //% " Separator below."
    QT_TRID_NOOP("playlist.item.separator_below_suffix"),
    //% "Item number: %1\nPlaylist number: %2\n%3\nType: %4\n%5\n%6"
    QT_TRID_NOOP("playlist.item.tooltip"),
    //% "\nThumbnail: %1"
    QT_TRID_NOOP("playlist.item.thumbnail_error"),
    //% "ON AIR ERROR"
    QT_TRID_NOOP("playlist.error.on_air"),
    //% "PREPARATION ERROR"
    QT_TRID_NOOP("playlist.error.cue"),

    //% "ON AIR"
    QT_TRID_NOOP("program.section"),
    //% "No file selected"
    QT_TRID_NOOP("program.file.none"),
    //% "Current on-air file: none"
    QT_TRID_NOOP("program.file.none_accessible"),
    //% "Current on-air file: %1"
    QT_TRID_NOOP("program.file.current_accessible"),
    //% "ON AIR ERROR"
    QT_TRID_NOOP("program.error.label"),
    //% "ERROR | %1"
    QT_TRID_NOOP("program.error.file_label"),
    //% "ON AIR error: %1"
    QT_TRID_NOOP("program.error.accessible"),
    //% "ON AIR actions"
    QT_TRID_NOOP("program.actions.accessible"),
    //% "ON AIR effect controls"
    QT_TRID_NOOP("program.effects.accessible"),
    //% "ON AIR transport controls"
    QT_TRID_NOOP("program.transport.accessible"),
    //% "PowerPoint slide controls"
    QT_TRID_NOOP("program.powerpoint.panel_accessible"),
    //% "POWERPOINT"
    QT_TRID_NOOP("program.powerpoint.title"),
    //% "PREV"
    QT_TRID_NOOP("program.powerpoint.previous"),
    //% "Go to the previous PowerPoint slide"
    QT_TRID_NOOP("program.powerpoint.previous.accessible"),
    //% "NEXT"
    QT_TRID_NOOP("program.powerpoint.next"),
    //% "Go to the next PowerPoint slide"
    QT_TRID_NOOP("program.powerpoint.next.accessible"),
    //% "PowerPoint slide position is unavailable"
    QT_TRID_NOOP("program.powerpoint.position_unavailable"),
    //% "PowerPoint slide %1 of %2"
    QT_TRID_NOOP("program.powerpoint.position_accessible"),
    //% "Go to the previous PowerPoint slide (%1)"
    QT_TRID_NOOP("program.powerpoint.previous.tooltip"),
    //% "Go to the next PowerPoint slide (%1)"
    QT_TRID_NOOP("program.powerpoint.next.tooltip"),
    //% "Already on the last PowerPoint slide (%1)"
    QT_TRID_NOOP("program.powerpoint.last.tooltip"),
    //% "PowerPoint is busy…"
    QT_TRID_NOOP("program.powerpoint.busy"),
    //% "PDF page controls"
    QT_TRID_NOOP("program.pdf.panel_accessible"),
    //% "PDF"
    QT_TRID_NOOP("program.pdf.title"),
    //% "PREV"
    QT_TRID_NOOP("program.pdf.previous"),
    //% "Go to the previous PDF page"
    QT_TRID_NOOP("program.pdf.previous.accessible"),
    //% "NEXT"
    QT_TRID_NOOP("program.pdf.next"),
    //% "Go to the next PDF page"
    QT_TRID_NOOP("program.pdf.next.accessible"),
    //% "PDF page position is unavailable"
    QT_TRID_NOOP("program.pdf.position_unavailable"),
    //% "PDF page %1 of %2"
    QT_TRID_NOOP("program.pdf.position_accessible"),
    //% "Go to the previous PDF page (%1)"
    QT_TRID_NOOP("program.pdf.previous.tooltip"),
    //% "Go to the next PDF page (%1)"
    QT_TRID_NOOP("program.pdf.next.tooltip"),
    //% "Already on the last PDF page (%1)"
    QT_TRID_NOOP("program.pdf.last.tooltip"),
    //% "PDF is busy…"
    QT_TRID_NOOP("program.pdf.busy"),
    //% "OFF AIR"
    QT_TRID_NOOP("program.off_air"),
    //% "End ON AIR output"
    QT_TRID_NOOP("program.off_air.accessible"),
    //% "Toggle fade to black"
    QT_TRID_NOOP("program.ftb.accessible"),
    //% "Pause on-air playback"
    QT_TRID_NOOP("program.pause.accessible"),
    //% "Resume on-air playback"
    QT_TRID_NOOP("program.play.accessible"),
    //% "Play the ON AIR item"
    QT_TRID_NOOP("program.play.tooltip"),
    //% "Pause ON AIR (%1: Play/Pause)"
    QT_TRID_NOOP("program.pause.shortcut_tooltip"),
    //% "Play ON AIR (%1: Play/Pause)"
    QT_TRID_NOOP("program.play.shortcut_tooltip"),
    //% "ON AIR seek bar"
    QT_TRID_NOOP("program.seek.accessible"),
    //% "ON AIR remaining timer panel"
    QT_TRID_NOOP("program.remaining.panel_accessible"),
    //% "REMAIN TIMER"
    QT_TRID_NOOP("program.remaining.title"),
    //% "ON AIR remaining time: %1"
    QT_TRID_NOOP("program.remaining.accessible"),
    //% "Total"
    QT_TRID_NOOP("program.remaining.total_mode"),
    //% "Toggle Total remaining time mode"
    QT_TRID_NOOP("program.remaining.total_mode.accessible"),
    //% "Show the remaining time to the Map Total arrival point"
    QT_TRID_NOOP("program.remaining.total_mode.tooltip"),
    //% "ON AIR Total remaining time: %1"
    QT_TRID_NOOP("program.remaining.total_accessible"),
    //% "ON AIR end mode panel"
    QT_TRID_NOOP("program.end_mode.panel_accessible"),
    //% "ON AIR end mode: %1"
    QT_TRID_NOOP("program.end_mode.accessible"),
    //% "ON AIR playback speed panel"
    QT_TRID_NOOP("program.playback_rate.panel_accessible"),
    //% "PLAYBACK SPEED"
    QT_TRID_NOOP("program.playback_rate.title"),
    //% "Set ON AIR playback speed to %1x"
    QT_TRID_NOOP("program.playback_rate.button.accessible"),
    //% "BGM controls"
    QT_TRID_NOOP("program.bgm.panel_accessible"),
    //% "BGM CONTROL"
    QT_TRID_NOOP("program.bgm.title"),
    //% "Show BGM fader"
    QT_TRID_NOOP("program.bgm.show_fader"),
    //% "Show ITEM fader"
    QT_TRID_NOOP("program.bgm.show_item_fader"),
    //% "Stop BGM and return to the In point"
    QT_TRID_NOOP("program.bgm.stop.accessible"),
    //% "Play BGM"
    QT_TRID_NOOP("program.bgm.play.accessible"),
    //% "Pause BGM"
    QT_TRID_NOOP("program.bgm.pause.accessible"),
    //% "BGM input signal indicator"
    QT_TRID_NOOP("program.bgm.signal.accessible"),
    //% "No BGM item"
    QT_TRID_NOOP("program.bgm.no_item"),
    //% "Current BGM: %1"
    QT_TRID_NOOP("program.bgm.current.accessible"),
    //% "BGM state: %1"
    QT_TRID_NOOP("program.bgm.state.accessible"),
    //% "STOPPED"
    QT_TRID_NOOP("program.bgm.state.stopped"),
    //% "LOADING"
    QT_TRID_NOOP("program.bgm.state.loading"),
    //% "PLAYING"
    QT_TRID_NOOP("program.bgm.state.playing"),
    //% "PAUSED"
    QT_TRID_NOOP("program.bgm.state.paused"),
    //% "FADE IN"
    QT_TRID_NOOP("program.bgm.state.fade_in"),
    //% "FADE OUT"
    QT_TRID_NOOP("program.bgm.state.fade_out"),
    //% "FADED OUT (VOLUME 0)"
    QT_TRID_NOOP("program.bgm.state.faded_out"),
    //% "ERROR"
    QT_TRID_NOOP("program.bgm.state.error"),
    //% "BGM signal detected"
    QT_TRID_NOOP("program.bgm.signal.active"),
    //% "No BGM signal"
    QT_TRID_NOOP("program.bgm.signal.inactive"),
    //% "BGM input is clipping"
    QT_TRID_NOOP("program.bgm.signal.clipped"),
    //% "The BGM controls will be hidden after BGM stops."
    QT_TRID_NOOP("program.bgm.hide_after_stop"),
    //% "No playable item was found in the current BGM playlist."
    QT_TRID_NOOP("program.bgm.no_playable_item"),
    //% "Skipped BGM item: %1"
    QT_TRID_NOOP("program.bgm.item_skipped"),
    //% "The BGM item transition could not be completed."
    QT_TRID_NOOP("program.bgm.transition_failed"),
    //% "BGM could not be stopped. Press STOP again to reset it."
    QT_TRID_NOOP("program.bgm.stop_failed"),
    //% "ON AIR audio controls"
    QT_TRID_NOOP("program.audio.accessible"),
    //% "Current item post-fader audio level meter"
    QT_TRID_NOOP("program.item_meter.accessible"),
    //% "ON AIR audio level meter"
    QT_TRID_NOOP("program.master_meter.accessible"),
    //% "Current item pre-fader input signal indicator"
    QT_TRID_NOOP("program.input_signal.accessible"),
    //% "MUTE"
    QT_TRID_NOOP("program.mute"),
    //% "ON AIR master mute"
    QT_TRID_NOOP("program.mute.accessible"),
    //% "Mute ON AIR"
    QT_TRID_NOOP("program.mute.tooltip"),
    //% "ITEM"
    QT_TRID_NOOP("program.item"),
    //% "MASTER"
    QT_TRID_NOOP("program.master"),
    //% "%1 %2 duration"
    QT_TRID_NOOP("program.manual_fade_duration.accessible"),
    //% "Turn DSK off and return it to the In point"
    QT_TRID_NOOP("program.dsk.off_tooltip"),
    //% "DSK is active; press to turn off"
    QT_TRID_NOOP("program.dsk.on_accessible"),
    //% "DSK ON queued: %1 (click to cancel)"
    QT_TRID_NOOP("program.dsk.queued_tooltip"),
    //% "DSK is preparing to turn on; press to cancel"
    QT_TRID_NOOP("program.dsk.queued_accessible"),
    //% "Preparing DSK: %1 (click to queue ON)"
    QT_TRID_NOOP("program.dsk.preparing_tooltip"),
    //% "DSK is preparing; press to turn on when ready"
    QT_TRID_NOOP("program.dsk.preparing_accessible"),
    //% "DSK error: %1"
    QT_TRID_NOOP("program.dsk.error_tooltip"),
    //% "DSK error: %1; press to retry with the selected item"
    QT_TRID_NOOP("program.dsk.retry_accessible"),
    //% "DSK error: %1; select a valid playlist item"
    QT_TRID_NOOP("program.dsk.select_accessible"),
    //% "Send the selected playlist item to DSK"
    QT_TRID_NOOP("program.dsk.send_tooltip"),
    //% "Select a playlist item to send to DSK"
    QT_TRID_NOOP("program.dsk.select_tooltip"),
    //% "DSK is inactive; press to show the selected item"
    QT_TRID_NOOP("program.dsk.inactive_ready_accessible"),
    //% "DSK is inactive; select a playlist item"
    QT_TRID_NOOP("program.dsk.inactive_accessible"),
    //% "Play the DSK item"
    QT_TRID_NOOP("program.dsk.play.tooltip"),
    //% "Pause the DSK item"
    QT_TRID_NOOP("program.dsk.pause.tooltip"),

    //% "In and Out markers are not set"
    QT_TRID_NOOP("range_markers.none"),
    //% "In marker: %1 ms; Out marker: not set; timeline before In is inactive"
    QT_TRID_NOOP("range_markers.in_only"),
    //% "In marker: %1 ms; Out marker: %2 ms; timeline before In and after Out is inactive"
    QT_TRID_NOOP("range_markers.both"),

    //% "Could not create the EDIT player (ON AIR will continue)"
    QT_TRID_NOOP("edit.error.player_create"),
    //% "Could not unload the item from EDIT."
    QT_TRID_NOOP("edit.error.unload"),
    //% "Could not go to the EDIT In point."
    QT_TRID_NOOP("edit.error.go_in"),
    //% "Could not go to the EDIT Out point."
    QT_TRID_NOOP("edit.error.go_out"),
    //% "Could not determine the EDIT end position."
    QT_TRID_NOOP("edit.error.end_position"),
    //% "The EDIT player is unavailable (ON AIR will continue)"
    QT_TRID_NOOP("edit.error.player_unavailable"),
    //% "The file to load into EDIT could not be found."
    QT_TRID_NOOP("edit.error.file_missing"),
    //% "Failed to load the file into EDIT."
    QT_TRID_NOOP("edit.error.load"),
    //% "The In/Out points of an item currently on air cannot be changed."
    QT_TRID_NOOP("edit.error.points_locked"),
    //% "Could not determine the current EDIT position."
    QT_TRID_NOOP("edit.error.current_position"),
    //% "Set the In/Out point within the media duration."
    QT_TRID_NOOP("edit.error.point_outside_media"),
    //% "Set the In point before the Out point and the end of the media."
    QT_TRID_NOOP("edit.error.in_order"),
    //% "Set the Out point after the In point."
    QT_TRID_NOOP("edit.error.out_order"),
    //% "Could not save the In/Out point."
    QT_TRID_NOOP("edit.error.save_points"),
    //% "Could not save the Start Mode."
    QT_TRID_NOOP("edit.error.save_start_mode"),
    //% "Could not save the End Mode."
    QT_TRID_NOOP("edit.error.save_end_mode"),
    //% "Could not save the GOTO destination."
    QT_TRID_NOOP("edit.error.save_goto_target"),
    //% "Could not save the repeat options."
    QT_TRID_NOOP("edit.error.save_repeat_options"),
    //% "Could not save the fade-in settings."
    QT_TRID_NOOP("edit.error.save_fade_in"),
    //% "Could not save the fade-out settings."
    QT_TRID_NOOP("edit.error.save_fade_out"),
    //% "Could not save the end crossfade settings."
    QT_TRID_NOOP("edit.error.save_end_crossfade"),
    //% "Could not save the nominal volume."
    QT_TRID_NOOP("edit.error.save_nominal"),
    //% "EDIT %1 failed."
    QT_TRID_NOOP("edit.error.operation_failed"),
    //% "EDIT Play failed."
    QT_TRID_NOOP("edit.error.play"),
    //% "EDIT Pause failed."
    QT_TRID_NOOP("edit.error.pause"),
    //% "Failed to stop EDIT."
    QT_TRID_NOOP("edit.error.stop"),
    //% "Could not determine the EDIT scan position."
    QT_TRID_NOOP("edit.error.scan_position"),
    //% "Failed to prepare EDIT for rewind."
    QT_TRID_NOOP("edit.error.prepare_rewind"),
    //% "EDIT scan failed."
    QT_TRID_NOOP("edit.error.scan"),
    //% "Failed to restore normal EDIT playback."
    QT_TRID_NOOP("edit.error.restore_playback"),
    //% "EDIT seek failed."
    QT_TRID_NOOP("edit.error.seek"),
    //% "Only EDIT playback was stopped"
    QT_TRID_NOOP("edit.error.playback_only_stopped"),

    //% "An item currently ON AIR cannot be deleted."
    QT_TRID_NOOP("program.warning.delete_on_air"),
    //% "An item currently in DSK cannot be deleted."
    QT_TRID_NOOP("program.warning.delete_dsk"),
    //% "An item currently used by BGM cannot be deleted."
    QT_TRID_NOOP("program.warning.delete_bgm"),
    //% "BGM mode items are controlled with BGM CONTROL and cannot be taken ON AIR."
    QT_TRID_NOOP("program.warning.bgm_mode_on_air"),
    //% "BGM mode items cannot be sent to DSK."
    QT_TRID_NOOP("program.warning.bgm_mode_dsk"),
    //% "The item cannot be deleted because DSK could not be turned off."
    QT_TRID_NOOP("program.warning.delete_dsk_off_failed"),
    //% "The item cannot be deleted because its preparation could not be released."
    QT_TRID_NOOP("program.warning.delete_cue_release_failed"),
    //% "Failed to switch OFF AIR. The playback state was not changed."
    QT_TRID_NOOP("program.warning.off_air_failed"),
    //% "Select an item to take ON AIR."
    QT_TRID_NOOP("program.warning.select_on_air_item"),
    //% "Could not take the selected item ON AIR."
    QT_TRID_NOOP("program.warning.take_cue_failed"),
    //% "Cannot go ON AIR because the ON AIR audio output device could not be opened. Check the audio output settings and device connection."
    QT_TRID_NOOP("program.warning.audio_output_unavailable"),
    //% "The ON AIR audio output device is being prepared. Wait a moment and try ON AIR again."
    QT_TRID_NOOP("program.warning.audio_output_changing"),
    //% "Could not retrieve the playlist item to take ON AIR."
    QT_TRID_NOOP("program.warning.retrieve_on_air_item"),
    //% "Could not retrieve the latest item information when ON AIR started."
    QT_TRID_NOOP("program.warning.retrieve_on_air_latest"),
    //% "The item was taken ON AIR, but Start Mode \"Play\" could not be started."
    QT_TRID_NOOP("program.warning.start_play_failed"),
    //% "Select a valid playlist item to send to DSK."
    QT_TRID_NOOP("program.warning.select_dsk_item"),
    //% "Could not prepare the DSK item."
    QT_TRID_NOOP("program.warning.dsk_cue_failed"),
    //% "Could not display DSK."
    QT_TRID_NOOP("program.warning.dsk_display_failed"),
    //% "Could not execute DSK End Mode \"Stop\"."
    QT_TRID_NOOP("program.warning.dsk_end_stop_failed"),
    //% "Could not execute DSK End Mode \"Pause\"."
    QT_TRID_NOOP("program.warning.dsk_end_pause_failed"),
    //% "Could not execute DSK End Mode \"Repeat\"."
    QT_TRID_NOOP("program.warning.dsk_end_repeat_failed"),
    //% "Could not execute DSK End Mode \"Next\"."
    QT_TRID_NOOP("program.warning.dsk_end_next_failed"),
    //% "Could not turn DSK off."
    QT_TRID_NOOP("program.warning.dsk_off_failed"),
    //% "A playback error occurred in DSK."
    QT_TRID_NOOP("program.warning.dsk_playback_error"),
    //% "Failed to seek to the In point before playback."
    QT_TRID_NOOP("program.warning.seek_in_failed"),
    //% "Playback failed."
    QT_TRID_NOOP("program.warning.playback_failed"),
    //% "Pause failed."
    QT_TRID_NOOP("program.warning.pause_failed"),
    //% "Playback rate change failed."
    QT_TRID_NOOP("program.warning.playback_rate_failed"),
    //% "Could not retrieve the latest playlist item information during end processing."
    QT_TRID_NOOP("program.warning.retrieve_end_item"),
    //% "Could not execute End Mode \"Repeat\"."
    QT_TRID_NOOP("program.warning.end_repeat_failed"),
    //% "Could not execute End Mode \"Stop\"."
    QT_TRID_NOOP("program.warning.end_stop_failed"),
    //% "Could not execute End Mode \"Pause\"."
    QT_TRID_NOOP("program.warning.end_pause_failed"),
    //% "Could not execute End Mode \"Next\"."
    QT_TRID_NOOP("program.warning.end_next_failed"),
    //% "ON AIR seek failed."
    QT_TRID_NOOP("program.warning.seek_failed"),
    //% "GStreamer playback error."
    QT_TRID_NOOP("program.error.gstreamer_playback"),
    //% "PowerPoint conversion cannot start while a native PowerPoint item is ON AIR. The current ON AIR output has priority."
    QT_TRID_NOOP("powerpoint.import.native_on_air"),
    //% "PowerPoint conversion could not start because an off-air native PowerPoint source could not be fully released."
    QT_TRID_NOOP("powerpoint.import.release_cue_failed"),

    //% "ON AIR audio output is being prepared or changed. Press BGM PLAY again after it finishes."
    QT_TRID_NOOP("program.bgm.audio_output_changing"),
    //% "ON AIR audio output is unavailable. Check audio output settings and press BGM PLAY again after recovery."
    QT_TRID_NOOP("program.bgm.audio_output_unavailable"),
    //% "BGM playback failed."
    QT_TRID_NOOP("program.bgm.play_failed"),
    //% "Load an item into EDIT before changing its In/Out points."
    QT_TRID_NOOP("edit.error.media_not_loaded"),
    //% "The In/Out points of an item in use by BGM cannot be changed. Stop BGM first."
    QT_TRID_NOOP("edit.error.points_bgm_active"),
    //% "In/Out points can only be changed for file sources."
    QT_TRID_NOOP("edit.error.points_file_only"),
    //% "This item's settings are locked. Its In/Out points cannot be changed."
    QT_TRID_NOOP("edit.error.points_settings_locked"),

    // View menu.
    //% "&View"
    QT_TRID_NOOP("menu.view"),
    //% "Fullscreen"
    QT_TRID_NOOP("menu.view.fullscreen"),
    //% "Always on Top"
    QT_TRID_NOOP("menu.view.fullscreen.always_on_top"),
    //% "Minimize"
    QT_TRID_NOOP("menu.view.fullscreen.minimize"),
    //% "Display Destination Window"
    QT_TRID_NOOP("menu.view.fullscreen.display_target_window"),
    //% "Controller"
    QT_TRID_NOOP("menu.view.controller"),
    //% "Full Screen"
    QT_TRID_NOOP("menu.view.controller.fullscreen"),
});

} // namespace

namespace vtrpon {

QString uiText(const char *id, int n) { return qtTrId(id, n); }

QStringList knownUiTextIds() {
  QStringList ids;
  ids.reserve(static_cast<qsizetype>(kUiTextIds.size()));
  for (const char *id : kUiTextIds) {
    ids.append(QString::fromLatin1(id));
  }
  return ids;
}

LocalizedText::LocalizedText(const char *id) : id_(id) {}

LocalizedText LocalizedText::fromRaw(const QString &text) {
  LocalizedText result;
  result.rawText_ = text;
  return result;
}

LocalizedText LocalizedText::arg(const QString &value) const {
  LocalizedText result = *this;
  result.arguments_.append(value);
  return result;
}

LocalizedText LocalizedText::arg(qint64 value) const {
  return arg(QString::number(value));
}

LocalizedText LocalizedText::arg(int value) const {
  return arg(QString::number(value));
}

QString LocalizedText::resolve() const {
  if (id_.isEmpty()) {
    return rawText_;
  }
  QString result = uiText(id_.constData());
  for (const QString &argument : arguments_) {
    result = result.arg(argument);
  }
  return result;
}

bool LocalizedText::isEmpty() const {
  return id_.isEmpty() && rawText_.isEmpty();
}

} // namespace vtrpon
