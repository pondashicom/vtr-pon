#include "decklink.h"
#include "program_gpu_compositor.h"
#include "gst_frame_capture.h"
#include "operator_monitor_output.h"
#include "virtual_camera/output.h"
#include "network_video.h"
#include "gpu_configuration.h"
#include <QDebug>

#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr GstClockTime kMonitorMinimumFrameInterval = GST_SECOND / 30;
constexpr auto kDeckCallbackDrainTimeout = 5000ms;

struct LatestFrame {
    ~LatestFrame()
    {
        clear();
    }

    void replace(GstSample *next)
    {
        if (!next) {
            return;
        }
        std::scoped_lock lock(mutex);
        gst_clear_sample(&sample);
        sample = next;
    }

    GstSample *snapshot() const
    {
        std::scoped_lock lock(mutex);
        return sample ? gst_sample_ref(sample) : nullptr;
    }

    bool ready() const
    {
        std::scoped_lock lock(mutex);
        return sample != nullptr;
    }

    void clear()
    {
        std::scoped_lock lock(mutex);
        gst_clear_sample(&sample);
    }

    mutable std::mutex mutex;
    GstSample *sample = nullptr;
};

struct DeckInput {
    LatestFrame latest;
    std::atomic_uint64_t frameCount = 0;
    std::mutex callbackMutex;
    std::condition_variable callbackCondition;
    GstAppSink *appSink = nullptr;
    unsigned int activeCallbacks = 0;
    bool acceptingCallbacks = false;
    std::atomic_bool frameUpdatesEnabled = {true};
};

class DeckCallbackScope final
{
public:
    explicit DeckCallbackScope(DeckInput &input) : input_(input)
    {
        std::scoped_lock lock(input_.callbackMutex);
        if (input_.acceptingCallbacks) {
            ++input_.activeCallbacks;
            active_ = true;
        }
    }

    ~DeckCallbackScope()
    {
        if (!active_) {
            return;
        }
        std::scoped_lock lock(input_.callbackMutex);
        --input_.activeCallbacks;
        input_.callbackCondition.notify_all();
    }

    explicit operator bool() const { return active_; }

private:
    DeckInput &input_;
    bool active_ = false;
};
bool currentThreadOwnsWindow(HWND window)
{
    return window && GetWindowThreadProcessId(window, nullptr)
        == GetCurrentThreadId();
}

void dispatchPendingWindowMessages()
{
    MSG message = {};
    PeekMessageW(&message, nullptr, 0, 0,
                 PM_NOREMOVE | PM_NOYIELD | PM_QS_SENDMESSAGE);
}

struct MonitorCadenceLimiter {
    MonitorCadenceLimiter()
    {
        gst_segment_init(&segment, GST_FORMAT_TIME);
    }

    void reset(bool resetSegment = false)
    {
        if (resetSegment) {
            gst_segment_init(&segment, GST_FORMAT_TIME);
        }
        lastKeptRunningTime = GST_CLOCK_TIME_NONE;
    }

    GstSegment segment;
    GstClockTime lastKeptRunningTime = GST_CLOCK_TIME_NONE;
};

GstPadProbeReturn limitMonitorCadence(GstPad *, GstPadProbeInfo *info,
                                      gpointer userData)
{
    auto *limiter = static_cast<MonitorCadenceLimiter *>(userData);
    const GstPadProbeType type = GST_PAD_PROBE_INFO_TYPE(info);

    if ((type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (!event) {
            return GST_PAD_PROBE_OK;
        }
        switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_SEGMENT: {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment && segment->format == GST_FORMAT_TIME) {
                limiter->segment = *segment;
                limiter->reset();
            } else {
                limiter->reset(true);
            }
            break;
        }
        case GST_EVENT_STREAM_START:
        case GST_EVENT_FLUSH_STOP:
        case GST_EVENT_EOS:
            limiter->reset(true);
            break;
        default:
            break;
        }
        return GST_PAD_PROBE_OK;
    }

    if ((type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) {
        return GST_PAD_PROBE_OK;
    }
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
        limiter->reset();
    }

    const GstClockTime timestamp = GST_CLOCK_TIME_IS_VALID(
        GST_BUFFER_PTS(buffer)) ? GST_BUFFER_PTS(buffer)
                                : GST_BUFFER_DTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(timestamp)) {
        return GST_PAD_PROBE_OK;
    }
    const GstClockTime runningTime = gst_segment_to_running_time(
        &limiter->segment, GST_FORMAT_TIME, timestamp);
    if (!GST_CLOCK_TIME_IS_VALID(runningTime)) {
        return GST_PAD_PROBE_OK;
    }

    if (GST_CLOCK_TIME_IS_VALID(limiter->lastKeptRunningTime)
        && runningTime >= limiter->lastKeptRunningTime
        && runningTime - limiter->lastKeptRunningTime
            < kMonitorMinimumFrameInterval) {
        return GST_PAD_PROBE_DROP;
    }
    limiter->lastKeptRunningTime = runningTime;
    return GST_PAD_PROBE_OK;
}

bool compileShader(const char *source, const char *entry, const char *target,
                   ComPtr<ID3DBlob> &byteCode)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), "vtr-pon-program-gpu-compositor", nullptr,
        nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &byteCode,
        &errors);
    if (FAILED(result)) {
        const char *detail = errors
            ? static_cast<const char *>(errors->GetBufferPointer())
            : "unknown shader compiler error";
        g_printerr("D3DCompile %s/%s failed: %s\n", entry, target, detail);
        return false;
    }
    return true;
}

struct alignas(16) LayerConstants {
    std::array<float, 4> color{};
    std::array<float, 4> params{};
};

struct DisplayTarget {
    HMONITOR monitor = nullptr;
    LUID adapterLuid = {};
    bool adapterFound = false;
    bool hybridPresentation = false;
    UINT32 refreshNumerator = 0;
    UINT32 refreshDenominator = 0;
    std::wstring deviceName;
};

bool luidEquals(const LUID &left, const LUID &right)
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

bool queryDisplayRefresh(const std::wstring &deviceName,
                         UINT32 &numerator, UINT32 &denominator)
{
    UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE
        | QDC_VIRTUAL_REFRESH_RATE_AWARE;
    for (int queryAttempt = 0; queryAttempt < 4; ++queryAttempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(
            flags, &pathCount, &modeCount);
        if (result == ERROR_INVALID_PARAMETER
            && (flags & QDC_VIRTUAL_REFRESH_RATE_AWARE) != 0) {
            flags &= ~QDC_VIRTUAL_REFRESH_RATE_AWARE;
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return false;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(flags, &pathCount, paths.data(),
                                    &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return false;
        }
        paths.resize(pathCount);
        for (const auto &path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header)
                    != ERROR_SUCCESS
                || lstrcmpiW(sourceName.viewGdiDeviceName,
                             deviceName.c_str()) != 0) {
                continue;
            }
            const auto refresh = path.targetInfo.refreshRate;
            if (refresh.Numerator == 0 || refresh.Denominator == 0) {
                return false;
            }
            numerator = refresh.Numerator;
            denominator = refresh.Denominator;
            return true;
        }
        return false;
    }
    return false;
}

DisplayTarget queryDisplayForWindow(HWND window)
{
    DisplayTarget target;
    if (!window || !IsWindow(window)) {
        return target;
    }

    target.monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!target.monitor || !GetMonitorInfoW(target.monitor, &info)) {
        target.monitor = nullptr;
        return target;
    }
    target.deviceName = info.szDevice;
    queryDisplayRefresh(target.deviceName, target.refreshNumerator,
                        target.refreshDenominator);

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return target;
    }
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult =
            factory->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapterResult) || !adapter) {
            break;
        }
        DXGI_ADAPTER_DESC1 adapterDesc = {};
        if (FAILED(adapter->GetDesc1(&adapterDesc))) {
            continue;
        }
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            const HRESULT outputResult =
                adapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outputResult) || !output) {
                break;
            }
            DXGI_OUTPUT_DESC outputDesc = {};
            if (SUCCEEDED(output->GetDesc(&outputDesc))
                && outputDesc.Monitor == target.monitor) {
                target.adapterLuid = adapterDesc.AdapterLuid;
                target.adapterFound = true;
                return target;
            }
        }
    }
    return target;
}

