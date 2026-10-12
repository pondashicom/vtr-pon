#include "deck_source_session.h"
#include "live_audio_source.h"
#include "live_audio_test_source.h"
#include "async_shutdown_test_utils.h"
#include "program_gpu_compositor.h"
#include "uvc_source_session_manager.h"
#include "uvc_thumbnail_mailbox.h"
#include "uvc_input_guard.h"
#include <QMetaObject>
#include <QTimer>
#include <thread>

#include <QApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <gst/gst.h>
#include <cmath>
#include <iostream>
#include <memory>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void pump(int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

std::atomic_bool slowStopEntered = false;
struct SlowStopElement { GstElement parent; };
struct SlowStopElementClass { GstElementClass parent; };
G_DEFINE_TYPE(SlowStopElement, slow_stop_element, GST_TYPE_ELEMENT)
GstStateChangeReturn slowStopChangeState(GstElement *element, GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_NULL) {
        slowStopEntered.store(true);
        QThread::msleep(300);
    }
    return GST_ELEMENT_CLASS(slow_stop_element_parent_class)->change_state(element, transition);
}
void slow_stop_element_class_init(SlowStopElementClass *klass)
{
    GST_ELEMENT_CLASS(klass)->change_state = slowStopChangeState;
}
void slow_stop_element_init(SlowStopElement *) {}

std::atomic_bool holdUvcStop{false};
std::atomic_bool uvcStopEntered{false};
std::atomic_int uvcOpenCount{0};
struct FakeUvcSource { GstBin parent; };
struct FakeUvcSourceClass { GstBinClass parent; };
G_DEFINE_TYPE(FakeUvcSource, fake_uvc_source, GST_TYPE_BIN)
GstStateChangeReturn fakeUvcChangeState(GstElement *element, GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_NULL) {
        uvcStopEntered.store(true);
        while (holdUvcStop.load()) QThread::msleep(5);
    }
    return GST_ELEMENT_CLASS(fake_uvc_source_parent_class)->change_state(element, transition);
}
void fake_uvc_source_class_init(FakeUvcSourceClass *klass)
{
    auto *objectClass = G_OBJECT_CLASS(klass);
    objectClass->set_property = [](GObject *, guint, const GValue *, GParamSpec *) {};
    objectClass->get_property = [](GObject *, guint id, GValue *value, GParamSpec *) {
        if (id == 1) g_value_set_boolean(value, TRUE);
        else g_value_set_string(value, "test-camera");
    };
    g_object_class_install_property(objectClass, 1, g_param_spec_boolean(
        "do-timestamp", "Timestamp", "Test timestamp property", TRUE, G_PARAM_READWRITE));
    g_object_class_install_property(objectClass, 2, g_param_spec_string(
        "device-path", "Device", "Test device property", "", G_PARAM_READWRITE));
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
        "Test camera", "Source/Video", "Controllable camera retirement", "VTR-PON tests");
    GST_ELEMENT_CLASS(klass)->change_state = fakeUvcChangeState;
}
void fake_uvc_source_init(FakeUvcSource *self)
{
    ++uvcOpenCount;
    auto *source = gst_element_factory_make("videotestsrc", nullptr);
    g_object_set(source, "is-live", TRUE, "do-timestamp", TRUE, nullptr);
    gst_bin_add(GST_BIN(self), source);
    auto *pad = gst_element_get_static_pad(source, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}

bool testAsyncStop()
{
    auto *pipeline = gst_pipeline_new("delayed-retirement");
    auto *slow = GST_ELEMENT(g_object_new(slow_stop_element_get_type(), nullptr));
    gst_bin_add(GST_BIN(pipeline), slow);
    gst_element_set_state(pipeline, GST_STATE_READY);
    auto released = std::make_shared<std::atomic_bool>(false);
    auto cleaned = std::make_shared<std::atomic_bool>(false);
    g_object_weak_ref(G_OBJECT(pipeline), [](gpointer data, GObject *) {
        std::unique_ptr<std::shared_ptr<std::atomic_bool>> retained(
            static_cast<std::shared_ptr<std::atomic_bool> *>(data));
        (*retained)->store(true);
    }, new std::shared_ptr<std::atomic_bool>(released));
    QElapsedTimer elapsed;
    elapsed.start();
    auto job = AsyncPipelineStop::start(pipeline, [cleaned] { cleaned->store(true); });
    gst_object_unref(pipeline); // The caller can relinquish ownership immediately.
    bool ok = require(elapsed.elapsed() < 100, "slow NULL transition never blocks the requesting thread");
    int ticks = 0;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; });
    timer.start();
    pump(100);
    ok &= require(slowStopEntered.load() && !job->done() && !released->load() && ticks >= 3,
                  "event loop continues while the worker retains a stopping backend");
    pump(300);
    ok &= require(job->succeeded() && cleaned->load() && released->load(),
                  "completion is published only after confirmed stop, cleanup, and final release");
    return ok;
}

