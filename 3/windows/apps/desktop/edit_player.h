#pragma once

#include "video_frame_capture.h"

#include <QString>
#include <QtGlobal>

#include <memory>

class QWidget;
class EditPflFeed;

class EditPlayer final
{
public:
    struct AudioLevel {
        double leftPeakDb = -60.0;
        double rightPeakDb = -60.0;
        bool clipped = false;
    };

    enum class Event { None, EndOfStream, ScanBoundary, Error, MediaOffline };

    explicit EditPlayer(QWidget *videoWindow);
    ~EditPlayer();

    EditPlayer(const EditPlayer &) = delete;
    EditPlayer &operator=(const EditPlayer &) = delete;

    VideoFrameCapture captureStillFrame() const;
    bool isAvailable() const;
    bool hasMedia() const;
    bool mediaOffline() const;
    bool isBusy() const;
    bool isPlaying() const;
    void requestShutdown();
    bool shutdownComplete() const;
    void shutdown();
    void refreshWindowHandle();

    bool load(const QString &filePath);
    bool unload();
    bool play();
    bool pause();
    bool stop();
    qint64 positionMs() const;
    qint64 durationMs() const;
    qint64 frameDurationMs() const;
    qint64 frameStepTargetMs(qint64 positionMs, int frameCount) const;
    bool seek(qint64 positionMs);
    bool seekAccurate(qint64 positionMs);
    bool startForwardScan(double speed);
    bool stopForwardScan(bool resumePlayback, bool boundaryReached,
                         qint64 &positionMs);
    bool setNominalVolume(double linearGain);
    bool setNominalDelayFrames(int frames);
    bool setNominalEq(double lowDb, double midDb, double highDb);
    std::shared_ptr<EditPflFeed> pflFeed() const;
    AudioLevel inputAudioLevel() const;
    AudioLevel audioLevel() const;
    void processBusMessages();
    Event takeEvent(QString &detail);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
