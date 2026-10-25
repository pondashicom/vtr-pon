#include "network_video.h"
#include "network_video_addon_api.h"
#include "network_video_input_clock.h"
#include "addon_platform.h"
#include "controller_settings.h"
#include "program_gpu_compositor.h"
#include "uvc_source_session_manager.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QLibrary>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>
#include <QUuid>
#include <QScopeGuard>
#include <QDebug>
#include <QProcess>
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <array>
#include <cmath>
#include <functional>

using namespace vtrpon;
namespace {
bool check(bool value,const char *label) { qInfo() << (value ? "PASS" : "FAIL") << label;return value; }
void pump(int ms) { QElapsedTimer t;t.start();while(t.elapsed()<ms) { QApplication::processEvents();QThread::msleep(3); } }
bool wait(const std::function<bool()> &f,int ms=5000) { QElapsedTimer t;t.start();while(t.elapsed()<ms) { if(f())return true;pump(10); }return false; }
struct Capture { QByteArray bytes;VtrPonNetworkVideoFrame v{};VtrPonNetworkAudioFrame a{}; };
void video(void *p,const VtrPonNetworkVideoFrame *v) { auto &c=*static_cast<Capture *>(p);c.v=*v;c.bytes=QByteArray(reinterpret_cast<const char *>(v->data),v->stride*v->height); }
void audio(void *p,const VtrPonNetworkAudioFrame *a) { auto &c=*static_cast<Capture *>(p);c.a=*a;c.bytes=QByteArray(reinterpret_cast<const char *>(a->data),a->samples*a->channels*sizeof(float)); }
bool color(const Capture &c,QColor expected,int tolerance=8) {
    if (c.v.width!=1920 || c.v.height!=1080 || c.bytes.size()<c.v.stride*c.v.height) return false;
    const auto *p=reinterpret_cast<const uint8_t *>(c.bytes.constData())+540*c.v.stride+960*4;
    return std::abs(p[0]-expected.blue())<=tolerance && std::abs(p[1]-expected.green())<=tolerance && std::abs(p[2]-expected.red())<=tolerance && std::abs(p[3]-expected.alpha())<=tolerance;
}
bool clockChecks() {
    NetworkVideoInputClock c;
    const auto v=c.map(10000000,50000000,false), a=c.map(10100000,50500000,true);
    bool ok=check(a.local-v.local==100000 && a.era==v.era,"source timecode preserves A/V offset despite arrival jitter");
    const auto resumed=c.map(100,90000000,false), resumedAudio=c.map(100100,90150000,true);
    ok &= check(resumed.era>v.era && resumedAudio.local-resumed.local==100000,"remote epoch reset rebases both streams together");
    ok &= check(!c.map(INT64_MAX,100000000,false).timed,"synthesized/unknown timestamp uses bounded arrival fallback");
    return ok;
}
}
namespace {
// Real-runtime transport checks cross a process boundary. Fixture contracts
// remain separate and must never stand in for this exchange.
int peerSender(const QStringList &args) {
    if (args.size()<5) return 2;
    QLibrary library(args[2]);
    auto get=reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
    const auto *api=get?get(2):nullptr;if(!api)return 2;
    char error[1024]{};void *ctx=api->create(error,sizeof(error));if(!ctx){qCritical()<<error;return 2;}
    const auto cleanup=qScopeGuard([&]{api->destroy(ctx);});
    const auto name=args[3].toUtf8();void *sender=api->createSender(ctx,name.constData());if(!sender)return 2;
    const auto senderCleanup=qScopeGuard([&]{api->destroySender(ctx,sender);});
    std::cout<<api->senderName(ctx,sender)<<std::endl;
    const bool blue=args[4]=="blue";
    std::array<uint8_t,64*36*4> pixels{};
    // Distinct hues identify the sending process generation. RGB fidelity of
    // the lossy vendor codec is separate from transport/reconnection checks.
    for(size_t n=0;n<pixels.size();n+=4){pixels[n]=blue?160:32;pixels[n+1]=blue?48:160;pixels[n+2]=blue?32:48;pixels[n+3]=128;}
    std::array<float,960> pcm{};
    for(size_t n=0;n<pcm.size();n+=2){pcm[n]=blue?.5F:.25F;pcm[n+1]=-.125F;}
    QElapsedTimer elapsed;elapsed.start();int frame=0;
    while(elapsed.elapsed()<45000){
        pollfd input{STDIN_FILENO,POLLIN,0};if(::poll(&input,1,0)>0)break;
        const auto timecode=networkVideoTimecode();
        if(frame++%3==0){VtrPonNetworkVideoFrame v{pixels.data(),64,36,256,100,3,1,timecode};api->sendVideo(ctx,sender,&v);}
        VtrPonNetworkAudioFrame a{pcm.data(),48000,2,480,timecode};api->sendAudio(ctx,sender,&a);
        QThread::msleep(10);
    }
    return 0;
}
int wireChecks(const QStringList &args) {
    if(args.size()<3)return 2;
    QLibrary library(args[2]);
    auto get=reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
    const auto *api=get?get(2):nullptr;if(!api)return 2;
    char error[1024]{};void *ctx=api->create(error,sizeof(error));if(!ctx){qCritical()<<error;return 2;}
    const auto contextCleanup=qScopeGuard([&]{api->destroy(ctx);});
    QProcess peer;peer.setProcessChannelMode(QProcess::SeparateChannels);
    const auto name="VTR-PON Mac Wire "+QUuid::createUuid().toString(QUuid::Id128).left(8);
    const auto stopPeer=[&]{if(peer.state()!=QProcess::NotRunning){peer.write("stop\n");peer.closeWriteChannel();if(!peer.waitForFinished(5000)){peer.kill();peer.waitForFinished(1000);}}};
    const auto peerCleanup=qScopeGuard(stopPeer);
    auto start=[&](const QString &color){
        peer.start(QCoreApplication::applicationFilePath(),{"--peer",args[2],name,color});
        if(!peer.waitForStarted(2000))return QString();
        QByteArray line;QElapsedTimer t;t.start();
        while(t.elapsed()<5000 && !line.contains('\n')){peer.waitForReadyRead(100);line+=peer.readAllStandardOutput();}
        return QString::fromUtf8(line).trimmed();
    };
    const auto source=start("green");if(!check(!source.isEmpty(),"independent NDI sender process starts"))return 2;
    qInfo()<<"wire source"<<source;
    struct Found {QString name;bool found=false;} found{source};
    const auto callback=+[](void *p,const char *name){auto &v=*static_cast<Found *>(p);if(QString::fromUtf8(name)==v.name)v.found=true;};
    bool ok=check(wait([&]{api->findSources(ctx,callback,&found);return found.found;},12000),"real finder discovers another process");
    auto utf8=source.toUtf8();auto *vr=api->createReceiver(ctx,utf8.constData(),false), *ar=api->createReceiver(ctx,utf8.constData(),true);
    if(!vr||!ar)return 2;
    const auto receivers=qScopeGuard([&]{api->destroyReceiver(ctx,vr);api->destroyReceiver(ctx,ar);});
    auto verify=[&](bool blue){Capture v,a;int lastV=0,lastA=0;const bool passed=wait([&]{
        lastV=api->receive(ctx,vr,false,video,audio,&v,20);lastA=api->receive(ctx,ar,true,video,audio,&a,20);
        if(v.v.width!=64||v.v.height!=36||v.bytes.size()<64*36*4||a.bytes.size()<2*int(sizeof(float)))return false;
        auto *p=reinterpret_cast<const uint8_t *>(v.bytes.constData())+(18*64+32)*4;
        auto *f=reinterpret_cast<const float *>(a.bytes.constData());
        const bool generation = blue ? p[0]>p[1]+64 && p[0]>p[2]+64
                                     : p[1]>p[0]+64 && p[1]>p[2]+64;
        return v.v.hasAlpha && generation && std::abs(p[3]-128)<=2
            && std::abs(f[0]-(blue?.5F:.25F))<.02F && std::abs(f[1]+.125F)<.02F;
    },12000);
        qInfo()<<"wire capture"<<blue<<v.v.width<<v.v.height<<v.v.hasAlpha<<v.bytes.mid((18*64+32)*4,4).toHex()<<a.a.samples<<a.bytes.left(8).toHex()<<lastV<<lastA<<"peer"<<peer.state()<<peer.exitCode()<<peer.readAllStandardError().left(1024);
        return passed;
    };
    ok &= check(verify(false),"video alpha and stereo audio cross a real process boundary");
    stopPeer();pump(1500);const auto restarted=start("blue");
    ok &= check(restarted==source,"sender restarts with the same exact NDI name");
    ok &= check(verify(true),"existing NDI receivers recover new video and audio after peer restart");
    stopPeer();qInfo()<<"Mac NDI cross-process test:"<<(ok?"PASS":"FAIL");return ok?0:1;
}
}

