#include "dual_deck_player.h"
#include "audio_output_settings.h"
#include "program_audio_output.h"
#include "program_audio_meter.h"
#include "deck_source_session.h"
#include "bgm_playback.h"

#if defined(_WIN32) || defined(__APPLE__)
#include "dsk_playback.h"
#endif
#ifdef _WIN32
#include "pdf_source_session_manager.h"
#include "powerpoint_source_session_manager.h"
#include "uvc_source_session_manager.h"
#endif

#if defined(_WIN32) || defined(__APPLE__)
#include "program_gpu_compositor.h"
#endif

#include <QDebug>
#include <QPalette>
#include <QStackedLayout>
#include <QWidget>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <QWindow>
#endif

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

namespace {

constexpr int kNoPendingEventDeck = -2;
constexpr gint64 kProgramFrameRestoreTimeoutUs = 2 * G_TIME_SPAN_SECOND;
#if !defined(_WIN32) && !defined(__APPLE__)

void setSurfaceOpacity(QWidget *surface, qreal opacity)
{
    if (QWindow *window = surface->windowHandle()) {
        window->setOpacity(opacity);
    }
}
#endif

} // namespace

// Coordinates PROGRAM/CUE selection, compositor layers and aggregate shutdown.
// Each playback component owns its pipelines and material-specific state.
class DualDeckPlayer::Impl final
{
public:
    Impl(QWidget *videoWindow, QWidget *monitorWindow,
         bool use4kProgramOutput,
         UvcSourceSessionManager *uvcSourceSessions)
        : videoWindow_(videoWindow)
        , monitorWindow_(monitorWindow)
#if defined(_WIN32) || defined(__APPLE__)
        , uvcSourceSessions_(uvcSourceSessions)
#endif
#if defined(_WIN32) || defined(__APPLE__)
        , outputGeometry_(use4kProgramOutput
              ? ProgramGpuCompositor::OutputGeometry{3840, 2160}
              : ProgramGpuCompositor::OutputGeometry{})
#endif
    {
        if (!initializeVideoSurfaces()
            || !initializeDeck(0) || !initializeDeck(1)
            || !bgm_.initialize(&Impl::handleBusSync, this)
            || !programAudioOutput_.initialize(
                &Impl::handleBusSync, this,
                vtrpon::savedProgramOutputDeviceId())) {
            return;
        }
#if defined(_WIN32) || defined(__APPLE__)
        if (!dsk_.initialize()) {
            return;
        }
#endif

        audioMeter_.setOutputSources(programAudioOutput_.masterLevel(),
                                     bgm_.inputLevel(),
                                     programAudioOutput_.bgmPostFaderLevel());
        GstClock *clock = gst_system_clock_obtain();
        if (clock) {
            gst_pipeline_use_clock(GST_PIPELINE(programAudioOutput_.pipeline()), clock);
            for (Deck &deck : decks_) {
                gst_pipeline_use_clock(GST_PIPELINE(deck.pipeline()), clock);
            }
            gst_pipeline_use_clock(GST_PIPELINE(bgm_.pipeline()), clock);
#if defined(_WIN32) || defined(__APPLE__)
            for (int index = 0; index < 2; ++index) {
                gst_pipeline_use_clock(GST_PIPELINE(dsk_.pipeline(index)), clock);
            }
#endif
            gst_object_unref(clock);
        }

#ifdef _WIN32
        videoLayout_->setCurrentWidget(gpuVideoSurface_);
        monitorLayout_->setCurrentWidget(gpuMonitorSurface_);
        gpuVideoSurface_->raise();
        gpuMonitorSurface_->raise();
        programGpuCompositor_ = std::make_unique<ProgramGpuCompositor>(
            static_cast<std::uintptr_t>(gpuVideoSurface_->winId()),
            static_cast<std::uintptr_t>(gpuMonitorSurface_->winId()),
            outputGeometry_);
        if (!programGpuCompositor_->start()) {
            qWarning() << "Failed to start the ON AIR GPU compositor";
            return;
        }
        powerPointSources_ =
            std::make_unique<PowerPointSourceSessionManager>(
                static_cast<int>(outputGeometry_.width),
                static_cast<int>(outputGeometry_.height));
        pdfSources_ = std::make_unique<PdfSourceSessionManager>(
            static_cast<int>(outputGeometry_.width),
            static_cast<int>(outputGeometry_.height));
        if (!uvcSourceSessions_) {
            qWarning() << "UVC source session manager is unavailable";
            return;
        }
        GstContext *inputContext = programGpuCompositor_->createInputContext();
        if (!inputContext) {
            qWarning() << "Failed to create the shared UVC D3D11 context";
            return;
        }
        uvcSourceSessions_->setProgramContext(inputContext);
        powerPointSources_->setProgramContext(inputContext);
        pdfSources_->setProgramContext(inputContext);
        gst_context_unref(inputContext);
        for (int index = 0; index < static_cast<int>(decks_.size()); ++index) {
            decks_[index].attachSourceManagers(uvcSourceSessions_, powerPointSources_.get(),
                pdfSources_.get(), programGpuCompositor_.get());
            if (!programGpuCompositor_->configureDeckInput(
                    index, decks_[index].pipeline(), decks_[index].appSink())) {
                qWarning() << "Failed to connect Deck" << index
                           << "to the ON AIR GPU compositor";
                return;
            }
        }
        for (int index = 0; index < 2; ++index) {
            if (!programGpuCompositor_->configureDskInput(
                    index, dsk_.pipeline(index), dsk_.appSink(index))) {
                qWarning() << "Failed to connect DSK" << index
                           << "to the ON AIR GPU compositor";
                return;
            }
        }
        dsk_.attachCompositor(programGpuCompositor_.get());
#endif
#ifdef __APPLE__
        videoLayout_->setCurrentWidget(gpuVideoSurface_);
        monitorLayout_->setCurrentWidget(gpuMonitorSurface_);
        programGpuCompositor_ = std::make_unique<ProgramGpuCompositor>(
            static_cast<std::uintptr_t>(gpuVideoSurface_->winId()),
            static_cast<std::uintptr_t>(gpuMonitorSurface_->winId()), outputGeometry_);
        if (!programGpuCompositor_->start()) {
            qWarning() << "Failed to start Mac ON AIR Metal compositor";
            return;
        }
        PdfSourceSessionManager *pdfManager = nullptr;
#ifdef VTRPON_PDF_SUPPORT
        pdfSources_ = std::make_unique<PdfSourceSessionManager>(
            static_cast<int>(outputGeometry_.width), static_cast<int>(outputGeometry_.height));
        pdfManager = pdfSources_.get();
#endif
        for (int i = 0; i < 2; ++i) {
            decks_[i].attachSourceManagers(uvcSourceSessions_, nullptr, pdfManager, programGpuCompositor_.get());
            if (!programGpuCompositor_->configureDeckInput(i, decks_[i].pipeline(), decks_[i].appSink())) return;
            if (!programGpuCompositor_->configureDskInput(i, dsk_.pipeline(i), dsk_.appSink(i))) return;
        }
        dsk_.attachCompositor(programGpuCompositor_.get());
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
        showProgramSurface(-1);
#endif
        available_ = true;
        // Device activation can block inside the driver, including on the
        // first launch. Use the same PROGRAM-only worker as an idle change.
        updateProgramOutputDevice();
    }

    ~Impl()
    {
        shutdown();
        detachCallbacks();
        if (!shutdownConfirmed_) {
            // A live GStreamer streaming thread can still own references into
            // this object. The process is already exiting, so leaking the
            // backend is safer than freeing callback storage that may still be
            // in use.
            qWarning() << "Skipping unsafe dual-deck backend release after"
                          " shutdown could not be confirmed";
            return;
        }

        for (Deck &deck : decks_) {
            deck.releaseResourcesAfterConfirmedShutdown();
        }
    }

    bool isAvailable() const { return available_ && !shutDown_; }

    VideoFrameCapture captureStillFrame() const {
#if defined(_WIN32) || defined(__APPLE__)
        if (isAvailable() && programGpuCompositor_) return programGpuCompositor_->captureStillFrame();
#endif
        return {};
    }

    FullscreenOutputStats fullscreenOutputStats() const
    {
#if defined(_WIN32) || defined(__APPLE__)
        if (isAvailable() && programGpuCompositor_
            && programGpuCompositor_->isAvailable()) {
            return {QSize(static_cast<int>(outputGeometry_.width),
                          static_cast<int>(outputGeometry_.height)),
                    programGpuCompositor_->presentedFrameCount()};
        }
#endif
        return {};
    }

    bool canReleaseAfterShutdown() const
    {
        return shutdownWorkerDone_.load(std::memory_order_acquire) && shutdownConfirmed_;
    }

    void requestProgramOutputDevice(const QString &id)
    {
        if (!shutDown_) programAudioOutput_.requestDevice(id);
    }

    QString appliedProgramOutputDevice() const
    {
        return programAudioOutput_.appliedDevice();
    }

    QString programOutputDeviceStatusKey() const
    {
        return programAudioOutput_.deviceStatusKey();
    }

    bool programAudioOutputReady() const { return programAudioOutput_.ready(); }

    void updateProgramOutputDevice()
    {
        const bool mayChange = !shutDown_ && (!programAudioOutput_.ready() || (stopped_ && !crossfadeActive_
            && (bgm_.cachedStatus() == BgmStatus::Empty
                || bgm_.cachedStatus() == BgmStatus::Ready
                || bgm_.cachedStatus() == BgmStatus::Stopped)));
        programAudioOutput_.updateDevice(mayChange);
    }

    void refreshWindowHandles()
    {
        if (!isAvailable()) return;
#if !defined(_WIN32) && !defined(__APPLE__)
        for (int index = 0; index < static_cast<int>(decks_.size()); ++index) {
            const auto &surfaces = deckSurfaces_[index];
            if (surfaces.video && surfaces.monitor)
                decks_[index].setNativeWindowHandles(
                    static_cast<guintptr>(surfaces.video->winId()),
                    static_cast<guintptr>(surfaces.monitor->winId()));
        }
#endif
    }

