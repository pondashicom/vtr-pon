#include "vst_protocol.h"
#include "state_stream.h"
#ifdef _WIN32
#include <objbase.h>
#include "public.sdk/source/common/memorystream.h"
#else
#include "vst_transport_mac.h"
#include <sys/event.h>
#include <pthread/qos.h>
#include <pthread.h>
#import <Cocoa/Cocoa.h>
#endif
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/base/ustring.h"
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>
#include <array>
#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace vtrpon::vst;
static_assert(VST_VERSION >= 0x030800, "VST 3 SDK 3.8.0 or newer is required");
namespace {
#ifdef _WIN32
const auto editorPlatform = kPlatformTypeHWND;
#else
const auto editorPlatform = kPlatformTypeNSView;
#endif
class Host final : public HostApplication {
    tresult PLUGIN_API getName(String128 name) override {
        UString(name, 128).fromAscii("VTR-PON3"); return kResultOk;
    }
};
class Handler final : public U::Implements<U::Directly<IComponentHandler, IComponentHandler2>> {
public:
    std::mutex mutex;
    std::map<ParamID, ParamValue> pending;
    std::map<ParamID, ParamValue> returned;
    std::atomic<int32> restart{0};
    std::atomic<uint64_t> edited{1}, applied{0};
    std::atomic_bool openRequested{false};
    tresult PLUGIN_API setDirty(TBool dirty) override { if (dirty) ++edited; return kResultOk; }
    tresult PLUGIN_API requestOpenEditor(FIDString) override { openRequested = true; return kResultOk; }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        std::lock_guard lock(mutex);
        if (pending.size() < 8192 || pending.contains(id)) pending[id] = value;
        ++edited;
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        restart.fetch_or(flags); ++edited; return kResultOk;
    }
};
class Frame final : public U::Implements<U::Directly<IPlugFrame>> {
public:
    QWidget *window = nullptr;
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *rect) override {
        if (!window || !view || !rect || rect->getWidth() < 1 || rect->getHeight() < 1 ||
            rect->getWidth() > 8192 || rect->getHeight() > 8192) return kInvalidArgument;
        // Native HWND editors use physical pixels. Qt uses logical pixels.
#ifdef _WIN32
        const auto scale = window->devicePixelRatioF();
#else
        const auto scale = 1.; // NSView and Qt both use logical points.
#endif
        window->setFixedSize(qRound(rect->getWidth() / scale), qRound(rect->getHeight() / scale));
        return view->onSize(rect);
    }
};
class EditorWindow final : public QWidget {
public:
    EditorWindow() : QWidget(nullptr, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint) {}
protected:
    void closeEvent(QCloseEvent *event) override {
        // QWidget::close() can destroy the native HWND, including the plug-in's
        // child windows. Keep the attached IPlugView and its parent alive.
        event->ignore();
        hide();
    }
};
QString argument(const QStringList &args, const QString &name) {
    const int index = args.indexOf(name);
    return index >= 0 ? args.value(index + 1) : QString();
}
bool monitorParent(const QStringList &args) {
    bool valid = false;
    const auto pid = argument(args, "--parent").toULong(&valid);
    if (!valid || !pid) return false;
#ifdef _WIN32
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!parent) return false;
    if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) {
        CloseHandle(parent);
        return false;
    }
    try {
        std::promise<void> started;
        auto ready = started.get_future();
        // Wait for the watchdog to enter its function before loading vendor
        // code: Windows serializes DLL initialization and new thread startup.
        // A hung DllMain must not prevent the watchdog from starting at all.
        std::thread([parent, started = std::move(started)]() mutable {
            started.set_value();
            WaitForSingleObject(parent, INFINITE);
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
        ready.get();
    } catch (...) {
        CloseHandle(parent);
        throw;
    }
    return true;
#else
    if (pid != static_cast<unsigned long>(getppid())) return false;
    const int queue = kqueue();
    if (queue < 0) return false;
    fcntl(queue, F_SETFD, FD_CLOEXEC);
    struct kevent event{};
    EV_SET(&event, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(queue, &event, 1, nullptr, 0, nullptr) < 0) { close(queue); return false; }
    try {
        std::promise<void> started;
        auto ready = started.get_future();
        std::thread([queue, started = std::move(started)]() mutable {
            started.set_value();
            struct kevent exit{};
            int result;
            do { result = kevent(queue, nullptr, 0, &exit, 1, nullptr); } while (result < 0 && errno == EINTR);
            // Exit without invoking possibly hung vendor destructors.
            _exit(result == 1 ? 0 : 4);
        }).detach();
        ready.get();
    } catch (...) { close(queue); throw; }
    return true;
#endif
}
int scan(const QString &path, const QString &result) {
    Host host;
    std::string error;
    auto module = VST3::Hosting::Module::create(path.toStdString(), error);
    if (!module) return 2;
    module->getFactory().setHostContext(&host);
    QJsonArray array;
    for (const auto &info : module->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass) continue;
        if (info.subCategoriesString().find("Instrument") != std::string::npos) continue;
        array.append(QJsonObject{{"uid", QString::fromStdString(info.ID().toString())},
            {"name", QString::fromStdString(info.name())}, {"vendor", QString::fromStdString(info.vendor())}});
    }
    QSaveFile file(result);
    if (!file.open(QIODevice::WriteOnly)) return 3;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    return file.commit() ? 0 : 3;
}
int run(QApplication &app, const QStringList &args) {
    const QString id = argument(args, "--run");
#ifdef _WIN32
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, reinterpret_cast<LPCWSTR>(id.utf16()));
    if (!mapping) return 4;
    auto *shared = static_cast<Shared *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared || shared->protocol != kProtocol) return 4;
    HANDLE request = OpenEventW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>((id + "-request").utf16()));
    HANDLE response = OpenEventW(EVENT_MODIFY_STATE, FALSE, reinterpret_cast<LPCWSTR>((id + "-response").utf16()));
    if (!request || !response) return 4;