bool stopPipeline(GstElement *pipeline, const char *label)
{
    if (!pipeline) {
        return true;
    }
    if (gst_element_set_state(pipeline, GST_STATE_NULL)
        == GST_STATE_CHANGE_FAILURE) {
        g_printerr("%s rejected NULL state\n", label);
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn result = GST_STATE_CHANGE_ASYNC;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        result = gst_element_get_state(
            pipeline, &current, &pending, 0);
        if (result == GST_STATE_CHANGE_FAILURE
            || current == GST_STATE_NULL) {
            break;
        }
        dispatchPendingWindowMessages();
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    if (result == GST_STATE_CHANGE_FAILURE || current != GST_STATE_NULL) {
        g_printerr("%s did not reach NULL (current=%s pending=%s)\n", label,
                   gst_element_state_get_name(current),
                   gst_element_state_get_name(pending));
        return false;
    }
    return true;
}

class DxgiPresenter final
{
public:
    enum class PublishResult { Published, Busy, Fatal };

    DxgiPresenter(const ProgramGpuCompositor::OutputGeometry &outputGeometry,
                  HWND targetWindow)
        : outputGeometry_(outputGeometry), targetWindow_(targetWindow)
    {
    }

    ~DxgiPresenter()
    {
        stop();
    }

    DxgiPresenter(const DxgiPresenter &) = delete;
    DxgiPresenter &operator=(const DxgiPresenter &) = delete;

    bool initializeProducer(GstD3D11Device *gstDevice,
                            const DisplayTarget &target)
    {
        if (!gstDevice || !target.monitor || !target.adapterFound
            || !targetWindow_ || !IsWindow(targetWindow_)) {
            g_printerr("Program presenter requires a valid product window and display adapter\n");
            return false;
        }

        target_ = target;
        producerDevice_ = gst_d3d11_device_get_device_handle(gstDevice);
        producerContext_ =
            gst_d3d11_device_get_device_context_handle(gstDevice);
        if (!producerDevice_ || !producerContext_) {
            g_printerr("Program presenter producer device/context unavailable\n");
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc = {};
        if (FAILED(producerDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))
            || FAILED(dxgiDevice->GetAdapter(&adapter))
            || FAILED(adapter->GetDesc(&adapterDesc))
            || !luidEquals(adapterDesc.AdapterLuid, target.adapterLuid)) {
            g_printerr("Program presenter producer device is on the wrong adapter\n");
            return false;
        }

        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        frameReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stopEvent_ || !frameReadyEvent_) {
            g_printerr("Program presenter event creation failed: %lu\n",
                       GetLastError());
            releaseProducerResources();
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = outputGeometry_.width;
        desc.Height = outputGeometry_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE
            | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        for (auto &slot : slots_) {
            HRESULT result = producerDevice_->CreateTexture2D(
                &desc, nullptr, &slot.producerTexture);
            if (FAILED(result)) {
                failHr("Create shared Program presenter texture", result);
                releaseProducerResources();
                return false;
            }
            result = slot.producerTexture.As(&slot.producerMutex);
            if (FAILED(result)) {
                failHr("Query Program producer keyed mutex", result);
                releaseProducerResources();
                return false;
            }
            ComPtr<IDXGIResource1> resource;
            result = slot.producerTexture.As(&resource);
            if (FAILED(result)) {
                failHr("Query Program shared IDXGIResource1", result);
                releaseProducerResources();
                return false;
            }
            result = resource->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr, &slot.sharedHandle);
            if (FAILED(result) || !slot.sharedHandle) {
                failHr("Create Program presenter shared handle", result);
                releaseProducerResources();
                return false;
            }
        }
        producerReady_ = true;
        return true;
    }

    bool start()
    {
        if (!producerReady_ || presenterThread_.joinable()) {
            return false;
        }
        {
            std::scoped_lock lock(startMutex_);
            threadReady_ = false;
            threadStarted_ = false;
        }
        try {
            presenterThread_ = std::thread(&DxgiPresenter::threadMain, this);
        } catch (const std::system_error &error) {
            g_printerr("Program presenter thread creation failed: %s\n",
                       error.what());
            fatal_.store(true, std::memory_order_relaxed);
            return false;
        }
        const bool pumpOwnerMessages = currentThreadOwnsWindow(targetWindow_);
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        bool threadReady = false;
        bool threadStarted = false;
        do {
            {
                std::unique_lock lock(startMutex_);
                startCondition_.wait_for(
                    lock, pumpOwnerMessages ? 10ms : 100ms,
                    [this] { return threadReady_; });
                threadReady = threadReady_;
                threadStarted = threadStarted_;
            }
            if (!threadReady && pumpOwnerMessages) {
                dispatchPendingWindowMessages();
            }
        } while (!threadReady
                 && std::chrono::steady_clock::now() < deadline);
        if (!threadReady || !threadStarted) {
            SetEvent(stopEvent_);
            joinPresenterThread();
            g_printerr("Program presenter thread failed to initialize\n");
            return false;
        }
        return true;
    }

    void stop()
    {
        if (stopEvent_) {
            SetEvent(stopEvent_);
        }
        joinPresenterThread();
        releaseProducerResources();
    }

    PublishResult publish(ID3D11RenderTargetView *renderTarget,
                          const std::array<GstClockTime, 2> &basePts)
    {
        if (!producerReady_ || !renderTarget
            || fatal_.load(std::memory_order_relaxed)) {
            return PublishResult::Fatal;
        }

        ComPtr<ID3D11Resource> resource;
        renderTarget->GetResource(&resource);
        ComPtr<ID3D11Texture2D> sourceTexture;
        if (!resource || FAILED(resource.As(&sourceTexture))) {
            g_printerr("Program presenter could not query compositor texture\n");
            fatal_.store(true, std::memory_order_relaxed);
            return PublishResult::Fatal;
        }

        D3D11_TEXTURE2D_DESC sourceDesc = {};
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
        sourceTexture->GetDesc(&sourceDesc);
        renderTarget->GetDesc(&viewDesc);
        UINT mipSlice = 0;
        UINT arraySlice = 0;
        if (viewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
            mipSlice = viewDesc.Texture2D.MipSlice;
        } else if (viewDesc.ViewDimension
                       == D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                   && viewDesc.Texture2DArray.ArraySize == 1) {
            mipSlice = viewDesc.Texture2DArray.MipSlice;
            arraySlice = viewDesc.Texture2DArray.FirstArraySlice;
        } else {
            g_printerr("Program presenter received an unsupported RTV\n");
            fatal_.store(true, std::memory_order_relaxed);
            return PublishResult::Fatal;
        }
        const UINT mipWidth = (std::max)(1U, sourceDesc.Width >> mipSlice);
        const UINT mipHeight = (std::max)(1U, sourceDesc.Height >> mipSlice);
        if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            || sourceDesc.SampleDesc.Count != 1
            || mipWidth != outputGeometry_.width
            || mipHeight != outputGeometry_.height) {
            g_printerr("Program presenter texture mismatch format=%u size=%ux%u samples=%u\n",
                       static_cast<unsigned>(sourceDesc.Format), mipWidth,
                       mipHeight, sourceDesc.SampleDesc.Count);
            fatal_.store(true, std::memory_order_relaxed);
            return PublishResult::Fatal;
        }
        const UINT sourceSubresource = D3D11CalcSubresource(
            mipSlice, arraySlice, sourceDesc.MipLevels);

        constexpr UINT64 acquisitionKeys[] = {0, 1};
        for (UINT64 acquisitionKey : acquisitionKeys) {
            for (std::size_t attempt = 0; attempt < slots_.size(); ++attempt) {
                const std::size_t index = (nextProducerSlot_ + attempt)
                    % slots_.size();
                auto &slot = slots_[index];
                const HRESULT acquired = slot.producerMutex->AcquireSync(
                    acquisitionKey, 0);
                if (acquired == WAIT_TIMEOUT) {
                    continue;
                }
                if (acquired != S_OK) {
                    failHr("Program producer AcquireSync", acquired);
                    return PublishResult::Fatal;
                }

                producerContext_->CopySubresourceRegion(
                    slot.producerTexture.Get(), 0, 0, 0, 0,
                    sourceTexture.Get(), sourceSubresource, nullptr);
                producerContext_->Flush();
                const std::uint64_t serial = nextSerial_.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                slot.basePts = basePts;
                slot.serial.store(serial, std::memory_order_release);
                const HRESULT released = slot.producerMutex->ReleaseSync(1);
                if (released != S_OK) {
                    failHr("Program producer ReleaseSync", released);
                    return PublishResult::Fatal;
                }
                nextProducerSlot_ = (index + 1) % slots_.size();
                SetEvent(frameReadyEvent_);
                return PublishResult::Published;
            }
        }
        return PublishResult::Busy;
    }

    std::uint64_t presentCount() const
    {
        return presentCount_.load(std::memory_order_relaxed);
    }

    std::uint64_t consumedCount() const
    {
        return consumedCount_.load(std::memory_order_relaxed);
    }

    std::uint64_t baseFrameCount() const
    {
        return baseFrameCount_.load(std::memory_order_relaxed);
    }

    bool failed() const
    {
        return fatal_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMailboxSlotCount = 8;

    struct SharedSlot {
        ComPtr<ID3D11Texture2D> producerTexture;
        ComPtr<IDXGIKeyedMutex> producerMutex;
        HANDLE sharedHandle = nullptr;
        std::atomic_uint64_t serial = 0;
        // Protected by the slot's keyed mutex, like its texture contents.
        std::array<GstClockTime, 2> basePts{GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE};
    };

    void joinPresenterThread()
    {
        if (!presenterThread_.joinable()) {
            return;
        }
        if (currentThreadOwnsWindow(targetWindow_)) {
            HANDLE threadHandle = presenterThread_.native_handle();
            while (true) {
                const DWORD result = MsgWaitForMultipleObjectsEx(
                    1, &threadHandle, 50, QS_SENDMESSAGE, 0);
                if (result == WAIT_OBJECT_0) {
                    break;
                }
                if (result == WAIT_OBJECT_0 + 1 || result == WAIT_TIMEOUT) {
                    dispatchPendingWindowMessages();
                    continue;
                }
                g_printerr("Program presenter join wait failed: %lu\n",
                           GetLastError());
                break;
            }
        }
        presenterThread_.join();
    }

    bool createPresenterResources()
    {
        if (!targetWindow_ || !IsWindow(targetWindow_)) {
            g_printerr("Program presenter target window is no longer valid\n");
            fatal_.store(true, std::memory_order_relaxed);
            return false;
        }

        ComPtr<IDXGIFactory2> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            return failHr("Create Program presenter DXGI factory", result);
        }

        ComPtr<IDXGIAdapter1> selectedAdapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result) || !candidate) {
                return failHr("Enumerate Program presenter adapter", result);
            }
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(candidate->GetDesc1(&desc))
                && luidEquals(desc.AdapterLuid, target_.adapterLuid)) {
                selectedAdapter = candidate;
                break;
            }
        }
        if (!selectedAdapter) {
            g_printerr("Program presenter could not find the display adapter\n");
            fatal_.store(true, std::memory_order_relaxed);
            return false;
        }

        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selectedLevel = {};
        result = D3D11CreateDevice(
            selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &presenterDevice_, &selectedLevel, &presenterContext_);
        if (FAILED(result)) {
            return failHr("Create Program presenter device", result);
        }

        ComPtr<ID3D11Device1> device1;
        result = presenterDevice_.As(&device1);
        if (FAILED(result)) {
            return failHr("Query Program presenter ID3D11Device1", result);
        }
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            result = device1->OpenSharedResource1(
                slots_[index].sharedHandle,
                IID_PPV_ARGS(&consumerTextures_[index]));
            if (FAILED(result)) {
                return failHr("Open Program presenter shared texture", result);
            }
            result = consumerTextures_[index].As(&consumerMutexes_[index]);
            if (FAILED(result)) {
                return failHr("Query Program presenter keyed mutex", result);
            }
        }

        D3D11_TEXTURE2D_DESC localDesc = {};
        localDesc.Width = outputGeometry_.width;
        localDesc.Height = outputGeometry_.height;
        localDesc.MipLevels = 1;
        localDesc.ArraySize = 1;
        localDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        localDesc.SampleDesc.Count = 1;
        localDesc.Usage = D3D11_USAGE_DEFAULT;
        result = presenterDevice_->CreateTexture2D(
            &localDesc, nullptr, &latestTexture_);
        if (FAILED(result)) {
            return failHr("Create Program presenter latest texture", result);
        }

        DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
        swapDesc.Width = outputGeometry_.width;
        swapDesc.Height = outputGeometry_.height;
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // DWM may retain a displayed surface while the next one is queued.
        // Keep a third surface available for the independent 60 Hz producer.
        swapDesc.BufferCount = 3;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        result = factory->CreateSwapChainForHwnd(
            presenterDevice_.Get(), targetWindow_, &swapDesc, nullptr, nullptr,
            &swapChain_);
        if (FAILED(result)) {
            return failHr("Create product Fullscreen swap chain", result);
        }
        factory->MakeWindowAssociation(targetWindow_, DXGI_MWA_NO_ALT_ENTER);

        ComPtr<IDXGISwapChain2> swapChain2;
        result = swapChain_.As(&swapChain2);
        if (FAILED(result)
            || FAILED(result = swapChain2->SetMaximumFrameLatency(2))) {
            return failHr("Configure Program presenter frame latency", result);
        }
        frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
        if (!frameLatencyWaitable_) {
            g_printerr("Program presenter frame-latency waitable object unavailable\n");
            fatal_.store(true, std::memory_order_relaxed);
            return false;
        }

        ComPtr<IDXGIOutput> containingOutput;
        DXGI_OUTPUT_DESC outputDesc = {};
        const bool monitorMatched = SUCCEEDED(
            swapChain_->GetContainingOutput(&containingOutput))
            && containingOutput
            && SUCCEEDED(containingOutput->GetDesc(&outputDesc))
            && outputDesc.Monitor == target_.monitor;
        // A manually selected render-only GPU uses Windows hybrid presentation.
        // GetContainingOutput cannot identify an output on a different adapter.
        if (!monitorMatched && !target_.hybridPresentation) {
            g_printerr("Product Fullscreen swap chain is on the wrong output\n");
            fatal_.store(true, std::memory_order_relaxed);
            return false;
        }
        UINT maximumLatency = 0;
        swapChain2->GetMaximumFrameLatency(&maximumLatency);
        qInfo().nospace() << "PROGRAM_PRESENT_CONFIG swapchain=0x"
            << Qt::hex << reinterpret_cast<quintptr>(swapChain_.Get()) << Qt::dec
            << " buffers=" << swapDesc.BufferCount
            << " maxLatency=" << maximumLatency
            << " syncInterval=0 presentFlags=" << DXGI_PRESENT_DO_NOT_WAIT
            << " flags=" << swapDesc.Flags;
        return true;
    }

    void threadMain()
    {
        const bool initialized = createPresenterResources();
        {
            std::scoped_lock lock(startMutex_);
            threadStarted_ = initialized;
            threadReady_ = true;
        }
        startCondition_.notify_all();
        if (!initialized) {
            cleanupPresenterResources();
            return;
        }

        bool latencyReady = false;
        auto timingAt = std::chrono::steady_clock::now();
        std::array<double, 4> timingMs{};
        std::array<double, 4> maximumMs{};
        std::uint64_t timingLoops = 0;
        std::uint64_t timingMisses = 0;
        std::uint64_t latencyTimeouts = 0;
        std::uint64_t timingSerial = 0;
        const auto recordTiming = [&](std::size_t phase, auto started) {
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            timingMs[phase] += ms;
            maximumMs[phase] = (std::max)(maximumMs[phase], ms);
        };
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - timingAt).count();
            if (seconds >= 5.0 && timingLoops) {
                const auto serial = nextSerial_.load(std::memory_order_relaxed);
                MONITORINFOEXW monitorInfo{};
                monitorInfo.cbSize = sizeof(monitorInfo);
                UINT32 refreshN = 0, refreshD = 0;
                if (GetMonitorInfoW(MonitorFromWindow(targetWindow_, MONITOR_DEFAULTTONEAREST), &monitorInfo))
                    queryDisplayRefresh(monitorInfo.szDevice, refreshN, refreshD);
                DWORD foregroundPid = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
                qInfo().nospace() << "PROGRAM_PRESENT_TIMING interval=" << seconds
                    << " published=" << double(serial - timingSerial) / seconds
                    << " loops=" << timingLoops << " mailboxMisses=" << timingMisses
                    << " latencyTimeouts=" << latencyTimeouts
                    << " busy=" << busyPresents_ << " occluded=" << occludedPresents_
                    << " avgMs(frame/latency/consume/present)="
                    << timingMs[0] / timingLoops << '/' << timingMs[1] / timingLoops
                    << '/' << timingMs[2] / timingLoops << '/' << timingMs[3] / timingLoops
                    << " maxMs=" << maximumMs[0] << '/' << maximumMs[1]
                    << '/' << maximumMs[2] << '/' << maximumMs[3]
                    << " display=" << QString::fromWCharArray(monitorInfo.szDevice)
                    << " refresh=" << refreshN << '/' << refreshD
                    << " foreground=" << (foregroundPid == GetCurrentProcessId());
                timingAt = now;
                timingSerial = serial;
                timingLoops = timingMisses = 0;
                latencyTimeouts = busyPresents_ = occludedPresents_ = 0;
                timingMs = {};
                maximumMs = {};
            }
            ++timingLoops;
            auto phaseAt = std::chrono::steady_clock::now();
            HANDLE waits[] = {
                stopEvent_, frameReadyEvent_};
            const DWORD waitResult = WaitForMultipleObjects(
                static_cast<DWORD>(std::size(waits)), waits, FALSE, INFINITE);
            recordTiming(0, phaseAt);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            if (waitResult != WAIT_OBJECT_0 + 1) {
                g_printerr("Program presenter wait failed: %lu\n",
                           GetLastError());
                fatal_.store(true, std::memory_order_relaxed);
                break;
            }

            // The producer already clocks this loop. A delayed DWM latency
            // notification must not become a second, slower output clock.
            // Consult it briefly, then let nonblocking Present decide whether
            // there is room. A full queue drops a frame, never queues a retry.
            if (!latencyReady) {
                phaseAt = std::chrono::steady_clock::now();
                HANDLE latencyWaits[] = {stopEvent_, frameLatencyWaitable_};
                const DWORD latencyResult = WaitForMultipleObjects(
                    static_cast<DWORD>(std::size(latencyWaits)), latencyWaits,
                    FALSE, 1);
                recordTiming(1, phaseAt);
                if (latencyResult == WAIT_OBJECT_0) break;
                if (latencyResult == WAIT_TIMEOUT) {
                    ++latencyTimeouts;
                } else if (latencyResult != WAIT_OBJECT_0 + 1) {
                    g_printerr("Program presenter latency wait failed: %lu\n",
                               GetLastError());
                    fatal_.store(true, std::memory_order_relaxed);
                    break;
                }
                latencyReady = latencyResult == WAIT_OBJECT_0 + 1;
            }
            phaseAt = std::chrono::steady_clock::now();
            const bool consumed = consumeLatest();
            recordTiming(2, phaseAt);
            if (failed()) break;
            // A nonblocking mailbox miss did not queue a Present. Retain the
            // latency permit while waiting for the next producer notification.
            if (!consumed) {
                ++timingMisses;
                continue;
            }
            phaseAt = std::chrono::steady_clock::now();
            const bool presented = presentCurrent();
            recordTiming(3, phaseAt);
            if (!presented) {
                break;
            }
            // An occluded window did not queue a frame either. New producer
            // frames provide bounded retries until the window becomes visible.
            latencyReady = latencyReady && presentationSkipped_;
        }
        cleanupPresenterResources();
    }

    bool consumeLatest()
    {
        std::array<bool, kMailboxSlotCount> acquiredSlots{};
        const auto releaseAcquired = [this, &acquiredSlots]() {
            bool releasedAll = true;
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                if (!acquiredSlots[index]) {
                    continue;
                }
                const HRESULT released =
                    consumerMutexes_[index]->ReleaseSync(0);
                acquiredSlots[index] = false;
                if (released != S_OK) {
                    g_printerr("Program presenter ReleaseSync failed: HRESULT=0x%08lx\n",
                               static_cast<unsigned long>(released));
                    releasedAll = false;
                }
            }
            return releasedAll;
        };

        std::size_t newestIndex = slots_.size();
        std::uint64_t newestSerial = lastConsumedSerial_;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const HRESULT acquired = consumerMutexes_[index]->AcquireSync(1, 0);
            if (acquired == WAIT_TIMEOUT) {
                continue;
            }
            if (acquired != S_OK) {
                releaseAcquired();
                failHr("Program presenter AcquireSync", acquired);
                return false;
            }
            acquiredSlots[index] = true;
            const std::uint64_t serial = slots_[index].serial.load(
                std::memory_order_acquire);
            if (serial > newestSerial) {
                newestSerial = serial;
                newestIndex = index;
            }
        }

        const bool consumed = newestIndex < slots_.size();
        if (consumed) {
            presenterContext_->CopyResource(
                latestTexture_.Get(), consumerTextures_[newestIndex].Get());
            presenterContext_->Flush();
            latestBasePts_ = slots_[newestIndex].basePts;
        }
        if (!releaseAcquired()) {
            fatal_.store(true, std::memory_order_relaxed);
            return false;
        }
        if (consumed) {
            lastConsumedSerial_ = newestSerial;
            consumedCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return consumed;
    }

    bool presentCurrent()
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result)) {
            return failHr("Get product Fullscreen back buffer", result);
        }
        presenterContext_->CopyResource(backBuffer.Get(), latestTexture_.Get());
        // Flip-model interval 0 allows the newest completed frame to replace
        // queued stale content. Interval 1 plus the waitable queue can halve the
        // cadence under DWM. Tearing is not enabled on this windowed swap chain.
        result = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        presentationSkipped_ = result == DXGI_STATUS_OCCLUDED
            || result == DXGI_ERROR_WAS_STILL_DRAWING;
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            ++busyPresents_;
            return true;
        }
        if (result == DXGI_STATUS_OCCLUDED) {
            ++occludedPresents_;
            return true;
        }
        if (FAILED(result)) {
            return failHr("Product Fullscreen Present(0)", result);
        }
        presentCount_.fetch_add(1, std::memory_order_relaxed);
        if (latestBasePts_ != presentedBasePts_) {
            if (latestBasePts_[0] != GST_CLOCK_TIME_NONE
                || latestBasePts_[1] != GST_CLOCK_TIME_NONE) {
                baseFrameCount_.fetch_add(1, std::memory_order_relaxed);
            }
            presentedBasePts_ = latestBasePts_;
        }
        return true;
    }

    bool failHr(const char *operation, HRESULT result)
    {
        g_printerr("%s failed: HRESULT=0x%08lx\n", operation,
                   static_cast<unsigned long>(result));
        fatal_.store(true, std::memory_order_relaxed);
        return false;
    }

    void cleanupPresenterResources()
    {
        if (frameLatencyWaitable_) {
            CloseHandle(frameLatencyWaitable_);
            frameLatencyWaitable_ = nullptr;
        }
        swapChain_.Reset();
        latestTexture_.Reset();
        for (auto &mutex : consumerMutexes_) {
            mutex.Reset();
        }
        for (auto &texture : consumerTextures_) {
            texture.Reset();
        }
        presenterContext_.Reset();
        presenterDevice_.Reset();
    }

    void releaseProducerResources()
    {
        producerReady_ = false;
        for (auto &slot : slots_) {
            slot.producerMutex.Reset();
            slot.producerTexture.Reset();
            if (slot.sharedHandle) {
                CloseHandle(slot.sharedHandle);
                slot.sharedHandle = nullptr;
            }
        }
        if (frameReadyEvent_) {
            CloseHandle(frameReadyEvent_);
            frameReadyEvent_ = nullptr;
        }
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        producerContext_ = nullptr;
        producerDevice_ = nullptr;
    }

    ProgramGpuCompositor::OutputGeometry outputGeometry_;
    HWND targetWindow_ = nullptr;
    DisplayTarget target_;
    ID3D11Device *producerDevice_ = nullptr;
    ID3D11DeviceContext *producerContext_ = nullptr;
    std::array<SharedSlot, kMailboxSlotCount> slots_;
    std::size_t nextProducerSlot_ = 0;
    std::atomic_uint64_t nextSerial_ = 0;
    bool producerReady_ = false;
    HANDLE stopEvent_ = nullptr;
    HANDLE frameReadyEvent_ = nullptr;
    std::thread presenterThread_;
    std::mutex startMutex_;
    std::condition_variable startCondition_;
    bool threadReady_ = false;
    bool threadStarted_ = false;
    ComPtr<ID3D11Device> presenterDevice_;
    ComPtr<ID3D11DeviceContext> presenterContext_;
    std::array<ComPtr<ID3D11Texture2D>, kMailboxSlotCount>
        consumerTextures_;
    std::array<ComPtr<IDXGIKeyedMutex>, kMailboxSlotCount>
        consumerMutexes_;
    ComPtr<ID3D11Texture2D> latestTexture_;
    ComPtr<IDXGISwapChain1> swapChain_;
    HANDLE frameLatencyWaitable_ = nullptr;
    std::uint64_t lastConsumedSerial_ = 0;
    std::atomic_uint64_t presentCount_ = 0;
    std::atomic_uint64_t consumedCount_ = 0;
    std::atomic_uint64_t baseFrameCount_ = 0;
    std::array<GstClockTime, 2> latestBasePts_{GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE};
    std::array<GstClockTime, 2> presentedBasePts_{GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE};
    bool presentationSkipped_ = false;
    std::uint64_t busyPresents_ = 0;
    std::uint64_t occludedPresents_ = 0;
    std::atomic_bool fatal_ = false;
};

