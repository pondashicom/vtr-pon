// Isolated distribution-candidate checks. No physical audio output is opened.
#include "deck_source_session.h"
#include "gpu_configuration.h"
#include "mac_media_runtime.h"
#include "media_duration_probe.h"
#include "program_gpu_compositor.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLibrary>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QWidget>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <mach-o/dyld.h>
#include <cmath>
#include <functional>
#include <mutex>

namespace {
bool check(bool value, const QString &label) {
    qInfo().noquote() << (value ? "PASS" : "FAIL") << label;
    return value;
}
bool waitFor(const std::function<bool()> &condition, int timeout=8000) {
    QElapsedTimer timer; timer.start();
    do { QApplication::processEvents(); if(condition()) return true; QThread::msleep(10); }
    while(timer.elapsed()<timeout);
    return false;
}
struct Samples {
    std::mutex mutex;
    qint64 frames=0,redFrames=0, audioFrames=0, invalid=0;
    double squares[2]={},crossings[2]={};
    bool armed[2]={};
    GstClockTime lastVideoPts=0;
    QStringList decoders;
};
GstFlowReturn videoSample(GstAppSink *sink,gpointer data) {
    auto *sample=gst_app_sink_pull_sample(sink); if(!sample) return GST_FLOW_EOS;
    auto &s=*static_cast<Samples *>(data); GstVideoInfo info; GstVideoFrame frame;
    if(gst_video_info_from_caps(&info,gst_sample_get_caps(sample)) &&
       gst_video_frame_map(&frame,&info,gst_sample_get_buffer(sample),GST_MAP_READ)) {
        const auto *pixel=static_cast<const guint8 *>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0))+
            (info.height/2)*GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0)+(info.width/2)*4;
        std::lock_guard lock(s.mutex); ++s.frames;
        if(pixel[2]>180 && pixel[1]<85 && pixel[0]<85 && pixel[3]==255) ++s.redFrames;
        auto pts=GST_BUFFER_PTS(gst_sample_get_buffer(sample));
        if(GST_CLOCK_TIME_IS_VALID(pts)) s.lastVideoPts=std::max(s.lastVideoPts,pts);
        gst_video_frame_unmap(&frame);
    }
    gst_sample_unref(sample); return GST_FLOW_OK;
}
GstFlowReturn audioSample(GstAppSink *sink,gpointer data) {
    auto *sample=gst_app_sink_pull_sample(sink); if(!sample) return GST_FLOW_EOS;
    auto &s=*static_cast<Samples *>(data); GstMapInfo map;
    if(gst_buffer_map(gst_sample_get_buffer(sample),&map,GST_MAP_READ)) {
        const auto *values=reinterpret_cast<const float *>(map.data);
        std::lock_guard lock(s.mutex);
        for(gsize i=0;i+1<map.size/sizeof(float);i+=2) {
            ++s.audioFrames;
            for(int c=0;c<2;++c) {
                const double v=values[i+c]; if(!std::isfinite(v)) {++s.invalid;continue;}
                s.squares[c]+=v*v;
                if(v < -0.02) s.armed[c]=true;
                if(s.armed[c] && v>0.02) {++s.crossings[c];s.armed[c]=false;}
            }
        }
        gst_buffer_unmap(gst_sample_get_buffer(sample),&map);
    }
    gst_sample_unref(sample); return GST_FLOW_OK;
}
void added(GstBin *,GstBin *,GstElement *element,gpointer data) {
    auto *factory=gst_element_get_factory(element); if(!factory) return;
    const char *klass=gst_element_factory_get_metadata(factory,GST_ELEMENT_METADATA_KLASS);
    if(!klass || !strstr(klass,"Decoder")) return;
    auto &s=*static_cast<Samples *>(data); std::lock_guard lock(s.mutex);
    s.decoders.append(QString::fromUtf8(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))));
}
QJsonObject decode(const QString &path,bool video) {
    Samples samples;
    auto *pipeline=gst_element_factory_make("playbin",nullptr);
    auto *vsink=gst_element_factory_make("appsink",nullptr);
    auto *asink=gst_element_factory_make("appsink",nullptr);
    if(!pipeline || !vsink || !asink) return {{"passed",false},{"error","missing playbin/appsink"}};
    auto *vcaps=gst_caps_from_string("video/x-raw,format=BGRA");
    auto *acaps=gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
    g_object_set(vsink,"caps",vcaps,"sync",FALSE,"emit-signals",TRUE,nullptr);
    g_object_set(asink,"caps",acaps,"sync",FALSE,"emit-signals",TRUE,nullptr);
    gst_caps_unref(vcaps); gst_caps_unref(acaps);
    g_signal_connect(vsink,"new-sample",G_CALLBACK(videoSample),&samples);
    g_signal_connect(asink,"new-sample",G_CALLBACK(audioSample),&samples);
    g_signal_connect(pipeline,"deep-element-added",G_CALLBACK(added),&samples);
    const auto uri=QUrl::fromLocalFile(path).toEncoded();
    g_object_set(pipeline,"uri",uri.constData(),"video-sink",vsink,"audio-sink",asink,"flags",guint(3),nullptr);
    auto *bus=gst_element_get_bus(pipeline); QString error; bool eos=false;
    gst_element_set_state(pipeline,GST_STATE_PLAYING);
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<12000 && !eos && error.isEmpty()) {
        QApplication::processEvents();
        auto *message=gst_bus_timed_pop_filtered(bus,50*GST_MSECOND,
            GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
        if(!message) continue;
        if(GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS) eos=true;
        else { GError *e=nullptr;gchar *debug=nullptr;gst_message_parse_error(message,&e,&debug);
            error=QString::fromUtf8(e->message); g_clear_error(&e);g_free(debug); }
        gst_message_unref(message);
    }
    gst_element_set_state(pipeline,GST_STATE_NULL);
    bool stopped=gst_element_get_state(pipeline,nullptr,nullptr,5*GST_SECOND)==GST_STATE_CHANGE_SUCCESS;
    gst_object_unref(bus);gst_object_unref(pipeline);
    const auto rms=[&](int c){return samples.audioFrames?std::sqrt(samples.squares[c]/samples.audioFrames):0;};
    const auto hz=[&](int c){return samples.audioFrames?samples.crossings[c]*48000/samples.audioFrames:0;};
    bool passed=eos && error.isEmpty() && stopped && samples.invalid==0 && samples.audioFrames>=40000 &&
        std::abs(rms(0)-0.07071)<0.023 && std::abs(rms(1)-0.14142)<0.04 &&
        std::abs(hz(0)-440)<45 && std::abs(hz(1)-880)<70 &&
        (!video || (samples.frames>=20 && samples.redFrames==samples.frames && samples.lastVideoPts>=800*GST_MSECOND));
    return {{"passed",passed},{"eos",eos},{"stopConfirmed",stopped},{"error",error},
        {"videoFrames",samples.frames},{"redFrames",samples.redFrames},{"lastVideoPtsMs",qint64(samples.lastVideoPts/GST_MSECOND)},
        {"pcmFrames",samples.audioFrames},{"rmsLeft",rms(0)},{"rmsRight",rms(1)},
        {"hzLeft",hz(0)},{"hzRight",hz(1)},{"decoders",QJsonArray::fromStringList(samples.decoders)}};
}
bool qtChecks(const QString &qtRoot,const QString &temporary) {
    bool ok=check(QLibraryInfo::path(QLibraryInfo::PluginsPath)==qtRoot,"Qt plugin directory is the selected copy");
    QImage source(48,32,QImage::Format_RGB32); source.fill(QColor(230,32,32));
    for(const auto &format:{"png","jpg","bmp","tiff","webp"}) {
        const auto path=temporary+"/image."+format;
        bool saved=source.save(path,format);const QImage loaded(path);
        const auto color=loaded.isNull()?QColor():loaded.pixelColor(24,16);
        ok &= check(saved && !loaded.isNull() && color.red()>180 && color.green()<85 && color.blue()<85,QString("Qt image roundtrip: ")+format);
    }
    const auto gif=QByteArray::fromBase64("R0lGODlhAQABAIAAAP8AAAAAACH5BAAAAAAALAAAAAABAAEAAAICRAEAOw==");
    QFile gf(temporary+"/image.gif");bool written=gf.open(QIODevice::WriteOnly);if(written){gf.write(gif);gf.close();}
    ok &= check(written && !QImage(gf.fileName()).isNull(),"Qt GIF reader");
    QFile sf(temporary+"/image.svg");written=sf.open(QIODevice::WriteOnly);
    if(written){sf.write("<svg xmlns='http://www.w3.org/2000/svg' width='48' height='32'><rect width='48' height='32' fill='#e62020'/></svg>");sf.close();}
    ok &= check(written && !QImage(sf.fileName()).isNull() && !QIcon(sf.fileName()).pixmap(48,32).isNull(),"Qt SVG reader and icon engine");
    ok &= check(QSslSocket::availableBackends().contains("securetransport") && QSslSocket::setActiveBackend("securetransport") && QSslSocket::supportsSsl(),"Qt native TLS backend loads (no network request)");
    return ok;
}
bool deckChecks(const QString &fixtures,QJsonArray &records) {
    QWidget output,monitor;output.resize(640,360);monitor.resize(640,360);output.show();monitor.show();QApplication::processEvents();
    ProgramGpuCompositor compositor(output.winId(),monitor.winId());DeckSourceSession deck;
    bool ok=check(compositor.start() && deck.initialize(0,1920,1080),"selected runtime starts real deck and Metal compositor");
    if(!ok) return false;
    deck.attachSourceManagers(nullptr,nullptr,nullptr,&compositor);
    compositor.configureDeckInput(0,deck.pipeline(),deck.appSink());
    for(const auto &name:{"mp4-h264-aac.mp4","mov-prores-pcm.mov","webm-vp9-opus.webm","mxf-mpeg2-pcm.mxf","audio-aac.m4a"}) {
        deck.prepare({DualDeckPlayer::DeckSource::Type::File,fixtures+"/"+name});QString error;
        bool ready=waitFor([&]{auto value=deck.refreshPreparation();if(deck.takeSourceError(error)) return true;
            return value.state==DualDeckPlayer::CueStatus::Ready;});
        ready=ready && error.isEmpty() && deck.hasAudio();bool frame=true;
        bool video=QString(name).contains("audio-")==false;
        if(ready && video) {
            compositor.setDeckLayers(0,1,-1,0);
            frame=waitFor([&]{auto capture=compositor.captureStillFrame();if(!capture) return false;
                const auto image=capture();if(image.isNull()) return false;const auto c=image.pixelColor(960,540);
                return c.red()>180 && c.green()<85 && c.blue()<85;});
        }
        ready=ready && deck.hasVideo()==video;
        bool play=ready && deck.setFileState(GST_STATE_PLAYING);
        bool seek=play && deck.seek(400);
        compositor.setDeckLayers(-1,0,-1,0);
        bool released=deck.clearAfterOffAir().succeeded;
        bool passed=ready && frame && play && seek && released; // release clears stream flags
        records.append(QJsonObject{{"file",name},{"ready",ready},{"metalFrame",frame},{"play",play},{"seek",seek},{"released",released},{"error",error},{"passed",passed}});
        ok &= check(passed,QString("real deck standby/play/seek/release: ")+name);
    }
    compositor.prepareShutdown();bool stopped=deck.stopPipelinesForShutdown() && compositor.stop();
    ok &= check(stopped,"real deck and compositor stop confirmed");
    if(stopped) deck.releaseResourcesAfterConfirmedShutdown();return ok;
}
}
int runMacRuntimeSelectionChecks() {
    const auto work=qEnvironmentVariable("VTRPON_RUNTIME_TEST_ROOT");const auto runtime=work+"/runtime";
    const auto contents=qEnvironmentVariable("VTRPON_BUNDLE_CONTENTS");
    const bool bundled=!contents.isEmpty();
    const auto qtRoot=bundled?contents+"/PlugIns/qt":runtime+"/qt";
    const auto gstRoot=bundled?contents+"/PlugIns/gstreamer":runtime+"/gstreamer";
    const auto ffmpegRoot=bundled?contents+"/Frameworks/ffmpeg":runtime+"/ffmpeg";
    QTemporaryDir temporary;QApplication::instance()->setProperty("vtrpon.settingsFile",temporary.filePath("settings.ini"));
    QFile ff(work+"/fixtures/fixtures.json");if(!ff.open(QIODevice::ReadOnly)) return 2;
    const auto cases=QJsonDocument::fromJson(ff.readAll()).object()["cases"].toArray();
    bool ok=check(cases.size()>=18,"authored media matrix is present");ok &= qtChecks(qtRoot,temporary.path());
    for(const auto &name:{"x264enc","x265enc","faad"}) {
        auto *factory=gst_element_factory_find(name);ok &= check(!factory,QString("excluded factory unavailable: ")+name);
        if(factory)gst_object_unref(factory);
    }
    // Negative control: software-only H.264 cannot borrow any excluded decoder.
    QVector<QPair<GstPluginFeature *,guint>> disabled;
    auto *features=gst_registry_get_feature_list_by_plugin(gst_registry_get(),"libav");
    for(auto *it=features;it;it=it->next){auto *f=GST_PLUGIN_FEATURE(it->data);disabled.append({GST_PLUGIN_FEATURE(gst_object_ref(f)),gst_plugin_feature_get_rank(f)});gst_plugin_feature_set_rank(f,0);}
    gst_plugin_feature_list_free(features);
    const auto nativeAdts=decode(work+"/fixtures/audio-adts.aac",false);
    vtrpon::configureVideoDecoders(0,true);
    const auto native=decode(work+"/fixtures/mp4-h264-aac.mp4",true);
    ok &= check(!native["passed"].toBool() && native["videoFrames"].toInteger()==0,"negative control: removing libav leaves no software H.264 decoder");
    for(const auto &item:disabled){gst_plugin_feature_set_rank(item.first,item.second);gst_object_unref(item.first);}
    configureMacAudioDecoders();
    QJsonArray media;
    // Match the production startup choice in separate phases; every previous pipeline is NULL.
    for(bool software:{false,true}) {
        vtrpon::configureVideoDecoders(0,software);
        for(const auto &value:cases) {
            auto item=value.toObject();QString path=work+"/fixtures/"+item["file"].toString();
            QFile input(path);bool hashOk=input.open(QIODevice::ReadOnly) && QString::fromLatin1(QCryptographicHash::hash(input.readAll(),QCryptographicHash::Sha256).toHex())==item["sha256"].toString();
            const auto probe=probeMediaDuration(path);
            auto result=decode(path,item["hasVideo"].toBool());
            bool passed=hashOk && probe.succeeded && probe.durationMs && *probe.durationMs>=800 && *probe.durationMs<=1500 && result["passed"].toBool();
            result["file"]=item["file"];result["softwareOnly"]=software;result["probeSucceeded"]=probe.succeeded;
            result["probeError"]=probe.errorDetail;result["passed"]=passed;media.append(result);
            ok &= check(passed,QString(software?"software: ":"native preference: ")+item["file"].toString());
            if(!passed) qWarning().noquote()<<QJsonDocument(result).toJson(QJsonDocument::Compact);
        }
    }
    vtrpon::configureVideoDecoders(0,false);QJsonArray decks;ok &= deckChecks(work+"/fixtures",decks);
    QJsonArray plugins;auto *list=gst_registry_get_plugin_list(gst_registry_get());
    for(auto *it=list;it;it=it->next){auto *p=GST_PLUGIN(it->data);const auto path=QString::fromUtf8(gst_plugin_get_filename(p));if(path.isEmpty()) continue;
        const auto license=QString::fromUtf8(gst_plugin_get_license(p));
        bool allowed=path.startsWith(gstRoot+"/") && !license.startsWith("GPL") && license!="unknown";
        ok &= check(allowed,QString("registry selected plugin: ")+gst_plugin_get_name(p));
        plugins.append(QJsonObject{{"name",gst_plugin_get_name(p)},{"path",path},{"license",license}});
    }gst_plugin_list_free(list);
    QLibrary codec(ffmpegRoot+"/libavcodec.62.dylib");
    auto licenseFunction=reinterpret_cast<const char *(*)()>(codec.resolve("avcodec_license"));
    auto configurationFunction=reinterpret_cast<const char *(*)()>(codec.resolve("avcodec_configuration"));
    QString ffmpegLicense=licenseFunction?QString::fromUtf8(licenseFunction()):QString();
    QString ffmpegConfiguration=configurationFunction?QString::fromUtf8(configurationFunction()):QString();
    ok &= check(ffmpegLicense=="LGPL version 2.1 or later" && ffmpegConfiguration.contains("--disable-gpl") &&
        ffmpegConfiguration.contains("--disable-nonfree") && !ffmpegConfiguration.contains("--enable-gpl"),"loaded FFmpeg reports LGPL and disabled GPL/nonfree features");
    if(bundled) {
        QFile brew("/opt/homebrew/lib/libgstreamer-1.0.0.dylib");
        ok &= check(!brew.open(QIODevice::ReadOnly),"Homebrew file reads are denied inside this process");
    }
    QJsonArray images;bool closure=true,lgplLoaded=false,qtPluginLoaded=false;
    for(uint32_t i=0;i<_dyld_image_count();++i){QString path=QString::fromUtf8(_dyld_get_image_name(i));images.append(path);
        if(path.contains("/Cellar/ffmpeg/") || path.contains("/opt/ffmpeg/") || path.contains("/libx264") || path.contains("/libx265") || path.contains("/libfaad")) closure=false;
        if(path.contains("/gstreamer-1.0/") && !path.contains("/libexec/")) closure=false;
        if(path.contains("/plugins/") && !path.startsWith(qtRoot+"/")) closure=false;
        if(bundled && !path.startsWith(contents+"/") && !path.startsWith("/usr/lib/") &&
            !path.startsWith("/System/Library/") && !path.startsWith("/System/Volumes/Preboot/Cryptexes/OS/") &&
            !path.startsWith("/Library/Apple/System/")) closure=false;
        if(path.startsWith(qtRoot+"/")) qtPluginLoaded=true;
        if(path.startsWith(ffmpegRoot+"/libavcodec"))lgplLoaded=true;
    }
    ok &= check(closure && lgplLoaded && qtPluginLoaded,"dyld: selected Qt/GStreamer plugins and LGPL FFmpeg; no Homebrew GPL FFmpeg or plugin fallback");
    QJsonObject report{{"passed",ok},{"selfContainedBundle",bundled},{"ffmpegLicense",ffmpegLicense},{"ffmpegConfiguration",ffmpegConfiguration},{"withoutLibavSoftwareH264Control",native},{"nativeAdtsObservation",nativeAdts},{"media",media},{"decks",decks},{"plugins",plugins},{"loadedImages",images}};
    QFile reportFile(work+"/runtime-results.json");if(!reportFile.open(QIODevice::WriteOnly))return 2;
    reportFile.write(QJsonDocument(report).toJson());return ok?0:1;
}
