#include "protocol.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <thread>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <system_error>

using namespace Microsoft::WRL;
using namespace vtrpon::vcam;
using Classic=RuntimeClassFlags<ClassicCom>;
#define HR(call) do { const HRESULT hr_=(call); if(FAILED(hr_)) return hr_; } while(false)
namespace {
std::atomic<long> objects{0};
struct Counted { Counted(){++objects;} ~Counted(){--objects;} };
HRESULT exceptionResult() noexcept {
    try { throw; }
    catch(const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch(...) { return E_FAIL; }
}
#ifdef VTRPON_VCAM_TEST
int failStartAt=0;
void checkStartFault(int step) { if(failStartAt==step) throw std::bad_alloc(); }
#else
void checkStartFault(int) {}
#endif
// Event queue remains allocated through Shutdown so concurrent GetEvent callers
// can finish without touching a released queue or holding the source lock.
#define EVENTS \
 STDMETHODIMP GetEvent(DWORD flags,IMFMediaEvent** event) override { return events->GetEvent(flags,event); } \
 STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb,IUnknown* state) override { return events->BeginGetEvent(cb,state); } \
 STDMETHODIMP EndGetEvent(IMFAsyncResult* result,IMFMediaEvent** event) override { return events->EndGetEvent(result,event); } \
 STDMETHODIMP QueueEvent(MediaEventType type,REFGUID guid,HRESULT status,const PROPVARIANT* value) override { return events->QueueEventParamVar(type,guid,status,value); }

HRESULT makeType(IMFMediaType** result) {
    ComPtr<IMFMediaType> type;
    HR(MFCreateMediaType(&type));
    HR(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
    HR(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));
    HR(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,width,height));
    HR(MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,fps,1));
    HR(MFSetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1));
    HR(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
    HR(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT,TRUE));
    HR(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES,TRUE));
    HR(type->SetUINT32(MF_MT_SAMPLE_SIZE,bytes));
    HR(type->SetUINT32(MF_MT_DEFAULT_STRIDE,width));
    HR(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235));
    HR(type->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT709));
    HR(type->SetUINT32(MF_MT_VIDEO_PRIMARIES,MFVideoPrimaries_BT709));
    HR(type->SetUINT32(MF_MT_TRANSFER_FUNCTION,MFVideoTransFunc_709));
    return type.CopyTo(result);
}

