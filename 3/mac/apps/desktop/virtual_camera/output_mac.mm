#include "output.h"
#include "mac_protocol.h"
#include "../controller_settings.h"
#include "../uvc_input_guard.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QImage>
#include <QDir>
#include <QDebug>
#include <cerrno>
#import <AppKit/AppKit.h>
#import <SystemExtensions/SystemExtensions.h>
#import <Security/Security.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <new>
#include <mutex>

namespace {
QString activationMessage;
}
@interface VTRCameraActivation : NSObject<OSSystemExtensionRequestDelegate>
@end
@implementation VTRCameraActivation
- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request {
    activationMessage=qtTrId("virtual_camera.mac.approval");
}
- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request actionForReplacingExtension:(OSSystemExtensionProperties *)existing withExtension:(OSSystemExtensionProperties *)replacement { return OSSystemExtensionReplacementActionReplace; }
- (void)request:(OSSystemExtensionRequest *)request didFinishWithResult:(OSSystemExtensionRequestResult)result {
    activationMessage=qtTrId(result==OSSystemExtensionRequestCompleted ? "virtual_camera.mac.activated" : "virtual_camera.mac.reboot");
    if(result==OSSystemExtensionRequestCompleted) {
        const auto message=activationMessage;
        QTimer::singleShot(5000,[message] { if(activationMessage==message) activationMessage.clear(); });
    }
}
- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error {
    activationMessage=qtTrId("virtual_camera.mac.activation_failed").arg(QString::fromNSString(error.localizedDescription));
}
@end
namespace vtrpon {
namespace {
struct Mapping {
    int fd=-1;
    vcam::MacShared *shared=nullptr;
    Mapping() {
        auto name=(QStringLiteral("/vtrcam-")+QUuid::createUuid().toString(QUuid::Id128).left(18)).toUtf8();
        fd=shm_open(name.constData(),O_CREAT|O_EXCL|O_RDWR,0600);
        if(fd<0) { qWarning()<<"Virtual camera shm_open failed"<<errno; return; }
        if(shm_unlink(name.constData()) || fcntl(fd,F_SETFD,FD_CLOEXEC) || ftruncate(fd,sizeof(vcam::MacShared))) { qWarning()<<"Virtual camera mapping setup failed"<<errno; return; }
        void *p=mmap(nullptr,sizeof(vcam::MacShared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(p==MAP_FAILED) { qWarning()<<"Virtual camera mmap failed"<<errno; return; }
        auto *s=new(p) vcam::MacShared;
        pthread_mutexattr_t a;
        bool ready=pthread_mutexattr_init(&a)==0;
        if(ready) { ready=pthread_mutexattr_setpshared(&a,PTHREAD_PROCESS_SHARED)==0 && pthread_mutex_init(&s->mutex,&a)==0; pthread_mutexattr_destroy(&a); }
        if(ready) shared=s; else { qWarning()<<"Virtual camera mutex initialization failed"; munmap(p,sizeof(vcam::MacShared)); }
    }
    ~Mapping() { if(shared) munmap(shared,sizeof(vcam::MacShared)); if(fd>=0) close(fd); }
};
std::atomic_bool enabled{false};
std::mutex mappingMutex;
std::weak_ptr<Mapping> active;
std::shared_ptr<Mapping> loadMapping() { std::unique_lock lock(mappingMutex,std::try_to_lock); return lock ? active.lock() : nullptr; }
void storeMapping(const std::shared_ptr<Mapping> &m) { std::lock_guard lock(mappingMutex); active=m; }
VTRCameraActivation *activationDelegate;
}
bool virtualCameraEnabled() { return enabled.load(); }
bool setVirtualCameraEnabled(bool value) {
    auto s=makeControllerSettings(); s.sync();
    if(s.status()!=QSettings::NoError || !s.isWritable()) return false;
    const QString key=QStringLiteral("virtualCamera/enabled");
    const bool existed=s.contains(key); const auto old=s.value(key);
    s.setValue(key,value); s.sync();
    if(s.status()!=QSettings::NoError) { if(existed) s.setValue(key,old); else s.remove(key); s.sync(); return false; }
    enabled.store(value); activationMessage.clear(); return true;
}
void reloadVirtualCameraEnabled() { enabled.store(makeControllerSettings().value(QStringLiteral("virtualCamera/enabled"),false).toBool()); }
bool virtualCameraFrameDemand() {
    if(!enabled.load()) return false;
    auto m=loadMapping(); auto *s=m ? m->shared : nullptr;
    return s && s->enabled.load() && s->demand.load() && vcam::nowNs()-s->heartbeat.load()<1500000000ULL;
}
bool publishVirtualCameraFrame(const QImage &image,uint64_t sequence) {
    if(!enabled.load() || image.size()!=QSize(vcam::width,vcam::height) || image.format()!=QImage::Format_ARGB32) return false;
    auto m=loadMapping(); auto *s=m ? m->shared : nullptr;
    if(!s || !s->enabled.load() || !s->demand.load() || pthread_mutex_trylock(&s->mutex)) return false;
    // Recheck permission after obtaining the slot; OFF is never queued as old video.
    if(enabled.load() && s->enabled.load()) {
        for(unsigned y=0;y<vcam::height;++y) std::memcpy(s->pixels+y*vcam::stride,image.constScanLine(y),vcam::stride);
        s->sequence=sequence; s->publishedNs=vcam::nowNs();
    }
    pthread_mutex_unlock(&s->mutex); return true;
}
QString virtualCameraStatus() {
    if(!activationMessage.isEmpty()) return activationMessage;
    if(!enabled.load()) return qtTrId("virtual_camera.mac.off");
    auto m=loadMapping(); auto *s=m ? m->shared : nullptr;
    if(!s) return qtTrId("virtual_camera.mac.helper_wait");
    switch(s->state.load()) {
    case 1: return qtTrId("virtual_camera.mac.receiver_wait");
    case 2: return qtTrId("virtual_camera.mac.sending");
    case 3: return qtTrId("virtual_camera.mac.source_wait");
    default: return qtTrId("virtual_camera.mac.extension_wait");
    }
}
bool activateVirtualCameraExtension() {
    activationMessage.clear();
    if(![NSBundle.mainBundle.bundlePath hasPrefix:@"/Applications/"]) {
        activationMessage=qtTrId("virtual_camera.mac.applications"); return false;
    }
    if(!activationDelegate) activationDelegate=[VTRCameraActivation new];
    auto request=[OSSystemExtensionRequest activationRequestForExtension: [NSString stringWithUTF8String:vcam::extensionId] queue:dispatch_get_main_queue()];
    request.delegate=activationDelegate;
    activationMessage=qtTrId("virtual_camera.mac.activating");
    [OSSystemExtensionManager.sharedManager submitRequest:request]; return true;
}
void openVirtualCameraSettings() {
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.LoginItems-Settings.extension"]];
}
bool cameraSenderSignatureValid(const QString &path) {
    SecCodeRef self=nullptr; CFDictionaryRef information=nullptr;
    const auto selfStatus=SecCodeCopySelf(kSecCSDefaultFlags,&self);
    const auto infoStatus=self ? SecCodeCopySigningInformation(self,kSecCSSigningInformation,&information) : errSecCSUnsigned;
    auto *info=(__bridge NSDictionary *)information;
    NSString *team=info[(__bridge NSString*)kSecCodeInfoTeamIdentifier];
    if(self) CFRelease(self);
    NSString *expression=team ? [NSString stringWithFormat:@"anchor apple generic and identifier \"com.pondashi.vtrpon3.camera-output\" and certificate leaf[subject.OU] = \"%@\"",team] : nil;
    SecRequirementRef requirement=nullptr; SecStaticCodeRef code=nullptr;
    const auto requirementStatus=expression ? SecRequirementCreateWithString((__bridge CFStringRef)expression,kSecCSDefaultFlags,&requirement) : errSecCSUnsigned;
    const auto codeStatus=SecStaticCodeCreateWithPath((__bridge CFURLRef)[NSURL fileURLWithPath:path.toNSString()],kSecCSDefaultFlags,&code);
    const bool valid=selfStatus==errSecSuccess && infoStatus==errSecSuccess && requirementStatus==errSecSuccess && codeStatus==errSecSuccess &&
        SecStaticCodeCheckValidity(code,kSecCSStrictValidate,requirement)==errSecSuccess;
    if(information) CFRelease(information);
    if(requirement) CFRelease(requirement);
    if(code) CFRelease(code);
    return valid;
}
class VirtualCameraRuntime::Impl : public QObject {
public:
    QProcess process;
    QTimer timer;
    std::shared_ptr<Mapping> mapping;
    uint64_t retryAt=0, stopAt=0, startedAt=0;
    Impl() {
        UvcSelfCameraRegistration registration; registration.finish(QStringLiteral("avfoundation:")+QString::fromLatin1(vcam::deviceUid));
        reloadVirtualCameraEnabled();
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QObject::connect(&process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError) { qWarning()<<"Virtual camera sender:"<<process.errorString(); });
        QObject::connect(&process,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status) { qInfo()<<"Virtual camera sender exited"<<code<<status; storeMapping({}); mapping.reset(); retryAt=vcam::nowNs()+500000000ULL; });
        QObject::connect(&timer,&QTimer::timeout,this,[this] { tick(); });
        timer.start(100); tick();
    }
    void tick() {
        const auto now=vcam::nowNs();
        if(!enabled.load()) {
            storeMapping({});
            if(mapping) mapping->shared->enabled.store(0);
            if(process.state()!=QProcess::NotRunning) {
                if(!stopAt) { stopAt=now; process.terminate(); }
                if(now-stopAt>250000000ULL) process.kill();
            } else mapping.reset();
            return;
        }
        if(process.state()!=QProcess::NotRunning) {
            const auto heartbeat=mapping ? mapping->shared->heartbeat.load() : 0;
            if(now-(heartbeat ? heartbeat : startedAt)>3000000000ULL) {
                if(mapping) { mapping->shared->enabled.store(0); mapping->shared->demand.store(0); }
                if(!stopAt) { stopAt=now; process.terminate(); }
                if(now-stopAt>250000000ULL) process.kill();
            }
            return;
        }
        if(now<retryAt) return;
        stopAt=0; mapping=std::make_shared<Mapping>();
        if(!mapping->shared) { mapping.reset(); retryAt=now+1000000000ULL; return; }
        const int fd=mapping->fd;
        process.setChildProcessModifier([fd] { if(dup2(fd,STDIN_FILENO)<0) _exit(126); });
        const auto sender=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../Helpers/vtr-pon-camera-output.app/Contents/MacOS/vtr-pon-camera-output"));
        if(!cameraSenderSignatureValid(sender)) {
            qWarning()<<"Virtual camera sender signature or team is invalid:"<<sender;
            storeMapping({}); mapping.reset(); retryAt=now+5000000000ULL; return;
        }
        process.setProgram(sender);
        process.setArguments({QString::number(getpid())});
        storeMapping(mapping); startedAt=now; process.start();
        retryAt=now+1000000000ULL;
    }
    ~Impl() override {
        timer.stop(); storeMapping({}); if(mapping) mapping->shared->enabled.store(0);
        if(process.state()!=QProcess::NotRunning) { process.terminate(); if(!process.waitForFinished(200)) { process.kill(); process.waitForFinished(200); } }
    }
};
VirtualCameraRuntime::VirtualCameraRuntime():impl_(std::make_unique<Impl>()) {}
VirtualCameraRuntime::~VirtualCameraRuntime()=default;
}
