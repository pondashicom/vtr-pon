#include "vst_service.h"
#include "vst_state.h"
#include "vst_transport_mac.h"
#include "vst_deferred_release.h"
#include <pthread/qos.h>
#include "audio_effects.h"
#include "audio_equalizer.h"
#include <iostream>
#include "vst_scan.h"
#include "controller_settings.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QThread>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTimer>
#include <cstring>
#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>
using namespace vtrpon;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return false; } } while(false)
template<class F> bool until(F condition, int ms = 6000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < ms) { QCoreApplication::processEvents(); QThread::msleep(5); }
    return condition();
}
QString fixtureUid(int number) { return QStringLiteral("425452504F4E565354335445535400%1").arg(number,2,16,QLatin1Char('0')).toUpper(); }
bool all(const std::vector<float> &pcm, float sample) {
    return std::all_of(pcm.begin(),pcm.end(),[sample](float x){return std::abs(x-sample)<1e-6f;});
}
bool apply(VstService &s, const QString &path, int id, VstSlot slot=VstSlot::Master) {
    s.select(slot,path,fixtureUid(id));
    return until([&]{return !s.applying(slot);},18000) && s.active(slot) && s.selectedUid(slot)==fixtureUid(id);
}
bool parentDeath(const QString &path, const char *variable, const QString &marker) {
    QProcess parent;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(variable,marker); parent.setProcessEnvironment(environment);
    parent.start(QCoreApplication::applicationFilePath(),{"--orphan-parent",path});
    CHECK(parent.waitForStarted(5000));
    CHECK(until([&]{return QFileInfo::exists(marker);}));
    QFile file(marker); CHECK(file.open(QIODevice::ReadOnly));
    const auto child = file.readAll().trimmed().toLongLong(); CHECK(child>0 && kill(pid_t(child),0)==0);
    parent.kill(); CHECK(parent.waitForFinished(5000));
    const bool exited = until([&]{return kill(pid_t(child),0)!=0;});
    if (!exited) kill(pid_t(child),SIGKILL);
    CHECK(exited); return true;
}
class PcmStrip {
public:
    // Count selects the standalone EQ, which has no VST slot or audibility.
    PcmStrip(const char *name, VstSlot slot, bool audible, double lowDb = 0)
        : effects_(slot != VstSlot::Count) {
        GError *error = nullptr;
        const auto description = QStringLiteral("appsrc name=input format=time caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! %1 name=%2 ! appsink name=output sync=false async=false")
            .arg(effects_ ? QStringLiteral("vtrponaudioeffects") : QStringLiteral("vtrponaudioequalizer"),
                 QString::fromLatin1(name));
        pipeline_ = gst_parse_launch(description.toUtf8().constData(), &error);
        if (error) {
            std::cerr << error->message << '\n'; g_error_free(error);
            if (pipeline_) gst_object_unref(pipeline_);
            pipeline_ = nullptr; return;
        }
        if (!pipeline_) return;
        input_ = gst_bin_get_by_name(GST_BIN(pipeline_), "input");
        output_ = gst_bin_get_by_name(GST_BIN(pipeline_), "output");
        strip_ = gst_bin_get_by_name(GST_BIN(pipeline_), name);
        if (!input_ || !output_ || !strip_) return;
        if (effects_) {
            g_object_set(strip_, "slot", int(slot), "vst-audible", audible, nullptr);
            setAudioEffectEqualizer(strip_, lowDb, 0, 0);
        } else setAudioEqualizer(strip_, lowDb, 0, 0);
        ready_ = gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    }
    ~PcmStrip() {
        if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (strip_) gst_object_unref(strip_);
        if (input_) gst_object_unref(input_);
        if (output_) gst_object_unref(output_);
        if (pipeline_) gst_object_unref(pipeline_);
    }
    bool check(float expected, int warmupBlocks = 0, double tolerance = 0, bool gap = false) {
        if (!ready_) return false;
        for (int block = 0; block <= warmupBlocks; ++block) {
            std::array<float, 960> samples; samples.fill(0.25f);
            auto *buffer = gst_buffer_new_allocate(nullptr, sizeof(samples), nullptr);
            gst_buffer_fill(buffer, 0, samples.data(), sizeof(samples));
            GST_BUFFER_PTS(buffer) = offset_;
            GST_BUFFER_DURATION(buffer) = 10 * GST_MSECOND;
            offset_ += GST_BUFFER_DURATION(buffer);
            if (gap) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_GAP);
            GstFlowReturn flow = GST_FLOW_ERROR;
            g_signal_emit_by_name(input_, "push-buffer", buffer, &flow); gst_buffer_unref(buffer);
            GstSample *sample = nullptr;
            if (flow == GST_FLOW_OK)
                g_signal_emit_by_name(output_, "try-pull-sample", guint64(GST_SECOND), &sample);
            if (!sample) return false;
            auto *received = gst_sample_get_buffer(sample);
            const bool complete = gst_buffer_get_size(received) == sizeof(samples)
                && gst_buffer_extract(received, 0, samples.data(), sizeof(samples)) == sizeof(samples);
            gst_sample_unref(sample);
            if (!complete) return false;
            if (block == warmupBlocks)
                return std::all_of(samples.begin(), samples.end(), [=](float value) {
                    return std::isfinite(value) && std::abs(double(value) - expected) <= tolerance;
                });
        }
        return false;
    }
