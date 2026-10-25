#include "audio_output_settings.h"
#include "controller_settings.h"
#include "program_audio_output.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <functional>
#include <iostream>

namespace {
bool check(bool condition, const char *message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    return condition;
}
bool until(const std::function<bool()> &done, int timeout = 7000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}
bool settle(ProgramAudioOutput &output) {
    return until([&] { output.updateDevice(true); return !output.deviceChanging(); });
}
bool route(ProgramAudioOutput &output, const QString &uid) {
    auto *sink = gst_bin_get_by_name(GST_BIN(output.pipeline()), "program-audio-output");
    gint device = 0;
    g_object_get(sink, "device", &device, nullptr);
    // Read the actual opened AudioDeviceID through HAL, independently of the
    // requested unique-id property. The pipeline outputs silence in this test.
    const AudioObjectPropertyAddress property{kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    const auto status = AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &value);
    const auto expected = CFStringCreateWithCharacters(nullptr,
        reinterpret_cast<const UniChar *>(uid.utf16()), uid.size());
    const bool matches = status == noErr && value && CFEqual(value, expected);
    if (value) CFRelease(value);
    CFRelease(expected);
    gst_object_unref(sink);
    return matches;
}
bool openSaved(const QString &uid) {
    ProgramAudioOutput output;
    if (!check(vtrpon::savedProgramOutputDeviceId() == uid, "fresh process loads saved UID")
        || !output.initialize(nullptr, nullptr, vtrpon::savedProgramOutputDeviceId())) return false;
    output.updateDevice(true);
    const bool ok = check(settle(output) && output.ready() && route(output, uid),
                          "fresh process opens the saved physical endpoint");
    return output.stop() && ok;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    gst_init(nullptr, nullptr);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == QStringLiteral("--restore")) {
        app.setProperty("vtrpon.settingsFile", arguments[2]);
        return openSaved(arguments[3]) ? 0 : 1;
    }
    if (arguments.size() != 2) {
        std::cerr << "Usage: mac-audio-output-test <output UID to verify>\n";
        return 2;
    }
    const auto selected = arguments[1];
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const auto settingsFile = temporary.filePath(QStringLiteral("user.ini"));
    app.setProperty("vtrpon.settingsFile", settingsFile);
    bool refreshed = false;
    bool success = false;
    AudioOutputDeviceMonitor monitor;
    monitor.setRefreshInterval(0);
    QObject::connect(&monitor, &AudioOutputDeviceMonitor::refreshed, &app,
        [&](bool ok) { refreshed = true; success = ok; });
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
    heartbeat.start();
    if (!check(until([&] { return refreshed; }) && success && heartbeats > 0,
               "isolated HAL enumeration completes while event loop runs")) return 1;
    if (!check(!vtrpon::missingAudioOutputs(monitor.devices(), monitor.defaultDeviceId(), selected, {}).program,
               "selected UID occurs in output enumeration")) return 1;
    if (!check(vtrpon::saveProgramOutputDeviceId(selected), "UID saved to isolated INI")) return 1;

    ProgramAudioOutput output;
    if (!output.initialize(nullptr, nullptr, {})) return 1;
    output.updateDevice(true);
    if (!check(settle(output) && output.ready() && route(output, monitor.defaultDeviceId()),
               "system default opens its real CoreAudio endpoint")) return 1;
    output.requestDevice(selected);
    output.updateDevice(false);
    if (!check(!output.deviceChanging() && output.appliedDevice().isEmpty()
                && output.deviceStatusKey() == QStringLiteral("settings.output.pending"),
               "busy output defers selection without disrupting current route")) return 1;
    output.updateDevice(true);
    if (!check(settle(output) && output.ready() && output.appliedDevice() == selected && route(output, selected),
               "idle output applies the explicit UID")) return 1;

    // Make sure actual streaming reaches the selected sink. The on-air and BGM
    // gates remain closed, so opening devices never emits a test tone.
    std::atomic_int buffers{0};
    auto *sink = gst_bin_get_by_name(GST_BIN(output.pipeline()), "program-audio-output");
    auto *pad = gst_element_get_static_pad(sink, "sink");
    const auto probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer context) {
            ++*static_cast<std::atomic_int *>(context);
            return GST_PAD_PROBE_OK;
        }, &buffers, nullptr);
    bool ok = check(until([&] { return buffers.load() >= 5; }), "audio buffers reach the selected sink");
    output.requestDevice(QStringLiteral("VTRPON-TEST-NONEXISTENT-UID"));
    output.updateDevice(true);
    ok &= check(settle(output) && output.ready() && output.appliedDevice() == selected
        && output.deviceStatusKey() == QStringLiteral("settings.output.failed") && route(output, selected),
        "missing selection reports failure and restores the previous endpoint");
    output.requestDevice({});
    output.updateDevice(true);
    ok &= check(settle(output) && output.ready() && output.appliedDevice().isEmpty()
        && route(output, monitor.defaultDeviceId()), "explicit selection can return to system default");
    ok &= check(output.stop(), "device worker and output shutdown are confirmed");
    gst_pad_remove_probe(pad, probe);
    gst_object_unref(pad);
    gst_object_unref(sink);
    {
        ProgramAudioOutput missing;
        const bool initialized = missing.initialize(nullptr, nullptr, QStringLiteral("VTRPON-TEST-MISSING-STARTUP"));
        if (initialized) missing.updateDevice(true);
        ok &= check(initialized && settle(missing) && !missing.ready()
            && missing.deviceStatusKey() == QStringLiteral("settings.output.failed"),
            "missing saved endpoint at startup never silently opens the system default");
        ok &= check(missing.stop(), "failed startup output confirms shutdown");
    }
    monitor.requestShutdown();
    ok &= check(until([&] { return monitor.shutdownComplete(); }), "enumerator shutdown completes");
    QProcess child;
    child.start(app.applicationFilePath(), {QStringLiteral("--restore"), settingsFile, selected});
    ok &= check(until([&] { return child.state() == QProcess::NotRunning; })
        && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "restarted process restores the selected endpoint");
    std::cout << child.readAllStandardOutput().constData();
    std::cerr << child.readAllStandardError().constData();
    return ok ? 0 : 1;
}
