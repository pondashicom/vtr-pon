#include "network_video.h"
#include "media_timecode.h"
#include "addon_platform.h"
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include "network_video_settings_page.h"
#include "decklink_settings_page.h"
#include "operator_monitor_output.h"
#include "operator_monitor_transport_mac.h"
#include "program_gpu_compositor.h"
#include "operator_monitor_settings_page.h"
#include "virtual_camera/output.h"
#include "controller_settings_io.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QTranslator>
#include <QLibrary>
#include <QJsonDocument>
#include <QJsonArray>
#include <QThread>
#include <QElapsedTimer>
#include <gst/gst.h>
#include <sys/mman.h>
#include <QScopeGuard>
#include <atomic>
#include <thread>
#include <cstdio>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #value); return 1; } } while(false)
template<class F> bool waitFor(F f, int milliseconds = 5000) {
    QElapsedTimer timer; timer.start();
    do { QApplication::processEvents(); if (f()) return true; QThread::msleep(10); } while (timer.elapsed() < milliseconds);
    return false;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv); gst_init(nullptr, nullptr);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir directory; CHECK(directory.isValid());
    app.setProperty("vtrpon.settingsFile",directory.filePath("settings.ini"));
    if(argc==2 && QString::fromLocal8Bit(argv[1])=="--absent") {
        qputenv("VTRPON_DECKLINK_ADDON_FILE","/nonexistent/vtr-return.dylib");
        OperatorMonitorSettingsPage page;
        CHECK(!page.findChild<QCheckBox *>("operatorMonitorNdiOutput")->isEnabled());
        CHECK(!page.findChild<QCheckBox *>("operatorMonitorDeckLinkOutput")->isEnabled());
        return 0;
    }
    CHECK(argc == 3 || argc == 4);
    const auto installed=vtrpon::networkVideoAddonPath(); CHECK(!QFileInfo::exists(installed));
    CHECK(QDir().mkpath(QFileInfo(installed).absolutePath()) && QFile::copy(QString::fromLocal8Bit(argv[1]),installed));
    const auto removeFixture=qScopeGuard([&] { QFile::remove(installed); QDir().rmdir(QFileInfo(installed).absolutePath()); QDir().rmdir(vtrpon::addons::installationDirectory(app.applicationDirPath())); });
    qputenv("VTRPON_DECKLINK_ADDON_FILE", argv[2]);
    qputenv("VTRPON_DECKLINK_ALLOW_TEST_ADDON", "1");
    std::fprintf(stderr, "Return routing: application ready\n");
    QTranslator translator; CHECK(translator.load(QString::fromLocal8Bit(VTRPON_RETURN_TRANSLATION_FILE))); app.installTranslator(&translator);
    vtrpon::reloadVirtualCameraEnabled();
    CHECK(!vtrpon::savedNetworkVideoSettings().returnMonitorEnabled);
    CHECK(!vtrpon::savedDeckLinkSettings().returnMonitorEnabled);
    CHECK(!vtrpon::virtualCameraReturnMonitor());
    OperatorMonitorSettingsPage monitorPage;
    NetworkVideoSettingsPage ndiPage;
    DeckLinkSettingsPage deckPage;
    std::fprintf(stderr, "Return routing: settings pages ready\n");
    monitorPage.changed = [&] { ndiPage.reload(); deckPage.reload(); };
    ndiPage.changed = [&] { monitorPage.reload(); };
    deckPage.changed = [&] { monitorPage.reload(); };
    auto *monitorNdi = monitorPage.findChild<QCheckBox *>("operatorMonitorNdiOutput");
    auto *ndiReturn = ndiPage.findChild<QCheckBox *>("networkVideoReturnMonitorEnabled");
    auto *ndiComposite = ndiPage.findChild<QCheckBox *>("networkVideoCompositeEnabled");
    auto *monitorWindow = monitorPage.findChild<QCheckBox *>("operatorMonitorWindowOutput");
    auto *monitorCamera = monitorPage.findChild<QCheckBox *>("operatorMonitorVirtualCameraOutput");
    CHECK(monitorNdi && ndiReturn && ndiComposite && monitorWindow && monitorCamera);
    CHECK(monitorNdi->isEnabled() && !monitorNdi->isChecked());
    monitorNdi->setChecked(true);
    CHECK(ndiReturn->isChecked() && !ndiComposite->isChecked()); // Checking return alone starts only return.
    ndiReturn->setChecked(false); CHECK(!monitorNdi->isChecked());
    ndiReturn->setChecked(true); CHECK(monitorNdi->isChecked());
    monitorWindow->setChecked(true); monitorPage.reload(); CHECK(monitorWindow->isChecked());
    monitorWindow->setChecked(false);
    monitorCamera->setChecked(true); CHECK(vtrpon::virtualCameraEnabled() && vtrpon::virtualCameraReturnMonitor());
    monitorCamera->setChecked(false); CHECK(!vtrpon::virtualCameraEnabled());
    CHECK(vtrpon::setVirtualCameraReturnMonitor(false));
    std::fprintf(stderr, "Return routing: settings linked\n");
    {
        auto store = vtrpon::makeControllerSettings();
        store.setValue("operatorMonitor/windowEnabled", true);
        store.setValue("virtualCamera/returnMonitor", true);
        store.setValue("deckLink/returnMonitorEnabled", true);
        store.setValue("deckLink/returnMonitorChannel", "decklink://100/4/1/0");
        store.setValue("deckLink/returnMonitorMode", "1001");
        store.sync();
        const auto snapshot = vtrpon::controllerSettingsSnapshot(store);
        CHECK(snapshot.value("networkVideo/returnMonitorEnabled").toBool());
        CHECK(snapshot.value("deckLink/returnMonitorEnabled").toBool());
        CHECK(snapshot.value("operatorMonitor/windowEnabled").toBool());
        CHECK(snapshot.value("virtualCamera/returnMonitor").toBool());
        const auto path = directory.filePath("export.json");
        CHECK(vtrpon::exportControllerSettings(store, path));
        CHECK(vtrpon::replaceControllerSettings(store, {}));
        CHECK(!store.contains("deckLink/returnMonitorChannel"));
        CHECK(vtrpon::importControllerSettings(store, path));
        CHECK(vtrpon::controllerSettingsSnapshot(store) == snapshot);
        auto invalid = snapshot; invalid.insert("deckLink/returnMonitorMode", "4294967296");
        CHECK(!vtrpon::replaceControllerSettings(store, invalid));
        invalid = snapshot; invalid.insert("deckLink/returnMonitorChannel", "not a channel");
        CHECK(!vtrpon::replaceControllerSettings(store, invalid));
        store.setValue("operatorMonitor/windowEnabled", false);
        store.setValue("virtualCamera/returnMonitor", false); store.sync();
    }
    ndiPage.resize(680, 780); monitorPage.resize(680, 780); deckPage.resize(680, 1000);
    std::fprintf(stderr, "Return routing: snapshot controls\n");
    ndiPage.grab().save(directory.filePath("ndi.png"));
    monitorPage.grab().save(directory.filePath("monitor.png"));
    const auto capturePath = qEnvironmentVariable("VTRPON_RETURN_TEST_CAPTURES");
    if (!capturePath.isEmpty()) {
        QDir().mkpath(capturePath);
        ndiPage.grab().save(capturePath + "/ndi.png"); monitorPage.grab().save(capturePath + "/monitor.png"); deckPage.grab().save(capturePath + "/decklink.png");
    }
    auto settings = vtrpon::savedNetworkVideoSettings();
    settings.outputEnabled = settings.returnMonitorEnabled = settings.compositeEnabled = settings.inputEnabled = true;
    settings.outputName = "Return-test";
    vtrpon::saveNetworkVideoSettings(settings); CHECK(vtrpon::savedNetworkVideoSettings() == settings);
    CHECK(vtrpon::networkVideoOutputName(settings, vtrpon::NetworkVideoKind::ReturnMonitor) == "Return-test Return Monitor");
    QLibrary fixture(vtrpon::networkVideoAddonPath()); CHECK(fixture.load());
    const auto stats = reinterpret_cast<void (*)(uint64_t *)>(fixture.resolve("vtrpon_return_test_stats")); CHECK(stats);
    vtrpon::NetworkVideoRuntime runtime;
    std::fprintf(stderr, "Return routing: NDI runtime started\n");
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask() == 17u; }));
    vtrpon::OperatorMonitorControlFeed feed;
    const QJsonObject presentation{{"information", true}, {"fields", QJsonArray{QJsonObject{{"key", "file"}, {"label", "FILE"}, {"value", "Return-only information"}}}}};
    feed.publish(QJsonDocument(presentation).toJson(QJsonDocument::Compact));
    QImage image(64,36,QImage::Format_ARGB32); image.fill(Qt::red);
    const QByteArray red(reinterpret_cast<const char *>(image.constBits()),image.sizeInBytes());
    uint64_t records[20]{};
    const auto publish = [&] {
        const auto generation=vtrpon::networkVideoOutputGeneration();
        vtrpon::publishNetworkVideo(red,{},64,36,generation,vtrpon::mediaTimecode100ns());
        vtrpon::publishOperatorMonitorFrame(image,1,generation,0,vtrpon::mediaTimecode100ns()); stats(records);
    };
    CHECK(waitFor([&] { publish(); return records[0] > 0 && records[10] > 0; }));
    CHECK(records[1] == 64 && records[2] == 36 && records[3] == 60 && records[4] == 0xffff0000u);
    CHECK(records[11] == 1920 && records[12] == 1080 && records[13] == 24);
    CHECK(records[14] == 0xff000000u && records[15] == 0xffff0000u && records[16] > 0 && records[19] == 0);
    // A separate receiver holding the window mailbox does not stop external return output.
    std::atomic_bool locked{false}, release{false};
    std::jthread holder([&] {
        auto *shared=static_cast<vtrpon::MacOperatorMonitorShared *>(mmap(nullptr,sizeof(vtrpon::MacOperatorMonitorShared),PROT_READ|PROT_WRITE,MAP_SHARED,feed.sharedDescriptor(),0));
        if(shared==MAP_FAILED) return;
        if(pthread_mutex_lock(&shared->videoMutex)) { munmap(shared,sizeof(*shared)); return; }
        locked=true; while(!release) QThread::msleep(5);
        pthread_mutex_unlock(&shared->videoMutex); munmap(shared,sizeof(*shared));
    });
    vtrpon::setOperatorMonitorEnabled(true);
    const bool gotLock = waitFor([&] { return locked.load(); });
    const auto before = records[10];
    const bool continued = gotLock && waitFor([&] { publish(); return records[10] > before + 2; });
    release = true; holder.join(); CHECK(gotLock && continued);
    vtrpon::setOperatorMonitorEnabled(false);
    auto *audio = gst_buffer_new_allocate(nullptr, 800 * 8, nullptr); gst_buffer_memset(audio, 0, 0, 800 * 8);
    vtrpon::publishNetworkAudio(audio); gst_buffer_unref(audio);
    CHECK(waitFor([&] { stats(records); return records[8] && records[18]; }));
    CHECK(!vtrpon::networkVideoInputAllowed(vtrpon::networkVideoSourcePath("TEST HOST (Return-test Return Monitor)")));
    const auto oldGeneration = vtrpon::networkVideoOutputGeneration();
    settings.returnMonitorEnabled = false; vtrpon::saveNetworkVideoSettings(settings);
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask() == 1u; }));
    stats(records); const auto stopped = records[10];
    settings.returnMonitorEnabled = true; vtrpon::saveNetworkVideoSettings(settings);
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask() == 17u; }));
    vtrpon::publishNetworkReturnMonitor(QByteArray(64 * 36 * 4, '\0'), 64, 36, oldGeneration, 123);
    QThread::msleep(60); stats(records); CHECK(records[10] == stopped);
    CHECK(waitFor([&] { publish(); return records[10] > stopped; }));
    // The production Metal path must publish returns without a window/IPC receiver.
    vtrpon::DeckLinkSettings deck; deck.outputEnabled=deck.returnMonitorEnabled=true;
    deck.outputChannel="decklink://100/0/1/0"; deck.returnMonitorChannel="decklink://100/4/1/0";
    deck.outputMode=deck.returnMonitorMode=1001; vtrpon::saveDeckLinkSettings(deck);
    QLibrary deckFixture(QString::fromLocal8Bit(argv[2])); CHECK(deckFixture.load());
    const auto deckStats=reinterpret_cast<void(*)(uint64_t*)>(deckFixture.resolve("vtrpon_decklink_test_outputs"));
    const auto returnStats=reinterpret_cast<void(*)(uint64_t*)>(deckFixture.resolve("vtrpon_decklink_test_return_output"));
    const auto expect=reinterpret_cast<void(*)(unsigned,uint32_t)>(deckFixture.resolve("vtrpon_decklink_test_expect")); CHECK(deckStats && returnStats && expect);
    expect(0,0xffff0000); expect(3,0xff000000);
    vtrpon::DeckLinkRuntime deckRuntime;
    const auto cleanup=qScopeGuard([&] { deckRuntime.requestShutdown(); waitFor([&] { return deckRuntime.shutdownComplete(); }); });
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask()==9u; }));
    QWidget output,monitor; output.resize(320,180); monitor.resize(320,180); output.show(); monitor.show();
    ProgramGpuCompositor compositor(output.winId(),monitor.winId(),argc==4 ? ProgramGpuCompositor::OutputGeometry{3840,2160} : ProgramGpuCompositor::OutputGeometry{1920,1080});
    compositor.setBackgroundColor(0xff0000); CHECK(compositor.start());
    const auto stop=qScopeGuard([&] { compositor.prepareShutdown(); compositor.stop(); });
    uint64_t routed[21]{}, returned[5]{};
    CHECK(waitFor([&] { stats(records); deckStats(routed); returnStats(returned); return records[1]==(argc==4 ? 3840u : 1920u) && records[10]>stopped && routed[0] && returned[0]; }));
    CHECK(records[14]==0xff000000u && records[15]==0xffff0000u && records[16]>0);
    CHECK(routed[3]==0xffff0000u && returned[3]==0xff000000u && returned[2]>0);
    auto *pcm=gst_buffer_new_allocate(nullptr,800*8,nullptr); gst_buffer_memset(pcm,0,0,800*8); vtrpon::publishDeckLinkAudio(pcm); gst_buffer_unref(pcm);
    CHECK(waitFor([&] { deckStats(routed); returnStats(returned); return routed[1] && returned[1]; }));
    settings.compositeEnabled=false; vtrpon::saveNetworkVideoSettings(settings);
    deck.compositeEnabled=false; vtrpon::saveDeckLinkSettings(deck);
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask()==16u && vtrpon::deckLinkOutputMask()==8u; }));
    QThread::msleep(100); stats(records); deckStats(routed); returnStats(returned);
    const auto stoppedAir=records[0], stoppedDeck=routed[0], previousReturn=returned[0];
    CHECK(waitFor([&] { returnStats(returned); return returned[0]>previousReturn+2; }));
    stats(records); deckStats(routed); CHECK(records[0]==stoppedAir && routed[0]==stoppedDeck);
    settings.compositeEnabled=true; vtrpon::saveNetworkVideoSettings(settings);
    deck.compositeEnabled=true; vtrpon::saveDeckLinkSettings(deck);
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask()==17u && vtrpon::deckLinkOutputMask()==9u; }));
    const auto rendered=compositor.renderedFrameCount();
    deck.returnMonitorChannel=deck.outputChannel; vtrpon::saveDeckLinkSettings(deck);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask()==1u && !vtrpon::deckLinkState().outputs[3].error.isEmpty(); }));
    CHECK(waitFor([&] { return compositor.renderedFrameCount()>rendered+5; }));
    const auto oldDeckGeneration=vtrpon::deckLinkOutputGeneration();
    deck.returnMonitorChannel="decklink://100/4/1/0"; vtrpon::saveDeckLinkSettings(deck);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask()==9u; }));
    // Stop rendering to observe stale publication rejection with no competing new frames.
    compositor.prepareShutdown(); compositor.stop();
    QThread::msleep(100); returnStats(returned); const auto beforeStale=returned[0];
    vtrpon::publishDeckLinkReturnMonitor(red,64,36,oldDeckGeneration,vtrpon::mediaTimecode100ns());
    QThread::msleep(100); returnStats(returned); CHECK(returned[0]==beforeStale);
    deck.outputEnabled=false; vtrpon::saveDeckLinkSettings(deck);
    CHECK(waitFor([] { return !vtrpon::deckLinkOutputMask(); }));
    // Failed saves preserve the active camera source.
    const auto oldSource=vtrpon::virtualCameraReturnMonitor();
    app.setProperty("vtrpon.settingsFile",directory.filePath("export.json/invalid.ini"));
    CHECK(!vtrpon::setVirtualCameraReturnMonitor(!oldSource) && vtrpon::virtualCameraReturnMonitor()==oldSource);
    app.setProperty("vtrpon.settingsFile",directory.filePath("settings.ini"));
    runtime.requestShutdown(); CHECK(waitFor([&] { return runtime.shutdownComplete(); }));
    std::puts("PASS: Mac linked settings/import/export/failures, production Metal ON AIR+return NDI/DeckLink pixels, overlays, audio, window mutex isolation, duplicate channels, generations, shutdown");
    return 0;
}