private:
    GstElement *pipeline_ = nullptr, *input_ = nullptr, *output_ = nullptr, *strip_ = nullptr;
    GstClockTime offset_ = 0;
    bool effects_ = false, ready_ = false;
};

bool deferredRelease() {
    const auto owner=std::this_thread::get_id();
    std::atomic_bool destroyed{false}, onOwner{false}, release{false}, borrowed{false};
    struct Resource {
        std::thread::id owner;
        std::atomic_bool &destroyed, &onOwner;
        ~Resource() { onOwner=std::this_thread::get_id()==owner; destroyed=true; }
    };
    vst::DeferredRelease<Resource> retired;
    auto resource=std::shared_ptr<Resource>(new Resource{owner,destroyed,onOwner});
    std::thread reader([held=resource,&release,&borrowed]() mutable {
        borrowed=true;
        while(!release) std::this_thread::yield();
        held.reset();
    });
    while(!borrowed) std::this_thread::yield();
    retired.retire(resource);
    retired.collect();
    const bool retained=!resource && !retired.empty() && !destroyed;
    release=true; reader.join();
    const bool survivedReader=!destroyed;
    retired.collect();
    CHECK(retained && survivedReader && retired.empty() && destroyed && onOwner);
    std::puts("VST_DEFERRED_RELEASE PASS: last audio reader does not destroy resources");
    return true;
}

bool transportDeadline() {
    CHECK(deferredRelease());
    using Transport = vst::MacTransport;
    for(int mode=0;mode<10;++mode) {
        Transport transport;
        const auto name=QStringLiteral("/vst-time-%1-%2").arg(QCoreApplication::applicationPid()).arg(mode).toUtf8();
        CHECK(transport.create(name.constData()));
        const bool legacy=mode==2 || mode==3, delayedCaller=mode==0 || mode==3 || mode==4 || (mode>=6 && mode<=8), slowWorker=mode==1 || mode==4;
        const auto deadline=Transport::Clock::now()+std::chrono::milliseconds(50);
        std::atomic_bool peerOk{false};
        transport.shared->input[0]=.125f;
        std::thread peer([&] {
            pollfd socket{transport.childSocket,POLLIN,0}; unsigned char byte=0;
            if(poll(&socket,1,1000)!=1 || recv(transport.childSocket,&byte,1,0)!=1 || byte!=1) return;
            if(!legacy) transport.markStarted();
            std::this_thread::sleep_for(std::chrono::milliseconds(slowWorker?80:5));
            transport.shared->output[0]=transport.shared->input[0]*.5f;
            if(!legacy) transport.markResponse();
            // Report a late completion with an already readable reply, so the
            // normal POLLIN path must also enforce the worker's deadline.
            if(mode==5 || mode==6 || mode==7) {
                auto *tag=reinterpret_cast<Transport::Timing *>(reinterpret_cast<unsigned char *>(transport.shared)+offsetof(Transport::Arena,timing));
                // Inject exact clock boundaries without relying on scheduler precision.
                const int64_t finished=mode==5 ? Transport::nowNs()+80000000LL : deadline.time_since_epoch().count()+(mode==7?1:0);
                __atomic_store_n(&tag->finishedNs,finished,__ATOMIC_SEQ_CST);
            }
            if(mode==9) byte=7;
            peerOk=mode==8 || send(transport.childSocket,&byte,1,0)==1;
        });
        transport.markRequest();
        const bool sent=transport.signal();
        if(delayedCaller) std::this_thread::sleep_for(std::chrono::milliseconds(mode==4?120:80));
        const auto result=transport.waitForResponse(deadline);
        peer.join();
        CHECK(sent && peerOk);
        const bool ready=result==Transport::Result::Ready || result==Transport::Result::ReadyAfterCallerDelay;
        const bool expectedReady=mode==0 || mode==2 || mode==6;
        std::printf("TRANSPORT_DEADLINE mode=%d ready=%d expectedReady=%d\n",mode,ready,expectedReady);
        CHECK(ready==expectedReady);
        if(mode==9) CHECK(result==Transport::Result::Failed);
        else if(!expectedReady) CHECK(result==Transport::Result::Timeout);
        if(ready) CHECK(transport.shared->output[0]==.0625f);
        if(mode==0) CHECK(result==Transport::Result::ReadyAfterCallerDelay);
        if(mode==2) CHECK(result==Transport::Result::Ready);
    }
    return true;
}

