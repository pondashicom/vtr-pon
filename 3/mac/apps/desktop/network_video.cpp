#include "platform_capabilities.h"
#include "addon_platform.h"
#include <QHash>
#include "network_video.h"
#include "media_timecode.h"
#include "network_video_addon_api.h"
#include "network_video_input_clock.h"
#include "controller_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QSysInfo>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <system_error>

namespace vtrpon {
namespace {
using Clock = std::chrono::steady_clock;
struct Video {
    QByteArray composite, alpha;
    qint64 timecode = 0;
    int width = 0, height = 0;
    quint64 generation = 0;
    Clock::time_point captured;
};
struct Audio { QByteArray pcm; quint64 generation; Clock::time_point captured; qint64 timecode; };
struct Publication {
    Video video;
    std::deque<Audio> audio;
    size_t audioBytes = 0;
};
constexpr std::array<unsigned, 3> groupMasks{1u, 6u, 8u};
struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    NetworkVideoSettings settings;
    NetworkVideoState state;
    QStringList ownNames, localHostNames;
    std::array<Publication, 3> publications;
    qint64 nextAudioTimecode = 0;
    std::atomic<unsigned> outputMask{0};
    std::atomic_bool stopping{false};
    std::atomic<quint64> generation{1}, publishing{0};
};
Shared &shared() { static auto *value = new Shared; return *value; }
QString localOutputHostId() {
    const auto id = QSysInfo::machineUniqueId();
    return id.isEmpty() ? QSysInfo::machineHostName() : QString::fromLatin1(id.toHex());
}
QStringList localNetworkVideoHostNames() {
    QStringList names{QSysInfo::machineHostName()};
    // NDI Access Manager can advertise a machine name different from Windows.
    // Read its documented shared configuration without changing it.
    const auto programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty()) {
        QFile file(QDir(programData).filePath(QStringLiteral("NDI/ndi-config.v1.json")));
        if (file.open(QIODevice::ReadOnly) && file.size() <= 1024 * 1024) {
            const auto alias = QJsonDocument::fromJson(file.readAll()).object()
                .value(QStringLiteral("ndi")).toObject().value(QStringLiteral("machinename")).toString();
            if (!alias.isEmpty()) names.append(alias);
        }
    }
    names.removeAll(QString{}); names.removeDuplicates();
    return names;
}
bool ownSource(const Shared &data, const QString &name) {
    if (data.ownNames.contains(name)) return true;

    for (const auto &host : data.localHostNames) {
        const auto prefix = host + QStringLiteral(" (");
        for (int i = 0; i < 4; ++i)
            if (name.startsWith(prefix, Qt::CaseInsensitive)
                && name.mid(prefix.size()) == networkVideoOutputName(data.settings, NetworkVideoKind(i)) + QLatin1Char(')')) return true;
    }
    return false;
}
class Addon {
public:
    QLibrary library{networkVideoAddonPath()};
    const VtrPonNetworkVideoApi *api = nullptr;
    void *context = nullptr;
    QString error;
    Addon() {
        if (!platformFeatureImplemented(PlatformFeature::NetworkVideo)) {
            error = QStringLiteral("NDI is not implemented on this platform."); return;
        }
#ifdef Q_OS_MACOS
        if (!addons::verifyInstalledBinary(networkVideoAddonPath(), &error)) return;
#endif
        const auto get = reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
        api = get ? get(VTRPON_NETWORK_VIDEO_ABI) : nullptr;
        if (!api || api->abiVersion != VTRPON_NETWORK_VIDEO_ABI || api->structSize < sizeof(*api)
            || !api->addonId || std::strcmp(api->addonId, "vtr-pon-network-video-addon") != 0
            || !api->create || !api->destroy || !api->findSources || !api->createSender
            || !api->destroySender || !api->senderName || !api->sendVideo || !api->sendAudio
            || !api->createReceiver || !api->destroyReceiver || !api->receive) {
            api = nullptr;
            error = QStringLiteral("NDI add-on ABI 2 is required. Update VTR-PON and vtr-pon-network-video-addon together. %1").arg(library.errorString());
            return;
        }
        char message[1024]{};
        context = api->create(message, sizeof(message));
        if (!context) error = QString::fromUtf8(message);
    }
    ~Addon() { if (context) api->destroy(context); }
};
void report(const QString &error) {
    auto &data = shared();
    std::lock_guard lock(data.mutex); data.state.error = error;
}
void sourceFound(void *value, const char *name) {
    if (!name || !*name) return;
    const QString text = QString::fromUtf8(name);
    static_cast<QList<NetworkVideoSource> *>(value)->append({text, networkVideoSourcePath(text)});
}
}
qint64 networkVideoTimecode() {
    return mediaTimecode100ns();
}
QString networkVideoOutputName(const NetworkVideoSettings &settings, NetworkVideoKind kind) {
    auto name = settings.outputName.trimmed();
    if (name.isEmpty()) name = defaultNetworkVideoOutputName();
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\x00])")), QStringLiteral(" "));
    // Reserve space for the host, delimiters and SDK collision suffix. Preserve
    // existing ordinary names; trim only unusually long host/name combinations.
    static const auto hosts = localNetworkVideoHostNames();
    qsizetype hostLength = 0;
    for (const auto &host : hosts) hostLength = std::max(hostLength, host.size());
    const qsizetype limit = std::clamp<qsizetype>(253 - hostLength - 24, 16, 200);
    name = name.left(limit);
    static const std::array<QString, 4> suffixes{QString{}, QStringLiteral(" Fill"), QStringLiteral(" Key"), QStringLiteral(" Alpha")};
    return name + suffixes[size_t(kind)];
}
QString networkVideoAddonPath() {
    return addons::addonFilePath(QCoreApplication::applicationDirPath(), QStringLiteral("vtr-pon-network-video-addon"));
}
bool networkVideoAddonInstalled() {
    if (!platformFeatureImplemented(PlatformFeature::NetworkVideo)) return false;
    // Add-ons are installed/removed while VTR-PON is closed; never poll for them.
    static const bool installed = QFileInfo(networkVideoAddonPath()).isFile();
    return installed;
}
NetworkVideoSettings savedNetworkVideoSettings() {
    auto settings = makeControllerSettings();
    return {settings.value("networkVideo/outputEnabled", false).toBool(),
        settings.value("networkVideo/inputEnabled", false).toBool(),
        settings.value("networkVideo/outputName").toString(),
        settings.value("networkVideo/compositeEnabled", true).toBool(),
        settings.value("networkVideo/fillEnabled", false).toBool(),
        settings.value("networkVideo/keyEnabled", false).toBool(),
        settings.value("networkVideo/alphaEnabled", false).toBool(),
        settings.value("networkVideo/fillFormat", "straight").toString() == QLatin1StringView("premultiplied")};
}
void applyNetworkVideoSettings(const NetworkVideoSettings &settings) {
    if (!networkVideoAddonInstalled()) return;
    auto &data = shared();
    std::lock_guard lock(data.mutex);
    if (data.settings == settings) return;
    data.settings = settings;
    data.generation.fetch_add(1);
    data.publishing.store(0);
    data.publications = {}; data.nextAudioTimecode = 0; data.outputMask.store(0);
    data.state.outputs = {};
    data.state.outputRunning = false;
    if (!settings.inputEnabled) { data.state.sources.clear(); data.state.inputRunning = false; }
    else data.state.sources.removeIf([&](const auto &source) { return ownSource(data, source.name); });
    data.state.error.clear();
    data.wake.notify_all();
}
void saveNetworkVideoSettings(const NetworkVideoSettings &settings) {
    if (!platformFeatureImplemented(PlatformFeature::NetworkVideo)) return;
    auto store = makeControllerSettings();
    store.setValue("networkVideo/outputEnabled", settings.outputEnabled);
    store.setValue("networkVideo/inputEnabled", settings.inputEnabled);
    store.setValue("networkVideo/outputName", settings.outputName);
    store.setValue("networkVideo/compositeEnabled", settings.compositeEnabled);
    store.setValue("networkVideo/fillEnabled", settings.fillEnabled);
    store.setValue("networkVideo/keyEnabled", settings.keyEnabled);
    store.setValue("networkVideo/alphaEnabled", settings.alphaEnabled);
    store.setValue("networkVideo/fillFormat", settings.premultipliedFill ? "premultiplied" : "straight");
    applyNetworkVideoSettings(settings);
}
NetworkVideoState networkVideoState() {
    auto &data = shared(); std::lock_guard lock(data.mutex); return data.state;
}
bool networkVideoInputAllowed(const QString &path, QString *error) {
    auto &data = shared(); std::lock_guard lock(data.mutex);
    const auto name = networkVideoSourceName(path);
    const bool allowed = data.settings.inputEnabled && data.state.inputRunning && !name.isEmpty()
        && !ownSource(data, name);
    if (error) *error = allowed ? QString{} : QStringLiteral("NDI input is disabled, unavailable, or refers to this VTR-PON's own output.");
    return allowed;
}
namespace {
struct SenderWorker {
    std::atomic_bool stop{false}, done{false};
    quint64 generation = 0;
    unsigned group = 0, mask = 0;
    NetworkVideoSettings settings;
};
void setOutputError(const SenderWorker &worker, const QString &error) {
    auto &data = shared(); std::lock_guard lock(data.mutex);
    if (worker.generation != data.generation.load()) return;
    for (unsigned i = 0; i < 4; ++i) if (worker.mask & (1u << i)) {
        auto &output = data.state.outputs[i];
        output.error = error; output.running = false; output.ready = false;
    }
    data.outputMask.fetch_and(~worker.mask);
    if (!data.outputMask.load()) data.publishing.store(0);
    data.state.outputRunning = std::any_of(data.state.outputs.begin(), data.state.outputs.end(),
        [](const auto &output) { return output.running; });
}
QByteArray outputPixels(const Video &video, NetworkVideoKind kind, bool premultiplied) {
    if (kind == NetworkVideoKind::Composite) return video.composite;
    QByteArray pixels = video.alpha;
    auto *bytes = reinterpret_cast<unsigned char *>(pixels.data());
    for (qsizetype i = 0; i < pixels.size(); i += 4) {
        const unsigned alpha = bytes[i + 3];
        for (int c = 0; c < 3; ++c) {
            if (kind == NetworkVideoKind::Key) bytes[i + c] = static_cast<unsigned char>(alpha);
            else if (!alpha) bytes[i + c] = 0;
            else if (kind == NetworkVideoKind::Alpha || !premultiplied)
                bytes[i + c] = static_cast<unsigned char>(std::min(255u, (unsigned(bytes[i + c]) * 255u + alpha / 2u) / alpha));
        }
        if (kind != NetworkVideoKind::Alpha) bytes[i + 3] = 255;
    }
    return pixels;
}
void runSender(const std::shared_ptr<SenderWorker> &worker) {
    auto &data = shared();
    auto active = [&] { return !worker->stop.load() && !data.stopping.load()
        && worker->generation == data.generation.load(); };
    while (active()) {
        Addon addon;
        std::array<void *, 4> senders{};
        struct Cleanup {
            Addon &addon; std::array<void *, 4> &senders;
            ~Cleanup() { for (auto *sender : senders) if (sender) addon.api->destroySender(addon.context, sender); }
        } cleanup{addon, senders};
        QString error = addon.error;
        if (addon.context) {
            for (unsigned i = 0; i < 4 && error.isEmpty() && active(); ++i) if (worker->mask & (1u << i)) {
                const auto name = networkVideoOutputName(worker->settings, NetworkVideoKind(i)).toUtf8();
                senders[i] = addon.api->createSender(addon.context, name.constData());
                if (!senders[i]) { error = QStringLiteral("NDI output could not be started."); break; }
                const auto ownName = QString::fromUtf8(addon.api->senderName(addon.context, senders[i]));
                std::lock_guard lock(data.mutex);
                if (worker->generation == data.generation.load()) {
                    data.state.outputs[i].name = ownName;
                    data.ownNames.removeAll(ownName); data.ownNames.append(ownName);
                    while (data.ownNames.size() > 128) data.ownNames.removeFirst();
                    // Serialize the machine-local history with all output workers.
                    auto store = makeControllerSettings();
                    store.setValue("networkVideo/localOutputHostId", localOutputHostId());
                    store.setValue("networkVideo/localOutputSourceNames", data.ownNames);
                    data.state.sources.removeIf([&](const auto &source) { return ownSource(data, source.name); });
                }
            }
        }
        if (!active()) break;
        if (!error.isEmpty()) {
            setOutputError(*worker, error);
            std::unique_lock lock(data.mutex);
            data.wake.wait_for(lock, std::chrono::seconds(3), [&] { return !active(); });
            continue;
        }
        {
            std::lock_guard lock(data.mutex);
            if (!active()) break;
            for (unsigned i = 0; i < 4; ++i) if (worker->mask & (1u << i)) {
                data.state.outputs[i].ready = true; data.state.outputs[i].error.clear();
            }
            data.outputMask.fetch_or(worker->mask);
            data.publishing.store(worker->generation);
        }
        while (active()) {
            Publication batch;
            {
                std::unique_lock lock(data.mutex);
                auto &queue = data.publications[worker->group];
                data.wake.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return !active() || queue.video.generation || !queue.audio.empty();
                });
                if (!active()) break;
                std::swap(batch, queue);
            }
            const auto &video = batch.video;
            if (video.generation == worker->generation && Clock::now() - video.captured < std::chrono::milliseconds(200)) {
                // Prepare the entire Fill/Key pair before submitting either.
                // One queue and one drop decision guarantee a common source frame.
                std::array<QByteArray, 4> pixels;
                for (unsigned i = 0; i < 4; ++i) if (senders[i])
                    pixels[i] = outputPixels(video, NetworkVideoKind(i), worker->settings.premultipliedFill);
                if (!active()) break;
                for (unsigned i = 0; i < 4; ++i) if (senders[i] && !pixels[i].isEmpty()) {
                    const VtrPonNetworkVideoFrame frame{reinterpret_cast<const uint8_t *>(pixels[i].constData()),
                        video.width, video.height, video.width * 4, 60, 1, i == 3, video.timecode};
                    addon.api->sendVideo(addon.context, senders[i], &frame);
                    std::lock_guard lock(data.mutex);
                    if (worker->generation == data.generation.load()) {
                        auto &output = data.state.outputs[i];
                        output.running = true; ++output.sentFrames;
                        data.state.outputRunning = true; ++data.state.sentFrames;
                    }
                }
            }
            for (const auto &audio : batch.audio) {
                if (!active()) break;
                if (audio.generation != worker->generation || Clock::now() - audio.captured >= std::chrono::milliseconds(200)) continue;
                const VtrPonNetworkAudioFrame frame{reinterpret_cast<const float *>(audio.pcm.constData()),
                    48000, 2, int(audio.pcm.size() / (2 * sizeof(float))), audio.timecode};
                for (unsigned i = 0; i < 4; ++i)
                    if (senders[i] && i != 2) addon.api->sendAudio(addon.context, senders[i], &frame);
            }
        }
    }
}
}
class NetworkVideoRuntime::Impl {
public:
    struct Worker { std::atomic_bool stop{false}, done{false}; };
    std::shared_ptr<Worker> worker = std::make_shared<Worker>();
    Impl() {
        if (!networkVideoAddonInstalled()) { worker->done.store(true); return; }
        shared().stopping.store(false);
        applyNetworkVideoSettings(savedNetworkVideoSettings());
        auto state = worker;
        try {
        std::thread([state] {
            try { run(state); } catch (...) { report(QStringLiteral("Network video worker failed.")); }
            shared().publishing.store(0); shared().outputMask.store(0);
            { std::lock_guard lock(shared().mutex); shared().state.outputRunning = false;
              shared().state.outputs = {}; shared().state.inputRunning = false; shared().state.sources.clear(); }
            state->done.store(true);
        }).detach();
        } catch (const std::system_error &error) {
            report(QStringLiteral("NDI runtime worker could not start: %1")
                       .arg(QString::fromLocal8Bit(error.what())));
            state->done.store(true);
        }
    }
    static void run(const std::shared_ptr<Worker> &worker) {
        auto &data = shared();
        {
            const auto hosts = localNetworkVideoHostNames();
            auto store = makeControllerSettings();
            const auto names = store.value("networkVideo/localOutputHostId").toString() == localOutputHostId()
                ? store.value("networkVideo/localOutputSourceNames").toStringList() : QStringList{};
            std::lock_guard lock(data.mutex);
            data.localHostNames = hosts; data.ownNames = names;
            while (data.ownNames.size() > 128) data.ownNames.removeFirst();
        }
        std::array<std::shared_ptr<SenderWorker>, 3> senders;
        struct StopOnExit {
            std::array<std::shared_ptr<SenderWorker>, 3> &senders;
            ~StopOnExit() { for (auto &sender : senders) if (sender) sender->stop.store(true); shared().wake.notify_all(); }
        } stopOnExit{senders};
        std::unique_ptr<Addon> finder;
        quint64 applied = 0;
        auto nextDiscovery = Clock::time_point{}, retry = Clock::time_point{};
        for (;;) {
            NetworkVideoSettings settings;
            quint64 generation;
            { std::lock_guard lock(data.mutex); settings = data.settings; generation = data.generation.load(); }
            const bool stopping = worker->stop.load();
            if (generation != applied || stopping) {
                for (auto &sender : senders) if (sender) sender->stop.store(true);
                finder.reset(); applied = generation; nextDiscovery = {}; retry = {};
                data.wake.notify_all();
            }
            bool pending = false;
            for (unsigned group = 0; group < senders.size(); ++group) {
                auto &sender = senders[group];
                if (sender && sender->done.load()) sender.reset();
                const auto mask = groupMasks[group] & settings.outputMask();
                if (!sender && !stopping && settings.outputEnabled && mask) {
                    sender = std::make_shared<SenderWorker>();
                    sender->generation = generation; sender->group = group; sender->mask = mask; sender->settings = settings;
                    try {
                    std::thread([state = sender] {
                        try { runSender(state); }
                        catch (...) { setOutputError(*state, QStringLiteral("NDI output worker failed.")); }
                        state->done.store(true); shared().wake.notify_all();
                    }).detach();
                    } catch (const std::system_error &error) {
                        setOutputError(*sender, QStringLiteral("NDI output worker could not start: %1")
                                                    .arg(QString::fromLocal8Bit(error.what())));
                        sender->done.store(true);
                        worker->stop.store(true);
                    }
                }
                pending = pending || bool(sender);
            }
            if (stopping && !pending) break;
            if (!stopping && settings.inputEnabled && !finder && Clock::now() >= retry) {
                finder = std::make_unique<Addon>();
                if (!finder->context) { report(finder->error); finder.reset(); retry = Clock::now() + std::chrono::seconds(3); }
                else report({});
            }
            if (finder && Clock::now() >= nextDiscovery) {
                QList<NetworkVideoSource> sources;
                const bool found = finder->api->findSources(finder->context, sourceFound, &sources) != 0;
                std::sort(sources.begin(), sources.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
                sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
                std::lock_guard lock(data.mutex);
                sources.removeIf([&](const auto &source) { return ownSource(data, source.name); });
                if (generation == data.generation.load()) {
                    data.state.sources = sources; data.state.inputRunning = found;
                    if (!found) data.state.error = QStringLiteral("NDI source discovery could not be started.");
                }
                nextDiscovery = Clock::now() + std::chrono::seconds(1);
            }
            std::unique_lock lock(data.mutex);
            data.wake.wait_for(lock, std::chrono::milliseconds(25));
        }
    }
};
NetworkVideoRuntime::NetworkVideoRuntime() : impl_(std::make_unique<Impl>()) {}
NetworkVideoRuntime::~NetworkVideoRuntime() { requestShutdown(); }
void NetworkVideoRuntime::requestShutdown() {
    impl_->worker->stop.store(true); shared().stopping.store(true);
    shared().publishing.store(0); shared().outputMask.store(0); shared().wake.notify_all();
}
bool NetworkVideoRuntime::shutdownComplete() const { return impl_->worker->done.load(); }
quint64 networkVideoOutputGeneration() { return shared().publishing.load(); }
unsigned networkVideoOutputMask() { return shared().outputMask.load(); }
void reportNetworkVideoOutputError(const QString &message) {
    auto &data = shared(); std::unique_lock lock(data.mutex, std::try_to_lock);
    if (lock) data.state.error = message;
}
void publishNetworkVideo(QByteArray composite, QByteArray alpha, int width, int height, quint64 generation, qint64 timecode) {
    if (!generation || generation != networkVideoOutputGeneration() || width <= 0 || height <= 0 || width > 8192 || height > 4320) return;
    const qsizetype bytes = qsizetype(width) * height * 4;
    if ((!composite.isEmpty() && composite.size() != bytes) || (!alpha.isEmpty() && alpha.size() != bytes)) return;
    auto &data = shared(); std::unique_lock lock(data.mutex, std::try_to_lock);
    if (!lock || generation != data.publishing.load()) return;
    const auto mask = data.outputMask.load();
    for (unsigned group = 0; group < groupMasks.size(); ++group) if (mask & groupMasks[group])
        data.publications[group].video = {composite, alpha, timecode, width, height, generation, Clock::now()};
    data.wake.notify_all();
}
void publishNetworkAudio(GstBuffer *buffer, qint64 timecode) {
    const auto generation = networkVideoOutputGeneration();
    if (!generation || !buffer) return;
    const auto size = gst_buffer_get_size(buffer);
    constexpr size_t maxBytes = 48000 * 2 * sizeof(float) / 5;
    if (!size || size > maxBytes || size % (2 * sizeof(float))) return;
    auto &data = shared(); std::unique_lock lock(data.mutex, std::try_to_lock);
    if (!lock || generation != data.publishing.load()) return;
    const auto now = timecode ? timecode : networkVideoTimecode();
    if (!data.nextAudioTimecode || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)
        || std::abs(now - data.nextAudioTimecode) > 1000000) data.nextAudioTimecode = now;
    const auto stamp = data.nextAudioTimecode;
    data.nextAudioTimecode += qint64(size / (2 * sizeof(float))) * 10000000 / 48000;
    QByteArray pcm(qsizetype(size), '\0');
    if (!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_GAP)) gst_buffer_extract(buffer, 0, pcm.data(), size);
    const auto mask = data.outputMask.load();
    for (unsigned group = 0; group < groupMasks.size(); ++group) if (mask & groupMasks[group] & ~4u) {
        auto &queue = data.publications[group];
        while (!queue.audio.empty() && queue.audioBytes + size > maxBytes) {
            queue.audioBytes -= queue.audio.front().pcm.size(); queue.audio.pop_front();
        }
        queue.audioBytes += size; queue.audio.push_back({pcm, generation, Clock::now(), stamp});
    }
    data.wake.notify_all();
}