bool testPreparationTimeout()
{
    auto *pipeline = gst_pipeline_new("blocked-preparation");
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto release = std::make_shared<std::atomic_bool>(false);
    auto stopped = std::make_shared<std::atomic_bool>(false);
    auto job = AsyncFilePreparation::startWithWork(pipeline,
        [entered, release](GstElement *, const auto &) {
            entered->store(true);
            while (!release->load()) QThread::msleep(2);
            AsyncFilePreparation::Result result;
            result.state = AsyncFilePreparation::State::Ready;
            return result;
        }, 40);
    int ticks = 0;
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; });
    timer.start();
    pump(100);
    bool ok = require(entered->load() && !job->done() && ticks >= 3
        && job->state() == AsyncFilePreparation::State::Offline,
        "an unresponsive file becomes offline while the GUI timer continues");
    // A late success must remain offline, even before cancellation.
    release->store(true);
    pump(30);
    ok &= require(job->done() && job->state() == AsyncFilePreparation::State::Offline,
        "late preparation success cannot revive a timed-out source");
    entered->store(false); release->store(false);
    auto active = AsyncFilePreparation::startWithWork(pipeline,
        [entered, release](GstElement *, const auto &) {
            entered->store(true);
            while (!release->load()) QThread::msleep(2);
            return AsyncFilePreparation::Result{};
        });
    auto stop = AsyncPipelineStop::start(pipeline, [stopped] { stopped->store(true); },
        AsyncFilePreparation::retire(active));
    pump(30);
    ok &= require(!stop->done() && !stopped->load(),
        "retirement waits for the old file worker without stopping the GUI");
    release->store(true);
    pump(50);
    ok &= require(active->state() == AsyncFilePreparation::State::Cancelled
        && stop->succeeded() && stopped->load(),
        "cancelled preparation retires only after the worker releases the pipeline");
    gst_object_unref(pipeline);
    return ok;
}

bool testThumbnailCoalescing()
{
    auto mailbox = std::make_shared<UvcThumbnailMailbox>();
    QObject receiver;
    int queued = 0;
    int delivered = 0;
    QRgb lastPixel = 0;
    for (int burst = 1; burst <= 3; ++burst) {
        // Hold GUI delivery while capture continues producing frames.
        std::thread capture([&, burst] {
            for (int frame = 0; frame < 1000; ++frame) {
                QImage image(120, 68, QImage::Format_RGBA8888);
                image.fill(QColor::fromRgb(burst, frame / 256, frame % 256));
                if (mailbox->publish(std::move(image))) {
                    ++queued;
                    QMetaObject::invokeMethod(&receiver, [&, mailbox] {
                        lastPixel = mailbox->consume().pixel(0, 0);
                        ++delivered;
                    }, Qt::QueuedConnection);
                }
            }
        });
        capture.join();
        if (!require(queued == burst && delivered == burst - 1,
                     "one pending thumbnail notification during a GUI stall")) return false;
        const QRgb expected = qRgb(burst, 999 / 256, 999 % 256);
        if (!require(mailbox->latest().pixel(0, 0) == expected,
                     "latest thumbnail is available before notification delivery")) return false;
        QCoreApplication::processEvents();
        if (!require(delivered == burst && lastPixel == expected,
                     "notification delivers newest frame, not queued old frame")) return false;
    }
    // Independent sessions do not share pending flags or retained images.
    UvcThumbnailMailbox replacement;
    if (!require(replacement.latest().isNull(), "new session has no old thumbnail")) return false;
    QImage next(120, 68, QImage::Format_RGBA8888);
    next.fill(Qt::green);
    if (!require(replacement.publish(next) && mailbox->publish(next),
                 "delivery rearms the old mailbox and new session starts independently")) return false;
    std::cout << "Thumbnail coalescing: 3000 frames, 3 notifications, latest-frame delivery passed\n";
    return true;
}

