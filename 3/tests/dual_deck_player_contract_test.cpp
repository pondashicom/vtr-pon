#include "dual_deck_player.h"
#include "async_shutdown_test_utils.h"
#include "uvc_source_session_manager.h"
#include "live_audio_source.h"
#include "live_audio_test_source.h"

#include <QApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLoggingCategory>
#include <QPainter>
#include <QPdfWriter>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <gst/gst.h>

#include <cmath>
#include <iostream>

namespace {
bool require(bool condition, const char *message)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    return condition;
}
#define CHECK(condition, message) do { if (!require((condition), message)) return false; } while (false)

// This replacement exists only in this test process. Production initialization
// still uses its normal factory name, but cannot open a physical audio device.
bool substituteProgramSink()
{
    GstElement *fake = gst_element_factory_make("fakesink", nullptr);
    if (!fake) return false;
    const GType fakeType = G_OBJECT_TYPE(fake);
    gst_object_unref(fake);
    GstRegistry *registry = gst_registry_get();
    if (GstPluginFeature *feature = gst_registry_find_feature(
            registry, "wasapi2sink", GST_TYPE_ELEMENT_FACTORY)) {
        gst_registry_remove_feature(registry, feature);
        gst_object_unref(feature);
    }
    return gst_element_register(nullptr, "wasapi2sink", GST_RANK_NONE, fakeType);
}

bool writeWave(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    constexpr quint32 rate = 48000, frames = rate * 60, dataSize = frames * 4;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataSize);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(2) << rate << quint32(rate * 4)
           << quint16(4) << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataSize;
    for (quint32 index = 0; index < frames; ++index) {
        const qint16 sample = static_cast<qint16>(
            std::sin(6.283185307179586 * 440.0 * index / rate) * 2000.0);
        stream << sample << sample;
    }
    return stream.status() == QDataStream::Ok;
}

bool writePdf(const QString &path)
{
    QPdfWriter writer(path);
    writer.setResolution(72);
    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    painter.fillRect(QRect(0, 0, writer.width(), writer.height()), QColor("#204080"));
    if (!writer.newPage()) return false;
    painter.fillRect(QRect(0, 0, writer.width(), writer.height()), QColor("#804020"));
    return painter.end();
}

class Contract {
public:
    explicit Contract(DualDeckPlayer &player) : player_(player) { budget_.start(); }