    bool prepareNativeDocumentShutdown()
    {
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
        available_ = false;
        if (!nativeDocumentShutdownRequested_) {
            // Detach consumers before producer workers are allowed to release
            // their input elements. The event loop remains alive until done.
            if (programGpuCompositor_ && !programGpuCompositor_->prepareShutdown()) return false;
            nativeDocumentShutdownRequested_ = true;
#ifdef _WIN32
            if (powerPointSources_) powerPointSources_->requestShutdown();
#endif
            if (pdfSources_) pdfSources_->requestShutdown();
        }
        return
#ifdef _WIN32
               (!powerPointSources_ || powerPointSources_->shutdownComplete()) &&
#endif
               (!pdfSources_ || pdfSources_->shutdownComplete());
#elif defined(__APPLE__)
        return !programGpuCompositor_ || programGpuCompositor_->prepareShutdown();
#else
        return true;
#endif
    }

    void requestShutdown()
    {
        if (!shutDown_) {
            shutDown_ = true;
            available_ = false;
            resetAudioLevelForDeck(-1);
        }
        if (shutdownWorkerStarted_) return;
#if defined(_WIN32) || defined(__APPLE__)
        if (!prepareNativeDocumentShutdown()) return;
#endif
        detachCallbacks();
        if (programAudioOutput_.deviceOperationPending()) return;
        for (const Deck &deck : decks_) if (deck.retirementPending()) return;
#if defined(_WIN32) || defined(__APPLE__)
        if (dsk_.retirementPending()) return;
#endif
        shutdownWorkerStarted_ = true;
        // UI producers and callbacks are quiescent. Keep Impl alive until this
        // terminal worker publishes completion; no QObject/widget work here.
        try {
            std::thread([this] {
                finishShutdown();
                shutdownWorkerDone_.store(true, std::memory_order_release);
            }).detach();
        } catch (const std::system_error &error) {
            shutdownWorkerStarted_ = false;
            qCritical() << "Could not start player shutdown worker:" << error.what();
        }
    }

    bool shutdownComplete()
    {
        if (shutDown_ && !shutdownWorkerStarted_) requestShutdown();
        return shutdownWorkerDone_.load(std::memory_order_acquire);
    }

    void shutdown() { requestShutdown(); }

    void finishShutdown()
    {
        bool decksConfirmed = true;
        for (Deck &deck : decks_) {
            decksConfirmed = deck.stopPipelinesForShutdown() && decksConfirmed;
        }
        const bool bgmConfirmed = bgm_.shutdown();
#if defined(_WIN32) || defined(__APPLE__)
        // Native managers have already completed on their own workers. Shared
        // UVC ownership remains with Main until it requests manager shutdown.
        const bool dskConfirmed = dsk_.shutdown();
#endif
        const bool programOutputConfirmed = programAudioOutput_.stop();
#ifdef _WIN32
        bool gpuConfirmed = true;
        if (programGpuCompositor_) {
            gpuConfirmed = decksConfirmed && dskConfirmed
                && programGpuCompositor_->stop();
        }
        shutdownConfirmed_ = decksConfirmed && bgmConfirmed && programOutputConfirmed
            && dskConfirmed && gpuConfirmed;
#elif defined(__APPLE__)
        const bool gpuConfirmed = !programGpuCompositor_ || programGpuCompositor_->stop();
        shutdownConfirmed_ = decksConfirmed && bgmConfirmed && programOutputConfirmed && dskConfirmed && gpuConfirmed;
#else
        shutdownConfirmed_ = decksConfirmed && bgmConfirmed && programOutputConfirmed;
#endif
    }

    bool cue(const DeckSource &source)
    {
        switch (source.type) {
        case DeckSource::Type::File:
            return available_ && !source.locator.isEmpty() && source.inPointMs >= 0
                && (source.outPointMs < 0 || source.outPointMs > source.inPointMs)
                && std::isfinite(source.playbackRate) && source.playbackRate > 0.0
                && prepareCueSource(source);
        case DeckSource::Type::Uvc:
            return cueUvc(source.locator, source.audioDeviceId);
        case DeckSource::Type::PowerPoint:
            return cuePowerPoint(source.locator);
        case DeckSource::Type::Pdf:
            return cuePdf(source.locator);
        }
        return false;
    }

    bool prepareCueSource(const DeckSource &source)
    {
        const int currentDeck = currentDeck_.load(std::memory_order_relaxed);
        const int targetDeck = currentDeck == 0 ? 1 : 0;
        cueDeck_ = targetDeck;
        programAudioOutput_.setDeckAudible(targetDeck, false);
        if (source.type == DeckSource::Type::File) {
#if !defined(_WIN32) && !defined(__APPLE__)
            showProgramSurface(currentDeck);
#endif
        }
        GstClock *clock = gst_system_clock_obtain();
        const auto result = decks_[targetDeck].prepare(source, itemVolume_, clock);
        if (clock) gst_object_unref(clock);
        cueStatus_ = result.state;
        cueErrorDetail_ = result.errorDetail;
        if (cueStatus_ == CueStatus::Error) {
            if (result.fileStateChangeFailed) {
                pollBus(decks_[targetDeck].bus(), targetDeck);
                qWarning() << "Failed to set CUE deck state to"
                           << gst_element_state_get_name(GST_STATE_PAUSED) << ':'
                           << cueErrorDetail_;
            }
            return false;
        }
        return true;
    }

    bool cueUvc(const QString &devicePath, const QString &audioDeviceId)
    {
#if defined(_WIN32) || defined(__APPLE__)
        if (!available_ || devicePath.isEmpty()) {
            cueErrorDetail_ = QStringLiteral("UVC device identification is missing.");
            return false;
        }
        return prepareCueSource({DeckSource::Type::Uvc, devicePath, audioDeviceId});
#else
        Q_UNUSED(devicePath); Q_UNUSED(audioDeviceId);
        cueErrorDetail_ = QStringLiteral("UVC input is available only on Windows.");
        return false;
#endif
    }

    bool cuePowerPoint(const QString &filePath)
    {
#ifdef _WIN32
        if (!available_ || filePath.isEmpty() || !powerPointSources_) {
            cueErrorDetail_ = QStringLiteral("PowerPoint playback is unavailable.");
            return false;
        }
        return prepareCueSource({DeckSource::Type::PowerPoint, filePath});
#else
        Q_UNUSED(filePath);
        cueErrorDetail_ = QStringLiteral("PowerPoint playback is available only on Windows.");
        return false;
#endif
    }

    bool cuePdf(const QString &filePath)
    {
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
        if (!available_ || filePath.isEmpty() || !pdfSources_) {
            cueErrorDetail_ = QStringLiteral("PDF playback is unavailable.");
            return false;
        }
        return prepareCueSource({DeckSource::Type::Pdf, filePath});
#else
        Q_UNUSED(filePath);
        cueErrorDetail_ = QStringLiteral("PDF playback is available only on Windows.");
        return false;
#endif
    }

    bool clearCue()
    {
        if (!isAvailable()) {
            return false;
        }
        if (cueDeck_ >= 0
            && cueDeck_ != currentDeck_.load(std::memory_order_relaxed)) {
            const bool retireFile = decks_[cueDeck_].sourceType() == DeckSourceType::File
                && (cueStatus_ == CueStatus::Loading || cueStatus_ == CueStatus::Error);
            if (retireFile ? !decks_[cueDeck_].replaceFilePipeline() : !clearDeckSource(
                    cueDeck_,
                    QStringLiteral("Could not release the standby player."))) {
                if (retireFile) cueErrorDetail_ = QStringLiteral(
                    "Could not safely replace the standby player. Current ON AIR output continues.");
                return false;
            }
            programAudioOutput_.setDeckAudible(cueDeck_, false);
        }
        cueDeck_ = -1;
        cueStatus_ = CueStatus::Empty;
        cueErrorDetail_.clear();
        return true;
    }

    CueStatus cueStatus()
    {
        if (!isAvailable()) {
            return CueStatus::Error;
        }
        return cueStatus_;
    }

    QString cueErrorDetail() const { return cueErrorDetail_; }
    bool cueMediaOffline() const { return cueDeck_ >= 0 && decks_[cueDeck_].mediaOffline(); }
    bool onAirMediaOffline() const {
        const int deck = currentDeck_.load(std::memory_order_relaxed);
        return deck >= 0 && decks_[deck].mediaOffline();
    }
    bool dskMediaOffline() const {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.mediaOffline();
#else
        return false;
#endif
    }
    bool bgmMediaOffline() const { return bgm_.mediaOffline(); }
    bool dskCueMediaOffline() const {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.cueMediaOffline();
#else
        return false;
#endif
    }