QString createTone(const QString &directory)
{
    const auto path = directory + QStringLiteral("/deck-source-tone.wav");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 samples = 48000 * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + samples * 4);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
           << quint32(192000) << quint16(4) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(samples * 4);
    for (quint32 index = 0; index < samples; ++index) {
        const auto sample = static_cast<qint16>(std::sin(index * 2.0 * 3.141592653589793
                                                       * 440.0 / 48000.0) * 4096.0);
        stream << sample << sample;
    }
    return stream.status() == QDataStream::Ok ? path : QString{};
}

QString createPdf(const QString &directory)
{
    const auto path = directory + QStringLiteral("/deck-source-pages.pdf");
    bool written = false;
    {
        QPdfWriter writer(path);
        writer.setResolution(72);
        const QPageSize size(QSizeF(320, 180), QPageSize::Point,
                             QStringLiteral("Deck Source Test"), QPageSize::ExactMatch);
        if (!writer.setPageLayout(QPageLayout(size, QPageLayout::Portrait,
                                               QMarginsF(), QPageLayout::Point))) return {};
        QPainter painter(&writer);
        if (!painter.isActive()) return {};
        const QRect page(0, 0, writer.width(), writer.height());
        painter.fillRect(page, QColor(210, 35, 35));
        if (!writer.newPage()) return {};
        painter.fillRect(page, QColor(35, 65, 210));
        written = painter.end();
    }
    return written && QFileInfo(path).size() > 0 ? path : QString{};
}

// File audio is sent only to the PCM bridge. There is deliberately no PROGRAM
// audio output here, so none of these tests can open a physical output device.
class Fixture {
public:
    ~Fixture() { (void)shutdown(); }

    bool initialize()
    {
        video.resize(320, 180);
        monitor.resize(320, 180);
        video.setAttribute(Qt::WA_NativeWindow);
        monitor.setAttribute(Qt::WA_NativeWindow);
        compositor = std::make_unique<ProgramGpuCompositor>(
            static_cast<std::uintptr_t>(video.winId()),
            static_cast<std::uintptr_t>(monitor.winId()),
            ProgramGpuCompositor::OutputGeometry{320, 180});
        if (!compositor->start() || !deck.initialize(0, 320, 180)) return false;
        deck.attachSourceManagers(&uvc, &powerPoint, &pdf, compositor.get());
        GstContext *context = compositor->createInputContext();
        if (!context) return false;
        uvc.setProgramContext(context);
        powerPoint.setProgramContext(context);
        pdf.setProgramContext(context);
        gst_context_unref(context);
        clock = gst_system_clock_obtain();
        gst_pipeline_use_clock(GST_PIPELINE(deck.pipeline()), clock);
        return compositor->configureDeckInput(0, deck.pipeline(), deck.appSink());
    }

    bool shutdown()
    {
        if (stopped_) return true;
        const bool detached = awaitShutdown([&] { return !compositor || compositor->prepareShutdown(); });
        if (!detached || !awaitShutdown([&] { return !deck.retirementPending(); })) return false;
        deck.detachCallbacks();
        const bool pipelinesStopped = deck.stopPipelinesForShutdown();
        powerPoint.requestShutdown();
        pdf.requestShutdown();
        const bool powerPointStopped = awaitShutdown([&] { return powerPoint.shutdownComplete(); });
        const bool pdfStopped = awaitShutdown([&] { return pdf.shutdownComplete(); });
        const bool compositorStopped = !compositor || compositor->stop();
        uvc.requestShutdown();
        const bool uvcStopped = awaitShutdown([&] { return uvc.shutdownComplete(); });
        stopped_ = detached && pipelinesStopped && powerPointStopped
            && pdfStopped && compositorStopped && uvcStopped;
        if (stopped_) deck.releaseResourcesAfterConfirmedShutdown();
        if (clock) {
            gst_object_unref(clock);
            clock = nullptr;
        }
        return stopped_;
    }

    QWidget video;
    QWidget monitor;
    std::unique_ptr<ProgramGpuCompositor> compositor;
    UvcSourceSessionManager uvc{false};
    PowerPointSourceSessionManager powerPoint{320, 180};
    PdfSourceSessionManager pdf{320, 180};
    DeckSourceSession deck;
    GstClock *clock = nullptr;

private:
    bool stopped_ = false;
};