// Emulate the old host's exact shared-memory length and absent timing tag.
bool legacyHost(const QString &path) {
    vst::MacTransport transport;
    const auto name=QStringLiteral("/vst-old-%1").arg(QCoreApplication::applicationPid()).toUtf8();
    transport.memory=shm_open(name.constData(),O_CREAT|O_EXCL|O_RDWR,0600);
    CHECK(transport.memory>=0);
    CHECK(shm_unlink(name.constData())==0 && ftruncate(transport.memory,sizeof(vst::Shared))==0);
    void *mapped=mmap(nullptr,sizeof(vst::Shared),PROT_READ|PROT_WRITE,MAP_SHARED,transport.memory,0);
    CHECK(mapped!=MAP_FAILED);
    transport.shared=new(mapped) vst::Shared;
    int pair[2]; CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,pair)==0);
    transport.socket=pair[0]; transport.childSocket=pair[1];
    for(int fd:{transport.memory,transport.socket,transport.childSocket}) CHECK(fcntl(fd,F_SETFD,FD_CLOEXEC)==0);
    for(int fd:pair) CHECK(fcntl(fd,F_SETFL,O_NONBLOCK)==0);
    QProcess helper;
    const int memory=transport.memory, socket=transport.childSocket;
    helper.setChildProcessModifier([memory,socket,&helper] {
        if(dup2(memory,STDIN_FILENO)<0 || dup2(socket,STDOUT_FILENO)<0 ||
           fcntl(STDIN_FILENO,F_SETFD,0)!=0 || fcntl(STDOUT_FILENO,F_SETFD,0)!=0)
            helper.failChildProcessModifier("legacy VST descriptors",errno);
    });
    helper.start(VstService::addonPath(),{"--run","fd:0,1","--module",path,"--class",fixtureUid(1),
        "--parent",QString::number(QCoreApplication::applicationPid())});
    const bool started=helper.waitForStarted(5000);
    const bool ready=started && until([&]{return vst::load(&transport.shared->ready)!=0;},18000);
    bool processed=false;
    if(ready) {
        transport.shared->frames=480;
        std::fill_n(transport.shared->input,960,.125f);
        processed=transport.signal() && transport.wait(vst::MacTransport::Clock::now()+std::chrono::milliseconds(50))==vst::MacTransport::Result::Ready
            && !vst::load(&transport.shared->failed)
            && std::all_of(transport.shared->output,transport.shared->output+960,[](float value){return value==.0625f;});
    }
    helper.kill(); helper.waitForFinished(5000);
    CHECK(started && ready && processed);
    std::puts("VST_LEGACY_HOST PASS");
    return true;
}

