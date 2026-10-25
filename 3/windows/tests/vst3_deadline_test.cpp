#include "vst_service.h"
#include "vst3_test_hooks.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
using namespace vtrpon;
namespace {
bool require(bool condition, const char *label) {
    std::cout << (condition ? "PASS " : "FAIL ") << label << std::endl;
    return condition;
}
template<class F> bool until(F predicate, int ms = 5000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < ms) {
        QCoreApplication::processEvents(); QThread::msleep(2);
    }
    return predicate();
}
int parentDelay = 0;
void delayParent(vst::Shared *, vst::Timing *timing) {
    if (parentDelay) Sleep(DWORD(parentDelay));
    if (timing && InterlockedCompareExchange64(&timing->completedRequest, 0, 0) == timing->request)
        std::cout << "QPC request=" << timing->requestedAt << " start=" << timing->startedAt
                  << " complete=" << timing->completedAt << " deadline=" << timing->deadline
                  << " resumed=" << vst::qpcNow() << " frequency=" << timing->frequency << '\n';
}
bool activate(VstService &service, const QString &plugin, const QString &uid, int childDelay = 0,
              VstSlot slot = VstSlot::Master) {
    service.select(slot, {}, {});
    qputenv("VTRPON_TEST_VST_DELAY_MS", QByteArray::number(childDelay));
    service.select(slot, plugin, uid);
    return until([&] { return service.active(slot) && !service.applying(slot); });
}
bool pcmCase(VstService &service, const QString &plugin, const QString &uid, const char *label,
             int childDelay, int hostDelay, size_t frames, bool wet) {
    bool ok = require(activate(service, plugin, uid, childDelay), "worker ready");
    std::vector<float> pcm(frames * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = 0.25f + float(i % 13) / 64.f;
    const auto original = pcm;
    parentDelay = hostDelay;
    vst::test::requested = delayParent;
    const auto start = vst::qpcNow();
    processVst(VstSlot::Master, pcm.data(), frames);
    const auto elapsedMs = (vst::qpcNow() - start) * 1000 / vst::qpcFrequency();
    vst::test::requested = nullptr;
    bool all = true;
    for (size_t i = 0; i < pcm.size(); ++i) all &= pcm[i] == original[i] * (wet ? 0.5f : 1.f);
    ok &= require(all, label);
    // Allow scheduler jitter, but detect a fresh 50 ms budget for each block.
    if (childDelay == 30 && frames == 8192) ok &= require(elapsedMs < 60, "one 50 ms budget for both blocks");
    std::cout << "elapsedMs=" << elapsedMs << " frames=" << frames << '\n';
    if (!wet) {
        ok &= require(until([&] { return service.statusKey(VstSlot::Master) == "vst.failed"; }), "fault reported");
        pcm = original;
        const auto repeated = vst::qpcNow();
        for (int i = 0; i < 50; ++i) processVst(VstSlot::Master, pcm.data(), frames);
        ok &= require(pcm == original && (vst::qpcNow() - repeated) * 1000 / vst::qpcFrequency() < 20,
                      "retired transport never waits or consumes late PCM");
        std::vector<float> other(960, 0.25f);
        processVst(VstSlot::Edit, other.data(), 480);
        ok &= require(std::all_of(other.begin(), other.end(), [](float v) { return v == 0.125f; }), "other slot continues");
    }
    return ok;
}
bool boundaryCases() {
    bool ok = true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    vst::Timing t;
    t.frequency = vst::qpcFrequency(); t.request = 7;
    const auto deadline = vst::qpcNow() - t.frequency / 10;
    t.requestedAt = deadline - 1000; t.startedAt = deadline - 900;
    for (const auto delta : {-2, -1, 0, 1, 2}) {
        t.completedAt = deadline + delta; InterlockedExchange64(&t.completedRequest, 7);
        SetEvent(event);
        const auto result = vst::waitForCompletion(event, &t, 7, deadline, t.frequency);
        std::cout << "boundary offset ticks=" << delta << '\n';
        ok &= require((result == WAIT_OBJECT_0) == (delta == -2), "QPC conservative boundary");
    }
    t.completedAt = deadline - 2;
    ok &= require(vst::waitForCompletion(event, &t, 7, deadline, t.frequency) == WAIT_TIMEOUT,
                  "completion without event is not accepted");
    SetEvent(event);
    ok &= require(vst::waitForCompletion(event, &t, 8, deadline, t.frequency) == WAIT_TIMEOUT,
                  "stale request completion is not accepted");
    CloseHandle(event);
    return ok;
}
bool oldHost(const QString &addon, const QString &plugin, const QString &uid, int layout) {
    // layout 0 is the exact old allocation; the others are unrecognized tails.
    const auto id = QStringLiteral("Local\\VtrPonDeadlineTest-") + QUuid::createUuid().toString(QUuid::Id128);
    const size_t bytes = layout ? sizeof(vst::TimedShared) : sizeof(vst::Shared);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(bytes),
                                       reinterpret_cast<LPCWSTR>(id.utf16()));
    auto *shared = static_cast<vst::Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    if (!shared) { if (mapping) CloseHandle(mapping); return require(false, "compatibility mapping"); }
    vst::Timing *timing = nullptr;
    if (layout) {
        auto *extended = new (shared) vst::TimedShared;
        timing = &extended->timing; timing->frequency = vst::qpcFrequency();
        if (layout == 1) timing->magic = 0;
        if (layout == 2) timing->version = 99;
        if (layout == 3) ++timing->frequency;
        if (layout == 4) --timing->bytes;
    } else new (shared) vst::Shared;
    HANDLE request = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-request").utf16()));
    HANDLE response = CreateEventW(nullptr, FALSE, FALSE, reinterpret_cast<LPCWSTR>((id + "-response").utf16()));
    qputenv("VTRPON_TEST_VST_DELAY_MS", "0");
    QProcess worker;
    worker.start(addon, {"--run", id, "--module", plugin, "--class", uid,
                        "--parent", QString::number(QCoreApplication::applicationPid())});
    bool ok = require(until([&] { return InterlockedCompareExchange(&shared->ready, 0, 0) != 0; }), "old/unknown host worker ready");
    if (ok) {
        shared->frames = 480; std::fill_n(shared->input, 960, 0.25f); SetEvent(request);
        ok &= require(WaitForSingleObject(response, 50) == WAIT_OBJECT_0
            && std::all_of(shared->output, shared->output + 960, [](float v) { return v == 0.125f; }),
            "new worker handles old/unknown host PCM");
        if (timing) ok &= require(timing->supported == 0, "unknown timing header not negotiated");
    }
    worker.kill(); worker.waitForFinished(5000);
    UnmapViewOfFile(shared); CloseHandle(mapping); CloseHandle(request); CloseHandle(response);
    return ok;
}
std::atomic_bool holding{false}, resumeReader{false};
std::atomic<const void *> heldBridge{nullptr};
std::atomic<DWORD> destroyThread{0};
void holdReader(const void *bridge) {
    heldBridge = bridge; holding = true;
    while (!resumeReader) Sleep(1);
}
void recordDestroy(const void *bridge) {
    if (bridge == heldBridge.load()) destroyThread = GetCurrentThreadId();
}
bool releaseCase(VstService &service, const QString &plugin, const QString &gain, const QString &other, int action) {
    bool ok = require(activate(service, plugin, gain), "release fixture ready");
    holding = false; resumeReader = false; heldBridge = nullptr; destroyThread = 0;
    vst::test::acquired = holdReader; vst::test::destroyed = recordDestroy;
    std::thread audio([] { float pcm[960]{}; processVst(VstSlot::Master, pcm, 480); });
    ok &= require(until([] { return holding.load(); }), "reader holds reference before processing lock");
    if (action == 0) service.select(VstSlot::Master, {}, {});
    if (action == 1) {
        service.select(VstSlot::Master, plugin, other);
        ok &= require(until([&] { return service.active(VstSlot::Master) && !service.applying(VstSlot::Master); }), "switch while old reader paused");
    }
    if (action == 2) service.setEnabled(false);
    if (action == 4) {
        const auto processes = service.findChildren<QProcess *>();
        if (!processes.isEmpty()) processes.back()->kill();
        ok &= require(until([&] { return service.statusKey(VstSlot::Master) == "vst.failed"; }), "worker exit while reader holds reference");
    }
    if (action == 5) {
        service.select(VstSlot::Master, plugin, other);
        ok &= require(until([&] { return !service.applying(VstSlot::Master); })
            && service.active(VstSlot::Master) && !service.applyError(VstSlot::Master).isEmpty(),
            "candidate failure preserves active reader");
        service.select(VstSlot::Master, {}, {});
    }
    if (action == 3) delete &service;
    QCoreApplication::processEvents();
    ok &= require(destroyThread == 0, "UI retains resources while reader holds reference");
    resumeReader = true; audio.join(); vst::test::acquired = nullptr;
    ok &= require(destroyThread == 0, "last audio reader does not destroy resources");
    ok &= require(until([] { return destroyThread.load() != 0; }) && destroyThread == GetCurrentThreadId(),
                  "resources destroyed later on UI thread including service shutdown");
    vst::test::destroyed = nullptr;
    if (action != 3) service.setEnabled(true);
    return ok;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    app.setProperty("vtrpon.settingsFile", temporary.filePath("settings.ini"));
    const auto addon = app.arguments().value(1).toUtf8();
    const auto plugin = app.arguments().value(2);
    const auto legacy = app.arguments().value(3).toUtf8();
    vst::test::addonOverride = addon.constData();
    QProcess scanner;
    scanner.start(QString::fromUtf8(addon), {"--scan", plugin, "--result", temporary.filePath("scan.json"),
        "--parent", QString::number(QCoreApplication::applicationPid())});
    if (!require(scanner.waitForFinished(10000) && scanner.exitCode() == 0, "real SDK scan")) return 1;
    QFile json(temporary.filePath("scan.json")); json.open(QIODevice::ReadOnly);
    const auto catalog = QJsonDocument::fromJson(json.readAll()).array();
    if (!require(catalog.size() == 9, "fixture classes")) return 1;
    const auto gain = catalog[0].toObject().value("uid").toString();
    auto &service = VstService::instance();
    bool ok = require(service.setEnabled(true), "enable fixture");
    ok &= boundaryCases();
    ok &= require(activate(service, plugin, gain, 0, VstSlot::Edit), "other slot ready");
    ok &= pcmCase(service, plugin, gain, "5 ms child / 80 ms parent recovers wet PCM", 5, 80, 480, true);
    ok &= pcmCase(service, plugin, gain, "recovered first block cannot start another block after deadline", 5, 80, 8192, false);
    ok &= pcmCase(service, plugin, gain, "80 ms child / normal parent keeps all dry PCM", 80, 0, 480, false);
    ok &= pcmCase(service, plugin, gain, "80 ms child / 120 ms parent rejects late event", 80, 120, 480, false);
    ok &= pcmCase(service, plugin, gain, "two 30 ms blocks keep entire 8192-frame buffer dry", 30, 0, 8192, false);
    ok &= pcmCase(service, plugin, gain, "manual recovery creates fresh transport", 0, 0, 8192, true);
    if (!legacy.isEmpty()) {
        vst::test::addonOverride = legacy.constData();
        ok &= pcmCase(service, plugin, gain, "old worker normal PCM", 0, 0, 480, true);
        ok &= pcmCase(service, plugin, gain, "old worker cannot recover after parent delay", 5, 80, 480, false);
        vst::test::addonOverride = addon.constData();
    } else ok &= require(false, "legacy worker must be supplied for compatibility coverage");
    for (int layout = 0; layout < 5; ++layout) ok &= oldHost(QString::fromUtf8(addon), plugin, gain, layout);
    for (const int action : {0, 1, 2, 4})
        ok &= releaseCase(service, plugin, gain, catalog[7].toObject().value("uid").toString(), action);
    ok &= releaseCase(service, plugin, gain, catalog[6].toObject().value("uid").toString(), 5);
    ok &= releaseCase(service, plugin, gain, {}, 3);
    std::cout << (ok ? "VST3 deadline/ownership/compatibility PASS\n" : "VST3 deadline/ownership/compatibility FAIL\n");
    return ok ? 0 : 1;
}