bool reachesState(GstElement *pipeline, GstState state)
{
    GstState current = GST_STATE_VOID_PENDING;
    return gst_element_get_state(pipeline, &current, nullptr, 2 * GST_SECOND)
        != GST_STATE_CHANGE_FAILURE && current == state;
}

bool awaitReady(DeckSourceSession &deck, const char *message)
{
    QElapsedTimer elapsed;
    elapsed.start();
    DeckSourceSession::PreparationResult result;
    do {
        pump(10);
        result = deck.refreshPreparation();
    } while (result.state == DeckSourceSession::PreparationState::Loading
             && elapsed.elapsed() < 5000);
    if (result.state != DeckSourceSession::PreparationState::Ready)
        std::cerr << result.errorDetail.toStdString() << '\n';
    return require(result.state == DeckSourceSession::PreparationState::Ready, message);
}

bool awaitFileError(DeckSourceSession &deck, DeckSourceSession::PreparationResult result)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QString busError;
    while (result.state != DeckSourceSession::PreparationState::Error
           && busError.isEmpty() && elapsed.elapsed() < 5000) {
        pump(10);
        // As in DualDeckPlayer, the owner consumes File pipeline errors.
        // A failed asynchronous preroll need not fail set_state immediately.
        while (GstMessage *message = gst_bus_pop(deck.bus())) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *error = nullptr;
                gst_message_parse_error(message, &error, nullptr);
                if (error) {
                    busError = QString::fromUtf8(error->message);
                    g_error_free(error);
                }
            }
            gst_message_unref(message);
        }
        result = deck.refreshPreparation();
    }
    return require((result.state == DeckSourceSession::PreparationState::Error
                        && !result.errorDetail.isEmpty()) || !busError.isEmpty(),
                   "a missing File reports an immediate or asynchronous preparation error");
}

