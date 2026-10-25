#include "system_load_widget.h"
#include "controller_scaling.h"
#include "language_text.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QThread>
#include <QHash>
#include <QElapsedTimer>
#include <QStyle>
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>
#endif

namespace {
using Values = std::array<double, 4>;
struct LoadSnapshot {
  Values system{-1, -1, -1, -1};
  Values process{-1, -1, -1, -1};
};

void setLoadWarning(QLabel *label, bool warning) {
  if (label->property("loadWarning").toBool() == warning) return;
  label->setProperty("loadWarning", warning);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->update();
}

void renderLoadSnapshot(const std::array<QLabel *, 8> &labels,
                        const std::array<QString, 4> &names,
                        QLabel *systemTitle, const LoadSnapshot &snapshot) {
  bool anyWarning = false;
  for (size_t i = 0; i < labels.size(); ++i) {
    const double value = i < 4 ? snapshot.system[i] : snapshot.process[i - 4];
    const bool available = value >= 0 && std::isfinite(value);
    const bool warning = i < 4 && available &&
        (value >= 90.0 || (labels[i]->property("loadWarning").toBool() && value >= 85.0));
    setLoadWarning(labels[i], warning);
    anyWarning = anyWarning || warning;
    const QString percentage = available
        ? QString::number(std::clamp(value, 0.0, 100.0), 'f', 1) : QStringLiteral("--");
    labels[i]->setText(names[i % 4] + QLatin1Char(' ') + percentage + QLatin1Char('%'));
  }
  const QString normal = vtrpon::uiText("system_load.pc");
  const QString warning = vtrpon::uiText("system_load.pc_warning");
  // Reserve the warning width so entering/leaving a warning never moves the metrics.
  systemTitle->setMinimumWidth(std::max(systemTitle->fontMetrics().horizontalAdvance(normal),
                                      systemTitle->fontMetrics().horizontalAdvance(warning)));
  systemTitle->setText(anyWarning ? warning : normal);
  setLoadWarning(systemTitle, anyWarning);
}

#ifdef _WIN32
class Sampler {
public:
  Sampler() {
    sampleProcessCpu();
    if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
      PdhAddEnglishCounterW(query_, L"\\Processor(_Total)\\% Processor Time", 0, &cpu_);
      PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_);
      PdhAddEnglishCounterW(query_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory_);
      PdhCollectQueryData(query_);
    }
  }
  ~Sampler() { if (query_) PdhCloseQuery(query_); }

  LoadSnapshot sample() {
    LoadSnapshot snapshot;
    auto &values = snapshot.system;
    auto &process = snapshot.process;
    process[0] = sampleProcessCpu();
    MEMORYSTATUSEX ram{};
    ram.dwLength = sizeof(ram);
    if (GlobalMemoryStatusEx(&ram) && ram.ullTotalPhys) {
      values[2] = 100.0 * (ram.ullTotalPhys - ram.ullAvailPhys) / ram.ullTotalPhys;
      PROCESS_MEMORY_COUNTERS counters{};
      if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        process[2] = 100.0 * counters.WorkingSetSize / ram.ullTotalPhys;
    }
    const bool collected = query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS;
    PDH_FMT_COUNTERVALUE cpu{};
    if (collected && cpu_ && PdhGetFormattedCounterValue(cpu_, PDH_FMT_DOUBLE, nullptr, &cpu) == ERROR_SUCCESS
        && valid(cpu.CStatus)) values[0] = cpu.doubleValue;

    // Sum processes sharing the same physical engine, then take the busiest engine.
    QHash<QString, double> engines;
    QHash<QString, double> processEngines;
    const QString processPrefix = QStringLiteral("pid_%1_").arg(GetCurrentProcessId());
    const bool gpuAvailable = collected && read(gpu_, [&](const QString &name, double value) {
      const auto luid = name.indexOf(QStringLiteral("luid_"));
      if (luid >= 0) {
        engines[name.mid(luid)] += value;
        if (name.startsWith(processPrefix)) processEngines[name.mid(luid)] += value;
      }
    });
    for (double value : engines) values[1] = std::max(values[1], value);
    if (gpuAvailable) {
      process[1] = 0;
      for (double value : processEngines) process[1] = std::max(process[1], value);
    }

    // Match dedicated usage to DXGI adapters; shared system RAM is not VRAM.
    QHash<QString, double> usage;
    if (collected)
      read(memory_, [&usage](const QString &name, double value) { usage[name.toLower()] = value; });
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    double total = 0, used = 0, processUsed = 0;
    bool complete = true;
    bool processComplete = true;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(i, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) { complete = processComplete = false; break; }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) { complete = processComplete = false; break; }
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || !desc.DedicatedVideoMemory) continue;
        const QString key = QStringLiteral("luid_0x%1_0x%2_phys_0")
            .arg(static_cast<quint32>(desc.AdapterLuid.HighPart), 8, 16, QLatin1Char('0'))
            .arg(desc.AdapterLuid.LowPart, 8, 16, QLatin1Char('0'));
        total += desc.DedicatedVideoMemory;
        if (!usage.contains(key)) complete = false;
        else used += usage.value(key);
        // DXGI reports the calling process, not all processes on the adapter.
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo{};
        if (SUCCEEDED(adapter.As(&adapter3)) &&
            SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo)))
          processUsed += memoryInfo.CurrentUsage;
        else processComplete = false;
      }
      if (complete && total > 0) values[3] = 100.0 * used / total;
      if (processComplete && total > 0) process[3] = 100.0 * processUsed / total;
    }
    return snapshot;
  }

