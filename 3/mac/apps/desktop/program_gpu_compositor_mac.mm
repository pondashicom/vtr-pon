#include "program_gpu_compositor.h"
#include "operator_monitor_output.h"
#include "network_video.h"
#include "virtual_camera/output.h"
#include "mac_gpu_device.h"
#include "mac_gpu_activity.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CGDirectDisplayMetal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include <QDebug>
#include <QTimer>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstring>
#include <vector>

namespace {
using Sample = std::shared_ptr<GstSample>;
struct Input {
    std::mutex mutex;
    Sample latest;
    bool enabled = true;
};
GstFlowReturn receive(GstAppSink *sink, gpointer data, bool preroll) {
    Sample sample(preroll ? gst_app_sink_pull_preroll(sink) : gst_app_sink_pull_sample(sink), gst_sample_unref);
    auto input = *static_cast<std::shared_ptr<Input> *>(data);
    std::lock_guard lock(input->mutex);
    if (sample && input->enabled) input->latest = std::move(sample);
    return GST_FLOW_OK;
}
GstFlowReturn sampleReady(GstAppSink *sink, gpointer data) { return receive(sink, data, false); }
GstFlowReturn prerollReady(GstAppSink *sink, gpointer data) { return receive(sink, data, true); }

// One immutable completed output texture is shared by both presenters. A slow or
// occluded drawable never holds the producer or the other presenter hostage.
struct OutputSurface { id<MTLTexture> texture, alpha; };
struct Output {
    id<MTLTexture> texture;
    uint64_t sequence = 0;
    quint64 generation = 0;
    qint64 timecode = 0;
    std::shared_ptr<OutputSurface> owner; // Pins a pool slot through presentation/capture.
};
struct Presenter {
    NSView *view;
    CAMetalLayer *layer;
    std::atomic_bool visible{false};
    std::thread worker;
    std::atomic_uint64_t presented{0};
};
struct LayerParameters {
    float scale[2];
    float gain;
    float hasFrame;
    float yuv[3][4]{};
};
struct Parameters {
    float background[4];
    LayerParameters layers[4]; // Lower/upper deck, then lower/upper DSK.
    float fade[4];
};
struct InputPlane {
    id<MTLBuffer> buffer;
    id<MTLTexture> texture;
    size_t rowBytes = 0;
};
struct TextureInput {
    Sample sample;
    std::array<InputPlane, 2> planes;
    LayerParameters parameters{};
    bool valid = false;
    float aspect = 16.0F / 9;
};
const char *shaderSource = R"(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position [[position]]; float2 uv; };
vertex Vertex fullscreenVertex(uint n [[vertex_id]]) {
    float2 p = float2((n << 1) & 2, n & 2);
    return {float4(p * float2(2, -2) + float2(-1, 1), 0, 1), p};
}
struct Layer { float2 scale; float gain; float hasFrame; float4 yuv0; float4 yuv1; float4 yuv2; };
struct Parameters { float4 background; Layer lowerDeck; Layer upperDeck; Layer lowerDsk; Layer upperDsk; float4 fade; };
float4 sampleLayer(float2 position, texture2d<float> source, texture2d<float> chroma, Layer p) {
    constexpr sampler linearSampler(coord::normalized, address::clamp_to_edge, filter::linear);
    float2 uv = (position - 0.5) / p.scale + 0.5;
    float4 color = float4(0);
    if (p.hasFrame > 0 && all(uv >= 0) && all(uv <= 1)) {
        color = source.sample(linearSampler, uv);
        if (p.hasFrame > 1.5) {
            float4 yuv = float4(color.r, chroma.sample(linearSampler, uv).rg, 1);
            color = float4(saturate(float3(dot(p.yuv0, yuv), dot(p.yuv1, yuv), dot(p.yuv2, yuv))), 1);
        }
    }
    // BGRA inputs are straight alpha; all composition below is premultiplied.
    return float4(color.rgb * color.a, color.a) * p.gain;
}
float4 over(float4 upper, float4 lower) { return upper + lower * (1 - upper.a); }
fragment float4 composite(Vertex v [[stage_in]], texture2d<float> lowerDeck [[texture(0)]],
                          texture2d<float> upperDeck [[texture(1)]], texture2d<float> lowerDsk [[texture(2)]],
                          texture2d<float> upperDsk [[texture(3)]],
                          texture2d<float> lowerDeckUV [[texture(4)]], texture2d<float> upperDeckUV [[texture(5)]],
                          texture2d<float> lowerDskUV [[texture(6)]], texture2d<float> upperDskUV [[texture(7)]],
                          constant Parameters &p [[buffer(0)]]) {
    // Preserve the Windows layer order and alpha-over contract. Fade the
    // already mixed deck group once, before adding DSK and applying FTB.
    float4 color = over(sampleLayer(v.uv, upperDeck, upperDeckUV, p.upperDeck),
                        sampleLayer(v.uv, lowerDeck, lowerDeckUV, p.lowerDeck)) * p.fade.y;
    color = over(sampleLayer(v.uv, lowerDsk, lowerDskUV, p.lowerDsk), color);
    color = over(sampleLayer(v.uv, upperDsk, upperDskUV, p.upperDsk), color) * p.fade.x;
    return float4(color.rgb + p.background.rgb * (1 - color.a), 1);
}
struct CompositeAndAlpha { float4 composite [[color(0)]]; float4 alpha [[color(1)]]; };
fragment CompositeAndAlpha compositeAlpha(Vertex v [[stage_in]], texture2d<float> lowerDeck [[texture(0)]],
                          texture2d<float> upperDeck [[texture(1)]], texture2d<float> lowerDsk [[texture(2)]],
                          texture2d<float> upperDsk [[texture(3)]],
                          texture2d<float> lowerDeckUV [[texture(4)]], texture2d<float> upperDeckUV [[texture(5)]],
                          texture2d<float> lowerDskUV [[texture(6)]], texture2d<float> upperDskUV [[texture(7)]],
                          constant Parameters &p [[buffer(0)]]) {
    // Preserve the Windows layer order and alpha-over contract. Fade the
    // already mixed deck group once, before adding DSK and applying FTB.
    float4 color = over(sampleLayer(v.uv, upperDeck, upperDeckUV, p.upperDeck),
                        sampleLayer(v.uv, lowerDeck, lowerDeckUV, p.lowerDeck)) * p.fade.y;
    color = over(sampleLayer(v.uv, lowerDsk, lowerDskUV, p.lowerDsk), color);
    color = over(sampleLayer(v.uv, upperDsk, upperDskUV, p.upperDsk), color) * p.fade.x;
    return {float4(color.rgb + p.background.rgb * (1 - color.a), 1), color};
}
fragment float4 present(Vertex v [[stage_in]], texture2d<float> source [[texture(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    return source.sample(s, v.uv);
}
)";
}

class ProgramGpuCompositor::Impl {
public:
    Impl(uintptr_t output, uintptr_t monitor, OutputGeometry size)
        : handles{output, monitor}, geometry(size) {}
    ~Impl() {
        prepareShutdown();
        stop();
        // Owned by the player, which is destroyed on the GUI thread.
        for (auto &p : presenters) [p.view removeFromSuperview];
    }
    bool start() {
        if (!((geometry.width == 1920 && geometry.height == 1080) ||
              (geometry.width == 3840 && geometry.height == 2160))) return fail("Unsupported Metal output dimensions");
        @autoreleasepool {
            auto host = (__bridge NSView *)(reinterpret_cast<void *>(handles[0]));
            NSNumber *screen = host.window.screen.deviceDescription[@"NSScreenNumber"];
            id<MTLDevice> displayDevice = screen ? CGDirectDisplayCopyCurrentMetalDevice(screen.unsignedIntValue) : nil;
            if (!displayDevice) displayDevice = MTLCreateSystemDefaultDevice();
            const auto preferences = vtrpon::startupGpuPreferences();
            const auto selection = vtrpon::selectGpuAdapter(vtrpon::availableGpuAdapters(),
                preferences.adapterId, displayDevice ? qint64(displayDevice.registryID) : 0);
            for (id<MTLDevice> candidate in MTLCopyAllDevices())
                if (qint64(candidate.registryID) == selection.adapter.luid) device = candidate;
            if (!device) return fail("Selected Metal device unavailable");
            vtrpon::configureVideoDecoders(selection.adapter.luid, preferences.softwareDecoding);
            vtrpon::publishGpuStatus(selection, preferences.softwareDecoding);
            queue = [device newCommandQueue];
            NSError *error = nil;
            auto library = [device newLibraryWithSource:[NSString stringWithUTF8String:shaderSource] options:nil error:&error];
            if (!library) return fail(error.localizedDescription.UTF8String);
            auto makePipeline = [&](NSString *name, bool alpha = false) {
                auto desc = [MTLRenderPipelineDescriptor new];
                desc.vertexFunction = [library newFunctionWithName:@"fullscreenVertex"];
                desc.fragmentFunction = [library newFunctionWithName:name];
                desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                if (alpha) desc.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
                return [device newRenderPipelineStateWithDescriptor:desc error:&error];
            };
            compositePipeline = makePipeline(@"composite");
            alphaPipeline = makePipeline(@"compositeAlpha", true);
            presentPipeline = makePipeline(@"present");
            if (!queue || !compositePipeline || !alphaPipeline || !presentPipeline) return fail("Metal pipeline initialization failed");
            for (size_t i = 0; i < presenters.size(); ++i) {
                auto &p = presenters[i];
                auto host = (__bridge NSView *)(reinterpret_cast<void *>(handles[i]));
                p.view = [[NSView alloc] initWithFrame:host.bounds];
                p.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
                p.layer = [CAMetalLayer layer];
                p.layer.device = device;
                p.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                p.layer.framebufferOnly = YES;
                p.layer.allowsNextDrawableTimeout = YES;
                p.view.wantsLayer = YES;
                p.view.layer = p.layer;
                [host addSubview:p.view];
            }
            // The producer never waits for a slow presenter or capture reader.
            // Pool slots remain immutable until every consumer releases its owner.
            for (int i = 0; i < 6; ++i) {
                auto surface = std::make_shared<OutputSurface>();
                surface->texture = makeTexture(geometry.width, geometry.height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
                if (!surface->texture) return fail("Cannot allocate bounded Metal output pool");
                outputPool.push_back(std::move(surface));
            }
            running = true;
            updateSurfaces();
            QObject::connect(&surfaceTimer, &QTimer::timeout, &surfaceTimer, [this] { updateSurfaces(); });
            surfaceTimer.start(50);
            try {
                renderWorker = std::thread([this] { renderLoop(); });
                for (size_t i = 0; i < presenters.size(); ++i)
                    presenters[i].worker = std::thread([this, i] { presentLoop(presenters[i]); });
            } catch (const std::exception &error) {
                prepareShutdown();
                stop();
                return fail(error.what());
            }
            try { operatorMonitorWorker = std::thread([this] { operatorMonitorLoop(); }); }
            catch (const std::exception &error) { qWarning() << "Mac return monitor worker unavailable:" << error.what(); }
            try { cameraWorker = std::thread([this] { cameraLoop(); }); }
            catch (const std::exception &error) { qWarning() << "Mac virtual camera reader unavailable:" << error.what(); }
            try { networkWorker = std::thread([this] { networkLoop(); }); }
            catch (const std::exception &error) { vtrpon::reportNetworkVideoOutputError(QString::fromLocal8Bit(error.what())); }
            qInfo() << "Mac Metal compositor started:" << geometry.width << "x" << geometry.height
                    << "shared output, independent presenters, NV12/BGRA shared buffers, 6 output slots";
            return true;
        }
    }
    void updateSurfaces() {
        for (auto &p : presenters) {
            NSWindow *window = p.view.window;
            const CGFloat scale = window ? window.backingScaleFactor : 1;
            const NSSize size = p.view.bounds.size;
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            p.layer.contentsScale = scale;
            p.layer.drawableSize = CGSizeMake(std::max(1.0, size.width * scale), std::max(1.0, size.height * scale));
            [CATransaction commit];
            p.visible = window && window.visible && !window.miniaturized && !p.view.hiddenOrHasHiddenAncestor
                && size.width > 0 && size.height > 0;
        }
    }
    bool configure(int index, GstElement *sink) {
        // Match isAvailable(): shutdown/failure must never attach fresh callbacks.
        // Callers retain the source identity when detaching is refused.
        if (!running || failed || index < 0 || index >= 4 || !sink || !GST_IS_APP_SINK(sink)) return false;
        detach(index);
        auto input = std::make_shared<Input>();
        { std::lock_guard lock(stateMutex); inputs[index] = input; }
        sinks[index] = GST_ELEMENT(gst_object_ref(sink));
        GstAppSinkCallbacks callbacks{};
        callbacks.new_preroll = prerollReady;
        callbacks.new_sample = sampleReady;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks,
            new std::shared_ptr<Input>(input), [](gpointer p) { delete static_cast<std::shared_ptr<Input> *>(p); });
        return true;
    }
    void detach(int index) {
        if (!sinks[index]) return;
        GstAppSinkCallbacks empty{};
        gst_app_sink_set_callbacks(GST_APP_SINK(sinks[index]), &empty, nullptr, nullptr);
        gst_object_unref(sinks[index]);
        sinks[index] = nullptr;
    }
    std::shared_ptr<Input> input(int index) const {
        std::lock_guard lock(stateMutex);
        return index >= 0 && index < 4 ? inputs[index] : nullptr;
    }
    bool prepareShutdown() { // GUI thread, before producer shutdown worker starts.
        surfaceTimer.stop();
        running = false;
        for (int i = 0; i < 4; ++i) detach(i);
        return true;
    }
    bool stop() { // Can be called on the aggregate shutdown worker.
        running = false;
        if (renderWorker.joinable()) renderWorker.join();
        if (operatorMonitorWorker.joinable()) operatorMonitorWorker.join();
        if (networkWorker.joinable()) networkWorker.join();
        if (cameraWorker.joinable()) cameraWorker.join();
        for (auto &p : presenters) if (p.worker.joinable()) p.worker.join();
        return true;
    }
    bool fail(const char *message) {
        std::lock_guard lock(errorMutex);
        errorText = message ? message : "Unknown Metal error";
        failed = true;
        qWarning() << "Mac Metal compositor:" << errorText.c_str();
        return false;
    }
    id<MTLTexture> makeTexture(unsigned width, unsigned height, MTLTextureUsage usage) {
        auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = usage;
        return [device newTextureWithDescriptor:d];
    }
    // Each input uses reusable shared Metal buffers. NV12 bypasses CPU BGRA
    // conversion and uploads 1.5 bytes/pixel; SDR conversion occurs in the shader.
    bool upload(TextureInput &cached, const Sample &sample, bool dsk) {
        if (sample == cached.sample) return true;
        cached.sample = sample;
        cached.valid = false;
        if (!sample) return true;
        GstVideoInfo info{};
        GstVideoFrame frame{};
        const char *error = nullptr;
        const bool haveInfo = gst_video_info_from_caps(&info, gst_sample_get_caps(sample.get()));
        const auto format = GST_VIDEO_INFO_FORMAT(&info);
        const bool nv12 = format == GST_VIDEO_FORMAT_NV12;
        if (!haveInfo || (format != GST_VIDEO_FORMAT_BGRA && !nv12) ||
            !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample.get()), GST_MAP_READ)) {
            error = "Cannot map NV12/BGRA input frame";
        } else {
            if (info.width <= 0 || info.height <= 0 || info.width > 16384 || info.height > 16384) {
                error = "Invalid video dimensions";
            } else for (int plane = 0; plane < (nv12 ? 2 : 1) && !error; ++plane) {
                const size_t width = plane ? (info.width + 1) / 2 : info.width;
                const size_t height = plane ? (info.height + 1) / 2 : info.height;
                const size_t rowSize = width * (nv12 ? (plane ? 2 : 1) : 4);
                const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
                if (stride < 0 || size_t(stride) < rowSize) { error = "Invalid video plane stride"; break; }
                const auto pixelFormat = nv12 ? (plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm) : MTLPixelFormatBGRA8Unorm;
                auto &p = cached.planes[plane];
                if (!p.texture || p.texture.width != width || p.texture.height != height || p.texture.pixelFormat != pixelFormat) {
                    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:width height:height mipmapped:NO];
                    descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageShaderRead;
                    const size_t align = [device minimumLinearTextureAlignmentForPixelFormat:pixelFormat];
                    if (!align) { error = "Shared Metal texture format is unsupported"; break; }
                    p.rowBytes = (rowSize + align - 1) / align * align;
                    p.buffer = [device newBufferWithLength:p.rowBytes * height options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
                    p.texture = [p.buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:p.rowBytes];
                    if (!p.texture) { error = "Cannot allocate shared Metal input buffer/texture"; break; }
                    ++inputAllocations;
                    qInfo() << "METAL_INPUT" << (nv12 ? "NV12" : "BGRA") << info.width << info.height << "plane" << plane << "shared-buffer";
                }
                const auto *source = static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, plane));
                auto *destination = static_cast<uint8_t *>(p.buffer.contents);
                for (size_t y = 0; y < height; ++y) std::memcpy(destination + y*p.rowBytes, source + y*stride, rowSize);
                uploadedBytes += rowSize * height;
            }
            cached.parameters = {};
            cached.parameters.hasFrame = nv12 ? 2 : 1;
            if (nv12) {
                double kr = .2126, kb = .0722;
                if (!gst_video_color_matrix_get_Kr_Kb(info.colorimetry.matrix, &kr, &kb)) {
                    if (info.height < 720) { kr = .299; kb = .114; }
                }
                const bool full = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
                const double yScale = full ? 1 : 255.0/219, uvScale = full ? 1 : 255.0/224;
                const double yOffset = full ? 0 : 16.0/255, uvOffset = 128.0/255;
                const double kg = 1 - kr - kb;
                const double u[3] = {0, -2*kb*(1-kb)/kg*uvScale, 2*(1-kb)*uvScale};
                const double v[3] = {2*(1-kr)*uvScale, -2*kr*(1-kr)/kg*uvScale, 0};
                for (int c = 0; c < 3; ++c) {
                    auto &row = cached.parameters.yuv[c];
                    row[0] = yScale; row[1] = u[c]; row[2] = v[c];
                    row[3] = -yScale*yOffset - (u[c]+v[c])*uvOffset;
                }
            }
            cached.aspect = float(info.width) * std::max(1, info.par_n) / (float(info.height) * std::max(1, info.par_d));
            cached.valid = !error;
            if (!error) { if (nv12) ++nv12Uploads; else ++bgraUploads; }
            gst_video_frame_unmap(&frame);
        }
        if (!error) return true;
        if (dsk) { dskFailed = true; qWarning() << "Mac Metal DSK:" << error; return true; }
        return fail(error);
    }
    void renderLoop() {
        std::array<TextureInput, 4> textures;
        @autoreleasepool {
            auto transparent = makeTexture(1, 1, MTLTextureUsageShaderRead);
            if (!transparent) { fail("Cannot allocate transparent texture"); return; }
            uint32_t zero = 0;
            [transparent replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
            while (running) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
                @autoreleasepool {
                    std::array<int, 4> indices;
                    std::array<float, 4> gains;
                    float groupFade, deckFade; uint32_t bg;
                    {
                        std::lock_guard lock(stateMutex);
                        indices = {lowerDeck, selected, lowerDsk < 0 ? -1 : lowerDsk + 2, upperDsk < 0 ? -1 : upperDsk + 2};
                        gains = {lowerDeckOpacity, opacity, lowerDskOpacity, upperDskOpacity};
                        groupFade = 1 - ftb; deckFade = itemFade; bg = background;
                    }
                    for (size_t layer = 0; layer < indices.size(); ++layer)
                        if (gains[layer] <= 0) indices[layer] = -1;
                    // Upload only visible inputs; a standby DSK must not affect
                    // the current overlay. Release inactive texture caches so
                    // retired decoder samples are not retained indefinitely.
                    bool valid = true;
                    for (int i = 0; i < 4; ++i) {
                        Sample sample;
                        if (std::find(indices.begin(), indices.end(), i) != indices.end()) {
                            if (auto slot = input(i)) { std::lock_guard lock(slot->mutex); sample = slot->latest; }
                        }
                        valid = upload(textures[i], sample, i >= 2) && valid;
                    }
                    if (!valid) break;
                    std::shared_ptr<OutputSurface> surface;
                    for (const auto &candidate : outputPool) if (candidate.use_count() == 1) { surface = candidate; break; }
                    if (!surface) { ++outputDrops; std::this_thread::sleep_until(deadline); continue; }
                    const auto generation = vtrpon::networkVideoOutputGeneration();
                    const bool wantsAlpha = generation && (vtrpon::networkVideoOutputMask() & 14u);
                    if (wantsAlpha && !surface->alpha)
                        surface->alpha = makeTexture(geometry.width, geometry.height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
                    if (!wantsAlpha) surface->alpha = nil;
                    const bool alpha = wantsAlpha && surface->alpha;
                    if (wantsAlpha && !alpha) vtrpon::reportNetworkVideoOutputError(QStringLiteral("NDI output: Metal alpha allocation failed."));
                    const auto timecode = vtrpon::networkVideoTimecode();
                    auto target = surface->texture;
                    Parameters p{};
                    p.background[0] = float((bg >> 16) & 255) / 255;
                    p.background[1] = float((bg >> 8) & 255) / 255;
                    p.background[2] = float(bg & 255) / 255;
                    p.background[3] = 1;
                    p.fade[0] = groupFade;
                    p.fade[1] = deckFade;
                    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = target;
                    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    if (alpha) {
                        pass.colorAttachments[1].texture = surface->alpha;
                        pass.colorAttachments[1].loadAction = MTLLoadActionDontCare;
                        pass.colorAttachments[1].storeAction = MTLStoreActionStore;
                    }
                    auto command = [queue commandBuffer];
                    auto encoder = [command renderCommandEncoderWithDescriptor:pass];
                    [encoder setRenderPipelineState:alpha ? alphaPipeline : compositePipeline];
                    for (size_t layer = 0; layer < indices.size(); ++layer) {
                        const int index = indices[layer];
                        auto texture = index >= 0 && textures[index].valid ? textures[index].planes[0].texture : nil;
                        const float aspect = index >= 0 ? textures[index].aspect : 16.0F / 9;
                        if (index >= 0) p.layers[layer] = textures[index].parameters;
                        p.layers[layer].scale[0] = std::min(1.0F, aspect / (16.0F/9));
                        p.layers[layer].scale[1] = std::min(1.0F, (16.0F/9) / aspect);
                        p.layers[layer].gain = gains[layer];
                        if (!texture) p.layers[layer].hasFrame = 0;
                        [encoder setFragmentTexture:texture ? texture : transparent atIndex:layer];
                        auto chroma = index >= 0 && textures[index].valid ? textures[index].planes[1].texture : nil;
                        [encoder setFragmentTexture:chroma ? chroma : transparent atIndex:layer + 4];
                    }
                    [encoder setFragmentBytes:&p length:sizeof(p) atIndex:0];
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [encoder endEncoding];
                    [command commit];
                    [command waitUntilCompleted];
                    if (command.status == MTLCommandBufferStatusError) { fail(command.error.localizedDescription.UTF8String); break; }
                    vtrpon::recordMetalActivity(command.GPUStartTime, command.GPUEndTime);
                    { std::lock_guard lock(outputMutex); output = {target, ++rendered, generation, timecode, surface}; }
                }
                std::this_thread::sleep_until(deadline);
            }
        }
    }
    void networkLoop() {
        uint64_t last = 0;
        while (running) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
            const auto generation = vtrpon::networkVideoOutputGeneration();
            if (generation) @autoreleasepool {
                Output frame;
                { std::unique_lock lock(outputMutex, std::try_to_lock); if (lock) frame = output; }
                if (frame.texture && frame.sequence != last && frame.generation == generation) {
                    const auto mask = vtrpon::networkVideoOutputMask();
                    std::array<QByteArray, 2> pixels;
                    id<MTLTexture> sources[2] = {frame.texture, frame.owner->alpha};
                    for (unsigned i = 0; i < 2; ++i) if (mask & (i ? 14u : 1u)) {
                        if (!sources[i]) continue;
                        pixels[i].resize(qsizetype(geometry.width) * geometry.height * 4);
                        [sources[i] getBytes:pixels[i].data() bytesPerRow:geometry.width * 4
                            fromRegion:MTLRegionMake2D(0, 0, geometry.width, geometry.height) mipmapLevel:0];
                    }
                    // Sender workers own encoding/network waits. This reader pins
                    // one immutable pool slot; rendering drops instead of waiting.
                    vtrpon::publishNetworkVideo(std::move(pixels[0]), std::move(pixels[1]),
                        int(geometry.width), int(geometry.height), generation, frame.timecode);
                    last = frame.sequence;
                }
            }
            std::this_thread::sleep_until(deadline);
        }
    }
    void cameraLoop() {
        auto next=std::chrono::steady_clock::now();
        constexpr auto interval=std::chrono::nanoseconds(1000000000/30);
        uint64_t last=0;
        QImage image(1920,1080,QImage::Format_ARGB32);
        auto cameraQueue=[device newCommandQueue];
        auto scaled=geometry.width!=1920 || geometry.height!=1080 ? makeTexture(1920,1080,MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) : nil;
        if(image.isNull() || !cameraQueue || ((geometry.width!=1920 || geometry.height!=1080) && !scaled)) return;
        while(running) {
            next+=interval;
            if(vtrpon::virtualCameraFrameDemand()) @autoreleasepool {
                Output frame;
                { std::unique_lock lock(outputMutex,std::try_to_lock); if(lock) frame=output; }
                if(frame.texture && frame.sequence!=last) {
                    auto source=frame.texture;
                    if(scaled) {
                        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
                        pass.colorAttachments[0].texture=scaled;
                        pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;
                        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
                        auto command=[cameraQueue commandBuffer];
                        auto encoder=[command renderCommandEncoderWithDescriptor:pass];
                        [encoder setRenderPipelineState:presentPipeline];
                        [encoder setFragmentTexture:source atIndex:0];
                        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
                        if(command.status==MTLCommandBufferStatusError) continue;
                        vtrpon::recordMetalActivity(command.GPUStartTime,command.GPUEndTime);
                        source=scaled;
                    }
                    [source getBytes:image.bits() bytesPerRow:image.bytesPerLine() fromRegion:MTLRegionMake2D(0,0,1920,1080) mipmapLevel:0];
                    // Only this output worker copies pixels; rendering never waits for IPC.
                    vtrpon::publishVirtualCameraFrame(image,frame.sequence);
                    last=frame.sequence;
                }
            }
            if(next<std::chrono::steady_clock::now()) next=std::chrono::steady_clock::now()+interval;
            std::this_thread::sleep_until(next);
        }
    }
    void operatorMonitorLoop() {
        auto next = std::chrono::steady_clock::now();
        constexpr auto interval = std::chrono::nanoseconds(1000000000 / 24);
        uint64_t last = 0;
        const auto width = std::min(geometry.width, 1920u), height = std::min(geometry.height, 1080u);
        QImage image(int(width), int(height), QImage::Format_ARGB32);
        auto readbackQueue = [device newCommandQueue];
        auto scaled = geometry.width > width ? makeTexture(width, height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead) : nil;
        if (!readbackQueue || (geometry.width > width && !scaled)) {
            qWarning() << "Mac return monitor readback resources unavailable; ON AIR is unaffected";
            return;
        }
        while (running) {
            next += interval;
            if (vtrpon::operatorMonitorEnabled() && !image.isNull()) {
                @autoreleasepool {
                    Output frame;
                    { std::unique_lock lock(outputMutex, std::try_to_lock); if (lock) frame = output; }
                    if (frame.texture && frame.sequence != last) {
                        auto source = frame.texture;
                        if (scaled) {
                            auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                            pass.colorAttachments[0].texture = scaled;
                            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                            auto command = [readbackQueue commandBuffer];
                            auto encoder = [command renderCommandEncoderWithDescriptor:pass];
                            [encoder setRenderPipelineState:presentPipeline];
                            [encoder setFragmentTexture:source atIndex:0];
                            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                            [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
                            if (command.status == MTLCommandBufferStatusError) {
                                std::this_thread::sleep_until(next);
                                continue;
                            }
                            vtrpon::recordMetalActivity(command.GPUStartTime, command.GPUEndTime);
                            source = scaled;
                        }
                        if (source.width == width && source.height == height) {
                            [source getBytes:image.bits() bytesPerRow:image.bytesPerLine()
                                fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
                            vtrpon::publishOperatorMonitorFrame(image, frame.sequence);
                        }
                        last = frame.sequence;
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now + interval;
            std::this_thread::sleep_until(next);
        }
    }
    void presentLoop(Presenter &p) {
        @autoreleasepool {
            auto presentationQueue = [device newCommandQueue];
            if (!presentationQueue) { fail("Cannot allocate Metal presentation queue"); return; }
            uint64_t last = 0;
            while (running) {
                @autoreleasepool {
                    Output frame;
                    { std::lock_guard lock(outputMutex); frame = output; }
                    if (!p.visible || !frame.texture || frame.sequence == last) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(8)); continue;
                    }
                    auto drawable = [p.layer nextDrawable];
                    if (!drawable || !running) continue;
                    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = drawable.texture;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    auto command = [presentationQueue commandBuffer];
                    auto encoder = [command renderCommandEncoderWithDescriptor:pass];
                    const double w = drawable.texture.width, h = drawable.texture.height;
                    const double scale = std::min(w / geometry.width, h / geometry.height);
                    const double vw = geometry.width * scale, vh = geometry.height * scale;
                    [encoder setViewport:MTLViewport{(w-vw)/2, (h-vh)/2, vw, vh, 0, 1}];
                    [encoder setRenderPipelineState:presentPipeline];
                    [encoder setFragmentTexture:frame.texture atIndex:0];
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [encoder endEncoding];
                    [command presentDrawable:drawable];
                    [command commit];
                    [command waitUntilCompleted];
                    if (command.status == MTLCommandBufferStatusError) { fail(command.error.localizedDescription.UTF8String); break; }
                    vtrpon::recordMetalActivity(command.GPUStartTime, command.GPUEndTime);
                    last = frame.sequence;
                    ++p.presented;
                }
            }
        }
    }
    std::array<uintptr_t, 2> handles;
    OutputGeometry geometry;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> compositePipeline, alphaPipeline, presentPipeline;
    std::array<Presenter, 2> presenters;
    std::array<GstElement *, 4> sinks{};
    std::array<std::shared_ptr<Input>, 4> inputs;
    mutable std::mutex stateMutex, outputMutex, errorMutex;
    Output output;
    std::vector<std::shared_ptr<OutputSurface>> outputPool;
    std::atomic_uint64_t uploadedBytes{0}, inputAllocations{0}, nv12Uploads{0}, bgraUploads{0}, outputDrops{0};
    int selected = -1, lowerDeck = -1;
    float lowerDeckOpacity = 0;
    int upperDsk = -1, lowerDsk = -1;
    float upperDskOpacity = 0, lowerDskOpacity = 0;
    std::atomic_bool dskFailed{false};
    float opacity = 1, itemFade = 1, ftb = 0;
    uint32_t background = 0;
    std::atomic_bool running{false}, failed{false};
    std::atomic_uint64_t rendered{0};
    std::string errorText;
    std::thread renderWorker, operatorMonitorWorker, networkWorker, cameraWorker;
    QTimer surfaceTimer;
};

ProgramGpuCompositor::ProgramGpuCompositor(uintptr_t a, uintptr_t b, OutputGeometry g) : impl_(std::make_unique<Impl>(a,b,g)) {}
ProgramGpuCompositor::~ProgramGpuCompositor() = default;
bool ProgramGpuCompositor::start() { return impl_->start(); }
bool ProgramGpuCompositor::configureDeckInput(int i, GstElement *, GstElement *sink) { return i >= 0 && i < 2 && impl_->configure(i,sink); }
bool ProgramGpuCompositor::tryConfigureDeckInput(int i, GstElement *p, GstElement *s) { return configureDeckInput(i,p,s); }
void ProgramGpuCompositor::clearDeckFrame(int i) { if (auto slot = (i >= 0 && i < 2 ? impl_->input(i) : nullptr)) { std::lock_guard lock(slot->mutex); slot->latest.reset(); } }
void ProgramGpuCompositor::setDeckFrameUpdatesEnabled(int i, bool enabled) { if (auto slot = (i >= 0 && i < 2 ? impl_->input(i) : nullptr)) { std::lock_guard lock(slot->mutex); slot->enabled = enabled; } }
bool ProgramGpuCompositor::suspendDeckFrameUpdatesAndClear(int i) {
    auto slot = (i >= 0 && i < 2 ? impl_->input(i) : nullptr); if (!slot) return false;
    std::lock_guard lock(slot->mutex); slot->enabled = false; slot->latest.reset(); return true;
}
bool ProgramGpuCompositor::deckFrameReady(int i) const { auto slot = (i >= 0 && i < 2 ? impl_->input(i) : nullptr); if (!slot) return false; std::lock_guard lock(slot->mutex); return bool(slot->latest); }
bool ProgramGpuCompositor::setDeckLayers(int upper, float alpha, int lower, float lowerAlpha) {
    if (upper < -1 || upper > 1 || lower < -1 || lower > 1 || !isAvailable()) return false;
    std::lock_guard lock(impl_->stateMutex);
    impl_->selected = upper; impl_->lowerDeck = lower;
    impl_->opacity = std::clamp(alpha, 0.0F, 1.0F);
    impl_->lowerDeckOpacity = std::clamp(lowerAlpha, 0.0F, 1.0F);
    return true;
}
void ProgramGpuCompositor::setItemFadeProgress(float v) { std::lock_guard lock(impl_->stateMutex); impl_->itemFade = std::clamp(v,0.0F,1.0F); }
void ProgramGpuCompositor::setFtbCoverage(float v) { std::lock_guard lock(impl_->stateMutex); impl_->ftb = std::clamp(v,0.0F,1.0F); }
void ProgramGpuCompositor::setBackgroundColor(uint32_t v) { std::lock_guard lock(impl_->stateMutex); impl_->background = v & 0xffffff; }
bool ProgramGpuCompositor::prepareShutdown() { return impl_->prepareShutdown(); }
bool ProgramGpuCompositor::stop() { return impl_->stop(); }
bool ProgramGpuCompositor::isAvailable() const { return impl_->running && !impl_->failed; }
bool ProgramGpuCompositor::takeRuntimeError(std::string &detail) { std::lock_guard lock(impl_->errorMutex); detail.swap(impl_->errorText); return !detail.empty(); }
uint64_t ProgramGpuCompositor::presentedFrameCount() const { return impl_->presenters[0].presented; }
uint64_t ProgramGpuCompositor::renderedFrameCount() const { return impl_->rendered; }
VideoFrameCapture ProgramGpuCompositor::captureStillFrame() const {
    Output frame; { std::lock_guard lock(impl_->outputMutex); frame = impl_->output; }
    if (!frame.texture) return {};
    return [frame] {
        auto texture = frame.texture;
        QImage image(int(texture.width), int(texture.height), QImage::Format_ARGB32);
        [texture getBytes:image.bits() bytesPerRow:image.bytesPerLine() fromRegion:MTLRegionMake2D(0,0,texture.width,texture.height) mipmapLevel:0];
        return image;
    };
}
bool ProgramGpuCompositor::configureDskInput(int i, GstElement *, GstElement *sink) {
    return i >= 0 && i < 2 && impl_->configure(i + 2, sink);
}
bool ProgramGpuCompositor::setDskLayers(int upper, float upperAlpha, int lower, float lowerAlpha) {
    if (upper < -1 || upper > 1 || lower < -1 || lower > 1 || !isAvailable()) return false;
    std::lock_guard lock(impl_->stateMutex);
    impl_->upperDsk = upper; impl_->lowerDsk = lower;
    impl_->upperDskOpacity = std::clamp(upperAlpha, 0.0F, 1.0F);
    impl_->lowerDskOpacity = std::clamp(lowerAlpha, 0.0F, 1.0F);
    return true;
}
void ProgramGpuCompositor::clearDskFrame(int i) {
    if (i < 0 || i >= 2) return;
    if (auto slot = impl_->input(i + 2)) { std::lock_guard lock(slot->mutex); slot->latest.reset(); }
}
bool ProgramGpuCompositor::dskFrameReady(int i) const {
    if (i < 0 || i >= 2) return false;
    auto slot = impl_->input(i + 2); if (!slot) return false;
    std::lock_guard lock(slot->mutex); return bool(slot->latest);
}
bool ProgramGpuCompositor::takeDskRenderError() { return impl_->dskFailed.exchange(false); }
GstContext *ProgramGpuCompositor::createInputContext() const { return nullptr; }

ProgramGpuCompositor::MacStatistics ProgramGpuCompositor::macStatistics() const {
    return {impl_->uploadedBytes.load(), impl_->inputAllocations.load(), impl_->nv12Uploads.load(),
            impl_->bgraUploads.load(), impl_->outputDrops.load()};
}
