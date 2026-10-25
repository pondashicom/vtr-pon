// Fault-injection plug-in, built only for isolated developer tests.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <atomic>
#include <cmath>
#import <Cocoa/Cocoa.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
void hangWhenRequested(const char *name) {
    const char *path = std::getenv(name);
    if (!path || !*path) return;
    if (auto *file = std::fopen(path, "w")) { std::fprintf(file, "%d\n", getpid()); std::fclose(file); }
    for (;;) pause();
}
[[maybe_unused]] const bool initialized = [] {
    hangWhenRequested("VTRPON_TEST_VST_LOAD_HANG"); return true;
}();
class TestView final : public CPluginView {
public:
    TestView() { setRect(ViewRect(0, 0, 320, 200)); }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type && std::strcmp(type, kPlatformTypeNSView) == 0 ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API attached(void *parent, FIDString type) override {
        if (!parent || isPlatformTypeSupported(type) != kResultOk) return kResultFalse;
        child = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)];
        [(__bridge NSView *)parent addSubview:child]; return kResultOk;
    }
    tresult PLUGIN_API removed() override { [child removeFromSuperview]; child = nil; return kResultOk; }
private:
    NSView *child = nil;
};
class Effect final : public SingleComponentEffect {
public:
    explicit Effect(int mode) : mode(mode), gainValue(mode == 6 ? .25 : .5) {}
    ~Effect() override { [controlTimer invalidate]; }
    static FUnknown *gain(void *) { return static_cast<IComponent *>(new Effect(0)); }
    static FUnknown *crash(void *) { return static_cast<IComponent *>(new Effect(1)); }
    static FUnknown *hang(void *) { return static_cast<IComponent *>(new Effect(2)); }
    static FUnknown *nan(void *) { return static_cast<IComponent *>(new Effect(3)); }
    static FUnknown *uiHang(void *) { return static_cast<IComponent *>(new Effect(4)); }
    static FUnknown *reject(void *) { return static_cast<IComponent *>(new Effect(5)); }
    static FUnknown *slow(void *) { return static_cast<IComponent *>(new Effect(6)); }
    static FUnknown *blockDelay(void *) { return static_cast<IComponent *>(new Effect(7)); }
    IPlugView *PLUGIN_API createView(FIDString) override {
        if (mode == 4) for (;;) pause();
        return new TestView;
    }
    tresult PLUGIN_API initialize(FUnknown *context) override {
        auto result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        parameters.addParameter(STR16("Gain"), nullptr, 0, mode == 6 ? .25 : .5, ParameterInfo::kCanAutomate, 0);
        const char *control = std::getenv("VTRPON_TEST_VST_STATE_CONTROL_DIR");
        if (control && *control) {
            controlDirectory = control;
            controlTimer = [NSTimer scheduledTimerWithTimeInterval:.025 repeats:YES block:^(NSTimer *) { readControl(); }];
        }
        return kResultOk;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        if (mode == 5) return kResultFalse;
        if (mode == 6) std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return SingleComponentEffect::setupProcessing(setup);
    }
    struct AudioState { uint64_t magic = 0x5653545354415445; double gain = .5, hidden = 1.; };
    tresult PLUGIN_API getState(IBStream *stream) override {
        if (stateFault == 1) return kResultFalse;
        if (stateFault == 2) for (;;) pause();
        if (stateFault == 3) {
            char bytes[16384]{};
            for (int i = 0; i < 300; ++i) if (stream->write(bytes, sizeof(bytes)) != kResultOk) return kResultFalse;
        }
        AudioState state; state.gain = gainValue; state.hidden = hiddenValue;
        return stream->write(&state, sizeof(state));
    }
    tresult PLUGIN_API setState(IBStream *stream) override {
        // Exercise the SDK's successful short-read/EOF contract as wrappers do.
        char buffer[32768]{}; int32 bytesRead = 0;
        if (stream->read(buffer, sizeof(buffer), &bytesRead) != kResultOk || bytesRead != sizeof(AudioState)) return kResultFalse;
        AudioState state; std::memcpy(&state, buffer, sizeof(state));
        if (state.magic != AudioState{}.magic ||
            !std::isfinite(state.gain) || !std::isfinite(state.hidden)) return kResultFalse;
        gainValue = state.gain; hiddenValue = state.hidden; setParamNormalized(0, state.gain); return kResultOk;
    }
    tresult PLUGIN_API getEditorState(IBStream *stream) override { return stream->write(&editorMarker, sizeof(editorMarker)); }
    tresult PLUGIN_API setEditorState(IBStream *stream) override {
        int32 bytesRead = 0;
        if (stream->read(&editorMarker, sizeof(editorMarker), &bytesRead) != kResultOk || bytesRead != sizeof(editorMarker)) return kResultFalse;
        const auto path = controlDirectory + "/" + std::to_string(getpid()) + ".restored";
        if (auto *file = std::fopen(path.c_str(), "w")) { std::fprintf(file, "%llu\n", (unsigned long long)editorMarker); std::fclose(file); }
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData &data) override {
        if (data.inputParameterChanges) for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
            auto *queue = data.inputParameterChanges->getParameterData(i);
            int32 offset = 0; ParamValue value = 0;
            if (queue && queue->getParameterId() == 0 && queue->getPointCount() &&
                queue->getPoint(queue->getPointCount()-1, offset, value) == kResultOk) gainValue = value;
        }
        bool triggered = data.numSamples && data.inputs[0].channelBuffers32[0][0] > .2f;
        if (triggered && mode == 7) usleep(30000);
        if (triggered && mode == 1) _exit(77);
        if (triggered && mode == 2) for (;;) pause();
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < data.numSamples; ++i)
            data.outputs[0].channelBuffers32[ch][i] = triggered && mode == 3
                ? std::numeric_limits<float>::quiet_NaN()
                : data.inputs[0].channelBuffers32[ch][i] * float(gainValue.load() * hiddenValue.load());
        return kResultOk;
    }
