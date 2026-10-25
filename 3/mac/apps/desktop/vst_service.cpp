#include "platform_capabilities.h"
#include "vst_service.h"
#include "vst_protocol.h"
#include "vst_state.h"
#include "vst_deferred_release.h"
#ifdef __APPLE__
#include "vst_transport_mac.h"
#include "addon_platform.h"
#endif
#include "vst_scan.h"
#include "controller_settings.h"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace vtrpon {
namespace {
#ifdef __APPLE__
using AudioClock = vst::MacTransport::Clock;
#else
using AudioClock = std::chrono::steady_clock;
#endif
constexpr int count = int(VstSlot::Count);
const std::array<QString, count> names{QStringLiteral("EDIT"), QStringLiteral("ITEM"), QStringLiteral("MASTER")};
std::atomic_bool mode{false};
struct Bridge {
    std::mutex processing;
    std::atomic_bool failed{false}, bypass{false};
    enum class Fault { None, OversizedBuffer, Worker, Deadline, Transport, NonFinite };
    std::atomic<Fault> fault{Fault::None};
    std::atomic_int lastFrames{0}, lastBudgetMs{0};
    std::atomic_int bufferMaxUs{0}, roundTripMaxUs{0}, lastRoundTripUs{0}, completedFrames{0}, recoveredReplies{0};
    std::atomic<int64_t> waitReturnedNs{0};
    // Some decoders deliver more than one VST block in a GstBuffer. Stage all
    // output so a failure in a later block still returns the entire dry buffer.
    std::array<float, 65536 * 2> staged{};
#ifdef _WIN32
    HANDLE mapping = nullptr, request = nullptr, response = nullptr;
    ~Bridge() {
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        if (request) CloseHandle(request);
        if (response) CloseHandle(response);
    }
#elif defined(__APPLE__)
    vst::MacTransport transport;
#endif
    vst::Shared *shared = nullptr;
};
// Use the shared_ptr atomic functions also supported by Apple libc++.
std::array<std::shared_ptr<Bridge>, count> bridges;
QString key(int slot, const char *field) {
    return QStringLiteral("audio/vst/%1/%2").arg(names[slot].toLower(), QString::fromLatin1(field));
}
void dispose(QProcess *process) {
    if (!process) return;
    process->disconnect();
    process->setParent(QCoreApplication::instance());
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    QObject::connect(process, &QProcess::started, process, &QProcess::kill);
    process->kill();
    if (process->state() == QProcess::NotRunning) process->deleteLater();
}
void configureProcess(QProcess *process) {
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
#ifdef _WIN32
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
}
}

