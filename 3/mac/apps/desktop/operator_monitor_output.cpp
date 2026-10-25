#include "operator_monitor_output.h"
#include "operator_monitor_transport.h"
#include <QCoreApplication>
#include <QUuid>
#include <QDebug>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <chrono>

namespace vtrpon {
namespace {
std::atomic_bool enabled{false};
}
QString operatorMonitorChannelName() {
  static const QString name = QStringLiteral("Local\\VtrPonOperator-%1-%2")
      .arg(QCoreApplication::applicationPid())
      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  return name;
}
void setOperatorMonitorEnabled(bool value) {
  enabled.store(value, std::memory_order_relaxed);
}

class OperatorMonitorControlFeed::Impl {
public:
  Impl() {
    const auto name = (operatorMonitorChannelName() + QStringLiteral("-info")).toStdWString();
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(OperatorMonitorPresentation), name.c_str());
    mutex = CreateMutexW(nullptr, FALSE, (name + L"-mutex").c_str());
    if (mapping) state = static_cast<OperatorMonitorPresentation *>(MapViewOfFile(
        mapping, FILE_MAP_WRITE, 0, 0, sizeof(OperatorMonitorPresentation)));
  }
  ~Impl() {
    if (state) UnmapViewOfFile(state);
    if (mapping) CloseHandle(mapping);
    if (mutex) CloseHandle(mutex);
  }
  HANDLE mapping = nullptr, mutex = nullptr;
  OperatorMonitorPresentation *state = nullptr;
};
OperatorMonitorControlFeed::OperatorMonitorControlFeed() : impl_(std::make_unique<Impl>()) {}
OperatorMonitorControlFeed::~OperatorMonitorControlFeed() = default;
bool OperatorMonitorControlFeed::available() const { return impl_->state && impl_->mutex; }
void OperatorMonitorControlFeed::publish(const QByteArray &json) {
  if (!impl_->state || !impl_->mutex || json.size() > sizeof(impl_->state->json)) return;
  const DWORD acquired = WaitForSingleObject(impl_->mutex, 0);
  if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) return;
  std::memcpy(impl_->state->json, json.constData(), json.size());
  impl_->state->size = unsigned(json.size());
  ++impl_->state->serial;
  ReleaseMutex(impl_->mutex);
}

class OperatorMonitorOutput::Impl {
public:
  ~Impl() {
    if (frame) UnmapViewOfFile(frame);
    if (mapping) CloseHandle(mapping);
    if (mutex) CloseHandle(mutex);
  }
  void fail() {
    failed = true;
    qWarning("Operator monitor copy unavailable; ON AIR output is unaffected.");
  }
  void publish(ID3D11RenderTargetView *target) {
    if (!enabled.load(std::memory_order_relaxed)) {
      pending = false;
      nextCopyAt = {};
      return;
    }
    if (failed || !target) return;
    const auto copyTime = std::chrono::steady_clock::now();
    // Poll an outstanding readback on every source frame. Throttling this poll
    // to the copy cadence also throws away a whole slot when the GPU is not ready.
    if (copyTime < nextCopyAt && !pending) return;
    const auto now = GetTickCount64();
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    target->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    if (!resource || FAILED(resource.As(&source))) { fail(); return; }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    target->GetDesc(&view);
    UINT mip = 0, slice = 0;
    if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
      mip = view.Texture2D.MipSlice;
    else if (view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
             view.Texture2DArray.ArraySize == 1) {
      mip = view.Texture2DArray.MipSlice;
      slice = view.Texture2DArray.FirstArraySlice;
    } else { fail(); return; }
    const UINT sourceSubresource = D3D11CalcSubresource(mip, slice, desc.MipLevels);
    desc.Width = (std::max)(1U, desc.Width >> mip);
    desc.Height = (std::max)(1U, desc.Height >> mip);
    if (desc.SampleDesc.Count != 1 || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Width > 3840 || desc.Height > 2160) { fail(); return; }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!staging) {
      const auto name = operatorMonitorChannelName().toStdWString();
      mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
          0, sizeof(OperatorMonitorFrame), name.c_str());
      mutex = CreateMutexW(nullptr, FALSE, (name + L"-mutex").c_str());
      if (mapping) frame = static_cast<OperatorMonitorFrame *>(
          MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(OperatorMonitorFrame)));
      if (!frame || !mutex) { fail(); return; }
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      desc.ArraySize = 1;
      desc.MipLevels = 1;
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) { fail(); return; }
      width = desc.Width;
      height = desc.Height;
    }
    if (width != desc.Width || height != desc.Height) { fail(); return; }
    if (pending) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      const HRESULT result = context->Map(staging.Get(), 0, D3D11_MAP_READ,
          D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (result == DXGI_ERROR_WAS_STILL_DRAWING) return;
      if (FAILED(result)) { fail(); return; }
      const DWORD acquired = WaitForSingleObject(mutex, 0);
      if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) {
        const double scale = (std::min)({1.0, double(operatorMonitorMaxWidth) / width,
                                             double(operatorMonitorMaxHeight) / height});
        const unsigned outWidth = (std::max)(1U, unsigned(width * scale));
        const unsigned outHeight = (std::max)(1U, unsigned(height * scale));
        for (unsigned y = 0; y < outHeight; ++y) {
          const auto *row = static_cast<const unsigned char *>(mapped.pData) +
                            (y * height / outHeight) * mapped.RowPitch;
          auto *destination = frame->pixels + y * outWidth * 4;
          if (outWidth == width) {
            std::memcpy(destination, row, outWidth * 4);
          } else {
            for (unsigned x = 0; x < outWidth; ++x)
              std::memcpy(destination + x * 4, row + (x * width / outWidth) * 4, 4);
          }
        }
        frame->width = outWidth;
        frame->height = outHeight;
        frame->publishedAt = now;
        ++frame->serial;
        ReleaseMutex(mutex);
      }
      context->Unmap(staging.Get(), 0);
      pending = false;
    }
    if (copyTime < nextCopyAt) return;
    // Keep the fractional deadline so source-frame scheduling jitter does not
    // accumulate. Late frames are skipped; they are never queued for catch-up.
    constexpr auto interval = std::chrono::nanoseconds(1'000'000'000 / operatorMonitorFramesPerSecond);
    if (nextCopyAt == std::chrono::steady_clock::time_point{}) nextCopyAt = copyTime;
    nextCopyAt += interval;
    if (nextCopyAt <= copyTime) nextCopyAt = copyTime + interval;
    // No flush, GPU wait, receiver acknowledgement, or unbounded queue.
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0,
                                  source.Get(), sourceSubresource, nullptr);
    pending = true;
  }
  HANDLE mapping = nullptr;
  HANDLE mutex = nullptr;
  OperatorMonitorFrame *frame = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  unsigned width = 0, height = 0;
  std::chrono::steady_clock::time_point nextCopyAt{};
  bool pending = false, failed = false;
};
OperatorMonitorOutput::OperatorMonitorOutput() : impl_(std::make_unique<Impl>()) {}
OperatorMonitorOutput::~OperatorMonitorOutput() = default;
void OperatorMonitorOutput::publish(ID3D11RenderTargetView *target) {
  impl_->publish(target);
}
}