private:
    void readControl() {
        const auto path = controlDirectory + "/" + std::to_string(getpid()) + ".cmd";
        auto *file = std::fopen(path.c_str(), "r"); if (!file) return;
        unsigned long long serial = 0, marker = 0; double gain = 0, hidden = 0; int fault = 0;
        const bool read = std::fscanf(file, "%llu %lf %lf %llu %d", &serial, &gain, &hidden, &marker, &fault) == 5;
        std::fclose(file);
        if (!read || serial <= controlSerial) return;
        controlSerial = serial; hiddenValue = hidden; editorMarker = marker; stateFault = fault;
        setParamNormalized(0, gain);
        if (componentHandler) {
            componentHandler->beginEdit(0); componentHandler->performEdit(0, gain); componentHandler->endEdit(0);
            if (FUnknownPtr<IComponentHandler2> extended(componentHandler); extended) extended->setDirty(true);
        }
        const auto ack = controlDirectory + "/" + std::to_string(getpid()) + ".edited";
        if (auto *file = std::fopen(ack.c_str(), "w")) { std::fprintf(file, "%llu\n", serial); std::fclose(file); }
    }
    int mode;
    std::atomic<double> gainValue, hiddenValue{1.};
    int stateFault = 0;
    uint64_t editorMarker = 11, controlSerial = 0;
    std::string controlDirectory;
    NSTimer *controlTimer = nil;
};
const FUID ids[] = {
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540001),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540002),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540003),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540004),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540005),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540006),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540007),
    FUID(0x42545250,0x4F4E5653,0x54335445,0x53540008)};
}
bool InitModule() { hangWhenRequested("VTRPON_TEST_VST_SCAN_HANG"); return true; }
bool DeinitModule() { return true; }
BEGIN_FACTORY_DEF("VTR-PON Test", "", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[0]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test Gain",0,"Fx","1.0",kVstVersionString,Effect::gain)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[1]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test Crash",0,"Fx","1.0",kVstVersionString,Effect::crash)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[2]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test Hang",0,"Fx","1.0",kVstVersionString,Effect::hang)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[3]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test NaN",0,"Fx","1.0",kVstVersionString,Effect::nan)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[4]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test UI Hang",0,"Fx","1.0",kVstVersionString,Effect::uiHang)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[5]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test Reject",0,"Fx","1.0",kVstVersionString,Effect::reject)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[6]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test Slow",0,"Fx","1.0",kVstVersionString,Effect::slow)
DEF_CLASS2(INLINE_UID_FROM_FUID(ids[7]),PClassInfo::kManyInstances,kVstAudioEffectClass,"Mac Test Block Delay",0,"Fx","1.0",kVstVersionString,Effect::blockDelay)
END_FACTORY
