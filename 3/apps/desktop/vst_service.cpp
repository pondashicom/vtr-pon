#include "vst_service.h"
#include "vst_protocol.h"
#include "controller_settings.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <mutex>

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
    ~Bridge() {
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        if (request) CloseHandle(request);
        if (response) CloseHandle(response);
    }
#endif
};
std::array<std::atomic<std::shared_ptr<Bridge>>, count> bridges;
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
    if (!vstModeEnabled() || !bridge || bridge->failed || bridge->bypass) return;
    std::unique_lock lock(bridge->processing, std::try_to_lock);
    if (!lock.owns_lock()) return;
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
    // Neural effects can process in bursts longer than 5 ms even for a 10 ms
    // input block. Allow a small scheduling margin and scale larger decoder
    // buffers to their audio duration, while keeping a hung worker bounded.
    const auto budgetMs = std::clamp<int>((frames * 1000 + vst::kRate - 1) / vst::kRate, 20, 50);
    bridge->lastFrames = int(frames); bridge->lastBudgetMs = budgetMs;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    for (size_t offset = 0; offset < frames;) {
        const size_t block = std::min(frames - offset, size_t(vst::kFrames));
        shared->frames = uint32_t(block);
        std::memcpy(shared->input, samples + offset * 2, block * 2 * sizeof(float));
        ResetEvent(bridge->response);
        SetEvent(bridge->request);
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const DWORD result = remaining <= 0 ? WAIT_TIMEOUT : WaitForSingleObject(bridge->response, DWORD(remaining));
        if (result != WAIT_OBJECT_0 || InterlockedCompareExchange(&shared->failed, 0, 0)) {
            bridge->fault = result == WAIT_TIMEOUT ? Bridge::Fault::Deadline : result != WAIT_OBJECT_0
                ? Bridge::Fault::Transport : Bridge::Fault::Worker;
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
        QString path, uid, status = QStringLiteral("vst.inactive"), detail;
        QProcess *process = nullptr;
        std::shared_ptr<Bridge> bridge;
        QElapsedTimer startup, heartbeatTimer;
        long heartbeat = 0;
        long editorStatus = 0;
        bool bypass = false;
    };
    std::array<Slot, count> channels;
    QTimer timer, scanTimeout;
    QProcess *scanner = nullptr;
    std::unique_ptr<QTemporaryDir> scanDir;
    QStringList scanPaths;
    QJsonArray catalog;
    QString scanFile;
    bool scanBusy = false;
    explicit Impl(VstService *owner) : q(owner) {}
    void stop(int i) {
        bridges[i].store(nullptr);
        auto &s = channels[i];
        if (s.bridge) s.bridge->failed = true;
        s.bridge.reset();
        dispose(s.process); s.process = nullptr;
        if (std::none_of(channels.cbegin(), channels.cend(), [](const auto &channel) { return bool(channel.bridge); }))
            timer.stop();
    }
    void fail(int i, const QString &reason) {
        const auto &s = channels[i];
        qWarning().noquote() << "VST_FAILURE" << names[i] << "path=" << s.path << "class=" << s.uid << reason;
#ifdef _WIN32
        if (s.bridge) {
            auto *shared = s.bridge->shared;
            qWarning() << "VST_TIMING warmupMaxUs=" << InterlockedCompareExchange(&shared->warmupMaxUs, 0, 0)
                << "processMaxUs=" << InterlockedCompareExchange(&shared->processMaxUs, 0, 0)
                << "processedBlocks=" << InterlockedCompareExchange(&shared->processedBlocks, 0, 0)
                << "bufferFrames=" << s.bridge->lastFrames.load() << "budgetMs=" << s.bridge->lastBudgetMs.load();
        }
#endif
        stop(i);
        channels[i].status = QStringLiteral("vst.failed");
        channels[i].detail = reason;
        emit q->failure(names[i], reason);
        emit q->changed();
    }
    void nextScan() {
        if (scanPaths.isEmpty()) {
            scanBusy = false;
            emit q->catalogChanged(); return;
        }
        const QString path = scanPaths.takeFirst();
        scanFile = scanDir->filePath(QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".json"));
        scanner = new QProcess(q);
        configureProcess(scanner);
        const auto current = scanner;
        QObject::connect(current, &QProcess::finished, q, [this, current, path](int code, QProcess::ExitStatus status) {
            if (scanner != current) return;
            scanTimeout.stop();
            QFile file(scanFile);
            if (code == 0 && status == QProcess::NormalExit && file.size() < 8 * 1024 * 1024 && file.open(QIODevice::ReadOnly)) {
                const auto array = QJsonDocument::fromJson(file.readAll()).array();
                for (const auto &value : array) {
                    auto item = value.toObject();
                    if (item.value("uid").toString().size() != 32 || item.value("name").toString().isEmpty()) continue;
                    item.insert("path", path); catalog.append(item);
                }
            }
            file.close(); QFile::remove(scanFile);
            current->deleteLater(); scanner = nullptr;
            emit q->catalogChanged(); nextScan();
        });
        QObject::connect(current, &QProcess::errorOccurred, q, [this, current](QProcess::ProcessError error) {
            if (scanner != current || error != QProcess::FailedToStart) return;
            scanTimeout.stop(); dispose(scanner); scanner = nullptr;
            nextScan();
        });
        current->start(VstService::addonPath(), {QStringLiteral("--scan"), path, QStringLiteral("--result"), scanFile,
            QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid())});
        scanTimeout.start(20000);
    }
};