    bool takeCue(bool crossfade, float itemVideoFadeProgress, TakeCueFailure *failure)
    {
        if (failure) *failure = TakeCueFailure::Other;
        if (!isAvailable()) {
            return false;
        }
#if defined(_WIN32) || defined(__APPLE__)
        if (!programGpuCompositor_->isAvailable()) {
            return false;
        }
#endif
        pollBuses();
        refreshCueStatus();
        bgm_.refreshStatus();
        if (cueDeck_ < 0 || cueStatus_ != CueStatus::Ready) {
            return false;
        }

        const int incomingDeck = cueDeck_;
        const int outgoingDeck = currentDeck_.load(std::memory_order_relaxed);
        Deck &incoming = decks_[incomingDeck];
        itemVideoFadeProgress_ =
            std::clamp(itemVideoFadeProgress, 0.0F, 1.0F);
#if defined(_WIN32) || defined(__APPLE__)
        programGpuCompositor_->setItemFadeProgress(itemVideoFadeProgress_);
        const bool useCrossfade = crossfade
            && outgoingDeck >= 0 && outgoingDeck != incomingDeck
            && incoming.hasVideo() && decks_[outgoingDeck].hasVideo();
#else
        const bool useCrossfade = false;
#endif

#if defined(_WIN32) || defined(__APPLE__)
        if (!programGpuCompositor_->setDeckLayers(
                incoming.hasVideo() ? incomingDeck : -1,
                useCrossfade ? 0.0F : 1.0F,
                outgoingDeck >= 0 && decks_[outgoingDeck].hasVideo()
                    ? outgoingDeck : -1,
                useCrossfade ? 1.0F : 0.0F)) {
            return false;
        }
#endif
        if (!useCrossfade && outgoingDeck >= 0 && outgoingDeck != incomingDeck
            && !setDeckStandby(
                outgoingDeck,
                QStringLiteral("Could not stop the previous ON AIR deck."))) {
#if defined(_WIN32) || defined(__APPLE__)
            (void)programGpuCompositor_->setDeckLayers(
                decks_[outgoingDeck].hasVideo() ? outgoingDeck : -1, 1.0F,
                -1, 0.0F);
#endif
            return false;
        }
        if (outgoingDeck >= 0 && outgoingDeck != incomingDeck) {
            programAudioOutput_.setDeckAudible(outgoingDeck, false);
        }
#if !defined(_WIN32) && !defined(__APPLE__)
        showProgramSurface(incomingDeck);
#endif
        currentDeck_.store(incomingDeck, std::memory_order_relaxed);
        programFrameClearedForStop_ = false;
        programFrameRestorePending_ = false;
        programFrameRestoreStartedUs_ = 0;
        programAudioOutput_.setDeckAudible(incomingDeck, incoming.hasAudio());
        programAudioOutput_.setFtbCoverage(ftbCoverage_);
        crossfadeActive_ = useCrossfade;
        crossfadeIncomingDeck_ = useCrossfade ? incomingDeck : -1;
        crossfadeOutgoingDeck_ = useCrossfade ? outgoingDeck : -1;
        if (outgoingDeck >= 0 && pendingEventDeck_ == outgoingDeck) {
            pendingEvent_ = Event::None;
            pendingEventDetail_.clear();
            pendingEventDeck_ = kNoPendingEventDeck;
        }
#ifdef _WIN32
        if (pendingEventDeck_ == kNoPendingEventDeck) {
            pendingEvent_ = Event::None;
            pendingEventDetail_.clear();
        }
#endif

        cueDeck_ = -1;
        cueStatus_ = CueStatus::Empty;
        cueErrorDetail_.clear();
        stopped_ = false;
        resetAudioLevelForDeck(incomingDeck, incoming.hasAudio());
        if (failure) *failure = TakeCueFailure::None;
        return true;
    }

    bool crossfadeActive() const { return crossfadeActive_; }
    int currentDeckIndex() const { return currentDeck_.load(std::memory_order_relaxed); }

    bool setCrossfadeProgress(float progress)
    {
        if (!crossfadeActive_) {
            return false;
        }
        const float clamped = std::clamp(progress, 0.0F, 1.0F);
#if defined(_WIN32) || defined(__APPLE__)
        if (!programGpuCompositor_->setDeckLayers(
                crossfadeIncomingDeck_, clamped,
                crossfadeOutgoingDeck_, 1.0F - clamped)) {
            return false;
        }
#endif
        if (clamped >= 1.0F) {
            if (crossfadeOutgoingDeck_ >= 0
                && !setDeckStandby(
                    crossfadeOutgoingDeck_,
                    QStringLiteral(
                        "Could not stop the previous ON AIR deck after the crossfade."))) {
                return false;
            }
            crossfadeActive_ = false;
            crossfadeIncomingDeck_ = -1;
            crossfadeOutgoingDeck_ = -1;
        }
        return true;
    }

    bool setItemVideoFadeProgress(float progress)
    {
        const bool hasCurrentInput =
            currentDeck_.load(std::memory_order_relaxed) >= 0;
        if (!isAvailable() || !hasCurrentInput) {
            return false;
        }
        itemVideoFadeProgress_ = std::clamp(progress, 0.0F, 1.0F);
#if defined(_WIN32) || defined(__APPLE__)
        programGpuCompositor_->setItemFadeProgress(itemVideoFadeProgress_);
#else
        updateFtbOverlay(currentDeck_.load(std::memory_order_relaxed));
#endif
        return true;
    }

    ClearMediaResult clearMedia()
    {
        if (!available_) {
            return ClearMediaResult::Unavailable;
        }

#if defined(_WIN32) || defined(__APPLE__)
        if (!programGpuCompositor_->setDeckLayers(-1, 0.0F, -1, 0.0F)) {
            return ClearMediaResult::Failed;
        }
#else
        showProgramSurface(-1);
#endif

        // Program is visibly blank from this point and OFF AIR is therefore
        // committed. Remaining source detach/teardown work is best-effort:
        // returning Failed after this point would leave a black Program while
        // the controller still advertised ON AIR.
        for (int index = 0; index < static_cast<int>(decks_.size()); ++index) {
            programAudioOutput_.setDeckAudible(index, false);
            (void)decks_[index].clearAfterOffAir();
        }

        currentDeck_.store(-1, std::memory_order_relaxed);
        programFrameClearedForStop_ = false;
        programFrameRestorePending_ = false;
        programFrameRestoreStartedUs_ = 0;
        crossfadeActive_ = false;
        crossfadeIncomingDeck_ = -1;
        crossfadeOutgoingDeck_ = -1;
        itemVideoFadeProgress_ = 1.0F;
        programAudioOutput_.setFtbCoverage(ftbCoverage_);
#if defined(_WIN32) || defined(__APPLE__)
        programGpuCompositor_->setItemFadeProgress(itemVideoFadeProgress_);
#endif
        cueDeck_ = -1;
        cueStatus_ = CueStatus::Empty;
        cueErrorDetail_.clear();
        stopped_ = true;
        pendingEvent_ = Event::None;
        pendingEventDetail_.clear();
        pendingEventDeck_ = kNoPendingEventDeck;
        resetAudioLevelForDeck(-1);
        return ClearMediaResult::Cleared;
    }

    bool cueDsk(const QString &filePath, qint64 inPointMs,
                qint64 outPointMs)
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.cue(filePath, inPointMs, outPointMs);
#else
        Q_UNUSED(filePath);
        Q_UNUSED(inPointMs);
        Q_UNUSED(outPointMs);
        return false;
#endif
    }

    bool takeDskCue(bool playOnShow, bool crossfade,
                    float videoOpacity)
    {
#if defined(_WIN32) || defined(__APPLE__)
        dsk_.pollBus();
        return isAvailable() && dsk_.takeCue(playOnShow, crossfade, videoOpacity);
#else
        Q_UNUSED(playOnShow);
        Q_UNUSED(crossfade);
        Q_UNUSED(videoOpacity);
        return false;
#endif
    }

    bool dskCrossfadeActive() const
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.crossfadeActive();
#else
        return false;
#endif
    }

    bool setDskCrossfadeProgress(float progress)
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.setCrossfadeProgress(progress);
#else
        Q_UNUSED(progress);
        return false;
#endif
    }

    bool setDskVideoOpacity(float opacity)
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.setVideoOpacity(opacity);
#else
        Q_UNUSED(opacity);
        return false;
#endif
    }

    bool clearDskCue()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.clearCue();
#else
        return false;
#endif
    }

    bool clearDsk()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.clear();
#else
        return false;
#endif
    }

    bool playDsk()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.play();
#else
        return false;
#endif
    }

    bool pauseDsk()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.pause();
#else
        return false;
#endif
    }

    bool dskIsPlaying() const
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.isPlaying();
#else
        return false;
#endif
    }

    bool pauseDskAtEnd()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.pauseAtEnd();
#else
        return false;
#endif
    }

    bool restartDskFromIn(bool play)
    {
#if defined(_WIN32) || defined(__APPLE__)
        return isAvailable() && dsk_.restartFromIn(play);
#else
        Q_UNUSED(play);
        return false;
#endif
    }

    DskStatus dskStatus()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.status();
#else
        return DskStatus::Error;
#endif
    }

    DskStatus dskCueStatus()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.cueStatus();
#else
        return DskStatus::Error;
#endif
    }

    qint64 dskPositionMs() const
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.positionMs();
#else
        return -1;
#endif
    }

    qint64 dskDurationMs() const
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.durationMs();
#else
        return -1;
#endif
    }

    QString dskErrorDetail() const
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.errorDetail();
#else
        return QStringLiteral("DSK GPU layer is unavailable on this platform.");