namespace {
std::shared_ptr<NetworkVideoInputClock> inputClock(const QString &name) {
    static std::mutex mutex;
    static QHash<QString, std::weak_ptr<NetworkVideoInputClock>> clocks;
    std::lock_guard lock(mutex);
    for (auto it = clocks.begin(); it != clocks.end();) {
        if (it.value().expired()) it = clocks.erase(it); else ++it;
    }
    if (auto clock = clocks.value(name).lock()) return clock;
    auto clock = std::make_shared<NetworkVideoInputClock>(); clocks.insert(name, clock); return clock;
}
}
class NetworkVideoReceiver::Impl {
public:
    GstElement *source;
    std::jthread thread;
    QString capsDescription;
    GstClockTime nextAudioPts = GST_CLOCK_TIME_NONE;
    std::shared_ptr<NetworkVideoInputClock> timeline;
    quint64 clockEra = 0;
    bool discontinuity = true;
    explicit Impl(const QString &path, GstElement *appsrc, bool audioOnly)
        : source(GST_ELEMENT(gst_object_ref(appsrc))), timeline(inputClock(networkVideoSourceName(path))) {
        try {
        thread = std::jthread([this, path, audioOnly](std::stop_token stop) {
            try {
                QString error;
                if (!networkVideoInputAllowed(path, &error)) { fail(error); return; }
                Addon addon;
                if (!addon.context) { fail(addon.error); return; }
                const auto name = networkVideoSourceName(path).toUtf8();
#ifdef Q_OS_MACOS
                while (!stop.stop_requested() && networkVideoInputAllowed(path)) {
                    void *receiver = addon.api->createReceiver(addon.context, name.constData(), audioOnly);
                    if (!receiver) {
                        for (int i = 0; i < 20 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        continue;
                    }
                    struct ReceiverCleanup {
                        Addon &addon; void *receiver;
                        ~ReceiverCleanup() { addon.api->destroyReceiver(addon.context, receiver); }
                    } cleanup{addon, receiver};
                    // A timeout preserves the SDK receiver so its automatic
                    // reconnection can follow a source with the same name.
                    // An actual receiver error recreates it without terminating
                    // the shared appsrc or blocking the Controller.
                    while (!stop.stop_requested() && networkVideoInputAllowed(path)) {
                        if (addon.api->receive(addon.context, receiver, audioOnly, video, audio, this, 50) < 0) {
                            discontinuity = true; nextAudioPts = GST_CLOCK_TIME_NONE;
                            break;
                        }
                    }
                    if (!stop.stop_requested()) for (int i = 0; i < 20 && !stop.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
#else
                void *receiver = addon.api->createReceiver(addon.context, name.constData(), audioOnly);
                if (!receiver) { fail(QStringLiteral("NDI receiver could not be started.")); return; }
                struct ReceiverCleanup {
                    Addon &addon; void *receiver;
                    ~ReceiverCleanup() { addon.api->destroyReceiver(addon.context, receiver); }
                } cleanup{addon, receiver};
                while (!stop.stop_requested() && networkVideoInputAllowed(path)) {
                    if (addon.api->receive(addon.context, receiver, audioOnly, video, audio, this, 50) < 0) {
                        fail(QStringLiteral("NDI input was disconnected.")); break;
                    }
                }
#endif
                if (!stop.stop_requested()) gst_app_src_end_of_stream(GST_APP_SRC(source));
            } catch (...) { fail(QStringLiteral("NDI receive worker failed.")); }
        });
        } catch (const std::system_error &error) {
            fail(QStringLiteral("NDI receive worker could not start: %1")
                     .arg(QString::fromLocal8Bit(error.what())));
        }
    }
    ~Impl() { thread.request_stop(); if (thread.joinable()) thread.join(); gst_object_unref(source); }
    void fail(const QString &message) {
        const auto utf8 = message.toUtf8();
        GError *error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ, utf8.constData());
        gst_element_post_message(source, gst_message_new_error(GST_OBJECT(source), error, utf8.constData()));
        g_error_free(error);
    }
    void timestamp(GstBuffer *buffer, qint64 remote, bool audio) {
        const auto arrival = networkVideoTimecode();
        const auto stamp = timeline->map(remote, arrival, audio);
        auto *clock = gst_element_get_clock(source);
        if (!clock) return;
        const auto now = gst_clock_get_time(clock), base = gst_element_get_base_time(source);
        gst_object_unref(clock);
        if (!GST_CLOCK_TIME_IS_VALID(base) || now < base) return;
        // Small common receive latency lets independently captured video/audio
        // use the sender's relative time rather than individual packet arrivals.
        const auto delta = std::clamp<qint64>(stamp.local - arrival, -2000000, 2000000);
        const auto desired = qint64(now - base) + delta * 100 + qint64(50 * GST_MSECOND);
        GstClockTime pts = GstClockTime(std::max<qint64>(0, desired));
        if (stamp.era != clockEra || discontinuity) {
            clockEra = stamp.era; nextAudioPts = GST_CLOCK_TIME_NONE;
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT); discontinuity = false;
        }
        if (audio) {
            if (!GST_CLOCK_TIME_IS_VALID(nextAudioPts) || std::abs(qint64(pts) - qint64(nextAudioPts)) > 50 * GST_MSECOND) {
                nextAudioPts = pts; GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
            }
            pts = nextAudioPts; nextAudioPts += GST_BUFFER_DURATION(buffer);
        }
        GST_BUFFER_PTS(buffer) = pts;
    }
    void push(GstBuffer *buffer, const QString &description) {
        if (description != capsDescription) {
            auto *caps = gst_caps_from_string(description.toUtf8().constData());
            if (!caps) { gst_buffer_unref(buffer); return; }
            gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
            capsDescription = description;
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        }
        // Explicit timestamps share the source timeline; do-timestamp is only
        // a fallback while a pipeline has not acquired its clock yet.
        gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
    }
    static void video(void *value, const VtrPonNetworkVideoFrame *frame) {
        if (!frame || !frame->data || frame->width <= 0 || frame->width > 8192
            || frame->height <= 0 || frame->height > 4320 || frame->stride < frame->width * 4
            || frame->fpsNumerator <= 0 || frame->fpsDenominator <= 0) return;
        auto *self = static_cast<Impl *>(value);
        const size_t row = size_t(frame->width) * 4;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, row * frame->height, nullptr);
        if (!buffer) return;
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) { gst_buffer_unref(buffer); return; }
        for (int y = 0; y < frame->height; ++y)
            std::memcpy(map.data + size_t(y) * row, frame->data + size_t(y) * frame->stride, row);
        gst_buffer_unmap(buffer, &map);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(GST_SECOND, frame->fpsDenominator, frame->fpsNumerator);
#ifdef Q_OS_MACOS
        self->timestamp(buffer, frame->timecode, false);
#endif
        self->push(buffer, QStringLiteral("video/x-raw,format=%1,width=%2,height=%3,framerate=%4/%5,pixel-aspect-ratio=1/1")
            .arg(frame->hasAlpha ? QStringLiteral("BGRA") : QStringLiteral("BGRx"))
            .arg(frame->width).arg(frame->height).arg(frame->fpsNumerator).arg(frame->fpsDenominator));
    }
    static void audio(void *value, const VtrPonNetworkAudioFrame *frame) {
        if (!frame || !frame->data || frame->sampleRate < 8000 || frame->sampleRate > 192000
            || frame->samples <= 0 || frame->samples > 192000 || frame->channels < 1 || frame->channels > 16) return;
        // PROGRAM is stereo. Mono feeds are duplicated; for multichannel feeds
        // use channels 1/2 instead of guessing speaker positions from no mask.
        const size_t size = size_t(frame->samples) * 2 * sizeof(float);
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        if (!buffer) return;
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) { gst_buffer_unref(buffer); return; }
        auto *pcm = reinterpret_cast<float *>(map.data);
        for (int sample = 0; sample < frame->samples; ++sample) {
            pcm[size_t(sample) * 2] = frame->data[size_t(sample) * frame->channels];
            pcm[size_t(sample) * 2 + 1] = frame->data[size_t(sample) * frame->channels + (frame->channels > 1 ? 1 : 0)];
        }
        gst_buffer_unmap(buffer, &map);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(GST_SECOND, frame->samples, frame->sampleRate);
        auto *self = static_cast<Impl *>(value);
        const auto description = QStringLiteral("audio/x-raw,format=F32LE,rate=%1,channels=2,layout=interleaved")
            .arg(frame->sampleRate);