private:
  double sampleProcessCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
      cpuClock_.invalidate();
      return -1;
    }
    const auto ticks = [](const FILETIME &time) {
      return (static_cast<quint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const quint64 current = ticks(kernel) + ticks(user);
    double result = -1;
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpuClock_.isValid()) {
      const qint64 elapsed = cpuClock_.nsecsElapsed();
      if (elapsed > 0 && processors && current >= previousCpuTicks_)
        result = 100.0 * (current - previousCpuTicks_) * 100.0 / elapsed / processors;
    }
    cpuClock_.start();
    previousCpuTicks_ = current;
    return result;
  }
  static bool valid(DWORD status) {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
  }
  template<typename Callback> bool read(PDH_HCOUNTER counter, Callback callback) {
    if (!counter) return false;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA)
      return false;
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer(
        (bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1) / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, buffer.data()) != ERROR_SUCCESS)
      return false;
    bool available = false;
    for (DWORD i = 0; i < count; ++i) {
      const auto &item = buffer[i];
      if (valid(item.FmtValue.CStatus) && std::isfinite(item.FmtValue.doubleValue)) {
        available = true;
        callback(QString::fromWCharArray(item.szName), std::max(0.0, item.FmtValue.doubleValue));
      }
    }
    return available;
  }
  QElapsedTimer cpuClock_;
  quint64 previousCpuTicks_ = 0;
  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER cpu_ = nullptr, gpu_ = nullptr, memory_ = nullptr;
};
#endif
} // namespace