#else
    if (id != QLatin1String("fd:0,1")) return 4;
    MacTransport transport;
    if (!transport.inherit()) return 4;
    auto *shared = transport.shared;
#endif

    Host host;
    PluginContextFactory::instance().setPluginContext(&host);
    std::string error;
    auto module = VST3::Hosting::Module::create(argument(args, "--module").toStdString(), error);
    if (!module) return 5;
    module->getFactory().setHostContext(&host);
    std::unique_ptr<PlugProvider> provider;
    for (const auto &info : module->getFactory().classInfos()) {
        if (QString::fromStdString(info.ID().toString()) == argument(args, "--class") && info.category() == kVstAudioEffectClass) {
            provider = std::make_unique<PlugProvider>(module->getFactory(), info, true); break;
        }
    }
    if (!provider || !provider->initialize()) return 5;
    auto component = provider->getComponentPtr();
    auto controller = provider->getControllerPtr();
    FUnknownPtr<IAudioProcessor> processor(component);
    if (!processor || processor->canProcessSampleSize(kSample32) != kResultOk) return 6;
    const auto inputsCount = component->getBusCount(kAudio, kInput);
    const auto outputsCount = component->getBusCount(kAudio, kOutput);
    if (inputsCount < 1 || outputsCount < 1 || inputsCount > 16 || outputsCount > 16) return 6;
    std::vector<SpeakerArrangement> inputArr(inputsCount), outputArr(outputsCount);
    for (int32 i = 0; i < inputsCount; ++i) processor->getBusArrangement(kInput, i, inputArr[i]);
    for (int32 i = 0; i < outputsCount; ++i) processor->getBusArrangement(kOutput, i, outputArr[i]);
    inputArr[0] = outputArr[0] = SpeakerArr::kStereo;
    if (processor->setBusArrangements(inputArr.data(), inputsCount, outputArr.data(), outputsCount) != kResultOk) return 6;
    BusInfo in{}, out{};
    if (component->getBusInfo(kAudio, kInput, 0, in) != kResultOk ||
        component->getBusInfo(kAudio, kOutput, 0, out) != kResultOk || in.channelCount != 2 || out.channelCount != 2) return 6;
    if (component->activateBus(kAudio, kInput, 0, true) != kResultOk ||
        component->activateBus(kAudio, kOutput, 0, true) != kResultOk) return 6;
    for (auto direction : {kInput, kOutput}) {
        for (int32 bus = 1; bus < component->getBusCount(kAudio, direction); ++bus) {
            BusInfo info{};
            if (component->getBusInfo(kAudio, direction, bus, info) != kResultOk ||
                info.busType != kAux || info.channelCount > 64) return 6;
            component->activateBus(kAudio, direction, bus, false);
        }
    }
    for (auto direction : {kInput, kOutput})
        for (int32 bus = 0; bus < component->getBusCount(kEvent, direction); ++bus)
            component->activateBus(kEvent, direction, bus, false);
    ProcessSetup setup{kRealtime, kSample32, kFrames, double(kRate)};
    if (processor->setupProcessing(setup) != kResultOk) return 6;
    auto handler = owned(new Handler);
    if (controller) controller->setComponentHandler(handler);
    // State files are optional: older hosts and Windows retain protocol 2.
    const auto stateDir = argument(args, "--state-dir");
    const auto statePath = [&](const char *name) { return QDir(stateDir).filePath(QString::fromLatin1(name)); };
    int stateSerial = 0, acknowledged = 0;