bool fileContract(Fixture &fixture, const QString &path)
{
    auto &deck = fixture.deck;
    DualDeckPlayer::DeckSource source;
    source.type = DualDeckPlayer::DeckSource::Type::File;
    source.locator = path;
    source.inPointMs = 250;
    source.outPointMs = 1750;
    const auto preparation = deck.prepare(source, 1.0, fixture.clock);
    if (!require(preparation.state == DeckSourceSession::PreparationState::Loading,
                 "File preparation enters asynchronous preroll")
        || !awaitReady(deck, "File preroll and In/Out preparation reach Ready")) return false;
    bool ok = require(deck.sourceType() == DeckSourceSession::Type::File
                          && deck.hasAudio() && !deck.hasVideo()
                          && deck.inPointMs() == 250 && deck.outPointMs() == 1750
                          && std::abs(deck.durationMs() - 2000) <= 50,
                      "prepared File exposes audio-only metadata, duration, and the requested range");
    ok &= require(reachesState(deck.pipeline(), GST_STATE_PAUSED)
                      && std::abs(deck.positionMs() - 250) <= 100,
                  "File CUE stays paused at its In point");
    ok &= require(deck.seek(700) && reachesState(deck.pipeline(), GST_STATE_PAUSED),
                  "seek while paused preserves the paused transport state");
    ok &= require(std::abs(deck.positionMs() - 700) <= 100,
                  "paused File seek reaches the requested source position");
    ok &= require(deck.setFileState(GST_STATE_PLAYING)
                      && reachesState(deck.pipeline(), GST_STATE_PLAYING),
                  "prepared File starts playback through its owned pipeline");
    const auto positionBeforePlayback = deck.positionMs();
    // PLAYING is reached before the first clocked audio buffer is rendered.
    // Allow startup to settle before requiring 50 ms of source clock progress.
    QElapsedTimer playbackProgress;
    playbackProgress.start();
    while (deck.positionMs() < positionBeforePlayback + 50
           && playbackProgress.elapsed() < 1000) pump(10);
    const auto positionAfterPlayback = deck.positionMs();
    const bool clockAdvanced = deck.sourceIsPlaying()
        && positionBeforePlayback >= 0 && positionAfterPlayback >= positionBeforePlayback + 50;
    if (!clockAdvanced) {
        GstState actual = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        gst_element_get_state(deck.pipeline(), &actual, &pending, 0);
        std::cerr << "File clock: before=" << positionBeforePlayback
                  << " after=" << positionAfterPlayback << " waitMs=" << playbackProgress.elapsed()
                  << " state=" << gst_element_state_get_name(actual)
                  << " pending=" << gst_element_state_get_name(pending) << '\n';
    }
    ok &= require(clockAdvanced,
                  "File clock advances without any physical audio output");
    ok &= require(deck.seek(900) && reachesState(deck.pipeline(), GST_STATE_PLAYING),
                  "seek while playing retains the playing transport state");
    ok &= require(deck.setRate(1.5), "File playback-rate change remains supported");
    ok &= require(deck.setFileState(GST_STATE_PAUSED)
                      && reachesState(deck.pipeline(), GST_STATE_PAUSED),
                  "File can pause after a rate change");
    const auto standby = deck.standbyAfterTake();
    ok &= require(standby.succeeded && standby.ownershipReleased && !standby.stopConfirmed
                      && reachesState(deck.pipeline(), GST_STATE_NULL)
                      && deck.sourceType() == DeckSourceSession::Type::None,
                  "off-air File retires its old graph and leaves a fresh NULL deck");
    const auto cleared = deck.clearForReplacement();
    if (cleared.stopConfirmed) {
        GstState current = GST_STATE_VOID_PENDING;
        const auto stateResult = gst_element_get_state(deck.pipeline(), &current, nullptr, 0);
        ok &= require(stateResult != GST_STATE_CHANGE_FAILURE && current == GST_STATE_NULL,
                      "a reported File stop confirmation already corresponds to NULL");
    }
    ok &= require(cleared.succeeded && cleared.ownershipReleased
                      && reachesState(deck.pipeline(), GST_STATE_NULL)
                      && deck.sourceType() == DeckSourceSession::Type::None,
                  "replacement clear reaches NULL and releases the old File identity");

    source.inPointMs = 0;
    source.outPointMs = -1;
    deck.prepare(source, 1.0, fixture.clock);
    ok &= awaitReady(deck, "the same deck can CUE a File again after replacement clear");
    const auto offAir = deck.clearAfterOffAir();
    ok &= require(offAir.succeeded && offAir.ownershipReleased
                      && deck.sourceType() == DeckSourceSession::Type::None,
                  "explicit off-air clear releases the File source");
    source.locator = QFileInfo(path).absolutePath() + QStringLiteral("/does-not-exist.wav");
    const auto missing = deck.prepare(source, 1.0, fixture.clock);
    ok &= awaitFileError(deck, missing);
    return ok;
}

bool blockedFileReplacementContract(Fixture &fixture, const QString &path)
{
    auto &deck = fixture.deck;
    if (!deck.clearForReplacement().succeeded) return false;
    struct Gate { std::atomic_bool entered{false}, release{false}; };
    auto gate = std::make_shared<Gate>();
    g_signal_connect_data(deck.pipeline(), "source-setup", G_CALLBACK(+[](GstElement *, GstElement *, gpointer data) {
        const auto &gate = *static_cast<std::shared_ptr<Gate> *>(data);
        gate->entered.store(true);
        while (!gate->release.load()) QThread::msleep(2);
    }), new std::shared_ptr<Gate>(gate), +[](gpointer data, GClosure *) {
        delete static_cast<std::shared_ptr<Gate> *>(data);
    }, GConnectFlags(0));
    DualDeckPlayer::DeckSource source;
    source.type = DualDeckPlayer::DeckSource::Type::File;
    source.locator = path;
    QElapsedTimer elapsed; elapsed.start();
    const auto prepared = deck.prepare(source, 1.0, fixture.clock);
    bool ok = require(elapsed.elapsed() < 100 && prepared.state == DeckSourceSession::PreparationState::Loading,
        "a blocked source open returns control to the GUI immediately");
    for (int i = 0; i < 200 && !gate->entered.load(); ++i) { deck.refreshPreparation(); pump(5); }
    ok &= require(gate->entered.load(), "the real playbin source-setup callback is held in its worker");
    elapsed.restart();
    const auto retired = deck.clearForReplacement();
    ok &= require(retired.succeeded && elapsed.elapsed() < 200,
        "replacing a blocked source never waits for its state lock or NULL transition");
    if (retired.succeeded) {
        const auto next = deck.prepare(source, 1.0, fixture.clock);
        ok &= require(next.state == DeckSourceSession::PreparationState::Loading,
            "replacement graph accepts another file while the old source remains blocked");
        ok &= awaitReady(deck, "replacement file reaches Ready independently of the blocked source");
    }
    gate->release.store(true);
    pump(100);
    ok &= require(deck.refreshPreparation().state == DeckSourceSession::PreparationState::Ready,
        "late completion of the retired source cannot overwrite the new standby state");
    return ok;
}