#endif
    }

    DskEvent takeDskEvent(QString &detail)
    {
#if defined(_WIN32) || defined(__APPLE__)
        return dsk_.takeEvent(detail);
#else
        detail.clear();
        return DskEvent::None;
#endif
    }

    bool play()
    {
        if (!isAvailable()) {
            return false;
        }
        Deck *deck = currentDeck();
        if (!deck) {
            return false;
        }
#ifdef _WIN32
        if (deck->sourceType() == DeckSourceType::PowerPoint) {
            const int deckIndex =
                currentDeck_.load(std::memory_order_relaxed);
            if (!deck->sourceIsPlaying()) {
                pollSourceError(deckIndex);
                return false;
            }
            deck->setLiveFrameUpdates(true);
            stopped_ = false;
            return true;
        }
#endif
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
        if (deck->sourceType() == DeckSourceType::Pdf) {
            const int deckIndex =
                currentDeck_.load(std::memory_order_relaxed);
            if (!deck->sourceIsPlaying()) {
                pollSourceError(deckIndex);
                return false;
            }
            deck->setLiveFrameUpdates(true);
            stopped_ = false;
            return true;
        }
#endif
#if defined(_WIN32) || defined(__APPLE__)
        if (deck->sourceType() == DeckSourceType::Uvc) {
            if (!deck->sourceIsPlaying()) {
                pollSourceError(currentDeck_.load(std::memory_order_relaxed));
                return false;
            }
            const int deckIndex =
                currentDeck_.load(std::memory_order_relaxed);
            programAudioOutput_.setDeckAudible(deckIndex, deck->hasAudio());
            audioMeter_.setDeckCollecting(deck->hasAudio());
            deck->setLiveFrameUpdates(true);
            stopped_ = false;
            return true;
        }
#endif
        if (deck->sourceType() != DeckSourceType::File) {
            return false;
        }
        pollBuses();
        if (pendingEvent_ == Event::EndOfStream
            && pendingEventDeck_
                == currentDeck_.load(std::memory_order_relaxed)) {
            pendingEvent_ = Event::None;
            pendingEventDetail_.clear();
            pendingEventDeck_ = kNoPendingEventDeck;
            if (!stop()) {
                return false;
            }
        }

        audioMeter_.setDeckCollecting(true);
#if defined(_WIN32) || defined(__APPLE__)
        if (programFrameClearedForStop_) {
            programGpuCompositor_->setDeckFrameUpdatesEnabled(
                currentDeck_.load(std::memory_order_relaxed), true);
        }
#endif
        if (!setCurrentState(GST_STATE_PLAYING)) {
#if defined(_WIN32) || defined(__APPLE__)
            if (programFrameClearedForStop_) {
                (void)programGpuCompositor_->suspendDeckFrameUpdatesAndClear(
                    currentDeck_.load(std::memory_order_relaxed));
            }
#endif
            audioMeter_.setActive(false);
            audioMeter_.setDeckCollecting(false);
            return false;
        }

#if !defined(_WIN32) && !defined(__APPLE__)
        if (programFrameClearedForStop_) {
            showProgramSurface(currentDeck_.load(std::memory_order_relaxed));
        }
#endif
        programFrameClearedForStop_ = false;
        programFrameRestorePending_ = false;
        programFrameRestoreStartedUs_ = 0;
        stopped_ = false;
        return true;
    }

    bool pause()
    {
        if (!isAvailable()) {
            return false;
        }
        audioMeter_.reset(true);
        Deck *deck = currentDeck();
        if (!deck) {
            return false;
        }
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
        if (deck->sourceType() == DeckSourceType::PowerPoint ||
            deck->sourceType() == DeckSourceType::Pdf) {
            return false;
        }
#endif
#if defined(_WIN32) || defined(__APPLE__)
        if (deck->sourceType() == DeckSourceType::Uvc) {
            programAudioOutput_.setDeckAudible(currentDeck_.load(std::memory_order_relaxed), false);
            deck->setLiveFrameUpdates(false);
            return true;
        }
#endif
        if (deck->sourceType() != DeckSourceType::File) {
            return false;
        }
        return setCurrentState(GST_STATE_PAUSED);
    }

    bool isPlaying() const
    {
        if (!isAvailable()) return false;
        const Deck *deck = currentDeck();
        if (!deck) return false;
        if (deck->sourceType() == DeckSourceType::File) return deck->sourceIsPlaying();
        return !stopped_ && !deck->uvcProgramPaused() && deck->sourceIsPlaying();
    }

    bool powerPointSourceIsOnAir() const
    {
#ifdef _WIN32
        const auto deckIsPowerPoint = [this](int deckIndex) {
            return deckIndex >= 0
                && deckIndex < static_cast<int>(decks_.size())
                && decks_[deckIndex].sourceType()
                    == DeckSourceType::PowerPoint;
        };
        if (deckIsPowerPoint(
                currentDeck_.load(std::memory_order_relaxed))) {
            return true;
        }
        return crossfadeActive_
            && deckIsPowerPoint(crossfadeOutgoingDeck_);
#else
        return false;
#endif
    }

    bool powerPointCueIsActive() const
    {
#ifdef _WIN32
        return cueDeck_ >= 0
            && cueDeck_ != currentDeck_.load(std::memory_order_relaxed)
            && cueDeck_ < static_cast<int>(decks_.size())
            && decks_[cueDeck_].sourceType() == DeckSourceType::PowerPoint;
#else
        return false;
#endif
    }

    void warmUpPowerPointApplication()
    {
#ifdef _WIN32
        if (powerPointSources_) powerPointSources_->requestApplicationWarmup();
#endif
    }

    bool preparePowerPointConversion(bool *powerPointCueReleased,
                                     QString *errorDetail)
    {
        if (powerPointCueReleased) {
            *powerPointCueReleased = false;
        }
        if (errorDetail) {
            errorDetail->clear();
        }
#ifdef _WIN32
        if (powerPointSourceIsOnAir()) {
            if (errorDetail) {
                *errorDetail = QStringLiteral(
                    "A native PowerPoint source is still ON AIR.");
            }
            return false;
        }

        const int currentDeck =
            currentDeck_.load(std::memory_order_relaxed);
        const int powerPointCueDeck = powerPointCueIsActive()
            ? cueDeck_ : -1;
        const auto clearOffAirPowerPointDeck =
            [this, currentDeck, powerPointCueDeck, powerPointCueReleased,
             errorDetail](int index) {
            const bool wasCueDeck = index == powerPointCueDeck;
            if (index == currentDeck
                || (crossfadeActive_
                    && index == crossfadeOutgoingDeck_)) {
                if (errorDetail) {
                    *errorDetail = QStringLiteral(
                        "A native PowerPoint source is still part of ON AIR output.");
                }
                return false;
            }
            const CueStatus previousCueStatus = cueStatus_;
            const QString previousCueErrorDetail = cueErrorDetail_;
            if (!clearDeckSource(
                    index,
                    QStringLiteral(
                        "Could not release the native PowerPoint source before conversion."))) {
                const QString cleanupError = cueErrorDetail_.isEmpty()
                    ? QStringLiteral(
                          "Could not release the native PowerPoint source before conversion.")
                    : cueErrorDetail_;
                if (!wasCueDeck) {
                    // A retained off-air PowerPoint deck is not the current
                    // CUE. Its cleanup failure must not turn an unrelated CUE
                    // into Error.
                    cueStatus_ = previousCueStatus;
                    cueErrorDetail_ = previousCueErrorDetail;
                }
                if (errorDetail) {
                    *errorDetail = cleanupError;
                }
                return false;
            }
            if (wasCueDeck) {
                cueDeck_ = -1;
                cueStatus_ = CueStatus::Empty;
                cueErrorDetail_.clear();
                if (powerPointCueReleased) {
                    *powerPointCueReleased = true;
                }
            }
            return true;
        };

        // Clear retained off-air decks first. If one of those retries fails,
        // the actual CUE remains intact and the controller state still matches
        // the player.
        for (int index = 0; index < static_cast<int>(decks_.size());
             ++index) {
            if (index == powerPointCueDeck
                || decks_[index].sourceType()
                    != DeckSourceType::PowerPoint) {
                continue;
            }
            if (!clearOffAirPowerPointDeck(index)) {
                return false;
            }
        }
        if (powerPointCueDeck >= 0
            && !clearOffAirPowerPointDeck(powerPointCueDeck)) {
            return false;
        }
        if (isPowerPointNativeSourceActive()) {
            if (errorDetail) {
                *errorDetail = QStringLiteral(
                    "A native PowerPoint source remained active after OFF AIR cleanup.");
            }
            return false;
        }
#else
        Q_UNUSED(powerPointCueReleased);
        Q_UNUSED(errorDetail);
#endif
        return true;
    }

    bool currentSourceHasAudio() const
    {
        const Deck *deck = currentDeck();
        return deck && deck->hasAudio();
    }

    bool stop()
    {
        if (!isAvailable()) {
            return false;
        }
        audioMeter_.reset();
        Deck *deck = currentDeck();
        if (!deck) {
            return false;
        }
        programFrameRestorePending_ = false;
        programFrameRestoreStartedUs_ = 0;
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
        if (deck->sourceType() == DeckSourceType::PowerPoint ||
            deck->sourceType() == DeckSourceType::Pdf) {
            deck->setLiveFrameUpdates(false);
            stopped_ = true;
            return true;
        }
#endif
#if defined(_WIN32) || defined(__APPLE__)
        if (deck->sourceType() == DeckSourceType::Uvc) {
            programAudioOutput_.setDeckAudible(currentDeck_.load(std::memory_order_relaxed), false);
            deck->setLiveFrameUpdates(false);
            stopped_ = true;
            return true;
        }
#endif
        if (deck->sourceType() != DeckSourceType::File) {
            return false;
        }
        if (!setCurrentState(GST_STATE_READY)) {
            return false;
        }
        stopped_ = true;
        return true;
    }

    bool stopAtInAndClearProgramFrame()
    {
        if (!isAvailable()) {
            return false;
        }
        const int deckIndex = currentDeck_.load(std::memory_order_relaxed);
        Deck *deck = currentDeck();
        if (!deck || deck->sourceType() != DeckSourceType::File || deckIndex < 0
            || !stop()) {
            return false;
        }
#if defined(_WIN32) || defined(__APPLE__)
        if (!programGpuCompositor_->suspendDeckFrameUpdatesAndClear(
                deckIndex)) {
            programGpuCompositor_->setDeckFrameUpdatesEnabled(deckIndex, true);
            return false;
        }
#else
        showProgramSurface(-1);
#endif
        if (!pause()) {
#if defined(_WIN32) || defined(__APPLE__)
            programGpuCompositor_->setDeckFrameUpdatesEnabled(deckIndex, true);
#else
            showProgramSurface(deckIndex);
#endif
            return false;
        }
        programFrameClearedForStop_ = true;
        stopped_ = true;
        return true;
    }

    DualDeckPlayer::ProgramFrameRestoreStatus restoreCurrentProgramFrame()
    {
        if (!isAvailable()) {
            return DualDeckPlayer::ProgramFrameRestoreStatus::Error;
        }
        const int deckIndex = currentDeck_.load(std::memory_order_relaxed);
        Deck *deck = currentDeck();
        if (!deck || deck->sourceType() != DeckSourceType::File || deckIndex < 0) {
            return DualDeckPlayer::ProgramFrameRestoreStatus::Error;
        }
        const auto failRestore = [this, deckIndex] {
#if defined(_WIN32) || defined(__APPLE__)
            (void)programGpuCompositor_->suspendDeckFrameUpdatesAndClear(
                deckIndex);
#else
            showProgramSurface(-1);
#endif
            programFrameRestorePending_ = false;
            programFrameRestoreStartedUs_ = 0;
            programFrameClearedForStop_ = true;
            stopped_ = true;
            return DualDeckPlayer::ProgramFrameRestoreStatus::Error;
        };
        if (!programFrameClearedForStop_ && !stopped_
            && !programFrameRestorePending_) {
            return DualDeckPlayer::ProgramFrameRestoreStatus::Ready;
        }
        if (!programFrameRestorePending_) {
            if (!setCurrentState(GST_STATE_READY)) {
                return failRestore();
            }
#if defined(_WIN32) || defined(__APPLE__)
            programGpuCompositor_->setDeckFrameUpdatesEnabled(deckIndex, true);
#else
            showProgramSurface(deckIndex);
#endif
            if (!setCurrentState(GST_STATE_PAUSED)) {
                return failRestore();
            }
            programFrameRestorePending_ = true;
            programFrameRestoreStartedUs_ = g_get_monotonic_time();
        }
        GstState restoredState = GST_STATE_VOID_PENDING;
        GstState pendingState = GST_STATE_VOID_PENDING;
        const GstStateChangeReturn restoreResult = gst_element_get_state(
            deck->pipeline(), &restoredState, &pendingState, 0);
        if (restoreResult == GST_STATE_CHANGE_FAILURE) {
            return failRestore();
        }
        if (restoredState != GST_STATE_PAUSED
            || pendingState != GST_STATE_VOID_PENDING
#if defined(_WIN32) || defined(__APPLE__)
            || !programGpuCompositor_->deckFrameReady(deckIndex)
#endif
        ) {
            if (programFrameRestoreStartedUs_ <= 0 ||
                g_get_monotonic_time() - programFrameRestoreStartedUs_ >=
                    kProgramFrameRestoreTimeoutUs) {
                return failRestore();
            }
            return DualDeckPlayer::ProgramFrameRestoreStatus::Loading;
        }
        if (pendingEvent_ == Event::EndOfStream
            && pendingEventDeck_ == deckIndex) {
            pendingEvent_ = Event::None;
            pendingEventDetail_.clear();
            pendingEventDeck_ = kNoPendingEventDeck;
        }
        programFrameRestorePending_ = false;
        programFrameRestoreStartedUs_ = 0;
        programFrameClearedForStop_ = false;
        stopped_ = false;
        return DualDeckPlayer::ProgramFrameRestoreStatus::Ready;
    }

    qint64 positionMs() const
    {
        const Deck *deck = currentDeck();
        if (!isAvailable() || !deck || deck->sourceType() != DeckSourceType::File) return -1;
        return stopped_ ? deck->inPointMs() : deck->positionMs();
    }

    qint64 durationMs() const
    {
        const Deck *deck = currentDeck();
        return isAvailable() && deck ? deck->durationMs() : -1;
    }

    bool seek(qint64 positionMs)
    {
        if (!isAvailable()) {
            return false;
        }
        pollBuses();
        const int deckIndex = currentDeck_.load(std::memory_order_relaxed);
        Deck *deck = currentDeck();
        if (!deck || deck->sourceType() != DeckSourceType::File) {
            return false;
        }
        const bool wasActive =
            audioMeter_.active();
        audioMeter_.reset(true);
        audioMeter_.setActive(wasActive);
        const bool succeeded = deck->seek(positionMs);
        if (succeeded) {
            stopped_ = false;
            if (pendingEvent_ == Event::EndOfStream
                && pendingEventDeck_ == deckIndex) {
                pendingEvent_ = Event::None;
                pendingEventDetail_.clear();
                pendingEventDeck_ = kNoPendingEventDeck;
            }
        }
        return succeeded;
    }

    bool setRate(double rate)
    {
        Deck *deck = currentDeck();
        return isAvailable() && deck && deck->setRate(rate);
    }

    AudioLevels audioLevels()
    {
        const Deck *deck = currentDeck();
        return audioMeter_.levels(deck ? deck->audioPipeline() : nullptr, programAudioOutput_.pipeline());
    }

    void setBackgroundColor(const QColor &color)
    {
#if defined(_WIN32) || defined(__APPLE__)
        if (programGpuCompositor_)
            programGpuCompositor_->setBackgroundColor(color.rgb());
#else
        Q_UNUSED(color);
#endif
    }

    void setFtbCoverage(qreal coverage)
    {
        ftbCoverage_ = qBound<qreal>(0.0, coverage, 1.0);
#if defined(_WIN32) || defined(__APPLE__)
        if (programGpuCompositor_) {
            programGpuCompositor_->setFtbCoverage(
                static_cast<float>(ftbCoverage_));
        }
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
        updateFtbOverlay(currentDeck_.load(std::memory_order_relaxed));
#endif
        programAudioOutput_.setFtbCoverage(ftbCoverage_);
    }

    void setMasterVolume(double volume)
    {
        programAudioOutput_.setMasterVolume(volume);
    }

    void setBgmItemEq(double lowDb, double midDb, double highDb)
    {
        bgm_.setItemEq(lowDb, midDb, highDb);
    }
    void setItemEq(double lowDb, double midDb, double highDb)
    {
        programAudioOutput_.setDeckEq(currentDeck_.load(std::memory_order_relaxed), lowDb, midDb, highDb);
    }
    void setCueEq(double lowDb, double midDb, double highDb)
    {
        programAudioOutput_.setDeckEq(cueDeck_, lowDb, midDb, highDb);
    }
    void setMasterDelayFrames(int frames) { programAudioOutput_.setMasterDelayFrames(frames); }
    void setItemDelayFrames(int frames) {
        programAudioOutput_.setDeckDelayFrames(currentDeck_.load(std::memory_order_relaxed), frames);
    }
    void setCueDelayFrames(int frames) { programAudioOutput_.setDeckDelayFrames(cueDeck_, frames); }
    void setBgmItemDelayFrames(int frames) { bgm_.setItemDelayFrames(frames); }

    void setMasterEq(double lowDb, double midDb, double highDb)
    {
        programAudioOutput_.setMasterEq(lowDb, midDb, highDb);
    }

    void setItemVolume(double volume)
    {
        itemVolume_ = std::clamp(volume, 0.0, 2.0);
        if (Deck *deck = currentDeck()) deck->setItemVolume(itemVolume_);
    }

    void setMasterMuted(bool muted)
    {
        programAudioOutput_.setMasterMuted(muted);
    }

    bool loadBgm(const QString &filePath, qint64 inPointMs,
                 qint64 outPointMs)
    {
        return isAvailable() && bgm_.load(filePath, inPointMs, outPointMs);
    }

    bool playBgm(BgmPlayFailure *failure)
    {
        if (failure) *failure = BgmPlayFailure::Other;
        if (programAudioOutput_.deviceChanging()) {
            if (failure) *failure = BgmPlayFailure::AudioOutputChanging;
            return false;
        }
        if (!programAudioOutput_.ready()) {
            if (failure) *failure = BgmPlayFailure::AudioOutputUnavailable;
            return false;
        }
        bool played = false;
        if (!isAvailable()) {
            bgm_.refreshStatus();
            played = bgm_.cachedStatus() == BgmStatus::Playing;
        } else {
            played = bgm_.play();
        }
        if (played && failure) *failure = BgmPlayFailure::None;
        return played;
    }

    bool pauseBgm()
    {
        return isAvailable() && bgm_.pause();
    }

    bool stopBgm()
    {
        if (!isAvailable()) return false;
        const auto result = bgm_.stop();
        if (result == BgmPlayback::StopResult::Stopped) audioMeter_.resetBgmSignal();
        return result != BgmPlayback::StopResult::Failed;
    }

    bool clearBgm()
    {
        return bgm_.clear();
    }

    bool seekBgm(qint64 positionMs)
    {
        return isAvailable() && bgm_.seek(positionMs);
    }

    qint64 bgmPositionMs() const
    {
        return isAvailable() ? bgm_.positionMs() : -1;
    }

    qint64 bgmDurationMs() const
    {
        return isAvailable() ? bgm_.durationMs() : -1;
    }

    BgmStatus bgmStatus()
    {
        return bgm_.status();
    }

    QString bgmErrorDetail() const
    {
        return bgm_.errorDetail();
    }

    BgmEvent takeBgmEvent(QString &detail)
    {
        return bgm_.takeEvent(detail);
    }

    AudioLevel bgmSignalLevel()
    {
        return audioMeter_.bgmSignalLevel();
    }

    void setBgmItemVolume(double volume)
    {
        bgm_.setItemVolume(volume);
    }

    void setBgmGain(double gain)
    {
        programAudioOutput_.setBgmGain(gain);
    }

    void setBgmManualFadeGain(double gain)
    {
        programAudioOutput_.setBgmManualFadeGain(gain);
    }

    void setBgmTransportGain(double gain)
    {
        programAudioOutput_.setBgmTransportGain(gain);
    }

    void setBgmMeterActive(bool active)
    {
        audioMeter_.setBgmActive(active);
    }

    PowerPointNavigationState powerPointNavigationState()
    {
        Deck *deck = currentDeck();
        if (isAvailable() && deck) return deck->powerPointNavigationState();
        return {};
    }

    PowerPointCommandResponse nextPowerPointSlide()
    {
        Deck *deck = currentDeck();
        if (isAvailable() && deck) return deck->nextPowerPointSlide();
        return {};
    }

    PowerPointCommandResponse previousPowerPointSlide()
    {
        Deck *deck = currentDeck();
        if (isAvailable() && deck) return deck->previousPowerPointSlide();
        return {};
    }

    PdfNavigationState pdfNavigationState()
    {
        Deck *deck = currentDeck();
        if (isAvailable() && deck) return deck->pdfNavigationState();
        return {};
    }

    void cancelDocumentNavigation()
    {
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
        // TAKE may already have switched currentDeck_ while the outgoing
        // input is still visible in a crossfade. Invalidate both owned slots.
        for (int index = 0; index < 2; ++index) {
#ifdef _WIN32
            if (powerPointSources_) powerPointSources_->cancelNavigation(index);
#endif
            if (pdfSources_) pdfSources_->cancelNavigation(index);
        }
#endif
    }

    PdfCommandResponse nextPdfPage()
    {
        Deck *deck = currentDeck();
        if (isAvailable() && deck) return deck->nextPdfPage();
        return {};
    }

    PdfCommandResponse previousPdfPage()
    {
        Deck *deck = currentDeck();
        if (isAvailable() && deck) return deck->previousPdfPage();
        return {};
    }

    void processBusMessages()
    {
        updateProgramOutputDevice();
        if (!isAvailable()) {
            return;
        }
        pollBuses();
        refreshCueStatus();
#if defined(_WIN32) || defined(__APPLE__)
        if (pendingEvent_ != Event::Error && programGpuCompositor_) {
            std::string detail;
            if (programGpuCompositor_->takeRuntimeError(detail)) {
                pendingEvent_ = Event::Error;
                pendingEventDetail_ = QStringLiteral(
                    "An error occurred in the ON AIR GPU output.\n")
                    + QString::fromUtf8(detail);
                pendingEventDeck_ = kNoPendingEventDeck;
            }
        }
#endif
    }

    Event takeEvent(QString &detail, bool *programAudioError)
    {
        const Event result = pendingEvent_;
        if (programAudioError)
            *programAudioError = result == Event::Error && pendingEventDeck_ == -1;
        detail = pendingEventDetail_;
        pendingEvent_ = Event::None;
        pendingEventDetail_.clear();
        pendingEventDeck_ = kNoPendingEventDeck;
        return result;
    }

