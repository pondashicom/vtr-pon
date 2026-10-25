#pragma once

#include <QDateTime>
#include <QWindow>
#include <QFileInfo>
#include <QDir>

// Opt-in hardware measurement. Uses the production compositor and real H.264
// inputs; the additional window exercises DWM composition without automating
// another application or changing the user's display settings.
int measureFullscreenCadence(QApplication &app) {
  gst_init(nullptr, nullptr);
  QScreen *output = app.primaryScreen();
  for (auto *screen : app.screens()) if (screen != app.primaryScreen()) { output = screen; break; }
  QWidget primary, monitor, cover;
  primary.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  primary.setWindowTitle("VTR-PON fullscreen cadence probe");
  primary.winId();
  primary.windowHandle()->setScreen(output);
  primary.setGeometry(output->geometry());
  primary.show();
  primary.raise();
  monitor.setWindowTitle("VTR-PON cadence ON AIR monitor");
  monitor.setGeometry(app.primaryScreen()->geometry().topLeft().x()+40,
                      app.primaryScreen()->geometry().topLeft().y()+60, 640, 360);
  monitor.show();
  cover.setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
  cover.setWindowTitle("VTR-PON cadence overlap fixture");
  cover.setGeometry(output->geometry().x()+100, output->geometry().y()+100, 450, 260);
  cover.setStyleSheet("background:#304050");
  ProgramGpuCompositor compositor(primary.winId(), monitor.winId());
  CHECK(compositor.start(), "cadence production compositor starts");
  std::array<GstElement *,2> inputs{};
  for (int deck=0; deck<2; ++deck) {
    GError *error=nullptr;
    inputs[deck]=gst_parse_launch(
        "filesrc name=source ! qtdemux ! h264parse ! d3d11h264dec ! d3d11convert ! "
        "video/x-raw(memory:D3D11Memory),format=BGRA ! "
        "appsink name=video sync=true max-buffers=1 drop=true", &error);
    CHECK(inputs[deck] && !error, "cadence input creates");
    auto *source=gst_bin_get_by_name(GST_BIN(inputs[deck]), "source");
    const auto path=QDir(app.applicationDirPath()).filePath("fullscreen-fps-1080p60.mp4").toUtf8();
    CHECK(QFileInfo::exists(QString::fromUtf8(path)), "60 FPS moving fixture exists");
    g_object_set(source,"location",path.constData(),nullptr);
    gst_object_unref(source);
    auto *sink=gst_bin_get_by_name(GST_BIN(inputs[deck]),"video");
    CHECK(compositor.configureDeckInput(deck,inputs[deck],sink), "cadence input connects");
    gst_object_unref(sink);
    CHECK(gst_element_set_state(inputs[deck],GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,
          "cadence decode starts");
  }
  CHECK(waitFor([&]{return compositor.deckFrameReady(0)&&compositor.deckFrameReady(1);}),
        "both 60 FPS inputs ready");
  compositor.setDeckLayers(0,1.0f,-1,0.0f);
  auto lastPresents=compositor.presentedFrameCount();
  auto lastRenders=compositor.renderedFrameCount();
  QElapsedTimer timer; timer.start();
  qint64 previous=0;
  // 3 repeatable phases: uncovered, overlapping window, uncovered.
  for (int second=1; second<=60; ++second) {
    if(second==21) cover.show();
    if(second==41) {cover.hide(); primary.raise();}
    if(second%8==0) for(auto *input:inputs)
      gst_element_seek_simple(input,GST_FORMAT_TIME,
          GstSeekFlags(GST_SEEK_FLAG_FLUSH|GST_SEEK_FLAG_KEY_UNIT),0);
    CHECK(waitFor([&]{
      const int t=int(timer.elapsed()%8000);
      const bool swap=(timer.elapsed()/8000)%2;
      if(t<1000) compositor.setDeckLayers(swap?1:0,float(t)/1000,swap?0:1,1.0f);
      else compositor.setDeckLayers(swap?1:0,1.0f,-1,0.0f);
      if(second>20&&second<=40) cover.update();
      return timer.elapsed()>=second*1000;
    },1500),"cadence interval completes");
    const auto now=timer.elapsed();
    const double seconds=double(now-previous)/1000;
    const auto presents=compositor.presentedFrameCount(), renders=compositor.renderedFrameCount();
    std::cout<<"CADENCE second="<<second<<" utc="<<QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()
             <<" present="<<double(presents-lastPresents)/seconds
             <<" render="<<double(renders-lastRenders)/seconds<<std::endl;
    lastPresents=presents; lastRenders=renders; previous=now;
  }
  for(auto *input:inputs) {gst_element_set_state(input,GST_STATE_NULL);gst_object_unref(input);}
  CHECK(compositor.stop(), "cadence compositor stops normally");
  return 0;
}