bool uvcRetirementContract(Fixture &fixture)
{
    auto *registry = gst_registry_get();
    if (auto *feature = gst_registry_find_feature(registry, "mfvideosrc", GST_TYPE_ELEMENT_FACTORY)) {
        gst_registry_remove_feature(registry, feature);
        gst_object_unref(feature);
    }
    if (!require(gst_element_register(nullptr, "mfvideosrc", GST_RANK_NONE, fake_uvc_source_get_type()),
                 "test camera replaces mfvideosrc without a physical device")) return false;
    DualDeckPlayer::DeckSource request;
    request.type = DualDeckPlayer::DeckSource::Type::Uvc;
    request.locator = QStringLiteral("async-retirement-test-camera");
    auto &deck = fixture.deck;
    if (!require(deck.prepare(request).state == DeckSourceSession::PreparationState::Loading,
                 "test camera starts preparing") || !awaitReady(deck, "test camera reaches Ready")) return false;
    const int opened = uvcOpenCount.load();
    holdUvcStop.store(true);
    uvcStopEntered.store(false);
    struct ReleaseStop { ~ReleaseStop() { holdUvcStop.store(false); } } releaseStop;
    QElapsedTimer elapsed;
    elapsed.start();
    bool ok = require(deck.clearForReplacement().succeeded && elapsed.elapsed() < 100,
                      "last shared camera release returns without waiting for its driver");
    ok &= require(awaitShutdown([] { return uvcStopEntered.load(); }, 2000),
                  "camera stop worker reaches the deliberately blocked driver");
    auto pending = deck.prepare(request);
    ok &= require(pending.state == DeckSourceSession::PreparationState::Loading && pending.errorDetail.isEmpty(),
                  "same-camera reacquire stays Loading while the old session retires");
    pump(50);
    pending = deck.refreshPreparation();
    ok &= require(pending.state == DeckSourceSession::PreparationState::Loading && uvcOpenCount.load() == opened,
                  "pending reacquire does not open a duplicate device session");
    holdUvcStop.store(false);
    ok &= awaitReady(deck, "same-camera reacquire completes after old session cleanup");
    ok &= require(uvcOpenCount.load() == opened + 1, "exactly one replacement camera opens");

    holdUvcStop.store(true);
    uvcStopEntered.store(false);
    ok &= require(deck.clearForReplacement().succeeded, "replacement can be retired again");
    ok &= require(awaitShutdown([] { return uvcStopEntered.load(); }, 2000), "second retirement starts");
    ok &= require(deck.prepare(request).state == DeckSourceSession::PreparationState::Loading,
                  "second reacquire is pending");
    ok &= require(deck.clearForReplacement().succeeded, "CLEAR cancels the pending reacquire");
    holdUvcStop.store(false);
    ok &= require(awaitShutdown([&] { return !fixture.uvc.sourceIsRetiring(request.locator); }),
                  "canceled camera's retirement completes");
    deck.refreshPreparation();
    ok &= require(deck.sourceType() == DeckSourceSession::Type::None && uvcOpenCount.load() == opened + 1,
                  "late retirement completion does not resurrect a canceled CUE");
    return ok;
}

PdfCommandResponse navigatePdf(DeckSourceSession &deck, bool forward)
{
    QElapsedTimer timer;
    timer.start();
    PdfCommandResponse response;
    do {
        response = forward ? deck.nextPdfPage() : deck.previousPdfPage();
        if (response.result != PdfCommandResult::Busy) return response;
        pump(10);
    } while (timer.elapsed() < 5000);
    return response;
}