private:
    using DeckSourceType = DeckSourceSession::Type;
    using Deck = DeckSourceSession;
#if !defined(_WIN32) && !defined(__APPLE__)
    struct DeckSurfaces { QWidget *video = nullptr; QWidget *monitor = nullptr; };
    std::array<DeckSurfaces, 2> deckSurfaces_;
#endif

    void resetAudioLevelForDeck(int deckIndex, bool collectPreroll = false)
    {
        GstElement *inputLevel = nullptr;
        GstElement *itemLevel = nullptr;
        if (deckIndex >= 0 && deckIndex < static_cast<int>(decks_.size())) {
            const Deck &deck = decks_[deckIndex];
            inputLevel = deck.inputAudioLevel();
            if (deck.sourceType() == DeckSourceType::File || deck.sourceType() == DeckSourceType::Uvc)
                itemLevel = programAudioOutput_.itemLevel(deckIndex);
        }
        audioMeter_.setDeckRoute(inputLevel, itemLevel, collectPreroll);
    }

    bool clearDeckSource(int deckIndex, const QString &errorDetail)
    {
        if (deckIndex < 0 || deckIndex >= static_cast<int>(decks_.size())) return false;
        const auto result = decks_[deckIndex].clearForReplacement();
        if (!result.succeeded) {
            cueStatus_ = CueStatus::Error;
            cueErrorDetail_ = errorDetail;
        }
        return result.succeeded;
    }

    bool setDeckStandby(int deckIndex, const QString &errorDetail)
    {
        if (deckIndex < 0 || deckIndex >= static_cast<int>(decks_.size())) return false;
        const auto result = decks_[deckIndex].standbyAfterTake();
        if (!result.succeeded) {
            cueErrorDetail_ = errorDetail;
            qWarning() << errorDetail;
        }
        return result.succeeded;
    }

    void detachCallbacks()
    {
        if (callbacksDetached_) return;
        callbacksDetached_ = true;
        for (Deck &deck : decks_) deck.detachCallbacks();
        bgm_.detachCallbacks();
        programAudioOutput_.detachCallbacks();
#if defined(_WIN32) || defined(__APPLE__)
        dsk_.detachCallbacks();
#endif
    }

    QWidget *addVideoSurface(QWidget *parent, QStackedLayout *layout,
                             const QString &name)
    {
        auto *surface = new QWidget(parent);
        surface->setObjectName(name);
        surface->setFocusPolicy(Qt::NoFocus);
        surface->setAttribute(Qt::WA_NativeWindow);
        surface->setAttribute(Qt::WA_OpaquePaintEvent);
        surface->setAttribute(Qt::WA_TransparentForMouseEvents);
        surface->setAutoFillBackground(true);
        QPalette palette = surface->palette();
        palette.setColor(QPalette::Window, Qt::black);
        surface->setPalette(palette);
        layout->addWidget(surface);
        surface->winId();
        return surface;
    }

    bool initializeVideoSurfaces()
    {
        if (!videoWindow_ || !monitorWindow_
            || videoWindow_->layout() || monitorWindow_->layout()) {
            return false;
        }

        videoLayout_ = new QStackedLayout(videoWindow_);
        videoLayout_->setContentsMargins(0, 0, 0, 0);
        videoLayout_->setStackingMode(QStackedLayout::StackAll);
        monitorLayout_ = new QStackedLayout(monitorWindow_);
        monitorLayout_->setContentsMargins(0, 0, 0, 0);
        monitorLayout_->setStackingMode(QStackedLayout::StackAll);

#if defined(_WIN32) || defined(__APPLE__)
        gpuVideoSurface_ = addVideoSurface(
            videoWindow_, videoLayout_, QStringLiteral("program-gpu-output"));
        gpuMonitorSurface_ = addVideoSurface(
            monitorWindow_, monitorLayout_, QStringLiteral("monitor-gpu-output"));
#else
        blackVideoSurface_ = addVideoSurface(
            videoWindow_, videoLayout_, QStringLiteral("program-black"));
        blackMonitorSurface_ = addVideoSurface(
            monitorWindow_, monitorLayout_, QStringLiteral("monitor-black"));
        for (int index = 0; index < static_cast<int>(decks_.size()); ++index) {
            const QString suffix = index == 0
                ? QStringLiteral("a") : QStringLiteral("b");
            deckSurfaces_[index].video = addVideoSurface(
                videoWindow_, videoLayout_,
                QStringLiteral("program-deck-") + suffix);
            deckSurfaces_[index].monitor = addVideoSurface(
                monitorWindow_, monitorLayout_,
                QStringLiteral("monitor-deck-") + suffix);
        }
#endif
        return true;
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    void showProgramSurface(int deckIndex)
    {
        if (deckIndex >= 0 && deckIndex < static_cast<int>(decks_.size())
            && decks_[deckIndex].hasVideo()) {
            videoLayout_->setCurrentWidget(deckSurfaces_[deckIndex].video);
            monitorLayout_->setCurrentWidget(deckSurfaces_[deckIndex].monitor);
            deckSurfaces_[deckIndex].video->raise();
            deckSurfaces_[deckIndex].monitor->raise();
            if (GST_IS_VIDEO_OVERLAY(decks_[deckIndex].videoSink())) {
                gst_video_overlay_expose(
                    GST_VIDEO_OVERLAY(decks_[deckIndex].videoSink()));
            }
            if (GST_IS_VIDEO_OVERLAY(decks_[deckIndex].monitorSink())) {
                gst_video_overlay_expose(
                    GST_VIDEO_OVERLAY(decks_[deckIndex].monitorSink()));
            }
        }
        updateFtbOverlay(deckIndex);
    }

    void updateFtbOverlay(int deckIndex)
    {
        const bool noVideo = deckIndex < 0
            || deckIndex >= static_cast<int>(decks_.size())
            || !decks_[deckIndex].hasVideo();
        const qreal opacity = noVideo
            ? 1.0
            : 1.0 - ((1.0 - ftbCoverage_) * itemVideoFadeProgress_);
        for (QWidget *surface : {blackVideoSurface_, blackMonitorSurface_}) {
            if (opacity > 0.0) {
                setSurfaceOpacity(surface, opacity);
                surface->show();
                surface->raise();
            } else {
                surface->hide();
            }
        }
    }
#endif

    bool initializeDeck(int index)
    {
#if defined(_WIN32) || defined(__APPLE__)
        return decks_[index].initialize(index,
            static_cast<int>(outputGeometry_.width), static_cast<int>(outputGeometry_.height),
            &Impl::handleBusSync, this);
#else
        return decks_[index].initialize(index, 1920, 1080, &Impl::handleBusSync, this,
            static_cast<guintptr>(deckSurfaces_[index].video->winId()),
            static_cast<guintptr>(deckSurfaces_[index].monitor->winId()));
#endif
    }

    Deck *currentDeck()
    {
        const int index = currentDeck_.load(std::memory_order_relaxed);
        return index >= 0 ? &decks_[index] : nullptr;
    }

    const Deck *currentDeck() const
    {
        const int index = currentDeck_.load(std::memory_order_relaxed);
        return index >= 0 ? &decks_[index] : nullptr;
    }

    bool setCurrentState(GstState state)
    {
        Deck *deck = currentDeck();
        if (!deck || deck->sourceType() != DeckSourceType::File) {
            return false;
        }

        if (deck->setFileState(state)) return true;
        pollBuses();
        if (pendingEvent_ == Event::Error
            && !pendingEventDetail_.isEmpty()) {
            qWarning() << "Failed to set current deck state to"
                       << gst_element_state_get_name(state) << ':'
                       << pendingEventDetail_;
        }
        return false;
    }

    void refreshCueStatus()
    {
        if (cueStatus_ != CueStatus::Loading) return;
        if (cueDeck_ < 0) {
            cueStatus_ = CueStatus::Error;
            cueErrorDetail_ = QStringLiteral("No standby player is available.");
            return;
        }
        const auto result = decks_[cueDeck_].refreshPreparation();
        cueStatus_ = result.state;
        cueErrorDetail_ = result.errorDetail;
    }

    void drainBus(GstBus *bus)
    {
        if (!bus) {
            return;
        }
        while (GstMessage *message = gst_bus_pop(bus)) {
            gst_message_unref(message);
        }
    }

    void pollBuses()
    {
        if (programAudioOutput_.deviceChanging()) drainBus(programAudioOutput_.bus());
        else pollBus(programAudioOutput_.bus(), -1);
        bgm_.pollBus();
        for (int index = 0; index < static_cast<int>(decks_.size()); ++index) {
            if (!decks_[index].filePreparationActive()) pollBus(decks_[index].bus(), index);
#if defined(_WIN32) || defined(__APPLE__)
            pollBus(decks_[index].mappedAudioBus(), index);
#endif
        }
#if defined(_WIN32) || defined(__APPLE__)
        for (int index = 0; index < static_cast<int>(decks_.size());
             ++index) {
            pollSourceError(index);
        }
#endif
#if defined(_WIN32) || defined(__APPLE__)
        dsk_.pollBus();
#endif
    }

#if defined(_WIN32) || defined(__APPLE__)
    void pollSourceError(int deckIndex)
    {
        if (deckIndex < 0 || deckIndex >= static_cast<int>(decks_.size())) return;
        QString detail;
        if (!decks_[deckIndex].takeSourceError(detail)) return;
        if (deckIndex == cueDeck_ && cueStatus_ != CueStatus::Empty) {
            cueStatus_ = CueStatus::Error;
            cueErrorDetail_ = detail;
        } else if (deckIndex == currentDeck_.load(std::memory_order_relaxed)) {
            pendingEvent_ = Event::Error;
            pendingEventDetail_ = detail;
            pendingEventDeck_ = deckIndex;
        }
    }

#endif

    void pollBus(GstBus *bus, int deckIndex)
    {
        if (deckIndex >= 0 && bus == decks_[deckIndex].bus()
            && decks_[deckIndex].filePreparationActive()) return;
        if (!bus) {
            return;
        }
        while (GstMessage *message = gst_bus_pop(bus)) {
            const GstMessageType type = GST_MESSAGE_TYPE(message);
            if (type == GST_MESSAGE_STATE_CHANGED) {
              GstState oldState, newState, pendingState;
              gst_message_parse_state_changed(message, &oldState, &newState,
                                              &pendingState);
              qDebug() << "GST_STATE" << GST_MESSAGE_SRC_NAME(message)
                       << gst_element_state_get_name(oldState) << "->"
                       << gst_element_state_get_name(newState) << "pending="
                       << gst_element_state_get_name(pendingState);
            }
            if (type == GST_MESSAGE_LATENCY) {
                // PROGRAM owns its latency updates in ProgramAudioOutput;
                // this bus loop handles only the producing deck pipelines.
                GstElement *pipeline = deckIndex < 0
                    ? nullptr
                    : (bus == decks_[deckIndex].bus()
                           ? decks_[deckIndex].pipeline()
                           : decks_[deckIndex].audioPipeline());
                if (pipeline && !gst_bin_recalculate_latency(GST_BIN(pipeline))) {
                    qWarning() << "Could not update audio pipeline latency";
                }
            } else if (type == GST_MESSAGE_ERROR) {
                if (deckIndex < 0) programAudioOutput_.reportFailure();
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                qWarning().noquote()
                    << "GST_BUS" << (error ? error->message : "Unknown error")
                    << (debug ? debug : "");
                const QString detail = QString::fromUtf8(
                    error ? error->message : "Unknown GStreamer error");
                if (deckIndex >= 0 && AsyncFilePreparation::isOfflineError(error))
                    decks_[deckIndex].reportFileOffline();
                g_clear_error(&error);
                g_free(debug);

                if (deckIndex >= 0 && deckIndex == cueDeck_
                    && cueStatus_ != CueStatus::Empty) {
                    cueStatus_ = CueStatus::Error;
                    cueErrorDetail_ = detail;
                } else if (deckIndex < 0 ||
                           deckIndex == currentDeck_.load(
                                            std::memory_order_relaxed)) {
                    pendingEventDetail_ = detail;
                    pendingEvent_ = Event::Error;
                    pendingEventDeck_ = deckIndex;
                }
            } else if (type == GST_MESSAGE_EOS
                       && deckIndex >= 0
                       && decks_[deckIndex].sourceType()
                           == DeckSourceType::File
                       && deckIndex
                           == currentDeck_.load(std::memory_order_relaxed)
                       && pendingEvent_ != Event::Error) {
                pendingEvent_ = Event::EndOfStream;
                pendingEventDeck_ = deckIndex;
            }
            gst_message_unref(message);
        }
    }

    static GstBusSyncReply handleBusSync(GstBus *, GstMessage *message,
                                         gpointer data)
    {
        auto *player = static_cast<Impl *>(data);
        const GstStructure *structure = gst_message_get_structure(message);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT && structure
            && gst_structure_has_name(structure, "level")) {
            player->audioMeter_.update(message);
            gst_message_unref(message);
            return GST_BUS_DROP;
        }

        if (!gst_is_video_overlay_prepare_window_handle_message(message)) {
            return GST_BUS_PASS;
        }

#if !defined(_WIN32) && !defined(__APPLE__)
        GstObject *source = GST_MESSAGE_SRC(message);
        for (Deck &deck : player->decks_) {
            if (player->belongsToSink(source, deck.monitorSink())) {
                player->applyWindowHandle(source,
                                          deck.monitorWindowHandle());
                gst_message_unref(message);
                return GST_BUS_DROP;
            }
            if (player->belongsToSink(source, deck.videoSink())) {
                player->applyWindowHandle(source,
                                          deck.videoWindowHandle());
                gst_message_unref(message);
                return GST_BUS_DROP;
            }
        }
#endif
        return GST_BUS_PASS;
    }

    bool belongsToSink(GstObject *object, GstElement *sink) const
    {
        return object && sink
            && (object == GST_OBJECT(sink)
                || gst_object_has_as_ancestor(object, GST_OBJECT(sink)));
    }

    void applyWindowHandle(GstObject *object, guintptr windowHandle)
    {
        if (object && windowHandle != 0 && GST_IS_VIDEO_OVERLAY(object)) {
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(object), windowHandle);
        }
    }

    QWidget *videoWindow_ = nullptr;
    QWidget *monitorWindow_ = nullptr;
    QStackedLayout *videoLayout_ = nullptr;
    QStackedLayout *monitorLayout_ = nullptr;
