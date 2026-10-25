#pragma once

#include "video_frame_capture.h"

#include <cstdint>
#include <memory>
#include <string>

typedef struct _GstElement GstElement;
typedef struct _GstContext GstContext;

class ProgramGpuCompositor final
{
public:
    struct OutputGeometry {
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
    };

    ProgramGpuCompositor(std::uintptr_t fullscreenWindowHandle,
                         std::uintptr_t monitorWindowHandle,
                         OutputGeometry outputGeometry = {});
    ~ProgramGpuCompositor();

    ProgramGpuCompositor(const ProgramGpuCompositor &) = delete;
    ProgramGpuCompositor &operator=(const ProgramGpuCompositor &) = delete;

    bool start();
    bool configureDeckInput(int deckIndex, GstElement *playbin,
                            GstElement *appSink);
    bool tryConfigureDeckInput(int deckIndex, GstElement *playbin,
                               GstElement *appSink);
    void setDeckFrameUpdatesEnabled(int deckIndex, bool enabled);
    bool suspendDeckFrameUpdatesAndClear(int deckIndex);
    bool configureDskInput(int dskIndex, GstElement *pipeline,
                           GstElement *appSink);
    GstContext *createInputContext() const;
    void clearDeckFrame(int deckIndex);
    void clearDskFrame(int dskIndex);
    bool deckFrameReady(int deckIndex) const;
    bool dskFrameReady(int dskIndex) const;
    bool setDeckLayers(int upperDeck, float upperOpacity,
                       int lowerDeck, float lowerOpacity);
    bool setDskLayers(int upperDsk, float upperOpacity,
                      int lowerDsk, float lowerOpacity);
    bool takeDskRenderError();
    bool takeRuntimeError(std::string &detail);
    void setItemFadeProgress(float progress);
    void setFtbCoverage(float coverage);
    void setBackgroundColor(std::uint32_t rgb);
    bool prepareShutdown();
    bool stop();
    VideoFrameCapture captureStillFrame() const;
    bool isAvailable() const;
    std::uint64_t presentedFrameCount() const;
    std::uint64_t renderedFrameCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
