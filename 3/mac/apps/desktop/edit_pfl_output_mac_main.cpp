#include "edit_pfl_protocol.h"
#include "edit_pfl_renderer.h"
#include "edit_pfl_clock_mac.h"

#include <QCoreApplication>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
using namespace vtrpon::pfl;
AudioObjectPropertyAddress property(AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}
bool aliveOutput(AudioDeviceID device) {
    UInt32 alive = 0, size = sizeof(alive);
    auto address = property(kAudioDevicePropertyDeviceIsAlive);
    if (device == kAudioObjectUnknown || AudioObjectGetPropertyData(device,
            &address, 0, nullptr, &size, &alive) != noErr || !alive) return false;
    address = property(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput);
    return AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) == noErr && size > 0;
}
QString deviceUid(AudioDeviceID device) {
    const auto address = property(kAudioDevicePropertyDeviceUID);
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr
        || !value) return {};
    QString result(CFStringGetLength(value), Qt::Uninitialized);
    CFStringGetCharacters(value, CFRangeMake(0, result.size()),
                          reinterpret_cast<UniChar *>(result.data()));
    CFRelease(value);
    return result;
}
AudioDeviceID resolveDevice(const QString &uid) {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (uid == QStringLiteral("@system-default")) {
        const auto address = property(kAudioHardwarePropertyDefaultOutputDevice);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0,
                nullptr, &size, &device) != noErr) return kAudioObjectUnknown;
    } else {
        const auto value = CFStringCreateWithCharacters(nullptr,
            reinterpret_cast<const UniChar *>(uid.utf16()), uid.size());
        const auto address = property(kAudioHardwarePropertyTranslateUIDToDevice);
        const auto status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
            &address, sizeof(value), &value, &size, &device);
        CFRelease(value);
        if (status != noErr) return kAudioObjectUnknown;
    }
    return aliveOutput(device) ? device : kAudioObjectUnknown;
}

struct RenderContext {
    SharedAudio &audio;
    Renderer renderer;
    std::atomic<OSStatus> error{noErr};
    std::atomic<std::uint32_t> callbacks{0};
    std::atomic<float> peak{0};
};
static_assert(std::atomic<float>::is_always_lock_free);
OSStatus render(void *user, AudioUnitRenderActionFlags *, const AudioTimeStamp *,
                UInt32, UInt32 frames, AudioBufferList *data) {
    auto &context = *static_cast<RenderContext *>(user);
    if (!data) return kAudio_ParamError;
    // AUHAL converts this private 48 kHz interleaved stereo client format to
    // the endpoint's mix format. Never allocate, log, or call HAL from here.
    const auto bytes = frames * kChannels * sizeof(float);
    if (frames > 16384 || data->mNumberBuffers != 1
        || data->mBuffers[0].mNumberChannels != kChannels
        || !data->mBuffers[0].mData || data->mBuffers[0].mDataByteSize < bytes) {
        for (UInt32 i = 0; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData)
                std::memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        context.error.store(kAudio_ParamError);
        return kAudio_ParamError;
    }
    auto *output = static_cast<float *>(data->mBuffers[0].mData);
    // HAL pulls a client block now. Use its service time, excluding any
    // downstream hardware/Bluetooth presentation latency, like the WASAPI
    // endpoint-buffer clock. Renderer smooths callback scheduling jitter.
    const auto now = monotonicNowNs();
    context.renderer.render(context.audio, output, frames, now);
    float peak = 0;
    for (UInt32 i = 0; i < frames * kChannels; ++i)
        peak = std::max(peak, std::abs(output[i]));
    context.peak.store(std::max(context.peak.load(), peak));
    context.callbacks.fetch_add(1);
    context.audio.heartbeatNs.store(now);
    return noErr;
}

