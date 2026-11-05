#include "operator_monitor_output.h"
#include "operator_monitor_transport.h"
#include "operator_monitor_renderer.h"
#include "network_video.h"
#include "decklink.h"
#include "virtual_camera/output.h"
#include "media_timecode.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QUuid>
#include <QDebug>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace vtrpon {
namespace {
std::atomic_bool enabled{false};
bool externalReturnEnabled() {
  return (networkVideoOutputMask() & 16u) || (deckLinkOutputMask() & 8u)
      || (virtualCameraEnabled() && virtualCameraReturnMonitor());
}
struct ReturnFrame {
  QImage image;
  quint64 ndiGeneration = 0, deckLinkGeneration = 0;
  qint64 timecode = 0;
  std::chrono::steady_clock::time_point captured;
};
struct ReturnMailbox {
  std::mutex mutex;
  std::condition_variable wake;
  ReturnFrame frame;
  QJsonObject presentation;
};
ReturnMailbox &returnMailbox() { static ReturnMailbox value; return value; }
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
    const auto font = QGuiApplication::font();
    worker = std::jthread([font](std::stop_token stop) {
      try {
      VirtualCameraOutput camera;
      auto &mailbox = returnMailbox();
      while (!stop.stop_requested()) {
        ReturnFrame frame;
        QJsonObject presentation;
        {
          std::unique_lock lock(mailbox.mutex);
          mailbox.wake.wait(lock, [&] {
            return stop.stop_requested() || !mailbox.frame.image.isNull();
          });
          if (stop.stop_requested()) break;
          frame = std::move(mailbox.frame); mailbox.frame = {};
          presentation = mailbox.presentation;
        }
        if (frame.image.isNull() || !externalReturnEnabled()
            || std::chrono::steady_clock::now() - frame.captured > std::chrono::milliseconds(200)) continue;
        // Fonts, layout, scaling and SDK delivery never run on the ON AIR render thread.
        QImage composed(operatorMonitorMaxWidth, operatorMonitorMaxHeight, QImage::Format_RGB32);
        {
          QPainter painter(&composed); painter.setFont(font);
          paintOperatorMonitor(painter, composed.size(), frame.image, presentation);
        }
        const QByteArray pixels(reinterpret_cast<const char *>(composed.constBits()), composed.sizeInBytes());
        publishNetworkReturnMonitor(pixels, composed.width(), composed.height(), frame.ndiGeneration, frame.timecode);
        publishDeckLinkReturnMonitor(pixels, composed.width(), composed.height(), frame.deckLinkGeneration, frame.timecode);
        camera.publishReturnMonitor(composed);
      }
      } catch (...) {
        const auto message = QStringLiteral("Return monitor output worker failed.");
        reportNetworkVideoOutputError(message); reportDeckLinkOutputError(message);
        qWarning().noquote() << message;
      }
    });
  }
  ~Impl() {
    worker.request_stop(); returnMailbox().wake.notify_all();
    if (worker.joinable()) worker.join();
    if (state) UnmapViewOfFile(state);
    if (mapping) CloseHandle(mapping);
    if (mutex) CloseHandle(mutex);
  }
  HANDLE mapping = nullptr, mutex = nullptr;
  OperatorMonitorPresentation *state = nullptr;
  std::jthread worker;
};
OperatorMonitorControlFeed::OperatorMonitorControlFeed() : impl_(std::make_unique<Impl>()) {}
OperatorMonitorControlFeed::~OperatorMonitorControlFeed() = default;
void OperatorMonitorControlFeed::publish(const QByteArray &json) {
  {
    auto &mailbox = returnMailbox();
    std::unique_lock lock(mailbox.mutex, std::try_to_lock);
    if (lock) mailbox.presentation = QJsonDocument::fromJson(json).object();
  }
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
    if (!enabled.load(std::memory_order_relaxed) && !externalReturnEnabled()) {
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
      {
        const double scale = (std::min)({1.0, double(operatorMonitorMaxWidth) / width,
                                             double(operatorMonitorMaxHeight) / height});
        const unsigned outWidth = (std::max)(1U, unsigned(width * scale));
        const unsigned outHeight = (std::max)(1U, unsigned(height * scale));
        QImage image(outWidth, outHeight, QImage::Format_RGB32);
        for (unsigned y = 0; y < outHeight; ++y) {
          const auto *row = static_cast<const unsigned char *>(mapped.pData) +
                            (y * height / outHeight) * mapped.RowPitch;
          auto *destination = image.scanLine(y);
          if (outWidth == width) {
            std::memcpy(destination, row, outWidth * 4);
          } else {
            for (unsigned x = 0; x < outWidth; ++x)
              std::memcpy(destination + x * 4, row + (x * width / outWidth) * 4, 4);
          }
        }
        // The disposable window receiver and external outputs have independent
        // mailboxes. A busy/stopped window cannot block NDI, DeckLink or camera.
        const DWORD acquired = mutex && frame && enabled.load()
            ? WaitForSingleObject(mutex, 0) : WAIT_TIMEOUT;
        if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) {
          std::memcpy(frame->pixels, image.constBits(), image.sizeInBytes());
          frame->width = outWidth; frame->height = outHeight;
          frame->publishedAt = now; ++frame->serial;
          ReleaseMutex(mutex);
        }
        if (externalReturnEnabled()) {
          auto &mailbox = returnMailbox();
          std::unique_lock lock(mailbox.mutex, std::try_to_lock);
          if (lock) {
            mailbox.frame = {std::move(image), ndiGeneration, deckLinkGeneration, timecode, captured};
            mailbox.wake.notify_one();
          }
        }
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
    ndiGeneration = networkVideoOutputGeneration();
    deckLinkGeneration = deckLinkOutputGeneration();
    timecode = mediaTimecode100ns(); captured = copyTime;
  }
  HANDLE mapping = nullptr;
  HANDLE mutex = nullptr;
  OperatorMonitorFrame *frame = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  unsigned width = 0, height = 0;
  std::chrono::steady_clock::time_point nextCopyAt{};
  bool pending = false, failed = false;
  quint64 ndiGeneration = 0, deckLinkGeneration = 0;
  qint64 timecode = 0;
  std::chrono::steady_clock::time_point captured;
};
OperatorMonitorOutput::OperatorMonitorOutput() : impl_(std::make_unique<Impl>()) {}
OperatorMonitorOutput::~OperatorMonitorOutput() = default;
void OperatorMonitorOutput::publish(ID3D11RenderTargetView *target) {
  impl_->publish(target);
}
}
