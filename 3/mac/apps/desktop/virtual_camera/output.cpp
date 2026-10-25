#include "output.h"
#include "protocol.h"
#include "../controller_settings.h"
#include "../uvc_input_guard.h"
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <d3d11.h>
#include <wrl.h>
#include <QDebug>
#include <atomic>
#include <mutex>
#include <thread>
#include <system_error>
#include <chrono>
#include <array>

using namespace Microsoft::WRL;
namespace vtrpon {
namespace {
std::atomic_bool enabled{false};
// A publisher retains the mapping (and producer lock) through its final copy.
// Failed/unconfirmed compositor shutdown cannot leave a dangling mapped pointer.
class PublishedMapping {
    std::mutex mutex_;
    std::shared_ptr<vcam::Mapping> value_;
public:
    std::shared_ptr<vcam::Mapping> load() {
        std::unique_lock lock(mutex_,std::try_to_lock);
        return lock.owns_lock()?value_:nullptr;
    }
    void store(std::shared_ptr<vcam::Mapping> value) {
        { std::lock_guard lock(mutex_); value_.swap(value); }
    }
} shared;
std::mutex statusMutex;
QString status;
void report(QString message) { std::lock_guard lock(statusMutex); status=message; qInfo().noquote()<<"Virtual camera:"<<message; }
}
bool virtualCameraEnabled() { return enabled.load(); }
bool setVirtualCameraEnabled(bool value) {
    auto settings=makeControllerSettings();
    settings.sync();
    if(settings.status()!=QSettings::NoError || !settings.isWritable()) return false;
    const QString key=QStringLiteral("virtualCamera/enabled");
    const bool existed=settings.contains(key);
    const QVariant previous=settings.value(key);
    settings.setValue(key,value);
    settings.sync();
    if(settings.status()!=QSettings::NoError) {
        if(existed) settings.setValue(key,previous);
        else settings.remove(key);
        settings.sync();
        return false;
    }
    // Control thread applies state changes; producer also checks enabled before publishing.
    enabled.store(value);
    return true;
}
void reloadVirtualCameraEnabled() {
    enabled.store(makeControllerSettings().value(QStringLiteral("virtualCamera/enabled"),false).toBool());
}
QString virtualCameraStatus() { std::lock_guard lock(statusMutex); return status; }
class VirtualCameraRuntime::Impl {
public:
    Impl() {
        reloadVirtualCameraEnabled();
        try {
            thread=std::jthread([this](std::stop_token stop) { run(stop); });
        } catch (const std::system_error &error) {
            report(QStringLiteral("Virtual camera worker could not start: %1")
                       .arg(QString::fromLocal8Bit(error.what())));
        }
    }
    void run(std::stop_token stop) {
        struct FinishRegistration {
            UvcSelfCameraRegistration &value;
            ~FinishRegistration() { value.finish(); }
        } finishRegistration{registration};
        const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(com)) { report(QStringLiteral("COM initialization failed: %1").arg(quint32(com),8,16,QChar('0'))); return; }
        const HRESULT mf=MFStartup(MF_VERSION);
        if(SUCCEEDED(mf)) {
            start(stop);
            MFShutdown();
        } else report(QStringLiteral("Media Foundation initialization failed."));
        CoUninitialize();
    }
    void start(std::stop_token stop) {
        // Probe installation before creating any camera device. Installation is explicit.
        HKEY key=nullptr;
        const auto reg=std::wstring(L"Software\\Classes\\CLSID\\")+vcam::clsidText+L"\\InprocServer32";
        if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,reg.c_str(),0,KEY_READ,&key)!=ERROR_SUCCESS) {
            report(QStringLiteral("Camera DLL is not installed. Register the virtual camera or reinstall VTR-PON, then restart it.")); return;
        }
        RegCloseKey(key);
        // A byte-range lock elects one producer across sessions. Mapped views are
        // independent of byte-range locks; the reader never accesses frame data via ReadFile.
        auto mapping=std::make_shared<vcam::Mapping>();
        if(!mapping->open() || !mapping->lockProducer()) {
            report(QStringLiteral("Shared memory is unavailable or another VTR-PON owns the camera.")); return;
        }
        auto* s=mapping->data;
        InterlockedExchange(&s->live,0);
        InterlockedIncrement(&s->generation);
        InterlockedExchange(&s->latest,-1);
        // Owner words are diagnostic. A lock released by Windows after a crash
        // is sufficient to reuse a slot, without clearing an active reader.
        InterlockedExchange(&s->signature,vcam::magic);
        HMODULE library=LoadLibraryW(L"mfsensorgroup.dll");
        auto create=library?reinterpret_cast<decltype(&MFCreateVirtualCamera)>(GetProcAddress(library,"MFCreateVirtualCamera")):nullptr;
        ComPtr<IMFVirtualCamera> camera;
        HRESULT hr=create?create(MFVirtualCameraType_SoftwareCameraSource,MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser,L"VTR-PON",vcam::clsidText,nullptr,0,&camera):E_NOTIMPL;
        if(SUCCEEDED(hr)) hr=camera->Start(nullptr);
        if(SUCCEEDED(hr)) {
            wchar_t* symbolicLink=nullptr;
            UINT32 length=0;
            hr=camera->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,&symbolicLink,&length);
            if(SUCCEEDED(hr) && symbolicLink && length>0) {
                registration.finish(QString::fromWCharArray(symbolicLink,static_cast<int>(length)));
                qInfo().noquote()<<"Virtual camera input excluded:"<<QString::fromWCharArray(symbolicLink);
            } else if(SUCCEEDED(hr)) hr=E_UNEXPECTED;
            CoTaskMemFree(symbolicLink);
        }
        if(SUCCEEDED(hr)) {
            shared.store(mapping);
            report(QStringLiteral("Virtual camera available: 1920 x 1080 / 30 fps / NV12"));
            bool wasLive=false;
            while(!stop.stop_requested()) {
                const bool live=enabled.load();
                if(live!=wasLive) {
                    InterlockedExchange(&s->live,0);
                    InterlockedIncrement(&s->generation);
                    InterlockedExchange(&s->latest,-1);
                    wasLive=live;
                }
                InterlockedExchange64(&s->heartbeat,static_cast<LONG64>(GetTickCount64()));
                InterlockedExchange(&s->live,live?1:0);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            shared.store(nullptr);
            InterlockedExchange(&s->live,0);
        } else report(QStringLiteral("Camera start failed: 0x%1").arg(quint32(hr),8,16,QChar('0')));
        if(camera) camera->Shutdown();
        camera.Reset();
        if(library) FreeLibrary(library);
    }
    // Constructed before the worker starts. All early exits resolve the guard;
    // an unidentified camera is shut down rather than left available as input.
    UvcSelfCameraRegistration registration;
    std::jthread thread;
};
VirtualCameraRuntime::VirtualCameraRuntime():impl_(std::make_unique<Impl>()) {}
VirtualCameraRuntime::~VirtualCameraRuntime()=default;

