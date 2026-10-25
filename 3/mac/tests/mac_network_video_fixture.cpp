#include "network_video_addon_api.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// SDK-free fault fixture. Real NDI interoperability is a separate invocation
// of the same host test with the native addon and vendor runtime.
namespace {
struct Channel {
    std::string name;
    bool online = false;
    std::vector<uint8_t> video;
    std::vector<float> audio;
    VtrPonNetworkVideoFrame v{};
    VtrPonNetworkAudioFrame a{};
    uint64_t vs = 0, as = 0;
};
struct Receiver { std::shared_ptr<Channel> channel; uint64_t vs=0, as=0; unsigned epoch=0; };
std::mutex mutex;
std::map<std::string, std::shared_ptr<Channel>> channels;
std::atomic<unsigned> errors{0}, created{0};
std::atomic<bool> slow{false};
void *create(char *, uint32_t) { return new int(1); }
void destroy(void *p) { delete static_cast<int *>(p); }
int find(void *, VtrPonNetworkSourceCallback callback, void *user) {
    std::vector<std::string> names;
    { std::lock_guard lock(mutex); for (const auto &[name,c] : channels) if (c->online) names.push_back(name); }
    for (const auto &name : names) callback(user,name.c_str());
    return 1;
}
void *sender(void *, const char *name) {
    std::lock_guard lock(mutex);
    const std::string key = "Fixture (" + std::string(name) + ")";
    auto &c=channels[key]; if (!c) c=std::make_shared<Channel>();
    c->name=key; c->online=true; c->video.clear(); c->audio.clear();
    return new std::shared_ptr<Channel>(c);
}
void destroySender(void *, void *p) {
    auto *ptr=static_cast<std::shared_ptr<Channel> *>(p);
    { std::lock_guard lock(mutex); (*ptr)->online=false; }
    delete ptr;
}
const char *senderName(void *, void *p) { return (*static_cast<std::shared_ptr<Channel> *>(p))->name.c_str(); }
void sendVideo(void *,void *p,const VtrPonNetworkVideoFrame *f) {
    if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(650));
    std::lock_guard lock(mutex); auto &c=**static_cast<std::shared_ptr<Channel> *>(p);
    c.video.assign(f->data,f->data+size_t(f->stride)*f->height); c.v=*f; ++c.vs;
}
void sendAudio(void *,void *p,const VtrPonNetworkAudioFrame *f) {
    std::lock_guard lock(mutex); auto &c=**static_cast<std::shared_ptr<Channel> *>(p);
    c.audio.assign(f->data,f->data+size_t(f->samples)*f->channels); c.a=*f; ++c.as;
}
void *receiver(void *,const char *name,int) {
    std::lock_guard lock(mutex); auto &c=channels[name]; if (!c) { c=std::make_shared<Channel>();c->name=name; }
    ++created; return new Receiver{c,0,0,errors.load()};
}
void destroyReceiver(void *,void *r) { delete static_cast<Receiver *>(r); }
int receive(void *,void *p,int audio,VtrPonNetworkVideoCallback v,VtrPonNetworkAudioCallback a,void *user,uint32_t ms) {
    auto &r=*static_cast<Receiver *>(p);
    if (r.epoch != errors.load()) { r.epoch=errors;return -1; }
    std::vector<uint8_t> pixels;std::vector<float> pcm; VtrPonNetworkVideoFrame vf{};VtrPonNetworkAudioFrame af{};
    { std::lock_guard lock(mutex);auto &c=*r.channel;
      if (c.online && !audio && !c.video.empty() && c.vs!=r.vs) { pixels=c.video;vf=c.v;r.vs=c.vs; }
      if (c.online && audio && !c.audio.empty() && c.as!=r.as) { pcm=c.audio;af=c.a;r.as=c.as; }
    }
    if (!pixels.empty()) { vf.data=pixels.data();v(user,&vf);return 1; }
    if (!pcm.empty()) { af.data=pcm.data();a(user,&af);return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms,10u)));return 0;
}
const VtrPonNetworkVideoApi api{2,sizeof(VtrPonNetworkVideoApi),"vtr-pon-network-video-addon",create,destroy,find,sender,destroySender,senderName,sendVideo,sendAudio,receiver,destroyReceiver,receive};
}
extern "C" __attribute__((visibility("default"))) const VtrPonNetworkVideoApi *vtrpon_network_video_get_api(uint32_t v) { return v==2 ? &api : nullptr; }
extern "C" __attribute__((visibility("default"))) void vtrpon_ndi_fixture_error() { ++errors; }
extern "C" __attribute__((visibility("default"))) unsigned vtrpon_ndi_fixture_receivers() { return created; }
extern "C" __attribute__((visibility("default"))) void vtrpon_ndi_fixture_slow(int value) { slow=value; }
