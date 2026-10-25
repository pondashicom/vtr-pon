#include "program_gpu_compositor.h"
#include "virtual_camera/output.h"
#include "dsk_playback.h"
#include "uvc_input_guard.h"
#include <QApplication>
#include <QWidget>
#include <QTemporaryDir>
#include <QProcess>
#include <QImage>
#include <QPainter>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QDebug>
#include <gst/gst.h>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <cmath>

static void pump(int ms) {
    QElapsedTimer t; t.start();
    while(t.elapsed()<ms) { QApplication::processEvents(); QThread::msleep(5); }
}
static pid_t senderPid() {
    pid_t children[64]{};
    const auto size=proc_listpids(PROC_PPID_ONLY,getpid(),children,sizeof(children));
    for(int i=0;i<size/int(sizeof(pid_t));++i) {
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        if(proc_pidpath(children[i],path,sizeof(path))>0 && QString::fromUtf8(path).contains("vtr-pon-camera-output")) return children[i];
    }
    return 0;
}
int main(int argc,char **argv) {
    QApplication app(argc,argv); gst_init(&argc,&argv);
    if(argc!=3 && argc!=4) return 2;
    const bool fourK=argc==4 && QString::fromLocal8Bit(argv[3])==QStringLiteral("--4k");
    const QString receiver=QString::fromLocal8Bit(argv[1]), evidence=QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(evidence);
    QTemporaryDir settings;
    app.setProperty("vtrpon.settingsFile",settings.filePath("user.ini"));
    vtrpon::VirtualCameraRuntime runtime;
    if(!vtrpon::setVirtualCameraEnabled(true)) return 3;
    QString reason;
    bool ok=!vtrpon::uvcInputAllowed("avfoundation:BAC4B305-4C38-44BE-A1D3-DA855F14415D",&reason) && !reason.isEmpty();
    QWidget output, monitor; output.resize(640,360); monitor.resize(320,180); output.show(); monitor.show(); pump(100);
    ProgramGpuCompositor compositor(output.winId(),monitor.winId(),fourK ? ProgramGpuCompositor::OutputGeometry{3840,2160} : ProgramGpuCompositor::OutputGeometry{1920,1080});
    if(!compositor.start()) return 4;
    GError *error=nullptr;
    auto *red=gst_parse_launch("videotestsrc pattern=red ! video/x-raw,format=BGRA,width=1440,height=1080,framerate=30/1 ! appsink name=frames max-buffers=2 sync=true wait-on-eos=false",&error);
    auto *blue=gst_parse_launch("videotestsrc pattern=blue ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=30/1 ! appsink name=frames max-buffers=2 sync=true wait-on-eos=false",&error);
    if(!red || !blue || error) return 5;
    auto *redSink=gst_bin_get_by_name(GST_BIN(red),"frames"), *blueSink=gst_bin_get_by_name(GST_BIN(blue),"frames");
    ok &= compositor.configureDeckInput(0,red,redSink) && compositor.configureDeckInput(1,blue,blueSink);
    gst_element_set_state(red,GST_STATE_PLAYING); gst_element_set_state(blue,GST_STATE_PLAYING);
    compositor.setDeckLayers(0,1,-1,0); compositor.setBackgroundColor(0x0000ff); pump(400);
    auto startReceiver=[&](QProcess &process,const QString &name,int frames=60) {
        process.setProgram(receiver);
        process.setArguments({"--device-id","BAC4B305-4C38-44BE-A1D3-DA855F14415D","--device-name","VTR-PON Virtual Camera","--no-bars","--allow-static","--warmup","15","--frames",QString::number(frames),"--output",QDir(evidence).filePath(name)});
        process.start();
    };
    auto waitReceiver=[&](QProcess &process) {
        QElapsedTimer elapsed; elapsed.start();
        while(process.state()!=QProcess::NotRunning && elapsed.elapsed()<18000) pump(10);
        if(process.state()!=QProcess::NotRunning) { process.kill(); process.waitForFinished(500); return false; }
        qInfo().noquote()<<process.readAllStandardError();
        return process.exitStatus()==QProcess::NormalExit && process.exitCode()==0;
    };
    auto capture=[&](const QString &name,QColor expected,int x=960,int y=540) {
        QProcess process; startReceiver(process,name);
        const bool received=waitReceiver(process);
        QImage frame(QDir(evidence).filePath(name+"/first-frame.png"));
        QColor pixel=frame.isNull()?QColor():frame.pixelColor(x,y);
        const bool match=received && frame.size()==QSize(1920,1080) && std::abs(pixel.red()-expected.red())<=3 && std::abs(pixel.green()-expected.green())<=3 && std::abs(pixel.blue()-expected.blue())<=3;
        qInfo()<<name<<match<<pixel<<vtrpon::virtualCameraStatus(); ok &= match;
        return frame;
    };
    capture("red",QColor(255,0,0));
    QImage redFrame(QDir(evidence).filePath("red/first-frame.png"));
    ok &= !redFrame.isNull() && redFrame.pixelColor(10,540)==QColor(0,0,255);
    compositor.setFtbCoverage(.5F); pump(200); capture("half-ftb",QColor(128,0,128));
    compositor.setFtbCoverage(0);
    DskPlayback dsk; ok &= dsk.initialize(); dsk.attachCompositor(&compositor);
    for(int i=0;i<2;++i) ok &= compositor.configureDskInput(i,dsk.pipeline(i),dsk.appSink(i));
    QImage overlay(960,540,QImage::Format_ARGB32); overlay.fill(Qt::transparent);
    { QPainter painter(&overlay); painter.fillRect(380,220,200,100,QColor(0,255,0,128)); }
    auto png=settings.filePath("dsk.png"); overlay.save(png);
    ok &= dsk.cue(png);
    for(int i=0;i<500 && dsk.cueStatus()==DskPlayback::DskStatus::Loading;++i) { dsk.pollBus(); pump(10); }
    ok &= dsk.cueStatus()==DskPlayback::DskStatus::Ready && dsk.takeCue(false,false,1);
    pump(200); capture("dsk",QColor(127,128,0));
    ok &= dsk.clear();
    for(int i=0;i<300 && dsk.retirementPending();++i) { dsk.pollBus(); pump(10); }
    compositor.setBackgroundColor(0); compositor.setDeckLayers(1,.5F,0,.5F); pump(200);
    capture("crossfade",QColor(64,0,128));
    compositor.setFtbCoverage(1); pump(200); capture("black-ftb",QColor(0,0,0));
    compositor.setFtbCoverage(0); compositor.setDeckLayers(0,1,-1,0);
    // Pause only this test's camera sender. Rendering must continue and the
    // watchdog must replace the sender/mapping rather than wait on the reader.
    QProcess continuous; startReceiver(continuous,"sender-recovery",180); pump(900);
    const auto old=senderPid(); const auto before=compositor.renderedFrameCount();
    ok &= old>0 && kill(old,SIGSTOP)==0;
    pump(4400);
    const auto replacement=senderPid();
    const bool isolated=compositor.renderedFrameCount()>before+30 && replacement>0 && replacement!=old;
    qInfo()<<"paused sender isolation and recovery"<<isolated<<old<<replacement;
    ok &= isolated && waitReceiver(continuous);
    capture("recovered-red",QColor(255,0,0));
    ok &= vtrpon::setVirtualCameraEnabled(false); pump(500);
    const auto waiting=capture("output-off",QColor(0,0,0));
    int white=0;
    if(!waiting.isNull()) for(int y=500;y<580;++y) for(int x=50;x<600;++x) if(waiting.pixelColor(x,y).red()>200) ++white;
    ok &= white>100 && senderPid()==0;
    qInfo()<<"OFF waiting label / sender stopped"<<white<<senderPid();
    dsk.attachCompositor(nullptr); dsk.shutdown(); compositor.stop();
    gst_element_set_state(red,GST_STATE_NULL); gst_element_set_state(blue,GST_STATE_NULL);
    gst_object_unref(redSink); gst_object_unref(blueSink); gst_object_unref(red); gst_object_unref(blue);
    QFile result(QDir(evidence).filePath("result.json")); if(!result.open(QIODevice::WriteOnly)) return 6;
    result.write(QJsonDocument(QJsonObject{{"passed",ok},{"fourK",fourK},{"senderRecovered",isolated},{"waitingLabelPixels",white}}).toJson());
    qInfo()<<(ok?"PASS":"FAIL")<<"mac virtual camera ON AIR integration";
    return ok?0:1;
}