    template<typename Predicate>
    bool wait(Predicate predicate, const char *message, int timeout = 8000)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        do {
            QApplication::processEvents();
            player_.processBusMessages();
            if (predicate()) return require(true, message);
            QThread::msleep(5);
        } while (elapsed.elapsed() < timeout && budget_.elapsed() < 45000);
        std::cerr << "CUE=" << player_.cueErrorDetail().toStdString()
                  << " BGM=" << player_.bgmErrorDetail().toStdString()
                  << " DSK=" << player_.dskErrorDetail().toStdString() << '\n';
        return require(false, message);
    }

    bool cueReady()
    {
        return wait([&] { return player_.cueStatus() == DualDeckPlayer::CueStatus::Ready; },
                    "public CUE reaches Ready");
    }

    bool liveAudio(const QString &endpoint = {})
    {
        using Source = DualDeckPlayer::DeckSource;
        if (endpoint.isEmpty())
            CHECK(substituteLiveAudioSource(), "live audio capture is replaced with a test tone");
        const Source source{Source::Type::Uvc, vtrpon::liveAudioSourcePath(
            endpoint.isEmpty() ? QStringLiteral("usb-input") : endpoint)};
        CHECK(player_.cue(source) && cueReady() && player_.takeCue() && player_.play(),
              "audio-only live input can be taken ON AIR without a video frame");
        CHECK(player_.isPlaying() && player_.currentSourceHasAudio() &&
              player_.positionMs() == -1 && player_.durationMs() == -1,
              "ON AIR live audio reports an active audio source without a file clock");
        const auto verifyNativeSignal = [&] {
            if (endpoint.isEmpty()) return true;
            return wait([&] {
                const auto levels = player_.audioLevels();
                const auto valid = [](const auto &level) {
                    return level.leftPeakDb > -24 && level.leftPeakDb < -17
                        && level.rightPeakDb > -24 && level.rightPeakDb < -17 && !level.clipped;
                };
                return valid(levels.input) && valid(levels.masterOutput);
            }, "native RAW stereo tone reaches input and master meters at the reference level");
        };
        CHECK(verifyNativeSignal(), "first deck delivers native RAW audio through the existing mixer");
        CHECK(player_.pause() && !player_.isPlaying() && player_.play() && player_.isPlaying(),
              "ON AIR live audio pause and resume use the live transport");
        CHECK(player_.cue(source) && cueReady() && player_.takeCue() && player_.play(),
              "same-endpoint live audio retake can use the other deck");
        CHECK(verifyNativeSignal(), "the other deck delivers native RAW audio after retake");
        CHECK(player_.clearMedia() == DualDeckPlayer::ClearMediaResult::Cleared, "OFF AIR clears live audio from both decks");
        CHECK(player_.cue(source) && cueReady() && player_.takeCue(),
              "live audio can be reacquired after OFF AIR");
        CHECK(player_.clearMedia() == DualDeckPlayer::ClearMediaResult::Cleared, "the reacquired live endpoint is released");
        return true;
    }

    bool nativeCue(const QString &pptx, const QString &video)
    {
        using Source = DualDeckPlayer::DeckSource;
        CHECK(player_.isAvailable(), "native CUE test player initializes");
        CHECK(player_.cue({Source::Type::File, video}) && cueReady() && player_.takeCue() && player_.play(),
              "moving file is ON AIR before native CUE");
        CHECK(wait([&] { return player_.positionMs() > 250; }, "PROGRAM starts advancing"),
              "PROGRAM plays before Office starts");
        for (int cycle = 0; cycle < 2; ++cycle) {
            QElapsedTimer elapsed;
            elapsed.start();
            CHECK(player_.cue({Source::Type::PowerPoint, pptx}), "native CUE request accepted");
            const qint64 bindMs = elapsed.elapsed();
            CHECK(bindMs < 500, "native CUE returns without waiting for Office");
            qint64 maximumPollMs = 0;
            qint64 maximumLoopGapMs = 0;
            QElapsedTimer loopGap;
            loopGap.start();
            const qint64 positionBefore = player_.positionMs();
            CHECK(wait([&] {
                maximumLoopGapMs = (std::max)(maximumLoopGapMs, loopGap.restart());
                QElapsedTimer poll;
                poll.start();
                const auto status = player_.cueStatus();
                (void)player_.powerPointNavigationState();
                maximumPollMs = (std::max)(maximumPollMs, poll.elapsed());
                return status == DualDeckPlayer::CueStatus::Ready;
            }, "native CUE becomes ready alongside PROGRAM", 30000), "Office preparation finishes");
            std::cout << "native cycle=" << cycle << " bindMs=" << bindMs
                      << " readyMs=" << elapsed.elapsed() << " maxPollMs=" << maximumPollMs
                      << " maxLoopGapMs=" << maximumLoopGapMs << '\n';
            CHECK(maximumPollMs < 100, "CUE polling never waits for COM");
            CHECK(maximumLoopGapMs < 500, "PROGRAM event processing continues throughout Office preparation");
            CHECK(player_.isPlaying() && player_.positionMs() > positionBefore,
                  "outgoing PROGRAM advances while native CUE loads");
            if (cycle == 0) {
                elapsed.restart();
                CHECK(player_.clearCue(), "native CUE can be detached");
                CHECK(elapsed.elapsed() < 500, "native CUE detachment does not wait for COM teardown");
            }
        }
        CHECK(player_.takeCue(true) && player_.crossfadeActive(), "file to native PPTX crossfade begins");
        CHECK(player_.setCrossfadeProgress(0.5F) && player_.setCrossfadeProgress(1.0F),
              "file to native PPTX crossfade completes");
        CHECK(player_.powerPointSourceIsOnAir(), "native PPTX owns PROGRAM after crossfade");
        CHECK(player_.cue({Source::Type::PowerPoint, pptx}), "another native CUE can start");
        QElapsedTimer cancel;
        cancel.start();
        CHECK(player_.clearCue(), "native CUE can be canceled during preparation");
        CHECK(cancel.elapsed() < 500 && player_.powerPointSourceIsOnAir(),
              "canceling native preparation preserves PROGRAM without waiting");
        return true;
    }

    bool run(const QString &wave, const QString &pdf, const QString &imageA, const QString &imageB)
    {
        using Source = DualDeckPlayer::DeckSource;
        using BgmStatus = DualDeckPlayer::BgmStatus;
        using DskStatus = DualDeckPlayer::DskStatus;
        CHECK(player_.isAvailable(), "normal native DualDeckPlayer initializes");
        CHECK(wait([&] {
            return player_.programOutputDeviceStatusKey() == QStringLiteral("settings.output.applied");
        }, "asynchronous PROGRAM output initialization completes"), "PROGRAM output is usable");

        Source audio{Source::Type::File, wave};
        audio.inPointMs = 100;
        audio.outPointMs = 5000;
        CHECK(player_.cue(audio) && cueReady(), "WAV material prepares with In/Out");
        CHECK(player_.takeCue() && player_.currentSourceHasAudio(), "TAKE selects the prepared audio source");
        CHECK(player_.play(), "PROGRAM play starts");
        CHECK(wait([&] { return player_.isPlaying(); }, "PROGRAM reports playing"), "play state settles");
        CHECK(player_.pause(), "PROGRAM pause succeeds");
        CHECK(player_.seek(1000), "PROGRAM seek succeeds");
        CHECK(wait([&] { return std::abs(player_.positionMs() - 1000) <= 250; },
                   "PROGRAM seek reaches requested position"), "seek state settles");
        std::cout << "Rate diagnostic: position=" << player_.positionMs()
                  << " playing=" << player_.isPlaying() << '\n';
        CHECK(player_.setRate(1.25), "paused PROGRAM accepts rate 1.25");
        QElapsedTimer rateSettling;
        rateSettling.start();
        CHECK(wait([&] {
            return rateSettling.elapsed() >= 150 && !player_.isPlaying()
                && std::abs(player_.positionMs() - 1000) <= 250;
        }, "paused rate change settles before the next transport operation"), "first rate change settles");
        CHECK(player_.setRate(1.0), "paused PROGRAM restores rate 1.0");
        rateSettling.restart();
        CHECK(wait([&] {
            return rateSettling.elapsed() >= 150 && !player_.isPlaying()
                && std::abs(player_.positionMs() - 1000) <= 250;
        }, "restored rate settles while PROGRAM remains paused"), "second rate change settles");
        CHECK(player_.play(), "PROGRAM resumes after seek and rate changes");
        CHECK(wait([&] { return player_.isPlaying(); }, "resumed PROGRAM reports playing"),
              "resume state settles before CUE isolation checks");

        Source invalid = audio;
        invalid.inPointMs = -1;
        CHECK(!player_.cue(invalid), "invalid CUE is rejected");
        CHECK(player_.currentSourceHasAudio() && player_.isPlaying(),
              "invalid CUE preserves the current PROGRAM source");
        CHECK(player_.cue(audio) && cueReady() && player_.clearCue()
                  && player_.cue(audio) && cueReady(),
              "serial CUE, clear and re-CUE cycles prepare the next source");
        CHECK(player_.clearCue() && player_.currentSourceHasAudio(),
              "clearing pending CUE preserves PROGRAM");

        CHECK(player_.loadBgm(wave, 200, 55000), "BGM loads independently of PROGRAM");
        CHECK(wait([&] { return player_.bgmStatus() == BgmStatus::Ready; }, "BGM reaches Ready"),
              "BGM preparation completes");
        CHECK(player_.playBgm() && player_.bgmStatus() == BgmStatus::Playing,
              "BGM plays alongside PROGRAM");
        CHECK(player_.pauseBgm() && player_.bgmStatus() == BgmStatus::Paused,
              "BGM pause preserves its source");
        CHECK(player_.seekBgm(1000), "BGM seek succeeds");
        CHECK(wait([&] { return std::abs(player_.bgmPositionMs() - 1000) <= 250; },
                   "BGM seek reaches requested position"), "BGM seek state settles");
        CHECK(player_.playBgm() && player_.stopBgm(), "BGM can resume and stop at In");
        CHECK(player_.bgmStatus() == BgmStatus::Stopped && player_.bgmPositionMs() == 200,
              "BGM stop reports its configured In point");
        CHECK(player_.playBgm(), "BGM can resume from Stopped");

        CHECK(player_.cueDsk(imageA, 0, -1), "first still-image DSK starts CUE");
        CHECK(wait([&] { return player_.dskCueStatus() == DskStatus::Ready; }, "first DSK reaches Ready"),
              "first DSK preparation completes");
        CHECK(player_.takeDskCue(false, false, 0.8F), "first DSK becomes visible");
        CHECK(player_.dskStatus() == DskStatus::On && player_.setDskVideoOpacity(0.4F),
              "DSK opacity changes independently of audio");
        CHECK(player_.cueDsk(imageB, 0, -1), "second DSK prepares while the first remains on air");
        CHECK(wait([&] { return player_.dskCueStatus() == DskStatus::Ready; }, "second DSK reaches Ready"),
              "second DSK preparation completes");
        CHECK(player_.takeDskCue(false, true, 1.0F) && player_.dskCrossfadeActive(),
              "DSK crossfade uses both prepared decks");
        CHECK(player_.setDskCrossfadeProgress(0.5F) && player_.setDskCrossfadeProgress(1.0F)
                  && !player_.dskCrossfadeActive(), "DSK crossfade completes and retires its outgoing deck");

        VideoFrameCapture capturedFrame;
        CHECK(wait([&] {
            capturedFrame = player_.captureStillFrame();
            const auto image = capturedFrame ? capturedFrame() : QImage{};
            return image.size() == QSize(1920, 1080) &&
                   image.pixelColor(960, 540) == QColor("#c08040");
        }, "PROGRAM capture contains the composited DSK pixels at 1920x1080"),
        "PROGRAM captures its rendered output");
        const auto frozen = capturedFrame();
        player_.setFtbCoverage(1.0F);
        CHECK(wait([&] {
            const auto capture = player_.captureStillFrame();
            return capture && capture().pixelColor(960, 540) == QColor(Qt::black);
        }, "PROGRAM capture includes FTB"), "FTB is represented in the capture");
        CHECK(capturedFrame() == frozen, "the captured frame survives subsequent output changes");
        capturedFrame = {};
        player_.setFtbCoverage(0.0F);
        CHECK(player_.pauseDskAtEnd() && player_.restartDskFromIn(false), "still DSK end/restart controls remain valid");
        CHECK(player_.clearDsk() && player_.dskStatus() == DskStatus::Empty, "DSK clear releases both decks");
        CHECK(player_.cueDsk(imageA) && player_.cueDsk(imageB) && player_.clearDskCue(),
              "rapid DSK replacement and cancellation are accepted during retirement");
        CHECK(player_.dskCueStatus() == DskStatus::Empty, "cancelled DSK CUE remains empty");
        CHECK(player_.cueDsk(imageA) && wait([&] { return player_.dskCueStatus() == DskStatus::Ready; },
              "DSK prepares again after retirement"), "DSK reuse waits for the old pipeline to stop");
        CHECK(player_.takeDskCue(false, false, 1.0F) && player_.clearDsk(),
              "DSK TAKE and CLEAR remain usable after rapid replacement");

        Source document{Source::Type::Pdf, pdf};
        CHECK(player_.cue(document) && cueReady(), "PDF prepares while PROGRAM and BGM exist");
        CHECK(player_.takeCue() && !player_.currentSourceHasAudio(), "File to PDF TAKE replaces the material route");
        CHECK(wait([&] {
            const auto state = player_.pdfNavigationState();
            return state.active && !state.busy && state.pageCount == 2 && state.currentPage == 1;
        }, "PDF navigation reaches page one of two"), "PDF source is active");
        CHECK(wait([&] {
            return player_.nextPdfPage().result == PdfCommandResult::Completed;
        }, "PDF next-page command completes asynchronously"), "PDF next-page command succeeds");
        CHECK(wait([&] {
            const auto state = player_.pdfNavigationState();
            return !state.busy && state.currentPage == 2;
        }, "PDF advances to page two"), "PDF navigation completes");
        CHECK(player_.bgmStatus() == BgmStatus::Playing, "PROGRAM material changes preserve BGM transport");
        CHECK(player_.cue(audio) && cueReady() && player_.takeCue(), "PDF to File TAKE restores the file material route");
        CHECK(player_.currentSourceHasAudio() && !player_.pdfNavigationState().active,
              "replaced PDF session no longer owns PROGRAM navigation");
        CHECK(player_.clearMedia() == DualDeckPlayer::ClearMediaResult::Cleared,
              "PROGRAM clear completes after mixed source transitions");
        CHECK(!player_.currentSourceHasAudio() && player_.bgmStatus() == BgmStatus::Playing,
              "PROGRAM clear leaves independently playing BGM intact");
        CHECK(player_.stopBgm() && player_.clearBgm() && player_.bgmStatus() == BgmStatus::Empty,
              "BGM final stop and clear succeed");
        return true;
    }