class GpuRenderer final
{
public:
    explicit GpuRenderer(
        const ProgramGpuCompositor::OutputGeometry &outputGeometry)
        : outputGeometry_(outputGeometry)
    {
    }

    bool initialize(GstD3D11Device *gstDevice)
    {
        if (initialized_) {
            return true;
        }

        device_ = gst_d3d11_device_get_device_handle(gstDevice);
        context_ = gst_d3d11_device_get_device_context_handle(gstDevice);
        if (!device_ || !context_) {
            g_printerr("Program renderer D3D11 device/context unavailable\n");
            return false;
        }

        static constexpr char kVertexShader[] = R"(
struct VsOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VsOutput main(uint vertexId : SV_VertexID) {
    float2 positions[4] = {
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0, -1.0)
    };
    float2 uvs[4] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 1.0)
    };
    VsOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

        static constexpr char kTextureShader[] = R"(
cbuffer LayerConstants : register(b0) {
    float4 color;
    float4 params;
};

Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 sampled = sourceTexture.Sample(sourceSampler, uv);
    return float4(sampled.rgb, sampled.a * params.x);
}
)";

        static constexpr char kOpaqueTextureShader[] = R"(
cbuffer LayerConstants : register(b0) {
    float4 color;
    float4 params;
};

Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 sampled = sourceTexture.Sample(sourceSampler, uv);
    return float4(sampled.rgb * sampled.a, params.x);
}
)";

        static constexpr char kSolidShader[] = R"(
