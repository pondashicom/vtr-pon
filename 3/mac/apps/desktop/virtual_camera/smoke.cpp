#include "protocol.h"
#include "test_mapping.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl.h>
#include <cstdio>
#include <algorithm>
#include <memory>

using namespace Microsoft::WRL;
using namespace vtrpon::vcam;
#define CHECK(call) do {HRESULT hr_=(call); if(FAILED(hr_)) { std::printf("FAIL line %d: %s = %08lx\n",__LINE__,#call,hr_); return 1; }}while(false)

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) { std::puts("Usage: virtual-camera-smoke path-to-DLL | --device"); return 2; }
    CHECK(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
    CHECK(MFStartup(MF_VERSION));
    HMODULE dll=nullptr;
    ComPtr<IMFActivate> activate;
    ComPtr<IMFMediaSource> source;
    if(std::wstring(argv[1])==L"--device") {
        ComPtr<IMFAttributes> attributes;
        CHECK(MFCreateAttributes(&attributes,1));
        CHECK(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
        IMFActivate** devices=nullptr; UINT32 count=0;
        CHECK(MFEnumDeviceSources(attributes.Get(),&devices,&count));
        for(UINT32 i=0;i<count;++i) {
            wchar_t* name=nullptr; UINT32 length=0;
            if(SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,&name,&length))) {
                if(std::wstring(name).find(L"VTR-PON")!=std::wstring::npos) activate=devices[i];
                CoTaskMemFree(name);
            }
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
        if(!activate) { std::puts("FAIL: VTR-PON camera is not enumerated; start VTR-PON first."); return 1; }
    } else {
        dll=LoadLibraryW(argv[1]); if(!dll) { std::printf("LoadLibrary failed %lu\n",GetLastError()); return 1; }
        auto get=reinterpret_cast<HRESULT(__stdcall*)(REFCLSID,REFIID,void**)>(GetProcAddress(dll,"DllGetClassObject"));
        if(!get) return 1;
        ComPtr<IClassFactory> factory;
        CHECK(get(clsid,IID_PPV_ARGS(&factory)));
        CHECK(factory->CreateInstance(nullptr,IID_PPV_ARGS(&activate)));
    }
    CHECK(activate->ActivateObject(IID_PPV_ARGS(&source)));
    ComPtr<IMFSourceReader> reader;
    CHECK(MFCreateSourceReaderFromMediaSource(source.Get(),nullptr,&reader));
    int received=0;
    for(int attempt=0;attempt<90 && received<3;++attempt) {
        ComPtr<IMFSample> sample; DWORD actual=0,flags=0; LONGLONG time=0;
        CHECK(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&actual,&flags,&time,&sample));
        if(!sample && (flags & MF_SOURCE_READERF_STREAMTICK)) continue;
        if(!sample) { std::printf("FAIL: no sample flags=%lu\n",flags); return 1; }
        ComPtr<IMFMediaBuffer> buffer; CHECK(sample->ConvertToContiguousBuffer(&buffer));
        BYTE* pixels=nullptr; DWORD length=0; CHECK(buffer->Lock(&pixels,nullptr,&length));
        if(length!=bytes) { buffer->Unlock(); std::printf("FAIL: sample size %lu\n",length); return 1; }
        const auto bright=std::count_if(pixels,pixels+width*height,[](BYTE p){return p>128;});
        std::printf("sample %d: %lu bytes, time=%lld, bright pixels=%lld\n",received,length,time,static_cast<long long>(bright));
        if(dll && (pixels[0]!=16 || bright>15000 || pixels[width*height]!=128)) {
            buffer->Unlock(); std::puts("FAIL: standby image mismatch"); return 1;
        }
        buffer->Unlock();
        ++received;
    }
    if(received!=3) { std::puts("FAIL: camera returned only stream ticks"); return 1; }
    reader.Reset(); source->Shutdown(); source.Reset(); activate->ShutdownObject(); activate.Reset();
    // Exercise transport acceptance gates independently of camera installation.
    TestMapping fixture;
    auto* state=fixture.mapping->data;
    state->signature=magic; state->live=1; state->generation=7; state->latest=1;
    state->heartbeat=GetTickCount64();
    if(showDisabledSlate(state)) return 1;
    state->live=0; if(!showDisabledSlate(state)) return 1;
    state->heartbeat=0; if(showDisabledSlate(state)) return 1;
    state->live=1; state->heartbeat=GetTickCount64();
    const auto slate=standby();
    unsigned textPixels=0;
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        if(slate[y*width+x]>128) {
            ++textPixels;
            if(x<48 || x>500 || y<height/2-30 || y>height/2+30) {
                std::puts("FAIL: standby text must be at left / vertical center"); return 1;
            }
        }
    }
    if(textPixels<100) return 1;
    state->frames[1].generation=7; state->frames[1].timestamp=GetTickCount64();
    std::memset(state->frames[1].pixels,42,bytes);
    std::vector<unsigned char> copy(bytes);
    if(!copyLatest(*fixture.mapping,copy.data()) || copy.front()!=42 || copy.back()!=42) return 1;
    {
        Mapping blocker; if(!blocker.open(fixture.path)) return 1;
        auto lease=blocker.lockSlot(1); if(!lease || copyLatest(*fixture.mapping,copy.data())) return 1;
    }
    state->live=0; if(copyLatest(*fixture.mapping,copy.data())) return 1;
    state->live=1; state->generation=8; if(copyLatest(*fixture.mapping,copy.data())) return 1;
    state->generation=7; state->heartbeat=0; if(copyLatest(*fixture.mapping,copy.data())) return 1;
    std::puts("PASS: source samples and transport live/generation/ownership/staleness gates");
    if(dll) {
        auto unload=reinterpret_cast<HRESULT(__stdcall*)()>(GetProcAddress(dll,"DllCanUnloadNow"));
        if(!unload || unload()!=S_OK) { std::puts("FAIL: DLL COM objects leaked"); return 1; }
        FreeLibrary(dll);
    }
    MFShutdown(); CoUninitialize();
    return 0;
}
