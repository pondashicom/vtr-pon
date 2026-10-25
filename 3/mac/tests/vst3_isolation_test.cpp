#include "vst_service.h"
#include "vst_editor.h"
#include "controller_settings.h"
#include "audio_equalizer.h"
#include "audio_effects.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QTranslator>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <windows.h>
#include <tlhelp32.h>
using namespace vtrpon;
namespace {
bool require(bool ok, const char *message) {
    if (!ok) std::cerr << "FAIL: " << message << '\n';
    return ok;
}
HWND findEditor(HWND owner, const QString &title) {
    struct Search { HWND owner; QString title; HWND found = nullptr; } search{owner, title};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto &s = *reinterpret_cast<Search *>(data);
        wchar_t text[512]{}; GetWindowTextW(window, text, 512);
        if (GetWindow(window, GW_OWNER) == s.owner && QString::fromWCharArray(text).startsWith(s.title)
            && QString::fromWCharArray(text).contains("VTR-PON3 VST")) {
            s.found = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
template<class F> bool until(F predicate, int timeout = 5000) {
    QElapsedTimer time; time.start();
    while (!predicate() && time.elapsed() < timeout) { QApplication::processEvents(); QThread::msleep(5); }
    return predicate();
}
int scanParent(VstService &service) {
    service.scan();
    DWORD scannerPid = 0;
    if (!until([&] {
        // The scanner belongs to the background worker, not the GUI QObject tree.
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) do {
            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"vtr-pon-vst3-addon.exe") == 0) {
                scannerPid = entry.th32ProcessID; break;
            }
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return scannerPid != 0;
    })) return 1;
    std::cout << "SCANNER_PID=" << scannerPid << std::endl;
    return QCoreApplication::exec();
}
bool checkScannerParentDeath(const QString &plugin, const QString &directory,
                            bool hangDllInitialization) {
    const auto common = directory + QStringLiteral("/common");
    const auto local = directory + QStringLiteral("/local");
    const auto pluginDir = common + QStringLiteral("/VST3");
    if (!require(QDir().mkpath(pluginDir) && QDir().mkpath(local)
        && QFile::copy(plugin, pluginDir + QStringLiteral("/scan-hang.vst3")), "prepare isolated scan roots")) return false;
    const auto eventName = QStringLiteral("Local\\VtrPonVstScanTest-") + QUuid::createUuid().toString(QUuid::Id128);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(eventName.utf16()));
    if (!require(ready != nullptr, "create scan initialization event")) return false;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("CommonProgramW6432"), common);
    environment.insert(QStringLiteral("LOCALAPPDATA"), local);
    environment.insert(hangDllInitialization ? QStringLiteral("VTRPON_TEST_VST_SCAN_DLL_HANG_EVENT")
        : QStringLiteral("VTRPON_TEST_VST_SCAN_HANG_EVENT"), eventName);
    QProcess parent;
    parent.setProcessEnvironment(environment);
    parent.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--scan-parent")});
    bool ok = require(parent.waitForStarted(5000), "start disposable Controller scan parent");
    QByteArray output;
    DWORD scannerPid = 0;
    ok &= require(until([&] {
        output += parent.readAllStandardOutput();
        const auto marker = output.indexOf("SCANNER_PID=");
        if (marker < 0 || output.indexOf('\n', marker) < 0) return false;
        scannerPid = output.mid(marker + 12).split('\n').front().trimmed().toULong();
        return scannerPid != 0;
    }), "Controller reports its actual scan child");
    HANDLE scanner = scannerPid ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, scannerPid) : nullptr;
    ok &= require(scanner != nullptr, "open scan child for exit verification");
    ok &= require(until([&] { return WaitForSingleObject(ready, 0) == WAIT_OBJECT_0; }),
        hangDllInitialization ? "scan reaches hung vendor DLL static initialization"
                              : "scan reaches hung vendor InitModule");
    ok &= require(scanner && WaitForSingleObject(scanner, 0) == WAIT_TIMEOUT, "scan remains alive while Controller lives");
    parent.kill();
    ok &= require(parent.waitForFinished(5000), "abruptly terminate only the disposable Controller");
    if (scanner) {
        const bool exited = until([&] { return WaitForSingleObject(scanner, 0) == WAIT_OBJECT_0; });
        ok &= require(exited, "hung scanner exits after abrupt Controller death");
        if (!exited) {
            TerminateProcess(scanner, 1);
            WaitForSingleObject(scanner, 5000);
        }
        CloseHandle(scanner);
    }
    CloseHandle(ready);
    return ok;
}
bool checkDualEditors(VstService &service, const QString &path, const QString &masterUid,
    const QString &masterTitle, const QString &itemUid, const QString &itemTitle) {
    service.setEnabled(true);
    QWidget control;
    control.setWindowTitle("VTR-PON dual VST editor test"); control.show();
    QPushButton itemButton("ITEM", &control), masterButton("MASTER", &control);
    masterButton.move(100, 0);
    bindVstButton(&itemButton, VstSlot::Item); bindVstButton(&masterButton, VstSlot::Master);
    const auto owner = reinterpret_cast<HWND>(control.winId());
    service.select(VstSlot::Master, path, masterUid);
    service.select(VstSlot::Item, path, itemUid);
    bool ok = require(until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)) && (service.active(VstSlot::Item) && !service.applying(VstSlot::Item)); }, 18000), "both effects active");
    std::array<HWND, 2> originalWindows{};
    std::array<HWND, 2> originalChildren{};
    std::atomic_bool validAudio{true};
    std::jthread audio([&](std::stop_token stop) {
        std::array<float, 960> pcm;
        while (!stop.stop_requested()) {
            pcm.fill(0.1f);
            processVst(VstSlot::Item, pcm.data(), 480);
            processVst(VstSlot::Master, pcm.data(), 480);
            for (auto sample : pcm) if (!std::isfinite(sample)) validAudio = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    for (int pass = 0; pass < 4 && ok; ++pass) {
        for (auto slot : {VstSlot::Item, VstSlot::Master}) {
            const bool item = slot == VstSlot::Item;
            showVstEditor(item ? &itemButton : &masterButton, slot);
            auto *selection = control.findChild<QDialog *>(item ? "vstITEMWindow" : "vstMASTERWindow");
            if (!require(selection != nullptr, "slot selector opens")) { ok = false; break; }
            selection->findChild<QPushButton *>("vstOpen")->click();
            const auto title = item ? itemTitle : masterTitle;
            ok &= require(until([&] {
                auto window = findEditor(owner, title);
                return window && IsWindowVisible(window) && !IsIconic(window) && GetForegroundWindow() == window;
            }), "requested editor is visible, restored and foreground");
            auto window = findEditor(owner, title);
            ok &= require(until([&] {
                const auto childWindow = window ? GetWindow(window, GW_CHILD) : nullptr;
                return childWindow && IsWindowVisible(childWindow);
            }), "plug-in finishes showing its native child");
            auto child = window ? GetWindow(window, GW_CHILD) : nullptr;
            if (pass == 0) { originalWindows[item] = window; originalChildren[item] = child; }
            ok &= require(window && window == originalWindows[item], "reopening preserves the native parent used by the plug-in view");
            ok &= require(child && child == originalChildren[item] && IsWindowVisible(child), "actual plug-in GUI survives close and reopen");
            std::cout << "pass=" << pass << " item=" << item << " window=" << window << " child=" << child
                << " foreground=" << GetForegroundWindow() << " iconic=" << (window ? IsIconic(window) : -1) << '\n';
            if (pass == 1 && window) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                ok &= require(until([&] { return !IsWindowVisible(window); }), "close hides the editor");
            }
            if (pass == 2 && window) {
                ShowWindowAsync(window, SW_MINIMIZE);
                ok &= require(until([&] { return IsIconic(window); }), "editor is minimized before reopen");
            }
        }
    }
    audio.request_stop(); audio.join();
    ok &= require(validAudio, "dual effects preserve finite PCM while switching editors");
    ok &= require((service.active(VstSlot::Master) && !service.applying(VstSlot::Master)) && (service.active(VstSlot::Item) && !service.applying(VstSlot::Item)), "both effects survive editor switching");
    return ok;
}
bool checkSwitching(VstService &service, const QString &pathA, const QString &uidA,
    const QString &pathB, const QString &uidB, bool testGain) {
    std::atomic_bool valid{true};
    std::atomic_int processed{0};
    int failures = 0;
    const auto connection = QObject::connect(&service, &VstService::failure, &service,
        [&](const QString &, const QString &) { ++failures; });
    std::jthread audio([&](std::stop_token stop) {
        std::array<float, 960> pcm;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            pcm.fill(0.25f);
            processVst(VstSlot::Master, pcm.data(), 480);
            for (const auto sample : pcm)
                if (!std::isfinite(sample) || (testGain && sample != 0.25f && sample != 0.125f)) valid = false;
            ++processed;
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    });
    bool ok = true;
    for (int pass = 0; pass < 4; ++pass) {
        service.select(VstSlot::Master, pass % 2 ? pathB : pathA, pass % 2 ? uidB : uidA);
        ok &= require(until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)) || service.statusKey(VstSlot::Master) == "vst.failed"; }, 18000)
            && (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)), "switch initializes while audio keeps streaming");
        const auto start = processed.load();
        ok &= require(until([&] { return processed.load() >= start + 200; }, 4000), "continuous 10 ms audio cadence");
        ok &= require((service.active(VstSlot::Master) && !service.applying(VstSlot::Master)), "effect remains active after processing bursts");
    }
    audio.request_stop(); audio.join();
    QObject::disconnect(connection);
    return require(valid && failures == 0, "switching preserves valid PCM and raises no fault") && ok;
}
bool checkApply(VstService &service, const QString &path, const QString &goodUid,
                const QString &badUid, const QString &nextUid = {}, const QString &hangUid = {}) {
    constexpr auto slot = VstSlot::Master;
    const bool fixture = !nextUid.isEmpty();
    service.setEnabled(true);
    service.select(slot, path, goodUid);
    if (!require(until([&] { return !service.applying(slot) && service.active(slot); }, 18000),
                 "original plug-in active before replacement")) return false;
    const auto savedUid = [&] { return makeControllerSettings().value("audio/vst/master/uid").toString(); };
    const auto runningPid = [&] {
        for (auto *process : service.findChildren<QProcess *>())
            if (process->state() == QProcess::Running && process->arguments().contains(goodUid)) return process->processId();
        return qint64(0);
    };
    const auto originalPid = runningPid();
    bool ok = require(originalPid != 0, "original plug-in process is identified");
    int rejected = 0, stopped = 0;
    const auto rejectedConnection = QObject::connect(&service, &VstService::applicationFailed, &service,
        [&](const QString &, const QString &) { ++rejected; });
    const auto stoppedConnection = QObject::connect(&service, &VstService::failure, &service,
        [&](const QString &, const QString &) { ++stopped; });
    std::atomic_bool valid{true};
    std::atomic_int processed{0};
    std::jthread audio([&](std::stop_token stop) {
        std::array<float, 960> pcm;
        while (!stop.stop_requested()) {
            pcm.fill(0.25f); processVst(slot, pcm.data(), 480);
            for (float value : pcm)
                if (!std::isfinite(value) || (fixture && value != 0.125f)) valid = false;
            ++processed; std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    QPushButton control("VST");
    bindVstButton(&control, slot); showVstEditor(&control, slot);
    if (fixture) service.cancelScan();
    else ok &= require(until([&] { return !service.scanning(); }, 30000), "installed plug-in names are available");
    auto *window = control.findChild<QDialog *>("vstMASTERWindow");
    auto *combo = window ? window->findChild<QComboBox *>("vstPluginComboBox") : nullptr;
    auto *apply = window ? window->findChild<QPushButton *>("vstApply") : nullptr;
    ok &= require(combo && apply, "selector has an explicit Apply button");
    if (combo && apply) {
        combo->addItem("Rejected candidate", QJsonObject{{"path", path}, {"uid", badUid}});
        combo->setCurrentIndex(combo->count() - 1);
        const int before = processed.load();
        ok &= require(until([&] { return processed >= before + 10; }), "audio continues while only changing selection");
        ok &= require(!service.applying(slot) && service.selectedUid(slot) == goodUid && savedUid() == goodUid,
                      "dropdown alone changes neither processing nor saved selection");
        service.catalogChanged();
        ok &= require(combo->currentData().toJsonObject().value("uid").toString() == badUid,
                      "catalog refresh preserves unapplied selection");
        apply->click();
        ok &= require(service.applying(slot) && service.active(slot) && service.selectedUid(slot) == goodUid,
                      "Apply starts candidate while original stays active");
        ok &= require(!apply->isEnabled(), "repeated Apply is disabled while checking candidate");
        ok &= require(until([&] { return !service.applying(slot); }, 18000), "candidate reports initialization failure");
        ok &= require(rejected == 1 && stopped == 0 && !service.applyError(slot).isEmpty(),
                      "application failure is separate from stopping current audio");
        ok &= require(service.active(slot) && service.selectedUid(slot) == goodUid && savedUid() == goodUid
                      && runningPid() == originalPid && valid, "failure preserves original process, PCM, and saved selection");
        ok &= require(apply->isEnabled(), "failed candidate can be retried explicitly");
        QApplication::processEvents();
        window->grab().save(QCoreApplication::applicationDirPath() + "/vst-apply-failure-verified.png");
        // The original bypass flag must also survive a rejected replacement.
        audio.request_stop(); audio.join();
        service.bypass(slot, true);
        apply->click();
        ok &= require(until([&] { return !service.applying(slot); }, 18000) && service.bypassed(slot),
                      "failed replacement preserves original bypass state");
        service.bypass(slot, false);
    }
    audio.request_stop(); if (audio.joinable()) audio.join();
    if (fixture) {
        // Check a hung initializer while processing every old block, not just a
        // cached active flag. Candidate timeout must never retire the old worker.
        service.select(slot, path, hangUid);
        const auto before = rejected;
        ok &= require(until([&] {
            std::array<float, 960> pcm; pcm.fill(0.25f); processVst(slot, pcm.data(), 480);
            if (pcm.front() != 0.125f || pcm.back() != 0.125f) valid = false;
            return !service.applying(slot);
        }, 18000) && rejected == before + 1 && service.active(slot) && valid,
                      "startup timeout preserves every original PCM block");
        service.select(slot, path + ".missing", goodUid);
        ok &= require(until([&] { return !service.applying(slot); }) && savedUid() == goodUid && service.active(slot),
                      "module load failure leaves committed selection intact");
        service.select(slot, path, nextUid);
        ok &= require(service.applying(slot) && service.selectedUid(slot) == goodUid && savedUid() == goodUid,
                      "valid replacement remains provisional during startup");
        ok &= require(until([&] {
            if (!service.applying(slot)) return true;
            std::array<float, 960> pcm; pcm.fill(0.25f); processVst(slot, pcm.data(), 480);
            if (pcm.front() != 0.125f || pcm.back() != 0.125f) valid = false;
            return false;
        }) && valid && service.selectedUid(slot) == nextUid
                      && savedUid() == nextUid, "successful startup commits and saves the new selection");
        std::array<float, 960> pcm; pcm.fill(0.25f); processVst(slot, pcm.data(), 480);
        ok &= require(pcm.front() == 0.0625f && pcm.back() == 0.0625f, "only successful replacement changes audible PCM");
        service.select(slot, path, hangUid); service.setEnabled(false); service.setEnabled(true);
        ok &= require(!service.applying(slot) && service.selectedUid(slot) == nextUid && savedUid() == nextUid,
                      "disabling VST cancels pending work without saving it");
        service.activate(slot);
        ok &= require(until([&] { return !service.applying(slot) && service.active(slot); }), "last successful selection reactivates");
    }
    if (combo && apply) {
        combo->setCurrentIndex(0);
        ok &= require(service.active(slot), "selecting None alone keeps the current effect");
        apply->click();
        ok &= require(!service.active(slot) && service.selectedUid(slot).isEmpty() && savedUid().isEmpty(),
                      "None is committed only by Apply");
    }
    QObject::disconnect(rejectedConnection); QObject::disconnect(stoppedConnection);
    std::cout << "Transactional VST selection: " << (ok ? "passed" : "FAILED") << '\n';
    return ok;
}
class PcmStrip {
public:
    // Count selects the standalone EQ, which has no VST slot or audibility.
    PcmStrip(const char *name, VstSlot slot, bool audible, double lowDb = 0)
        : effects_(slot != VstSlot::Count) {
        GError *error = nullptr;
        const auto description = QStringLiteral("appsrc name=input format=time caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! %1 name=%2 ! appsink name=output sync=false async=false")
            .arg(effects_ ? QStringLiteral("vtrponaudioeffects") : QStringLiteral("vtrponaudioequalizer"),
                 QString::fromLatin1(name));
        pipeline_ = gst_parse_launch(description.toUtf8().constData(), &error);
        if (error) {
            std::cerr << error->message << '\n'; g_error_free(error);
            if (pipeline_) gst_object_unref(pipeline_);
            pipeline_ = nullptr; return;
        }
        if (!pipeline_) return;
        input_ = gst_bin_get_by_name(GST_BIN(pipeline_), "input");
        output_ = gst_bin_get_by_name(GST_BIN(pipeline_), "output");
        strip_ = gst_bin_get_by_name(GST_BIN(pipeline_), name);
        if (!input_ || !output_ || !strip_) return;
        if (effects_) {
            g_object_set(strip_, "slot", int(slot), "vst-audible", audible, nullptr);
            setAudioEffectEqualizer(strip_, lowDb, 0, 0);
        } else setAudioEqualizer(strip_, lowDb, 0, 0);
        ready_ = gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    }
    ~PcmStrip() {
        if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (strip_) gst_object_unref(strip_);
        if (input_) gst_object_unref(input_);
        if (output_) gst_object_unref(output_);
        if (pipeline_) gst_object_unref(pipeline_);
    }
    bool check(float expected, int warmupBlocks = 0, double tolerance = 0, bool gap = false) {
        if (!ready_) return false;
        for (int block = 0; block <= warmupBlocks; ++block) {
            std::array<float, 960> samples; samples.fill(0.25f);
            auto *buffer = gst_buffer_new_allocate(nullptr, sizeof(samples), nullptr);
            gst_buffer_fill(buffer, 0, samples.data(), sizeof(samples));
            GST_BUFFER_PTS(buffer) = offset_;
            GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
            offset_ += GST_BUFFER_DURATION(buffer);
            if (gap) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_GAP);
            GstFlowReturn flow = GST_FLOW_ERROR;
            g_signal_emit_by_name(input_, "push-buffer", buffer, &flow); gst_buffer_unref(buffer);
            GstSample *sample = nullptr;
            if (flow == GST_FLOW_OK)
                g_signal_emit_by_name(output_, "try-pull-sample", guint64(GST_SECOND), &sample);
            if (!sample) return false;
            auto *received = gst_sample_get_buffer(sample);
            const bool complete = gst_buffer_get_size(received) == sizeof(samples)
                && gst_buffer_extract(received, 0, samples.data(), sizeof(samples)) == sizeof(samples);
            gst_sample_unref(sample);
            if (!complete) return false;
            if (block == warmupBlocks)
                return std::all_of(samples.begin(), samples.end(), [=](float value) {
                    return std::isfinite(value) && std::abs(double(value) - expected) <= tolerance;
                });
        }
        return false;
    }
private:
    GstElement *pipeline_ = nullptr, *input_ = nullptr, *output_ = nullptr, *strip_ = nullptr;
    GstClockTime offset_ = 0;
    bool effects_ = false, ready_ = false;
};
bool checkStrip(const char *name, VstSlot slot, bool audible, float expected, double lowDb = 0) {
    return PcmStrip(name, slot, audible, lowDb).check(expected);
}
bool checkEffectRouting(VstService &service) {
    bool ok = true;
    ok &= require(checkStrip("renamed-edit", VstSlot::Edit, false, 0.125f, -12), "EDIT route is explicit and replaces non-flat EQ");
    ok &= require(checkStrip("renamed-master", VstSlot::Master, false, 0.125f, -12), "MASTER route is explicit and replaces non-flat EQ");
    ok &= require(checkStrip("renamed-deck-a", VstSlot::Item, true, 0.125f, -12), "ON AIR deck A uses explicit ITEM route");
    ok &= require(checkStrip("renamed-deck-b", VstSlot::Item, false, 0.25f, -12), "CUE bypasses ITEM instance and replacement EQ");
    ok &= require(checkStrip("renamed-deck-a", VstSlot::Item, false, 0.25f, -12), "outgoing deck bypasses ITEM instance");
    ok &= require(checkStrip("renamed-deck-b", VstSlot::Item, true, 0.125f, -12), "ON AIR deck B uses explicit ITEM route");
    ok &= require(PcmStrip("master-gap", VstSlot::Master, false).check(0, 0, 0, true), "VST receives silence for nonzero GAP payload");

    // Deliberately misleading former EQ names must not select a VST instance.
    service.bypass(VstSlot::Master, true);
    PcmStrip switched("edit-nominal-eq", VstSlot::Master, false, -12);
    ok &= require(switched.check(0.25f), "bypassed MASTER stays dry with a misleading EDIT name and non-flat EQ");
    ok &= require(checkStrip("program-audio-eq", VstSlot::Edit, false, 0.125f), "explicit EDIT ignores the former MASTER name");
    ok &= require(checkStrip("program-audio-eq", VstSlot::Item, true, 0.125f), "explicit ITEM ignores the former MASTER name");
    service.bypass(VstSlot::Master, false);
    service.bypass(VstSlot::Item, true);
    ok &= require(checkStrip("edit-nominal-eq", VstSlot::Item, true, 0.25f), "ITEM bypass does not route to EDIT by name");
    ok &= require(checkStrip("deck-a-item-eq", VstSlot::Edit, false, 0.125f), "EDIT remains active while ITEM is bypassed");
    service.bypass(VstSlot::Item, false);

    // A low shelf has its full gain at DC after its startup transient settles.
    const float equalized = float(0.25 * std::pow(10.0, -12.0 / 20.0));
    for (const auto *name : {"edit-nominal-eq", "program-audio-eq", "deck-a-item-eq", "deck-b-item-eq", "bgm-item-eq"})
        ok &= require(PcmStrip(name, VstSlot::Count, false, -12).check(equalized, 8, 0.00001),
                      "standalone EQ remains active in VST mode regardless of element name");
    ok &= require(switched.check(0.125f), "unbypass restores MASTER processing on the existing strip");
    ok &= require(service.setEnabled(false), "disable VST for EQ fallback");
    ok &= require(switched.check(equalized, 8, 0.00001), "global disable restores stored EQ on the existing strip");
    ok &= require(service.setEnabled(true), "re-enable VST after EQ fallback");
    for (const auto slot : {VstSlot::Edit, VstSlot::Item, VstSlot::Master}) service.activate(slot);
    ok &= require(until([&] {
        return (service.active(VstSlot::Edit) && !service.applying(VstSlot::Edit)) && (service.active(VstSlot::Item) && !service.applying(VstSlot::Item)) && (service.active(VstSlot::Master) && !service.applying(VstSlot::Master));
    }), "all slots reactivate after mode change");
    ok &= require(switched.check(0.125f), "global re-enable replaces stored EQ on the existing strip");
    return ok;
}
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::cerr << message.toStdString() << '\n';
    });
    QTranslator translator;
    if (translator.load(QCoreApplication::applicationDirPath() + "/vtrpon_ja.qm")) app.installTranslator(&translator);
    gst_init(nullptr, nullptr);
    if (!require(registerAudioEqualizer() && registerAudioEffects(), "register independent EQ and effect elements")) return 1;
    QTemporaryDir temporary;
    app.setProperty("vtrpon.settingsFile", temporary.filePath("settings.ini"));
    auto &service = VstService::instance();
    if (app.arguments().value(1) == "--scan-parent") return scanParent(service);
    if (app.arguments().value(1) == "--dual-editor") {
        const bool ok = checkDualEditors(service, app.arguments().value(2), app.arguments().value(3),
            app.arguments().value(4), app.arguments().value(5), app.arguments().value(6));
        service.setEnabled(false);
        return ok ? 0 : 1;
    }
    if (app.arguments().value(1) == "--switch") {
        service.setEnabled(true);
        const bool passed = checkSwitching(service, app.arguments().value(2), app.arguments().value(3),
            app.arguments().value(4), app.arguments().value(5), false);
        service.setEnabled(false);
        std::cout << "Installed plug-in continuous switching: " << (passed ? "passed" : "FAILED") << '\n';
        return passed ? 0 : 1;
    }
    if (app.arguments().value(1) == "--apply-installed") {
        const bool passed = checkApply(service, app.arguments().value(2), app.arguments().value(3), app.arguments().value(4));
        service.setEnabled(false); return passed ? 0 : 1;
    }
    if (app.arguments().value(1) == "--installed") {
        service.setEnabled(true);
        service.select(VstSlot::Master, app.arguments().value(2), app.arguments().value(3));
        if (!until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)) || service.statusKey(VstSlot::Master) == "vst.failed"; }, 18000)
            || !(service.active(VstSlot::Master) && !service.applying(VstSlot::Master))) {
            std::cerr << "Installed plug-in initialization: " << service.detail(VstSlot::Master).toStdString() << '\n'; return 1;
        }
        std::array<float, 960> audio{};
        for (int block = 0; block < 200; ++block) {
            for (int frame = 0; frame < 480; ++frame)
                audio[frame * 2] = audio[frame * 2 + 1] = float(0.1 * std::sin((block * 480 + frame) * 0.1));
            processVst(VstSlot::Master, audio.data(), 480);
            QApplication::processEvents(); QThread::msleep(10);
        }
        bool passed = until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)); }, 300);
        if (app.arguments().contains("--editor")) {
            QPushButton control("VST");
            control.setWindowTitle("VTR-PON3 VST editor test"); control.move(120, 120); control.show();
            bindVstButton(&control, VstSlot::Master);
            showVstEditor(&control, VstSlot::Master);
            passed &= require(until([&] { return !service.scanning(); }, 30000), "Controller catalog scan finishes");
            std::cout << "Controller catalog: " << service.plugins().size() << " effects\n";
            auto *selection = control.findChild<QDialog *>("vstMASTERWindow");
            passed &= require(selection != nullptr, "non-modal selector window");
            if (selection) {
                const auto *combo = selection->findChild<QComboBox *>("vstPluginComboBox");
                passed &= require(!combo->currentData().toJsonObject().value("name").toString().isEmpty(), "saved Windows path resolves to the plug-in display name");
                auto *bypass = selection->findChild<QCheckBox *>("vstBypass");
                bypass->setChecked(true);
                passed &= require(!control.property("vstActive").toBool(), "bypass UI clears green");
                bypass->setChecked(false);
                passed &= require(control.property("vstActive").toBool(), "bypass UI restores green");
                selection->grab().save(QCoreApplication::applicationDirPath() + "/vst-selector-verified.png");
            }
            struct Search { HWND owner; HWND found = nullptr; } search{reinterpret_cast<HWND>(control.winId())};
            const auto editorVisible = [&] {
                search.found = nullptr;
                EnumWindows([](HWND window, LPARAM data) -> BOOL {
                    auto &s = *reinterpret_cast<Search *>(data);
                    if (GetWindow(window, GW_OWNER) == s.owner && IsWindowVisible(window)) {
                        wchar_t title[512]{}; GetWindowTextW(window, title, 512);
                        if (QString::fromWCharArray(title).contains("VTR-PON3 VST")) { s.found = window; return FALSE; }
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&search));
                return search.found != nullptr;
            };
            if (selection) selection->findChild<QPushButton *>("vstOpen")->click();
            else service.openEditor(VstSlot::Master, quintptr(control.winId()), 160, 180);
            passed &= require(until(editorVisible), "native plug-in GUI opens with Controller owner");
            if (search.found) {
                RECT rect{}; GetClientRect(search.found, &rect);
                passed &= require(rect.right > 100 && rect.bottom > 100, "native editor has plug-in dimensions");
                PostMessageW(search.found, WM_CLOSE, 0, 0);
                passed &= require(until([&] { return !editorVisible(); }), "editor can close without stopping audio worker");
                service.openEditor(VstSlot::Master, quintptr(control.winId()), 160, 180);
                passed &= require(until(editorVisible), "native editor can reopen");
            }
            passed &= require((service.active(VstSlot::Master) && !service.applying(VstSlot::Master)), "closing GUI preserves processing");
            std::cout << "Installed plug-in native editor: " << (passed ? "passed" : "FAILED") << '\n';
        }
        std::cout << "Installed plug-in PCM: " << (passed ? "passed" : "FAILED") << "; latency " << service.latency(VstSlot::Master) << " samples; " << service.detail(VstSlot::Master).toStdString() << '\n';
        service.setEnabled(false); return passed ? 0 : 1;
    }
    const QString plugin = app.arguments().value(1);
    QProcess scanner;
    scanner.start(VstService::addonPath(), {"--scan", plugin, "--result", temporary.filePath("scan.json"),
        "--parent", QString::number(QCoreApplication::applicationPid())});
    if (!require(scanner.waitForFinished(10000) && scanner.exitCode() == 0, "isolated SDK scan")) return 1;
    QFile file(temporary.filePath("scan.json")); file.open(QIODevice::ReadOnly);
    const auto catalog = QJsonDocument::fromJson(file.readAll()).array();
    if (!require(catalog.size() == 9, "enumerate every class in one module")) return 1;
    if (app.arguments().contains("--apply-only")) {
        const bool passed = checkApply(service, plugin, catalog[0].toObject().value("uid").toString(),
            catalog[6].toObject().value("uid").toString(), catalog[7].toObject().value("uid").toString(),
            catalog[8].toObject().value("uid").toString());
        service.setEnabled(false); return passed ? 0 : 1;
    }
    bool ok = checkScannerParentDeath(plugin, temporary.filePath("scan-module"), false);
    ok &= checkScannerParentDeath(plugin, temporary.filePath("scan-loader"), true);
    scanner.start(VstService::addonPath(), {"--scan", plugin, "--result", temporary.filePath("unowned-scan.json")});
    ok &= require(scanner.waitForFinished(10000) && scanner.exitCode() == 4
        && !QFile::exists(temporary.filePath("unowned-scan.json")), "scan rejects missing parent before loading vendor code");
    ok &= require(service.setEnabled(true), "enable add-on");
    QPushButton button;
    bindVstButton(&button, VstSlot::Master);
    ok &= require(button.text() == "VST" && !button.property("vstActive").toBool(), "button reflects stopped state");
    std::array<float, 960> pcm{};
    const auto gainUid = catalog[0].toObject().value("uid").toString();
    ok &= checkDualEditors(service, plugin, gainUid, QStringLiteral("VTR Test Gain"),
        catalog[5].toObject().value("uid").toString(), QStringLiteral("VTR Test Burst"));
    ok &= checkSwitching(service, plugin, gainUid, plugin, catalog[5].toObject().value("uid").toString(), true);
    ok &= checkApply(service, plugin, catalog[0].toObject().value("uid").toString(),
        catalog[6].toObject().value("uid").toString(), catalog[7].toObject().value("uid").toString(),
        catalog[8].toObject().value("uid").toString());
    for (const auto slot : {VstSlot::Edit, VstSlot::Item, VstSlot::Master}) {
        service.select(slot, plugin, gainUid);
        ok &= require(until([&] { return (service.active(slot) && !service.applying(slot)); }), "plug-in initialization");
        pcm.fill(0.25f); processVst(slot, pcm.data(), pcm.size() / 2);
        ok &= require(pcm[0] == 0.125f && pcm.back() == 0.125f, "actual PCM processing through SDK");
        service.bypass(slot, true); pcm.fill(0.25f); processVst(slot, pcm.data(), pcm.size() / 2);
        ok &= require(pcm[0] == 0.25f && !(service.active(slot) && !service.applying(slot)), "dry bypass");
    }
    service.bypass(VstSlot::Master, false);
    std::array<float, 8192 * 2> largeBuffer; largeBuffer.fill(0.25f);
    processVst(VstSlot::Master, largeBuffer.data(), 8192);
    ok &= require(largeBuffer.front() == 0.125f && largeBuffer.back() == 0.125f, "large decoder buffers are split into VST blocks");
    service.bypass(VstSlot::Edit, false); service.bypass(VstSlot::Item, false);
    ok &= checkEffectRouting(service);
    ok &= require(button.property("vstActive").toBool(), "green only when running");
    int failures = 0;
    QObject::connect(&service, &VstService::failure, &app, [&](const QString &, const QString &) { ++failures; });
    for (int i = 1; i < 4; ++i) {
        service.select(VstSlot::Master, plugin, catalog[i].toObject().value("uid").toString());
        ok &= require(until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)); }), "fault plug-in initialization");
        pcm.fill(0.25f); QElapsedTimer deadline; deadline.start();
        if (i == 1) {
            largeBuffer.fill(0.1f); largeBuffer[4096 * 2] = 0.25f;
            const auto original = largeBuffer;
            processVst(VstSlot::Master, largeBuffer.data(), 8192);
            ok &= require(largeBuffer == original, "later-block crash preserves earlier dry samples too");
        } else processVst(VstSlot::Master, pcm.data(), pcm.size() / 2);
        ok &= require(deadline.elapsed() < 100, "fault never blocks playback indefinitely");
        ok &= require(pcm[0] == 0.25f && pcm.back() == 0.25f, "fault preserves complete dry buffer");
        ok &= require(until([&] { return service.statusKey(VstSlot::Master) == "vst.failed"; }), "watchdog disables faulted plugin");
        ok &= require(!button.property("vstActive").toBool(), "fault clears green state");
        ok &= require(checkStrip("renamed-master", VstSlot::Master, false, 0.25f), "streaming continues after plug-in fault");
        ok &= require(checkStrip("renamed-edit", VstSlot::Edit, false, 0.125f), "other fader plug-in remains operational after master fault");
        // Repeated calls after timeout must neither consume late output nor wait.
        for (int j = 0; j < 50; ++j) processVst(VstSlot::Master, pcm.data(), pcm.size() / 2);
        ok &= require(pcm[0] == 0.25f, "continued playback stays dry");
    }
    ok &= require(failures == 3, "one failure notification per failed instance");
    service.select(VstSlot::Master, plugin, catalog[4].toObject().value("uid").toString());
    ok &= require(until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)); }), "UI hang plug-in initialization");
    service.openEditor(VstSlot::Master, 0, 100, 100);
    ok &= require(until([&] { return service.statusKey(VstSlot::Master) == "vst.failed"; }, 4000), "idle UI heartbeat timeout without audio requests");
    ok &= require(failures == 4, "UI hang emits one failure notification");
    service.select(VstSlot::Master, plugin, gainUid);
    ok &= require(until([&] { return (service.active(VstSlot::Master) && !service.applying(VstSlot::Master)); }), "manual recovery after crash");
    service.setEnabled(false);
    pcm.fill(0.25f); processVst(VstSlot::Master, pcm.data(), pcm.size() / 2);
    ok &= require(pcm[0] == 0.25f && button.text() == "EQ", "global disable restores EQ and dry routing");
    std::cout << (ok ? "VST3 isolation / PCM / bypass / crash / hang / NaN / recovery passed\n" : "VST3 isolation FAILED\n");
    return ok ? 0 : 1;
}