cbuffer LayerConstants : register(b0) {
    float4 color;
    float4 params;
};

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return color;
}
)";

        static constexpr char kCompositeShader[] = R"(
cbuffer LayerConstants : register(b0) { float4 color; float4 params; };
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 p = sourceTexture.Sample(sourceSampler, uv);
    return float4(p.rgb + color.rgb * (1.0 - p.a), 1.0);
}
)";

        ComPtr<ID3DBlob> vertexCode;
        ComPtr<ID3DBlob> textureCode;
        ComPtr<ID3DBlob> opaqueTextureCode;
        ComPtr<ID3DBlob> solidCode;
        ComPtr<ID3DBlob> compositeCode;
        if (!compileShader(kVertexShader, "main", "vs_4_0", vertexCode)
            || !compileShader(kTextureShader, "main", "ps_4_0", textureCode)
            || !compileShader(kOpaqueTextureShader, "main", "ps_4_0",
                              opaqueTextureCode)
            || !compileShader(kSolidShader, "main", "ps_4_0", solidCode)
            || !compileShader(kCompositeShader, "main", "ps_4_0", compositeCode)) {
            return false;
        }

        HRESULT result = device_->CreateVertexShader(
            vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
            nullptr, &vertexShader_);
        if (FAILED(result)) {
            return failHr("Create Program vertex shader", result);
        }
        result = device_->CreatePixelShader(
            textureCode->GetBufferPointer(), textureCode->GetBufferSize(),
            nullptr, &textureShader_);
        if (FAILED(result)) {
            return failHr("Create Program texture shader", result);
        }
        result = device_->CreatePixelShader(
            opaqueTextureCode->GetBufferPointer(),
            opaqueTextureCode->GetBufferSize(), nullptr,
            &opaqueTextureShader_);
        if (FAILED(result)) {
            return failHr("Create Program opaque shader", result);
        }
        result = device_->CreatePixelShader(
            solidCode->GetBufferPointer(), solidCode->GetBufferSize(), nullptr,
            &solidShader_);
        if (FAILED(result)) {
            return failHr("Create Program solid shader", result);
        }
        result = device_->CreatePixelShader(compositeCode->GetBufferPointer(),
            compositeCode->GetBufferSize(), nullptr, &compositeShader_);
        if (FAILED(result)) return failHr("Create Program composite shader", result);

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        result = device_->CreateSamplerState(&samplerDesc, &sampler_);
        if (FAILED(result)) {
            return failHr("Create Program sampler", result);
        }

        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        result = device_->CreateBlendState(&blendDesc, &blendState_);
        if (FAILED(result)) {
            return failHr("Create Program blend state", result);
        }
        // Scale the already composited group, including its coverage. Applying
        // a fade independently to overlapping layers would change their mix.
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_BLEND_FACTOR;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        result = device_->CreateBlendState(&blendDesc, &fadeBlendState_);
        if (FAILED(result)) return failHr("Create Program group fade", result);

        D3D11_TEXTURE2D_DESC transparentDesc{};
        transparentDesc.Width = outputGeometry_.width;
        transparentDesc.Height = outputGeometry_.height;
        transparentDesc.MipLevels = transparentDesc.ArraySize = 1;
        transparentDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        transparentDesc.SampleDesc.Count = 1;
        transparentDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        result = device_->CreateTexture2D(&transparentDesc, nullptr, &transparentTexture_);
        if (FAILED(result)) return failHr("Create transparent Program", result);
        result = device_->CreateRenderTargetView(transparentTexture_.Get(), nullptr, &transparentTarget_);
        if (FAILED(result)) return failHr("Create transparent Program target", result);
        result = device_->CreateShaderResourceView(transparentTexture_.Get(), nullptr, &transparentView_);
        if (FAILED(result)) return failHr("Create transparent Program view", result);

        D3D11_BUFFER_DESC constantsDesc = {};
        constantsDesc.ByteWidth = sizeof(LayerConstants);
        constantsDesc.Usage = D3D11_USAGE_DEFAULT;
        constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        result = device_->CreateBuffer(&constantsDesc, nullptr,
                                       &layerConstants_);
        if (FAILED(result)) {
            return failHr("Create Program layer constants", result);
        }

        initialized_ = true;
        return true;
    }

    bool draw(ID3D11RenderTargetView *renderTarget, GstSample *upperSample,
              GstSample *lowerSample, float upperOpacity, float lowerOpacity,
              float itemFadeProgress, GstSample *upperDskSample,
              GstSample *lowerDskSample, float upperDskOpacity,
              float lowerDskOpacity, float ftbAlpha, std::uint32_t backgroundState,
              bool &dskFailed)
    {
        dskFailed = false;
        if (!initialized_ || !renderTarget) {
            return false;
        }

        ID3D11RenderTargetView *targets[] = {transparentTarget_.Get()};
        context_->OMSetRenderTargets(1, targets, nullptr);
        const float background[] = {
            float((backgroundState >> 16) & 0xff) / 255.0F,
            float((backgroundState >> 8) & 0xff) / 255.0F,
            float(backgroundState & 0xff) / 255.0F, 1.0F};
        const float transparent[] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(transparentTarget_.Get(), transparent);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        ID3D11SamplerState *samplers[] = {sampler_.Get()};
        context_->PSSetSamplers(0, 1, samplers);

        // Input padding is transparent so the selected background remains visible.
        if (lowerSample && lowerOpacity > 0.0F
            && !drawTexture(lowerSample, 0.0F, 0.0F,
                            static_cast<float>(outputGeometry_.width),
                            static_cast<float>(outputGeometry_.height), false,
                            lowerOpacity)) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            return false;
        }

        if (upperSample && upperOpacity > 0.0F
            && !drawTexture(upperSample, 0.0F, 0.0F,
                            static_cast<float>(outputGeometry_.width),
                            static_cast<float>(outputGeometry_.height), false,
                            upperOpacity)) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            return false;
        }

        fadeGroup(std::clamp(itemFadeProgress, 0.0F, 1.0F));

        if (lowerDskSample && lowerDskOpacity > 0.0F
            && !drawContainedTexture(lowerDskSample, lowerDskOpacity)) {
            dskFailed = true;
        }
        if (upperDskSample && upperDskOpacity > 0.0F
            && !drawContainedTexture(upperDskSample, upperDskOpacity)) {
            dskFailed = true;
        }

        fadeGroup(1.0F - std::clamp(ftbAlpha, 0.0F, 1.0F));

        // Preserve the premultiplied Program for network outputs; flatten only
        // the display/Composite target, after all fades and DSK composition.
        targets[0] = renderTarget;
        context_->OMSetRenderTargets(1, targets, nullptr);
        setAlphaBlend(false);
        setViewport(0, 0, float(outputGeometry_.width), float(outputGeometry_.height));
        LayerConstants constants{};
        constants.color = {background[0], background[1], background[2], 1.0F};
        updateConstants(constants);
        ID3D11ShaderResourceView *views[] = {transparentView_.Get()};
        context_->PSSetShaderResources(0, 1, views);
        context_->PSSetShader(compositeShader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
        views[0] = nullptr;
        context_->PSSetShaderResources(0, 1, views);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    ID3D11RenderTargetView *transparentTarget() const { return transparentTarget_.Get(); }

    void fadeGroup(float gain)
    {
        if (gain >= 1.0F) return;
        const float factors[] = {gain, gain, gain, gain};
        context_->OMSetBlendState(fadeBlendState_.Get(), factors, 0xffffffffU);
        setViewport(0, 0, float(outputGeometry_.width), float(outputGeometry_.height));
        updateConstants({});
        context_->PSSetShader(solidShader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
    }

    void reset()
    {
        initialized_ = false;
        transparentView_.Reset();
        transparentTarget_.Reset();
        transparentTexture_.Reset();
        compositeShader_.Reset();
        fadeBlendState_.Reset();
        layerConstants_.Reset();
        blendState_.Reset();
        sampler_.Reset();
        solidShader_.Reset();
        opaqueTextureShader_.Reset();
        textureShader_.Reset();
        vertexShader_.Reset();
        context_ = nullptr;
        device_ = nullptr;
    }

private:
    bool drawContainedTexture(GstSample *sample, float opacity)
    {
        GstCaps *caps = gst_sample_get_caps(sample);
        GstVideoInfo info = {};
        if (!caps || !gst_video_info_from_caps(&info, caps)
            || GST_VIDEO_INFO_WIDTH(&info) == 0
            || GST_VIDEO_INFO_HEIGHT(&info) == 0) {
            g_printerr("Could not read Program layer video geometry\n");
            return false;
        }

        const double parNumerator = GST_VIDEO_INFO_PAR_N(&info) > 0
            ? GST_VIDEO_INFO_PAR_N(&info) : 1;
        const double parDenominator = GST_VIDEO_INFO_PAR_D(&info) > 0
            ? GST_VIDEO_INFO_PAR_D(&info) : 1;
        const double displayWidth = GST_VIDEO_INFO_WIDTH(&info)
            * parNumerator / parDenominator;
        const double displayHeight = GST_VIDEO_INFO_HEIGHT(&info);
        const double scale = (std::min)(
            static_cast<double>(outputGeometry_.width) / displayWidth,
            static_cast<double>(outputGeometry_.height) / displayHeight);
        const float width = static_cast<float>(displayWidth * scale);
        const float height = static_cast<float>(displayHeight * scale);
        const float x = (static_cast<float>(outputGeometry_.width) - width)
            * 0.5F;
        const float y = (static_cast<float>(outputGeometry_.height) - height)
            * 0.5F;
        return drawTexture(sample, x, y, width, height, false, opacity);
    }

    bool deviceMatches(GstD3D11Device *other) const
    {
        return other && device_
            && gst_d3d11_device_get_device_handle(other) == device_;
    }

    bool drawTexture(GstSample *sample, float x, float y,
                     float width, float height, bool opaque, float opacity)
    {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (!buffer || gst_buffer_n_memory(buffer) != 1) {
            g_printerr("Expected one D3D11 memory block in Program sample\n");
            return false;
        }
        GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
        if (!memory || !gst_is_d3d11_memory(memory)) {
            g_printerr("Program sample is not D3D11Memory\n");
            return false;
        }

        auto *d3dMemory = GST_D3D11_MEMORY_CAST(memory);
        if (!deviceMatches(d3dMemory->device)) {
            g_printerr("Program sample uses a different D3D11 device\n");
            return false;
        }

        GstMapInfo mapInfo = {};
        if (!gst_memory_map(
                memory, &mapInfo,
                static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_D3D11))) {
            g_printerr("Could not map Program D3D11 sample\n");
            return false;
        }
        ID3D11ShaderResourceView *view =
            gst_d3d11_memory_get_shader_resource_view(d3dMemory, 0);
        if (!view) {
            g_printerr("Program D3D11 sample has no shader resource view\n");
            gst_memory_unmap(memory, &mapInfo);
            return false;
        }

        opacity = std::clamp(opacity, 0.0F, 1.0F);
        LayerConstants constants = {};
        constants.params[0] = opacity;
        updateConstants(constants);
        setAlphaBlend(!opaque || opacity < 1.0F);
        setViewport(x, y, width, height);
        ID3D11ShaderResourceView *views[] = {view};
        context_->PSSetShaderResources(0, 1, views);
        context_->PSSetShader(
            opaque ? opaqueTextureShader_.Get() : textureShader_.Get(),
            nullptr, 0);
        context_->Draw(4, 0);
        ID3D11ShaderResourceView *emptyViews[] = {nullptr};
        context_->PSSetShaderResources(0, 1, emptyViews);
        gst_memory_unmap(memory, &mapInfo);
        return true;
    }

    void setAlphaBlend(bool enabled)
    {
        const float blendFactor[] = {0.0F, 0.0F, 0.0F, 0.0F};
        context_->OMSetBlendState(enabled ? blendState_.Get() : nullptr,
                                  blendFactor, 0xffffffffU);
    }

    void setViewport(float x, float y, float width, float height)
    {
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = x;
        viewport.TopLeftY = y;
        viewport.Width = width;
        viewport.Height = height;
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;
        context_->RSSetViewports(1, &viewport);
    }

    void updateConstants(const LayerConstants &constants)
    {
        context_->UpdateSubresource(layerConstants_.Get(), 0, nullptr,
                                    &constants, 0, 0);
        ID3D11Buffer *buffers[] = {layerConstants_.Get()};
        context_->PSSetConstantBuffers(0, 1, buffers);
    }

    bool failHr(const char *operation, HRESULT result)
    {
        g_printerr("%s failed: HRESULT=0x%08lx\n", operation,
                   static_cast<unsigned long>(result));
        return false;
    }

    bool initialized_ = false;
    ID3D11Device *device_ = nullptr;
    ID3D11DeviceContext *context_ = nullptr;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> textureShader_;
    ComPtr<ID3D11PixelShader> opaqueTextureShader_;
    ComPtr<ID3D11PixelShader> solidShader_;
    ComPtr<ID3D11PixelShader> compositeShader_;
    ComPtr<ID3D11Texture2D> transparentTexture_;
    ComPtr<ID3D11RenderTargetView> transparentTarget_;
    ComPtr<ID3D11ShaderResourceView> transparentView_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11BlendState> fadeBlendState_;
    ComPtr<ID3D11Buffer> layerConstants_;
    ProgramGpuCompositor::OutputGeometry outputGeometry_;
};

} // namespace