OSStatus run(SharedAudio &audio, const QString &requested, RenderContext &context) {
    const auto device = resolveDevice(requested);
    if (device == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    const auto uid = deviceUid(device);
    AudioComponentDescription description{};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_HALOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    if (!component) return kAudio_ParamError;
    AudioUnit unit = nullptr;
    auto result = AudioComponentInstanceNew(component, &unit);
    if (result != noErr) return result;
    struct Unit {
        AudioUnit value;
        ~Unit() {
            AudioOutputUnitStop(value);
            AudioUnitUninitialize(value);
            AudioComponentInstanceDispose(value);
        }
    } lifetime{unit};
    UInt32 enabled = 1, disabled = 0;
    if ((result = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Output, 0, &enabled, sizeof(enabled))) != noErr) return result;
    if ((result = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Input, 1, &disabled, sizeof(disabled))) != noErr) return result;
    if ((result = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0, &device, sizeof(device))) != noErr) return result;
    AudioDeviceID opened = kAudioObjectUnknown;
    UInt32 size = sizeof(opened);
    if ((result = AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0, &opened, &size)) != noErr) return result;
    if (opened != device || deviceUid(opened) != uid) return kAudioHardwareBadDeviceError;
    AudioStreamBasicDescription format{};
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = format.mBytesPerFrame = kChannels * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    if ((result = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input, 0, &format, sizeof(format))) != noErr) return result;
    UInt32 maximumFrames = 16384;
    if ((result = AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global, 0, &maximumFrames, sizeof(maximumFrames))) != noErr) return result;
    AURenderCallbackStruct callback{render, &context};
    if ((result = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0, &callback, sizeof(callback))) != noErr) return result;
    if ((result = AudioUnitInitialize(unit)) != noErr) return result;
    if ((result = AudioOutputUnitStart(unit)) != noErr) return result;
    while (!context.callbacks.load()) {
        if (audio.quit.load()) return noErr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    audio.state = static_cast<std::uint32_t>(State::Ready);
    const auto encodedUid = uid.toUtf8();
    std::printf("PFL_COREAUDIO ready uid=%s device=%u rate=%u channels=%u\n",
                encodedUid.constData(), opened, kRate, kChannels);
    std::fflush(stdout);
    auto lastCheck = monotonicNowNs(), lastReport = lastCheck;
    std::int64_t closeAfter = 0;
    while (true) {
        const auto now = monotonicNowNs();
        if (audio.quit.load() && !closeAfter) {
            audio.enabled = 0;
            // Drain the five millisecond release ramp before stopping AUHAL.
            closeAfter = now + 100000000LL;
        }
        if (closeAfter && now >= closeAfter) break;
        if (context.error.load() != noErr) return context.error.load();
        if (now - lastCheck >= 250000000LL) {
            size = sizeof(opened);
            if (!aliveOutput(device)
                || AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global, 0, &opened, &size) != noErr
                || opened != device || deviceUid(opened) != uid
                || (requested == QStringLiteral("@system-default") && resolveDevice(requested) != device))
                return kAudioHardwareBadDeviceError;
            lastCheck = now;
        }
        if (now - lastReport >= 1000000000LL) {
            std::printf("PFL_COREAUDIO callbacks=%u peak=%.6f\n",
                        context.callbacks.load(), context.peak.exchange(0));
            std::fflush(stdout);
            lastReport = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return noErr;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4 || !args[1].startsWith(QStringLiteral("fd:")) || args[2].isEmpty()) return 2;
    bool validFd = false, validPid = false;
    const int descriptor = args[1].mid(3).toInt(&validFd);
    const auto parent = args[3].toLongLong(&validPid);
    if (!validFd || descriptor != STDIN_FILENO || !validPid || parent <= 1 || getppid() != parent) return 3;
    struct stat info{};
    if (fstat(descriptor, &info) != 0) {
        std::perror("PFL shared descriptor");
        return 4;
    }
    // Darwin rounds POSIX shared memory to a VM page boundary.
    if (info.st_uid != geteuid() || info.st_size < sizeof(SharedAudio)) {
        std::fprintf(stderr, "PFL shared mapping size=%lld expected=%zu owner=%u\n",
                     info.st_size, sizeof(SharedAudio), info.st_uid);
        return 4;
    }
    void *memory = mmap(nullptr, sizeof(SharedAudio), PROT_READ | PROT_WRITE,
                        MAP_SHARED, descriptor, 0);
    close(descriptor);
    if (memory == MAP_FAILED) return 4;
    auto &audio = *static_cast<SharedAudio *>(memory);
    if (audio.version != kVersion) { munmap(memory, sizeof(SharedAudio)); return 5; }
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    const auto started = monotonicNowNs();
    RenderContext context{audio};
    std::atomic<bool> done{false};
    // Runs independently of AudioUnit open/close/driver calls. A dead parent,
    // stalled output or runaway resident memory only terminates this child.
    std::thread watchdog([&] {
        while (!done.load()) {
            if (getppid() != parent) _exit(0);
            const auto heartbeat = audio.heartbeatNs.load();
            const bool opening = audio.state.load() == static_cast<std::uint32_t>(State::Opening);
            const auto elapsed = monotonicNowNs() - (opening ? started : heartbeat);
            mach_task_basic_info_data_t usage{};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            const bool overBudget = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&usage), &count) == KERN_SUCCESS
                && usage.resident_size > 256ULL * 1024 * 1024;
            if (elapsed > (opening ? 5000000000LL : 2000000000LL) || overBudget) {
                audio.error = static_cast<std::uint32_t>(kAudioHardwareUnspecifiedError);
                audio.state = static_cast<std::uint32_t>(State::Failed);
                _exit(6);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    const auto result = run(audio, args[2], context);
    if (result != noErr) {
        std::fprintf(stderr, "PFL_COREAUDIO failed status=%d\n", result);
        audio.error = static_cast<std::uint32_t>(result);
        audio.state = static_cast<std::uint32_t>(State::Failed);
    } else audio.state = static_cast<std::uint32_t>(State::Closed);
    done = true;
    watchdog.join();
    munmap(memory, sizeof(SharedAudio));
    return result == noErr ? 0 : 6;
}