class Stream final : public RuntimeClass<Classic,ChainInterfaces<IMFMediaStream2,IMFMediaStream,IMFMediaEventGenerator>>, public Counted {
public:
    std::mutex mutex;
    ComPtr<IMFMediaEventQueue> events;
    ComPtr<IMFMediaSource> parent;
    ComPtr<IMFStreamDescriptor> descriptor;
    ComPtr<IMFVideoSampleAllocator> allocator;
    ComPtr<IMFMediaType> mediaType;
    MF_STREAM_STATE state=MF_STREAM_STATE_STOPPED;
    bool shutdown=false;
    bool allocatorInitialized=false;
    ULONGLONG epoch=0;
    std::chrono::steady_clock::time_point nextSample{};
#ifdef VTRPON_VCAM_TEST
    bool inTimedWait=false;
#endif
    LONGLONG lastTime=0;
    Mapping mapping;
    ULONGLONG nextOpen=0;
    std::vector<unsigned char> idle,frame,black;
    ULONGLONG cachedTimestamp=0;
    LONG cachedGeneration=-1;
    std::condition_variable wake;
    std::deque<ComPtr<IUnknown>> requests;
    std::thread worker;
    ~Stream() { close(); }
    HRESULT initialize(IMFMediaSource* source) {
        HR(MFCreateEventQueue(&events));
        HR(makeType(&mediaType));
        IMFMediaType* types[]={mediaType.Get()};
        HR(MFCreateStreamDescriptor(0,1,types,&descriptor));
        ComPtr<IMFMediaTypeHandler> handler;
        HR(descriptor->GetMediaTypeHandler(&handler));
        HR(handler->SetCurrentMediaType(mediaType.Get()));
        HR(descriptor->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY,PINNAME_VIDEO_CAPTURE));
        HR(descriptor->SetUINT32(MF_DEVICESTREAM_STREAM_ID,0));
        HR(descriptor->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED,1));
        parent=source;
        return S_OK;
    }
    EVENTS
    STDMETHODIMP GetMediaSource(IMFMediaSource** result) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN; return parent.CopyTo(result);
    }
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** result) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN; return descriptor.CopyTo(result);
    }
    HRESULT start(const PROPVARIANT* position) {
        std::lock_guard lock(mutex);
        if(shutdown) return MF_E_SHUTDOWN;
        bool initializedHere=false;
        try {
            if(idle.size()!=bytes || black.size()!=bytes || frame.size()!=bytes) {
                checkStartFault(1); auto newIdle=standby();
                checkStartFault(2); auto newBlack=blackFrame();
                checkStartFault(3); std::vector<unsigned char> newFrame(bytes);
                idle.swap(newIdle); black.swap(newBlack); frame.swap(newFrame);
            }
            if (!allocator) HR(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator)));
            if(!allocatorInitialized) {
                HR(allocator->InitializeSampleAllocator(8,mediaType.Get()));
                allocatorInitialized=true; initializedHere=true;
            }
            if(!worker.joinable()) {
                checkStartFault(4);
                worker=std::thread([this] { pump(); });
            }
        } catch(...) {
            if(initializedHere) { allocator->UninitializeSampleAllocator(); allocatorInitialized=false; }
            return exceptionResult();
        }
        const HRESULT hr=events->QueueEventParamVar(MEStreamStarted,GUID_NULL,S_OK,position);
        if(FAILED(hr)) {
            if(initializedHere) { allocator->UninitializeSampleAllocator(); allocatorInitialized=false; }
            return hr;
        }
        state=MF_STREAM_STATE_RUNNING; ++epoch;
        nextSample=std::chrono::steady_clock::now();
        wake.notify_all();
        return S_OK;
    }
    HRESULT stop() {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        state=MF_STREAM_STATE_STOPPED; ++epoch;
        requests.clear();
        wake.notify_all();
        if(allocatorInitialized) allocator->UninitializeSampleAllocator();
        allocatorInitialized=false;
        mapping.close();
        cachedGeneration=-1; cachedTimestamp=0; nextOpen=0;
        return events->QueueEventParamVar(MEStreamStopped,GUID_NULL,S_OK,nullptr);
    }
    void close() {
        {
            std::lock_guard lock(mutex); if(shutdown) return;
            shutdown=true; state=MF_STREAM_STATE_STOPPED; requests.clear();
            if(events) events->Shutdown();
        }
        wake.notify_all();
        if(worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        parent.Reset();
        if(allocator) allocator->UninitializeSampleAllocator();
        allocator.Reset(); mapping.close();
    }
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        if(value!=MF_STREAM_STATE_RUNNING && value!=MF_STREAM_STATE_PAUSED && value!=MF_STREAM_STATE_STOPPED) return MF_E_INVALID_STATE_TRANSITION;
        if(value==MF_STREAM_STATE_STOPPED) requests.clear();
        state=value; ++epoch;
        nextSample=std::chrono::steady_clock::now();
        wake.notify_all(); return S_OK;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override {
        if(!value) return E_POINTER;
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN; *value=state; return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard lock(mutex);
        if(shutdown) return MF_E_SHUTDOWN;
        if(state==MF_STREAM_STATE_STOPPED) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        if(!allocatorInitialized) return MF_E_NOT_INITIALIZED;
        // A full live queue drops this request; it is not a fatal stream error.
        if(requests.size()>=8) return S_OK;
        try { requests.emplace_back(token); } catch(...) { return exceptionResult(); }
        wake.notify_all();
        return S_OK;
    }
    void pump() {
        std::unique_lock lock(mutex);
        constexpr auto interval=std::chrono::nanoseconds(1'000'000'000/fps);
        while(!shutdown) {
            wake.wait(lock,[this] {return shutdown || (state==MF_STREAM_STATE_RUNNING && !requests.empty());});
            if(shutdown) break;
            const auto requestEpoch=epoch;
#ifdef VTRPON_VCAM_TEST
            inTimedWait=true;
#endif
            wake.wait_until(lock,nextSample,[this,requestEpoch] {return shutdown || state!=MF_STREAM_STATE_RUNNING || epoch!=requestEpoch;});
#ifdef VTRPON_VCAM_TEST
            inTimedWait=false;
#endif
            if(shutdown) break;
            if(state!=MF_STREAM_STATE_RUNNING || epoch!=requestEpoch || requests.empty()) continue;
            auto token=requests.front();
            HRESULT hr;
            try { hr=sendSample(token.Get()); } catch(...) { hr=exceptionResult(); }
            if(hr==MF_E_SAMPLEALLOCATOR_EMPTY) {
                // Bounded backpressure: keep the accepted request until a sample
                // is returned. Stop/pause interrupts this short retry wait.
                wake.wait_for(lock,std::chrono::milliseconds(10),[this,requestEpoch] {
                    return shutdown || state!=MF_STREAM_STATE_RUNNING || epoch!=requestEpoch;
                });
                continue;
            }
            requests.pop_front();
            if(FAILED(hr)) events->QueueEventParamVar(MEError,GUID_NULL,hr,nullptr);
            nextSample=(std::max)(nextSample+interval,std::chrono::steady_clock::now());
        }
    }
    HRESULT sendSample(IUnknown* token) {
        ComPtr<IMFSample> sample;
        HR(allocator->AllocateSample(&sample));
        const auto now=GetTickCount64();
        if(!mapping.data && now>=nextOpen) { mapping.open(); nextOpen=now+1000; }
        if(mapping.data) InterlockedExchange64(&mapping.data->consumerHeartbeat,static_cast<LONG64>(now));
        const bool copied=copyLatest(mapping,frame.data(),&cachedTimestamp,&cachedGeneration);
        auto* s=mapping.data;
        // A producer can briefly own the newest slot. Repeat the last validated
        // frame instead of flashing the standby slate during that collision.
        const auto heartbeat=s?static_cast<ULONGLONG>(InterlockedCompareExchange64(&s->heartbeat,0,0)):0;
        const bool cached=s && read(s->signature)==magic && read(s->live) &&
            read(s->generation)==cachedGeneration && cachedTimestamp<=now &&
            now-cachedTimestamp<=1500 && heartbeat<=now && now-heartbeat<=1500;
        const auto* src=(copied || cached)?frame.data():
            (showDisabledSlate(s)?idle.data():black.data());
        ComPtr<IMFMediaBuffer> buffer;
        HR(sample->GetBufferByIndex(0,&buffer));
        ComPtr<IMF2DBuffer> twoD;
        HR(buffer.As(&twoD));
        HR(twoD->ContiguousCopyFrom(src,bytes));
        HR(buffer->SetCurrentLength(bytes));
        lastTime=(std::max)(MFGetSystemTime(),lastTime+1);
        HR(sample->SetSampleTime(lastTime));
        HR(sample->SetSampleDuration(10'000'000/fps));
        if(token) HR(sample->SetUnknown(MFSampleExtension_Token,token));
        return events->QueueEventParamUnk(MEMediaSample,GUID_NULL,S_OK,sample.Get());
    }
};

class Source final : public RuntimeClass<Classic,ChainInterfaces<IMFMediaSourceEx,IMFMediaSource,IMFMediaEventGenerator>,IMFSampleAllocatorControl,IKsControl,IMFGetService>, public Counted {
public:
    std::mutex mutex;
    ComPtr<IMFMediaEventQueue> events;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFPresentationDescriptor> presentation;
    ComPtr<Stream> stream;
    bool shutdown=false,announced=false;
    HRESULT initialize(IMFAttributes* activation) {
        HR(MFCreateEventQueue(&events));
        HR(MFCreateAttributes(&attributes,8));
        if(activation) HR(activation->CopyAllItems(attributes.Get()));
        ComPtr<IMFSensorProfileCollection> profiles;
        ComPtr<IMFSensorProfile> profile;
        HR(MFCreateSensorProfileCollection(&profiles));
        HR(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy,0,nullptr,&profile));
        HR(profile->AddProfileFilter(0,L"((RES==;FRT<=30,1;SUT==))"));
        HR(profiles->AddProfile(profile.Get()));
        HR(attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION,profiles.Get()));
        HR(attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,MFFrameSourceTypes_Color));
        HR(attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED,1));
        stream=Make<Stream>(); if(!stream) return E_OUTOFMEMORY;
        HR(stream->initialize(this));
        IMFStreamDescriptor* descriptors[]={stream->descriptor.Get()};
        HR(MFCreatePresentationDescriptor(1,descriptors,&presentation));
        return presentation->SelectStream(0);
    }
    EVENTS
    STDMETHODIMP GetCharacteristics(DWORD* value) override {
        if(!value) return E_POINTER; std::lock_guard lock(mutex);
        if(shutdown) return MF_E_SHUTDOWN; *value=MFMEDIASOURCE_IS_LIVE; return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** value) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN; return presentation->Clone(value);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd,const GUID* time,const PROPVARIANT* position) override {
        if(!pd || !position) return E_INVALIDARG;
        if(time && *time!=GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        if(position->vt!=VT_EMPTY && (position->vt!=VT_I8 || position->hVal.QuadPart!=0)) return MF_E_INVALIDREQUEST;
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        DWORD count=0; HR(pd->GetStreamDescriptorCount(&count)); if(count!=1) return E_INVALIDARG;
        BOOL selected=FALSE; ComPtr<IMFStreamDescriptor> desc;
        HR(pd->GetStreamDescriptorByIndex(0,&selected,&desc));
        DWORD id=~0U; HR(desc->GetStreamIdentifier(&id)); if(!selected || id!=0) return E_INVALIDARG;
        ComPtr<IMFMediaTypeHandler> handler; ComPtr<IMFMediaType> type;
        HR(desc->GetMediaTypeHandler(&handler)); HR(handler->GetCurrentMediaType(&type));
        DWORD flags=0; if(type->IsEqual(stream->mediaType.Get(),&flags)!=S_OK) return MF_E_INVALIDMEDIATYPE;
        PROPVARIANT actualStart{}; actualStart.vt=VT_I8;
        actualStart.hVal.QuadPart=MFGetSystemTime();
        HR(stream->start(&actualStart));
        HR(events->QueueEventParamUnk(announced?MEUpdatedStream:MENewStream,GUID_NULL,S_OK,stream.Get()));
        announced=true;
        return events->QueueEventParamVar(MESourceStarted,GUID_NULL,S_OK,&actualStart);
    }
    STDMETHODIMP Stop() override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        HR(stream->stop()); return events->QueueEventParamVar(MESourceStopped,GUID_NULL,S_OK,nullptr);
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        shutdown=true; if(stream) stream->close(); if(events) events->Shutdown(); return S_OK;
    }
    STDMETHODIMP GetSourceAttributes(IMFAttributes** value) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN; return attributes.CopyTo(value);
    }
    STDMETHODIMP GetStreamAttributes(DWORD id,IMFAttributes** value) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        if(id!=0) return MF_E_INVALIDSTREAMNUMBER; return stream->descriptor.CopyTo(value);
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }
    STDMETHODIMP SetDefaultAllocator(DWORD id,IUnknown* value) override {
        std::lock_guard lock(mutex); if(shutdown) return MF_E_SHUTDOWN;
        if(id!=0) return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard streamLock(stream->mutex);
        if(stream->state==MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        if(!value) return E_POINTER;
        ComPtr<IMFVideoSampleAllocator> replacement;
        HR(value->QueryInterface(IID_PPV_ARGS(&replacement)));
        if(stream->allocatorInitialized) stream->allocator->UninitializeSampleAllocator();
        stream->allocatorInitialized=false;
        stream->allocator=std::move(replacement);
        return S_OK;
    }
    STDMETHODIMP GetAllocatorUsage(DWORD id,DWORD* input,MFSampleAllocatorUsage* usage) override {
        if(!input || !usage) return E_POINTER;
        if(id!=0) return MF_E_INVALIDSTREAMNUMBER;
        *input=0; *usage=MFSampleAllocatorUsage_UsesProvidedAllocator; return S_OK;
    }
    STDMETHODIMP GetService(REFGUID,REFIID,void** value) override { if(!value) return E_POINTER; *value=nullptr; return MF_E_UNSUPPORTED_SERVICE; }
    STDMETHODIMP KsProperty(PKSPROPERTY,ULONG,LPVOID,ULONG,ULONG* returned) override { if(returned) *returned=0; return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD,ULONG,LPVOID,ULONG,ULONG* returned) override { if(returned) *returned=0; return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT,ULONG,LPVOID,ULONG,ULONG* returned) override { if(returned) *returned=0; return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
};

class Activate final : public RuntimeClass<Classic,ChainInterfaces<IMFActivate,IMFAttributes>>, public Counted {
public:
    ComPtr<IMFAttributes> attributes;
    ComPtr<Source> source;
    std::mutex mutex;
    HRESULT initialize() { return MFCreateAttributes(&attributes,8); }
    STDMETHODIMP ActivateObject(REFIID iid,void** value) override {
        if(!value) return E_POINTER; *value=nullptr;
        std::lock_guard lock(mutex);
        if(!source) {
            auto candidate=Make<Source>(); if(!candidate) return E_OUTOFMEMORY;
            const HRESULT hr=candidate->initialize(attributes.Get());
            if(FAILED(hr)) { candidate->Shutdown(); return hr; }
            source=candidate;
        }
        return source->QueryInterface(iid,value);
    }
    STDMETHODIMP ShutdownObject() override {
        std::lock_guard lock(mutex); if(source) source->Shutdown(); source.Reset(); return S_OK;
    }
    STDMETHODIMP DetachObject() override { std::lock_guard lock(mutex); source.Reset(); return S_OK; }
    // IMFAttributes forwarding follows below.
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override { return attributes->GetItem(key,value); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* value) override { return attributes->GetItemType(key,value); }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override { return attributes->CompareItem(key,value,result); }
    STDMETHODIMP Compare(IMFAttributes* other, MF_ATTRIBUTES_MATCH_TYPE mode, BOOL* result) override { return attributes->Compare(other,mode,result); }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override { return attributes->GetUINT32(key,value); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override { return attributes->GetUINT64(key,value); }
    STDMETHODIMP GetDouble(REFGUID key, double* value) override { return attributes->GetDouble(key,value); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override { return attributes->GetGUID(key,value); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override { return attributes->GetStringLength(key,length); }
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override { return attributes->GetString(key,value,size,length); }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override { return attributes->GetAllocatedString(key,value,length); }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return attributes->GetBlobSize(key,size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* value, UINT32 size, UINT32* length) override { return attributes->GetBlob(key,value,size,length); }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** value, UINT32* size) override { return attributes->GetAllocatedBlob(key,value,size); }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID iid, void** value) override { return attributes->GetUnknown(key,iid,value); }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override { return attributes->SetItem(key,value); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return attributes->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return attributes->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override { return attributes->SetUINT32(key,value); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override { return attributes->SetUINT64(key,value); }
    STDMETHODIMP SetDouble(REFGUID key, double value) override { return attributes->SetDouble(key,value); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override { return attributes->SetGUID(key,value); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override { return attributes->SetString(key,value); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* value, UINT32 size) override { return attributes->SetBlob(key,value,size); }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override { return attributes->SetUnknown(key,value); }
    STDMETHODIMP LockStore() override { return attributes->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attributes->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* count) override { return attributes->GetCount(count); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override { return attributes->GetItemByIndex(index,key,value); }
    STDMETHODIMP CopyAllItems(IMFAttributes* dest) override { return attributes->CopyAllItems(dest); }
};
class Factory final : public RuntimeClass<Classic,IClassFactory>, public Counted {
public:
    STDMETHODIMP CreateInstance(IUnknown* outer,REFIID iid,void** value) override {
        if(!value) return E_POINTER; *value=nullptr; if(outer) return CLASS_E_NOAGGREGATION;
        auto object=Make<Activate>(); if(!object) return E_OUTOFMEMORY;
        HR(object->initialize()); return object->QueryInterface(iid,value);
    }
    STDMETHODIMP LockServer(BOOL lock) override { objects.fetch_add(lock?1:-1); return S_OK; }
};
}
STDAPI DllGetClassObject(REFCLSID id,REFIID iid,void** value) {
    if(!value) return E_POINTER; *value=nullptr;
    if(id!=clsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory=Make<Factory>(); return factory?factory->QueryInterface(iid,value):E_OUTOFMEMORY;
}
STDAPI DllCanUnloadNow() { return objects.load()==0?S_OK:S_FALSE; }
