#define VTRPON_VCAM_TEST
#include "media_source.cpp"
#include "test_mapping.h"
#include <cstdio>
#include <limits>

#define REQUIRE(value) do { if(!(value)) { std::printf("FAIL line %d: %s\n",__LINE__,#value); return false; } } while(false)

namespace {
ComPtr<Stream> makeStream() {
    auto stream=Make<Stream>();
    if(!stream || FAILED(stream->initialize(nullptr))) return {};
    stream->nextOpen=(std::numeric_limits<ULONGLONG>::max)();
    return stream;
}
bool takeSample(Stream* stream,ComPtr<IMFSample>& sample,DWORD timeout=1000) {
    const auto deadline=GetTickCount64()+timeout;
    do {
        ComPtr<IMFMediaEvent> event;
        if(stream->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event)==S_OK) {
            MediaEventType type; REQUIRE(SUCCEEDED(event->GetType(&type)));
            if(type==MEError) { HRESULT hr; event->GetStatus(&hr); std::printf("MEError=%08lx\n",hr); return false; }
            if(type==MEMediaSample) {
                PROPVARIANT value; PropVariantInit(&value);
                REQUIRE(SUCCEEDED(event->GetValue(&value)));
                const auto hr=value.punkVal->QueryInterface(IID_PPV_ARGS(&sample));
                PropVariantClear(&value); return SUCCEEDED(hr);
            }
        } else Sleep(1);
    }while(GetTickCount64()<deadline);
    return false;
}
bool noSamplesOrErrors(Stream* stream) {
    ComPtr<IMFMediaEvent> event;
    while(stream->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event)==S_OK) {
        MediaEventType type; REQUIRE(SUCCEEDED(event->GetType(&type)));
        REQUIRE(type!=MEError && type!=MEMediaSample); event.Reset();
    }
    return true;
}
bool lifecycle() {
    auto stream=makeStream(); REQUIRE(stream);
    REQUIRE(stream->RequestSample(nullptr)==MF_E_MEDIA_SOURCE_WRONGSTATE);
    PROPVARIANT position{}; position.vt=VT_I8; position.hVal.QuadPart=MFGetSystemTime();
    REQUIRE(SUCCEEDED(stream->start(&position)));
    {
        std::lock_guard lock(stream->mutex);
        stream->nextSample=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    }
    REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));
    const auto deadline=GetTickCount64()+1000;
    bool waiting=false;
    do { { std::lock_guard lock(stream->mutex); waiting=stream->inTimedWait; } if(!waiting) Sleep(1); }
    while(!waiting && GetTickCount64()<deadline);
    REQUIRE(waiting);
    REQUIRE(SUCCEEDED(stream->stop()));
    REQUIRE(stream->RequestSample(nullptr)==MF_E_MEDIA_SOURCE_WRONGSTATE);
    stream->nextOpen=(std::numeric_limits<ULONGLONG>::max)();
    REQUIRE(SUCCEEDED(stream->start(&position)));
    Sleep(20);
    REQUIRE(noSamplesOrErrors(stream.Get()));
    REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));
    ComPtr<IMFSample> sample;
    REQUIRE(takeSample(stream.Get(),sample)); sample.Reset();
    REQUIRE(SUCCEEDED(stream->SetStreamState(MF_STREAM_STATE_PAUSED)));
    REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));
    Sleep(20); REQUIRE(noSamplesOrErrors(stream.Get()));
    REQUIRE(SUCCEEDED(stream->SetStreamState(MF_STREAM_STATE_RUNNING)));
    REQUIRE(takeSample(stream.Get(),sample)); sample.Reset();
    stream->close();
    REQUIRE(stream->RequestSample(nullptr)==MF_E_SHUTDOWN);
    std::puts("PASS stopped HRESULT, timed-wait Stop/Start and paused request resume");
    return true;
}
bool backpressure() {
    auto stream=makeStream(); REQUIRE(stream);
    PROPVARIANT position{}; position.vt=VT_I8;
    REQUIRE(SUCCEEDED(stream->start(&position)));
    std::vector<ComPtr<IMFSample>> held;
    for(int i=0;i<8;++i) {
        REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));
        ComPtr<IMFSample> sample; REQUIRE(takeSample(stream.Get(),sample));
        held.push_back(sample);
    }
    REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));
    Sleep(40); REQUIRE(noSamplesOrErrors(stream.Get()));
    held.front().Reset();
    ComPtr<IMFSample> resumed;
    REQUIRE(takeSample(stream.Get(),resumed));
    resumed.Reset(); held.clear(); stream->close();
    std::puts("PASS allocator exhaustion retries accepted request after sample release");
    return true;
}
bool allocationFailures() {
    PROPVARIANT position{}; position.vt=VT_I8;
    for(int stage=1;stage<=4;++stage) {
        auto stream=makeStream(); REQUIRE(stream);
        failStartAt=stage;
        REQUIRE(stream->start(&position)==E_OUTOFMEMORY);
        REQUIRE(stream->state==MF_STREAM_STATE_STOPPED && !stream->allocatorInitialized);
        failStartAt=0;
        REQUIRE(SUCCEEDED(stream->start(&position)));
        REQUIRE(SUCCEEDED(stream->RequestSample(nullptr)));
        ComPtr<IMFSample> sample; REQUIRE(takeSample(stream.Get(),sample));
        sample.Reset(); stream->close();
    }
    std::puts("PASS initialization failure rollback and retry at four resource boundaries");
    return true;
}
bool startEvent() {
    auto source=Make<Source>(); REQUIRE(source && SUCCEEDED(source->initialize(nullptr)));
    source->stream->nextOpen=(std::numeric_limits<ULONGLONG>::max)();
    ComPtr<IMFPresentationDescriptor> pd;
    REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&pd)));
    PROPVARIANT position{};
    REQUIRE(SUCCEEDED(source->Start(pd.Get(),nullptr,&position)));
    bool found=false; ComPtr<IMFMediaEvent> event;
    while(source->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event)==S_OK) {
        MediaEventType type; event->GetType(&type);
        if(type==MESourceStarted) {
            PROPVARIANT value; PropVariantInit(&value); event->GetValue(&value);
            found=value.vt==VT_I8 && value.hVal.QuadPart>0; PropVariantClear(&value);
        }
        event.Reset();
    }
    REQUIRE(found);
    REQUIRE(SUCCEEDED(source->stream->SetStreamState(MF_STREAM_STATE_STOPPED)));
    ComPtr<IMFVideoSampleAllocator> replacement;
    REQUIRE(SUCCEEDED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&replacement))));
    REQUIRE(SUCCEEDED(source->SetDefaultAllocator(0,replacement.Get())));
    REQUIRE(!source->stream->allocatorInitialized);
    REQUIRE(SUCCEEDED(source->Start(pd.Get(),nullptr,&position)));
    REQUIRE(SUCCEEDED(source->stream->RequestSample(nullptr)));
    ComPtr<IMFSample> sample; REQUIRE(takeSample(source->stream.Get(),sample));
    sample.Reset(); REQUIRE(SUCCEEDED(source->Shutdown()));
    std::puts("PASS initial start clock and allocator replacement while stopped"); return true;
}
int holdLocks(const wchar_t* path,const wchar_t* eventName) {
    Mapping mapping; if(!mapping.open(path)) return 2;
    auto l0=mapping.lockSlot(0),l1=mapping.lockSlot(1),l2=mapping.lockSlot(2);
    if(!l0 || !l1 || !l2) return 3;
    for(auto& frame:mapping.data->frames) InterlockedExchange(&frame.owner,2);
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,eventName); if(!ready) return 4;
    SetEvent(ready); CloseHandle(ready);
    Sleep(10000); return 5; // Parent terminates only this dedicated test child.
}
bool abandonedLocks() {
    TestMapping fixture;
    auto* s=fixture.mapping->data;
    s->signature=magic; s->live=1; s->generation=1;
    for(auto& frame:s->frames) { frame.generation=1; std::memset(frame.pixels,42,bytes); }
    const auto eventName=L"Local\\VtrPonCameraTest-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()); REQUIRE(ready);
    wchar_t executable[32768]{}; REQUIRE(GetModuleFileNameW(nullptr,executable,32768));
    std::wstring command=L"\""+std::wstring(executable)+L"\" --hold-locks \""+fixture.path+L"\" \""+eventName+L"\"";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION child{};
    const bool created=CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child);
    if(!created) { CloseHandle(ready); return false; }
    const bool acquired=WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;
    bool blocked=true;
    for(unsigned i=0;i<3;++i) { auto lease=fixture.mapping->lockSlot(i); blocked=blocked && !lease; }
    TerminateProcess(child.hProcess,77);
    const bool exited=WaitForSingleObject(child.hProcess,3000)==WAIT_OBJECT_0;
    CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(ready);
    REQUIRE(acquired && blocked && exited);
    std::vector<unsigned char> pixels(bytes);
    for(unsigned i=0;i<3;++i) {
        s->latest=i; s->heartbeat=GetTickCount64(); s->frames[i].timestamp=s->heartbeat;
        bool copied=false; const auto deadline=GetTickCount64()+1000;
        do { copied=copyLatest(*fixture.mapping,pixels.data()); if(!copied) Sleep(1); }
        while(!copied && GetTickCount64()<deadline);
        REQUIRE(copied && pixels.front()==42 && s->frames[i].owner==0);
    }
    std::puts("PASS all three reader slots recover after isolated child process death");
    return true;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==4 && std::wstring(argv[1])==L"--hold-locks") return holdLocks(argv[2],argv[3]);
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) return 2;
    const bool passed=lifecycle() && backpressure() && allocationFailures() && startEvent() && abandonedLocks();
    const bool released=DllCanUnloadNow()==S_OK;
    MFShutdown(); CoUninitialize();
    return passed && released?0:1;
}