bool liveAudioContract(Fixture &fixture)
{
    if (!require(substituteLiveAudioSource(), "live audio uses a synthetic capture source")) return false;
    auto &deck = fixture.deck;
    const int camerasBefore = uvcOpenCount.load();
    const QString endpoint = QStringLiteral("{0.0.1.00000000}.USB audio 入力 / %1");
    DualDeckPlayer::DeckSource source;
    source.type = DualDeckPlayer::DeckSource::Type::Uvc;
    source.locator = vtrpon::liveAudioSourcePath(endpoint);
    const auto prepared = deck.prepare(source, 0.5, fixture.clock);
    if (!require(prepared.state == DeckSourceSession::PreparationState::Loading,
                 "audio-only preparation starts without requesting video") ||
        !awaitReady(deck, "audio-only input reaches Ready without a camera frame")) return false;
    bool ok = require(deck.hasAudio() && !deck.hasVideo() && deck.sourceIsPlaying() &&
                      deck.positionMs() == -1 && deck.durationMs() == -1 &&
                      !deck.seek(100) && !deck.setRate(1.5) &&
                      !fixture.compositor->deckFrameReady(0) && uvcOpenCount.load() == camerasBefore,
                      "live audio has no video, duration, seek or rate and never opens a camera");
    auto *input = gst_bin_get_by_name(GST_BIN(deck.audioPipeline()), "deck-a-uvc-audio-source");
    gchar *id = nullptr;
    g_object_get(input, "device", &id, nullptr);
    ok &= require(QString::fromUtf8(id) == endpoint, "the exact selected endpoint is passed to the capture source");
    g_free(id);
    gst_object_unref(input);
    pump(150);
    GstMessage *level = gst_bus_pop_filtered(deck.mappedAudioBus(), GST_MESSAGE_ELEMENT);
    ok &= require(level && GST_MESSAGE_SRC(level) == GST_OBJECT(deck.inputAudioLevel()),
                  "live input produces level messages on the existing metering route");
    if (level) gst_message_unref(level);
    deck.setLiveFrameUpdates(false);
    ok &= require(deck.uvcProgramPaused() && deck.sourceIsPlaying(), "pausing gates the live route while capture remains live");
    deck.setLiveFrameUpdates(true);
    ok &= require(!deck.uvcProgramPaused(), "live input can resume without seeking");
    const auto cleared = deck.clearAfterOffAir();
    ok &= require(cleared.succeeded && cleared.ownershipReleased && !deck.hasAudio(),
                  "OFF AIR detaches and retires the audio capture");
    ok &= require(awaitShutdown([&] { return !deck.retirementPending(); }), "live capture retirement completes");
    deck.prepare(source, 1.0, fixture.clock);
    ok &= awaitReady(deck, "the endpoint can be prepared again after OFF AIR");
    ok &= require(deck.clearForReplacement().succeeded, "audio-to-file replacement releases the capture route");
    ok &= require(awaitShutdown([&] { return !deck.retirementPending(); }), "replacement capture retirement completes");
    source.locator = vtrpon::liveAudioSourcePath(QStringLiteral("missing-input"));
    ok &= require(deck.prepare(source, 1.0, fixture.clock).state == DeckSourceSession::PreparationState::Error,
                  "an unavailable endpoint fails preparation without waiting for video");
    ok &= require(deck.clearForReplacement().succeeded, "failed live capture can be cleared safely");
    ok &= require(awaitShutdown([&] { return !deck.retirementPending(); }), "failed capture is retired before the next source");
    return ok;
}

