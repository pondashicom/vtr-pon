#include "operator_monitor_output.h"
#include "operator_monitor_transport_mac.h"
#include "program_gpu_compositor.h"
#include "view_menu_controller.h"
#include "controller_settings.h"
#include <QApplication>
#include <QAction>
#include <QMenuBar>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTranslator>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QImage>
#include <QWidget>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <functional>
#include <memory>
using namespace vtrpon;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
struct MappingView {
  MacOperatorMonitorShared *shared = nullptr;
  explicit MappingView(int fd) {
    void *value = mmap(nullptr, sizeof(MacOperatorMonitorShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (value != MAP_FAILED) shared = static_cast<MacOperatorMonitorShared *>(value);
  }
  ~MappingView() { if (shared) munmap(shared,sizeof(*shared)); }
};
static bool waitFor(const std::function<bool()> &condition, int timeout = 3000) {
  QElapsedTimer timer; timer.start();
  while (!condition() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(5); }
  return condition();
}
static void pump(int ms) { QElapsedTimer timer; timer.start(); while (timer.elapsed()<ms) { QCoreApplication::processEvents(); QThread::msleep(5); } }
static void inherit(QProcess &child, int descriptor) {
  auto *process=&child;
  child.setChildProcessModifier([descriptor,process] {
    if (dup2(descriptor,STDIN_FILENO)<0 || fcntl(STDIN_FILENO,F_SETFD,0)!=0)
      process->failChildProcessModifier("Return monitor test mapping",errno);
  });
  child.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
}
static QString helper() { return QCoreApplication::applicationDirPath()+"/../Helpers/vtr-pon-operator-monitor.app/Contents/MacOS/vtr-pon-operator-monitor"; }
static int transport() {
  setOperatorMonitorEnabled(false);
  auto feed=std::make_unique<OperatorMonitorControlFeed>(); CHECK(feed->available());
  MappingView view(feed->sharedDescriptor()); CHECK(view.shared);
  auto &shared=*view.shared;
  QImage image(32,18,QImage::Format_ARGB32); image.fill(QColor(12,34,56));
  CHECK(!publishOperatorMonitorFrame(image,7)); CHECK(shared.frame.serial==0);
  setOperatorMonitorEnabled(true); CHECK(publishOperatorMonitorFrame(image,7));
  CHECK(shared.sourceSequence==7 && shared.frame.width==32 && shared.frame.height==18);
  CHECK(qBlue(*reinterpret_cast<const QRgb *>(shared.frame.pixels))==56);
  feed->publish(R"({"title":"test","cue":"日本語"})"); CHECK(shared.presentation.serial==1);
  CHECK(pthread_mutex_lock(&shared.videoMutex)==0);
  QElapsedTimer timer; timer.start();
  for(int i=0;i<100;++i) CHECK(!publishOperatorMonitorFrame(image,8));
  CHECK(timer.elapsed()<100);
  feed->publish(R"({"title":"independent metadata"})"); CHECK(shared.presentation.serial==2);
  CHECK(pthread_mutex_unlock(&shared.videoMutex)==0);
  CHECK(pthread_mutex_lock(&shared.infoMutex)==0);
  feed->publish(R"({"title":"dropped metadata"})"); CHECK(shared.presentation.serial==2);
  CHECK(publishOperatorMonitorFrame(image,9));
  CHECK(pthread_mutex_unlock(&shared.infoMutex)==0);
  feed->publish(QByteArray(16385,'x')); CHECK(shared.presentation.serial==2);
  CHECK(!publishOperatorMonitorFrame(QImage(1921,1,QImage::Format_ARGB32),10));
  CHECK(!publishOperatorMonitorFrame(QImage(8,8,QImage::Format_Grayscale8),10));
  // A fresh feed replaces an abandoned lock without touching or waiting on it.
  CHECK(pthread_mutex_lock(&shared.videoMutex)==0);
  feed=std::make_unique<OperatorMonitorControlFeed>(); CHECK(feed->available());
  MappingView fresh(feed->sharedDescriptor()); CHECK(fresh.shared && fresh.shared!=view.shared);
  CHECK(publishOperatorMonitorFrame(image,10)); CHECK(fresh.shared->sourceSequence==10);
  pthread_mutex_unlock(&shared.videoMutex);
  setOperatorMonitorEnabled(false); return 0;
}
namespace vtrpon {
class MacOperatorMonitorTest {
public:
  static OperatorMonitorControlFeed *feed(ViewMenuController &v) { return v.operatorMonitorFeed_.get(); }
  static QProcess *process(ViewMenuController &v) { return v.operatorMonitorProcess_; }
};
}
static int lifetimeParent() {
  OperatorMonitorControlFeed feed; CHECK(feed.available()); setOperatorMonitorEnabled(true);
  QProcess child; inherit(child,feed.sharedDescriptor());
  child.setStandardOutputFile(QProcess::nullDevice()); child.setStandardErrorFile(QProcess::nullDevice());
  child.start(helper(),{"fd:0",QString::number(QCoreApplication::applicationPid()),"lifetime child"});
  CHECK(waitFor([&] { return child.state()==QProcess::Running; }));
  std::printf("CHILD_PID %lld\n",(long long)child.processId()); std::fflush(stdout);
  QImage image(32,18,QImage::Format_ARGB32); image.fill(Qt::black);
  QTimer pulse; QObject::connect(&pulse,&QTimer::timeout,[&] { publishOperatorMonitorFrame(image,1); }); pulse.start(30);
  return QCoreApplication::exec();
}
static int nativeTest() {
  QTemporaryDir directory; CHECK(directory.isValid());
  QCoreApplication::instance()->setProperty("vtrpon.settingsFile",directory.filePath("settings.ini"));
  auto settings=makeControllerSettings(); OperatorMonitorSettings options;
  options.timerEnabled=true; options.timerDurationSeconds=90; options.fields.fill(true);
  CHECK(saveOperatorMonitorSettings(settings,options));
  QWidget output,control; QMenuBar menu(&control);
  output.setWindowTitle("Mac return monitor test output"); control.setWindowTitle("Mac return monitor test control");
  output.resize(640,360); control.resize(480,300); output.show(); control.show(); pump(100);
  ViewMenuController view(&menu,&output,&control);
  view.operatorMonitorStatusProvider=[] { return QStringList{"00:01:00","日本語 Test","PLAY / PAUSE","100%","2 / 3"}; };
  ProgramGpuCompositor compositor(output.winId(),control.winId()); CHECK(compositor.start());
  compositor.setBackgroundColor(0x223344);
  auto *action=menu.findChild<QAction *>("operatorMonitorAction"); CHECK(action && action->isEnabled());
  int unavailable=0; QObject::connect(&view,&ViewMenuController::operatorMonitorUnavailable,[&]{++unavailable;});
  action->trigger(); auto *process=MacOperatorMonitorTest::process(view); CHECK(process);
  auto ready=[&] {
    auto *feed=MacOperatorMonitorTest::feed(view); if(!feed || !feed->available()) return false;
    MappingView mapping(feed->sharedDescriptor());
    return mapping.shared && mapping.shared->receiverFrameSerial.load(std::memory_order_acquire)>0;
  };
  CHECK(waitFor(ready,5000)); CHECK(action->isChecked() && process->state()==QProcess::Running);
  auto shared=std::make_unique<MappingView>(MacOperatorMonitorTest::feed(view)->sharedDescriptor()); CHECK(shared->shared);
  auto color=[&](QColor expected) {
    std::uint64_t matching=0;
    return waitFor([&] {
      auto &s=*shared->shared;
      if(pthread_mutex_trylock(&s.videoMutex)!=0) return false;
      const auto &f=s.frame;
      const QRgb pixel=f.width==1920 && f.height==1080 ? reinterpret_cast<const QRgb *>(f.pixels)[540*1920+960] : 0;
      const bool match=std::abs(qRed(pixel)-expected.red())<=2 && std::abs(qGreen(pixel)-expected.green())<=2 && std::abs(qBlue(pixel)-expected.blue())<=2;
      if(match && !matching) matching=f.serial;
      const bool source=s.sourceSequence>0 && s.sourceSequence<=compositor.renderedFrameCount();
      pthread_mutex_unlock(&s.videoMutex);
      return match && source && matching && s.receiverFrameSerial.load(std::memory_order_acquire)>=matching;
    });
  };
  CHECK(color(QColor(0x22,0x33,0x44)));
  auto *pipeline=gst_parse_launch("videotestsrc pattern=red ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=30/1 ! appsink name=frames max-buffers=2 sync=true wait-on-eos=false",nullptr);
  CHECK(pipeline); auto *sink=gst_bin_get_by_name(GST_BIN(pipeline),"frames");
  const auto pipelineCleanup=std::shared_ptr<void>(pipeline,[sink](void *p){gst_element_set_state(GST_ELEMENT(p),GST_STATE_NULL);gst_object_unref(sink);gst_object_unref(p);});
  CHECK(compositor.configureDeckInput(0,pipeline,sink)); gst_element_set_state(pipeline,GST_STATE_PLAYING);
  CHECK(waitFor([&]{return compositor.deckFrameReady(0);})); CHECK(compositor.setDeckLayers(0,1,-1,0)); CHECK(color(QColor(255,0,0)));
  compositor.setBackgroundColor(0); compositor.setFtbCoverage(1); CHECK(color(Qt::black));
  compositor.setFtbCoverage(0); compositor.setItemFadeProgress(.5f); CHECK(color(QColor(128,0,0)));
  compositor.setItemFadeProgress(1);
  auto *overlay=gst_parse_launch("videotestsrc pattern=green ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=30/1 ! appsink name=frames max-buffers=2 sync=true wait-on-eos=false",nullptr);
  CHECK(overlay); auto *overlaySink=gst_bin_get_by_name(GST_BIN(overlay),"frames");
  const auto overlayCleanup=std::shared_ptr<void>(overlay,[overlaySink](void *p){gst_element_set_state(GST_ELEMENT(p),GST_STATE_NULL);gst_object_unref(overlaySink);gst_object_unref(p);});
  CHECK(compositor.configureDskInput(0,overlay,overlaySink)); gst_element_set_state(overlay,GST_STATE_PLAYING);
  CHECK(waitFor([&]{return compositor.dskFrameReady(0);})); CHECK(compositor.setDskLayers(0,.5f,-1,0)); CHECK(color(QColor(128,128,0)));
  view.setOperatorMonitorCue(QStringLiteral("本番まで30秒"));
  const auto infoSerial=shared->shared->presentation.serial;
  CHECK(waitFor([&]{return shared->shared->receiverInfoSerial.load(std::memory_order_acquire)>=infoSerial;}));
  const auto info=QJsonDocument::fromJson(QByteArray(shared->shared->presentation.json,int(shared->shared->presentation.size))).object();
  CHECK(info.value("cue").toString()==QStringLiteral("本番まで30秒"));
  CHECK(info.value("timer").toObject().value("enabled").toBool());
  CHECK(info.value("fields").toArray().size()==1);
  view.setOperatorMonitorCue({}); view.startOperatorTimer(); pump(150); view.pauseOperatorTimer(); view.resetOperatorTimer();

  // A stopped reader owns the frame lock. Neither the renderer nor GUI waits.
  QProcess locked; inherit(locked,MacOperatorMonitorTest::feed(view)->sharedDescriptor());
  locked.start(QCoreApplication::applicationFilePath(),{"--hold-video-lock"});
  CHECK(waitFor([&]{return locked.bytesAvailable()>0;})); CHECK(locked.readAllStandardOutput().contains("LOCKED"));
  CHECK(::kill(pid_t(locked.processId()),SIGSTOP)==0);
  int pulses=0; QTimer pulse; QObject::connect(&pulse,&QTimer::timeout,[&]{++pulses;}); pulse.start(10);
  const auto before=compositor.renderedFrameCount(); pump(600);
  CHECK(compositor.renderedFrameCount()>before+5 && pulses>20 && compositor.isAvailable());
  locked.kill(); CHECK(waitFor([&]{return locked.state()==QProcess::NotRunning;}));
  action->trigger(); CHECK(waitFor([&]{return process->state()==QProcess::NotRunning;}));
  CHECK(!action->isChecked() && unavailable==0);
  action->trigger(); CHECK(waitFor(ready,5000));
  shared=std::make_unique<MappingView>(MacOperatorMonitorTest::feed(view)->sharedDescriptor()); CHECK(color(QColor(128,128,0)));
  // Abnormal receiver exit resets only its action, and can be restarted.
  process->kill(); CHECK(waitFor([&]{return process->state()==QProcess::NotRunning && !action->isChecked();}));
  CHECK(unavailable==1 && action->isEnabled() && compositor.isAvailable());
  action->trigger(); CHECK(waitFor(ready,5000));
  control.hide(); output.hide(); const auto hidden=compositor.renderedFrameCount(); pump(250);
  CHECK(compositor.renderedFrameCount()>hidden+3 && ready()); control.show(); output.show();
  view.requestShutdown(); CHECK(waitFor([&]{return view.shutdownComplete();})); CHECK(!action->isChecked());
  CHECK(compositor.prepareShutdown() && compositor.stop());

  QProcess parent; auto env=QProcessEnvironment::systemEnvironment(); env.insert("QT_QPA_PLATFORM","offscreen"); parent.setProcessEnvironment(env);
  parent.start(QCoreApplication::applicationFilePath(),{"--lifetime-parent"});
  CHECK(waitFor([&]{return parent.bytesAvailable()>0;}));
  const auto text=parent.readAllStandardOutput().trimmed(); CHECK(text.startsWith("CHILD_PID "));
  const auto pid=text.mid(10).toLongLong(); CHECK(pid>1);
  pump(400); CHECK(::kill(pid_t(pid),0)==0);
  parent.kill(); CHECK(waitFor([&]{return parent.state()==QProcess::NotRunning;}));
  CHECK(waitFor([&]{return ::kill(pid_t(pid),0)!=0 && errno==ESRCH;},2500));
  qInfo()<<"Mac return monitor: final pixels, metadata, timer, stopped reader, restart, crash, hidden surfaces and parent cleanup PASS";
  return 0;
}
int main(int argc,char **argv) {
  if(argc>1 && QByteArray(argv[1])=="--hold-video-lock") {
    QCoreApplication app(argc,argv); MappingView map(STDIN_FILENO); CHECK(map.shared);
    CHECK(pthread_mutex_lock(&map.shared->videoMutex)==0); std::puts("LOCKED"); std::fflush(stdout); for(;;) pause();
  }
  if(argc>1 && (QByteArray(argv[1])=="--transport" || QByteArray(argv[1])=="--lifetime-parent")) {
    QCoreApplication app(argc,argv); return QByteArray(argv[1])=="--transport" ? transport() : lifetimeParent();
  }
  QApplication app(argc,argv); gst_init(&argc,&argv);
  QTranslator translator; if(translator.load(QCoreApplication::applicationDirPath()+"/vtrpon_ja.qm")) app.installTranslator(&translator);
  return nativeTest();
}