bool vstModeEnabled() { return mode.load(std::memory_order_relaxed); }
void processVst(VstSlot slot, float *samples, size_t frames) {
#if defined(_WIN32) || defined(__APPLE__)
    auto bridge = std::atomic_load(&bridges[int(slot)]);
    if (!vstModeEnabled() || !bridge || bridge->failed || bridge->bypass) return;
    std::unique_lock lock(bridge->processing, std::try_to_lock);
    if (!lock.owns_lock() || bridge->failed) return;
    auto *shared = bridge->shared;
    if (!shared || !vst::load(&shared->ready)) return;
    if (frames > bridge->staged.size() / 2 || vst::load(&shared->failed)) {
        bridge->fault = frames > bridge->staged.size() / 2 ? Bridge::Fault::OversizedBuffer : Bridge::Fault::Worker;
        bridge->failed = true;
        return;
    }
    if (!frames) return;
    // This is the only wait in the audio path. No Qt, pipe I/O, allocation or
    // plug-in teardown here. A missed deadline permanently retires this bridge.
    // Neural effects batch several input blocks internally: a 10 ms input can
    // take over 20 ms during normal processing under video load. Use the same
    // bounded scheduling margin for small and large buffers. This deadline
    // covers the entire buffer, not each sub-block separately.
    constexpr int budgetMs = 50;
    bridge->lastFrames = int(frames); bridge->lastBudgetMs = budgetMs;
    const auto started = AudioClock::now();
    struct BufferTiming {
        Bridge &bridge; AudioClock::time_point start;
        ~BufferTiming() {
            const auto us = int(std::chrono::duration_cast<std::chrono::microseconds>(AudioClock::now() - start).count());
            if (us > bridge.bufferMaxUs.load()) bridge.bufferMaxUs = us;
        }
    } timing{*bridge, started};
    bridge->completedFrames = 0;
    const auto deadline = started + std::chrono::milliseconds(budgetMs);
    for (size_t offset = 0; offset < frames;) {
        const size_t block = std::min(frames - offset, size_t(vst::kFrames));
        shared->frames = uint32_t(block);
        std::memcpy(shared->input, samples + offset * 2, block * 2 * sizeof(float));
        const auto requestStarted = AudioClock::now();
#ifdef _WIN32
        ResetEvent(bridge->response);
        SetEvent(bridge->request);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - AudioClock::now()).count();
        const DWORD result = remaining <= 0 ? WAIT_TIMEOUT : WaitForSingleObject(bridge->response, DWORD(remaining));
        const bool responded = result == WAIT_OBJECT_0;
        const bool timedOut = result == WAIT_TIMEOUT;
#else
        bridge->transport.markRequest();
        const bool sent = bridge->transport.signal();
        const auto result = sent ? bridge->transport.waitForResponse(deadline) : vst::MacTransport::Result::Failed;
        bridge->waitReturnedNs = vst::MacTransport::nowNs();
        const bool recovered = result == vst::MacTransport::Result::ReadyAfterCallerDelay;
        if (recovered) ++bridge->recoveredReplies;
        const bool responded = result == vst::MacTransport::Result::Ready || recovered;
        const bool timedOut = result == vst::MacTransport::Result::Timeout;
#endif
        const auto roundTripUs = int(std::chrono::duration_cast<std::chrono::microseconds>(AudioClock::now() - requestStarted).count());
        bridge->lastRoundTripUs = roundTripUs;
        if (roundTripUs > bridge->roundTripMaxUs.load()) bridge->roundTripMaxUs = roundTripUs;
        if (!responded || vst::load(&shared->failed)) {
            bridge->fault = timedOut ? Bridge::Fault::Deadline : !responded
                ? Bridge::Fault::Transport : Bridge::Fault::Worker;
            bridge->failed = true;
            return; // Preserve the entire dry buffer, including on partial failure.
        }
        for (size_t i = 0; i < block * 2; ++i) {
            if (!std::isfinite(shared->output[i])) { bridge->fault = Bridge::Fault::NonFinite; bridge->failed = true; return; }
        }
        std::memcpy(bridge->staged.data() + offset * 2, shared->output, block * 2 * sizeof(float));
        offset += block;
        bridge->completedFrames = int(offset);
    }
    std::memcpy(samples, bridge->staged.data(), frames * 2 * sizeof(float));
#else
    Q_UNUSED(slot); Q_UNUSED(samples); Q_UNUSED(frames);
#endif
}

