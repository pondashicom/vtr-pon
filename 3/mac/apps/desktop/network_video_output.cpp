#include "network_video.h"
#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstring>

namespace vtrpon {
using Microsoft::WRL::ComPtr;
class NetworkVideoOutput::Impl {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    struct Slot {
        std::array<ComPtr<ID3D11Texture2D>, 2> textures;
        qint64 timecode = 0;
        quint64 generation = 0, serial = 0;
        std::chrono::steady_clock::time_point captured;
    };
    std::array<Slot, 3> readbacks;
    unsigned width = 0, height = 0, mask = 0;
    quint64 serial = 0, published = 0;
    void publish(ID3D11RenderTargetView *target, ID3D11RenderTargetView *alpha, qint64 timecode) {
        const auto generation = networkVideoOutputGeneration();
        const auto requested = networkVideoOutputMask();
        if (!generation || !target || !requested) {
            readbacks = {}; context.Reset(); device.Reset(); width = height = mask = 0;
            return;
        }
        std::array<ComPtr<ID3D11Texture2D>, 2> sources;
        std::array<UINT, 2> subresources{};
        const std::array<ID3D11RenderTargetView *, 2> targets{target, alpha};
        const std::array<bool, 2> needed{bool(requested & 1u), bool(requested & 14u)};
        D3D11_TEXTURE2D_DESC desc{};
        ComPtr<ID3D11Device> current;
        for (unsigned i = 0; i < 2; ++i) {
            if (!needed[i]) continue;
            if (!targets[i]) return;
            ComPtr<ID3D11Resource> resource; targets[i]->GetResource(&resource);
            if (FAILED(resource.As(&sources[i]))) return;
            D3D11_TEXTURE2D_DESC candidate{}; sources[i]->GetDesc(&candidate);
            if (candidate.Format != DXGI_FORMAT_B8G8R8A8_UNORM || candidate.SampleDesc.Count != 1) return;
            ComPtr<ID3D11Device> candidateDevice; sources[i]->GetDevice(&candidateDevice);
            if (current && (current.Get() != candidateDevice.Get()
                || desc.Width != candidate.Width || desc.Height != candidate.Height)) return;
            desc = candidate; current = candidateDevice;
            D3D11_RENDER_TARGET_VIEW_DESC view{}; targets[i]->GetDesc(&view);
            if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                && view.Texture2DArray.ArraySize == 1 && view.Texture2DArray.MipSlice == 0)
                subresources[i] = D3D11CalcSubresource(0, view.Texture2DArray.FirstArraySlice, desc.MipLevels);
            else if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0) return;
        }
        if (current.Get() != device.Get() || width != desc.Width || height != desc.Height || mask != requested) {
            readbacks = {}; device = current; device->GetImmediateContext(&context);
            width = desc.Width; height = desc.Height; mask = requested;
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width = width; staging.Height = height;
            staging.MipLevels = staging.ArraySize = 1;
            staging.Format = desc.Format; staging.SampleDesc.Count = 1;
            staging.Usage = D3D11_USAGE_STAGING; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto &slot : readbacks) for (unsigned i = 0; i < 2; ++i) {
                if (needed[i] && FAILED(device->CreateTexture2D(&staging, nullptr, &slot.textures[i]))) {
                    reportNetworkVideoOutputError(QStringLiteral("NDI output: GPU readback allocation failed."));
                    width = 0; return;
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto &slot : readbacks) {
            if (!slot.generation) continue;
            if (slot.generation != generation || now - slot.captured > std::chrono::milliseconds(200)) {
                slot.generation = 0; continue;
            }
            std::array<D3D11_MAPPED_SUBRESOURCE, 2> maps{};
            std::array<bool, 2> mapped{};
            bool ready = true, failed = false;
            for (unsigned i = 0; i < 2; ++i) {
                if (!needed[i]) continue;
                const auto hr = context->Map(slot.textures[i].Get(), 0, D3D11_MAP_READ,
                    D3D11_MAP_FLAG_DO_NOT_WAIT, &maps[i]);
                mapped[i] = SUCCEEDED(hr);
                ready = ready && mapped[i];
                failed = failed || (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING);
            }
            std::array<QByteArray, 2> pixels;
            if (ready && slot.serial > published) {
                for (unsigned i = 0; i < 2; ++i) if (needed[i]) {
                    pixels[i].resize(qsizetype(width) * height * 4);
                    for (unsigned y = 0; y < height; ++y)
                        std::memcpy(pixels[i].data() + size_t(y) * width * 4,
                            static_cast<const char *>(maps[i].pData) + size_t(y) * maps[i].RowPitch, width * 4);
                }
            }
            for (unsigned i = 0; i < 2; ++i)
                if (mapped[i]) context->Unmap(slot.textures[i].Get(), 0);
            if (failed) reportNetworkVideoOutputError(QStringLiteral("NDI output: GPU readback failed."));
            if (ready && slot.serial > published) {
                publishNetworkVideo(std::move(pixels[0]), std::move(pixels[1]), int(width), int(height),
                    generation, slot.timecode);
                published = slot.serial;
            }
            if (ready || failed) slot.generation = 0;
        }

        for (auto &slot : readbacks) {
            if (slot.generation) continue;
            for (unsigned i = 0; i < 2; ++i) if (needed[i])
                context->CopySubresourceRegion(slot.textures[i].Get(), 0, 0, 0, 0,
                    sources[i].Get(), subresources[i], nullptr);
            slot.generation = generation; slot.serial = ++serial; slot.captured = now;
            slot.timecode = timecode ? timecode : networkVideoTimecode();
            break;
        }
    }
};
NetworkVideoOutput::NetworkVideoOutput() : impl_(std::make_unique<Impl>()) {}
NetworkVideoOutput::~NetworkVideoOutput() = default;
void NetworkVideoOutput::publish(ID3D11RenderTargetView *target, ID3D11RenderTargetView *alpha, qint64 timecode) { impl_->publish(target, alpha, timecode); }
}
#else
namespace vtrpon {
class NetworkVideoOutput::Impl {};
NetworkVideoOutput::NetworkVideoOutput() = default;
NetworkVideoOutput::~NetworkVideoOutput() = default;
void NetworkVideoOutput::publish(ID3D11RenderTargetView *, ID3D11RenderTargetView *, qint64) {}
}
#endif
