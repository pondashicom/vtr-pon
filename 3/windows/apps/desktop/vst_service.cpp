#include "vst_service.h"
#include "vst_protocol.h"
#include "vst_deadline.h"
#include "vst_deferred_release.h"
#ifdef VTRPON_VST_TEST_HOOKS
#include "vst3_test_hooks.h"
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
#include <QUuid>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace vtrpon {
namespace {
constexpr int count = int(VstSlot::Count);
const std::array<QString, count> names{QStringLiteral("EDIT"), QStringLiteral("ITEM"), QStringLiteral("MASTER")};
std::atomic_bool mode{false};
struct Bridge {
    std::mutex processing;
    std::atomic_bool failed{false}, bypass{false};
    enum class Fault { None, OversizedBuffer, Worker, Deadline, Transport, NonFinite };
    std::atomic<Fault> fault{Fault::None};
    std::atomic_int lastFrames{0}, lastBudgetMs{0};
    // Some decoders deliver more than one VST block in a GstBuffer. Stage all
    // output so a failure in a later block still returns the entire dry buffer.
    std::array<float, 65536 * 2> staged{};
#ifdef _WIN32
    HANDLE mapping = nullptr, request = nullptr, response = nullptr;
    vst::Shared *shared = nullptr;
    vst::Timing *timing = nullptr;
    int64_t frequency = vst::qpcFrequency(), sequence = 0;
    std::atomic<int64_t> requestedAt{0}, resumedAt{0}, startedAt{0}, completedAt{0}, deadlineAt{0};
    ~Bridge() {
#ifdef VTRPON_VST_TEST_HOOKS
        if (vst::test::destroyed) vst::test::destroyed(this);
#endif
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        if (request) CloseHandle(request);
        if (response) CloseHandle(response);
    }
#endif
};
std::array<std::atomic<std::shared_ptr<Bridge>>, count> bridges;
struct BridgeReclaimer {
    vst::DeferredRelease<Bridge> pending;
    QTimer timer;
    BridgeReclaimer() {
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] {
            pending.collect();
            if (pending.empty()) timer.stop();
        });
    }
    void retire(std::shared_ptr<Bridge> &bridge) {
        pending.retire(bridge);
        pending.collect();
        if (!pending.empty()) timer.start();
    }
};
BridgeReclaimer &bridgeReclaimer() {
    // Created/used only by the UI thread. Outlives VstService so shutdown cannot
    // transfer destruction to a paused audio reader. If the event loop has
    // already stopped, remaining resources stay owned until process exit.
    static auto *reclaimer = new BridgeReclaimer;
    return *reclaimer;
}
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
#ifdef _WIN32
    auto bridge = bridges[int(slot)].load();
#ifdef VTRPON_VST_TEST_HOOKS
    if (bridge && vst::test::acquired) vst::test::acquired(bridge.get());