struct VstService::Impl {
    VstService *q;
    struct Slot {
        QString path, uid, status = QStringLiteral("vst.inactive"), detail, applyError;
        QProcess *process = nullptr;
        std::shared_ptr<Bridge> bridge;
        QElapsedTimer startup, heartbeatTimer;
        long heartbeat = 0;
        long editorStatus = 0;
        int reportedRecoveries = 0;
        bool bypass = false;
        std::shared_ptr<QTemporaryDir> stateDirectory;
        QString savedStateFile, stateError;
        int stateSerial = 0, stateRequest = 0, stateAcknowledged = 0;
        bool stateEligible = false, resetSavedState = false;
        QElapsedTimer retirement;

    };
    std::array<Slot, count> channels;
    std::array<Slot, count> candidates;
    std::vector<Slot> retired;
    vst::DeferredRelease<Bridge> closedBridges;
    QTimer timer, scanPoll;
    std::shared_ptr<VstScan> scanState;
    VstScan::Snapshot scanSnapshot;
    QJsonArray catalog;
    bool scanBusy = false, scanRequested = false, scanCanceled = false;
    explicit Impl(VstService *owner) : q(owner) {}
    static QString statePath(const Slot &s, const char *name) {
        return s.stateDirectory ? s.stateDirectory->filePath(QString::fromLatin1(name)) : QString();
    }
    static void requestState(Slot &s) {
        if (!s.stateDirectory || !s.stateEligible) return;
        if (!vststate::write(statePath(s, "request.json"), {{"request", ++s.stateRequest}}))
            s.stateError = QStringLiteral("Could not request a plug-in state checkpoint.");
    }
    static QString collectState(Slot &s) {
        if (!s.stateDirectory || !s.stateEligible) return {};
        const auto status = vststate::read(statePath(s, "status.json"), 4096);
        s.stateAcknowledged = status.value("request").toInt();
        const int serial = status.value("serial").toInt();
        if (serial > s.stateSerial) {
            const auto snapshot = vststate::read(statePath(s, "snapshot.json"));
            if (snapshot.value("serial").toInt() != serial) return {}; // Next atomic snapshot is being published.
            const auto directory = QFileInfo(s.savedStateFile).absolutePath();
            if (!vststate::valid(snapshot, s.path, s.uid)) {
                s.stateError = QStringLiteral("The plug-in produced an invalid state snapshot.");
            } else if (!QDir().mkpath(directory) || !vststate::write(s.savedStateFile, snapshot)) {
                s.stateError = QStringLiteral("Could not save the plug-in adjustments. Check free disk space and folder permissions.");
            } else {
                QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
                s.stateSerial = serial; s.stateError.clear();
                qInfo().noquote() << "VST_STATE_SAVED" << s.path << s.uid << "serial=" << serial;
            }
        }
        if (status.contains("ok") && !status.value("ok").toBool()) s.stateError = status.value("error").toString();
        else if (serial && serial == s.stateSerial) s.stateError.clear();
        return s.stateError;
    }
    bool saveAll() {
        const auto each = [&](const auto &operation) {
            for (auto &s : channels) operation(s);
            for (auto &s : retired) operation(s);
        };
        each([](Slot &s) { requestState(s); });
        QElapsedTimer deadline; deadline.start();
        bool pending;
        do {
            pending = false;
            each([&](Slot &s) {
                collectState(s);
                pending |= s.stateEligible && s.stateDirectory && s.stateAcknowledged < s.stateRequest;
            });
            if (pending) QThread::msleep(10);
        } while (pending && deadline.elapsed() < 1500);
        bool ok = !pending;
        each([&](Slot &s) {
            if (!s.stateError.isEmpty() || (s.stateEligible && s.stateDirectory && s.stateAcknowledged < s.stateRequest)) {
                ok = false;
                qWarning().noquote() << "VST_STATE_SAVE_FAILURE" << s.path << s.uid
                    << (s.stateError.isEmpty() ? QStringLiteral("Checkpoint exceeded 1500 ms; retaining the last saved state.") : s.stateError);
            }
        });
        return ok;
    }
    void retire(Slot previous) {
        if (!previous.bridge) { stopInstance(previous); return; }
        requestState(previous);
        previous.retirement.start();
        retired.push_back(std::move(previous));
        timer.start();
    }
    void stopInstance(Slot &s) {
        if (s.bridge) s.bridge->failed = true;
        s.stateEligible = false;
        closedBridges.retire(s.bridge);
        if (!closedBridges.empty()) timer.start();
        dispose(s.process); s.process = nullptr;
    }
    void updateTimer() {
        const auto running = [](const auto &s) { return bool(s.bridge); };
        if (retired.empty() && closedBridges.empty() && std::none_of(channels.cbegin(), channels.cend(), running)
            && std::none_of(candidates.cbegin(), candidates.cend(), running))
            timer.stop();
    }
    void stop(int i) {
        std::atomic_store(&bridges[i], std::shared_ptr<Bridge>{});
#ifdef __APPLE__
        auto previous = std::move(channels[i]);
        channels[i] = {};
        channels[i].path = previous.path; channels[i].uid = previous.uid;
        retire(std::move(previous));
#else
        stopInstance(channels[i]);
#endif
        stopInstance(candidates[i]);
        candidates[i] = {};
        channels[i].applyError.clear();
        updateTimer();
    }
    void collectRetired() {
        for (auto it = retired.begin(); it != retired.end();) {
            // Let an in-flight block finish on the previous plug-in. Never wait
            // on the UI thread or kill that process in the middle of its block.
            collectState(*it);
            if (it->stateDirectory && it->stateEligible && it->stateAcknowledged < it->stateRequest
                && it->retirement.elapsed() < 1500) { ++it; continue; }
            auto bridge = it->bridge;
            std::unique_lock lock(bridge->processing, std::try_to_lock);
            if (!lock.owns_lock()) { ++it; continue; }
            stopInstance(*it);
            it = retired.erase(it);
        }
        closedBridges.collect();
        updateTimer();
    }
    void startCandidate(int i, QProcess *process, const QStringList &arguments) {
        auto &s = candidates[i];
        if (s.process != process) return;
        collectRetired();
        // A quick None -> same plug-in (or A -> B -> A) must wait for the
        // previous instance's final checkpoint before copying its input state.
        if (s.stateDirectory && std::any_of(retired.cbegin(), retired.cend(), [&](const Slot &old) {
                return old.stateEligible && old.savedStateFile == s.savedStateFile
                    && old.stateAcknowledged < old.stateRequest && old.retirement.elapsed() < 1500;
            })) {
            QTimer::singleShot(50, q, [this, i, process, arguments] { startCandidate(i, process, arguments); });
            return;
        }
        if (s.stateDirectory && !s.resetSavedState && QFileInfo::exists(s.savedStateFile)
            && (QFileInfo(s.savedStateFile).size() > vststate::maxFileBytes
                || !QFile::copy(s.savedStateFile, statePath(s, "input.json")))) {
            fail(i, QStringLiteral("Could not prepare the saved plug-in state."), true); return;
        }
        process->start(VstService::addonPath(), arguments);
    }
    void saveSelection(int i) {
        auto &s = channels[i];
        auto settings = makeControllerSettings();
        settings.setValue(key(i, "path"), s.path);
        settings.setValue(key(i, "uid"), s.uid);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            s.detail = QStringLiteral("Could not save the plug-in selection.");
    }
    void commit(int i) {
        auto previous = std::move(channels[i]);
        channels[i] = std::move(candidates[i]);
        candidates[i] = {};
        auto &s = channels[i];
        s.status = QStringLiteral("vst.active");
        std::atomic_store(&bridges[i], s.bridge);
        s.stateEligible = true;
        // A successful reset replaces the old state only after the new instance
        // is ready. The retiring instance must not subsequently overwrite it.
        if (s.resetSavedState && previous.savedStateFile == s.savedStateFile) previous.stateEligible = false;
        retire(std::move(previous));
        saveSelection(i);
        qInfo().noquote() << "VST_APPLIED" << names[i] << "path=" << s.path << "class=" << s.uid;
    }
    void fail(int i, const QString &reason, bool candidate = false) {
        auto &s = candidate ? candidates[i] : channels[i];
        qWarning().noquote() << (candidate ? "VST_APPLY_FAILURE" : "VST_FAILURE")
            << names[i] << "path=" << s.path << "class=" << s.uid << reason;
#if defined(_WIN32) || defined(__APPLE__)
        if (s.bridge) {
            auto *shared = s.bridge->shared;
            qWarning() << "VST_TIMING warmupMaxUs=" << vst::load(&shared->warmupMaxUs)
                << "processMaxUs=" << vst::load(&shared->processMaxUs)
                << "processedBlocks=" << vst::load(&shared->processedBlocks)
                << "bufferFrames=" << s.bridge->lastFrames.load() << "budgetMs=" << s.bridge->lastBudgetMs.load()
                << "bufferMaxUs=" << s.bridge->bufferMaxUs.load() << "roundTripMaxUs=" << s.bridge->roundTripMaxUs.load()
                << "lastRoundTripUs=" << s.bridge->lastRoundTripUs.load() << "completedFrames=" << s.bridge->completedFrames.load();
#ifdef __APPLE__
            const auto timestamps = s.bridge->transport.timing();
            const auto deltaUs = [](int64_t begin, int64_t end) { return begin && end >= begin ? (end-begin)/1000 : -1; };
            qWarning() << "VST_WAIT_TIMING workerWakeUs=" << deltaUs(timestamps.requestNs, timestamps.startedNs)
                << "workerUs=" << deltaUs(timestamps.startedNs, timestamps.finishedNs)
                << "parentAfterWorkerUs=" << deltaUs(timestamps.finishedNs, s.bridge->waitReturnedNs.load());
#endif
        }
#endif
        if (candidate) {
            stopInstance(s);
            s = {};
            channels[i].applyError = reason;
            emit q->applicationFailed(names[i], reason);
        } else {
            std::atomic_store(&bridges[i], std::shared_ptr<Bridge>{});
            collectState(s); // Preserve its last completed snapshot, never wait on a failed vendor.
            stopInstance(s);
            s.status = QStringLiteral("vst.failed");
            s.detail = reason;
            emit q->failure(names[i], reason);
        }
        updateTimer();
        emit q->changed();
    }
    void processFailed(int i, QProcess *process, const QString &reason) {
        auto detail = reason;
        const auto appendStateError = [&](const Slot &s) {
            if (!s.stateDirectory) return;
            const auto error = vststate::read(statePath(s, "status.json"), 4096).value("error").toString();
            if (!error.isEmpty()) detail += QLatin1Char('\n') + error;
        };
        if (candidates[i].process == process) { appendStateError(candidates[i]); fail(i, detail, true); }
        else if (channels[i].process == process) { appendStateError(channels[i]); fail(i, detail); }
    }
    bool poll(int i, bool candidate) {
#if defined(_WIN32) || defined(__APPLE__)
        auto &s = candidate ? candidates[i] : channels[i];
        if (!s.bridge) return false;
        auto *shared = s.bridge->shared;
        if (s.bridge->failed || vst::load(&shared->failed)) {
            QString reason;
            switch (s.bridge->fault.load()) {
            case Bridge::Fault::Deadline: reason = QStringLiteral("Audio processing exceeded its %1 ms deadline.").arg(s.bridge->lastBudgetMs.load()); break;
            case Bridge::Fault::NonFinite: reason = QStringLiteral("The plug-in returned invalid audio (NaN/Inf)."); break;
            case Bridge::Fault::OversizedBuffer: reason = QStringLiteral("The audio buffer exceeds the supported size."); break;
            case Bridge::Fault::Transport: reason = QStringLiteral("The VST audio transport failed."); break;
            default: reason = QStringLiteral("The plug-in reported an audio processing error."); break;
            }
            fail(i, reason, candidate); return false;
        }
        const long heartbeat = vst::load(&shared->heartbeat);
        if (s.heartbeat != heartbeat) { s.heartbeat = heartbeat; s.heartbeatTimer.restart(); }
        const bool ready = vst::load(&shared->ready);
        if ((!ready && s.startup.elapsed() > 15000) || (ready && s.heartbeatTimer.elapsed() > 2000)) {
            fail(i, QStringLiteral("The plug-in stopped responding."), candidate); return false;
        }
        if (candidate) {
            if (ready) {
                if (s.stateDirectory && vststate::read(statePath(s, "status.json"), 4096).value("version").toInt() != 1) {
                    fail(i, QStringLiteral("Update the VST3 add-on to a version that supports saved adjustments."), true); return false;
                }
                commit(i); return true;
            }
            return false;
        }
        bool changed = false;
#ifdef __APPLE__
        const int recovered = s.bridge->recoveredReplies.load();
        if (recovered != s.reportedRecoveries) {
            s.reportedRecoveries = recovered;
            qInfo() << "VST_WAIT_RECOVERED" << names[i] << "timelyReplies=" << recovered
                << "roundTripMaxUs=" << s.bridge->roundTripMaxUs.load();
        }
#endif
        const auto previousError = s.stateError;
        const auto error = collectState(s);
        if (previousError != error) {
            if (!error.isEmpty() || s.detail == previousError) s.detail = error;
            if (!error.isEmpty()) qWarning().noquote() << "VST_STATE_SAVE_FAILURE" << names[i] << error;
            changed = true;
        }
        const long editorStatus = vst::load(&shared->editorStatus);
        if (editorStatus != s.editorStatus) {
            s.editorStatus = editorStatus;
            if (editorStatus) qInfo().noquote() << "VST_EDITOR_RESULT" << names[i]
                << "status=" << editorStatus << "pid=" << s.process->processId();
        }
        if (editorStatus < 0 && s.detail.isEmpty()) {
            s.detail = QStringLiteral("This plug-in does not provide a compatible editor for this platform."); changed = true;
        }
        const QString state = s.bypass ? QStringLiteral("vst.bypassed") : QStringLiteral("vst.active");
        if (s.status != state) { s.status = state; changed = true; }
        return changed;
#else
        Q_UNUSED(i); Q_UNUSED(candidate); return false;
#endif
    }
};