class ProgramGpuCompositor::Impl final
{
public:
    struct DeckLayerState {
        int upperDeck = -1;
        int lowerDeck = -1;
        float upperOpacity = 0.0F;
        float lowerOpacity = 0.0F;
    };

    struct DskLayerState {
        int upperDsk = -1;
        int lowerDsk = -1;
        float upperOpacity = 0.0F;
        float lowerOpacity = 0.0F;
    };

    Impl(std::uintptr_t fullscreenWindowHandle,
         std::uintptr_t monitorWindowHandle,
         const OutputGeometry &outputGeometry)
        : fullscreenWindow_(reinterpret_cast<HWND>(fullscreenWindowHandle)),
          monitorWindow_(reinterpret_cast<HWND>(monitorWindowHandle)),
          outputGeometry_(outputGeometry), renderer_(outputGeometry),
          presenter_(outputGeometry, fullscreenWindow_)
    {
    }

    ~Impl()
    {
        stop();
    }

    VideoFrameCapture captureStillFrame() const {
        return isAvailable() ? captureSinkFrame(monitorSink_) : VideoFrameCapture{};
    }

    bool start()
    {
        if (startAttempted_) {
            return isAvailable();
        }
        startAttempted_ = true;
        if (!fullscreenWindow_ || !monitorWindow_
            || !IsWindow(fullscreenWindow_) || !IsWindow(monitorWindow_)
            || outputGeometry_.width == 0 || outputGeometry_.height == 0) {
            g_printerr("Program GPU compositor received invalid output windows or geometry\n");
            stop();
            return false;
        }

        DisplayTarget displayTarget =
            queryDisplayForWindow(fullscreenWindow_);
        if (!displayTarget.monitor || !displayTarget.adapterFound) {
            g_printerr("Could not resolve the product Fullscreen display adapter\n");
            stop();
            return false;
        }

        const auto preferences = vtrpon::startupGpuPreferences();
        const auto selection = vtrpon::selectGpuAdapter(
            vtrpon::availableGpuAdapters(), preferences.adapterId,
            gst_d3d11_luid_to_int64(&displayTarget.adapterLuid));
        if (selection.adapter.id.isEmpty()) {
            g_printerr("No available GPU matches the Program output display\n");
            stop();
            return false;
        }
        const auto selectedLuid = static_cast<std::uint64_t>(selection.adapter.luid);
        displayTarget.hybridPresentation = selection.adapter.luid
            != gst_d3d11_luid_to_int64(&displayTarget.adapterLuid);
        displayTarget.adapterLuid.LowPart = static_cast<DWORD>(selectedLuid);
        displayTarget.adapterLuid.HighPart = static_cast<LONG>(selectedLuid >> 32);
        vtrpon::configureVideoDecoders(selection.adapter.luid, preferences.softwareDecoding);

        device_ = gst_d3d11_device_new_for_adapter_luid(
            gst_d3d11_luid_to_int64(&displayTarget.adapterLuid), 0);
        if (!device_) {
            g_printerr("Failed to create the Program GstD3D11Device\n");
            stop();
            return false;
        }

        if (!presenter_.initializeProducer(device_, displayTarget)
            || !presenter_.start()
            || !createOutputPipeline()) {
            stop();
            return false;
        }

        const GstStateChangeReturn playRequest =
            gst_element_set_state(outputPipeline_, GST_STATE_PLAYING);
        GstState current = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        GstStateChangeReturn playResult = playRequest;
        const auto playingDeadline = std::chrono::steady_clock::now() + 5s;
        while (playResult != GST_STATE_CHANGE_FAILURE
               && current != GST_STATE_PLAYING
               && std::chrono::steady_clock::now() < playingDeadline) {
            playResult = gst_element_get_state(
                outputPipeline_, &current, &pending, 0);
            if (playResult != GST_STATE_CHANGE_FAILURE
                && current != GST_STATE_PLAYING) {
                dispatchPendingWindowMessages();
                std::this_thread::sleep_for(10ms);
            }
        }
        if (playResult == GST_STATE_CHANGE_FAILURE
            || current != GST_STATE_PLAYING) {
            g_printerr("Program GPU output failed to enter PLAYING\n");
            stop();
            return false;
        }

        const auto firstFrameDeadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < firstFrameDeadline) {
            if (drawFailed_.load(std::memory_order_relaxed)
                || presenter_.failed() || pollBusFailure()) {
                stop();
                return false;
            }
            if (renderCount_.load(std::memory_order_relaxed) > 0
                && presenter_.presentCount() > 0) {
                available_ = true;
                vtrpon::publishGpuStatus(selection, preferences.softwareDecoding);
                g_print("Program GPU compositor started: %ux%u/60, display refresh=%u/%u\n",
                        outputGeometry_.width, outputGeometry_.height,
                        displayTarget.refreshNumerator,
                        displayTarget.refreshDenominator);
                return true;
            }
            dispatchPendingWindowMessages();
            std::this_thread::sleep_for(10ms);
        }