bool contract(const QString &path) {
    CHECK(transportDeadline());
    CHECK(legacyHost(path));
    QTemporaryDir directory; CHECK(directory.isValid());
    // A .vst3 bundle is one module, even if it contains nested assets.
    const auto link=directory.filePath("test.vst3"); CHECK(QFile::link(path,link));
    auto scan=VstScan::start(VstService::addonPath(),{directory.path()},QCoreApplication::applicationPid());
    CHECK(until([&]{return scan->snapshot().done;},25000));
    CHECK(scan->snapshot().total==1 && scan->snapshot().checked==1 && scan->snapshot().catalog.size()==8);
    CHECK(parentDeath(path,"VTRPON_TEST_VST_LOAD_HANG",directory.filePath("load-ready")));
    CHECK(parentDeath(path,"VTRPON_TEST_VST_SCAN_HANG",directory.filePath("init-ready")));
    qputenv("VTRPON_TEST_VST_SCAN_HANG",directory.filePath("cancel-ready").toUtf8());
    auto cancel=VstScan::start(VstService::addonPath(),{directory.path()},QCoreApplication::applicationPid());
    CHECK(until([&]{return QFileInfo::exists(directory.filePath("cancel-ready"));}));
    cancel->cancel(); qunsetenv("VTRPON_TEST_VST_SCAN_HANG");
    CHECK(until([&]{return cancel->snapshot().done;}));
    auto &s=VstService::instance(); CHECK(s.available() && s.setEnabled(true));
    std::vector<float> pcm(960,.1f);
    for(auto slot : {VstSlot::Edit,VstSlot::Item,VstSlot::Master}) {
        CHECK(apply(s,path,1,slot)); pcm.assign(960,.1f); processVst(slot,pcm.data(),480); CHECK(all(pcm,.05f));
        s.bypass(slot,true); pcm.assign(960,.1f); processVst(slot,pcm.data(),480); CHECK(all(pcm,.1f));
        s.activate(slot); pcm.assign(960,.1f); processVst(slot,pcm.data(),480); CHECK(all(pcm,.05f));
    }
    CHECK(PcmStrip("mac-edit",VstSlot::Edit,false,-12).check(.125f));
    CHECK(PcmStrip("mac-master",VstSlot::Master,false,-12).check(.125f));
    CHECK(PcmStrip("mac-onair",VstSlot::Item,true,-12).check(.125f));
    CHECK(PcmStrip("mac-standby",VstSlot::Item,false,-12).check(.25f));
    CHECK(PcmStrip("mac-gap",VstSlot::Master,false).check(0,0,0,true));
    CHECK(PcmStrip("mac-bgm",VstSlot::Count,false).check(.25f));
    // Opening a native editor must not stop its audio or the other slots.
    s.openEditor(VstSlot::Master,0,60,60);
    QThread::msleep(200); QCoreApplication::processEvents();
    pcm.assign(960,.1f); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,.05f));
    s.select(VstSlot::Master,path,fixtureUid(6));
    CHECK(until([&]{return !s.applying(VstSlot::Master);}));
    CHECK(s.active(VstSlot::Master) && s.selectedUid(VstSlot::Master)==fixtureUid(1) && !s.applyError(VstSlot::Master).isEmpty());
    CHECK(makeControllerSettings().value("audio/vst/master/uid").toString()==fixtureUid(1));
    s.select(VstSlot::Master,path,fixtureUid(7));
    CHECK(s.applying(VstSlot::Master));
    pcm.assign(960,.1f); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,.05f));
    CHECK(until([&]{return !s.applying(VstSlot::Master);}));
    pcm.assign(960,.1f); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,.025f));
    for(int id : {2,3,4}) {
        CHECK(apply(s,path,id));
        // Fail the second sub-block after the first has already been processed.
        pcm.assign(8192*2,.1f); std::fill(pcm.begin()+4096*2,pcm.end(),.4f); const auto dry=pcm;
        QElapsedTimer elapsed; elapsed.start(); processVst(VstSlot::Master,pcm.data(),8192);
        CHECK(pcm==dry && elapsed.elapsed()<180);
        CHECK(until([&]{return s.statusKey(VstSlot::Master)=="vst.failed";}));
        pcm.assign(960,.3f); elapsed.restart(); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,.3f) && elapsed.elapsed()<20);
        CHECK(apply(s,path,1)); pcm.assign(960,.1f); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,.05f));
    }
    // Two healthy 30 ms sub-blocks exceed one 50 ms input-buffer budget.
    // A per-sub-block deadline would incorrectly return the processed PCM.
    CHECK(apply(s,path,8));
    pcm.assign(8192*2,.4f); const auto original=pcm;
    processVst(VstSlot::Master,pcm.data(),8192);
    CHECK(pcm==original);
    CHECK(until([&]{return s.statusKey(VstSlot::Master)=="vst.failed";}));
    CHECK(apply(s,path,1));
    CHECK(apply(s,path,5)); s.openEditor(VstSlot::Master,0,60,60);
    CHECK(until([&]{return s.statusKey(VstSlot::Master)=="vst.failed";},5000));
    CHECK(apply(s,path,1));
    pcm.assign(65537*2,.1f); processVst(VstSlot::Master,pcm.data(),65537); CHECK(all(pcm,.1f));
    CHECK(until([&]{return s.statusKey(VstSlot::Master)=="vst.failed";}));
    for(int i=0;i<8;++i) { CHECK(apply(s,path,i%2 ? 1:7)); pcm.assign(960,.1f); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,i%2?.05f:.025f)); }
    s.select(VstSlot::Master,{},{}); CHECK(!s.active(VstSlot::Master) && s.selectedPath(VstSlot::Master).isEmpty());
    CHECK(s.setEnabled(false));
    CHECK(until([&]{for(auto *p:QCoreApplication::instance()->findChildren<QProcess *>()) if(p->state()!=QProcess::NotRunning) return false; return true;}));
    return true;
}
bool real(const QString &path) {
    auto &s=VstService::instance(); CHECK(s.available() && s.setEnabled(true)); s.scan();
    CHECK(until([&]{return !s.scanning();},25000));
    QString uid;
    for(auto value:s.plugins()) { const auto p=value.toObject(); if(p.value("path").toString()==path) uid=p.value("uid").toString(); }
    CHECK(!uid.isEmpty());
    for(auto slot:{VstSlot::Edit,VstSlot::Item,VstSlot::Master}) {
        s.select(slot,path,uid); CHECK(until([&]{return !s.applying(slot);},18000)); CHECK(s.active(slot));
        double energy=0; std::vector<float> pcm(960);
        for(int block=0;block<100;++block) {
            for(int n=0;n<480;++n) { pcm[n*2]=float(.05*std::sin((block*480+n)*440.*6.283185307179586/48000.)); pcm[n*2+1]=0; }
            processVst(slot,pcm.data(),480);
            for(int n=0;n<480;++n) { CHECK(std::isfinite(pcm[n*2]) && std::abs(pcm[n*2+1])<1e-5f); energy+=pcm[n*2]*pcm[n*2]; }
            QCoreApplication::processEvents();
        }
        CHECK(s.active(slot) && energy>1.);
        s.openEditor(slot,0,80+int(slot)*40,80+int(slot)*40);
        CHECK(until([&]{return s.latency(slot)>0;},1000));
        std::fprintf(stdout,"NOVA slot=%d latency=%d energy=%.5f\n",int(slot),s.latency(slot),energy);
        s.bypass(slot,true); std::fill(pcm.begin(),pcm.end(),.1f); processVst(slot,pcm.data(),480); CHECK(all(pcm,.1f));
        s.bypass(slot,false);
    }
    CHECK(s.saveStates());
    CHECK(s.setEnabled(false));
    CHECK(until([&]{for(auto *p:QCoreApplication::instance()->findChildren<QProcess *>()) if(p->state()!=QProcess::NotRunning) return false; return true;}));
    CHECK(s.setEnabled(true));
    for(auto slot:{VstSlot::Edit,VstSlot::Item,VstSlot::Master}) {
        s.activate(slot); CHECK(until([&]{return !s.applying(slot);},18000)); CHECK(s.active(slot));
        std::vector<float> pcm(960,.05f); processVst(slot,pcm.data(),480);
        CHECK(std::all_of(pcm.begin(),pcm.end(),[](float value){return std::isfinite(value);}));
        std::fprintf(stdout,"NOVA restored slot=%d latency=%d\n",int(slot),s.latency(slot));
    }
    CHECK(s.saveStates()); CHECK(s.setEnabled(false));
    CHECK(until([&]{for(auto *p:QCoreApplication::instance()->findChildren<QProcess *>()) if(p->state()!=QProcess::NotRunning) return false; return true;}));
    return true;
}