VstService &VstService::instance() {
    static auto *service = new VstService;
    return *service;
}
QString VstService::addonPath() {
#ifdef __APPLE__
    const auto override = qEnvironmentVariable("VTRPON_VST3_ADDON_FILE");
    return override.isEmpty() ? addons::addonFilePath(QCoreApplication::applicationDirPath(),
        QStringLiteral("vtr-pon-vst3-addon")) : override;
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/audio-addons/vtr-pon-vst3-addon.exe");
#endif
}
bool VstService::available() const {
    if (!platformFeatureImplemented(PlatformFeature::Vst3)) return false;
    // Add-on installation is fixed for this application session.
    static const bool installed = [] {
#ifdef __APPLE__
        QString error;
        const bool valid = QFileInfo(addonPath()).isExecutable() && addons::verifyInstalledBinary(addonPath(), &error);
        if (!valid && !error.isEmpty()) qWarning().noquote() << "VST_ADDON_UNAVAILABLE" << error;
        return valid;
#else
        return QFileInfo(addonPath()).isFile();
#endif
    }();
    return installed;
}
VstService::VstService() : QObject(QCoreApplication::instance()), d(std::make_unique<Impl>(this)) {
    reloadSettings();
    d->scanPoll.setInterval(100);
    connect(&d->scanPoll, &QTimer::timeout, this, [this] {
        if (!d->scanState) return;
        const auto snapshot = d->scanState->snapshot();
        if (snapshot == d->scanSnapshot) return;
        const bool catalogChangedState = snapshot.catalog != d->catalog;
        d->scanSnapshot = snapshot;
        d->catalog = snapshot.catalog;
        if (snapshot.done) {
            d->scanState.reset(); d->scanPoll.stop(); d->scanBusy = false;
            if (!snapshot.error.isEmpty()) qWarning().noquote() << "VST_SCAN_FAILURE" << snapshot.error;
        }
        if (catalogChangedState) emit catalogChanged();
        emit scanProgressChanged();
    });
    connect(&d->timer, &QTimer::timeout, this, [this] {
#if defined(_WIN32) || defined(__APPLE__)
        bool changedState = false;
        for (int i = 0; i < count; ++i) {
            changedState |= d->poll(i, false);
            changedState |= d->poll(i, true);
        }
        d->collectRetired();
        if (changedState) emit changed();
#endif
    });
    d->timer.setParent(this);
    d->timer.setObjectName(QStringLiteral("vstMonitorTimer"));
    d->timer.setInterval(100);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        mode = false;
        d->saveAll();
        for (int i = 0; i < count; ++i) d->stop(i);
        for (auto &s : d->retired) d->stopInstance(s);
        d->retired.clear();
        cancelScan();
    });
}
VstService::~VstService() {
    mode = false;
    d->saveAll();
    for (int i = 0; i < count; ++i) d->stop(i);
    for (auto &s : d->retired) d->stopInstance(s);
    d->retired.clear();
    cancelScan();
}
void VstService::reloadSettings() {
    auto settings = makeControllerSettings();
    const bool enabled = settings.value(QStringLiteral("audio/useVstPlugins"), false).toBool() && available();
    mode = enabled;
    for (int i = 0; i < count; ++i) {
        const auto path = settings.value(key(i, "path")).toString();
        const auto uid = settings.value(key(i, "uid")).toString();
        auto &s = d->channels[i];
        if (!enabled || s.path != path || s.uid != uid) { d->stop(i); s.status = QStringLiteral("vst.inactive"); }
        s.path = path; s.uid = uid;
    }
    emit changed();
}
bool VstService::setEnabled(bool enabled) {
    if (!platformFeatureImplemented(PlatformFeature::Vst3)) return false;
    if (enabled && !available()) return false;
    auto settings = makeControllerSettings();
    settings.setValue(QStringLiteral("audio/useVstPlugins"), enabled); settings.sync();
    if (settings.status() != QSettings::NoError) return false;
    reloadSettings(); return true;
}
void VstService::scan() {
    if (d->scanBusy || !available()) return;
    QStringList roots;
#ifdef __APPLE__
    roots = {QStringLiteral("/Library/Audio/Plug-Ins/VST3"),
             QDir::homePath() + QStringLiteral("/Library/Audio/Plug-Ins/VST3")};
#else
    const auto common = qEnvironmentVariable("CommonProgramW6432");
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    if (!common.isEmpty()) roots.append(common + QStringLiteral("/VST3"));
    if (!local.isEmpty()) roots.append(local + QStringLiteral("/Programs/Common/VST3"));
#endif
    d->catalog = {}; d->scanSnapshot = {};
    d->scanBusy = d->scanRequested = true; d->scanCanceled = false;
    d->scanState = VstScan::start(addonPath(), roots, QCoreApplication::applicationPid());
    d->scanPoll.start();
    emit catalogChanged(); emit scanProgressChanged();
}
void VstService::cancelScan() {
    if (!d->scanState) return;
    d->scanState->cancel();
    d->scanSnapshot = d->scanState->snapshot();
    d->catalog = d->scanSnapshot.catalog;
    d->scanState.reset(); d->scanPoll.stop();
    d->scanBusy = false; d->scanCanceled = true;
    emit catalogChanged(); emit scanProgressChanged();
}
bool VstService::scanning() const { return d->scanBusy; }
bool VstService::catalogRequested() const { return d->scanRequested; }
VstScanProgress VstService::scanProgress() const {
    const auto &s = d->scanSnapshot;
    return {s.currentPath, s.error, s.checked, s.total, s.skipped, s.discovering, d->scanCanceled};
}
QJsonArray VstService::plugins() const { return d->catalog; }
QString VstService::selectedPath(VstSlot slot) const { return d->channels[int(slot)].path; }
QString VstService::selectedUid(VstSlot slot) const { return d->channels[int(slot)].uid; }
bool VstService::applying(VstSlot slot) const { return bool(d->candidates[int(slot)].bridge); }
QString VstService::applyError(VstSlot slot) const { return d->channels[int(slot)].applyError; }
bool VstService::saveStates() { return d->saveAll(); }
void VstService::select(VstSlot slot, const QString &path, const QString &uid, bool resetSavedState) {
    const int i = int(slot);
    if (!vstModeEnabled() || !available()) return;
    qInfo().noquote() << "VST_APPLY" << names[i] << "path=" << path << "class=" << uid;
    auto &current = d->channels[i];
    auto &s = d->candidates[i];
    d->stopInstance(s); s = {};
    current.applyError.clear();
    if (path.isEmpty() && uid.isEmpty()) {
        d->stop(i); current = {}; d->saveSelection(i);
        emit changed(); return;
    }
    // Reapplying the running instance must preserve its parameter state.
    if (!resetSavedState && current.bridge && !current.bridge->failed && current.path == path && current.uid == uid) {
        bypass(slot, false); d->updateTimer(); return;
    }
    s.path = path; s.uid = uid;
#ifdef __APPLE__
    s.resetSavedState = resetSavedState;
    s.stateDirectory = std::make_shared<QTemporaryDir>();
    s.savedStateFile = vststate::savedFile(makeControllerSettings().fileName(), names[i].toLower(), path, uid);
    if (!s.stateDirectory->isValid()) {
        d->fail(i, QStringLiteral("Could not prepare the saved plug-in state."), true); return;
    }
#endif
#if defined(_WIN32) || defined(__APPLE__)
    auto bridge = std::make_shared<Bridge>();
#ifdef _WIN32
    const QString id = QStringLiteral("Local\\VtrPonVst-") + QUuid::createUuid().toString(QUuid::Id128);
    bridge->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vst::Shared), reinterpret_cast<LPCWSTR>(id.utf16()));
    if (bridge->mapping) bridge->shared = static_cast<vst::Shared *>(MapViewOfFile(bridge->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vst::Shared)));
    bridge->request = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-request").utf16()));
    bridge->response = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-response").utf16()));
    if (!bridge->shared || !bridge->request || !bridge->response) { d->fail(i, QStringLiteral("Could not create VST audio transport."), true); return; }
    new (bridge->shared) vst::Shared;