VstService &VstService::instance() {
    static auto *service = new VstService;
    return *service;
}
QString VstService::addonPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/audio-addons/vtr-pon-vst3-addon.exe");
}
bool VstService::available() const {
    // Add-on installation is fixed for this application session.
    static const bool installed = QFileInfo(addonPath()).isFile();
    return installed;
}
VstService::VstService() : QObject(QCoreApplication::instance()), d(std::make_unique<Impl>(this)) {
    reloadSettings();
    connect(&d->scanTimeout, &QTimer::timeout, this, [this] { if (d->scanner) d->scanner->kill(); });
    d->scanTimeout.setSingleShot(true);
    connect(&d->timer, &QTimer::timeout, this, [this] {
#ifdef _WIN32
        bool changedState = false;
        for (int i = 0; i < count; ++i) {
            auto &s = d->channels[i];
            if (!s.bridge) continue;
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
                d->fail(i, reason); continue;
            }
            const long heartbeat = InterlockedCompareExchange(&shared->heartbeat, 0, 0);
            if (s.heartbeat != heartbeat) { s.heartbeat = heartbeat; s.heartbeatTimer.restart(); }
            const bool ready = InterlockedCompareExchange(&shared->ready, 0, 0);
            const long editorStatus = InterlockedCompareExchange(&shared->editorStatus, 0, 0);
            if (editorStatus != s.editorStatus) {
                s.editorStatus = editorStatus;
                if (editorStatus) qInfo().noquote() << "VST_EDITOR_RESULT" << names[i]
                    << "status=" << editorStatus << "pid=" << s.process->processId();
            }
            if (editorStatus < 0 && s.detail.isEmpty()) {
                s.detail = QStringLiteral("This plug-in does not provide a compatible Windows editor."); changedState = true;
            }
            if ((!ready && s.startup.elapsed() > 15000) || (ready && s.heartbeatTimer.elapsed() > 2000)) {
                d->fail(i, QStringLiteral("The plug-in stopped responding.")); continue;
            }
            const QString state = ready ? (s.bypass ? QStringLiteral("vst.bypassed") : QStringLiteral("vst.active")) : QStringLiteral("vst.loading");
            if (s.status != state) {
                s.status = state; changedState = true;
                qInfo() << "VST_STATE" << names[i] << state << "warmupMaxUs=" << InterlockedCompareExchange(&shared->warmupMaxUs, 0, 0);
            }
        }
        if (changedState) emit changed();
#endif
    });
    d->timer.setParent(this);
    d->timer.setObjectName(QStringLiteral("vstMonitorTimer"));
    d->timer.setInterval(100);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        mode = false;
        for (int i = 0; i < count; ++i) d->stop(i);
        dispose(d->scanner); d->scanner = nullptr;
    });
}
VstService::~VstService() {
    mode = false;
    for (int i = 0; i < count; ++i) d->stop(i);
    dispose(d->scanner);
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
    if (!d->scanDir) d->scanDir = std::make_unique<QTemporaryDir>();
    if (!d->scanDir->isValid()) return;
    d->catalog = {}; d->scanPaths.clear();
    QStringList roots{qEnvironmentVariable("CommonProgramW6432") + QStringLiteral("/VST3"),
        qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/Programs/Common/VST3")};
    for (const auto &root : roots) {
        if (!QDir(root).exists()) continue;
        QDirIterator it(root, {QStringLiteral("*.vst3")}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            // Bundle binaries live below another .vst3 directory; scan the bundle once.
            if (!path.contains(QStringLiteral(".vst3/"), Qt::CaseInsensitive)) d->scanPaths.append(path);
        }
    }
    d->scanPaths.removeDuplicates(); d->scanBusy = true;
    emit catalogChanged(); d->nextScan();
}
bool VstService::scanning() const { return d->scanBusy; }
QJsonArray VstService::plugins() const { return d->catalog; }
QString VstService::selectedPath(VstSlot slot) const { return d->channels[int(slot)].path; }
QString VstService::selectedUid(VstSlot slot) const { return d->channels[int(slot)].uid; }
void VstService::select(VstSlot slot, const QString &path, const QString &uid) {
    const int i = int(slot);
    qInfo().noquote() << "VST_SELECT" << names[i] << "path=" << path << "class=" << uid;
    d->stop(i); auto &s = d->channels[i]; s.path = path; s.uid = uid; s.detail.clear();
    s.status = QStringLiteral("vst.inactive"); s.bypass = false;
    auto settings = makeControllerSettings();
    settings.setValue(key(i, "path"), path); settings.setValue(key(i, "uid"), uid); settings.sync();
    if (settings.status() != QSettings::NoError) s.detail = QStringLiteral("Could not save the plug-in selection.");
    activate(slot);
}
void VstService::activate(VstSlot slot) {
    const int i = int(slot);
    auto &s = d->channels[i];
    if (!vstModeEnabled() || !available() || s.path.isEmpty() || s.uid.isEmpty()) { emit changed(); return; }
    if (s.process) { bypass(slot, false); return; }
#ifdef _WIN32
    auto bridge = std::make_shared<Bridge>();
    const QString id = QStringLiteral("Local\\VtrPonVst-") + QUuid::createUuid().toString(QUuid::Id128);
    bridge->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vst::Shared), reinterpret_cast<LPCWSTR>(id.utf16()));
    if (bridge->mapping) bridge->shared = static_cast<vst::Shared *>(MapViewOfFile(bridge->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vst::Shared)));
    bridge->request = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-request").utf16()));
    bridge->response = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-response").utf16()));
    if (!bridge->shared || !bridge->request || !bridge->response) { d->fail(i, QStringLiteral("Could not create VST audio transport.")); return; }
    new (bridge->shared) vst::Shared;
    s.bridge = bridge; s.bypass = false; s.detail.clear(); s.status = QStringLiteral("vst.loading");
    s.heartbeat = 0; s.editorStatus = 0; s.startup.start(); s.heartbeatTimer.start(); bridges[i].store(bridge);
    s.process = new QProcess(this); configureProcess(s.process);
    auto *process = s.process;
    connect(process, &QProcess::finished, this, [this, i, process](int code, QProcess::ExitStatus) {
        if (d->channels[i].process == process) d->fail(i, code == 6
            ? QStringLiteral("This plug-in cannot process stereo 32-bit float audio at 48 kHz.")
            : code == 5 ? QStringLiteral("The plug-in could not be loaded or initialized. Check its installation and license.")
            : QStringLiteral("The VST add-on exited (code %1).").arg(code));
    });
    connect(process, &QProcess::errorOccurred, this, [this, i, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && d->channels[i].process == process) d->fail(i, QStringLiteral("Could not start the VST add-on."));
    });
    d->timer.start();
    process->start(addonPath(), {QStringLiteral("--run"), id, QStringLiteral("--module"), s.path,
        QStringLiteral("--class"), s.uid, QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid())});
#endif
    emit changed();
}
void VstService::bypass(VstSlot slot, bool bypassed) {
    auto &s = d->channels[int(slot)]; s.bypass = bypassed;
    if (s.bridge) s.bridge->bypass = bypassed;
    if (s.status == QLatin1StringView("vst.active") || s.status == QLatin1StringView("vst.bypassed")) s.status = bypassed ? QStringLiteral("vst.bypassed") : QStringLiteral("vst.active");
    emit changed();
}
bool VstService::active(VstSlot slot) const { return vstModeEnabled() && d->channels[int(slot)].status == QLatin1StringView("vst.active"); }
bool VstService::bypassed(VstSlot slot) const { return d->channels[int(slot)].bypass; }
QString VstService::statusKey(VstSlot slot) const { return d->channels[int(slot)].status; }
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
