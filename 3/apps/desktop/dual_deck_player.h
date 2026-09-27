#pragma once

#include "video_frame_capture.h"

#include <QColor>

#include "pdf_source_session_manager.h"
#include "powerpoint_source_session_manager.h"

#include <QString>
#include <QSize>

#include <memory>

class QWidget;
class UvcSourceSessionManager;

class DualDeckPlayer final
{
public:
    struct AudioLevel {
        double leftPeakDb = -60.0;
        double rightPeakDb = -60.0;
        bool clipped = false;
    };

    struct AudioLevels {
        AudioLevel input;
        AudioLevel itemPostFader;
        AudioLevel masterOutput;
        AudioLevel bgmPostFader;
    };

    struct FullscreenOutputStats {
        QSize resolution; // Empty when fullscreen output is unavailable.
        quint64 presentedFrames = 0;
    };

    enum class Event { None, EndOfStream, Error };
    enum class DskEvent { None, EndOfStream, CueError, Error };
    enum class CueStatus { Empty, Loading, Ready, Error };
    enum class TakeCueFailure { None, AudioOutputChanging, AudioOutputUnavailable, Other };
    enum class DskStatus { Empty, Loading, Ready, On, Error };
    enum class BgmStatus { Empty, Loading, Ready, Playing, Paused, Stopped, Error };
    enum class BgmEvent { None, EndOfStream, Error };
    enum class BgmPlayFailure { None, AudioOutputChanging, AudioOutputUnavailable, Other };
    enum class ClearMediaResult { Unavailable, Failed, Cleared };
    enum class ProgramFrameRestoreStatus { Ready, Loading, Error };
    struct DeckSource {
        enum class Type { File, Uvc, PowerPoint, Pdf };

        Type type = Type::File;
        QString locator;
        QString audioDeviceId;
        qint64 inPointMs = 0;
        qint64 outPointMs = -1;
        double playbackRate = 1.0;
    };

    DualDeckPlayer(QWidget *videoWindow, QWidget *monitorWindow,
                   bool use4kProgramOutput = false,
                   UvcSourceSessionManager *uvcSourceSessions = nullptr);
    ~DualDeckPlayer();

    DualDeckPlayer(const DualDeckPlayer &) = delete;
    DualDeckPlayer &operator=(const DualDeckPlayer &) = delete;

    VideoFrameCapture captureStillFrame() const;
    bool isAvailable() const;
    FullscreenOutputStats fullscreenOutputStats() const;
    bool prepareNativeDocumentShutdown();
    void shutdown();
    void requestShutdown();
    bool shutdownComplete();
    void refreshWindowHandles();

    bool cue(const DeckSource &source);
    bool clearCue();
    CueStatus cueStatus();
    QString cueErrorDetail() const;
    bool takeCue(bool crossfade = false,
                 float itemVideoFadeProgress = 1.0F,
                 TakeCueFailure *failure = nullptr);
    bool crossfadeActive() const;
    int currentDeckIndex() const;
    bool setCrossfadeProgress(float progress);
    bool setItemVideoFadeProgress(float progress);
    ClearMediaResult clearMedia();

    bool cueDsk(const QString &filePath, qint64 inPointMs = 0,
                qint64 outPointMs = -1);
    bool takeDskCue(bool playOnShow, bool crossfade = false,
                    float videoOpacity = 1.0F);
    bool dskCrossfadeActive() const;
    bool setDskCrossfadeProgress(float progress);
    bool setDskVideoOpacity(float opacity);
    bool clearDskCue();
    bool clearDsk();
    bool playDsk();
    bool pauseDsk();
    bool dskIsPlaying() const;
    bool pauseDskAtEnd();
    bool restartDskFromIn(bool play);
    DskStatus dskStatus();
    DskStatus dskCueStatus();
    qint64 dskPositionMs() const;
    qint64 dskDurationMs() const;
    QString dskErrorDetail() const;
    DskEvent takeDskEvent(QString &detail);

    bool play();
    bool pause();
    bool isPlaying() const;
    bool currentSourceHasAudio() const;
    bool powerPointSourceIsOnAir() const;
    bool powerPointCueIsActive() const;
    void warmUpPowerPointApplication();
    bool preparePowerPointConversion(bool *powerPointCueReleased,
                                     QString *errorDetail = nullptr);
    bool stop();
    bool stopAtInAndClearProgramFrame();
    // Re-prerolls a file source after Stop cleared its Program frame, while
    // keeping the media pipeline paused. Used by timed still images so Play
    // resumes their logical clock without sending the image decoder to EOS.
    ProgramFrameRestoreStatus restoreCurrentProgramFrame();
    qint64 positionMs() const;
    qint64 durationMs() const;
    bool seek(qint64 positionMs);
    bool setRate(double rate);
    PowerPointNavigationState powerPointNavigationState();
    PowerPointCommandResponse nextPowerPointSlide();
    PowerPointCommandResponse previousPowerPointSlide();
    void cancelDocumentNavigation();
    PdfNavigationState pdfNavigationState();
    PdfCommandResponse nextPdfPage();
    PdfCommandResponse previousPdfPage();
    AudioLevels audioLevels();
    void setFtbCoverage(qreal coverage);
    void setBackgroundColor(const QColor &color);
    void setMasterVolume(double volume);
    void setMasterDelayFrames(int frames);
    void setItemDelayFrames(int frames);
    void setCueDelayFrames(int frames);
    void setBgmItemDelayFrames(int frames);
    void setMasterEq(double lowDb, double midDb, double highDb);
    void setItemEq(double lowDb, double midDb, double highDb);
    void setCueEq(double lowDb, double midDb, double highDb);
    void setBgmItemEq(double lowDb, double midDb, double highDb);
    void setItemVolume(double volume);
    void setMasterMuted(bool muted);
    void requestProgramOutputDevice(const QString &id);
    QString appliedProgramOutputDevice() const;
    QString programOutputDeviceStatusKey() const;
    bool programAudioOutputReady() const;
    bool loadBgm(const QString &filePath, qint64 inPointMs = 0,
                 qint64 outPointMs = -1);
    bool playBgm(BgmPlayFailure *failure = nullptr);
    bool pauseBgm();
    bool stopBgm();
    bool clearBgm();
    bool seekBgm(qint64 positionMs);
    qint64 bgmPositionMs() const;
    qint64 bgmDurationMs() const;
    BgmStatus bgmStatus();
    QString bgmErrorDetail() const;
    BgmEvent takeBgmEvent(QString &detail);
    AudioLevel bgmSignalLevel();
    void setBgmItemVolume(double volume);
    void setBgmGain(double gain);
    void setBgmManualFadeGain(double gain);
    void setBgmTransportGain(double gain);
    void setBgmMeterActive(bool active);
    // Called by the ordinary bus timer, independently of audio level sampling.
    void processBusMessages();
    Event takeEvent(QString &detail, bool *programAudioError = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
