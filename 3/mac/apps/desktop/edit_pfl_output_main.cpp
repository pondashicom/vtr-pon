#include "edit_pfl_protocol.h"
#include "edit_pfl_renderer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>

using Microsoft::WRL::ComPtr;
namespace {
int listDevices() {
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) return 1;
    QJsonArray devices;
    QString defaultId;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            ComPtr<IMMDevice> defaultDevice;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(defaultDevice->GetId(&id))) { defaultId = QString::fromWCharArray(id); CoTaskMemFree(id); }
            }
            ComPtr<IMMDeviceCollection> collection;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
                UINT count = 0; collection->GetCount(&count);
                for (UINT index = 0; index < count; ++index) {
                    ComPtr<IMMDevice> device;
                    if (FAILED(collection->Item(index, &device))) continue;
                    LPWSTR id = nullptr;
                    if (FAILED(device->GetId(&id))) continue;
                    const auto key = QString::fromWCharArray(id); CoTaskMemFree(id);
                    QString name = key;
                    ComPtr<IPropertyStore> properties;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                        PROPVARIANT value; PropVariantInit(&value);
                        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                            name = QString::fromWCharArray(value.pwszVal);
                        PropVariantClear(&value);
                    }
                    devices.append(QJsonObject{{QStringLiteral("id"), key}, {QStringLiteral("name"), name}});
                }
            }
        }
    }
    CoUninitialize();
    const auto json = QJsonDocument(QJsonObject{{QStringLiteral("default"), defaultId},
        {QStringLiteral("devices"), devices}}).toJson(QJsonDocument::Compact);
    std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
    return 0;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
struct MappingView {
    vtrpon::pfl::SharedAudio *audio = nullptr;
    ~MappingView() { if (audio) UnmapViewOfFile(audio); }
};
std::int64_t nowNs() {
    static const auto frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (c.QuadPart / frequency) * 1000000000LL
        + (c.QuadPart % frequency) * 1000000000LL / frequency;
}
HRESULT renderDevice(vtrpon::pfl::SharedAudio &audio, LPCWSTR endpoint, HANDLE parent) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    // The Controller explicitly saves this selection before opening the default.
    hr = QString::fromWCharArray(endpoint) == QStringLiteral("@system-default")
        ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
        : enumerator->GetDevice(endpoint, &device);
    if (FAILED(hr)) return hr;
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = vtrpon::pfl::kChannels;
    format.nSamplesPerSec = vtrpon::pfl::kRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * sizeof(float);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    // Shared mode and a private session: no endpoint/master volume changes,
    // exclusive access, capture/loopback, or Program session APIs.
    GUID session{};
    if (FAILED(CoCreateGuid(&session))) return E_FAIL;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
        400000, 0, &format, &session);
    if (FAILED(hr)) return hr;
    Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!event.value) return HRESULT_FROM_WIN32(GetLastError());
    hr = client->SetEventHandle(event.value);
    if (FAILED(hr)) return hr;
    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) return hr;
    UINT32 capacity = 0;
    hr = client->GetBufferSize(&capacity);
    if (FAILED(hr)) return hr;
    BYTE *buffer = nullptr;
    hr = render->GetBuffer(capacity, &buffer);
    if (FAILED(hr)) return hr;
    hr = render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return hr;
    hr = client->Start();
    if (FAILED(hr)) return hr;
    auto lastRenderNs = nowNs();
    audio.heartbeatNs = lastRenderNs;
    audio.state = static_cast<std::uint32_t>(vtrpon::pfl::State::Ready);
    vtrpon::pfl::Renderer renderer;
    HANDLE waits[]{parent, event.value};
    std::int64_t closeAfterNs = 0;
    while (true) {
        if (audio.quit.load() && closeAfterNs == 0) {
            audio.enabled = 0;
            // Let the renderer's short ramp and already queued endpoint PCM
            // leave this private buffer before Stop. The Controller never
            // waits here, and retains its independent child-kill deadline.
            closeAfterNs = nowNs() + 20000000LL
                + static_cast<std::int64_t>(capacity) * 1000000000LL / vtrpon::pfl::kRate;
        }
        if (closeAfterNs != 0 && nowNs() >= closeAfterNs) break;
        const auto wait = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_TIMEOUT) {
            if (nowNs() - lastRenderNs > 2000000000LL) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break;
            }
            continue;
        }
        if (wait != WAIT_OBJECT_0 + 1) { hr = E_FAIL; break; }
        if (nowNs() - lastRenderNs > 2000000000LL) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break;
        }
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;
        if (padding >= capacity) continue;
        const UINT32 frames = capacity - padding;
        // Schedule at the shared endpoint buffer, not at the physical device.
        // Keep device/transport latency (including Bluetooth) out of the
        // producer's deadline. A cumulative submitted-frame counter is also
        // no longer a reliable queue length after a callback underrun.
        const auto bufferTime = nowNs()
            + static_cast<std::int64_t>(padding) * 1000000000LL / vtrpon::pfl::kRate;
        hr = render->GetBuffer(frames, &buffer);
        if (FAILED(hr)) break;
        renderer.render(audio, reinterpret_cast<float *>(buffer), frames, bufferTime);
        hr = render->ReleaseBuffer(frames, 0);
        if (FAILED(hr)) break;
        lastRenderNs = nowNs();
        audio.heartbeatNs = lastRenderNs;
    }
    (void)client->Stop();
    return hr;
}
} // namespace

int main(int argc, char **argv) {
    // Driver faults belong to this child; never show a system crash dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 2 && args[1] == QStringLiteral("--list")) return listDevices();
    if (args.size() != 4 || args[2].isEmpty()) return 2;
    bool validPid = false;
    const DWORD pid = args[3].toUInt(&validPid);
    if (!validPid || pid == 0) return 2;
    Handle parent{OpenProcess(SYNCHRONIZE, FALSE, pid)};
    if (!parent.value) return 3;
    Handle mapping{OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
        reinterpret_cast<LPCWSTR>(args[1].utf16()))};
    if (!mapping.value) return 4;
    MappingView view{static_cast<vtrpon::pfl::SharedAudio *>(
        MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vtrpon::pfl::SharedAudio)))};
    if (!view.audio || view.audio->version != vtrpon::pfl::kVersion) return 5;
    auto &audio = *view.audio;
    audio.heartbeatNs = nowNs();
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT result = SUCCEEDED(init)
        ? renderDevice(audio, reinterpret_cast<LPCWSTR>(args[2].utf16()), parent.value) : init;
    if (FAILED(result)) {
        audio.error = static_cast<std::uint32_t>(result);
        audio.state = static_cast<std::uint32_t>(vtrpon::pfl::State::Failed);
    } else audio.state = static_cast<std::uint32_t>(vtrpon::pfl::State::Closed);
    if (SUCCEEDED(init)) CoUninitialize();
    return FAILED(result) ? 6 : 0;
}
