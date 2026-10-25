#include "program_gpu_compositor.h"
#include "deck_source_session.h"
#include "gpu_configuration.h"
#include "video_output_settings.h"
#include "mac_gpu_activity.h"
#include "mac_system_load_sampler.h"
#include "operator_monitor_output.h"
#include "operator_monitor_transport_mac.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QWidget>
#include <QScopeGuard>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/video/video-converter.h>
#include <sys/mman.h>
#include <functional>
#include <cmath>
#include <cstring>
using namespace vtrpon;
#define CHECK(x) do { if (!(x)) { qCritical()<<"FAIL line"<<__LINE__<<#x; return 1; } } while(false)
static bool waitFor(const std::function<bool()> &condition,int timeout=7000) {
  QElapsedTimer t;t.start();
  do {QApplication::processEvents();if(condition())return true;QThread::msleep(5);}while(t.elapsed()<timeout);
  return false;
}
static void pump(int ms) {QElapsedTimer t;t.start();while(t.elapsed()<ms){QApplication::processEvents();QThread::msleep(5);}}
struct Pipeline {
  GstElement *pipeline=nullptr,*sink=nullptr;
  ~Pipeline(){if(pipeline){gst_element_set_state(pipeline,GST_STATE_NULL);if(sink)gst_object_unref(sink);gst_object_unref(pipeline);}}
};
static QStringList decoderNames(GstElement *pipeline) {
  QStringList result;auto *it=gst_bin_iterate_recurse(GST_BIN(pipeline));GValue item=G_VALUE_INIT;
  bool done=false;while(!done){switch(gst_iterator_next(it,&item)){
  case GST_ITERATOR_OK:{auto *element=GST_ELEMENT(g_value_get_object(&item));auto *f=gst_element_get_factory(element);
    if(f && gst_element_factory_list_is_type(f,GstElementFactoryListType(GST_ELEMENT_FACTORY_TYPE_DECODER|GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO)))
      result<<QString::fromUtf8(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)));
    g_value_reset(&item);break;}
  case GST_ITERATOR_RESYNC:gst_iterator_resync(it);result.clear();break;
  default:done=true;break;}}
  g_value_unset(&item);gst_iterator_free(it);return result;
}
static bool generate(const QString &file) {
  if(QFile::exists(file))return true;
  GError *error=nullptr;
  auto *pipeline=gst_parse_launch("videotestsrc num-buffers=240 pattern=red ! video/x-raw,format=NV12,width=3840,height=2160,framerate=30/1,colorimetry=bt709 ! vtenc_h264_hw bitrate=4000 realtime=false ! h264parse ! mp4mux ! filesink name=output",&error);
  if(error || !pipeline){if(error){qWarning()<<error->message;g_error_free(error);}if(pipeline)gst_object_unref(pipeline);return false;}
  auto *output=gst_bin_get_by_name(GST_BIN(pipeline),"output");g_object_set(output,"location",file.toUtf8().constData(),nullptr);gst_object_unref(output);
  auto *bus=gst_element_get_bus(pipeline);gst_element_set_state(pipeline,GST_STATE_PLAYING);
  auto *msg=gst_bus_timed_pop_filtered(bus,30*GST_SECOND,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
  bool ok=msg && GST_MESSAGE_TYPE(msg)==GST_MESSAGE_EOS;
  if(msg){if(!ok){GError *detail=nullptr;gst_message_parse_error(msg,&detail,nullptr);qWarning()<<(detail?detail->message:"");g_clear_error(&detail);}gst_message_unref(msg);}
  gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(bus);gst_object_unref(pipeline);return ok;
}
static bool pixelNear(const QImage &image,int x,int y,QColor expected,int tolerance=4) {
  if(image.size()!=QSize(3840,2160))return false;const auto actual=image.pixelColor(x,y);
  return std::abs(actual.red()-expected.red())<=tolerance && std::abs(actual.green()-expected.green())<=tolerance && std::abs(actual.blue()-expected.blue())<=tolerance;
}
static bool colorMatrix(ProgramGpuCompositor &compositor,const char *matrix,bool full) {
  GstVideoInfo input{},output{};gst_video_info_set_format(&input,GST_VIDEO_FORMAT_NV12,64,36);
  gst_video_colorimetry_from_string(&input.colorimetry,matrix);if(full)input.colorimetry.range=GST_VIDEO_COLOR_RANGE_0_255;
  input.fps_n=30;input.fps_d=1;
  gst_video_info_set_format(&output,GST_VIDEO_FORMAT_BGRA,64,36);
  output.fps_n=input.fps_n;output.fps_d=input.fps_d;
  auto *buffer=gst_buffer_new_allocate(nullptr,input.size,nullptr);
  GstVideoFrame in{},out{};auto *converted=gst_buffer_new_allocate(nullptr,output.size,nullptr);
  if(!gst_video_frame_map(&in,&input,buffer,GST_MAP_WRITE) || !gst_video_frame_map(&out,&output,converted,GST_MAP_WRITE))return false;
  const int ys[]={16,235,81,145,41,128},us[]={128,128,90,54,240,170},vs[]={128,128,240,34,110,200};
  for(int y=0;y<36;++y){auto *row=static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&in,0))+y*GST_VIDEO_FRAME_PLANE_STRIDE(&in,0);
    for(int x=0;x<64;++x)row[x]=ys[x*6/64];}
  for(int y=0;y<18;++y){auto *row=static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&in,1))+y*GST_VIDEO_FRAME_PLANE_STRIDE(&in,1);
    for(int x=0;x<32;++x){row[x*2]=us[x*6/32];row[x*2+1]=vs[x*6/32];}}
  auto *converter=gst_video_converter_new(&input,&output,nullptr);if(!converter)return false;
  gst_video_converter_frame(converter,&in,&out);
  QColor expected[6];for(int i=0;i<6;++i){int x=(2*i+1)*64/12;auto *p=static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&out,0))+18*GST_VIDEO_FRAME_PLANE_STRIDE(&out,0)+x*4;expected[i]=QColor(p[2],p[1],p[0]);}
  gst_video_converter_free(converter);gst_video_frame_unmap(&in);gst_video_frame_unmap(&out);gst_buffer_unref(converted);
  Pipeline p;p.pipeline=gst_parse_launch("appsrc name=input ! appsink name=frames max-buffers=2 sync=false wait-on-eos=false",nullptr);
  if(!p.pipeline)return false;p.sink=gst_bin_get_by_name(GST_BIN(p.pipeline),"frames");
  if(!compositor.configureDeckInput(1,p.pipeline,p.sink))return false;
  auto *source=gst_bin_get_by_name(GST_BIN(p.pipeline),"input");auto *caps=gst_video_info_to_caps(&input);
  gst_app_src_set_caps(GST_APP_SRC(source),caps);gst_caps_unref(caps);
  gst_element_set_state(p.pipeline,GST_STATE_PAUSED);gst_app_src_push_buffer(GST_APP_SRC(source),buffer);gst_object_unref(source);
  compositor.setDeckLayers(1,1,-1,0);
  const bool ok=waitFor([&]{auto f=compositor.captureStillFrame();if(!f)return false;auto image=f();
    for(int i=0;i<6;++i)if(!pixelNear(image,(2*i+1)*3840/12,1080,expected[i],6))return false;return true;});
  qInfo()<<"COLOR_MATRIX"<<matrix<<(full?"full":"limited")<<ok;
  return ok;
}
static bool finePixels(ProgramGpuCompositor &compositor) {
  Pipeline p;p.pipeline=gst_parse_launch("appsrc name=input ! video/x-raw,format=BGRA,width=3840,height=2160,framerate=30/1 ! appsink name=frames sync=false wait-on-eos=false",nullptr);
  if(!p.pipeline)return false;p.sink=gst_bin_get_by_name(GST_BIN(p.pipeline),"frames");
  if(!compositor.configureDeckInput(1,p.pipeline,p.sink))return false;
  auto *buffer=gst_buffer_new_allocate(nullptr,3840*2160*4,nullptr);GstMapInfo map{};gst_buffer_map(buffer,&map,GST_MAP_WRITE);
  auto *pixels=reinterpret_cast<quint32*>(map.data);for(int y=0;y<2160;++y)for(int x=0;x<3840;++x)pixels[y*3840+x]=(x%2)?0xffffffff:0xff000000;
  gst_buffer_unmap(buffer,&map);auto *source=gst_bin_get_by_name(GST_BIN(p.pipeline),"input");
  auto *caps=gst_caps_from_string("video/x-raw,format=BGRA,width=3840,height=2160,framerate=30/1");gst_app_src_set_caps(GST_APP_SRC(source),caps);gst_caps_unref(caps);
  gst_element_set_state(p.pipeline,GST_STATE_PAUSED);gst_app_src_push_buffer(GST_APP_SRC(source),buffer);gst_object_unref(source);compositor.setDeckLayers(1,1,-1,0);
  return waitFor([&]{auto f=compositor.captureStillFrame();if(!f)return false;auto image=f();return pixelNear(image,1920,1080,Qt::black,1)&&pixelNear(image,1921,1080,Qt::white,1);});
}
int main(int argc,char **argv) {
  QApplication app(argc,argv);gst_init(&argc,&argv);
  QTemporaryDir temporary;CHECK(temporary.isValid());
  app.setProperty("vtrpon.settingsFile",temporary.filePath("settings.ini"));
  const bool software=app.arguments().contains("--software");
  const QString directory=QString::fromUtf8(VTRPON_MAC_VIDEO_EVIDENCE);QDir().mkpath(directory);
  const auto file=directory+"/日本語 4K SDR.mp4";CHECK(generate(file));
  const auto adapters=availableGpuAdapters();CHECK(!adapters.isEmpty());
  const auto requested=software?QStringLiteral("missing-windows-gpu"):adapters.front().id;
  auto settings=makeControllerSettings();CHECK(saveGpuPreferences(settings,{requested,software}));CHECK(save4kProgramOutput(settings,true));
  CHECK(initializeProgramOutputResolution());freezeGpuPreferences();
  CHECK(save4kProgramOutput(settings,false) && initializeProgramOutputResolution());
  CHECK(availableGpuAdapters().front().id==adapters.front().id);
  CHECK(adapters.front().id.startsWith("metal:") && adapters.front().luid);
  CHECK(std::abs(metalBusyPercent({{.1,.4},{.2,.6},{.8,1.2}},0,1)-70)<1e-6);
  QWidget output,monitor;output.resize(640,360);monitor.resize(320,180);output.show();monitor.show();pump(100);
  ProgramGpuCompositor c(output.winId(),monitor.winId(),{3840,2160});CHECK(c.start());
  CHECK(app.property("gpu.activeSoftwareDecoding").toBool()==software);
  CHECK(app.property("gpu.requestedAdapterMissing").toBool()==software);
  CHECK(readGpuPreferences(settings).adapterId==requested);
  CHECK(app.property("gpu.activeAdapterId").toString()==adapters.front().id);
  DeckSourceSession deck;CHECK(deck.initialize(0,3840,2160));deck.attachSourceManagers(nullptr,nullptr,nullptr,&c);
  CHECK(c.configureDeckInput(0,deck.pipeline(),deck.appSink()));deck.prepare({DualDeckPlayer::DeckSource::Type::File,file});
  CHECK(waitFor([&]{return deck.refreshPreparation().state==DualDeckPlayer::CueStatus::Ready;}));
  CHECK(deck.hasVideo()&&!deck.hasAudio());const auto decoders=decoderNames(deck.pipeline());qInfo()<<"DECODERS"<<decoders;
  CHECK(software ? decoders.contains("avdec_h264") : decoders.contains("vtdec_hw"));
  CHECK(c.setDeckLayers(0,1,-1,0));CHECK(waitFor([&]{auto f=c.captureStillFrame();return f&&pixelNear(f(),1920,1080,Qt::red,6);}));
  DeckSourceSession second;CHECK(second.initialize(1,3840,2160));second.attachSourceManagers(nullptr,nullptr,nullptr,&c);
  const auto cleanup=qScopeGuard([&]{setOperatorMonitorEnabled(false);c.prepareShutdown();
    if(deck.stopPipelinesForShutdown())deck.releaseResourcesAfterConfirmedShutdown();
    if(second.stopPipelinesForShutdown())second.releaseResourcesAfterConfirmedShutdown();c.stop();});
  CHECK(c.configureDeckInput(1,second.pipeline(),second.appSink()));second.prepare({DualDeckPlayer::DeckSource::Type::File,file});
  CHECK(waitFor([&]{return second.refreshPreparation().state==DualDeckPlayer::CueStatus::Ready;}));
  CHECK(c.setDeckLayers(1,.5f,0,.5f));
  CHECK(deck.setFileState(GST_STATE_PLAYING) && second.setFileState(GST_STATE_PLAYING));pump(400);
  MacSystemLoadSampler load;const auto before=c.macStatistics();const auto frames=c.renderedFrameCount();QElapsedTimer elapsed;elapsed.start();
  int beats=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start(10);pump(1600);
  const auto seconds=elapsed.elapsed()/1000.0;const auto measured=load.sample();const auto stats=c.macStatistics();
  const double renderFps=(c.renderedFrameCount()-frames)/seconds;
  CHECK(stats.inputAllocations==before.inputAllocations && c.renderedFrameCount()>frames+20 && beats>40);
  CHECK(measured.processMetal>=0 && measured.processMetal<=100);
  if(!software)CHECK(stats.nv12Uploads>0 && stats.uploadedBytes==stats.nv12Uploads*3840ULL*2160*3/2);
  qInfo()<<"METAL_METRICS"<<"renderFPS"<<renderFps<<"systemGPU"<<measured.systemGpu<<"appMetal"<<measured.processMetal<<"appCPU"<<measured.processCpu<<"uploadedBytes"<<stats.uploadedBytes<<"NV12"<<stats.nv12Uploads<<"BGRA"<<stats.bgraUploads<<"allocations"<<stats.inputAllocations;
  CHECK(deck.setFileState(GST_STATE_PAUSED) && second.setFileState(GST_STATE_PAUSED));
  CHECK(c.setDeckLayers(0,1,-1,0));CHECK(second.clearAfterOffAir().succeeded);
  CHECK(waitFor([&]{auto f=c.captureStillFrame();return f&&pixelNear(f(),1920,1080,Qt::red,6);}));
  // The same 4K final texture supplies a bounded 1080p return image.
  OperatorMonitorControlFeed feed;CHECK(feed.available());setOperatorMonitorEnabled(true);
  void *mapping=mmap(nullptr,sizeof(MacOperatorMonitorShared),PROT_READ|PROT_WRITE,MAP_SHARED,feed.sharedDescriptor(),0);CHECK(mapping!=MAP_FAILED);
  auto *shared=static_cast<MacOperatorMonitorShared*>(mapping);
  CHECK(waitFor([&]{if(pthread_mutex_trylock(&shared->videoMutex)!=0)return false;bool ok=shared->frame.width==1920&&shared->frame.height==1080&&shared->frame.serial>0;pthread_mutex_unlock(&shared->videoMutex);return ok;}));
  setOperatorMonitorEnabled(false);munmap(mapping,sizeof(MacOperatorMonitorShared));
  auto pinned=c.captureStillFrame();CHECK(pinned);c.setFtbCoverage(1);CHECK(waitFor([&]{auto f=c.captureStillFrame();return f&&pixelNear(f(),1920,1080,Qt::black);}));
  CHECK(pixelNear(pinned(),1920,1080,Qt::red,6));pinned={};c.setFtbCoverage(0);
  for(const auto *matrix:{"bt601","bt709"})for(bool full:{false,true})CHECK(colorMatrix(c,matrix,full));
  CHECK(finePixels(c));
  output.hide();monitor.hide();const auto hidden=c.renderedFrameCount();pump(200);CHECK(c.renderedFrameCount()>hidden+3);
  c.setDeckLayers(-1,0,-1,0);CHECK(deck.clearAfterOffAir().succeeded);c.prepareShutdown();CHECK(deck.stopPipelinesForShutdown()&&c.stop());deck.releaseResourcesAfterConfirmedShutdown();
  QFile result(directory+(software?"/software-results.json":"/hardware-results.json"));CHECK(result.open(QIODevice::WriteOnly));
  result.write(QJsonDocument(QJsonObject{{"mode",software?"software":"hardware"},{"active4kInputs",2},{"decoder",decoders.join(",")},{"width",3840},{"height",2160},{"renderFPS",renderFps},{"systemGpu",measured.systemGpu},{"metal",measured.processMetal},{"cpu",measured.processCpu},{"uploadedBytes",double(stats.uploadedBytes)},{"nv12Frames",double(stats.nv12Uploads)},{"bgraFrames",double(stats.bgraUploads)},{"inputAllocations",double(stats.inputAllocations)},{"pass",true}}).toJson());
  qInfo()<<"PASS: 4K normal deck, actual decoder, bounded copies, color ranges, fine pixels, immutable capture, return downscale, GPU and shutdown";
  return 0;
}