#else
    const QString id = QStringLiteral("fd:0,1");
    const auto name = (QStringLiteral("/vtrvst-") + QUuid::createUuid().toString(QUuid::Id128).left(20)).toUtf8();
    if (!bridge->transport.create(name.constData())) { d->fail(i, QStringLiteral("Could not create VST audio transport."), true); return; }
    bridge->shared = bridge->transport.shared;
#endif
    s.bridge = bridge; s.startup.start(); s.heartbeatTimer.start();
    s.process = new QProcess(this); configureProcess(s.process);
    auto *process = s.process;
#ifdef __APPLE__
    const int memory = bridge->transport.memory, socket = bridge->transport.childSocket;
    process->setChildProcessModifier([memory, socket, process] {
        if (dup2(memory, STDIN_FILENO) < 0 || dup2(socket, STDOUT_FILENO) < 0 ||
            fcntl(STDIN_FILENO, F_SETFD, 0) != 0 || fcntl(STDOUT_FILENO, F_SETFD, 0) != 0)
            process->failChildProcessModifier("VST shared memory/socket descriptors", errno);
    });
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
    connect(process, &QProcess::finished, this, [this, i, process](int code, QProcess::ExitStatus) {
        d->processFailed(i, process, code == 6
            ? QStringLiteral("This plug-in cannot process stereo 32-bit float audio at 48 kHz.")
            : code == 5 ? QStringLiteral("The plug-in could not be loaded or initialized. Check its installation and license.")
            : QStringLiteral("The VST add-on exited (code %1).").arg(code));
    });
    connect(process, &QProcess::errorOccurred, this, [this, i, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) d->processFailed(i, process, QStringLiteral("Could not start the VST add-on."));
    });
    d->timer.start();
    QStringList arguments{QStringLiteral("--run"), id, QStringLiteral("--module"), s.path,
        QStringLiteral("--class"), s.uid, QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid())};