#ifdef Q_OS_MACOS
        if (description != self->capsDescription) self->discontinuity = true;
        self->timestamp(buffer, frame->timecode, true);
#else
        if (auto *clock = gst_element_get_clock(self->source)) {
            const auto now = gst_clock_get_time(clock);
            const auto base = gst_element_get_base_time(self->source);
            gst_object_unref(clock);
            if (GST_CLOCK_TIME_IS_VALID(base) && now >= base) {
                const auto running = now - base;
                // Network packets can arrive in bursts. Keep sample-contiguous
                // audio timestamps, rebasing only after a gap or format change.
                if (!GST_CLOCK_TIME_IS_VALID(self->nextAudioPts) || description != self->capsDescription
                    || (running > self->nextAudioPts && running - self->nextAudioPts > 200 * GST_MSECOND)
                    || (self->nextAudioPts > running && self->nextAudioPts - running > 200 * GST_MSECOND)) {
                    self->nextAudioPts = running;
                    GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
                }
                GST_BUFFER_PTS(buffer) = self->nextAudioPts;
                self->nextAudioPts += GST_BUFFER_DURATION(buffer);
            }
        }
#endif
        self->push(buffer, description);
    }
};
NetworkVideoReceiver::NetworkVideoReceiver(const QString &path, GstElement *source, bool audioOnly)
    : impl_(std::make_unique<Impl>(path, source, audioOnly)) {}
NetworkVideoReceiver::~NetworkVideoReceiver() = default;
}
