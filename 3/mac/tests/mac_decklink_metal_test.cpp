#include "decklink.h"
#include "program_gpu_compositor.h"
#include "controller_settings.h"
#include <QApplication>
#include <QWidget>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QLibrary>
#include <QScopeGuard>
#include <QThread>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <cstdio>
#include <functional>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
bool waitFor(const std::function<bool()> &f, int ms = 5000) {
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<ms) { QApplication::processEvents(); if(f())return true;QThread::msleep(10); }
    return f();
}
int main(int argc,char **argv) {
    QApplication app(argc,argv);gst_init(nullptr,nullptr);CHECK(argc==2);
    QTemporaryDir temporary;CHECK(temporary.isValid());
    app.setProperty("vtrpon.settingsFile",temporary.filePath("settings.ini"));
    qputenv("VTRPON_DECKLINK_ADDON_FILE",QByteArray(argv[1]));qputenv("VTRPON_DECKLINK_ALLOW_TEST_ADDON","1");
    QLibrary library(QString::fromLocal8Bit(argv[1]));CHECK(library.load());
    auto expect=reinterpret_cast<void(*)(unsigned,uint32_t)>(library.resolve("vtrpon_decklink_test_expect"));
    auto stats=reinterpret_cast<void(*)(uint64_t*)>(library.resolve("vtrpon_decklink_test_outputs"));CHECK(expect&&stats);
    // Half-alpha red over blue yields purple Composite, straight red Fill,
    // and a half-white Key. The mock checks first and last pixels of each frame.
    expect(0,0xff80007f);expect(1,0xffff0000);expect(2,0xff808080);
    vtrpon::DeckLinkSettings settings;settings.outputEnabled=true;settings.fillEnabled=settings.keyEnabled=true;
    settings.outputChannel="decklink://100/0/1/0";settings.fillChannel="decklink://100/2/1/0";settings.keyChannel="decklink://100/3/1/0";
    settings.outputMode=settings.fillMode=settings.keyMode=1001;vtrpon::saveDeckLinkSettings(settings);
    vtrpon::DeckLinkRuntime runtime;
    const auto cleanup=qScopeGuard([&]{runtime.requestShutdown();waitFor([&]{return runtime.shutdownComplete();});});
    CHECK(waitFor([]{return vtrpon::deckLinkOutputMask()==7;}));
    QWidget output,monitor;output.resize(320,180);monitor.resize(320,180);output.show();monitor.show();
    ProgramGpuCompositor compositor(output.winId(),monitor.winId());CHECK(compositor.start());
    const auto stop=qScopeGuard([&]{compositor.prepareShutdown();compositor.stop();});
    auto *pipeline=gst_parse_launch("appsrc name=input is-live=true format=time do-timestamp=true block=false max-buffers=2 leaky-type=downstream ! video/x-raw,format=BGRA,width=1920,height=1080,framerate=60/1 ! appsink name=frames max-buffers=2 drop=true sync=false async=false wait-on-eos=false",nullptr);CHECK(pipeline);
    auto *src=gst_bin_get_by_name(GST_BIN(pipeline),"input"),*sink=gst_bin_get_by_name(GST_BIN(pipeline),"frames");
    const auto release=qScopeGuard([&]{compositor.clearDeckFrame(0);gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(src);gst_object_unref(sink);gst_object_unref(pipeline);});
    CHECK(compositor.configureDeckInput(0,pipeline,sink));compositor.setDeckLayers(0,1,-1,0);compositor.setBackgroundColor(0x0000ff);
    gst_element_set_state(pipeline,GST_STATE_PLAYING);
    QByteArray image(1920*1080*4,'\0');for(qsizetype n=0;n<image.size();n+=4){image[n+2]=char(255);image[n+3]=char(128);}
    auto *buffer=gst_buffer_new_allocate(nullptr,image.size(),nullptr);gst_buffer_fill(buffer,0,image.constData(),image.size());CHECK(gst_app_src_push_buffer(GST_APP_SRC(src),buffer)==GST_FLOW_OK);
    uint64_t routed[21]{};
    CHECK(waitFor([&]{stats(routed);return routed[0]>0&&routed[6]>0&&routed[12]>0&&routed[8]==routed[14];}));
    CHECK(routed[3]==0xff80007f&&routed[9]==0xffff0000&&routed[15]==0xff808080);
    const auto capture=compositor.captureStillFrame()();CHECK(!capture.isNull());
    const auto pixel=capture.pixelColor(960,540);CHECK(std::abs(pixel.red()-128)<=1&&std::abs(pixel.blue()-127)<=1);
    buffer=gst_buffer_new_allocate(nullptr,480*8,nullptr);gst_buffer_memset(buffer,0,0,480*8);vtrpon::publishDeckLinkAudio(buffer);gst_buffer_unref(buffer);
    CHECK(waitFor([&]{stats(routed);return routed[1]>0&&routed[7]>0;}));CHECK(!routed[13]);
    const auto opens=routed[18];settings.premultipliedFill=true;expect(1,0xff800000);vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([&]{stats(routed);return routed[9]==0xff800000&&routed[6]>0;}));CHECK(routed[18]==opens);
    expect(0,0xff0000ff);expect(1,0xff000000);expect(2,0xff000000);compositor.setFtbCoverage(1);
    CHECK(waitFor([&]{stats(routed);return routed[3]==0xff0000ff&&routed[9]==0xff000000&&routed[15]==0xff000000;}));
    const auto rendered=compositor.renderedFrameCount();
    qputenv("VTRPON_DECKLINK_TEST_FAIL_KEY","1");
    CHECK(waitFor([]{return !vtrpon::deckLinkState().outputs[2].error.isEmpty();}));
    CHECK(waitFor([&]{return compositor.renderedFrameCount()>rendered+5;}));
    qunsetenv("VTRPON_DECKLINK_TEST_FAIL_KEY");
    settings.outputEnabled=false;vtrpon::saveDeckLinkSettings(settings);CHECK(waitFor([]{return !vtrpon::deckLinkOutputMask();}));
    std::puts("Mac production Metal Composite/Fill/Key pixels, same frame/time, FTB, premultiplied fill, audio routing, fault isolation and stop: PASS");return 0;
}
