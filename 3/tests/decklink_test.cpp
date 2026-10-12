#include "decklink.h"
#include "controller_settings.h"
#include <QCoreApplication>
#include <QLibrary>
#include <QTemporaryDir>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cmath>
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #value); return 1; } } while(false)
template<class F> bool waitFor(F f, int milliseconds = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}
struct Pipeline {
    GstElement *pipeline = nullptr, *source = nullptr, *sink = nullptr;
    explicit Pipeline(bool audio) {
        pipeline = gst_parse_launch(audio ?
            "appsrc name=source is-live=true format=time do-timestamp=true block=false max-buffers=2 leaky-type=downstream ! audioconvert ! audio/x-raw,format=F32LE ! appsink name=sink sync=false max-buffers=2 drop=true" :
            "appsrc name=source is-live=true format=time do-timestamp=true block=false max-buffers=2 leaky-type=downstream ! appsink name=sink sync=false max-buffers=2 drop=true", nullptr);
        source = gst_bin_get_by_name(GST_BIN(pipeline), "source"); sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    }
    ~Pipeline() { gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(source); gst_object_unref(sink); gst_object_unref(pipeline); }
    GstSample *sample() { return gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND); }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); gst_init(nullptr, nullptr);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--hardware-discovery")) {
        QTemporaryDir temporary; CHECK(temporary.isValid());
        app.setProperty("vtrpon.settingsFile", temporary.filePath("user.ini"));
        qputenv("VTRPON_DECKLINK_ADDON_FILE", QByteArray(argv[2]));
        qunsetenv("VTRPON_DECKLINK_ALLOW_TEST_ADDON");
        vtrpon::DeckLinkRuntime runtime;
        CHECK(waitFor([] { return vtrpon::deckLinkState().availability == vtrpon::DeckLinkState::Availability::Ready; }));
        for (const auto &channel : vtrpon::deckLinkState().channels)
            std::printf("%s | %s | %s | modes=%lld\n", channel.input ? "IN" : "OUT",
                channel.name.toUtf8().constData(), channel.path.toUtf8().constData(), static_cast<long long>(channel.modes.size()));
        runtime.requestShutdown(); CHECK(waitFor([&] { return runtime.shutdownComplete(); }));
        return 0;
    }
    CHECK(argc == 2); QTemporaryDir temporary; CHECK(temporary.isValid());
    app.setProperty("vtrpon.settingsFile", temporary.filePath("user.ini"));
    qputenv("VTRPON_DECKLINK_ADDON_FILE", QByteArray(argv[1]));
    qputenv("VTRPON_DECKLINK_ALLOW_TEST_ADDON", "1");
    QLibrary mock(QString::fromLocal8Bit(argv[1])); CHECK(mock.load());
    const auto stats = reinterpret_cast<void (__cdecl *)(uint32_t *)>(mock.resolve("vtrpon_decklink_test_stats")); CHECK(stats);
    const auto outputStats = reinterpret_cast<void (__cdecl *)(uint64_t *)>(mock.resolve("vtrpon_decklink_test_outputs")); CHECK(outputStats);
    const auto expectColor = reinterpret_cast<void (__cdecl *)(unsigned, uint32_t)>(mock.resolve("vtrpon_decklink_test_expect")); CHECK(expectColor);
    VtrPonDeckLinkStreamOptions options{sizeof(options), 1, 0, 0, 100};
    const auto path = vtrpon::deckLinkSourcePath(options); VtrPonDeckLinkStreamOptions parsed{};
    CHECK(vtrpon::deckLinkSourceOptions(path, &parsed) && parsed.persistentId == 100);
    CHECK(!vtrpon::deckLinkSourceOptions("decklink://100/0/3/0", &parsed));
    CHECK(!vtrpon::deckLinkSourceOptions("decklink://100/0/1/4294967296", &parsed));
    CHECK(!vtrpon::deckLinkSourceOptions("decklink://0100/0/1/0", &parsed));
    qputenv("VTRPON_DECKLINK_TEST_NO_DEVICES", "1");
    vtrpon::DeckLinkRuntime runtime;
    CHECK(waitFor([] { return vtrpon::deckLinkState().availability == vtrpon::DeckLinkState::Availability::NoDevices; }));
    auto settings = vtrpon::savedDeckLinkSettings(); CHECK(settings.inputEnabled && !settings.outputEnabled);
    settings.outputChannel = path; settings.outputMode = 1001; settings.outputEnabled = true;
    vtrpon::saveDeckLinkSettings(settings);
    CHECK(!vtrpon::deckLinkOutputGeneration() && vtrpon::savedDeckLinkSettings() == settings);
    qunsetenv("VTRPON_DECKLINK_TEST_NO_DEVICES");
    CHECK(waitFor([] { return vtrpon::deckLinkState().channels.size() == 5 && vtrpon::deckLinkOutputGeneration(); }));
    CHECK(vtrpon::deckLinkInputAllowed(path));
    Pipeline video(false), second(false), audio(true);
    auto receiver = std::make_unique<vtrpon::DeckLinkReceiver>(path, video.source, false);
    auto secondReceiver = std::make_unique<vtrpon::DeckLinkReceiver>(path, second.source, false);
    auto audioReceiver = std::make_unique<vtrpon::DeckLinkReceiver>(path, audio.source, true);
    auto *sample = video.sample(); CHECK(sample);
    auto *buffer = gst_sample_get_buffer(sample); CHECK(GST_BUFFER_PTS_IS_VALID(buffer) && GST_BUFFER_DURATION(buffer) > 0);
    GstMapInfo map{}; CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ));
    CHECK(map.size == 64 * 36 * 4 && map.data[0] == 10 && map.data[2] == 200); gst_buffer_unmap(buffer, &map); gst_sample_unref(sample);
    sample = audio.sample(); CHECK(sample); buffer = gst_sample_get_buffer(sample); CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ));
    CHECK(map.size > 0 && std::abs(reinterpret_cast<float *>(map.data)[0] - 0.25f) < 0.001f); gst_buffer_unmap(buffer, &map); gst_sample_unref(sample);
    uint32_t counts[5]{}; stats(counts); CHECK(counts[0] == 1 && counts[1] == 1);
    {
        Pipeline conflict(false); options.modeId = 1000;
        vtrpon::DeckLinkReceiver differentFormat(vtrpon::deckLinkSourcePath(options), conflict.source, false);
        auto *bus = gst_element_get_bus(conflict.pipeline);
        auto *error = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ERROR);
        CHECK(error); gst_message_unref(error); gst_object_unref(bus);
        stats(counts); CHECK(counts[0] == 1 && counts[1] == 1);
    }
    const auto generation = vtrpon::deckLinkOutputGeneration();
    settings.inputMode = 1000; vtrpon::saveDeckLinkSettings(settings);
    CHECK(vtrpon::deckLinkOutputGeneration() == generation); // Changing input defaults leaves output running.
    settings.inputMode = 0; vtrpon::saveDeckLinkSettings(settings);
    settings.outputChannel = path; settings.outputMode = 1001; settings.outputEnabled = true; vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputGeneration() != 0; }));
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL feature{};
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &feature, &context)));
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 64; desc.Height = 36; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> view;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture))); CHECK(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &view)));
    const float color[4]{200.f/255, 40.f/255, 10.f/255, 1}; context->ClearRenderTargetView(view.Get(), color);
    vtrpon::DeckLinkOutput output;
    CHECK(waitFor([&] { output.publish(view.Get()); context->Flush(); stats(counts); return counts[2] > 0; }));
    buffer = gst_buffer_new_allocate(nullptr, 800 * 8, nullptr); CHECK(buffer); gst_buffer_memset(buffer, 0, 0, 800 * 8);
    vtrpon::publishDeckLinkAudio(buffer); gst_buffer_unref(buffer);
    CHECK(waitFor([&] { stats(counts); return counts[3] > 0 && counts[4] > 0; }));
    ComPtr<ID3D11Texture2D> alphaTexture; ComPtr<ID3D11RenderTargetView> alphaView;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &alphaTexture))); CHECK(SUCCEEDED(device->CreateRenderTargetView(alphaTexture.Get(), nullptr, &alphaView)));
    const float translucent[4]{100.f/255, 40.f/255, 10.f/255, 128.f/255}; context->ClearRenderTargetView(alphaView.Get(), translucent);
    uint64_t routed[21]{}; outputStats(routed); const auto compositeOpens = routed[18];
    settings.fillEnabled = settings.keyEnabled = true;
    settings.fillChannel = QStringLiteral("decklink://100/2/1/0"); settings.keyChannel = QStringLiteral("decklink://100/3/1/0"); settings.fillMode = settings.keyMode = 1001;
    expectColor(1, 0xffc75014); expectColor(2, 0xff808080); vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 7 && vtrpon::deckLinkState().pairSynchronized; }));
    outputStats(routed); CHECK(routed[18] == compositeOpens); const auto fillBefore = routed[6], keyBefore = routed[12];
    CHECK(waitFor([&] { output.publish(view.Get(), alphaView.Get()); context->Flush(); outputStats(routed); return routed[6] > fillBefore && routed[12] > keyBefore && routed[8] == routed[14]; }));
    CHECK(routed[9] == 0xffc75014 && routed[15] == 0xff808080 && routed[11] > 0 && routed[11] == routed[17]);
    buffer = gst_buffer_new_allocate(nullptr, 800 * 8, nullptr); CHECK(buffer); gst_buffer_memset(buffer, 0, 0, 800 * 8);
    vtrpon::publishDeckLinkAudio(buffer); gst_buffer_unref(buffer);
    CHECK(waitFor([&] { outputStats(routed); return routed[7] > 0; })); CHECK(routed[13] == 0); // Key carries silence, never the final mix.
    const auto oldGeneration = vtrpon::deckLinkOutputGeneration();
    settings.premultipliedFill = true; expectColor(1, 0xff64280a); vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([&] { return vtrpon::deckLinkOutputMask() == 7 && vtrpon::deckLinkOutputGeneration() != oldGeneration; }));
    outputStats(routed); CHECK(routed[18] == compositeOpens); const auto premultipliedBefore = routed[6];
    vtrpon::publishDeckLinkVideo(QByteArray(64*36*4, '\0'), QByteArray(64*36*4, '\0'), 64, 36, oldGeneration);
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); outputStats(routed); CHECK(routed[6] == premultipliedBefore);
    CHECK(waitFor([&] { output.publish(view.Get(), alphaView.Get()); context->Flush(); outputStats(routed); return routed[6] > premultipliedBefore; })); CHECK(routed[9] == 0xff64280a);
    // Fully transparent FTB must produce black Fill and Key; Composite retains its background.
    const float transparent[4]{}; context->ClearRenderTargetView(alphaView.Get(), transparent); expectColor(1, 0xff000000); expectColor(2, 0xff000000);
    settings.compositeEnabled = false; vtrpon::saveDeckLinkSettings(settings); CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 6; }));
    outputStats(routed); const auto transparentBefore = routed[6];
    CHECK(waitFor([&] { output.publish(view.Get(), alphaView.Get()); context->Flush(); outputStats(routed); return routed[6] > transparentBefore && routed[9] == 0xff000000 && routed[15] == 0xff000000; })); CHECK(routed[4] == 0);
    context->ClearRenderTargetView(alphaView.Get(), color); expectColor(1, 0xffc8280a); expectColor(2, 0xffffffff);
    outputStats(routed); const auto opaqueBefore = routed[6];
    CHECK(waitFor([&] { output.publish(view.Get(), alphaView.Get()); context->Flush(); outputStats(routed); return routed[6] > opaqueBefore && routed[15] == 0xffffffff; }));
    settings.fillEnabled = false; vtrpon::saveDeckLinkSettings(settings); CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 4; }));
    outputStats(routed); const auto keyOnlyBefore = routed[12];
    CHECK(waitFor([&] { output.publish(view.Get(), alphaView.Get()); context->Flush(); outputStats(routed); return routed[12] > keyOnlyBefore && !routed[10]; }));
    settings.fillEnabled = true; settings.keyEnabled = false; vtrpon::saveDeckLinkSettings(settings); CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 2; }));
    outputStats(routed); const auto fillOnlyBefore = routed[6];
    CHECK(waitFor([&] { output.publish(view.Get(), alphaView.Get()); context->Flush(); outputStats(routed); return routed[6] > fillOnlyBefore && !routed[16]; }));
    settings.keyEnabled = true; settings.compositeEnabled = true; settings.keyChannel = settings.fillChannel; vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { const auto state = vtrpon::deckLinkState(); return vtrpon::deckLinkOutputMask() == 1 && !state.outputs[1].error.isEmpty() && !state.outputs[2].error.isEmpty(); }));
    settings.keyChannel = QStringLiteral("decklink://100/3/1/0"); settings.keyMode = 1000; vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 1 && vtrpon::deckLinkState().outputs[2].error.contains("same video format"); }));
    settings.keyMode = 1001; qputenv("VTRPON_DECKLINK_TEST_NO_SYNC", "1"); vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { const auto state = vtrpon::deckLinkState(); return vtrpon::deckLinkOutputMask() == 7 && !state.pairSynchronized && !state.syncWarning.isEmpty(); }));
    outputStats(routed); CHECK(routed[11] == 0 && routed[17] == 0);
    qunsetenv("VTRPON_DECKLINK_TEST_NO_SYNC"); qputenv("VTRPON_DECKLINK_TEST_FAIL_GROUP", "1"); settings.premultipliedFill = false; vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 1 && vtrpon::deckLinkState().outputs[1].error.contains("group start"); }));
    outputStats(routed); CHECK(!routed[10] && !routed[16]); // A backend failure never falls back to independent output.
    qunsetenv("VTRPON_DECKLINK_TEST_FAIL_GROUP"); CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 7 && vtrpon::deckLinkState().pairSynchronized; }));
    qputenv("VTRPON_DECKLINK_TEST_FAIL_KEY", "1"); CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 1 && !vtrpon::deckLinkState().outputs[1].error.isEmpty(); }));
    sample = video.sample(); CHECK(sample); gst_sample_unref(sample); // Other output and capture survive pair failure.
    qunsetenv("VTRPON_DECKLINK_TEST_FAIL_KEY"); settings.fillEnabled = settings.keyEnabled = false; vtrpon::saveDeckLinkSettings(settings);
    CHECK(waitFor([] { return vtrpon::deckLinkOutputMask() == 1; }));
    qputenv("VTRPON_DECKLINK_TEST_FAIL_OUTPUT", "1");
    CHECK(waitFor([] { return !vtrpon::deckLinkState().outputError.isEmpty(); }));
    // Failure of this output and removal of one consumer leave capture alive.
    receiver.reset(); sample = second.sample(); CHECK(sample); gst_sample_unref(sample);
    sample = audio.sample(); CHECK(sample); gst_sample_unref(sample); stats(counts); CHECK(counts[0] == 1 && counts[1] == 1);
    qunsetenv("VTRPON_DECKLINK_TEST_FAIL_OUTPUT");
    qputenv("VTRPON_DECKLINK_TEST_NO_DEVICES", "1");
    CHECK(waitFor([] { return vtrpon::deckLinkState().availability == vtrpon::DeckLinkState::Availability::NoDevices && !vtrpon::deckLinkOutputGeneration(); }));
    CHECK(vtrpon::savedDeckLinkSettings() == settings);
    CHECK(waitFor([&] { stats(counts); return counts[1] == 0; }));
    secondReceiver.reset(); audioReceiver.reset();
    qunsetenv("VTRPON_DECKLINK_TEST_NO_DEVICES");
    CHECK(waitFor([] { return vtrpon::deckLinkState().availability == vtrpon::DeckLinkState::Availability::Ready && vtrpon::deckLinkOutputGeneration(); }));
    secondReceiver = std::make_unique<vtrpon::DeckLinkReceiver>(path, second.source, false);
    sample = second.sample(); CHECK(sample); gst_sample_unref(sample);
    CHECK(waitFor([&] { stats(counts); return counts[0] == 2 && counts[1] == 1; }));
    settings.outputEnabled = false; vtrpon::saveDeckLinkSettings(settings); secondReceiver.reset();
    CHECK(waitFor([&] { stats(counts); return counts[1] == 0; }));
    runtime.requestShutdown(); CHECK(waitFor([&] { return runtime.shutdownComplete(); }));
    std::puts("DeckLink capture sharing, GPU Composite/Fill/Key routing, straight/premultiplied/transparent pixels, paired timestamps, audio routing, synchronization fallback, duplicate/format rejection, fault isolation, absent hardware, reconnection and shutdown passed.");
    return 0;
}