        g_printerr("Program GPU output timed out before its first Fullscreen present\n");
        stop();
        return false;
    }

    bool configureDeckInput(int deckIndex, GstElement *playbin,
                            GstElement *appSink)
    {
        return configureDeckInputWithTimeout(
            deckIndex, playbin, appSink, kDeckCallbackDrainTimeout);
    }

    bool tryConfigureDeckInput(int deckIndex, GstElement *playbin,
                               GstElement *appSink)
    {
        return configureDeckInputWithTimeout(
            deckIndex, playbin, appSink, 0ms);
    }

    void setDeckFrameUpdatesEnabled(int deckIndex, bool enabled)
    {
        if (deckIndex >= 0
            && deckIndex < static_cast<int>(deckInputs_.size())) {
            deckInputs_[deckIndex].frameUpdatesEnabled.store(
                enabled, std::memory_order_release);
        }
    }

    bool suspendDeckFrameUpdatesAndClear(int deckIndex)
    {
        if (deckIndex < 0
            || deckIndex >= static_cast<int>(deckInputs_.size())) {
            return false;
        }
        DeckInput &input = deckInputs_[deckIndex];
        input.frameUpdatesEnabled.store(false, std::memory_order_release);
        std::unique_lock lock(input.callbackMutex);
        const bool callbacksDrained = input.callbackCondition.wait_for(
            lock, kDeckCallbackDrainTimeout,
            [&input] { return input.activeCallbacks == 0; });
        lock.unlock();
        input.latest.clear();
        return callbacksDrained;
    }

    bool configureDskInput(int dskIndex, GstElement *pipeline,
                           GstElement *appSink)
    {
        if (dskIndex < 0
            || dskIndex >= static_cast<int>(dskInputs_.size())) {
            return false;
        }
        return configureFrameInput(dskInputs_[dskIndex], pipeline, appSink);
    }

    GstContext *createInputContext() const
    {
        return device_ ? gst_d3d11_context_new(device_) : nullptr;
    }

    void clearDeckFrame(int deckIndex)
    {
        if (deckIndex >= 0
            && deckIndex < static_cast<int>(deckInputs_.size())) {
            deckInputs_[deckIndex].latest.clear();
        }
    }

    void clearDskFrame(int dskIndex)
    {
        if (dskIndex < 0
            || dskIndex >= static_cast<int>(dskInputs_.size())) {
            return;
        }
        dskRenderFailed_.store(false, std::memory_order_release);
        dskInputs_[dskIndex].latest.clear();
    }

    bool deckFrameReady(int deckIndex) const
    {
        return deckIndex >= 0
            && deckIndex < static_cast<int>(deckInputs_.size())
            && deckInputs_[deckIndex].latest.ready();
    }

    bool dskFrameReady(int dskIndex) const
    {
        return dskIndex >= 0
            && dskIndex < static_cast<int>(dskInputs_.size())
            && dskInputs_[dskIndex].latest.ready();
    }

    bool setDeckLayers(int upperDeck, float upperOpacity,
                       int lowerDeck, float lowerOpacity)
    {
        if (!isAvailable()
            || upperDeck < -1
            || upperDeck >= static_cast<int>(deckInputs_.size())
            || lowerDeck < -1
            || lowerDeck >= static_cast<int>(deckInputs_.size())) {
            return false;
        }
        DeckLayerState state;
        state.upperDeck = upperDeck;
        state.lowerDeck = lowerDeck;
        state.upperOpacity = std::clamp(upperOpacity, 0.0F, 1.0F);
        state.lowerOpacity = std::clamp(lowerOpacity, 0.0F, 1.0F);
        {
            std::scoped_lock lock(deckLayerMutex_);
            deckLayerState_ = state;
        }
        return true;
    }

    bool setDskLayers(int upperDsk, float upperOpacity,
                      int lowerDsk, float lowerOpacity)
    {
        if (!isAvailable()
            || upperDsk < -1
            || upperDsk >= static_cast<int>(dskInputs_.size())
            || lowerDsk < -1
            || lowerDsk >= static_cast<int>(dskInputs_.size())) {
            return false;
        }
        DskLayerState state;
        state.upperDsk = upperDsk;
        state.lowerDsk = lowerDsk;
        state.upperOpacity = std::clamp(upperOpacity, 0.0F, 1.0F);
        state.lowerOpacity = std::clamp(lowerOpacity, 0.0F, 1.0F);
        {
            std::scoped_lock lock(dskLayerMutex_);
            dskLayerState_ = state;
        }
        return true;
    }

    bool takeDskRenderError()
    {
        return dskRenderFailed_.exchange(false, std::memory_order_acq_rel);
    }

    bool takeRuntimeError(std::string &detail)
    {
        logFrameCadence();
        detail.clear();
        if (shutdownRequested_ || runtimeErrorReported_) {
            return false;
        }

        bool failed = pollBusFailure(&detail);
        if (!failed && drawFailed_.load(std::memory_order_relaxed)) {
            detail = "Program GPU renderer failed to draw an output frame.";
            failed = true;
        }
        if (!failed && presenter_.failed()) {
            detail = "Program GPU presenter stopped after a fatal error.";
            failed = true;
        }
        if (!failed) {
            return false;
        }

        runtimeErrorReported_ = true;
        available_ = false;
        return true;
    }

    void setBackgroundColor(std::uint32_t rgb)
    {
        backgroundState_.store(rgb & 0x00ffffffU, std::memory_order_release);
    }

    void setFtbCoverage(float coverage)
    {
        ftbCoverage_.store(std::clamp(coverage, 0.0F, 1.0F),
                           std::memory_order_release);
    }

    void setItemFadeProgress(float progress)
    {
        itemFadeProgress_.store(std::clamp(progress, 0.0F, 1.0F),
                                std::memory_order_release);
    }

    bool prepareShutdown()
    {
        setDeckLayers(-1, 0.0F, -1, 0.0F);
        shutdownRequested_ = true;
        available_ = false;
        {
            std::scoped_lock lock(dskLayerMutex_);
            dskLayerState_ = {};
        }
        const bool inputsDetached = detachFrameInputs();
        detachCallbacks();
        return inputsDetached;
    }

    bool stop()
    {
        if (shutdownConfirmed_) {
            return true;
        }
        if (!prepareShutdown()) {
            return false;
        }

        if (!stopPipeline(outputPipeline_, "Program GPU output")) {
            return false;
        }

        presenter_.stop();
        renderer_.reset();
        for (DeckInput &input : deckInputs_) {
            input.latest.clear();
        }
        for (DeckInput &input : dskInputs_) {
            input.latest.clear();
        }
        releaseOutputPipeline();
        if (device_) {
            gst_object_unref(device_);
            device_ = nullptr;
        }
        shutdownConfirmed_ = true;
        return true;
    }

    bool isAvailable() const
    {
        return available_ && !shutdownRequested_
            && !drawFailed_.load(std::memory_order_relaxed)
            && !presenter_.failed();
    }

    bool canReleaseAfterShutdown() const
    {
        return shutdownConfirmed_;
    }

    std::uint64_t presentedFrameCount() const { return presenter_.presentCount(); }
    std::uint64_t renderedFrameCount() const { return renderCount_.load(std::memory_order_relaxed); }