class VirtualCameraOutput::Impl {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<ID3D11VideoProcessorOutputView> output;
    struct Readback { ComPtr<ID3D11Texture2D> texture; bool pending=false; LONG generation=0; ULONGLONG serial=0,captured=0; };
    std::array<Readback,3> staging;
    unsigned sourceWidth=0,sourceHeight=0;
    ULONGLONG serial=0,published=0;
    std::chrono::steady_clock::time_point next{};
    bool failed=false;
#ifdef VTRPON_VCAM_TEST
    unsigned busyDrops=0;
#endif
    HRESULT initialize(ID3D11Texture2D* source,const D3D11_TEXTURE2D_DESC& desc) {
        source->GetDevice(&device); device->GetImmediateContext(&context);
        HRESULT hr=device.As(&videoDevice); if(FAILED(hr)) return hr;
        hr=context.As(&videoContext); if(FAILED(hr)) return hr;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate={60,1}; content.OutputFrameRate={vcam::fps,1};
        content.InputWidth=desc.Width; content.InputHeight=desc.Height;
        content.OutputWidth=vcam::width; content.OutputHeight=vcam::height;
        content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr=videoDevice->CreateVideoProcessorEnumerator(&content,&enumerator); if(FAILED(hr)) return hr;
        UINT support=0;
        hr=enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,&support);
        if(FAILED(hr) || !(support&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) return E_NOTIMPL;
        hr=videoDevice->CreateVideoProcessor(enumerator.Get(),0,&processor); if(FAILED(hr)) return hr;
        D3D11_TEXTURE2D_DESC tex{}; tex.Width=vcam::width; tex.Height=vcam::height;
        tex.MipLevels=1; tex.ArraySize=1; tex.Format=DXGI_FORMAT_NV12; tex.SampleDesc.Count=1;
        tex.Usage=D3D11_USAGE_DEFAULT; tex.BindFlags=D3D11_BIND_RENDER_TARGET;
        hr=device->CreateTexture2D(&tex,nullptr,&nv12); if(FAILED(hr)) return hr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr=videoDevice->CreateVideoProcessorOutputView(nv12.Get(),enumerator.Get(),&view,&output); if(FAILED(hr)) return hr;
        tex.Usage=D3D11_USAGE_STAGING; tex.BindFlags=0; tex.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for(auto& slot:staging) { hr=device->CreateTexture2D(&tex,nullptr,&slot.texture); if(FAILED(hr)) return hr; }
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{}; inputColor.RGB_Range=0; inputColor.YCbCr_Matrix=1;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{}; outputColor.YCbCr_Matrix=1; outputColor.Nominal_Range=1;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(),0,&inputColor);
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(),&outputColor);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(),0,FALSE);
        RECT src{0,0,static_cast<LONG>(desc.Width),static_cast<LONG>(desc.Height)};
        RECT dest{0,0,vcam::width,vcam::height};
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(),0,TRUE,&src);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(),0,TRUE,&dest);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(),TRUE,&dest);
        sourceWidth=desc.Width; sourceHeight=desc.Height;
        return S_OK;
    }
    void publish(ID3D11RenderTargetView* target) {
        auto mapping=shared.load();
        auto* s=mapping?mapping->data:nullptr;
        if(!s || !enabled.load() || !vcam::read(s->live)) {
            for(auto& slot:staging) slot.pending=false;
            return;
        }
        if(failed || !target) return;
        const auto consumer=static_cast<ULONGLONG>(InterlockedCompareExchange64(&s->consumerHeartbeat,0,0));
        const auto tick=GetTickCount64();
        if(consumer>tick || tick-consumer>1500) {
            for(auto& slot:staging) slot.pending=false;
            return;
        }
        ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;
        if(FAILED(resource.As(&source))) return;
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{}; target->GetDesc(&targetDesc);
        UINT slice=0;
        if(targetDesc.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
            targetDesc.Texture2DArray.ArraySize==1 && targetDesc.Texture2DArray.MipSlice==0)
            slice=targetDesc.Texture2DArray.FirstArraySlice;
        else if(targetDesc.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || targetDesc.Texture2D.MipSlice!=0) return;
        if(desc.SampleDesc.Count!=1 || desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM) return;
        if(!processor) {
            const HRESULT hr=initialize(source.Get(),desc);
            if(FAILED(hr)) { failed=true; report(QStringLiteral("NV12 GPU conversion unavailable: 0x%1").arg(quint32(hr),8,16,QChar('0'))); return; }
        }
        if(sourceWidth!=desc.Width || sourceHeight!=desc.Height) { failed=true; report(QStringLiteral("Program geometry changed; restart VTR-PON.")); return; }
        const LONG generation=vcam::read(s->generation);
        for(auto& pending:staging) {
            if(!pending.pending) continue;
            if(pending.captured>tick || tick-pending.captured>1500 || pending.generation!=generation) {
                pending.pending=false; continue;
            }
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr=context->Map(pending.texture.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if(FAILED(hr)) { failed=true; report(QStringLiteral("NV12 readback failed.")); return; }
            if(pending.generation==generation && pending.serial>published) {
                bool written=false;
                for(unsigned i=0;i<3;++i) {
                    const auto index=static_cast<unsigned>((pending.serial+i)%3);
                    auto lease=mapping->lockSlot(index);
                    if(!lease) continue;
                    auto& slot=s->frames[index];
                    InterlockedExchange(&slot.owner,1);
                    // A writer crash must not expose a partially overwritten old frame.
                    slot.timestamp=~ULONGLONG{0};
                    const auto* src=static_cast<const unsigned char*>(map.pData);
                    for(unsigned y=0;y<vcam::height*3/2;++y)
                        std::memcpy(slot.pixels+y*vcam::width,src+y*map.RowPitch,vcam::width);
                    slot.generation=generation; slot.serial=pending.serial; slot.timestamp=pending.captured;
                    InterlockedExchange(&slot.owner,0);
                    InterlockedExchange(&s->latest,static_cast<LONG>(&slot-s->frames));
                    published=pending.serial;
                    written=true;
                    break;
                }
#ifdef VTRPON_VCAM_TEST
                if(!written) ++busyDrops;
#else
                (void)written;
#endif
            }
            context->Unmap(pending.texture.Get(),0); pending.pending=false;
        }
        const auto now=std::chrono::steady_clock::now();
        if(now<next) return;
        auto free=std::find_if(staging.begin(),staging.end(),[](const auto& v){return !v.pending;});
        if(free==staging.end()) return;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.ArraySlice=slice;
        ComPtr<ID3D11VideoProcessorInputView> input;
        HRESULT hr=videoDevice->CreateVideoProcessorInputView(source.Get(),enumerator.Get(),&inputDesc,&input);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable=TRUE; stream.pInputSurface=input.Get();
        if(SUCCEEDED(hr)) hr=videoContext->VideoProcessorBlt(processor.Get(),output.Get(),0,1,&stream);
        if(FAILED(hr)) { failed=true; report(QStringLiteral("NV12 conversion failed: 0x%1").arg(quint32(hr),8,16,QChar('0'))); return; }
        context->CopyResource(free->texture.Get(),nv12.Get());
        free->generation=generation; free->serial=++serial; free->captured=GetTickCount64(); free->pending=true;
        constexpr auto interval=std::chrono::nanoseconds(1'000'000'000/vcam::fps);
        next=(next==std::chrono::steady_clock::time_point{}?now:next)+interval;
        if(next<=now) next=now+interval;
    }
};
VirtualCameraOutput::VirtualCameraOutput():impl_(std::make_unique<Impl>()) {}
VirtualCameraOutput::~VirtualCameraOutput()=default;
void VirtualCameraOutput::publish(ID3D11RenderTargetView* target) { impl_->publish(target); }
}