#if !defined(_WIN32) && !defined(__APPLE__)
    QWidget *blackVideoSurface_ = nullptr;
    QWidget *blackMonitorSurface_ = nullptr;
#endif
#if defined(_WIN32) || defined(__APPLE__)
    QWidget *gpuVideoSurface_ = nullptr;
    QWidget *gpuMonitorSurface_ = nullptr;
#endif
    UvcSourceSessionManager *uvcSourceSessions_ = nullptr;
#ifdef _WIN32
    std::unique_ptr<PowerPointSourceSessionManager> powerPointSources_;
#endif
#if defined(_WIN32) || defined(VTRPON_PDF_SUPPORT)
    std::unique_ptr<PdfSourceSessionManager> pdfSources_;
    bool nativeDocumentShutdownRequested_ = false;
#endif
#if defined(_WIN32) || defined(__APPLE__)
    ProgramGpuCompositor::OutputGeometry outputGeometry_;
    std::unique_ptr<ProgramGpuCompositor> programGpuCompositor_;
#endif
#if defined(_WIN32) || defined(__APPLE__)
    DskPlayback dsk_;
#endif
    std::array<Deck, 2> decks_;
    BgmPlayback bgm_;
    ProgramAudioOutput programAudioOutput_;
    std::atomic_int currentDeck_ = {-1};
    int cueDeck_ = -1;
    bool crossfadeActive_ = false;
    int crossfadeIncomingDeck_ = -1;
    int crossfadeOutgoingDeck_ = -1;
    float itemVideoFadeProgress_ = 1.0F;
    CueStatus cueStatus_ = CueStatus::Empty;
    QString cueErrorDetail_;
    bool available_ = false;
    bool shutDown_ = false;
    bool shutdownWorkerStarted_ = false;
    std::atomic_bool shutdownWorkerDone_{false};
    bool callbacksDetached_ = false;
    bool shutdownConfirmed_ = false;
    bool stopped_ = true;
    bool programFrameClearedForStop_ = false;
    bool programFrameRestorePending_ = false;
    gint64 programFrameRestoreStartedUs_ = 0;
    ProgramAudioMeter audioMeter_;
    double itemVolume_ = 1.0;
    double ftbCoverage_ = 0.0;
    Event pendingEvent_ = Event::None;
    QString pendingEventDetail_;
    int pendingEventDeck_ = kNoPendingEventDeck;
};