private:
    void logFrameCadence()
    {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - cadenceAt_).count();
        if (seconds < 5.0 || shutdownRequested_) return;
        const std::array<std::uint64_t, 6> counts = {
            deckInputs_[0].frameCount.load(std::memory_order_relaxed),
            deckInputs_[1].frameCount.load(std::memory_order_relaxed),
            renderedFrameCount(), presenter_.consumedCount(), presentedFrameCount(),
            presenter_.baseFrameCount()};
        const auto fps = [&](std::size_t index) {
            return double(counts[index] - cadenceCounts_[index]) / seconds;
        };
        qInfo().nospace() << "PROGRAM_FPS interval=" << seconds
            << " inputA=" << fps(0) << " inputB=" << fps(1)
            << " render=" << fps(2) << " fresh=" << fps(3)
            << " present=" << fps(4) << " baseUpdates=" << fps(5);
        cadenceCounts_ = counts;
        cadenceAt_ = now;
    }

    bool configureDeckInputWithTimeout(
        int deckIndex, GstElement *playbin, GstElement *appSink,
        std::chrono::milliseconds callbackDrainTimeout)
    {
        if (deckIndex < 0
            || deckIndex >= static_cast<int>(deckInputs_.size())) {
            return false;
        }
        for (int otherIndex = 0;
             otherIndex < static_cast<int>(deckInputs_.size());
             ++otherIndex) {
            if (otherIndex == deckIndex) {
                continue;
            }
            DeckInput &otherInput = deckInputs_[otherIndex];
            bool usesSameAppSink = false;
            {
                std::scoped_lock lock(otherInput.callbackMutex);
                usesSameAppSink = otherInput.appSink == GST_APP_SINK(appSink)
                    && otherInput.acceptingCallbacks;
            }
            if (usesSameAppSink
                && !detachFrameInput(otherInput, false,
                                     callbackDrainTimeout)) {
                return false;
            }
        }
        return configureFrameInput(deckInputs_[deckIndex], playbin, appSink,
                                   callbackDrainTimeout);
    }

    bool configureFrameInput(
        DeckInput &input, GstElement *pipeline, GstElement *appSink,
        std::chrono::milliseconds callbackDrainTimeout =
            kDeckCallbackDrainTimeout)
    {
        if (!isAvailable() || !pipeline || !appSink
            || !GST_IS_APP_SINK(appSink)) {
            return false;
        }

        {
            std::scoped_lock lock(input.callbackMutex);
            if (input.appSink == GST_APP_SINK(appSink)
                && input.acceptingCallbacks) {
                return true;
            }
        }
        if (!detachFrameInput(input, true, callbackDrainTimeout)) {
            return false;
        }

        GstContext *context = gst_d3d11_context_new(device_);
        gst_element_set_context(pipeline, context);
        gst_context_unref(context);

        {
            std::scoped_lock lock(input.callbackMutex);
            if (input.appSink || input.acceptingCallbacks
                || input.activeCallbacks != 0) {
                return false;
            }
            input.appSink = GST_APP_SINK(gst_object_ref(appSink));
            input.acceptingCallbacks = true;
            input.frameUpdatesEnabled.store(true,
                                            std::memory_order_release);
        }

        GstAppSinkCallbacks callbacks = {};
        callbacks.new_preroll = &Impl::onNewDeckPreroll;
        callbacks.new_sample = &Impl::onNewDeckSample;
        gst_app_sink_set_callbacks(input.appSink, &callbacks, &input, nullptr);
        return true;
    }

    bool detachFrameInput(
        DeckInput &input, bool clearLatest = true,
        std::chrono::milliseconds callbackDrainTimeout =
            kDeckCallbackDrainTimeout)
    {
        GstAppSink *appSink = nullptr;
        {
            std::scoped_lock lock(input.callbackMutex);
            input.acceptingCallbacks = false;
            appSink = input.appSink;
        }
        if (appSink) {
            GstAppSinkCallbacks disabledCallbacks = {};
            gst_app_sink_set_callbacks(
                appSink, &disabledCallbacks, nullptr, nullptr);
        }

        std::unique_lock lock(input.callbackMutex);
        if (!input.callbackCondition.wait_for(
                lock, callbackDrainTimeout,
                [&input] { return input.activeCallbacks == 0; })) {
            return false;
        }
        appSink = input.appSink;
        input.appSink = nullptr;
        lock.unlock();
        if (clearLatest) {
            input.latest.clear();
        }
        if (appSink) {
            gst_object_unref(appSink);
        }
        return true;
    }

    static GstFlowReturn storeDeckSample(GstAppSink *sink, gpointer userData,
                                         bool preroll)
    {
        auto &input = *static_cast<DeckInput *>(userData);
        DeckCallbackScope callbackScope(input);
        if (!callbackScope) {
            return GST_FLOW_FLUSHING;
        }

        GstSample *sample = preroll ? gst_app_sink_pull_preroll(sink)
                                    : gst_app_sink_pull_sample(sink);
        if (!sample) {
            return GST_FLOW_EOS;
        }
        if (input.frameUpdatesEnabled.load(std::memory_order_acquire)) {
            input.latest.replace(sample);
            input.frameCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            gst_sample_unref(sample);
        }
        return GST_FLOW_OK;
    }

    static GstFlowReturn onNewDeckPreroll(GstAppSink *sink, gpointer userData)
    {
        return storeDeckSample(sink, userData, true);
    }

    static GstFlowReturn onNewDeckSample(GstAppSink *sink, gpointer userData)
    {
        return storeDeckSample(sink, userData, false);
    }

    static gboolean onOverlayDraw(GstElement *, GstObject *deviceObject,
                                  gpointer renderTarget, guint64, guint64,
                                  gpointer userData)
    {
        auto *self = static_cast<Impl *>(userData);
        auto *device = GST_D3D11_DEVICE(deviceObject);
        GstSample *upperSample = nullptr;
        GstSample *lowerSample = nullptr;
        GstSample *upperDskSample = nullptr;
        GstSample *lowerDskSample = nullptr;
        DeckLayerState layerState;
        DskLayerState dskLayerState;
        {
            std::scoped_lock lock(self->deckLayerMutex_);
            layerState = self->deckLayerState_;
        }
        {
            std::scoped_lock lock(self->dskLayerMutex_);
            dskLayerState = self->dskLayerState_;
        }
        if (layerState.upperDeck >= 0
            && layerState.upperDeck
                < static_cast<int>(self->deckInputs_.size())
            && layerState.upperOpacity > 0.0F) {
            upperSample = self->deckInputs_[layerState.upperDeck].latest.snapshot();
        }
        if (layerState.lowerDeck >= 0
            && layerState.lowerDeck
                < static_cast<int>(self->deckInputs_.size())
            && layerState.lowerOpacity > 0.0F) {
            lowerSample = self->deckInputs_[layerState.lowerDeck].latest.snapshot();
        }
        if (dskLayerState.upperDsk >= 0
            && dskLayerState.upperDsk
                < static_cast<int>(self->dskInputs_.size())
            && dskLayerState.upperOpacity > 0.0F) {
            upperDskSample =
                self->dskInputs_[dskLayerState.upperDsk].latest.snapshot();
        }
        if (dskLayerState.lowerDsk >= 0
            && dskLayerState.lowerDsk
                < static_cast<int>(self->dskInputs_.size())
            && dskLayerState.lowerOpacity > 0.0F) {
            lowerDskSample =
                self->dskInputs_[dskLayerState.lowerDsk].latest.snapshot();
        }
        bool dskFailed = false;
        const auto networkTimecode = vtrpon::networkVideoTimecode();
        if (!self->renderer_.initialize(device)
            || !self->renderer_.draw(
                static_cast<ID3D11RenderTargetView *>(renderTarget),
                upperSample, lowerSample,
                layerState.upperOpacity, layerState.lowerOpacity,
                self->itemFadeProgress_.load(std::memory_order_acquire),
                upperDskSample, lowerDskSample,
                dskLayerState.upperOpacity, dskLayerState.lowerOpacity,
                self->ftbCoverage_.load(std::memory_order_acquire),
                self->backgroundState_.load(std::memory_order_acquire),
                dskFailed)) {
            gst_clear_sample(&upperSample);
            gst_clear_sample(&lowerSample);
            gst_clear_sample(&upperDskSample);
            gst_clear_sample(&lowerDskSample);
            self->drawFailed_.store(true, std::memory_order_relaxed);
            return FALSE;
        }
        std::array<GstClockTime, 2> basePts{GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE};
        if (upperSample) {
            basePts[layerState.upperDeck] = GST_BUFFER_PTS(gst_sample_get_buffer(upperSample));
        }
        if (lowerSample) {
            basePts[layerState.lowerDeck] = GST_BUFFER_PTS(gst_sample_get_buffer(lowerSample));
        }
        gst_clear_sample(&upperSample);
        gst_clear_sample(&lowerSample);
        gst_clear_sample(&upperDskSample);
        gst_clear_sample(&lowerDskSample);
        if (dskFailed) {
            std::scoped_lock lock(self->dskLayerMutex_);
            self->dskLayerState_ = {};
            self->dskRenderFailed_.store(true, std::memory_order_release);
        }
        self->renderCount_.fetch_add(1, std::memory_order_relaxed);
        if (self->presenter_.publish(
                static_cast<ID3D11RenderTargetView *>(renderTarget), basePts)
            == DxgiPresenter::PublishResult::Fatal) {
            self->drawFailed_.store(true, std::memory_order_relaxed);
            return FALSE;
        }
        // Disposable post-composition copy. Its failures never enter Program's
        // bus or drawFailed state. Service the primary presenter first.
        self->operatorMonitor_.publish(
            static_cast<ID3D11RenderTargetView *>(renderTarget));
        self->virtualCamera_.publish(
            static_cast<ID3D11RenderTargetView *>(renderTarget));
        self->deckLink_.publish(static_cast<ID3D11RenderTargetView *>(renderTarget),
            self->renderer_.transparentTarget(), networkTimecode);
        self->networkVideo_.publish(
            static_cast<ID3D11RenderTargetView *>(renderTarget),
            self->renderer_.transparentTarget(), networkTimecode);
        return TRUE;
    }

    static GstBusSyncReply handleBusSync(GstBus *, GstMessage *message,
                                         gpointer userData)
    {
        auto *self = static_cast<Impl *>(userData);
        if (gst_is_video_overlay_prepare_window_handle_message(message)
            && self->monitorWindow_ != nullptr
            && GST_IS_VIDEO_OVERLAY(GST_MESSAGE_SRC(message))) {
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)),
                reinterpret_cast<guintptr>(self->monitorWindow_));
            gst_message_unref(message);
            return GST_BUS_DROP;
        }
        return GST_BUS_PASS;
    }

    bool createOutputPipeline()
    {
        const std::string description =
            "d3d11testsrc is-live=true pattern=black ! "
            "video/x-raw(memory:D3D11Memory),format=BGRA,width="
            + std::to_string(outputGeometry_.width) + ",height="
            + std::to_string(outputGeometry_.height)
            + ",pixel-aspect-ratio=1/1,framerate=60/1 ! "
              "d3d11overlay name=program-gpu-overlay ! "
              "tee name=program-gpu-tee "
              "program-gpu-tee. ! queue name=program-gpu-clock-queue "
              "max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
              "fakesink name=program-gpu-clock-sink sync=true "
              "enable-last-sample=false "
              "program-gpu-tee. ! queue name=program-gpu-monitor-queue "
              "leaky=downstream max-size-buffers=1 max-size-bytes=0 "
              // Downscale only after the monitor queue/cadence limiter. The
              // compositor and fullscreen mailbox retain their output geometry.
              "max-size-time=0 ! d3d11convert ! "
              "video/x-raw(memory:D3D11Memory),format=BGRA,"
              "width=1920,height=1080,pixel-aspect-ratio=1/1 ! "
              "d3d11videosink "
              "name=program-gpu-monitor-sink sync=false async=false "
              "qos=false enable-last-sample=true";

        GError *error = nullptr;
        outputPipeline_ = gst_parse_launch(description.c_str(), &error);
        if (error) {
            g_printerr("Program GPU output parse error: %s\n", error->message);
            g_error_free(error);
        }
        if (!outputPipeline_ || !GST_IS_PIPELINE(outputPipeline_)) {
            return false;
        }

        GstContext *context = gst_d3d11_context_new(device_);
        gst_element_set_context(outputPipeline_, context);
        gst_context_unref(context);

        overlay_ = gst_bin_get_by_name(
            GST_BIN(outputPipeline_), "program-gpu-overlay");
        monitorQueue_ = gst_bin_get_by_name(
            GST_BIN(outputPipeline_), "program-gpu-monitor-queue");
        monitorSink_ = gst_bin_get_by_name(
            GST_BIN(outputPipeline_), "program-gpu-monitor-sink");
        if (!overlay_ || !monitorQueue_ || !monitorSink_
            || !GST_IS_VIDEO_OVERLAY(monitorSink_)) {
            return false;
        }

        gst_video_overlay_set_window_handle(
            GST_VIDEO_OVERLAY(monitorSink_),
            reinterpret_cast<guintptr>(monitorWindow_));
        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(monitorSink_), FALSE);

        monitorCadencePad_ = gst_element_get_static_pad(monitorQueue_, "src");
        if (!monitorCadencePad_) {
            return false;
        }
        monitorCadenceProbeId_ = gst_pad_add_probe(
            monitorCadencePad_,
            static_cast<GstPadProbeType>(
                GST_PAD_PROBE_TYPE_BUFFER
                | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
            &limitMonitorCadence, &monitorCadence_, nullptr);
        if (monitorCadenceProbeId_ == 0) {
            return false;
        }

        drawHandlerId_ = g_signal_connect(
            overlay_, "draw", G_CALLBACK(&Impl::onOverlayDraw), this);
        if (drawHandlerId_ == 0) {
            return false;
        }

        outputBus_ = gst_element_get_bus(outputPipeline_);
        if (!outputBus_) {
            return false;
        }
        gst_bus_set_sync_handler(outputBus_, &Impl::handleBusSync,
                                 this, nullptr);
        return true;
    }

    bool detachFrameInputs()
    {
        const std::array<DeckInput *, 4> inputs = {
            &deckInputs_[0], &deckInputs_[1],
            &dskInputs_[0], &dskInputs_[1]};
        bool detached = true;
        for (DeckInput *input : inputs) {
            // Shutdown callers poll again while callbacks drain; never wait
            // for a streaming callback on the controller event loop.
            detached = detachFrameInput(*input, true, std::chrono::milliseconds(0)) && detached;
        }
        return detached;
    }

    bool pollBusFailure(std::string *firstError = nullptr)
    {
        if (!outputBus_) {
            return false;
        }
        bool failed = false;
        while (GstMessage *message = gst_bus_pop_filtered(
                   outputBus_, static_cast<GstMessageType>(
                       GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            const char *sourceName = GST_MESSAGE_SRC_NAME(message);
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                gst_message_parse_error(message, &error, &debug);
                failed = true;
                if (firstError && firstError->empty()) {
                    *firstError = "Program GPU output ERROR from ";
                    *firstError += sourceName;
                    *firstError += ": ";
                    *firstError += error ? error->message : "unknown error";
                }
                g_printerr("Program GPU output ERROR from %s: %s\n",
                           sourceName,
                           error ? error->message : "unknown error");
            } else {
                gst_message_parse_warning(message, &error, &debug);
                g_printerr("Program GPU output WARNING from %s: %s\n",
                           sourceName,
                           error ? error->message : "unknown warning");
            }
            if (debug) {
                g_printerr("  debug: %s\n", debug);
            }
            g_clear_error(&error);
            g_free(debug);
            gst_message_unref(message);
        }
        return failed;
    }

    void detachCallbacks()
    {
        if (callbacksDetached_) {
            return;
        }
        callbacksDetached_ = true;
        if (outputBus_) {
            gst_bus_set_sync_handler(outputBus_, nullptr, nullptr, nullptr);
        }
        if (monitorCadencePad_ && monitorCadenceProbeId_ != 0) {
            gst_pad_remove_probe(monitorCadencePad_, monitorCadenceProbeId_);
            monitorCadenceProbeId_ = 0;
        }
        if (overlay_ && drawHandlerId_ != 0) {
            g_signal_handler_disconnect(overlay_, drawHandlerId_);
            drawHandlerId_ = 0;
        }
    }

    void releaseOutputPipeline()
    {
        if (monitorCadencePad_) {
            gst_object_unref(monitorCadencePad_);
            monitorCadencePad_ = nullptr;
        }
        if (outputBus_) {
            gst_object_unref(outputBus_);
            outputBus_ = nullptr;
        }
        if (monitorSink_) {
            gst_object_unref(monitorSink_);
            monitorSink_ = nullptr;
        }
        if (monitorQueue_) {
            gst_object_unref(monitorQueue_);
            monitorQueue_ = nullptr;
        }
        if (overlay_) {
            gst_object_unref(overlay_);
            overlay_ = nullptr;
        }
        if (outputPipeline_) {
            gst_object_unref(outputPipeline_);
            outputPipeline_ = nullptr;
        }
    }

    HWND fullscreenWindow_ = nullptr;
    HWND monitorWindow_ = nullptr;
    OutputGeometry outputGeometry_;
    GpuRenderer renderer_;
    DxgiPresenter presenter_;
    vtrpon::OperatorMonitorOutput operatorMonitor_;
    vtrpon::VirtualCameraOutput virtualCamera_;
    vtrpon::NetworkVideoOutput networkVideo_;
    vtrpon::DeckLinkOutput deckLink_;
    // These are logical Deck A/B inputs. Their physical source pipeline can be
    // rebound while the corresponding deck is off air.
    std::array<DeckInput, 2> deckInputs_;
    std::array<DeckInput, 2> dskInputs_;
    mutable std::mutex deckLayerMutex_;
    DeckLayerState deckLayerState_;
    mutable std::mutex dskLayerMutex_;
    DskLayerState dskLayerState_;
    std::atomic_bool dskRenderFailed_ = {false};
    std::atomic<float> itemFadeProgress_ = {1.0F};
    std::atomic<float> ftbCoverage_ = {0.0F};
    std::atomic<std::uint32_t> backgroundState_ = {0};
    GstD3D11Device *device_ = nullptr;
    GstElement *outputPipeline_ = nullptr;
    GstElement *overlay_ = nullptr;
    GstElement *monitorQueue_ = nullptr;
    GstElement *monitorSink_ = nullptr;
    GstBus *outputBus_ = nullptr;
    GstPad *monitorCadencePad_ = nullptr;
    gulong monitorCadenceProbeId_ = 0;
    gulong drawHandlerId_ = 0;
    MonitorCadenceLimiter monitorCadence_;
    std::atomic_uint64_t renderCount_ = 0;
    std::array<std::uint64_t, 6> cadenceCounts_{};
    std::chrono::steady_clock::time_point cadenceAt_ = std::chrono::steady_clock::now();
    std::atomic_bool drawFailed_ = false;
    bool startAttempted_ = false;
    bool available_ = false;
    bool shutdownRequested_ = false;
    bool shutdownConfirmed_ = false;
    bool callbacksDetached_ = false;
    bool runtimeErrorReported_ = false;
};

