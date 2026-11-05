#include "network_video.h"
#include "network_video_settings_page.h"
#include "decklink_settings_page.h"
#include "operator_monitor_output.h"
#include "operator_monitor_transport.h"
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
#include <d3d11.h>
#include <wrl/client.h>
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
    std::fprintf(stderr, "Return routing: application ready\n");
    QTemporaryDir directory; CHECK(directory.isValid());
    app.setProperty("vtrpon.settingsFile", directory.filePath("settings.ini"));
    QTranslator translator; CHECK(translator.load(app.applicationDirPath() + "/../vtrpon_ja.qm")); app.installTranslator(&translator);
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
    const auto failDiscovery = reinterpret_cast<void (*)(int)>(fixture.resolve("vtrpon_return_test_discovery_failure"));
    const auto discoveryChecks = reinterpret_cast<unsigned (*)()>(fixture.resolve("vtrpon_return_test_discovery_checks"));
    CHECK(failDiscovery && discoveryChecks);
    vtrpon::NetworkVideoRuntime runtime;
    std::fprintf(stderr, "Return routing: NDI runtime started\n");
    CHECK(waitFor([] { return vtrpon::networkVideoOutputMask() == 17u; }));
    failDiscovery(1);
    CHECK(waitFor([] {
        const auto state = vtrpon::networkVideoState();
        return !state.inputRunning && state.error == QStringLiteral("NDI source discovery could not be started.");
    }));
    failDiscovery(0);
    CHECK(waitFor([] {
        const auto state = vtrpon::networkVideoState();
        return state.inputRunning && state.error.isEmpty();
    }));
    CHECK(waitFor([] {
        vtrpon::reportNetworkVideoOutputError(QStringLiteral("Test output path error"));
        return vtrpon::networkVideoState().error == QStringLiteral("Test output path error");
    }));
    const auto checked = discoveryChecks();
    // A second query proves the previous discovery result was committed.
    CHECK(waitFor([&] { return discoveryChecks() > checked + 1; }));
    CHECK(vtrpon::networkVideoState().error == QStringLiteral("Test output path error"));
    CHECK(waitFor([] {
        vtrpon::reportNetworkVideoOutputError({});
        return vtrpon::networkVideoState().error.isEmpty();
    }));
    vtrpon::OperatorMonitorControlFeed feed;
    const QJsonObject presentation{{"information", true}, {"fields", QJsonArray{QJsonObject{{"key", "file"}, {"label", "FILE"}, {"value", "Return-only information"}}}}};
    feed.publish(QJsonDocument(presentation).toJson(QJsonDocument::Compact));
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 64; desc.Height = 36;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> target;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    CHECK(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)));
    const float red[]{1, 0, 0, 1}; context->ClearRenderTargetView(target.Get(), red);
    vtrpon::NetworkVideoOutput onAir; vtrpon::OperatorMonitorOutput returns;
    uint64_t records[20]{};
    const auto publish = [&] { onAir.publish(target.Get()); returns.publish(target.Get()); context->Flush(); stats(records); };
    CHECK(waitFor([&] { publish(); return records[0] > 0 && records[10] > 0; }));
    CHECK(records[1] == 64 && records[2] == 36 && records[3] == 60 && records[4] == 0xffff0000u);
    CHECK(records[11] == 1920 && records[12] == 1080 && records[13] == 24);
    CHECK(records[14] == 0xff000000u && records[15] == 0xffff0000u && records[16] > 0 && records[19] == 0);
    // A separate receiver holding the window mailbox does not stop external return output.
    std::atomic_bool locked{false}, release{false};
    std::jthread holder([&] {
        const auto name = (vtrpon::operatorMonitorChannelName() + "-mutex").toStdWString();
        HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
        if (!mutex || WaitForSingleObject(mutex, 2000) != WAIT_OBJECT_0) return;
        locked = true; while (!release) QThread::msleep(5); ReleaseMutex(mutex); CloseHandle(mutex);
    });
    const bool gotLock = waitFor([&] { return locked.load(); });
    const auto before = records[10];
    const bool continued = gotLock && waitFor([&] { publish(); return records[10] > before + 2; });
    release = true; holder.join(); CHECK(gotLock && continued);
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
    runtime.requestShutdown(); CHECK(waitFor([&] { return runtime.shutdownComplete(); }));
    std::puts("PASS: linked settings, separate ON AIR/return NDI frames and names, overlays, audio, window isolation, source loop guard, stale generations, shutdown");
    return 0;
}