SystemLoadWidget::SystemLoadWidget(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("systemLoadWidget"));
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(vtrpon::scalePx(8), 0, vtrpon::scalePx(12), 0);
  layout->setSpacing(vtrpon::scalePx(10));
  const std::array<QString, 4> names{QStringLiteral("CPU"), QStringLiteral("GPU"),
                                   QStringLiteral("RAM"), QStringLiteral("VRAM")};
  auto *systemTitle = new QLabel(vtrpon::uiText("system_load.pc"), this);
  systemTitle->setObjectName(QStringLiteral("systemLoadTitle"));
  layout->addWidget(systemTitle);
  std::array<QLabel *, 8> labels{};
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i == 4) {
      layout->addSpacing(vtrpon::scalePx(6));
      layout->addWidget(new QLabel(QStringLiteral("|"), this));
      layout->addSpacing(vtrpon::scalePx(6));
      auto *processTitle = new QLabel(QStringLiteral("VTR-PON"), this);
      layout->addWidget(processTitle);
    }
    const QString name = names[i % 4];
    labels[i] = new QLabel(name + QStringLiteral(" --%"), this);
    labels[i]->setObjectName((i < 4 ? QStringLiteral("systemLoad") : QStringLiteral("processLoad")) + name);
    labels[i]->setMinimumWidth(labels[i]->fontMetrics().horizontalAdvance(name + QStringLiteral(" 100.0%")));
    layout->addWidget(labels[i]);
  }
  layout->addSpacing(vtrpon::scalePx(6));
  layout->addWidget(new QLabel(QStringLiteral("|"), this));
  layout->addSpacing(vtrpon::scalePx(6));
  fullscreenOutput_ = new QLabel(this);
  fullscreenOutput_->setObjectName(QStringLiteral("fullscreenOutputStatus"));
  layout->addWidget(fullscreenOutput_);
  setFullscreenOutput(0, {});
  renderLoadSnapshot(labels, names, systemTitle, LoadSnapshot{});
  setToolTip(vtrpon::uiText("system_load.description"));
  sampler_ = QThread::create([this, labels, names, systemTitle] {
#ifdef _WIN32
    Sampler sampler;
#endif
    while (!QThread::currentThread()->isInterruptionRequested()) {
      for (int i = 0; i < 10 && !QThread::currentThread()->isInterruptionRequested(); ++i)
        QThread::msleep(100);
      if (QThread::currentThread()->isInterruptionRequested()) break;
      LoadSnapshot snapshot;
#ifdef _WIN32
      snapshot = sampler.sample();
#endif
      QMetaObject::invokeMethod(this, [this, labels, names, snapshot, systemTitle] {
        setToolTip(vtrpon::uiText("system_load.description"));
        renderLoadSnapshot(labels, names, systemTitle, snapshot);
      }, Qt::QueuedConnection);
    }
  });
  sampler_->start();
}

void SystemLoadWidget::setFullscreenOutput(quint64 presentedFrames,
                                           const QSize &resolution) {
  double fps = -1;
  if (!resolution.isEmpty()) {
    if (fullscreenClock_.isValid() && resolution == fullscreenResolution_
        && presentedFrames >= previousPresentedFrames_) {
      const qint64 elapsed = fullscreenClock_.nsecsElapsed();
      if (elapsed > 0)
        fps = static_cast<double>(presentedFrames - previousPresentedFrames_)
            * 1.0e9 / elapsed;
    }
    fullscreenClock_.start();
  } else {
    fullscreenClock_.invalidate();
  }
  previousPresentedFrames_ = presentedFrames;
  fullscreenResolution_ = resolution;

  const QString format = vtrpon::uiText("system_load.fullscreen");
  const QString unavailable = QStringLiteral("--");
  const QString fpsText = fps >= 0 ? locale().toString(fps, 'f', 1) : unavailable;
  const QString resolutionText = resolution.isEmpty() ? unavailable
      : QStringLiteral("%1\u00d7%2").arg(resolution.width()).arg(resolution.height());
  fullscreenOutput_->setText(format.arg(fpsText, resolutionText));
  // Reserve stable metric width across FPS updates, skins and UI languages.
  fullscreenOutput_->setMinimumWidth(fullscreenOutput_->fontMetrics().horizontalAdvance(
      format.arg(locale().toString(999.9, 'f', 1), QStringLiteral("9999\u00d79999"))));
  fullscreenOutput_->setToolTip(vtrpon::uiText("system_load.fullscreen_description"));
}

SystemLoadWidget::~SystemLoadWidget() {
  sampler_->requestInterruption();
  sampler_->wait();
  delete sampler_;
}