// Concurrent, paced PCM with GUI and checkpoint activity; no output device.
bool timing(const QString &path) {
    auto &s = VstService::instance(); CHECK(s.available() && s.setEnabled(true));
    s.scan(); CHECK(until([&]{ return !s.scanning(); },25000));
    QString uid;
    for (auto value : s.plugins()) { const auto p=value.toObject(); if(p.value("path").toString()==path) uid=p.value("uid").toString(); }
    CHECK(!uid.isEmpty());
    for (auto slot : {VstSlot::Edit,VstSlot::Item,VstSlot::Master}) {
        s.select(slot,path,uid); CHECK(until([&]{return !s.applying(slot);},18000)); CHECK(s.active(slot));
    }
    struct Stats { unsigned calls=0; long long maximumUs=0; std::vector<long long> times; };
    std::array<Stats,3> stats;
    int policies=0;
    for(auto *process:s.findChildren<QProcess *>()) {
        const auto args=process->arguments(); const int at=args.indexOf("--state-dir");
        if(at<0 || process->state()!=QProcess::Running) continue;
        const auto statusPath=QDir(args.value(at+1)).filePath("status.json");
        CHECK(until([&]{const auto status=vststate::read(statusPath,4096);
            return status.value("audioQosClass").toInt()==int(QOS_CLASS_USER_INTERACTIVE)
                && status.value("audioQosError").toInt()==0 && status.value("latencyActivity").toBool();}));
        ++policies;
        std::printf("VST_AUDIO_POLICY qos=%d latencyActivity=1\n",int(QOS_CLASS_USER_INTERACTIVE));
    }
    CHECK(policies==3);
    std::atomic_bool stop{false};
    std::vector<std::thread> audio;
    const int seconds = qEnvironmentVariableIsSet("VTRPON_TEST_VST_TIMING_SECONDS")
        ? std::clamp(qEnvironmentVariableIntValue("VTRPON_TEST_VST_TIMING_SECONDS"),30,600) : 90;
    for(int i=0;i<3;++i) audio.emplace_back([&,i] {
        std::array<float,960> pcm{};
        auto due=std::chrono::steady_clock::now();
        while(!stop) {
            for(int n=0;n<480;++n) { pcm[n*2]=float(.05*std::sin((stats[i].calls*480+n)*440.*6.283185307179586/48000.)); pcm[n*2+1]=0; }
            const auto start=std::chrono::steady_clock::now();
            processVst(VstSlot(i),pcm.data(),480);
            const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            stats[i].maximumUs=std::max(stats[i].maximumUs,static_cast<long long>(us));
            stats[i].times.push_back(us); ++stats[i].calls;
            due += std::chrono::milliseconds(10);
            if(due < std::chrono::steady_clock::now()) due=std::chrono::steady_clock::now();
            std::this_thread::sleep_until(due);
        }
    });
    std::vector<std::thread> load;
    const int loadThreads=std::clamp(qEnvironmentVariableIntValue("VTRPON_TEST_VST_TIMING_LOAD_THREADS"),0,8);
    for(int i=0;i<loadThreads;++i) load.emplace_back([&] {
        double value=.123;
        while(!stop) { for(int j=0;j<10000;++j) value=std::sin(value)+.123; std::atomic_signal_fence(std::memory_order_seq_cst); }
        if(value==0) std::fputs("load completed\n",stdout);
    });
    int failures=0, checkpoints=0, failedCheckpoints=0;
    const auto connection=QObject::connect(&s,&VstService::failure,&s,[&](const QString &slot,const QString &reason) {
        ++failures; std::fprintf(stderr,"TIMING_FAILURE slot=%s %s\n",qPrintable(slot),qPrintable(reason));
    });
    QElapsedTimer elapsed, checkpoint; elapsed.start();checkpoint.start();
    bool opened=false;
    while(elapsed.elapsed()<seconds*1000 && failures==0) {
        QCoreApplication::processEvents();
        if(!opened && elapsed.elapsed()>5000) { s.openEditor(VstSlot::Master,0,80,80);opened=true; }
        if(checkpoint.elapsed()>2000) { ++checkpoints;if(!s.saveStates()) ++failedCheckpoints;checkpoint.restart(); }
        QThread::msleep(2);
    }
    stop=true; for(auto &t:audio)t.join(); for(auto &t:load)t.join();
    QElapsedTimer settle;settle.start();
    while(settle.elapsed()<150) {QCoreApplication::processEvents();QThread::msleep(2);}
    const bool healthy=s.active(VstSlot::Edit) && s.active(VstSlot::Item) && s.active(VstSlot::Master);
    QObject::disconnect(connection);
    for(int i=0;i<3;++i) {
        auto &v=stats[i].times;std::sort(v.begin(),v.end());
        std::printf("VST_TIMING_RESULT slot=%d calls=%u maxUs=%lld p99Us=%lld\n",i,stats[i].calls,stats[i].maximumUs,v.empty()?0:v[v.size()*99/100]);
    }
    std::printf("VST_TIMING_SUMMARY elapsedMs=%lld failures=%d checkpoints=%d failedCheckpoints=%d loadThreads=%d\n",elapsed.elapsed(),failures,checkpoints,failedCheckpoints,loadThreads);
    const bool ok = healthy && failures==0 && checkpoints>0 && failedCheckpoints==0;
    CHECK(s.setEnabled(false));
    CHECK(until([&]{for(auto *p:QCoreApplication::instance()->findChildren<QProcess *>()) if(p->state()!=QProcess::NotRunning)return false;return true;}));
    return ok;
}