#endif
    if (!vstModeEnabled() || !bridge || bridge->failed || bridge->bypass) return;
    std::unique_lock lock(bridge->processing, std::try_to_lock);
    if (!lock.owns_lock() || bridge->failed) return;
    auto *shared = bridge->shared;
    if (!shared || !InterlockedCompareExchange(&shared->ready, 0, 0)) return;
    if (frames > bridge->staged.size() / 2 || InterlockedCompareExchange(&shared->failed, 0, 0)) {
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
    const auto deadline = vst::qpcNow() + bridge->frequency * budgetMs / 1000;
    bridge->deadlineAt = deadline;
    auto *timing = bridge->timing;
    if (!InterlockedCompareExchange(&timing->supported, 0, 0)) timing = nullptr;
    for (size_t offset = 0; offset < frames;) {
        if (vst::qpcNow() >= deadline) {
            bridge->fault = Bridge::Fault::Deadline; bridge->failed = true; return;
        }
        const size_t block = std::min(frames - offset, size_t(vst::kFrames));
        shared->frames = uint32_t(block);
        std::memcpy(shared->input, samples + offset * 2, block * 2 * sizeof(float));
        const auto sequence = ++bridge->sequence;
        bridge->requestedAt = vst::qpcNow();
        bridge->startedAt = 0; bridge->completedAt = 0; bridge->resumedAt = 0;
        if (timing) {
            timing->request = sequence;
            timing->requestedAt = bridge->requestedAt.load();
            timing->deadline = deadline;
            InterlockedExchange64(&timing->completedRequest, 0);
        }
        if (!ResetEvent(bridge->response) || !SetEvent(bridge->request)) {
            bridge->fault = Bridge::Fault::Transport; bridge->failed = true; return;
        }
#ifdef VTRPON_VST_TEST_HOOKS
        if (vst::test::requested) vst::test::requested(shared, timing);
#endif
        const DWORD result = vst::waitForCompletion(bridge->response, timing, sequence, deadline, bridge->frequency);
        bridge->resumedAt = vst::qpcNow();
        if (timing && InterlockedCompareExchange64(&timing->completedRequest, 0, 0) == sequence) {
            bridge->startedAt = timing->startedAt; bridge->completedAt = timing->completedAt;
        }
        const bool workerFailed = InterlockedCompareExchange(&shared->failed, 0, 0) != 0;
        if (result != WAIT_OBJECT_0 || workerFailed) {
            bridge->fault = workerFailed ? Bridge::Fault::Worker : result == WAIT_TIMEOUT
                ? Bridge::Fault::Deadline : Bridge::Fault::Transport;
            bridge->failed = true;
            return; // Preserve the entire dry buffer, including on partial failure.
        }
        for (size_t i = 0; i < block * 2; ++i) {
            if (!std::isfinite(shared->output[i])) { bridge->fault = Bridge::Fault::NonFinite; bridge->failed = true; return; }
        }
        std::memcpy(bridge->staged.data() + offset * 2, shared->output, block * 2 * sizeof(float));
        offset += block;
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
        bool bypass = false;
    };
    std::array<Slot, count> channels;
    std::array<Slot, count> candidates;
    std::vector<Slot> retired;
    QTimer timer, scanPoll;
    std::shared_ptr<VstScan> scanState;
    VstScan::Snapshot scanSnapshot;
    QJsonArray catalog;
    bool scanBusy = false, scanRequested = false, scanCanceled = false;
    explicit Impl(VstService *owner) : q(owner) {}
    static void stopInstance(Slot &s) {
        if (s.bridge) s.bridge->failed = true;
        bridgeReclaimer().retire(s.bridge);
        dispose(s.process); s.process = nullptr;
    }
    void updateTimer() {
        const auto running = [](const auto &s) { return bool(s.bridge); };
        if (retired.empty() && std::none_of(channels.cbegin(), channels.cend(), running)
            && std::none_of(candidates.cbegin(), candidates.cend(), running))
            timer.stop();
    }
    void stop(int i) {
        bridges[i].store(nullptr);
        stopInstance(channels[i]);
        stopInstance(candidates[i]);
        candidates[i] = {};
        channels[i].applyError.clear();
        updateTimer();
    }
    void collectRetired() {
        for (auto it = retired.begin(); it != retired.end();) {
            // Let an in-flight block finish on the previous plug-in. Never wait
            // on the UI thread or kill that process in the middle of its block.
            auto bridge = it->bridge;
            std::unique_lock lock(bridge->processing, std::try_to_lock);
            if (!lock.owns_lock()) { ++it; continue; }
            stopInstance(*it);
            it = retired.erase(it);
        }
        updateTimer();
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
        bridges[i].store(s.bridge);
        if (previous.bridge) retired.push_back(std::move(previous));
        saveSelection(i);
        qInfo().noquote() << "VST_APPLIED" << names[i] << "path=" << s.path << "class=" << s.uid;
    }
    void fail(int i, const QString &reason, bool candidate = false) {
        auto &s = candidate ? candidates[i] : channels[i];
        qWarning().noquote() << (candidate ? "VST_APPLY_FAILURE" : "VST_FAILURE")
            << names[i] << "path=" << s.path << "class=" << s.uid << reason;
#ifdef _WIN32
        if (s.bridge) {
            auto *shared = s.bridge->shared;
            qWarning() << "VST_TIMING warmupMaxUs=" << InterlockedCompareExchange(&shared->warmupMaxUs, 0, 0)
                << "processMaxUs=" << InterlockedCompareExchange(&shared->processMaxUs, 0, 0)
                << "processedBlocks=" << InterlockedCompareExchange(&shared->processedBlocks, 0, 0)
                << "bufferFrames=" << s.bridge->lastFrames.load() << "budgetMs=" << s.bridge->lastBudgetMs.load();
            qWarning() << "VST_QPC frequency=" << s.bridge->frequency
                << "request=" << s.bridge->requestedAt.load() << "start=" << s.bridge->startedAt.load()
                << "complete=" << s.bridge->completedAt.load() << "resume=" << s.bridge->resumedAt.load()
                << "deadline=" << s.bridge->deadlineAt.load();
        }
#endif
        if (candidate) {
            stopInstance(s);
            s = {};
            channels[i].applyError = reason;
            emit q->applicationFailed(names[i], reason);
        } else {
            bridges[i].store(nullptr);
            stopInstance(s);
            s.status = QStringLiteral("vst.failed");
            s.detail = reason;
            emit q->failure(names[i], reason);
        }
        updateTimer();
        emit q->changed();
    }
    void processFailed(int i, QProcess *process, const QString &reason) {
        if (candidates[i].process == process) fail(i, reason, true);
        else if (channels[i].process == process) fail(i, reason);
    }
    bool poll(int i, bool candidate) {
#ifdef _WIN32
        auto &s = candidate ? candidates[i] : channels[i];
        if (!s.bridge) return false;
        auto *shared = s.bridge->shared;
        if (s.bridge->failed || InterlockedCompareExchange(&shared->failed, 0, 0)) {
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
        const long heartbeat = InterlockedCompareExchange(&shared->heartbeat, 0, 0);
        if (s.heartbeat != heartbeat) { s.heartbeat = heartbeat; s.heartbeatTimer.restart(); }
        const bool ready = InterlockedCompareExchange(&shared->ready, 0, 0);
        if ((!ready && s.startup.elapsed() > 15000) || (ready && s.heartbeatTimer.elapsed() > 2000)) {
            fail(i, QStringLiteral("The plug-in stopped responding."), candidate); return false;
        }
        if (candidate) {
            if (ready) { commit(i); return true; }
            return false;
        }
        bool changed = false;
        const long editorStatus = InterlockedCompareExchange(&shared->editorStatus, 0, 0);
        if (editorStatus != s.editorStatus) {
            s.editorStatus = editorStatus;
            if (editorStatus) qInfo().noquote() << "VST_EDITOR_RESULT" << names[i]
                << "status=" << editorStatus << "pid=" << s.process->processId();
        }
        if (editorStatus < 0 && s.detail.isEmpty()) {
            s.detail = QStringLiteral("This plug-in does not provide a compatible Windows editor."); changed = true;
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
#ifdef VTRPON_VST_TEST_HOOKS
    if (vst::test::addonOverride) return QString::fromUtf8(vst::test::addonOverride);
#endif
    return QCoreApplication::applicationDirPath() + QStringLiteral("/audio-addons/vtr-pon-vst3-addon.exe");
}
bool VstService::available() const {
    // Add-on installation is fixed for this application session.
    static const bool installed = QFileInfo(addonPath()).isFile();
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
#ifdef _WIN32
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
        for (int i = 0; i < count; ++i) d->stop(i);
        for (auto &s : d->retired) Impl::stopInstance(s);
        d->retired.clear();
        cancelScan();
    });
}
VstService::~VstService() {
    mode = false;
    for (int i = 0; i < count; ++i) d->stop(i);
    for (auto &s : d->retired) Impl::stopInstance(s);
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
    if (enabled && !available()) return false;
    auto settings = makeControllerSettings();
    settings.setValue(QStringLiteral("audio/useVstPlugins"), enabled); settings.sync();
    if (settings.status() != QSettings::NoError) return false;
    reloadSettings(); return true;
}
void VstService::scan() {
    if (d->scanBusy || !available()) return;
    QStringList roots;
    const auto common = qEnvironmentVariable("CommonProgramW6432");
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    if (!common.isEmpty()) roots.append(common + QStringLiteral("/VST3"));
    if (!local.isEmpty()) roots.append(local + QStringLiteral("/Programs/Common/VST3"));
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
void VstService::select(VstSlot slot, const QString &path, const QString &uid) {
    const int i = int(slot);
    if (!vstModeEnabled() || !available()) return;
    qInfo().noquote() << "VST_APPLY" << names[i] << "path=" << path << "class=" << uid;
    auto &current = d->channels[i];
    auto &s = d->candidates[i];
    Impl::stopInstance(s); s = {};
    current.applyError.clear();
    if (path.isEmpty() && uid.isEmpty()) {
        d->stop(i); current = {}; d->saveSelection(i);
        emit changed(); return;
    }
    // Reapplying the running instance must preserve its parameter state.
    if (current.bridge && !current.bridge->failed && current.path == path && current.uid == uid) {
        bypass(slot, false); d->updateTimer(); return;
    }
    s.path = path; s.uid = uid;
#ifdef _WIN32
    auto bridge = std::make_shared<Bridge>();
    const QString id = QStringLiteral("Local\\VtrPonVst-") + QUuid::createUuid().toString(QUuid::Id128);
    bridge->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vst::TimedShared), reinterpret_cast<LPCWSTR>(id.utf16()));
    if (bridge->mapping) bridge->shared = static_cast<vst::Shared *>(MapViewOfFile(bridge->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vst::TimedShared)));
    bridge->request = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-request").utf16()));
    bridge->response = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-response").utf16()));
    if (!bridge->shared || !bridge->request || !bridge->response) { d->fail(i, QStringLiteral("Could not create VST audio transport."), true); return; }
    auto *transport = new (bridge->shared) vst::TimedShared;
    bridge->timing = &transport->timing;
    bridge->timing->frequency = bridge->frequency;
    s.bridge = bridge; s.startup.start(); s.heartbeatTimer.start();
    s.process = new QProcess(this); configureProcess(s.process);
    auto *process = s.process;
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
    process->start(addonPath(), {QStringLiteral("--run"), id, QStringLiteral("--module"), s.path,
        QStringLiteral("--class"), s.uid, QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid())});
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
#ifdef _WIN32
    const auto &s = d->channels[int(slot)];
    return s.bridge ? InterlockedCompareExchange(&s.bridge->shared->latency, 0, 0) : 0;
#else
    Q_UNUSED(slot); return 0;
#endif
}
void VstService::openEditor(VstSlot slot, quintptr owner, int x, int y) {
#ifdef _WIN32
    auto &s = d->channels[int(slot)];
    qInfo().noquote() << "VST_EDITOR_OPEN" << names[int(slot)] << "state=" << s.status
        << "pid=" << (s.process ? s.process->processId() : 0) << "owner=" << owner;
    if (!s.bridge) return;
    auto *shared = s.bridge->shared;
    s.editorStatus = 0;
    InterlockedExchange(&shared->editorStatus, 0);
    shared->owner = owner; shared->x = x; shared->y = y;
    InterlockedExchange(&shared->openEditor, 1);
#else
    Q_UNUSED(slot); Q_UNUSED(owner); Q_UNUSED(x); Q_UNUSED(y);
#endif
}
} // namespace vtrpon
