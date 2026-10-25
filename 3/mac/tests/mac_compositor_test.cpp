#include "program_gpu_compositor.h"
#include "dsk_playback.h"
#include <QTemporaryDir>
#include <QPainter>
#include <QApplication>
#include <QWidget>
#include <QElapsedTimer>
#include <QThread>
#include <QDebug>
#include <gst/gst.h>
#include <cmath>

static void pump(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    gst_init(&argc, &argv);
    QWidget output, monitor;
    output.resize(640,360); monitor.resize(320,180);
    output.show(); monitor.show(); pump(100);
    ProgramGpuCompositor c(output.winId(), monitor.winId());
    if (!c.start()) return 1;
    GError *error = nullptr;
    auto pipeline = gst_parse_launch("videotestsrc pattern=red ! video/x-raw,format=BGRA,width=1440,height=1080,framerate=30/1 ! appsink name=frames max-buffers=2 sync=true wait-on-eos=false", &error);
    if (!pipeline || error) return 2;
    auto sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
    bool ok = c.configureDeckInput(0,pipeline,sink);
    gst_element_set_state(pipeline, GST_STATE_PAUSED); pump(400);
    ok &= c.deckFrameReady(0);
    c.setDeckLayers(0,1,-1,0); c.setBackgroundColor(0x0000ff); pump(200);
    auto check = [&](const char *name, int x, int y, QColor expected) {
        auto capture = c.captureStillFrame();
        QImage frame = capture ? capture() : QImage();
        QColor pixel = frame.isNull() ? QColor() : frame.pixelColor(x,y);
        bool match = frame.size() == QSize(1920,1080) && std::abs(pixel.red()-expected.red())<=2 && std::abs(pixel.green()-expected.green())<=2 && std::abs(pixel.blue()-expected.blue())<=2;
        qInfo() << name << match << pixel;
        ok &= match;
    };
    check("preroll red center",960,540,QColor(255,0,0));
    check("aspect padding blue",10,540,QColor(0,0,255));
    c.setFtbCoverage(.5F); pump(150); check("half fade over blue",960,540,QColor(128,0,128));
    c.setFtbCoverage(1); pump(150); check("full fade to background",960,540,QColor(0,0,255));
    c.setBackgroundColor(0); pump(150); check("full FTB black",960,540,QColor(0,0,0));
    c.setFtbCoverage(0); gst_element_set_state(pipeline,GST_STATE_PLAYING); pump(300);
    monitor.hide(); auto before=c.presentedFrameCount(); pump(400);
    ok &= c.presentedFrameCount()>before;
    output.hide(); before=c.renderedFrameCount(); pump(400);
    ok &= c.renderedFrameCount()>before;
    monitor.show(); output.show(); pump(300);
    check("resumed presentation",960,540,QColor(255,0,0));
    c.suspendDeckFrameUpdatesAndClear(0); pump(150);
    check("clear while playing",960,540,QColor(0,0,0));
    c.setDeckFrameUpdatesEnabled(0,true); pump(150);
    check("resume frames",960,540,QColor(255,0,0));
    auto incoming = gst_parse_launch("videotestsrc pattern=blue ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=25/1 ! appsink name=frames max-buffers=2 sync=true wait-on-eos=false", &error);
    if (!incoming || error) return 2;
    auto incomingSink = gst_bin_get_by_name(GST_BIN(incoming), "frames");
    ok &= c.configureDeckInput(1,incoming,incomingSink);
    gst_element_set_state(incoming,GST_STATE_PAUSED); pump(400);
    ok &= c.deckFrameReady(1);
    gst_element_set_state(incoming,GST_STATE_PLAYING);
    // Match Windows: lower deck at (1-p), then upper deck at p with alpha-over.
    ok &= c.setDeckLayers(1,0,0,1); pump(150);
    check("crossfade start",960,540,QColor(255,0,0));
    ok &= c.setDeckLayers(1,.25F,0,.75F); pump(150);
    check("crossfade quarter",960,540,QColor(143,0,64));
    ok &= c.setDeckLayers(1,.5F,0,.5F); pump(150);
    check("crossfade midpoint",960,540,QColor(64,0,128));
    check("crossfade source aspect padding",10,540,QColor(0,0,128));
    c.setBackgroundColor(0x00ff00); pump(150);
    check("crossfade coverage over background",960,540,QColor(64,64,128));
    c.setBackgroundColor(0); c.setItemFadeProgress(.5F); pump(150);
    check("item fade scales mixed group once",960,540,QColor(32,0,64));
    c.setFtbCoverage(.5F); pump(150);
    check("FTB scales mixed group",960,540,QColor(16,0,32));
    c.setItemFadeProgress(1); c.setFtbCoverage(0);
    monitor.hide(); before=c.presentedFrameCount(); pump(300);
    ok &= c.presentedFrameCount()>before;
    output.hide(); before=c.renderedFrameCount(); pump(300);
    ok &= c.renderedFrameCount()>before;
    monitor.show(); output.show();
    ok &= c.setDeckLayers(1,1,0,0); pump(150);
    check("crossfade completed",960,540,QColor(0,0,255));
    ok &= c.suspendDeckFrameUpdatesAndClear(0); pump(150);
    check("outgoing cleared after crossfade",960,540,QColor(0,0,255));
    // Reuse the retired physical deck for the next transition back.
    c.setDeckFrameUpdatesEnabled(0,true); pump(150);
    ok &= c.setDeckLayers(0,.5F,1,.5F); pump(150);
    check("crossfade back through reused deck",960,540,QColor(128,0,64));
    ok &= c.setDeckLayers(0,1,-1,0); pump(150);
    // Exercise the real video-only DSK producers, not just compositor setters.
    DskPlayback dsk;
    ok &= dsk.initialize();
    dsk.attachCompositor(&c);
    for (int i = 0; i < 2; ++i) ok &= c.configureDskInput(i, dsk.pipeline(i), dsk.appSink(i));
    QTemporaryDir dir;
    QImage overlay(960,540,QImage::Format_ARGB32);
    overlay.fill(Qt::transparent);
    { QPainter painter(&overlay); painter.fillRect(380,220,200,100,QColor(0,255,0,128)); }
    const auto png = dir.filePath(QStringLiteral("transparent.png"));
    ok &= overlay.save(png);
    auto waitCue = [&] {
        QElapsedTimer timer; timer.start();
        while (dsk.cueStatus() == DskPlayback::DskStatus::Loading && timer.elapsed() < 11000) {
            dsk.pollBus(); pump(10);
        }
        const bool ready = dsk.cueStatus() == DskPlayback::DskStatus::Ready;
        qInfo() << "DSK cue ready" << ready << dsk.errorDetail();
        ok &= ready;
        return ready;
    };
    auto waitRetirement = [&] {
        QElapsedTimer timer; timer.start();
        while (dsk.retirementPending() && timer.elapsed() < 3000) { dsk.pollBus(); pump(10); }
        dsk.pollBus();
        ok &= !dsk.retirementPending();
    };
    ok &= dsk.cue(png);
    if (waitCue()) ok &= dsk.takeCue(false,false,1);
    pump(200);
    check("transparent DSK over base",960,540,QColor(127,128,0));
    check("transparent DSK keeps base",600,300,QColor(255,0,0));
    ok &= c.setDeckLayers(1,.5F,0,.5F); pump(150);
    check("DSK over two video crossfade",960,540,QColor(32,128,64));
    c.setFtbCoverage(.5F); pump(150);
    check("FTB over DSK and two videos",960,540,QColor(16,64,32));
    c.setFtbCoverage(0); ok &= c.setDeckLayers(0,1,-1,0); pump(150);
    ok &= dsk.setVideoOpacity(.5F); pump(150);
    check("DSK opacity",960,540,QColor(191,64,0));
    ok &= dsk.setVideoOpacity(1);
    c.setItemFadeProgress(0); pump(150);
    check("item fade leaves DSK",960,540,QColor(0,128,0));
    c.setFtbCoverage(.5F); pump(150);
    check("FTB fades DSK group",960,540,QColor(0,64,0));
    c.setFtbCoverage(1); pump(150);
    check("FTB hides DSK",960,540,QColor(0,0,0));
    c.setFtbCoverage(0); c.setItemFadeProgress(1);
    // Incoming DSK is above outgoing DSK; both retain source alpha.
    overlay.fill(Qt::blue);
    const auto bluePng = dir.filePath(QStringLiteral("blue.png"));
    ok &= overlay.save(bluePng);
    ok &= dsk.cue(bluePng);
    if (waitCue()) ok &= dsk.takeCue(false,true,1);
    ok &= dsk.setCrossfadeProgress(.5F); pump(150);
    check("DSK crossfade midpoint",960,540,QColor(95,32,128));
    ok &= dsk.setCrossfadeProgress(1); pump(150);
    check("DSK crossfade completed",960,540,QColor(0,0,255));
    ok &= dsk.clear(); waitRetirement(); pump(150);
    check("DSK off restores base",960,540,QColor(255,0,0));
    // A video carrying an audio track stays video-only and advances independently.
    ok &= dsk.cue(QString::fromUtf8(VTRPON_TEST_MEDIA_DIR "/Color_Bars_Motion_1080p30_180s.mp4"),1000,2000);
    if (waitCue()) ok &= dsk.takeCue(true,false,1);
    pump(200); const auto position = dsk.positionMs(); pump(250);
    ok &= dsk.isPlaying() && position >= 1000 && dsk.positionMs() > position;
    for (int i = 0; i < 2; ++i) {
        guint flags = 0; gboolean mute = FALSE; gdouble volume = 1; GstElement *audio = nullptr;
        g_object_get(dsk.pipeline(i),"flags",&flags,"mute",&mute,"volume",&volume,"audio-sink",&audio,nullptr);
        const auto *factory = audio ? gst_element_get_factory(audio) : nullptr;
        const char *name = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : nullptr;
        const bool isolated = !(flags & (1U<<1)) && mute && volume == 0 && g_strcmp0(name,"fakesink")==0;
        qInfo() << "DSK audio isolation" << i << isolated;
        ok &= isolated;
        if (audio) gst_object_unref(audio);
    }
    ok &= dsk.pause(); pump(100); const auto paused = dsk.positionMs(); pump(150);
    ok &= !dsk.isPlaying() && std::abs(dsk.positionMs()-paused) < 35;
    ok &= dsk.restartFromIn(true); pump(150);
    ok &= dsk.positionMs() >= 1000 && dsk.positionMs() < 1500;
    pump(1100); dsk.pollBus(); QString detail;
    ok &= dsk.takeEvent(detail) == DskPlayback::DskEvent::EndOfStream;
    // A failed replacement must preserve the currently displayed DSK/base.
    ok &= dsk.cue(dir.filePath(QStringLiteral("missing.png")));
    for (int i = 0; i < 100 && dsk.cueStatus() == DskPlayback::DskStatus::Loading; ++i) { dsk.pollBus(); pump(10); }
    ok &= dsk.cueStatus() == DskPlayback::DskStatus::Error && dsk.status() == DskPlayback::DskStatus::On;
    ok &= c.isAvailable();
    ok &= dsk.clear(); waitRetirement(); pump(150);
    check("DSK failure keeps base available",960,540,QColor(255,0,0));
    // Leave a playing producer alive while the compositor detaches on shutdown.
    ok &= dsk.cue(png);
    if (waitCue()) ok &= dsk.takeCue(true,false,1);
    c.prepareShutdown();
    ok &= dsk.shutdown();
    gst_element_set_state(pipeline,GST_STATE_NULL);
    ok &= gst_element_get_state(pipeline,nullptr,nullptr,2*GST_SECOND) != GST_STATE_CHANGE_FAILURE;
    ok &= c.stop();
    gst_element_set_state(incoming,GST_STATE_NULL);
    ok &= gst_element_get_state(incoming,nullptr,nullptr,2*GST_SECOND) != GST_STATE_CHANGE_FAILURE;
    gst_object_unref(incomingSink); gst_object_unref(incoming);
    gst_object_unref(sink); gst_object_unref(pipeline);
    qInfo() << "Mac compositor test:" << (ok ? "PASS" : "FAIL");
    return ok ? 0 : 3;
}