#ifdef __APPLE__
    arguments << QStringLiteral("--state-dir") << s.stateDirectory->path();
#endif
    d->startCandidate(i, process, arguments);
#endif
    emit changed();
}
void VstService::activate(VstSlot slot) {
    auto &s = d->channels[int(slot)];
    if (!vstModeEnabled() || !available()) return;
    if (s.process) { bypass(slot, false); return; }
    if (applying(slot) || s.path.isEmpty() || s.uid.isEmpty()) { emit changed(); return; }
    select(slot, s.path, s.uid);
}
void VstService::bypass(VstSlot slot, bool bypassed) {
    auto &s = d->channels[int(slot)]; s.bypass = bypassed;
    if (s.bridge) s.bridge->bypass = bypassed;
    if (s.status == QLatin1StringView("vst.active") || s.status == QLatin1StringView("vst.bypassed")) s.status = bypassed ? QStringLiteral("vst.bypassed") : QStringLiteral("vst.active");
    emit changed();
}
bool VstService::active(VstSlot slot) const { return vstModeEnabled() && d->channels[int(slot)].status == QLatin1StringView("vst.active"); }
bool VstService::bypassed(VstSlot slot) const { return d->channels[int(slot)].bypass; }
QString VstService::statusKey(VstSlot slot) const {
    const auto &s = d->channels[int(slot)];
    return !s.bridge && applying(slot) ? QStringLiteral("vst.loading") : s.status;
}
QString VstService::detail(VstSlot slot) const { return d->channels[int(slot)].detail; }
int VstService::latency(VstSlot slot) const {
#if defined(_WIN32) || defined(__APPLE__)
    const auto &s = d->channels[int(slot)];
    return s.bridge ? vst::load(&s.bridge->shared->latency) : 0;
#else
    Q_UNUSED(slot); return 0;
#endif
}
void VstService::openEditor(VstSlot slot, quintptr owner, int x, int y) {
#if defined(_WIN32) || defined(__APPLE__)
    auto &s = d->channels[int(slot)];
    qInfo().noquote() << "VST_EDITOR_OPEN" << names[int(slot)] << "state=" << s.status
        << "pid=" << (s.process ? s.process->processId() : 0) << "owner=" << owner;
    if (!s.bridge) return;
    auto *shared = s.bridge->shared;
    s.editorStatus = 0;
    vst::exchange(&shared->editorStatus, 0);
    shared->owner = owner; shared->x = x; shared->y = y;
    vst::exchange(&shared->openEditor, 1);
#else
    Q_UNUSED(slot); Q_UNUSED(owner); Q_UNUSED(x); Q_UNUSED(y);
#endif
}
} // namespace vtrpon
