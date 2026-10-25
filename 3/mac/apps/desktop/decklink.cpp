#include "platform_capabilities.h"
#include "addon_platform.h"
#include "decklink.h"
#include "decklink_output_api.h"
#include "media_timecode.h"
#include "controller_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QRegularExpression>
#include <QImage>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace vtrpon {
namespace {
using Clock = std::chrono::steady_clock;
struct Video { QByteArray bytes, alpha; int width = 0, height = 0; quint64 generation = 0; Clock::time_point captured; qint64 time100ns = 0; };
struct Audio { QByteArray bytes; qint64 time100ns; };
struct Shared {
    std::mutex mutex;
    DeckLinkSettings settings;
    DeckLinkState state;
    Video video;
    std::deque<Audio> audio;
    size_t audioBytes = 0;
    std::atomic_bool stopping{false}, done{true};
    std::atomic_bool inputEnabled{true};
    std::atomic<quint64> generation{1}, publishing{0};
    std::atomic_uint captures{0}, outputMask{0};
};
Shared &shared() { static auto *s = new Shared; return *s; }
class Addon {
public:
    QLibrary library{deckLinkAddonPath()};
    const VtrPonDeckLinkApi *api = nullptr;
    const VtrPonDeckLinkMediaApi *media = nullptr;
    const VtrPonDeckLinkOutputApi *output = nullptr;
    void *context = nullptr;
    QString error;
    Addon() {
        if (!platformFeatureImplemented(PlatformFeature::ExternalAddons)) {
            error = QStringLiteral("DeckLink is not implemented on this platform."); return;
        }
#ifdef Q_OS_MACOS
        if (!addons::verifyInstalledBinary(deckLinkAddonPath(), &error)) return;
#endif
        const auto get = reinterpret_cast<VtrPonGetDeckLinkApi>(library.resolve("vtrpon_decklink_get_api"));
        const auto getMedia = reinterpret_cast<VtrPonGetDeckLinkMediaApi>(library.resolve("vtrpon_decklink_get_media_api"));
        api = get ? get(1) : nullptr; media = getMedia ? getMedia(1) : nullptr;
        if (!api || !media || api->abiVersion != 1 || api->structSize < sizeof(*api) ||
            media->abiVersion != 1 || media->structSize < sizeof(*media) || !api->addonId || !media->addonId ||
            std::strcmp(api->addonId, (api->flags & VtrPonDeckLinkTestOnly) ? "vtr-pon-decklink-test-addon" : "vtr-pon-decklink-addon") || std::strcmp(media->addonId, api->addonId) ||
            (api->flags & VtrPonDeckLinkDiscoveryOnly) ||
            (((api->flags | media->flags) & VtrPonDeckLinkTestOnly) && qEnvironmentVariableIntValue("VTRPON_DECKLINK_ALLOW_TEST_ADDON") != 1) ||
            !api->create || !api->destroy || !api->listDevices || !api->listModes || !media->listChannels || !media->openInput ||
            !media->readInput || !media->closeInput || !media->openOutput || !media->sendVideo || !media->sendAudio || !media->pumpOutput || !media->closeOutput) {
            error = QStringLiteral("DeckLink media add-on ABI 1 is required. %1").arg(library.errorString()); api = nullptr; media = nullptr; return;
        }
        const auto getOutput = reinterpret_cast<VtrPonGetDeckLinkOutputApi>(library.resolve("vtrpon_decklink_get_output_api"));
        output = getOutput ? getOutput(1) : nullptr;
        if (output && (output->abiVersion != 1 || output->structSize < sizeof(*output) || output->flags != media->flags ||
            !output->addonId || std::strcmp(output->addonId, media->addonId) || !output->openGroupedOutput || !output->startGroup)) output = nullptr;
        char detail[1024]{}; if (api->create(&context, detail, sizeof(detail))) error = QString::fromUtf8(detail);
    }
    ~Addon() { if (context) { char detail[1024]{}; api->destroy(context, detail, sizeof(detail)); } }
    QList<DeckLinkChannel> discover() {
        QList<DeckLinkChannel> result; if (!context) return result;
        std::vector<VtrPonDeckLinkDevice> devices(256); uint32_t count = 0; char detail[1024]{};
        if (api->listDevices(context, devices.data(), 256, &count, detail, sizeof(detail))) { error = QString::fromUtf8(detail); return result; }
        std::vector<VtrPonDeckLinkChannel> channels(4096);
        if (media->listChannels(context, channels.data(), 4096, &count, detail, sizeof(detail)) || count > 4096) { error = QString::fromUtf8(detail); return result; }
        for (uint32_t i = 0; i < count; ++i) {
            const auto &channel = channels[i]; if (channel.structSize != sizeof(channel) || !channel.stableId) continue;
            const auto connector = channel.connection == 1 ? QStringLiteral("SDI") : channel.connection == 2 ? QStringLiteral("HDMI") :
                channel.connection == 4 ? QStringLiteral("Optical SDI") : channel.connection == 8 ? QStringLiteral("Component") :
                channel.connection == 16 ? QStringLiteral("Composite") : channel.connection == 32 ? QStringLiteral("S-Video") : QString::number(channel.connection);
            VtrPonDeckLinkStreamOptions options{sizeof(options), channel.connection, channel.subDeviceIndex, 0, channel.persistentId};
            DeckLinkChannel item{QStringLiteral("DeckLink · %1 · CH %2 · %3").arg(QString::fromUtf8(channel.nameUtf8, int(strnlen(channel.nameUtf8, 256))))
                .arg(channel.subDeviceIndex + 1).arg(connector), deckLinkSourcePath(options), channel.direction == 1, {}};
            std::vector<VtrPonDeckLinkMode> modes(1024); uint32_t modesCount = 0;
            if (!api->listModes(context, channel.deviceIndex, channel.direction, modes.data(), 1024, &modesCount, detail, sizeof(detail)) && modesCount <= 1024)
                for (uint32_t j = 0; j < modesCount; ++j) if (modes[j].structSize == sizeof(modes[j]) && modes[j].width <= 4096 && modes[j].height <= 2160) item.modes.append(modes[j]);
            result.append(item);
        }
        error.clear(); return result;
    }
};
// Every logical output owns a context on this one owner worker (MTA on Windows). Destruction closes
// hardware before unloading the library, including exceptional worker exits.
struct OutputBundle {
    Addon &addon;
    std::array<void *, 3> contexts{};
    std::array<bool, 3> open{};
    std::array<VtrPonDeckLinkMode, 3> modes{};
    std::array<DeckLinkState::Output, 3> states{};
    std::array<Clock::time_point, 3> retry{};
    QString warning;
    bool synchronized = false;
    explicit OutputBundle(Addon &a) : addon(a) {}
    void close(unsigned i) {
        if (open[i]) { char detail[1024]{}; addon.media->closeOutput(contexts[i], detail, sizeof(detail)); }
        open[i] = false; states[i].running = false;
    }
    ~OutputBundle() {
        for (unsigned i = 0; i < 3; ++i) close(i);
        for (auto *context : contexts) if (context) { char detail[1024]{}; addon.api->destroy(context, detail, sizeof(detail)); }
    }
    int prepare(unsigned i, char *detail) {
        int status = 0;
        if (!contexts[i]) status = addon.api->create(&contexts[i], detail, 1024);
        if (!status) {
            std::array<VtrPonDeckLinkDevice, 256> devices{}; uint32_t count = 0;
            status = addon.api->listDevices(contexts[i], devices.data(), 256, &count, detail, 1024);
        }
        return status;
    }
};
QByteArray outputPixels(const Video &video, unsigned kind, const VtrPonDeckLinkMode &mode, bool premultiplied) {
    const auto &bytes = kind ? video.alpha : video.bytes;
    if (bytes.isEmpty()) return {};
    QImage image(reinterpret_cast<const uchar *>(bytes.constData()), video.width, video.height, video.width * 4,
        kind ? QImage::Format_ARGB32_Premultiplied : QImage::Format_ARGB32);
    if (image.width() != mode.width || image.height() != mode.height)
        image = image.scaled(mode.width, mode.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) return {};
    QByteArray result(reinterpret_cast<const char *>(image.constBits()), qsizetype(image.sizeInBytes()));
    if (!kind) return result;
    auto *pixels = reinterpret_cast<unsigned char *>(result.data());
    for (qsizetype i = 0; i < result.size(); i += 4) {
        const unsigned alpha = pixels[i + 3];
        for (unsigned c = 0; c < 3; ++c) {
            if (kind == 2) pixels[i + c] = static_cast<unsigned char>(alpha);
            else if (!alpha) pixels[i + c] = 0;
            else if (!premultiplied) pixels[i + c] = static_cast<unsigned char>(std::min(255u, (pixels[i + c] * 255u + alpha / 2u) / alpha));
        }
        pixels[i + 3] = 255;
    }
    return result;
}
void runtimeLoop() {
    auto &data = shared(); std::unique_ptr<Addon> addon; std::unique_ptr<OutputBundle> outputs;
    quint64 applied = 0; DeckLinkSettings appliedSettings; auto nextDiscovery = Clock::now();
    while (!data.stopping) {
        const auto now = Clock::now();
        if (now >= nextDiscovery) {
            if (!addon || !addon->context) { outputs.reset(); addon = std::make_unique<Addon>(); }
            auto channels = addon->discover();
            if (!outputs && addon->context) outputs = std::make_unique<OutputBundle>(*addon);
            { std::lock_guard lock(data.mutex); data.state.channels = channels; data.state.error = addon->error;
              data.state.availability = !addon->context ? DeckLinkState::Availability::RuntimeUnavailable :
                  channels.isEmpty() ? DeckLinkState::Availability::NoDevices : DeckLinkState::Availability::Ready; }
            nextDiscovery = now + std::chrono::seconds(1);
        }
        DeckLinkSettings settings; QList<DeckLinkChannel> channels;
        const auto generation = data.generation.load();
        { std::lock_guard lock(data.mutex); settings = data.settings; channels = data.state.channels; }
        if (!outputs) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
        auto &o = *outputs; const auto enabled = settings.outputMask(); const bool pair = (enabled & 6u) == 6u;
        if (applied != generation) {
            data.publishing = 0; data.outputMask = 0;
            const auto previousMask = appliedSettings.outputMask();
            const bool previousPair = (previousMask & 6u) == 6u;
            const bool pairChanged = previousPair != pair || (pair &&
                (appliedSettings.fillChannel != settings.fillChannel || appliedSettings.keyChannel != settings.keyChannel ||
                 appliedSettings.fillMode != settings.fillMode || appliedSettings.keyMode != settings.keyMode ||
                 appliedSettings.premultipliedFill != settings.premultipliedFill));
            // Keep unchanged channels scheduling their last frame while the GPU
            // switches generations. Editing Fill/Key must not restart Composite.
            for (unsigned i = 0; i < 3; ++i) {
                const bool changed = !applied || ((previousMask ^ enabled) & (1u << i)) ||
                    appliedSettings.channel(i) != settings.channel(i) || appliedSettings.mode(i) != settings.mode(i) ||
                    (i > 0 && pairChanged) || (i == 1 && appliedSettings.premultipliedFill != settings.premultipliedFill);
                if (changed) { o.close(i); o.states[i] = {}; o.retry[i] = now; }
            }
            if (pairChanged) { o.warning.clear(); o.synchronized = false; }
            appliedSettings = settings; applied = generation;
            std::lock_guard lock(data.mutex); data.video = {}; data.audio.clear(); data.audioBytes = 0;
        }
        std::array<VtrPonDeckLinkStreamOptions, 3> options{};
        std::array<QString, 3> invalid{};
        for (unsigned i = 0; i < 3; ++i) if (enabled & (1u << i)) {
            const auto found = std::find_if(channels.cbegin(), channels.cend(), [&](const auto &c) { return !c.input && c.path == settings.channel(i); });
            if (found == channels.cend() || !deckLinkSourceOptions(settings.channel(i), &options[i])) invalid[i] = QStringLiteral("Select an available DeckLink output channel and video format.");
            else {
                const auto mode = std::find_if(found->modes.cbegin(), found->modes.cend(), [&](const auto &m) { return m.modeId == settings.mode(i); });
                if (mode == found->modes.cend()) invalid[i] = QStringLiteral("The selected output format is unavailable.");
                else { o.modes[i] = *mode; options[i].modeId = mode->modeId; }
            }
        }
        for (unsigned i = 0; i < 3; ++i) for (unsigned j = i + 1; j < 3; ++j)
            if ((enabled & (1u << i)) && (enabled & (1u << j)) && options[i].structSize && options[j].structSize &&
                options[i].persistentId == options[j].persistentId && options[i].subDeviceIndex == options[j].subDeviceIndex)
                invalid[i] = invalid[j] = QStringLiteral("Each enabled output must use a different DeckLink hardware channel.");
        if (pair && settings.fillMode != settings.keyMode) invalid[1] = invalid[2] = QStringLiteral("Fill and Key must use the same video format.");
        if (pair && (!invalid[1].isEmpty() || !invalid[2].isEmpty())) {
            const auto failure = !invalid[1].isEmpty() ? invalid[1] : invalid[2]; invalid[1] = invalid[2] = failure;
        }
        auto fail = [&](unsigned i, const QString &failure) {
            o.close(i); o.states[i].error = failure; o.retry[i] = now + std::chrono::seconds(1);
            if (pair && i > 0) { const unsigned other = 3 - i; o.close(other); o.states[other].error = failure; o.retry[other] = o.retry[i]; o.synchronized = false; }
        };
        for (unsigned i = 0; i < 3; ++i) if (!invalid[i].isEmpty()) { if (o.open[i]) fail(i, invalid[i]); else o.states[i].error = invalid[i]; }
        auto openSingle = [&](unsigned i) {
            char detail[1024]{}; options[i].outputStartTime100ns = mediaTimecode100ns();
            int result = o.prepare(i, detail);
            if (!result) result = addon->media->openOutput(o.contexts[i], &options[i], detail, sizeof(detail));
            if (result) fail(i, *detail ? QString::fromUtf8(detail) : QStringLiteral("DeckLink output could not start."));
            else { o.open[i] = true; o.states[i].error.clear(); o.states[i].running = true; }
        };
        if ((enabled & 1u) && !o.open[0] && invalid[0].isEmpty() && now >= o.retry[0]) openSingle(0);
        if (pair && !o.open[1] && !o.open[2] && invalid[1].isEmpty() && now >= o.retry[1] && now >= o.retry[2]) {
            char detail[1024]{}; int result = o.prepare(1, detail); if (!result) result = o.prepare(2, detail);
            const auto epoch = mediaTimecode100ns(); options[1].outputStartTime100ns = options[2].outputStartTime100ns = epoch;
            o.warning.clear();
            if (!result && !addon->output) { result = VTRPON_DECKLINK_SYNC_UNAVAILABLE; std::strcpy(detail, "Update the DeckLink add-on for synchronized playback support."); }
            for (unsigned i = 1; i <= 2 && !result; ++i) {
                result = addon->output->openGroupedOutput(o.contexts[i], &options[i], epoch, detail, sizeof(detail));
                o.open[i] = result == 0;
            }
            if (!result) {
                void *members[]{o.contexts[1], o.contexts[2]}; result = addon->output->startGroup(members, 2, detail, sizeof(detail));
                o.synchronized = result == 0;
            }
            if (result == VTRPON_DECKLINK_SYNC_UNAVAILABLE) {
                o.warning = QString::fromUtf8(detail); o.close(1); o.close(2);
                // Same source frames/timestamps remain paired; hardware phase is
                // unverified without grouping and a common locked reference.
                for (unsigned i = 1; i <= 2; ++i) {
                    result = addon->media->openOutput(o.contexts[i], &options[i], detail, sizeof(detail));
                    o.open[i] = result == 0; if (result) break;
                }
            }
            if (result) fail(1, *detail ? QString::fromUtf8(detail) : QStringLiteral("Fill/Key outputs could not start."));
            else for (unsigned i = 1; i <= 2; ++i) { o.states[i].error.clear(); o.states[i].running = true; }
        } else if (!pair) {
            for (unsigned i = 1; i <= 2; ++i) if ((enabled & (1u << i)) && !o.open[i] && invalid[i].isEmpty() && now >= o.retry[i]) openSingle(i);
        }
        Video video; std::deque<Audio> audio;
        unsigned mask = 0; for (unsigned i = 0; i < 3; ++i) if (o.open[i]) mask |= 1u << i;
        data.outputMask = mask; data.publishing = mask ? applied : 0;
        if (mask) {
            { std::lock_guard lock(data.mutex); video = std::move(data.video); data.video = {}; audio.swap(data.audio); data.audioBytes = 0; }
            std::array<QByteArray, 3> pixels;
            if (video.generation == applied && now - video.captured < std::chrono::milliseconds(200))
                for (unsigned i = 0; i < 3; ++i) if (o.open[i]) pixels[i] = outputPixels(video, i, o.modes[i], settings.premultipliedFill);
            // Prepare every image before submitting either Fill/Key member.
            for (unsigned i = 0; i < 3; ++i) if (o.open[i] && !pixels[i].isEmpty()) {
                char detail[1024]{};
                const auto result = addon->media->sendVideo(o.contexts[i], reinterpret_cast<const uint8_t *>(pixels[i].constData()), o.modes[i].width, o.modes[i].height, video.time100ns, detail, sizeof(detail));
                if (result) fail(i, *detail ? QString::fromUtf8(detail) : QStringLiteral("DeckLink video output failed."));
                else ++o.states[i].sentFrames;
            }
            for (unsigned i = 0; i < 3; ++i) if (o.open[i]) {
                char detail[1024]{}; int result = 0;
                if (i != 2) for (const auto &pcm : audio) if (!result)
                    result = addon->media->sendAudio(o.contexts[i], reinterpret_cast<const float *>(pcm.bytes.constData()), uint32_t(pcm.bytes.size() / 8), pcm.time100ns, detail, sizeof(detail));
                if (!result) result = addon->media->pumpOutput(o.contexts[i], detail, sizeof(detail));
                if (result) fail(i, *detail ? QString::fromUtf8(detail) : QStringLiteral("DeckLink output disconnected or failed."));
            }
        }
        mask = 0; QStringList failures; quint64 sent = 0;
        for (unsigned i = 0; i < 3; ++i) { if (o.open[i]) mask |= 1u << i; if (!o.states[i].error.isEmpty()) failures.append(o.states[i].error); sent += o.states[i].sentFrames; }
        data.outputMask = mask; data.publishing = mask ? applied : 0;
        { std::lock_guard lock(data.mutex); data.state.outputs = o.states; data.state.outputRunning = mask != 0; data.state.outputError = failures.join(QLatin1Char('\n'));
          data.state.sentFrames = sent; data.state.syncWarning = o.warning; data.state.pairSynchronized = o.synchronized; }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    data.publishing = 0; data.outputMask = 0; outputs.reset(); addon.reset(); data.done = true;
}
struct Subscriber {
    GstElement *source;
    bool audio;
    explicit Subscriber(GstElement *s, bool a) : source(GST_ELEMENT(gst_object_ref(s))), audio(a) {}
    ~Subscriber() { gst_object_unref(source); }
};
struct Capture : std::enable_shared_from_this<Capture> {
    std::mutex mutex;
    // Successive captures of this physical input serialize SDK open/close on
    // their workers; detaching a UI consumer must never wait for the driver.
    std::shared_ptr<std::mutex> hardware = std::make_shared<std::mutex>();
    QString path;
    std::map<uint64_t, std::shared_ptr<Subscriber>> subscribers;
    uint64_t serial = 0;
    std::atomic_bool stopping{false};
    qint64 origin = -1, lastVideo = -1;
    Clock::time_point wall;
    explicit Capture(QString p) : path(std::move(p)) {}
    void error(const QString &text) {
        std::lock_guard lock(mutex);
        stopping = true;
        for (const auto &[id, subscriber] : subscribers) {
            auto *err = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ, text.toUtf8().constData());
            gst_element_post_message(subscriber->source, gst_message_new_error(GST_OBJECT(subscriber->source), err, "DeckLink input")); g_error_free(err);
        }
    }
    void push(const std::shared_ptr<Subscriber> &subscriber, const VtrPonDeckLinkPacket &p) {
        const auto bytes = subscriber->audio ? uint64_t(p.audioFrames) * 8 : p.videoBytes;
        const auto *source = subscriber->audio ? reinterpret_cast<const uint8_t *>(p.audio) : p.video;
        if (!source || !bytes || bytes > 64u * 1024u * 1024u) return;
        if (!subscriber->audio && (!p.width || p.width > 4096 || !p.height || p.height > 2160 || !p.frameDuration || !p.timeScale ||
            (p.pixelFormat != 1 && p.pixelFormat != 2) || p.rowBytes < p.width * (p.pixelFormat == 1 ? 4u : 2u) || uint64_t(p.rowBytes) * p.height != bytes)) return;
        GstCaps *caps = nullptr;
        if (subscriber->audio) caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S32LE", "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2, "layout", G_TYPE_STRING, "interleaved", nullptr);
        else {
            const bool interlaced = p.fieldDominance == 0x75707072u || p.fieldDominance == 0x6c6f7772u;
            const auto description = QStringLiteral("video/x-raw,format=%1,width=%2,height=%3,framerate=%4/%5,pixel-aspect-ratio=1/1,interlace-mode=%6")
                .arg(p.pixelFormat == 1 ? QStringLiteral("BGRA") : QStringLiteral("UYVY")).arg(p.width).arg(p.height).arg(p.timeScale).arg(p.frameDuration)
                .arg(interlaced ? QStringLiteral("interleaved") : QStringLiteral("progressive"));
            caps = gst_caps_from_string(description.toUtf8().constData());
            if (interlaced) gst_caps_set_simple(caps, "field-order", G_TYPE_STRING, p.fieldDominance == 0x75707072u ? "top-field-first" : "bottom-field-first", nullptr);
        }
        gst_app_src_set_caps(GST_APP_SRC(subscriber->source), caps); gst_caps_unref(caps);
        const auto packed = subscriber->audio ? bytes : uint64_t(p.width) * p.height * (p.pixelFormat == 1 ? 4 : 2);
        auto *buffer = gst_buffer_new_allocate(nullptr, size_t(packed), nullptr); if (!buffer) return;
        if (subscriber->audio) gst_buffer_fill(buffer, 0, source, size_t(bytes));
        else { const auto row = p.width * (p.pixelFormat == 1 ? 4 : 2); for (uint32_t y = 0; y < p.height; ++y) gst_buffer_fill(buffer, size_t(y) * row, source + size_t(y) * p.rowBytes, row); }
        const auto captureTime = subscriber->audio ? p.audioTime100ns : p.videoTime100ns;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall).count();
        const auto age = std::max<int64_t>(0, elapsed - (captureTime - origin) * 100);
        auto *clock = gst_element_get_clock(subscriber->source); const auto base = gst_element_get_base_time(subscriber->source);
        if (clock && GST_CLOCK_TIME_IS_VALID(base)) {
            const auto running = gst_clock_get_time(clock) - base; GST_BUFFER_PTS(buffer) = running >= uint64_t(age) ? running - age : 0;
        }
        if (clock) gst_object_unref(clock);
        GST_BUFFER_DURATION(buffer) = subscriber->audio ? gst_util_uint64_scale(p.audioFrames, GST_SECOND, 48000) : uint64_t(std::max<int64_t>(1, p.frameDuration100ns)) * 100;
        gst_app_src_push_buffer(GST_APP_SRC(subscriber->source), buffer);
    }
    void run() {
        std::unique_lock hardwareLock(*hardware);
        if (stopping || shared().stopping) { stopping = true; --shared().captures; return; }
        {
            Addon addon; VtrPonDeckLinkStreamOptions options{}; char detail[1024]{};
            addon.discover();
            if (!addon.context || !deckLinkSourceOptions(path, &options) || addon.media->openInput(addon.context, &options, detail, sizeof(detail)))
                error(*detail ? QString::fromUtf8(detail) : addon.error.isEmpty() ? QStringLiteral("Selected DeckLink input is unavailable.") : addon.error);
            else {
                auto lastSignal = Clock::now();
                while (!stopping && !shared().stopping) {
                    if (!shared().inputEnabled) { error(QStringLiteral("DeckLink input was disabled.")); break; }
                    VtrPonDeckLinkPacket packet{};
                    if (addon.media->readInput(addon.context, &packet, 15, detail, sizeof(detail))) { error(QString::fromUtf8(detail)); break; }
                    if (packet.noSignal) { if (Clock::now() - lastSignal > std::chrono::seconds(3)) { error(QStringLiteral("DeckLink input has no signal.")); break; } continue; }
                    if (packet.videoBytes) lastSignal = Clock::now();
                    if (Clock::now() - lastSignal > std::chrono::seconds(5)) { error(QStringLiteral("No DeckLink input frames arrived; check signal and input format.")); break; }
                    if (!packet.videoBytes && !packet.audioFrames) continue;
                    const auto timestamp = packet.videoBytes ? packet.videoTime100ns : packet.audioTime100ns;
                    if (origin < 0 || (packet.videoBytes && packet.videoTime100ns < lastVideo)) { origin = timestamp; wall = Clock::now(); }
                    if (packet.videoBytes) lastVideo = packet.videoTime100ns;
                    std::vector<std::shared_ptr<Subscriber>> targets;
                    { std::lock_guard lock(mutex); for (const auto &[id, subscriber] : subscribers) targets.push_back(subscriber); }
                    for (const auto &subscriber : targets) push(subscriber, packet);
                }
                addon.media->closeInput(addon.context, detail, sizeof(detail));
            }
        }
        stopping = true; --shared().captures;
    }
};
std::mutex capturesMutex;
std::map<QString, std::weak_ptr<Capture>> captures;
}
bool isDeckLinkSource(const QString &path) { return path.startsWith(QLatin1StringView("decklink://")); }
QString deckLinkSourcePath(const VtrPonDeckLinkStreamOptions &o) {
    return QStringLiteral("decklink://%1/%2/%3/%4").arg(QString::number(o.persistentId)).arg(o.subDeviceIndex).arg(o.connection).arg(o.modeId);
}
bool deckLinkSourceOptions(const QString &path, VtrPonDeckLinkStreamOptions *out) {
    static const QRegularExpression pattern(QStringLiteral("^decklink://(-?[0-9]+)/([0-9]+)/([0-9]+)/([0-9]+)$"));
    const auto match = pattern.match(path); if (!match.hasMatch() || !out) return false;
    bool idOk = false, subOk = false, connectionOk = false, modeOk = false;
    const auto id = match.captured(1).toLongLong(&idOk); const auto sub = match.captured(2).toUInt(&subOk);
    const auto connection = match.captured(3).toUInt(&connectionOk); const auto mode = match.captured(4).toUInt(&modeOk);
    if (!idOk || !subOk || !connectionOk || !modeOk || !connection || connection > 128 || (connection & (connection - 1))) return false;
    *out = {sizeof(*out), connection, sub, mode, id}; return deckLinkSourcePath(*out) == path;
}
QString deckLinkAddonPath() {
    const auto override = qEnvironmentVariable("VTRPON_DECKLINK_ADDON_FILE");
    return !override.isEmpty() && QDir::isAbsolutePath(override) ? override :
        addons::addonFilePath(QCoreApplication::applicationDirPath(), QStringLiteral("vtr-pon-decklink-addon"));
}
bool deckLinkAddonInstalled() {
#ifdef Q_OS_MACOS
    if (!deckLinkAddonPath().endsWith(QLatin1StringView(".dylib"))) return false;
#endif
    return platformFeatureImplemented(PlatformFeature::ExternalAddons) && QFileInfo(deckLinkAddonPath()).isFile();
}
DeckLinkSettings savedDeckLinkSettings() {
    auto s = makeControllerSettings(); return {s.value("deckLink/inputEnabled", true).toBool(), s.value("deckLink/outputEnabled", false).toBool(),
        s.value("deckLink/outputChannel").toString(), s.value("deckLink/outputMode", "0").toString().toUInt(), s.value("deckLink/inputMode", "0").toString().toUInt(),
        s.value("deckLink/compositeEnabled", true).toBool(), s.value("deckLink/fillEnabled", false).toBool(), s.value("deckLink/keyEnabled", false).toBool(),
        s.value("deckLink/fillFormat", "straight").toString() == QLatin1StringView("premultiplied"),
        s.value("deckLink/fillChannel").toString(), s.value("deckLink/keyChannel").toString(),
        s.value("deckLink/fillMode", "0").toString().toUInt(), s.value("deckLink/keyMode", "0").toString().toUInt()};
}
void applyDeckLinkSettings(const DeckLinkSettings &settings) {
    if (!platformFeatureImplemented(PlatformFeature::ExternalAddons)) return;
    auto &data = shared(); std::lock_guard lock(data.mutex); if (data.settings == settings) return;
    auto before = data.settings, after = settings;
    before.inputEnabled = after.inputEnabled; before.inputMode = after.inputMode;
    const bool outputChanged = before != after;
    data.settings = settings; data.inputEnabled = settings.inputEnabled;
    if (outputChanged) { ++data.generation; data.publishing = 0; }
}
void saveDeckLinkSettings(const DeckLinkSettings &settings) {
    if (!platformFeatureImplemented(PlatformFeature::ExternalAddons)) return;
    auto s = makeControllerSettings(); s.setValue("deckLink/inputEnabled", settings.inputEnabled); s.setValue("deckLink/outputEnabled", settings.outputEnabled);
    s.setValue("deckLink/outputChannel", settings.outputChannel); s.setValue("deckLink/outputMode", QString::number(settings.outputMode)); s.setValue("deckLink/inputMode", QString::number(settings.inputMode));
    s.setValue("deckLink/compositeEnabled", settings.compositeEnabled); s.setValue("deckLink/fillEnabled", settings.fillEnabled); s.setValue("deckLink/keyEnabled", settings.keyEnabled);
    s.setValue("deckLink/fillFormat", settings.premultipliedFill ? "premultiplied" : "straight");
    s.setValue("deckLink/fillChannel", settings.fillChannel); s.setValue("deckLink/keyChannel", settings.keyChannel);
    s.setValue("deckLink/fillMode", QString::number(settings.fillMode)); s.setValue("deckLink/keyMode", QString::number(settings.keyMode));
    s.sync(); if (s.status() == QSettings::NoError) applyDeckLinkSettings(settings);
}
DeckLinkState deckLinkState() { auto &s = shared(); std::lock_guard lock(s.mutex); return s.state; }
bool deckLinkInputAllowed(const QString &path, QString *error) {
    VtrPonDeckLinkStreamOptions options{}; const bool allowed = deckLinkAddonInstalled() && savedDeckLinkSettings().inputEnabled && deckLinkSourceOptions(path, &options);
    if (!allowed && error) *error = QStringLiteral("DeckLink input is disabled, unavailable or has an invalid channel identifier."); return allowed;
}
DeckLinkRuntime::DeckLinkRuntime() {
    if (!deckLinkAddonInstalled()) return;
    auto &s = shared(); s.stopping = false; s.settings = savedDeckLinkSettings(); s.inputEnabled = s.settings.inputEnabled; s.done = false;
    { std::lock_guard lock(s.mutex); s.state.availability = DeckLinkState::Availability::Checking; }
    std::thread([] { try { runtimeLoop(); } catch (...) { shared().publishing = 0; shared().outputMask = 0; reportDeckLinkOutputError(QStringLiteral("DeckLink worker failed.")); shared().done = true; } }).detach();
}
DeckLinkRuntime::~DeckLinkRuntime() { requestShutdown(); }
void DeckLinkRuntime::requestShutdown() { shared().stopping = true; shared().publishing = 0; }
bool DeckLinkRuntime::shutdownComplete() const { return shared().done && shared().captures == 0; }
quint64 deckLinkOutputGeneration() { return shared().publishing; }
unsigned deckLinkOutputMask() { return shared().outputMask; }
void publishDeckLinkVideo(QByteArray bytes, int width, int height, quint64 generation, qint64 time100ns) {
    publishDeckLinkVideo(std::move(bytes), {}, width, height, generation, time100ns);
}
void publishDeckLinkVideo(QByteArray bytes, QByteArray alpha, int width, int height, quint64 generation, qint64 time100ns) {
    auto &s = shared(); const auto mask = s.outputMask.load(); const auto size = qsizetype(width) * height * 4;
    if (!generation || generation != s.publishing || width <= 0 || width > 4096 || height <= 0 || height > 2160 ||
        ((mask & 1u) && bytes.size() != size) || ((mask & 6u) && alpha.size() != size)) return;
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (lock.owns_lock() && generation == s.publishing) { s.video = {std::move(bytes), std::move(alpha), width, height, generation, Clock::now(), time100ns ? time100ns : mediaTimecode100ns()}; s.state.readbackError.clear(); }
}
void publishDeckLinkAudio(GstBuffer *buffer, qint64 time100ns) {
    auto &s = shared(); if (!s.publishing || !buffer) return;
    GstMapInfo map{}; if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    if (map.size && map.size % 8 == 0 && map.size <= 48000 * 8) {
        QByteArray bytes(reinterpret_cast<const char *>(map.data), qsizetype(map.size));
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (lock.owns_lock() && s.publishing && s.audioBytes + map.size <= 48000 * 8) { s.audioBytes += map.size; s.audio.push_back({std::move(bytes), time100ns ? time100ns : mediaTimecode100ns()}); }
    }
    gst_buffer_unmap(buffer, &map);
}
void reportDeckLinkOutputError(const QString &text) { auto &s = shared(); std::unique_lock lock(s.mutex, std::try_to_lock); if (lock.owns_lock()) s.state.readbackError = text; }
class DeckLinkReceiver::Impl {
public:
    std::shared_ptr<Capture> capture;
    uint64_t token = 0;
    Impl(const QString &path, GstElement *source, bool audio) {
        if (!source) return;
        VtrPonDeckLinkStreamOptions options{};
        if (!deckLinkSourceOptions(path, &options)) return;
        const auto physical = QStringLiteral("%1/%2").arg(options.persistentId).arg(options.subDeviceIndex);
        std::lock_guard outer(capturesMutex); capture = captures[physical].lock();
        std::unique_lock<std::mutex> subscriptionLock;
        if (capture) subscriptionLock = std::unique_lock(capture->mutex);
        if (capture && !capture->stopping && capture->path != path) {
            auto *error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_BUSY,
                "This DeckLink channel is already capturing another connection or format.");
            gst_element_post_message(source, gst_message_new_error(GST_OBJECT(source), error, "DeckLink input channel is busy"));
            g_error_free(error); capture.reset(); return;
        }
        bool start = !capture || capture->stopping;
        if (start) {
            auto previous = capture;
            if (subscriptionLock.owns_lock()) subscriptionLock.unlock();
            capture = std::make_shared<Capture>(path);
            if (previous) capture->hardware = previous->hardware;
            captures[physical] = capture;
            subscriptionLock = std::unique_lock(capture->mutex);
        }
        token = ++capture->serial; capture->subscribers[token] = std::make_shared<Subscriber>(source, audio);
        if (start) { auto owner = capture; ++shared().captures; std::thread([owner] { try { owner->run(); } catch (...) { owner->error(QStringLiteral("DeckLink capture worker failed.")); owner->stopping = true; --shared().captures; } }).detach(); }
    }
    ~Impl() {
        if (!capture) return;
        std::lock_guard outer(capturesMutex);
        std::lock_guard lock(capture->mutex); capture->subscribers.erase(token); if (capture->subscribers.empty()) capture->stopping = true;
    }
};
DeckLinkReceiver::DeckLinkReceiver(const QString &path, GstElement *source, bool audio) : impl_(std::make_unique<Impl>(path, source, audio)) {}
DeckLinkReceiver::~DeckLinkReceiver() = default;
}