QString stateFile(const QString &path, VstSlot slot = VstSlot::Master) {
    const QString names[]{"edit","item","master"};
    return vststate::savedFile(makeControllerSettings().fileName(), names[int(slot)], path, fixtureUid(1));
}
qint64 fixturePid(VstService &s) {
    qint64 result = 0;
    for (auto *process : s.findChildren<QProcess *>())
        if (process->state() == QProcess::Running) result = std::max(result, process->processId());
    return result;
}
bool editFixture(qint64 pid, int serial, double gain, double hidden, int marker, int fault = 0) {
    const auto directory = qEnvironmentVariable("VTRPON_TEST_VST_STATE_CONTROL_DIR");
    const auto prefix = directory + QLatin1Char('/') + QString::number(pid);
    QSaveFile file(prefix + ".cmd");
    CHECK(file.open(QIODevice::WriteOnly));
    const auto text = QStringLiteral("%1 %2 %3 %4 %5\n").arg(serial).arg(gain,0,'g',12).arg(hidden,0,'g',12).arg(marker).arg(fault).toUtf8();
    CHECK(file.write(text) == text.size() && file.commit());
    CHECK(until([&] { QFile ack(prefix + ".edited"); return ack.open(QIODevice::ReadOnly) && ack.readAll().trimmed().toInt() == serial; }));
    return true;
}
bool savedValues(const QString &path, double gain, double hidden) {
    const auto state = vststate::read(path); QByteArray bytes;
    if (!vststate::decode(state.value("component"),bytes) || bytes.size() != 24) return false;
    double actual = 0, privateValue = 0;
    std::memcpy(&actual,bytes.constData()+8,8); std::memcpy(&privateValue,bytes.constData()+16,8);
    return std::abs(actual-gain) < 1e-10 && std::abs(privateValue-hidden) < 1e-10;
}
QByteArray contents(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool stateSession(const QString &path, bool writing) {
    auto &s = VstService::instance(); CHECK(s.setEnabled(true));
    for (auto slot : {VstSlot::Edit,VstSlot::Item,VstSlot::Master}) {
        CHECK(apply(s,path,1,slot));
        const auto pid = fixturePid(s); CHECK(pid > 0);
        const double gain = .2 + int(slot)*.1, hidden = .8 - int(slot)*.1;
        if (writing) {
            // Autosave must work with no audio callbacks and with bypass on.
            s.bypass(slot,true);
            CHECK(editFixture(pid,1,gain+.05,hidden,40+int(slot)));
            CHECK(until([&]{ return savedValues(stateFile(path,slot),gain+.05,hidden); },4000));
            // Last edit is acknowledged immediately before application quit.
            CHECK(editFixture(pid,2,gain,hidden,70+int(slot)));
        } else {
            std::vector<float> pcm(960,.1f); processVst(slot,pcm.data(),480);
            CHECK(all(pcm,float(.1*gain*hidden)));
            const auto marker = qEnvironmentVariable("VTRPON_TEST_VST_STATE_CONTROL_DIR") + QLatin1Char('/') + QString::number(pid) + ".restored";
            CHECK(until([&]{return contents(marker).trimmed().toInt() == 70+int(slot);}));
        }
    }
    return true;
}
bool persistence(const QString &path) {
    QTemporaryDir directory; CHECK(directory.isValid());
    qputenv("VTRPON_TEST_VST_STATE_CONTROL_DIR",directory.path().toUtf8());
    const auto ini = directory.filePath("session.ini");
    for (const auto &mode : {QStringLiteral("--state-write"),QStringLiteral("--state-read")}) {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),{mode,path,ini});
        CHECK(child.waitForStarted(5000));
        CHECK(until([&]{return child.state()==QProcess::NotRunning;},25000));
        std::fputs(child.readAllStandardError().constData(),stderr);
        CHECK(child.exitStatus()==QProcess::NormalExit && child.exitCode()==0);
    }
    auto &s = VstService::instance(); CHECK(s.setEnabled(true));
    CHECK(apply(s,path,1)); auto pid = fixturePid(s);
    CHECK(editFixture(pid,1,.31,.9,123)); CHECK(s.saveStates());
    const auto saved = stateFile(path); const auto good = contents(saved); CHECK(!good.isEmpty() && savedValues(saved,.31,.9));
    // Immediate re-selection must use the final retiring checkpoint, even
    // before the debounce timer or the audio thread has applied this edit.
    CHECK(editFixture(pid,2,.33,.8,125));
    s.select(VstSlot::Master,{},{});
    CHECK(apply(s,path,1)); pid = fixturePid(s);
    std::vector<float> switched(960,.1f); processVst(VstSlot::Master,switched.data(),480);
    CHECK(all(switched,.0264f));
    CHECK(editFixture(pid,1,.31,.9,123)); CHECK(s.saveStates());
    const auto switchGood = contents(saved);
    // A rejected replacement must preserve the running instance and its state.
    s.select(VstSlot::Master,path,fixtureUid(6));
    CHECK(until([&]{return !s.applying(VstSlot::Master);}));
    CHECK(s.active(VstSlot::Master) && contents(saved)==switchGood);
    // Read errors and bounded-stream overflow retain the previous good file.
    for (int fault : {1,3}) {
        CHECK(editFixture(pid,fault+1,.6,.5,200+fault,fault));
        CHECK(!s.saveStates()); CHECK(contents(saved)==switchGood);
    }
    CHECK(editFixture(pid,5,.31,.9,123)); CHECK(s.saveStates());
    const auto beforeWriteFailure = contents(saved);
    // Deterministic write failure, independent of filesystem/root permissions.
    const auto folder = QFileInfo(saved).absolutePath(); CHECK(QDir().rename(folder,folder+".held"));
    { QFile obstruction(folder); CHECK(obstruction.open(QIODevice::WriteOnly)); }
    CHECK(editFixture(pid,6,.32,.9,124)); CHECK(!s.saveStates());
    CHECK(contents(folder+".held/"+QFileInfo(saved).fileName())==beforeWriteFailure);
    CHECK(QFile::remove(folder) && QDir().rename(folder+".held",folder)); CHECK(s.saveStates());
    // A hung getState is isolated and the all-slot shutdown wait stays bounded.
    const auto beforeHang = contents(saved);
    CHECK(editFixture(pid,7,.8,.5,999,2));
    QElapsedTimer wait; wait.start(); CHECK(!s.saveStates()); CHECK(wait.elapsed()<1800);
    CHECK(contents(saved)==beforeHang);
    CHECK(until([&]{return s.statusKey(VstSlot::Master)=="vst.failed";},5000));
    std::vector<float> pcm(960,.1f); processVst(VstSlot::Master,pcm.data(),480); CHECK(all(pcm,.1f));
    // Invalid or rejected state fails the candidate and stays intact until reset.
    auto bad = vststate::read(saved); bad.insert("uid",fixtureUid(7)); CHECK(vststate::write(saved,bad));
    const auto corrupted = contents(saved);
    s.select(VstSlot::Master,path,fixtureUid(1)); CHECK(until([&]{return !s.applying(VstSlot::Master);}));
    CHECK(!s.active(VstSlot::Master) && !s.applyError(VstSlot::Master).isEmpty() && contents(saved)==corrupted);
    bad = vststate::read(saved); bad.insert("uid",fixtureUid(1));
    bad.insert("component",vststate::blob(QByteArray(24,'x'))); CHECK(vststate::write(saved,bad));
    const auto rejectedState = contents(saved);
    s.select(VstSlot::Master,path,fixtureUid(1)); CHECK(until([&]{return !s.applying(VstSlot::Master);}));
    CHECK(!s.active(VstSlot::Master) && contents(saved)==rejectedState);
    s.select(VstSlot::Master,path,fixtureUid(1),true); CHECK(until([&]{return !s.applying(VstSlot::Master);}));
    CHECK(s.active(VstSlot::Master) && s.saveStates());
    CHECK(savedValues(saved,.5,1));
    CHECK(s.setEnabled(false));
    CHECK(until([&]{return fixturePid(s)==0;}));
    qunsetenv("VTRPON_TEST_VST_STATE_CONTROL_DIR");
    return true;
}
int main(int argc,char **argv) {
    QApplication app(argc,argv); QTemporaryDir settings;
    const bool session = app.arguments().contains("--state-write") || app.arguments().contains("--state-read");
    app.setProperty("vtrpon.settingsFile",session ? app.arguments().value(3) : settings.filePath("user.ini"));
    if(app.arguments().contains("--orphan-parent")) {
        QProcess scanner; scanner.start(VstService::addonPath(),{"--scan",app.arguments().value(2),"--result",settings.filePath("scan.json"),"--parent",QString::number(QCoreApplication::applicationPid())});
        return app.exec();
    }
    gst_init(nullptr,nullptr);
    if(!registerAudioEffects() || !registerAudioEqualizer()) return 1;
    if (session) {
        bool ok = false;
        QTimer::singleShot(0,&app,[&] {
            ok = stateSession(app.arguments().value(2),app.arguments().contains("--state-write"));
            app.quit();
        });
        app.exec(); return ok ? 0 : 1;
    }
    bool ok=app.arguments().contains("--transport") ? transportDeadline()
        : app.arguments().contains("--timing") ? timing(app.arguments().value(2))
        : app.arguments().contains("--real") ? real(app.arguments().value(2))
        : app.arguments().contains("--state") ? persistence(app.arguments().value(2)) : contract(app.arguments().value(1));
    std::puts(ok?"Mac VST3 PASS":"Mac VST3 FAIL"); return ok?0:1;
}
