// Deterministic VST3 fixture: real SDK factory/component plus isolated faults.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include <windows.h>
#include <cstring>
#include <limits>
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
void hangScanWhenRequested(const wchar_t *environmentName) {
    wchar_t eventName[256]{};
    const auto length = GetEnvironmentVariableW(environmentName, eventName, 256);
    if (length && length < 256) {
        if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName)) {
            SetEvent(ready);
            CloseHandle(ready);
            Sleep(INFINITE);
        }
    }
}
// MSVC runs DLL static initialization while holding the Windows loader lock.
// This differs from InitModule(), which the VST host calls after LoadLibrary.
[[maybe_unused]] const bool loaderInitialized = [] {
    hangScanWhenRequested(L"VTRPON_TEST_VST_SCAN_DLL_HANG_EVENT");
    return true;
}();
class TestView final : public CPluginView {
public:
    TestView() { setRect(ViewRect(0, 0, 320, 200)); }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API attached(void *parent, FIDString type) override {
        if (isPlatformTypeSupported(type) != kResultOk) return kResultFalse;
        child_ = CreateWindowExW(0, L"STATIC", L"VST test editor", WS_CHILD | WS_VISIBLE,
            0, 0, 320, 200, static_cast<HWND>(parent), nullptr, GetModuleHandleW(nullptr), nullptr);
        return child_ ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API removed() override {
        if (child_) DestroyWindow(child_);
        child_ = nullptr;
        return kResultOk;
    }
private:
    HWND child_ = nullptr;
};
class Effect final : public SingleComponentEffect {
public:
    explicit Effect(int mode) : mode_(mode) {}
    static FUnknown *createGain(void *) { return static_cast<IComponent *>(new Effect(0)); }
    static FUnknown *createCrash(void *) { return static_cast<IComponent *>(new Effect(1)); }
    static FUnknown *createHang(void *) { return static_cast<IComponent *>(new Effect(2)); }
    static FUnknown *createNan(void *) { return static_cast<IComponent *>(new Effect(3)); }
    static FUnknown *createUiHang(void *) { return static_cast<IComponent *>(new Effect(4)); }
    static FUnknown *createBurst(void *) { return static_cast<IComponent *>(new Effect(5)); }
    static FUnknown *createReject(void *) { return static_cast<IComponent *>(new Effect(6)); }
    static FUnknown *createSlow(void *) { return static_cast<IComponent *>(new Effect(7)); }
    static FUnknown *createStartupHang(void *) { return static_cast<IComponent *>(new Effect(8)); }
    IPlugView *PLUGIN_API createView(FIDString) override {
        if (mode_ == 4) Sleep(INFINITE);
        return mode_ == 0 || mode_ == 5 ? new TestView : nullptr;
    }
    tresult PLUGIN_API initialize(FUnknown *context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        if (mode_ == 6) { Sleep(300); return kResultFalse; }
        if (mode_ == 7) Sleep(500);
        if (mode_ == 8) Sleep(INFINITE);
        return SingleComponentEffect::setupProcessing(setup);
    }
    tresult PLUGIN_API process(ProcessData &data) override {
        // A later block can trigger a fault after an earlier block succeeded.
        if (data.numSamples && data.inputs[0].channelBuffers32[0][0] > 0.2f) {
            if (mode_ == 1) TerminateProcess(GetCurrentProcess(), 77);
            if (mode_ == 2) Sleep(INFINITE);
            if (mode_ == 5 && ++blocks_ % 8 == 1) Sleep(30);
        }
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < data.numSamples; ++i)
                data.outputs[0].channelBuffers32[ch][i] = mode_ == 3
                    ? std::numeric_limits<float>::quiet_NaN() : data.inputs[0].channelBuffers32[ch][i] * (mode_ == 7 ? 0.25f : 0.5f);
        return kResultOk;
    }
private:
    int mode_;
    int blocks_ = 0;
};
const FUID gainId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540001);
const FUID crashId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540002);
const FUID hangId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540003);
const FUID nanId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540004);
const FUID uiHangId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540005);
const FUID burstId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540006);
const FUID rejectId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540007);
const FUID slowId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540008);
const FUID startupHangId(0x42545250, 0x4F4E5653, 0x54335445, 0x53540009);
}
bool InitModule() {
    hangScanWhenRequested(L"VTRPON_TEST_VST_SCAN_HANG_EVENT");
    return true;
}
bool DeinitModule() { return true; }
BEGIN_FACTORY_DEF("VTR-PON Test", "", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(gainId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Gain", 0, "Fx", "1.0", kVstVersionString, Effect::createGain)
DEF_CLASS2(INLINE_UID_FROM_FUID(crashId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Crash", 0, "Fx", "1.0", kVstVersionString, Effect::createCrash)
DEF_CLASS2(INLINE_UID_FROM_FUID(hangId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Hang", 0, "Fx", "1.0", kVstVersionString, Effect::createHang)
DEF_CLASS2(INLINE_UID_FROM_FUID(nanId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test NaN", 0, "Fx", "1.0", kVstVersionString, Effect::createNan)
DEF_CLASS2(INLINE_UID_FROM_FUID(uiHangId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test UI Hang", 0, "Fx", "1.0", kVstVersionString, Effect::createUiHang)
DEF_CLASS2(INLINE_UID_FROM_FUID(burstId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Burst", 0, "Fx", "1.0", kVstVersionString, Effect::createBurst)
DEF_CLASS2(INLINE_UID_FROM_FUID(rejectId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Reject", 0, "Fx", "1.0", kVstVersionString, Effect::createReject)
DEF_CLASS2(INLINE_UID_FROM_FUID(slowId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Slow Start", 0, "Fx", "1.0", kVstVersionString, Effect::createSlow)
DEF_CLASS2(INLINE_UID_FROM_FUID(startupHangId), PClassInfo::kManyInstances, kVstAudioEffectClass,
    "VTR Test Startup Hang", 0, "Fx", "1.0", kVstVersionString, Effect::createStartupHang)
END_FACTORY