bool pdfContract(Fixture &fixture, const QString &path)
{
    auto &deck = fixture.deck;
    DualDeckPlayer::DeckSource source;
    source.type = DualDeckPlayer::DeckSource::Type::Pdf;
    source.locator = path;
    const auto preparation = deck.prepare(source, 1.0, fixture.clock);
    if (!require(preparation.state == DeckSourceSession::PreparationState::Loading,
                 "PDF preparation enters frame preroll")
        || !awaitReady(deck, "PDF produces a shared D3D11 frame and reaches Ready")) return false;
    bool ok = require(deck.sourceType() == DeckSourceSession::Type::Pdf
                          && deck.hasVideo() && !deck.hasAudio() && deck.sourceIsPlaying()
                          && fixture.compositor->deckFrameReady(0),
                      "native PDF preparation exposes a ready silent video source");
    const auto initial = deck.pdfNavigationState();
    ok &= require(initial.active && initial.currentPage == 1 && initial.pageCount == 2
                      && initial.canGoNext && !initial.canGoPrevious,
                  "prepared PDF navigation starts on the first of two pages");
    ok &= require(navigatePdf(deck, true).result == PdfCommandResult::Completed
                      && deck.pdfNavigationState().currentPage == 2,
                  "PDF NEXT advances through the owned session");
    ok &= require(navigatePdf(deck, true).result == PdfCommandResult::Boundary
                      && deck.pdfNavigationState().currentPage == 2,
                  "PDF page boundary leaves the selected page unchanged");
    ok &= require(navigatePdf(deck, false).result == PdfCommandResult::Completed
                      && deck.pdfNavigationState().currentPage == 1,
                  "PDF PREV restores the first page");
    const auto cleared = deck.clearForReplacement();
    ok &= require(cleared.succeeded && cleared.detached && cleared.ownershipReleased
                      && !cleared.stopConfirmed && deck.sourceType() == DeckSourceSession::Type::None
                      && !fixture.pdf.navigationState(0).active,
                  "PDF replacement detaches the compositor and releases its owned session");

    deck.prepare(source, 1.0, fixture.clock);
    if (!awaitReady(deck, "PDF can be prepared again after its previous session is released")) return false;
    // Exercise a real refusal at the compositor boundary. Once shutdown has
    // detached its inputs, reconfiguration must fail; no blocked driver or
    // artificial NULL timeout is needed to verify retained ownership.
    if (!require(fixture.compositor->prepareShutdown(),
                 "compositor safely enters shutdown before the detach-refusal test")) return false;
    const auto standby = deck.standbyAfterTake();
    ok &= require(standby.succeeded && !standby.detached && !standby.ownershipReleased
                      && !standby.stopConfirmed && deck.sourceType() == DeckSourceSession::Type::Pdf
                      && fixture.pdf.navigationState(0).active,
                  "TAKE standby keeps the completed transition valid while retaining an undetached PDF identity");
    const auto replacement = deck.clearForReplacement();
    ok &= require(!replacement.succeeded && !replacement.ownershipReleased && !replacement.stopConfirmed
                      && deck.sourceType() == DeckSourceSession::Type::Pdf
                      && fixture.pdf.navigationState(0).active,
                  "replacement refusal preserves the retained PDF session for shutdown cleanup");
    return ok;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (!testThumbnailCoalescing()) return 1;
    gst_init(nullptr, nullptr);
    if (!testAsyncStop() || !testPreparationTimeout()) return 1;
    {
        const QString ownPath=QStringLiteral("opaque-self-camera-test-path");
        vtrpon::UvcSelfCameraRegistration registration;
        QString reason;
        UvcSourceSessionManager guarded(false);
        if (!require(!guarded.ensureSource(ownPath,&reason) && !reason.isEmpty(),
                     "UVC cannot open before self-camera identification completes")) return 1;
        registration.finish(ownPath);
        if (!require(!guarded.ensureSource(ownPath.toUpper(),&reason) && reason.contains(QStringLiteral("feedback")),
                     "self camera is rejected at the shared UVC source boundary")) return 1;
        UvcSourceSessionManager::ProgramSource program;
        if (!require(!guarded.bindProgramDeck(0,ownPath,program,&reason) && !program.isValid(),
                     "saved self-camera item cannot bind to Program")) return 1;
        if (!require(vtrpon::uvcInputAllowed(QStringLiteral("other-camera-path")),
                     "other cameras remain eligible after registration")) return 1;
    }
    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary fixture directory is available")) return 1;
    const QString tone = createTone(directory.path());
    const QString pdf = createPdf(directory.path());
    if (!require(!tone.isEmpty() && !pdf.isEmpty(),
                 "temporary File and PDF fixtures are created")) return 1;
    Fixture fixture;
    if (!require(fixture.initialize(), "deck, source managers, and native compositor initialize")) return 1;
    bool ok = fileContract(fixture, tone);
    ok &= blockedFileReplacementContract(fixture, tone);
    ok &= uvcRetirementContract(fixture);
    ok &= liveAudioContract(fixture);
    // PDF's final case deliberately shuts the compositor down to exercise
    // refusal/retained ownership, so no new input may be prepared afterwards.
    ok &= pdfContract(fixture, pdf);
    ok &= require(fixture.shutdown(), "aggregate shutdown releases pipelines and the retained PDF session");
    ok &= require(!fixture.pdf.navigationState(0).active, "shutdown leaves no retained PDF session");
    std::cout << "Deck source session contract: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