DualDeckPlayer::DualDeckPlayer(QWidget *videoWindow, QWidget *monitorWindow,
                               bool use4kProgramOutput,
                               UvcSourceSessionManager *uvcSourceSessions)
    : impl_(std::make_unique<Impl>(videoWindow, monitorWindow,
                                  use4kProgramOutput, uvcSourceSessions))
{
}

DualDeckPlayer::~DualDeckPlayer()
{
    if (!impl_) {
        return;
    }
    impl_->shutdown();
    if (!impl_->canReleaseAfterShutdown()) {
        // Keep the Impl storage itself alive if GStreamer did not confirm a
        // terminal state. Streaming callbacks may still hold its addresses.
        (void)impl_.release();
    }
}

bool DualDeckPlayer::isAvailable() const { return impl_->isAvailable(); }

VideoFrameCapture DualDeckPlayer::captureStillFrame() const { return impl_->captureStillFrame(); }

DualDeckPlayer::FullscreenOutputStats DualDeckPlayer::fullscreenOutputStats() const
{
    return impl_->fullscreenOutputStats();
}
void DualDeckPlayer::requestShutdown() { impl_->requestShutdown(); }
bool DualDeckPlayer::shutdownComplete() { return impl_->shutdownComplete(); }
bool DualDeckPlayer::prepareNativeDocumentShutdown()
{
    return impl_->prepareNativeDocumentShutdown();
}

void DualDeckPlayer::shutdown() { impl_->shutdown(); }
void DualDeckPlayer::refreshWindowHandles()
{
    impl_->refreshWindowHandles();
}
bool DualDeckPlayer::cue(const DeckSource &source)
{
    return impl_->cue(source);
}
bool DualDeckPlayer::clearCue() { return impl_->clearCue(); }
DualDeckPlayer::CueStatus DualDeckPlayer::cueStatus()
{
    return impl_->cueStatus();
}
bool DualDeckPlayer::onAirMediaOffline() const { return impl_->onAirMediaOffline(); }
bool DualDeckPlayer::dskMediaOffline() const { return impl_->dskMediaOffline(); }
bool DualDeckPlayer::cueMediaOffline() const { return impl_->cueMediaOffline(); }
bool DualDeckPlayer::dskCueMediaOffline() const { return impl_->dskCueMediaOffline(); }
bool DualDeckPlayer::bgmMediaOffline() const { return impl_->bgmMediaOffline(); }

