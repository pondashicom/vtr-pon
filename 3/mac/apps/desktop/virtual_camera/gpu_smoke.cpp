// Compile the actual producer implementation into this validation executable so
// its private runtime state can be injected without a production test API.
#define VTRPON_VCAM_TEST
#include "output.cpp"
#include "test_mapping.h"
#include <QCoreApplication>
#include <cstdio>
namespace vtrpon {
struct VirtualCameraOutputTestAccess {
    static auto& impl(VirtualCameraOutput& output) { return *output.impl_; }
};
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(app.arguments().contains(QStringLiteral("--identity"))) {
        vtrpon::VirtualCameraRuntime runtime;
        const auto deadline=GetTickCount64()+15000;
        QString ownPath;
        do {
            {
                auto& state=vtrpon::uvcInputGuardDetail::state();
                std::lock_guard lock(state.mutex);
                if(!state.registering) {
                    if(!state.ownDevicePaths.isEmpty()) ownPath=state.ownDevicePaths.constLast();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while(GetTickCount64()<deadline);
        QString reason;
        if(ownPath.isEmpty() || vtrpon::uvcInputAllowed(ownPath.toUpper(),&reason) ||
            !vtrpon::uvcInputAllowed(QStringLiteral("another-camera"))) {
            std::printf("FAIL actual camera identity: %s\n",vtrpon::virtualCameraStatus().toUtf8().constData());
            return 1;
        }
        std::printf("PASS Windows-created camera is blocked as UVC input: %s\n",ownPath.toUtf8().constData());
        return 0;
    }
    TestMapping memory;
    auto* s=memory.mapping->data;
    s->signature=vtrpon::vcam::magic; s->live=1; s->generation=1; s->latest=-1;
    vtrpon::shared.store(memory.mapping); vtrpon::enabled.store(true);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    if(FAILED(hr)) { std::printf("FAIL D3D11 %08lx\n",hr); return 1; }
    const bool fourK=app.arguments().contains(QStringLiteral("--input-4k"));
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=fourK?3840:1920; desc.Height=fourK?2160:1080; desc.MipLevels=1;
    std::printf("GPU input=%ux%u output=%ux%u NV12\n",desc.Width,desc.Height,vtrpon::vcam::width,vtrpon::vcam::height);
    desc.ArraySize=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> target;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) ||
        FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&target))) return 1;
    vtrpon::VirtualCameraOutput output;
    std::vector<unsigned char> copy(vtrpon::vcam::bytes);
    auto verify=[&](const float* color,int y,int u,int v) {
        context->ClearRenderTargetView(target.Get(),color);
        InterlockedIncrement(&s->generation); InterlockedExchange(&s->latest,-1);
        const auto deadline=GetTickCount64()+3000;
        do {
            s->heartbeat=GetTickCount64(); s->consumerHeartbeat=GetTickCount64();
            output.publish(target.Get());
            context->Flush(); // Test stands in for the normal downstream presenter.
            if(vtrpon::vcam::copyLatest(*memory.mapping,copy.data())) {
                const int actualY=copy[540*1920+960],actualU=copy[1920*1080+270*1920+960],actualV=copy[1920*1080+270*1920+961];
                std::printf("NV12 center YUV=%d,%d,%d expected=%d,%d,%d\n",actualY,actualU,actualV,y,u,v);
                return std::abs(actualY-y)<=3 && std::abs(actualU-u)<=3 && std::abs(actualV-v)<=3;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while(GetTickCount64()<deadline);
        std::printf("FAIL GPU timeout: %s\n",vtrpon::virtualCameraStatus().toUtf8().constData()); return false;
    };
    const float red[]={1,0,0,1},black[]={0,0,0,1};
    if(!verify(red,63,102,240) || !verify(black,16,128,128)) return 1;
    auto& impl=vtrpon::VirtualCameraOutputTestAccess::impl(output);
    // Submit a red readback, then simulate a long disconnect without changing
    // the shared generation. It must not reappear as fresh after reconnection.
    for(auto& pending:impl.staging) pending.pending=false;
    impl.next={}; context->ClearRenderTargetView(target.Get(),red);
    output.publish(target.Get()); context->Flush();
    bool aged=false;
    for(auto& pending:impl.staging) if(pending.pending) { pending.captured-=1600; aged=true; }
    if(!aged) return 1;
    InterlockedExchange(&s->latest,-1);
    context->ClearRenderTargetView(target.Get(),black);
    const auto reconnectDeadline=GetTickCount64()+3000;
    bool reconnected=false;
    do {
        s->heartbeat=GetTickCount64(); s->consumerHeartbeat=s->heartbeat;
        output.publish(target.Get()); context->Flush();
        if(vtrpon::vcam::copyLatest(*memory.mapping,copy.data())) {
            if(copy[540*1920+960]!=16) { std::puts("FAIL stale frame on reconnect"); return 1; }
            reconnected=true; break;
        }
        Sleep(2);
    }while(GetTickCount64()<reconnectDeadline);
    if(!reconnected) return 1;
    {
        vtrpon::vcam::Mapping blocker; if(!blocker.open(memory.path)) return 1;
        auto l0=blocker.lockSlot(0),l1=blocker.lockSlot(1),l2=blocker.lockSlot(2);
        if(!l0 || !l1 || !l2) return 1;
        const LONG latest=s->latest;
        const auto drops=impl.busyDrops;
        impl.next={}; output.publish(target.Get()); context->Flush();
        const auto deadline=GetTickCount64()+3000;
        do {
            s->heartbeat=GetTickCount64(); s->consumerHeartbeat=s->heartbeat;
            const auto begin=GetTickCount64(); output.publish(target.Get());
            if(GetTickCount64()-begin>500) return 1;
            context->Flush(); Sleep(2);
        }while(impl.busyDrops==drops && GetTickCount64()<deadline);
        if(impl.busyDrops==drops || s->latest!=latest) return 1;
    }
    // A publisher lease keeps both mapped data and producer election alive.
    if(!memory.mapping->lockProducer()) return 1;
    auto publisherLease=vtrpon::shared.load();
    vtrpon::shared.store(nullptr);
    memory.mapping.reset();
    vtrpon::vcam::Mapping contender; if(!contender.open(memory.path) || contender.lockProducer()) return 1;
    if(publisherLease->data!=s) return 1;
    publisherLease.reset();
    if(!contender.lockProducer()) return 1;
    contender.close();
    std::puts("PASS: GPU NV12, reconnect freshness, actual busy-reader drop and publisher mapping lease");
    return 0;
}