private:
    DualDeckPlayer &player_;
    QElapsedTimer budget_;
};
} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary fixture/settings directory exists")) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       directory.filePath(QStringLiteral("system-settings")));
    QApplication application(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false"));
    gst_init(nullptr, nullptr);
    if (!require(substituteProgramSink(), "test process routes wasapi2sink factory to fakesink")) return 1;

    const QString wave = directory.filePath(QStringLiteral("tone.wav"));
    const QString pdf = directory.filePath(QStringLiteral("two-pages.pdf"));
    const QString imageA = directory.filePath(QStringLiteral("dsk-a.png"));
    const QString imageB = directory.filePath(QStringLiteral("dsk-b.png"));
    QImage still(320, 180, QImage::Format_ARGB32);
    still.fill(QColor("#4080c0"));
    bool fixtures = still.save(imageA);
    still.fill(QColor("#c08040"));
    fixtures = still.save(imageB) && fixtures && writeWave(wave) && writePdf(pdf);
    if (!require(fixtures, "WAV, PNG and two-page PDF fixtures are created locally")) return 1;

    QWidget programWindow, monitorWindow;
    programWindow.resize(640, 360);
    monitorWindow.resize(320, 180);
    UvcSourceSessionManager uvcSources(false);
    bool ok = false;
    {
        // Native surfaces are created by the normal constructor. They remain
        // hidden; this test checks backend contracts, not visible acceptance.
        DualDeckPlayer player(&programWindow, &monitorWindow, false, &uvcSources);
        Contract contract(player);
        const auto arguments = application.arguments();
        ok = arguments.size() == 3 && arguments.at(1) == QStringLiteral("--live-audio-endpoint")
            ? contract.liveAudio(arguments.at(2))
            : arguments.size() == 4 && arguments.at(1) == QStringLiteral("--native-cue")
            ? contract.nativeCue(arguments.at(2), arguments.at(3))
            : contract.run(wave, pdf, imageA, imageB);
        if (ok && arguments.size() == 1) ok = contract.liveAudio();
        player.requestShutdown();
        ok = require(awaitShutdown([&] { return player.shutdownComplete(); }),
                     "PROGRAM terminal worker finishes before surfaces are destroyed") && ok;
        player.shutdown();
        ok = require(!player.isAvailable(), "public shutdown disables further playback") && ok;
        // The normal facade destructor retains an unconfirmed backend. Surface,
        // manager and QApplication lifetimes cover all confirmed destruction.
    }
    uvcSources.requestShutdown();
    ok = require(awaitShutdown([&] { return uvcSources.shutdownComplete(); }),
                 "shared UVC workers finish after PROGRAM detaches") && ok;
    return ok ? 0 : 1;
}