QString DualDeckPlayer::cueErrorDetail() const
{
    return impl_->cueErrorDetail();
}
bool DualDeckPlayer::takeCue(bool crossfade, float itemVideoFadeProgress,
                             TakeCueFailure *failure)
{
    return impl_->takeCue(crossfade, itemVideoFadeProgress, failure);
}

bool DualDeckPlayer::crossfadeActive() const
{
    return impl_->crossfadeActive();
}

int DualDeckPlayer::currentDeckIndex() const { return impl_->currentDeckIndex(); }

bool DualDeckPlayer::setCrossfadeProgress(float progress)
{
    return impl_->setCrossfadeProgress(progress);
}

bool DualDeckPlayer::setItemVideoFadeProgress(float progress)
{
    return impl_->setItemVideoFadeProgress(progress);
}

DualDeckPlayer::ClearMediaResult DualDeckPlayer::clearMedia()
{
    return impl_->clearMedia();
}
bool DualDeckPlayer::cueDsk(const QString &filePath, qint64 inPointMs,
                            qint64 outPointMs)
{
    return impl_->cueDsk(filePath, inPointMs, outPointMs);
}
bool DualDeckPlayer::takeDskCue(bool playOnShow, bool crossfade,
                                float videoOpacity)
{
    return impl_->takeDskCue(playOnShow, crossfade, videoOpacity);
}
bool DualDeckPlayer::dskCrossfadeActive() const
{
    return impl_->dskCrossfadeActive();
}
bool DualDeckPlayer::setDskCrossfadeProgress(float progress)
{
    return impl_->setDskCrossfadeProgress(progress);
}
bool DualDeckPlayer::setDskVideoOpacity(float opacity)
{
    return impl_->setDskVideoOpacity(opacity);
}
bool DualDeckPlayer::clearDskCue() { return impl_->clearDskCue(); }
bool DualDeckPlayer::playDsk() { return impl_->playDsk(); }
bool DualDeckPlayer::pauseDsk() { return impl_->pauseDsk(); }
bool DualDeckPlayer::dskIsPlaying() const { return impl_->dskIsPlaying(); }
bool DualDeckPlayer::pauseDskAtEnd() { return impl_->pauseDskAtEnd(); }
bool DualDeckPlayer::restartDskFromIn(bool play)
{
    return impl_->restartDskFromIn(play);
}
DualDeckPlayer::DskStatus DualDeckPlayer::dskStatus()
{
    return impl_->dskStatus();
}
DualDeckPlayer::DskStatus DualDeckPlayer::dskCueStatus()
{
    return impl_->dskCueStatus();
}
qint64 DualDeckPlayer::dskPositionMs() const
{
    return impl_->dskPositionMs();
}
qint64 DualDeckPlayer::dskDurationMs() const
{
    return impl_->dskDurationMs();
}
bool DualDeckPlayer::clearDsk() { return impl_->clearDsk(); }
QString DualDeckPlayer::dskErrorDetail() const
{
    return impl_->dskErrorDetail();
}
DualDeckPlayer::DskEvent DualDeckPlayer::takeDskEvent(QString &detail)
{
    return impl_->takeDskEvent(detail);
}
bool DualDeckPlayer::play() { return impl_->play(); }
bool DualDeckPlayer::pause() { return impl_->pause(); }
bool DualDeckPlayer::isPlaying() const { return impl_->isPlaying(); }
bool DualDeckPlayer::powerPointSourceIsOnAir() const
{
    return impl_->powerPointSourceIsOnAir();
}
bool DualDeckPlayer::powerPointCueIsActive() const
{
    return impl_->powerPointCueIsActive();
}
void DualDeckPlayer::warmUpPowerPointApplication()
{
    impl_->warmUpPowerPointApplication();
}
bool DualDeckPlayer::preparePowerPointConversion(
    bool *powerPointCueReleased, QString *errorDetail)
{
    return impl_->preparePowerPointConversion(powerPointCueReleased,
                                              errorDetail);
}
bool DualDeckPlayer::currentSourceHasAudio() const
{
    return impl_->currentSourceHasAudio();
}
bool DualDeckPlayer::stop() { return impl_->stop(); }
bool DualDeckPlayer::stopAtInAndClearProgramFrame()
{
    return impl_->stopAtInAndClearProgramFrame();
}
DualDeckPlayer::ProgramFrameRestoreStatus
DualDeckPlayer::restoreCurrentProgramFrame()
{
    return impl_->restoreCurrentProgramFrame();
}
qint64 DualDeckPlayer::positionMs() const { return impl_->positionMs(); }
qint64 DualDeckPlayer::durationMs() const { return impl_->durationMs(); }
bool DualDeckPlayer::seek(qint64 positionMs) { return impl_->seek(positionMs); }
bool DualDeckPlayer::setRate(double rate) { return impl_->setRate(rate); }
PowerPointNavigationState DualDeckPlayer::powerPointNavigationState()
{
    return impl_->powerPointNavigationState();
}
PowerPointCommandResponse DualDeckPlayer::nextPowerPointSlide()
{
    return impl_->nextPowerPointSlide();
}
PowerPointCommandResponse DualDeckPlayer::previousPowerPointSlide()
{
    return impl_->previousPowerPointSlide();
}
PdfNavigationState DualDeckPlayer::pdfNavigationState()
{
    return impl_->pdfNavigationState();
}
void DualDeckPlayer::cancelDocumentNavigation()
{
    impl_->cancelDocumentNavigation();
}

PdfCommandResponse DualDeckPlayer::nextPdfPage()
{
    return impl_->nextPdfPage();
}
PdfCommandResponse DualDeckPlayer::previousPdfPage()
{
    return impl_->previousPdfPage();
}
DualDeckPlayer::AudioLevels DualDeckPlayer::audioLevels()
{
    return impl_->audioLevels();
}
void DualDeckPlayer::setBackgroundColor(const QColor &color)
{
    impl_->setBackgroundColor(color);
}

void DualDeckPlayer::setFtbCoverage(qreal coverage)
{
    impl_->setFtbCoverage(coverage);
}
void DualDeckPlayer::setMasterVolume(double volume)
{
    impl_->setMasterVolume(volume);
}

void DualDeckPlayer::setBgmItemEq(double lowDb, double midDb, double highDb)
{
    impl_->setBgmItemEq(lowDb, midDb, highDb);
}
void DualDeckPlayer::setItemEq(double lowDb, double midDb, double highDb)
{
    impl_->setItemEq(lowDb, midDb, highDb);
}
void DualDeckPlayer::setCueEq(double lowDb, double midDb, double highDb)
{
    impl_->setCueEq(lowDb, midDb, highDb);
}
void DualDeckPlayer::setMasterDelayFrames(int frames) { impl_->setMasterDelayFrames(frames); }
void DualDeckPlayer::setItemDelayFrames(int frames) { impl_->setItemDelayFrames(frames); }
void DualDeckPlayer::setCueDelayFrames(int frames) { impl_->setCueDelayFrames(frames); }
void DualDeckPlayer::setBgmItemDelayFrames(int frames) { impl_->setBgmItemDelayFrames(frames); }

void DualDeckPlayer::setMasterEq(double lowDb, double midDb, double highDb)
{
    impl_->setMasterEq(lowDb, midDb, highDb);
}
void DualDeckPlayer::setItemVolume(double volume)
{
    impl_->setItemVolume(volume);
}
void DualDeckPlayer::setMasterMuted(bool muted)
{
    impl_->setMasterMuted(muted);
}
void DualDeckPlayer::requestProgramOutputDevice(const QString &id) { impl_->requestProgramOutputDevice(id); }
QString DualDeckPlayer::appliedProgramOutputDevice() const { return impl_->appliedProgramOutputDevice(); }
QString DualDeckPlayer::programOutputDeviceStatusKey() const { return impl_->programOutputDeviceStatusKey(); }
bool DualDeckPlayer::programAudioOutputReady() const { return impl_->programAudioOutputReady(); }
bool DualDeckPlayer::loadBgm(const QString &filePath, qint64 inPointMs,
                             qint64 outPointMs)
{
    return impl_->loadBgm(filePath, inPointMs, outPointMs);
}
bool DualDeckPlayer::playBgm(BgmPlayFailure *failure) { return impl_->playBgm(failure); }
bool DualDeckPlayer::pauseBgm() { return impl_->pauseBgm(); }
bool DualDeckPlayer::stopBgm() { return impl_->stopBgm(); }
bool DualDeckPlayer::clearBgm() { return impl_->clearBgm(); }
bool DualDeckPlayer::seekBgm(qint64 positionMs)
{
    return impl_->seekBgm(positionMs);
}
qint64 DualDeckPlayer::bgmPositionMs() const
{
    return impl_->bgmPositionMs();
}
qint64 DualDeckPlayer::bgmDurationMs() const
{
    return impl_->bgmDurationMs();
}
DualDeckPlayer::BgmStatus DualDeckPlayer::bgmStatus()
{
    return impl_->bgmStatus();
}
QString DualDeckPlayer::bgmErrorDetail() const
{
    return impl_->bgmErrorDetail();
}
DualDeckPlayer::BgmEvent DualDeckPlayer::takeBgmEvent(QString &detail)
{
    return impl_->takeBgmEvent(detail);
}
DualDeckPlayer::AudioLevel DualDeckPlayer::bgmSignalLevel()
{
    return impl_->bgmSignalLevel();
}
void DualDeckPlayer::setBgmItemVolume(double volume)
{
    impl_->setBgmItemVolume(volume);
}
void DualDeckPlayer::setBgmGain(double gain)
{
    impl_->setBgmGain(gain);
}
void DualDeckPlayer::setBgmManualFadeGain(double gain)
{
    impl_->setBgmManualFadeGain(gain);
}
void DualDeckPlayer::setBgmTransportGain(double gain)
{
    impl_->setBgmTransportGain(gain);
}
void DualDeckPlayer::setBgmMeterActive(bool active)
{
    impl_->setBgmMeterActive(active);
}
void DualDeckPlayer::processBusMessages() { impl_->processBusMessages(); }
DualDeckPlayer::Event DualDeckPlayer::takeEvent(QString &detail, bool *programAudioError)
{
    return impl_->takeEvent(detail, programAudioError);
}