#ifdef __APPLE__
    std::atomic<int> audioQosClass{0}, audioQosError{0};
    // This helper owns no CoreAudio device, so the OS cannot infer that its
    // socket-driven DSP is latency-sensitive. Keep idle sleep user-controlled.
    auto audioActivity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
        reason:@"VTR-PON VST audio processing"];
#endif
    const auto reportState = [&](bool ok, const QString &error = {}) {
        if (!stateDir.isEmpty()) vtrpon::vststate::write(statePath("status.json"),
            {{"version", 1}, {"serial", stateSerial}, {"request", acknowledged}, {"ok", ok}, {"error", error}
#ifdef __APPLE__
             , {"audioQosClass", audioQosClass.load()}, {"audioQosError", audioQosError.load()},
             {"latencyActivity", audioActivity != nil}
#endif
            });
    };
    const auto modulePath = argument(args, "--module"), classId = argument(args, "--class");
    bool restored = false;
    if (!stateDir.isEmpty() && QFileInfo::exists(statePath("input.json"))) {
        const auto state = vtrpon::vststate::read(statePath("input.json"));
        QByteArray componentBytes, controllerBytes;
        if (!vtrpon::vststate::valid(state, modulePath, classId)) {
            reportState(false, QStringLiteral("Saved plug-in state is invalid. Reset the saved adjustments to use defaults.")); return 8;
        }
        vtrpon::vststate::decode(state.value("component"), componentBytes);
        auto stream = owned(new StateStream(componentBytes));
        if (component->setState(stream) != kResultOk) {
            reportState(false, QStringLiteral("The plug-in rejected its saved audio state. Reset the saved adjustments to use defaults.")); return 8;
        }
        if (controller) {
            stream->seek(0, IBStream::kIBSeekSet, nullptr);
            // A combined component/controller may not implement this sync call.
            const auto result = controller->setComponentState(stream);
            if (result != kResultOk && result != kNotImplemented) {
                reportState(false, QStringLiteral("The plug-in could not synchronize its saved adjustments.")); return 8;
            }
            if (!state.value("controller").isNull()) {
                vtrpon::vststate::decode(state.value("controller"), controllerBytes);
                auto editorState = owned(new StateStream(controllerBytes));
                if (controller->setState(editorState) != kResultOk) {
                    reportState(false, QStringLiteral("The plug-in rejected its saved editor state.")); return 8;
                }
            }
        }
        restored = true;
    }
    if (controller && !restored) {
#ifdef _WIN32
        // Preserve the existing Windows initialization contract.
        MemoryStream state;
        if (component->getState(&state) == kResultOk) {
            state.seek(0, IBStream::kIBSeekSet, nullptr); controller->setComponentState(&state);
        }
#else
        auto state = owned(new StateStream);
        if (component->getState(state) == kResultOk && !state->failed) {
            state->seek(0, IBStream::kIBSeekSet, nullptr); controller->setComponentState(state);
        }
#endif
    }
    reportState(true);
    // prepare() queries IComponent bus information, which requires the UI thread.
    // Keep the buffers alive until the audio thread has joined below.
    HostProcessData data;
    if (!data.prepare(*component, kFrames, kSample32)) return 6;
    if (component->setActive(true) != kResultOk) return 6;

    EditorWindow editor;
    editor.setWindowTitle(QString::fromStdString(provider->getClassInfo().name()) + QStringLiteral(" — VTR-PON3 VST"));
    editor.setAttribute(Qt::WA_NativeWindow);
    auto frame = owned(new Frame); frame->window = &editor;
    IPtr<IPlugView> view;
    std::atomic_bool stop{false};
    std::atomic_bool warmupComplete{false};
    const auto respond = [&] {
#ifdef _WIN32
        SetEvent(response);
#else
        transport.markResponse();
        if (!transport.signal()) exchange(&shared->failed, 1);
#endif
    };
    std::thread audio([&] {
#ifdef __APPLE__
        pthread_setname_np("VTR-PON VST audio");
        audioQosError = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        audioQosClass = int(qos_class_self());
#endif
#ifdef _WIN32
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        if (processor->setProcessing(true) != kResultOk) { exchange(&shared->failed, 1); return; }
        auto &input = data.inputs[0]; auto &output = data.outputs[0];
        float *leftIn = input.channelBuffers32[0], *rightIn = input.channelBuffers32[1];
        float *leftOut = output.channelBuffers32[0], *rightOut = output.channelBuffers32[1];
        ParameterChanges changes(8192), outputChanges(8192);
        ProcessContext context{};
        context.sampleRate = kRate;
        context.state = ProcessContext::kPlaying;
        data.processMode = kRealtime;
        data.inputParameterChanges = &changes; data.outputParameterChanges = &outputChanges;
        data.processContext = &context;
        // Lazy DSP/model initialization must finish outside the playout deadline.
        // Keep the Controller on dry audio until actual process() calls succeed.
        for (int warmup = 0; warmup < 32; ++warmup) {
            for (int bus = 0; bus < data.numInputs; ++bus) {
                auto &b = data.inputs[bus]; b.silenceFlags = 0;
                for (int ch = 0; ch < b.numChannels; ++ch) std::fill_n(b.channelBuffers32[ch], kFrames, 0.f);
            }
            for (int bus = 0; bus < data.numOutputs; ++bus) {
                auto &b = data.outputs[bus]; b.silenceFlags = 0;
                for (int ch = 0; ch < b.numChannels; ++ch) std::fill_n(b.channelBuffers32[ch], kFrames, 0.f);
            }
            data.numSamples = warmup < 16 ? 480 : kFrames;
            outputChanges.clearQueue();
            const auto start = std::chrono::steady_clock::now();
            const auto result = processor->process(data);
            const auto us = Word(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            if (us > shared->warmupMaxUs) exchange(&shared->warmupMaxUs, us);
            if (result != kResultOk) { exchange(&shared->failed, 1); return; }
        }
        // The UI thread publishes readiness after querying latency there.
        warmupComplete = true;
        while (!stop) {
#ifdef _WIN32
            const bool hasAudio = WaitForSingleObject(request, 100) == WAIT_OBJECT_0;
#else
            const auto requestResult = transport.wait(MacTransport::Clock::now() + std::chrono::milliseconds(100));
            const bool hasAudio = requestResult == MacTransport::Result::Ready;
            if (requestResult == MacTransport::Result::Failed) { exchange(&shared->failed, 1); break; }
#endif
            if (hasAudio) {
#ifdef __APPLE__
                transport.markStarted();
#endif
            }
            if (!hasAudio) {
                if (stateDir.isEmpty()) continue;
                changes.clearQueue();
                uint64_t generation = 0;
                {
                    std::unique_lock lock(handler->mutex, std::try_to_lock);
                    if (!lock.owns_lock()) continue;
                    generation = handler->edited.load();
                    for (const auto &[id, value] : handler->pending) {
                        int32 index = 0;
                        if (auto *queue = changes.addParameterData(id, index)) queue->addPoint(0, value, index);
                    }
                    handler->pending.clear();
                }
                if (changes.getParameterCount()) {
                    ProcessData flush{}; flush.processMode = kRealtime; flush.symbolicSampleSize = kSample32;
                    flush.inputParameterChanges = &changes; flush.processContext = &context;
                    if (processor->process(flush) != kResultOk) { exchange(&shared->failed, 1); break; }
                }
                handler->applied = generation;
                continue;
            }
            const auto n = shared->frames;
            if (!n || n > kFrames) { exchange(&shared->failed, 1); respond(); break; }
            for (int bus = 0; bus < data.numInputs; ++bus) {
                auto &b = data.inputs[bus]; b.silenceFlags = bus ? HostProcessData::kAllChannelsSilent : 0;
                for (int ch = 0; ch < b.numChannels; ++ch) std::fill_n(b.channelBuffers32[ch], n, 0.f);
            }
            for (int bus = 0; bus < data.numOutputs; ++bus) {
                auto &b = data.outputs[bus]; b.silenceFlags = 0;
                for (int ch = 0; ch < b.numChannels; ++ch) std::fill_n(b.channelBuffers32[ch], n, 0.f);
            }
            for (uint32_t i = 0; i < n; ++i) {
                leftIn[i] = shared->input[2 * i]; rightIn[i] = shared->input[2 * i + 1];
                leftOut[i] = rightOut[i] = 0;
            }
            changes.clearQueue(); outputChanges.clearQueue();
            uint64_t generation = 0;
            {
                std::unique_lock lock(handler->mutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    generation = handler->edited.load();
                    for (const auto &[id, value] : handler->pending) {
                        int32 index = 0;
                        if (auto *queue = changes.addParameterData(id, index)) queue->addPoint(0, value, index);
                    }
                    handler->pending.clear();
                }
            }
            input.silenceFlags = 0; output.silenceFlags = 0;
            data.numSamples = int32(n);
            const auto start = std::chrono::steady_clock::now();
            const auto result = processor->process(data);
            const auto us = Word(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            if (us > shared->processMaxUs) exchange(&shared->processMaxUs, us);
            increment(&shared->processedBlocks);
            if (result != kResultOk) { exchange(&shared->failed, 1); respond(); break; }
            for (uint32_t i = 0; i < n; ++i) {
                shared->output[2 * i] = (output.silenceFlags & 1) ? 0.f : leftOut[i];
                shared->output[2 * i + 1] = (output.silenceFlags & 2) ? 0.f : rightOut[i];
            }
            if (generation) handler->applied = generation;
            context.projectTimeSamples += n;
            {
                std::unique_lock lock(handler->mutex, std::try_to_lock);
                if (lock.owns_lock()) for (int32 p = 0; p < outputChanges.getParameterCount(); ++p) {
                    auto *queue = outputChanges.getParameterData(p);
                    int32 offset = 0; ParamValue value = 0;
                    if (queue && queue->getPointCount() && queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk &&
                        (handler->returned.size() < 8192 || handler->returned.contains(queue->getParameterId())))
                        handler->returned[queue->getParameterId()] = value;
                }
            }
            respond();
        }
        processor->setProcessing(false);
#ifdef _WIN32
        CoUninitialize();
#endif
    });
    QElapsedTimer dirtyTimer, checkpointTimer;
    dirtyTimer.start(); checkpointTimer.start();
    uint64_t observed = 0, saved = 0;
    QString lastStateError;
    const auto saveState = [&] {
        auto audioState = owned(new StateStream);
        if (component->getState(audioState) != kResultOk || audioState->failed) {
            lastStateError = QStringLiteral("Could not read the plug-in audio state (maximum 4 MiB)."); return false;
        }
        QJsonValue editorState(QJsonValue::Null);
        if (controller) {
            auto state = owned(new StateStream);
            const auto result = controller->getState(state);
            if (state->failed) { lastStateError = QStringLiteral("The plug-in editor state exceeds 4 MiB."); return false; }
            if (result == kResultOk) editorState = vtrpon::vststate::blob(state->bytes);
            else if (result != kNotImplemented && result != kResultFalse) {
                lastStateError = QStringLiteral("Could not read the plug-in editor state."); return false;
            }
        }
        QJsonObject state{{"format", "vtr-pon-vst3-state"}, {"version", 1},
            {"module", vtrpon::vststate::moduleKey(modulePath)}, {"uid", classId}, {"serial", stateSerial + 1},
            {"component", vtrpon::vststate::blob(audioState->bytes)}, {"controller", editorState}};
        if (!vtrpon::vststate::write(statePath("snapshot.json"), state)) {
            lastStateError = QStringLiteral("Could not write the plug-in state snapshot."); return false;
        }
        ++stateSerial; lastStateError.clear(); return true;
    };
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        increment(&shared->heartbeat);
        if (controller) {
            std::map<ParamID, ParamValue> returned;
            { std::lock_guard lock(handler->mutex); returned.swap(handler->returned); }
            for (const auto &[id, value] : returned) controller->setParamNormalized(id, value);
        }
        const int32 restart = handler->restart.exchange(0);
        if (restart & (kIoChanged | kReloadComponent)) exchange(&shared->failed, 1);
        const bool publishReady = warmupComplete.exchange(false);
        if ((publishReady || (restart & kLatencyChanged)) && !load(&shared->failed)) {
            // getLatencySamples() is a UI-thread API, including the initial query.
            exchange(&shared->latency, Word(processor->getLatencySamples()));
            if (publishReady) exchange(&shared->ready, 1);
        }
        if (controller && (restart & kParamValuesChanged)) {
            for (int32 i = 0; i < controller->getParameterCount(); ++i) {
                ParameterInfo info{};
                if (controller->getParameterInfo(i, info) == kResultOk) handler->performEdit(info.id, controller->getParamNormalized(info.id));
            }
        }
        if (exchange(&shared->openEditor, 0) || handler->openRequested.exchange(false)) {
            if (!view) {
                if (controller) view = owned(controller->createView(ViewType::kEditor));
                if (view && view->isPlatformTypeSupported(editorPlatform) == kResultOk) {
                    view->setFrame(frame);
#ifdef _WIN32
                    if (FUnknownPtr<IPlugViewContentScaleSupport> scale(view); scale)
                        scale->setContentScaleFactor(float(editor.devicePixelRatioF()));
#endif
                    ViewRect rect{};
                    if (view->getSize(&rect) == kResultOk) frame->resizeView(view, &rect);
                    if (view->attached(reinterpret_cast<void *>(editor.winId()), editorPlatform) != kResultOk) { view->setFrame(nullptr); view = nullptr; }
                } else view = nullptr;
            }
            if (view) {
#ifdef _WIN32
                const auto owner = reinterpret_cast<HWND>(quintptr(shared->owner));
#endif
                editor.move(shared->x, shared->y); editor.showNormal();
#ifdef _WIN32
                if (IsWindow(owner)) SetWindowLongPtrW(reinterpret_cast<HWND>(editor.winId()), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
#else
                [NSApp activateIgnoringOtherApps:YES];
#endif
                editor.raise(); editor.activateWindow();
            }
            exchange(&shared->editorStatus, view ? 1 : -1);
        }
        if (exchange(&shared->closeEditor, 0)) editor.hide();
        if (!stateDir.isEmpty() && load(&shared->ready) && !load(&shared->failed)) {
            const auto generation = handler->edited.load();
            if (generation != observed) { observed = generation; dirtyTimer.restart(); }
            const int requestId = vtrpon::vststate::read(statePath("request.json"), 4096).value("request").toInt();
            const bool requested = requestId > acknowledged;
            // Debounce gestures; periodically capture state changes from plug-ins
            // that omit setDirty. get/setState stay on the plug-in UI thread.
            if (handler->applied.load() >= generation &&
                (requested || (!saved) || (saved != generation && dirtyTimer.elapsed() >= 500) || checkpointTimer.elapsed() >= 10000)) {
                const bool ok = saveState();
                saved = generation; // Failed snapshots retry on the next edit/checkpoint/request.
                acknowledged = std::max(acknowledged, requestId);
                reportState(ok, lastStateError); checkpointTimer.restart();
            }
        }
    });
    timer.start(100);
    app.setQuitOnLastWindowClosed(false);
    const int result = app.exec();
    stop = true; audio.join();
#ifdef __APPLE__
    [[NSProcessInfo processInfo] endActivity:audioActivity];
#endif
    if (view) { view->removed(); view->setFrame(nullptr); view = nullptr; }
    if (controller) controller->setComponentHandler(nullptr);
    component->setActive(false);
    return result;
}
}
int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    QApplication app(argc, argv);
#ifdef __APPLE__
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
#endif
    const auto args = app.arguments();
    try {
        if ((args.contains("--scan") || args.contains("--run")) && !monitorParent(args)) return 4;
        if (args.contains("--scan")) return scan(argument(args, "--scan"), argument(args, "--result"));
        if (args.contains("--run")) return run(app, args);
    } catch (...) { return 10; }
    return 1;
}