int main(int argc,char **argv) {
    QApplication app(argc,argv);gst_init(&argc,&argv);
    if (app.arguments().value(1)=="--peer") return peerSender(app.arguments());
    if (app.arguments().value(1)=="--wire") return wireChecks(app.arguments());
    if (app.arguments().contains("--clock")) return clockChecks()?0:1;
    const auto args=app.arguments();if(args.size()<2) return 2;
    QTemporaryDir temporary;QStandardPaths::setTestModeEnabled(true);
    app.setProperty("vtrpon.settingsFile",temporary.filePath("user.ini"));
    const QString installed=networkVideoAddonPath();
    if (!check(!QFileInfo::exists(installed),"isolated test add-on location is empty")) return 2;
    QDir().mkpath(QFileInfo(installed).absolutePath());
    if (!QFile::copy(args[1],installed)) return 2;
    const auto cleanup=qScopeGuard([&] { QFile::remove(installed);QDir().rmdir(QFileInfo(installed).absolutePath());QDir().rmdir(addons::installationDirectory(QCoreApplication::applicationDirPath())); });
    QLibrary library(installed);
    const auto get=reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
    const auto *api=get?get(2):nullptr;
    if (!api) { qCritical()<<library.errorString();return 2; }
    char error[1024]{};void *ctx=api->create(error,sizeof(error));if(!ctx) { qCritical()<<error;return 2; }
    const auto contextCleanup=qScopeGuard([&] { api->destroy(ctx); });
    const auto suffix=QUuid::createUuid().toString(QUuid::Id128).left(10);
    auto inputName=("Mac NDI Input "+suffix).toUtf8();
    void *input=api->createSender(ctx,inputName.constData());if(!input)return 2;
    auto inputCleanup=qScopeGuard([&] { if(input)api->destroySender(ctx,input); });
    const QString sourceName=QString::fromUtf8(api->senderName(ctx,input));
    const QString path=networkVideoSourcePath(sourceName);
    NetworkVideoSettings settings;settings.inputEnabled=true;settings.outputEnabled=true;settings.outputName="Mac NDI Output "+suffix;
    settings.fillEnabled=settings.keyEnabled=settings.alphaEnabled=true;
    saveNetworkVideoSettings(settings);
    NetworkVideoRuntime runtime;
    const auto runtimeCleanup=qScopeGuard([&] { runtime.requestShutdown();wait([&]{return runtime.shutdownComplete();}); });
    bool ok=check(wait([&]{return networkVideoState().inputRunning && networkVideoOutputMask()==15;}),"finder and all four output senders start");
    ok &= check(wait([&]{for(const auto &s:networkVideoState().sources)if(s.path==path)return true;return false;}),"external source discovered");
    std::array<void *,4> receivers{};
    for(unsigned i=0;i<4;++i) {
        const auto name=networkVideoState().outputs[i].name.toUtf8();
        ok &= check(!networkVideoInputAllowed(networkVideoSourcePath(QString::fromUtf8(name))),"own output cannot be selected as input");
        receivers[i]=api->createReceiver(ctx,name.constData(),false);ok &= receivers[i]!=nullptr;
    }
    const auto receiverCleanup=qScopeGuard([&]{for(auto *r:receivers)if(r)api->destroyReceiver(ctx,r);});
    QWidget output,monitor;output.resize(480,270);monitor.resize(320,180);output.show();monitor.show();pump(100);
    ProgramGpuCompositor compositor(output.winId(),monitor.winId());
    if(!check(compositor.start(),"production Metal compositor starts"))return 2;
    const auto compositorCleanup=qScopeGuard([&]{compositor.prepareShutdown();compositor.stop();});
    GError *parse=nullptr;
    auto *pipeline=gst_parse_launch("appsrc name=input is-live=true format=time do-timestamp=true ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=60/1 ! appsink name=frames max-buffers=2 drop=true sync=true async=false wait-on-eos=false",&parse);
    if(!pipeline||parse)return 2;
    auto *src=gst_bin_get_by_name(GST_BIN(pipeline),"input"), *sink=gst_bin_get_by_name(GST_BIN(pipeline),"frames");
    const auto pipelineCleanup=qScopeGuard([&]{gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(src);gst_object_unref(sink);gst_object_unref(pipeline);});
    compositor.configureDeckInput(0,pipeline,sink);compositor.setDeckLayers(0,1,-1,0);compositor.setBackgroundColor(0x0000ff);
    gst_element_set_state(pipeline,GST_STATE_PLAYING);
    QByteArray image(1920*1080*4,'\0');auto *bytes=reinterpret_cast<uint8_t *>(image.data());
    for(qsizetype n=0;n<image.size();n+=4){bytes[n+2]=255;bytes[n+3]=128;}
    auto *buffer=gst_buffer_new_allocate(nullptr,image.size(),nullptr);gst_buffer_fill(buffer,0,image.constData(),image.size());gst_app_src_push_buffer(GST_APP_SRC(src),buffer);
    ok &= check(wait([&]{return compositor.deckFrameReady(0);}),"source frame reaches Metal");pump(120);
    const std::array<QColor,4> expected{QColor(128,0,127,255),QColor(255,0,0,255),QColor(128,128,128,255),QColor(255,0,0,128)};
    std::array<Capture,4> capture;
    for(unsigned i=0;i<4;++i) ok &= check(wait([&]{api->receive(ctx,receivers[i],false,video,audio,&capture[i],10);return color(capture[i],expected[i]);}),"Composite/Fill/Key/Alpha pixels come from the same Metal rendering");
    const auto still=compositor.captureStillFrame()();
    ok &= check(!still.isNull() && std::abs(still.pixelColor(960,540).red()-128)<=2,"local final frame retains normal background composite");
    compositor.setItemFadeProgress(.5F);pump(80);
    ok &= check(wait([&]{api->receive(ctx,receivers[3],false,video,audio,&capture[3],10);return color(capture[3],QColor(255,0,0,64));}),"item fade scales alpha once");
    compositor.setFtbCoverage(.5F);pump(80);
    ok &= check(wait([&]{api->receive(ctx,receivers[0],false,video,audio,&capture[0],10);return color(capture[0],QColor(32,0,223,255));}),"item fade and partial FTB preserve the normal background composite");
    ok &= check(wait([&]{api->receive(ctx,receivers[3],false,video,audio,&capture[3],10);return color(capture[3],QColor(255,0,0,32));}),"item fade and partial FTB preserve straight alpha output");
    compositor.setItemFadeProgress(1);
    compositor.setFtbCoverage(1);pump(100);
    ok &= check(wait([&]{api->receive(ctx,receivers[3],false,video,audio,&capture[3],10);return color(capture[3],QColor(0,0,0,0));}),"FTB removes alpha and RGB together");
    compositor.setFtbCoverage(0);settings.premultipliedFill=true;saveNetworkVideoSettings(settings);
    ok &= check(wait([&]{return networkVideoOutputMask()==15;}),"output generation switches after settings change");
    for(auto *r:receivers)if(r)api->destroyReceiver(ctx,r);receivers={};
    for(unsigned i=0;i<4;++i)receivers[i]=api->createReceiver(ctx,networkVideoState().outputs[i].name.toUtf8().constData(),false);
    ok &= check(wait([&]{api->receive(ctx,receivers[1],false,video,audio,&capture[1],10);return color(capture[1],QColor(128,0,0,255));}),"premultiplied Fill preserves source coverage");
    // Match frames by sender timecode, independent of receiver queue depth.
    std::array<QSet<qint64>,2> pairStamps;
    ok &= check(wait([&]{
        for(unsigned lane=0;lane<2;++lane){Capture c;api->receive(ctx,receivers[lane+1],false,video,audio,&c,10);if(c.v.timecode)pairStamps[lane].insert(c.v.timecode);}
        for(auto stamp:pairStamps[0])if(pairStamps[1].contains(stamp))return true;
        return false;
    }),"Fill and Key retain matching frame timecodes");
    // Real shared input session, plus an independently clocked audio receiver.
    UvcSourceSessionManager manager(false);manager.setRegisteredDevicePaths({path});manager.setAvailableDevicePaths({path});manager.setPreviewDevicePaths({path});
    auto *audioPipeline=gst_parse_launch("appsrc name=input is-live=true format=time do-timestamp=true block=false ! appsink name=frames max-buffers=2 drop=true sync=false async=false wait-on-eos=false",nullptr);
    auto *audioSrc=gst_bin_get_by_name(GST_BIN(audioPipeline),"input"),*audioSink=gst_bin_get_by_name(GST_BIN(audioPipeline),"frames");
    gst_element_set_state(audioPipeline,GST_STATE_PLAYING);
    auto audioReceiver=std::make_unique<NetworkVideoReceiver>(path,audioSrc,true);
    const auto inputPipelineCleanup=qScopeGuard([&]{audioReceiver.reset();manager.shutdown();gst_element_set_state(audioPipeline,GST_STATE_NULL);gst_object_unref(audioSrc);gst_object_unref(audioSink);gst_object_unref(audioPipeline);});
    std::array<uint8_t,64*36*4> inputPixels{};for(size_t n=0;n<inputPixels.size();n+=4){inputPixels[n+1]=255;inputPixels[n+3]=128;}
    std::array<float,960> pcm{};for(size_t n=0;n<pcm.size();n+=2){pcm[n]=.25F;pcm[n+1]=-.125F;}
    auto inputEpoch=networkVideoTimecode();
    auto send=[&]{const auto stamp=networkVideoTimecode()-inputEpoch+10000000;VtrPonNetworkVideoFrame f{inputPixels.data(),64,36,64*4,60,1,1,stamp};VtrPonNetworkAudioFrame a{pcm.data(),48000,2,480,stamp};api->sendVideo(ctx,input,&f);api->sendAudio(ctx,input,&a);};
    ok &= check(wait([&]{send();return !manager.latestThumbnail(path).isNull();}),"NDI enters the production shared input session");
    GstSample *received=nullptr;
    ok &= check(wait([&]{send();received=gst_app_sink_try_pull_sample(GST_APP_SINK(audioSink),0);return received!=nullptr;}),"audio arrives through the production receiver");
    if(received){GstMapInfo m{};auto *b=gst_sample_get_buffer(received);gst_buffer_map(b,&m,GST_MAP_READ);auto *f=reinterpret_cast<float *>(m.data);ok &= check(m.size==pcm.size()*sizeof(float)&&std::abs(f[0]-.25F)<.02F&&std::abs(f[1]+.125F)<.02F,"stereo PCM channel order and levels");gst_buffer_unmap(b,&m);gst_sample_unref(received);}
    std::array<float,441> mono;mono.fill(.375F);
    ok &= check(wait([&]{const VtrPonNetworkAudioFrame f{mono.data(),44100,1,441,networkVideoTimecode()-inputEpoch+10000000};api->sendAudio(ctx,input,&f);
        auto *sample=gst_app_sink_try_pull_sample(GST_APP_SINK(audioSink),0);if(!sample)return false;
        GstMapInfo m{};gst_buffer_map(gst_sample_get_buffer(sample),&m,GST_MAP_READ);const auto *data=reinterpret_cast<float *>(m.data);
        const bool match=m.size==441*2*sizeof(float)&&std::abs(data[0]-.375F)<.02F&&std::abs(data[1]-.375F)<.02F;
        gst_buffer_unmap(gst_sample_get_buffer(sample),&m);gst_sample_unref(sample);return match;
    }),"audio format change and mono duplication preserve both stereo channels");
    api->destroySender(ctx,input);input=nullptr;pump(1200);
    input=api->createSender(ctx,inputName.constData());inputEpoch=networkVideoTimecode();
    for(size_t n=0;n<inputPixels.size();n+=4){inputPixels[n]=255;inputPixels[n+1]=0;}
    ok &= check(wait([&]{send();const auto thumb=manager.latestThumbnail(path);return !thumb.isNull() && thumb.pixelColor(thumb.width()/2,thumb.height()/2).blue()>200;},8000),"new video arrives after same-source restart and timecode reset");
    while(auto *old=gst_app_sink_try_pull_sample(GST_APP_SINK(audioSink),0))gst_sample_unref(old);
    pcm[0]=.5F;
    ok &= check(wait([&]{send();auto *sample=gst_app_sink_try_pull_sample(GST_APP_SINK(audioSink),0);if(!sample)return false;
        GstMapInfo m{};auto *b=gst_sample_get_buffer(sample);gst_buffer_map(b,&m,GST_MAP_READ);
        const bool changed=m.size>=sizeof(float) && std::abs(*reinterpret_cast<float *>(m.data)-.5F)<.03F;
        gst_buffer_unmap(b,&m);gst_sample_unref(sample);return changed;
    }),"new audio arrives after sender reconnection");
    UvcSourceSessionManager::ProgramSource live;
    ok &= check(wait([&]{send();return manager.bindProgramDeck(0,path,live);}),"shared NDI video binds to the normal deck sink");
    if(live.isValid()) {
        while(auto *old=gst_app_sink_try_pull_sample(GST_APP_SINK(live.appSink),0))gst_sample_unref(old);
        while(auto *old=gst_app_sink_try_pull_sample(GST_APP_SINK(audioSink),0))gst_sample_unref(old);
        send();GstSample *v=nullptr,*a=nullptr;
        const bool samples=wait([&]{if(!v)v=gst_app_sink_try_pull_sample(GST_APP_SINK(live.appSink),0);if(!a)a=gst_app_sink_try_pull_sample(GST_APP_SINK(audioSink),0);return v&&a;});
        ok &= check(samples,"both synchronized input lanes produce samples");
        if(samples){const auto vb=gst_sample_get_buffer(v),ab=gst_sample_get_buffer(a);
            const auto vt=GST_BUFFER_PTS(vb)+gst_element_get_base_time(live.pipeline);
            const auto at=GST_BUFFER_PTS(ab)+gst_element_get_base_time(audioPipeline);
            qInfo()<<"input scheduling delta ns"<<qint64(vt)-qint64(at);
            ok &= check(std::abs(qint64(vt)-qint64(at))<45*GST_MSECOND,"video/audio schedule uses one source clock across pipeline base times");}
        if(v)gst_sample_unref(v);if(a)gst_sample_unref(a);
        manager.releaseProgramDeck(0);
    }
    if(auto inject=reinterpret_cast<void(*)()>(library.resolve("vtrpon_ndi_fixture_error"))) {
        const auto count=reinterpret_cast<unsigned(*)()>(library.resolve("vtrpon_ndi_fixture_receivers"));auto before=count();inject();
        ok &= check(wait([&]{send();return count()>before+1;}),"receiver errors recreate video and audio handles");
        for(size_t n=0;n<inputPixels.size();n+=4){inputPixels[n]=0;inputPixels[n+1]=255;}
        ok &= check(wait([&]{send();const auto thumb=manager.latestThumbnail(path);return !thumb.isNull()&&thumb.pixelColor(thumb.width()/2,thumb.height()/2).green()>200;}),"new video is delivered after receiver error recovery");
        auto slow=reinterpret_cast<void(*)(int)>(library.resolve("vtrpon_ndi_fixture_slow"));slow(1);auto frames=compositor.renderedFrameCount();pump(1000);
        ok &= check(compositor.renderedFrameCount()>frames+15,"blocked network encoding does not stop ON AIR rendering");slow(0);
    }
    // Feed final audio output and receive it from an independent NDI handle.
    auto *audioOut=api->createReceiver(ctx,networkVideoState().outputs[0].name.toUtf8().constData(),true);Capture audioCapture;
    ok &= check(wait([&]{auto *b=gst_buffer_new_allocate(nullptr,sizeof(pcm),nullptr);gst_buffer_fill(b,0,pcm.data(),sizeof(pcm));publishNetworkAudio(b);gst_buffer_unref(b);api->receive(ctx,audioOut,true,video,audio,&audioCapture,10);return audioCapture.a.samples>0;}),"final ON AIR audio publishes through the independent sender");
    api->destroyReceiver(ctx,audioOut);
    settings.outputEnabled=settings.inputEnabled=false;saveNetworkVideoSettings(settings);
    ok &= check(wait([&]{return networkVideoOutputMask()==0;}),"disabled output stops all publications");
    qInfo()<<"Mac NDI test:"<<(ok?"PASS":"FAIL");return ok?0:1;
}
