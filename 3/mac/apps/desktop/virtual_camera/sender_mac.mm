#include "mac_protocol.h"
#import <Foundation/Foundation.h>
#import <CoreMediaIO/CoreMediaIO.h>
#import <CoreVideo/CoreVideo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cerrno>
using namespace vtrpon::vcam;
namespace {
volatile sig_atomic_t running=1;
void stop(int) { running=0; }
void reportConnection(const char *stage, OSStatus status=0) {
    static const char *previous=nullptr; static OSStatus previousStatus=0;
    if(previous!=stage || previousStatus!=status) {
        std::fprintf(stderr,"Virtual camera: %s (%d)\n",stage,int(status));
        previous=stage; previousStatus=status;
    }
}
template<typename T> bool read(CMIOObjectID object,CMIOObjectPropertySelector selector,T &value,CMIOObjectPropertyScope scope=kCMIOObjectPropertyScopeGlobal) {
    CMIOObjectPropertyAddress a{selector,scope,kCMIOObjectPropertyElementMain}; UInt32 size=sizeof(T);
    return CMIOObjectGetPropertyData(object,&a,0,nullptr,size,&size,&value)==noErr;
}
std::vector<CMIOObjectID> list(CMIOObjectID object,CMIOObjectPropertySelector selector,CMIOObjectPropertyScope scope=kCMIOObjectPropertyScopeGlobal) {
    CMIOObjectPropertyAddress a{selector,scope,kCMIOObjectPropertyElementMain}; UInt32 size=0;
    if(CMIOObjectGetPropertyDataSize(object,&a,0,nullptr,&size)!=noErr || size>65536 || size%sizeof(CMIOObjectID)) return {};
    std::vector<CMIOObjectID> ids(size/sizeof(CMIOObjectID));
    if(size && CMIOObjectGetPropertyData(object,&a,0,nullptr,size,&size,ids.data())!=noErr) return {};
    return ids;
}
struct Sink {
    CMIODeviceID device=0; CMIOStreamID stream=0;
    CMSimpleQueueRef queue=nullptr; CVPixelBufferPoolRef pool=nullptr; CMVideoFormatDescriptionRef format=nullptr;
    bool connect() {
        for(auto id:list(kCMIOObjectSystemObject,kCMIOHardwarePropertyDevices)) {
            CFStringRef uid=nullptr;
            if(!read(id,kCMIODevicePropertyDeviceUID,uid) || !uid) continue;
            const bool match=CFStringCompare(uid,CFSTR("BAC4B305-4C38-44BE-A1D3-DA855F14415D"),kCFCompareCaseInsensitive)==kCFCompareEqualTo;
            CFRelease(uid); if(match) { device=id; break; }
        }
        if(!device) { reportConnection("camera not available"); return false; }
        auto streams=list(device,kCMIODevicePropertyStreams,kCMIODevicePropertyScopeOutput);
        if(streams.size()!=1) { reportConnection("camera output stream not available",OSStatus(streams.size())); return false; }
        stream=streams.front();
        auto status=CMIOStreamCopyBufferQueue(stream,[](CMIOStreamID,void*,void*) {},nullptr,&queue);
        if(status!=noErr || !queue) { reportConnection("camera queue unavailable",status); return false; }
        NSDictionary *attrs=@{(__bridge NSString*)kCVPixelBufferWidthKey:@(width),(__bridge NSString*)kCVPixelBufferHeightKey:@(height),
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        if(CVPixelBufferPoolCreate(nullptr,nullptr,(__bridge CFDictionaryRef)attrs,&pool)!=kCVReturnSuccess) return false;
        if(CMVideoFormatDescriptionCreate(nullptr,kCVPixelFormatType_32BGRA,width,height,nullptr,&format)!=noErr) return false;
        status=CMIODeviceStartStream(device,stream);
        reportConnection(status==noErr ? "ON AIR producer connected" : "camera stream start failed",status);
        return status==noErr;
    }
    int demand() {
        CFStringRef clients=nullptr;
        if(!read(device,'vcus',clients) || !clients) return -1;
        const bool result=CFStringGetIntValue(clients)>0; CFRelease(clients); return result;
    }
    bool publish(MacShared *shared) {
        if(CMSimpleQueueGetFullness(queue)>=1) { shared->dropped.fetch_add(1); return false; }
        CVPixelBufferRef pixel=nullptr;
        NSDictionary *aux=@{(__bridge NSString*)kCVPixelBufferPoolAllocationThresholdKey:@6};
        if(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr,pool,(__bridge CFDictionaryRef)aux,&pixel)!=kCVReturnSuccess) return false;
        bool fresh=false;
        uint64_t pts=0;
        if(!pthread_mutex_trylock(&shared->mutex)) {
            pts=shared->publishedNs;
            fresh=shared->enabled.load() && pts && nowNs()-pts<1500000000ULL;
            if(fresh && CVPixelBufferLockBaseAddress(pixel,0)==kCVReturnSuccess) {
                auto *out=static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(pixel));
                const auto row=CVPixelBufferGetBytesPerRow(pixel);
                for(unsigned y=0;y<height;++y) std::memcpy(out+y*row,shared->pixels+y*stride,stride);
                CVPixelBufferUnlockBaseAddress(pixel,0);
            } else fresh=false;
            pthread_mutex_unlock(&shared->mutex);
        }
        if(!fresh) { CFRelease(pixel); return false; }
        CMSampleTimingInfo timing{CMTimeMake(1,30),CMTimeMake(pts,1000000000),kCMTimeInvalid};
        CMSampleBufferRef sample=nullptr;
        const auto status=CMSampleBufferCreateForImageBuffer(nullptr,pixel,true,nullptr,nullptr,format,&timing,&sample);
        CFRelease(pixel);
        if(status!=noErr || !sample) return false;
        // Queue takes the retained sample; the CMIO consumer releases it.
        if(CMSimpleQueueEnqueue(queue,sample)!=noErr) { CFRelease(sample); shared->dropped.fetch_add(1); return false; }
        shared->sent.fetch_add(1); return true;
    }
    ~Sink() {
        if(device && stream) {
            CMIODeviceStopStream(device,stream);
            CMSimpleQueueRef removed=nullptr;
            CMIOStreamCopyBufferQueue(stream,nullptr,nullptr,&removed);
            if(removed) CFRelease(removed);
        }
        if(queue) { while(const void *p=CMSimpleQueueDequeue(queue)) CFRelease(p); CFRelease(queue); }
        if(pool) CFRelease(pool); if(format) CFRelease(format);
    }
};
}
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    const auto parent=static_cast<pid_t>(strtol(argv[1],nullptr,10));
    if(parent<=1 || getppid()!=parent) return 2;
    // Darwin rounds POSIX shared memory to VM pages; validate minimum size.
    struct stat info{}; if(fstat(STDIN_FILENO,&info) || info.st_uid!=geteuid() || info.st_size<sizeof(MacShared)) { std::fprintf(stderr,"Camera mapping descriptor invalid: size=%lld expected=%zu errno=%d\n",static_cast<long long>(info.st_size),sizeof(MacShared),errno); return 3; }
    auto *shared=static_cast<MacShared*>(mmap(nullptr,sizeof(MacShared),PROT_READ|PROT_WRITE,MAP_SHARED,STDIN_FILENO,0));
    close(STDIN_FILENO);
    if(shared==MAP_FAILED) { std::perror("Camera mapping mmap"); return 3; }
    if(shared->magic!=macMagic || shared->version!=macVersion) { std::fprintf(stderr,"Camera mapping ABI mismatch\n"); return 3; }
    signal(SIGTERM,stop); signal(SIGINT,stop);
    while(running && shared->enabled.load() && getppid()==parent) {
        @autoreleasepool {
            shared->heartbeat.store(nowNs()); shared->demand.store(0); shared->state.store(0);
            Sink sink;
            if(sink.connect()) {
                while(running && shared->enabled.load() && getppid()==parent) {
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::nanoseconds(1000000000/30);
                    shared->heartbeat.store(nowNs());
                    const int demand=sink.demand();
                    if(demand<0) break;
                    const bool wanted=demand>0; shared->demand.store(wanted);
                    shared->state.store(!wanted ? 1 : sink.publish(shared) ? 2 : 3);
                    std::this_thread::sleep_until(deadline);
                }
            }
        }
        for(int i=0;i<10 && running && shared->enabled.load() && getppid()==parent;++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    shared->demand.store(0); munmap(shared,sizeof(MacShared)); return 0;
}