ProgramGpuCompositor::ProgramGpuCompositor(
    std::uintptr_t fullscreenWindowHandle,
    std::uintptr_t monitorWindowHandle,
    OutputGeometry outputGeometry)
    : impl_(std::make_unique<Impl>(fullscreenWindowHandle,
                                  monitorWindowHandle,
                                  outputGeometry))
{
}

ProgramGpuCompositor::~ProgramGpuCompositor()
{
    if (!impl_) {
        return;
    }
    impl_->stop();
    if (!impl_->canReleaseAfterShutdown()) {
        (void)impl_.release();
    }
}

bool ProgramGpuCompositor::start()
{
    return impl_->start();
}

bool ProgramGpuCompositor::configureDeckInput(int deckIndex,
                                              GstElement *playbin,
                                              GstElement *appSink)
{
    return impl_->configureDeckInput(deckIndex, playbin, appSink);
}

bool ProgramGpuCompositor::tryConfigureDeckInput(int deckIndex,
                                                 GstElement *playbin,
                                                 GstElement *appSink)
{
    return impl_->tryConfigureDeckInput(deckIndex, playbin, appSink);
}

void ProgramGpuCompositor::setDeckFrameUpdatesEnabled(int deckIndex,
                                                       bool enabled)
{
    impl_->setDeckFrameUpdatesEnabled(deckIndex, enabled);
}

bool ProgramGpuCompositor::suspendDeckFrameUpdatesAndClear(int deckIndex)
{
    return impl_->suspendDeckFrameUpdatesAndClear(deckIndex);
}

bool ProgramGpuCompositor::configureDskInput(int dskIndex,
                                             GstElement *pipeline,
                                             GstElement *appSink)
{
    return impl_->configureDskInput(dskIndex, pipeline, appSink);
}

GstContext *ProgramGpuCompositor::createInputContext() const
{
    return impl_->createInputContext();
}

void ProgramGpuCompositor::clearDeckFrame(int deckIndex)
{
    impl_->clearDeckFrame(deckIndex);
}

void ProgramGpuCompositor::clearDskFrame(int dskIndex)
{
    impl_->clearDskFrame(dskIndex);
}

bool ProgramGpuCompositor::deckFrameReady(int deckIndex) const
{
    return impl_->deckFrameReady(deckIndex);
}

bool ProgramGpuCompositor::dskFrameReady(int dskIndex) const
{
    return impl_->dskFrameReady(dskIndex);
}

bool ProgramGpuCompositor::setDeckLayers(int upperDeck, float upperOpacity,
                                          int lowerDeck, float lowerOpacity)
{
    return impl_->setDeckLayers(upperDeck, upperOpacity,
                                lowerDeck, lowerOpacity);
}

bool ProgramGpuCompositor::setDskLayers(int upperDsk, float upperOpacity,
                                        int lowerDsk, float lowerOpacity)
{
    return impl_->setDskLayers(upperDsk, upperOpacity,
                               lowerDsk, lowerOpacity);
}

bool ProgramGpuCompositor::takeDskRenderError()
{
    return impl_->takeDskRenderError();
}

bool ProgramGpuCompositor::takeRuntimeError(std::string &detail)
{
    return impl_->takeRuntimeError(detail);
}

void ProgramGpuCompositor::setItemFadeProgress(float progress)
{
    impl_->setItemFadeProgress(progress);
}

void ProgramGpuCompositor::setBackgroundColor(std::uint32_t rgb)
{
    impl_->setBackgroundColor(rgb);
}

void ProgramGpuCompositor::setFtbCoverage(float coverage)
{
    impl_->setFtbCoverage(coverage);
}

bool ProgramGpuCompositor::prepareShutdown()
{
    return impl_->prepareShutdown();
}

bool ProgramGpuCompositor::stop()
{
    return impl_->stop();
}

VideoFrameCapture ProgramGpuCompositor::captureStillFrame() const { return impl_->captureStillFrame(); }

bool ProgramGpuCompositor::isAvailable() const
{
    return impl_->isAvailable();
}

std::uint64_t ProgramGpuCompositor::presentedFrameCount() const
{
    return impl_->presentedFrameCount();
}

std::uint64_t ProgramGpuCompositor::renderedFrameCount() const
{
    return impl_->renderedFrameCount();
}
