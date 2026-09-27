#include "powerpoint_source_session_manager.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <Windows.h>
#include <OleAuto.h>
#include <TlHelp32.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr long kMsoFalse = 0;
constexpr long kMsoTrue = -1;
constexpr long kMsoGroup = 6;
constexpr long kMsoWebVideo = 26;
constexpr long kPpShowAll = 1;
constexpr long kPpShowTypeWindow = 2;
constexpr long kPpSlideShowRunning = 1;
constexpr long kPpSlideShowPaused = 2;
constexpr long kMsoClickStateAfterAllAnimations = -2;
constexpr int kMaximumCaptureWidth = 1920;
constexpr int kMaximumCaptureHeight = 1080;
constexpr int kCaptureFramesPerSecond = 30;
constexpr float kPointsPerPixelAt96Dpi = 0.75F;
constexpr float kOffscreenPositionPoints = -32000.0F;
constexpr UINT kPwRenderFullContent = 0x00000002;
constexpr GstClockTime kShutdownStateTimeout = 2 * GST_SECOND;
constexpr DWORD kCaptureShutdownTimeoutMs = 2000;
std::recursive_mutex gPowerPointApplicationMutex;
std::mutex gPowerPointInteropMutex;
thread_local bool gPowerPointInteropInProgress = false;

class PowerPointInteropOperation final {
public:
  PowerPointInteropOperation() : lock_(gPowerPointInteropMutex) {
    gPowerPointInteropInProgress = true;
  }
  ~PowerPointInteropOperation() { gPowerPointInteropInProgress = false; }

private:
  std::scoped_lock<std::mutex> lock_;
};

std::atomic_int gPowerPointNativeSourceCount{0};
// Logical deck ownership is separate from worker-owned teardown. Conversion
// may start preparing after detachment, but must wait for physical teardown.
std::atomic_int gPowerPointAttachedSourceCount{0};
int gPowerPointAutomationCount = 0;
DWORD gPowerPointStartedProcessId = 0;

QString nativeSourceActiveError() {
  return QStringLiteral(
      "PowerPoint conversion is unavailable while a native PowerPoint "
      "source is being prepared or is ON AIR. The current ON AIR source has "
      "priority.");
}

bool cancelWasRequested(const std::atomic_bool *cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_acquire);
}

QString powerPointFileValidationError(const QString &filePath) {
  const QFileInfo fileInfo(filePath);
  if (!fileInfo.exists() || !fileInfo.isFile()) {
    return QStringLiteral("The PowerPoint file was not found.");
  }
  if (fileInfo.suffix().compare(QStringLiteral("pptx"),
                                Qt::CaseInsensitive) != 0) {
    return QStringLiteral("Only .pptx PowerPoint files are supported.");
  }

  QFile file(fileInfo.absoluteFilePath());
  if (!file.open(QIODevice::ReadOnly)) {
    return QStringLiteral("The PowerPoint file could not be read. Access may be denied: %1")
        .arg(file.errorString());
  }
  const QByteArray signature = file.read(8);
  static const QByteArray encryptedOfficeSignature =
      QByteArray::fromHex("d0cf11e0a1b11ae1");
  if (signature == encryptedOfficeSignature) {
    return QStringLiteral(
        "Password-protected or encrypted PowerPoint files are not supported.");
  }
  if (signature.size() < 4 || signature.at(0) != 'P' ||
      signature.at(1) != 'K') {
    return QStringLiteral(
        "The file is not a valid Office Open XML PowerPoint document or is damaged.");
  }
  return {};
}

QString systemMessage(HRESULT result) {
  wchar_t *buffer = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result),
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
  QString message;
  if (size != 0 && buffer) {
    message = QString::fromWCharArray(buffer, static_cast<qsizetype>(size))
                  .trimmed();
  }
  if (buffer) {
    LocalFree(buffer);
  }
  return message;
}

class ComFailure final : public std::runtime_error {
public:
  ComFailure(HRESULT result, const QString &context)
      : std::runtime_error(context.toStdString()), result_(result),
        detail_(QStringLiteral("%1 (HRESULT=0x%2)%3")
                    .arg(context)
                    .arg(static_cast<quint32>(result), 8, 16,
                         QLatin1Char('0'))
                    .arg(systemMessage(result).isEmpty()
                             ? QString()
                             : QStringLiteral(": %1").arg(
                                   systemMessage(result)))) {}

  HRESULT result() const noexcept { return result_; }
  const QString &detail() const noexcept { return detail_; }

private:
  HRESULT result_ = E_FAIL;
  QString detail_;
};

void throwIfFailed(HRESULT result, const QString &context) {
  if (FAILED(result)) {
    throw ComFailure(result, context);
  }
}

bool isPowerPointBusy(const ComFailure &failure) {
  return failure.result() == RPC_E_CALL_REJECTED ||
         failure.result() == RPC_E_SERVERCALL_RETRYLATER;
}

bool isPowerPointDisconnected(HRESULT result) {
  return result == RPC_E_DISCONNECTED || result == RPC_E_SERVER_DIED ||
         result == RPC_E_SERVER_DIED_DNE ||
         result == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
         result == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) ||
         result == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE) ||
         result == CO_E_OBJNOTCONNECTED;
}

class ComApartment final {
public:
  ComApartment() {
    const HRESULT result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (result == RPC_E_CHANGED_MODE) {
      throw ComFailure(
          result,
          QStringLiteral("PowerPoint requires a single-threaded COM apartment"));
    }
    throwIfFailed(result, QStringLiteral("CoInitializeEx"));
    initialized_ = true;
  }

  ~ComApartment() {
    if (initialized_) {
      CoUninitialize();
    }
  }

private:
  bool initialized_ = false;
};

class Variant final {
public:
  Variant() { VariantInit(&value_); }

  explicit Variant(long value) : Variant() {
    value_.vt = VT_I4;
    value_.lVal = value;
  }

  explicit Variant(float value) : Variant() {
    value_.vt = VT_R4;
    value_.fltVal = value;
  }

  explicit Variant(const std::wstring &value) : Variant() {
    value_.vt = VT_BSTR;
    value_.bstrVal =
        SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    if (!value_.bstrVal && !value.empty()) {
      throw std::bad_alloc();
    }
  }

  Variant(const Variant &other) : Variant() {
    throwIfFailed(VariantCopy(&value_, const_cast<VARIANT *>(&other.value_)),
                  QStringLiteral("VariantCopy"));
  }

  Variant(Variant &&other) noexcept : value_(other.value_) {
    VariantInit(&other.value_);
  }

  ~Variant() { VariantClear(&value_); }

  Variant &operator=(const Variant &other) {
    if (this == &other) {
      return *this;
    }
    VARIANT copy;
    VariantInit(&copy);
    throwIfFailed(VariantCopy(&copy, const_cast<VARIANT *>(&other.value_)),
                  QStringLiteral("VariantCopy"));
    VariantClear(&value_);
    value_ = copy;
    return *this;
  }

  Variant &operator=(Variant &&other) noexcept {
    if (this != &other) {
      VariantClear(&value_);
      value_ = other.value_;
      VariantInit(&other.value_);
    }
    return *this;
  }

  const VARIANT &raw() const noexcept { return value_; }

  long asLong() const {
    switch (value_.vt) {
    case VT_I4:
    case VT_INT:
      return value_.lVal;
    case VT_UI4:
    case VT_UINT:
      return static_cast<long>(value_.ulVal);
    default:
      throw std::runtime_error("COM result is not a 32-bit integer");
    }
  }

  bool asBool() const {
    switch (value_.vt) {
    case VT_BOOL:
      return value_.boolVal != VARIANT_FALSE;
    case VT_I4:
    case VT_INT:
      return value_.lVal != 0;
    case VT_UI4:
    case VT_UINT:
      return value_.ulVal != 0;
    default:
      throw std::runtime_error("COM result is not a Boolean");
    }
  }

  std::int64_t asInt64() const {
    switch (value_.vt) {
    case VT_I4:
    case VT_INT:
      return value_.lVal;
    case VT_UI4:
    case VT_UINT:
      return value_.ulVal;
    case VT_I8:
      return value_.llVal;
    case VT_UI8:
      return static_cast<std::int64_t>(value_.ullVal);
    default:
      throw std::runtime_error("COM result is not an integer handle");
    }
  }

  float asFloat() const {
    switch (value_.vt) {
    case VT_R4:
      return value_.fltVal;
    case VT_R8:
      return static_cast<float>(value_.dblVal);
    case VT_I4:
      return static_cast<float>(value_.lVal);
    default:
      throw std::runtime_error("COM result is not a floating-point value");
    }
  }

  ComPtr<IDispatch> asDispatch() const {
    ComPtr<IDispatch> result;
    if (value_.vt == VT_DISPATCH && value_.pdispVal) {
      result = value_.pdispVal;
      return result;
    }
    if (value_.vt == VT_UNKNOWN && value_.punkVal) {
      throwIfFailed(value_.punkVal->QueryInterface(IID_PPV_ARGS(&result)),
                    QStringLiteral("IUnknown::QueryInterface(IDispatch)"));
      return result;
    }
    throw std::runtime_error("COM result is not an automation object");
  }

private:
  explicit Variant(VARIANT &&value) noexcept : value_(value) {
    VariantInit(&value);
  }

  VARIANT value_{};
  friend class DispatchObject;
};

class DispatchObject final {
public:
  DispatchObject() = default;
  explicit DispatchObject(ComPtr<IDispatch> dispatch)
      : dispatch_(std::move(dispatch)) {}

  bool valid() const noexcept { return dispatch_ != nullptr; }

  Variant property(const wchar_t *name) const {
    return invoke(name, DISPATCH_PROPERTYGET, {}, nullptr, 0);
  }

  DispatchObject objectProperty(const wchar_t *name) const {
    return DispatchObject(property(name).asDispatch());
  }

  void setProperty(const wchar_t *name, const Variant &value) const {
    DISPID propertyPut = DISPID_PROPERTYPUT;
    (void)invoke(name, DISPATCH_PROPERTYPUT, {value}, &propertyPut, 1);
  }

  Variant call(const wchar_t *name,
               const std::vector<Variant> &arguments = {}) const {
    return invoke(name, DISPATCH_METHOD, arguments, nullptr, 0);
  }

private:
  DISPID memberId(const wchar_t *name) const {
    if (!dispatch_) {
      throw std::runtime_error("COM object is null");
    }
    LPOLESTR mutableName = const_cast<LPOLESTR>(name);
    DISPID id = DISPID_UNKNOWN;
    throwIfFailed(dispatch_->GetIDsOfNames(
                      IID_NULL, &mutableName, 1, LOCALE_USER_DEFAULT, &id),
                  QStringLiteral("IDispatch::GetIDsOfNames(%1)")
                      .arg(QString::fromWCharArray(name)));
    return id;
  }

  Variant invoke(const wchar_t *name, WORD flags,
                 const std::vector<Variant> &argumentsInNaturalOrder,
                 DISPID *namedArguments, UINT namedArgumentCount) const {
    const DISPID id = memberId(name);
    std::vector<VARIANTARG> arguments(argumentsInNaturalOrder.size());
    for (VARIANTARG &argument : arguments) {
      VariantInit(&argument);
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const Variant &source = argumentsInNaturalOrder[
          argumentsInNaturalOrder.size() - 1 - index];
      const HRESULT copyResult = VariantCopy(
          &arguments[index], const_cast<VARIANT *>(&source.raw()));
      if (FAILED(copyResult)) {
        for (VARIANTARG &argument : arguments) {
          VariantClear(&argument);
        }
        throw ComFailure(copyResult,
                         QStringLiteral("VariantCopy for COM invocation"));
      }
    }

    DISPPARAMS parameters{};
    parameters.rgvarg = arguments.empty() ? nullptr : arguments.data();
    parameters.cArgs = static_cast<UINT>(arguments.size());
    parameters.rgdispidNamedArgs = namedArguments;
    parameters.cNamedArgs = namedArgumentCount;

    VARIANT rawResult;
    VariantInit(&rawResult);
    EXCEPINFO exceptionInfo{};
    UINT argumentError = 0;
    const HRESULT result = dispatch_->Invoke(
        id, IID_NULL, LOCALE_USER_DEFAULT, flags, &parameters, &rawResult,
        &exceptionInfo, &argumentError);
    for (VARIANTARG &argument : arguments) {
      VariantClear(&argument);
    }

    QString context = QStringLiteral("IDispatch::Invoke(%1)")
                          .arg(QString::fromWCharArray(name));
    if (exceptionInfo.bstrDescription) {
      context += QStringLiteral(": %1").arg(
          QString::fromWCharArray(exceptionInfo.bstrDescription));
    } else if (exceptionInfo.bstrSource) {
      context += QStringLiteral(": %1").arg(
          QString::fromWCharArray(exceptionInfo.bstrSource));
    }
    SysFreeString(exceptionInfo.bstrSource);
    SysFreeString(exceptionInfo.bstrDescription);
    SysFreeString(exceptionInfo.bstrHelpFile);
    if (FAILED(result)) {
      VariantClear(&rawResult);
      throw ComFailure(result, context);
    }
    return Variant(std::move(rawResult));
  }

  ComPtr<IDispatch> dispatch_;
};

struct WindowSearchContext {
  DWORD processId = 0;
  std::vector<HWND> windows;
};

BOOL CALLBACK collectProcessWindows(HWND window, LPARAM parameter) {
  auto *context = reinterpret_cast<WindowSearchContext *>(parameter);
  DWORD processId = 0;
  GetWindowThreadProcessId(window, &processId);
  if (context->processId == 0 || processId == context->processId) {
    context->windows.push_back(window);
  }
  return TRUE;
}

std::set<HWND> windowsForProcess(DWORD processId) {
  WindowSearchContext context{processId, {}};
  EnumWindows(collectProcessWindows, reinterpret_cast<LPARAM>(&context));
  return std::set<HWND>(context.windows.begin(), context.windows.end());
}

std::set<DWORD> powerPointProcessIds() {
  std::set<DWORD> processIds;
  const HANDLE snapshot =
      CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return processIds;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"POWERPNT.EXE") == 0) {
        processIds.insert(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return processIds;
}

DWORD powerPointApplicationProcessId(
    const DispatchObject &application,
    const std::set<DWORD> &processIdsBefore,
    bool applicationWasCreated) {
  try {
    const auto rawHandle = static_cast<std::uintptr_t>(
        static_cast<std::uint32_t>(application.property(L"HWND").asInt64()));
    DWORD processId = 0;
    GetWindowThreadProcessId(reinterpret_cast<HWND>(rawHandle), &processId);
    if (processId != 0) {
      return processId;
    }
  } catch (...) {
  }

  for (int attempt = 0; attempt < 20; ++attempt) {
    const std::set<DWORD> currentProcessIds = powerPointProcessIds();
    if (gPowerPointStartedProcessId != 0 &&
        currentProcessIds.contains(gPowerPointStartedProcessId)) {
      return gPowerPointStartedProcessId;
    }
    if (applicationWasCreated || processIdsBefore.empty()) {
      std::vector<DWORD> newProcessIds;
      std::set_difference(currentProcessIds.begin(), currentProcessIds.end(),
                          processIdsBefore.begin(), processIdsBefore.end(),
                          std::back_inserter(newProcessIds));
      if (newProcessIds.size() == 1) {
        return newProcessIds.front();
      }
    } else if (processIdsBefore.size() == 1) {
      return *processIdsBefore.begin();
    }
    if (attempt + 1 < 20) {
      std::this_thread::sleep_for(50ms);
    }
  }
  return 0;
}

QString windowClass(HWND window) {
  wchar_t buffer[256]{};
  const int length =
      GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
  return length > 0 ? QString::fromWCharArray(buffer, length) : QString();
}

bool looksLikeSlideShowWindow(HWND window) {
  const QString className = windowClass(window).toLower();
  return className.contains(QStringLiteral("screenclass")) ||
         className.contains(QStringLiteral("slideshow")) ||
         className.contains(QStringLiteral("pptframeclass"));
}

void pumpPendingMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

bool waitForPowerPointProcessExit(DWORD processId,
                                  std::chrono::milliseconds timeout) {
  if (processId == 0) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (powerPointProcessIds().contains(processId)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    pumpPendingMessages();
    std::this_thread::sleep_for(50ms);
  }
  return true;
}

template <typename Operation>
auto invokePowerPointWithRetry(Operation &&operation)
    -> decltype(operation()) {
  constexpr int maximumAttempts = 30;
  for (int attempt = 0;; ++attempt) {
    try {
      return operation();
    } catch (const ComFailure &failure) {
      if (!isPowerPointBusy(failure) ||
          attempt + 1 >= maximumAttempts) {
        throw;
      }
      pumpPendingMessages();
      std::this_thread::sleep_for(100ms);
    }
  }
}

void trySetProperty(const DispatchObject &object, const wchar_t *name,
                    const Variant &value) noexcept {
  try {
    object.setProperty(name, value);
  } catch (...) {
  }
}

void validatePowerPointShape(const DispatchObject &shape) {
  const long shapeType = shape.property(L"Type").asLong();
  if (shapeType == kMsoGroup) {
    DispatchObject groupItems = shape.objectProperty(L"GroupItems");
    const long itemCount = groupItems.property(L"Count").asLong();
    for (long index = 1; index <= itemCount; ++index) {
      DispatchObject child(
          groupItems.call(L"Item", {Variant(index)}).asDispatch());
      validatePowerPointShape(child);
    }
  } else if (shapeType == kMsoWebVideo) {
    throw std::runtime_error(
        "Online video is unavailable in native PowerPoint mode");
  }
}

void validatePowerPointPresentation(const DispatchObject &presentation) {
  DispatchObject slides = presentation.objectProperty(L"Slides");
  const long slideCount = slides.property(L"Count").asLong();
  for (long slideIndex = 1; slideIndex <= slideCount; ++slideIndex) {
    DispatchObject slide(
        slides.call(L"Item", {Variant(slideIndex)}).asDispatch());
    DispatchObject shapes = slide.objectProperty(L"Shapes");
    const long shapeCount = shapes.property(L"Count").asLong();
    for (long shapeIndex = 1; shapeIndex <= shapeCount; ++shapeIndex) {
      DispatchObject shape(
          shapes.call(L"Item", {Variant(shapeIndex)}).asDispatch());
      validatePowerPointShape(shape);
    }
  }
}

class PowerPointSlideShow final {
public:
  ~PowerPointSlideShow() { close(); }

  bool open(const DispatchObject &application, const QString &filePath,
            QString *errorDetail, HRESULT *failureCode = nullptr) {
    close();
    if (failureCode) *failureCode = S_OK;
    QElapsedTimer preparationTimer;
    preparationTimer.start();
    try {
      const QFileInfo fileInfo(filePath);
      const QString validationError =
          powerPointFileValidationError(filePath);
      if (!validationError.isEmpty()) {
        throw std::runtime_error(validationError.toUtf8().constData());
      }
      path_ = fileInfo.absoluteFilePath();
      DispatchObject presentations =
          application.objectProperty(L"Presentations");
      Variant opened = invokePowerPointWithRetry([&] {
        return presentations.call(
            L"Open",
            {Variant(QDir::toNativeSeparators(path_).toStdWString()),
             Variant(kMsoTrue), Variant(kMsoFalse), Variant(kMsoFalse)});
      });
      presentation_ = DispatchObject(opened.asDispatch());
      const qint64 documentOpenMs = preparationTimer.elapsed();

      DispatchObject slides = presentation_.objectProperty(L"Slides");
      slideCount_ = slides.property(L"Count").asLong();
      if (slideCount_ <= 0) {
        throw std::runtime_error("The presentation has no slides");
      }
      // VTR-PON captures video only. Preserve PowerPoint's audio settings so
      // playback can reach its Windows output device directly, including in CUE.
      invokePowerPointWithRetry(
          [&] { validatePowerPointPresentation(presentation_); });
      const qint64 validationMs = preparationTimer.elapsed();

      float slideWidth = 16.0F;
      float slideHeight = 9.0F;
      try {
        DispatchObject pageSetup =
            presentation_.objectProperty(L"PageSetup");
        slideWidth = pageSetup.property(L"SlideWidth").asFloat();
        slideHeight = pageSetup.property(L"SlideHeight").asFloat();
      } catch (...) {
      }
      const float aspect = std::clamp(slideWidth / slideHeight, 0.3F, 3.0F);
      SIZE requested{kMaximumCaptureWidth,
                     static_cast<LONG>(std::lround(
                         kMaximumCaptureWidth / aspect))};
      if (requested.cy > kMaximumCaptureHeight) {
        requested.cy = kMaximumCaptureHeight;
        requested.cx = static_cast<LONG>(
            std::lround(kMaximumCaptureHeight * aspect));
      }
      requestedSize_ = requested;

      DWORD processId = 0;
      try {
        const auto rawHandle = static_cast<std::uintptr_t>(
            static_cast<std::uint32_t>(
                application.property(L"HWND").asInt64()));
        GetWindowThreadProcessId(reinterpret_cast<HWND>(rawHandle),
                                 &processId);
      } catch (...) {
      }
      const std::set<HWND> windowsBefore = windowsForProcess(processId);
      DispatchObject settings =
          presentation_.objectProperty(L"SlideShowSettings");
      settings.setProperty(L"RangeType", Variant(kPpShowAll));
      settings.setProperty(L"ShowType", Variant(kPpShowTypeWindow));
      trySetProperty(settings, L"ShowPresenterView", Variant(kMsoFalse));
      trySetProperty(settings, L"ShowScrollbar", Variant(kMsoFalse));
      trySetProperty(settings, L"ShowMediaControls", Variant(kMsoFalse));

      Variant showResult = invokePowerPointWithRetry(
          [&] { return settings.call(L"Run"); });
      const qint64 slideShowRunMs = preparationTimer.elapsed();
      slideShowWindow_ = DispatchObject(showResult.asDispatch());
      view_ = slideShowWindow_.objectProperty(L"View");
      trySetProperty(view_, L"AcceleratorsEnabled", Variant(kMsoFalse));

      try {
        const auto rawHandle = static_cast<std::uintptr_t>(
            static_cast<std::uint32_t>(
                slideShowWindow_.property(L"HWND").asInt64()));
        const HWND candidate = reinterpret_cast<HWND>(rawHandle);
        if (IsWindow(candidate)) {
          window_ = candidate;
        }
      } catch (...) {
      }
      if (!window_) {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!window_ && std::chrono::steady_clock::now() < deadline) {
          const std::set<HWND> current = windowsForProcess(processId);
          for (HWND candidate : current) {
            if (!windowsBefore.contains(candidate) &&
                IsWindowVisible(candidate) &&
                looksLikeSlideShowWindow(candidate)) {
              window_ = candidate;
              break;
            }
          }
          if (!window_) {
            for (HWND candidate : current) {
              if (IsWindowVisible(candidate) &&
                  looksLikeSlideShowWindow(candidate)) {
                window_ = candidate;
                break;
              }
            }
          }
          if (!window_) {
            pumpPendingMessages();
            std::this_thread::sleep_for(50ms);
          }
        }
      }
      if (!window_) {
        throw std::runtime_error(
            "Could not find the PowerPoint slide-show window");
      }

      // PowerPoint can inherit a maximized/minimized state when Run creates
      // the slide show. Its COM geometry setters reject that state. Restore
      // only this capture window, off-screen and without activating it,
      // before setting Width/Height/Left/Top.
      if (IsZoomed(window_) || IsIconic(window_)) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(window_, &placement)) {
          throw std::runtime_error(
              "Could not read the PowerPoint slide-show window placement");
        }
        const UINT previousShowCommand = placement.showCmd;
        placement.flags = 0;
        placement.showCmd = SW_SHOWNOACTIVATE;
        const LONG offscreen = static_cast<LONG>(kOffscreenPositionPoints);
        placement.rcNormalPosition = {offscreen, offscreen,
                                      offscreen + requested.cx,
                                      offscreen + requested.cy};
        if (!SetWindowPlacement(window_, &placement) ||
            IsZoomed(window_) || IsIconic(window_)) {
          throw std::runtime_error(
              "Could not restore the PowerPoint slide-show window for capture");
        }
        qInfo() << "PowerPoint slide-show window restored before sizing:"
                << path_ << "previous show command" << previousShowCommand;
      }
      slideShowWindow_.setProperty(
          L"Width", Variant(requested.cx * kPointsPerPixelAt96Dpi));
      slideShowWindow_.setProperty(
          L"Height", Variant(requested.cy * kPointsPerPixelAt96Dpi));
      slideShowWindow_.setProperty(L"Left",
                                   Variant(kOffscreenPositionPoints));
      slideShowWindow_.setProperty(L"Top",
                                   Variant(kOffscreenPositionPoints));

      SetLastError(ERROR_SUCCESS);
      const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
      if (style == 0 && GetLastError() != ERROR_SUCCESS) {
        throw std::runtime_error(
            "Could not read the PowerPoint slide-show window style");
      }
      const LONG_PTR borderless =
          style & ~(static_cast<LONG_PTR>(WS_CAPTION) |
                    static_cast<LONG_PTR>(WS_THICKFRAME) |
                    static_cast<LONG_PTR>(WS_MINIMIZEBOX) |
                    static_cast<LONG_PTR>(WS_MAXIMIZEBOX) |
                    static_cast<LONG_PTR>(WS_SYSMENU));
      SetWindowLongPtrW(window_, GWL_STYLE, borderless);
      // A frame-only change at the same outer size leaves PowerPoint's MDI
      // client at its old size. Force a real resize, then settle to the exact
      // capture size below so stale frame pixels cannot enter the video.
      if (!SetWindowPos(window_, nullptr,
                        static_cast<int>(kOffscreenPositionPoints),
                        static_cast<int>(kOffscreenPositionPoints),
                        requested.cx + 1, requested.cy + 1,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
        throw std::runtime_error(
            "Could not move the PowerPoint slide show off-screen");
      }

      // Only this VTR-PON-owned slide-show window is a capture surface. Keep
      // it visible off-screen for rendering, but remove its taskbar/Alt-Tab
      // entry without changing the user's PowerPoint editor windows.
      const LONG_PTR extendedStyle = GetWindowLongPtrW(window_, GWL_EXSTYLE);
      ShowWindow(window_, SW_HIDE);
      SetWindowLongPtrW(window_, GWL_EXSTYLE,
                       (extendedStyle & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                           WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
      ShowWindow(window_, SW_SHOWNOACTIVATE);
      stopTaskbarFlashing();

      for (int attempt = 0; attempt < 5; ++attempt) {
        pumpPendingMessages();
        std::this_thread::sleep_for(50ms);
        RECT client{};
        if (!GetClientRect(window_, &client)) {
          break;
        }
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width == requested.cx && height == requested.cy) {
          break;
        }
        (void)SetWindowPos(window_, nullptr,
                           static_cast<int>(kOffscreenPositionPoints),
                           static_cast<int>(kOffscreenPositionPoints),
                           requested.cx, requested.cy,
                           SWP_NOZORDER | SWP_NOACTIVATE |
                               SWP_FRAMECHANGED);
      }

      RECT client{};
      if (!GetClientRect(window_, &client) ||
          client.right <= client.left || client.bottom <= client.top) {
        throw std::runtime_error(
            "Could not determine the PowerPoint capture size");
      }
      captureSize_ = {client.right - client.left,
                      client.bottom - client.top};
      const qint64 windowReadyMs = preparationTimer.elapsed();
      cachedState_.active = true;
      cachedState_.slideCount = slideCount_;
      cachedState_.busy = true;
      try {
        cachedState_ = queryNavigationState();
      } catch (const ComFailure &failure) {
        if (!isPowerPointBusy(failure)) {
          throw;
        }
      }
      qInfo() << "PowerPoint native opened:" << path_
              << "window class" << windowClass(window_)
              << "capture" << captureSize_.cx << captureSize_.cy
              << "slide" << cachedState_.currentSlide << "/" << slideCount_
              << "busy" << cachedState_.busy;
      qInfo() << "PowerPoint native stages ms:" << path_
              << "open" << documentOpenMs
              << "validate" << validationMs - documentOpenMs
              << "run" << slideShowRunMs - validationMs
              << "window" << windowReadyMs - slideShowRunMs
              << "navigation" << preparationTimer.elapsed() - windowReadyMs;
      return true;
    } catch (const ComFailure &failure) {
      if (failureCode) *failureCode = failure.result();
      if (errorDetail) {
        *errorDetail = failure.detail();
      }
    } catch (const std::exception &failure) {
      if (errorDetail) {
        *errorDetail = QString::fromUtf8(failure.what());
      }
    }
    close();
    return false;
  }

  void close() noexcept {
    window_ = nullptr;
    try {
      if (view_.valid()) {
        (void)invokePowerPointWithRetry(
            [&] { return view_.call(L"Exit"); });
      }
    } catch (...) {
    }
    view_ = DispatchObject();
    slideShowWindow_ = DispatchObject();
    try {
      if (presentation_.valid()) {
        (void)invokePowerPointWithRetry(
            [&] { return presentation_.call(L"Close"); });
      }
    } catch (...) {
    }
    presentation_ = DispatchObject();
    slideCount_ = 0;
    path_.clear();
    cachedState_ = {};
    transitionDeadline_.reset();
    transitionRecoveryAttempted_ = false;
    resumeRequired_ = false;
    transitionError_.clear();
    requestedSize_ = {kMaximumCaptureWidth, kMaximumCaptureHeight};
  }

  HWND window() const noexcept { return window_; }
  SIZE captureSize() const noexcept { return captureSize_; }
  PowerPointNavigationState cachedNavigationState() const { return cachedState_; }

  PowerPointNavigationState navigationState() {
    cachedState_ = queryNavigationState();
    return cachedState_;
  }

  PowerPointCommandResponse next() { return navigate(true); }
  PowerPointCommandResponse previous() { return navigate(false); }

private:
  void stopTaskbarFlashing() const {
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = window_;
    flash.dwFlags = FLASHW_STOP;
    (void)FlashWindowEx(&flash);
  }

  int currentPosition() {
    if (!transitionError_.isEmpty()) {
      throw std::runtime_error(transitionError_.toUtf8().constData());
    }
    // If PowerPoint rejected the resume after accepting our pause, finish it
    // on the next poll before publishing a ready state or accepting NEXT.
    if (resumeRequired_) {
      view_.setProperty(L"State", Variant(kPpSlideShowRunning));
      resumeRequired_ = false;
    }
    int position = view_.property(L"CurrentShowPosition").asLong();
    if (position == 0) {
      const auto now = std::chrono::steady_clock::now();
      if (!transitionDeadline_) {
        // CurrentShowPosition is zero during a transition. Use its own
        // duration, so a deliberately long transition is not cut short.
        auto timeout = 8000ms;
        try {
          DispatchObject slide = view_.objectProperty(L"Slide");
          const float duration = slide.objectProperty(L"SlideShowTransition")
                                     .property(L"Duration").asFloat();
          if (std::isfinite(duration) && duration >= 0.0F && duration <= 86400.0F) {
            timeout = std::chrono::milliseconds(static_cast<long long>(
                std::ceil(static_cast<double>(duration) * 1000.0 + 2000.0)));
          }
        } catch (const ComFailure &) {
          // Older Office versions may not expose the transition duration.
        }
        transitionDeadline_ = now + timeout;
      }
      if (now >= *transitionDeadline_) {
        if (transitionRecoveryAttempted_ ||
            view_.property(L"State").asLong() != kPpSlideShowRunning) {
          transitionError_ = QStringLiteral(
              "PowerPoint did not finish the slide transition. "
              "Switch OFF AIR and prepare the item again.");
          qWarning() << transitionError_ << path_;
          throw std::runtime_error(transitionError_.toUtf8().constData());
        }
        // Some Office builds remain Running with position zero indefinitely.
        // Pause/resume finishes that stalled transition without issuing an
        // extra NEXT (which could consume a click animation or skip a slide).
        transitionRecoveryAttempted_ = true;
        transitionDeadline_ = now + 2s;
        qWarning() << "PowerPoint transition stalled; recovering:" << path_
                   << "last slide" << cachedState_.currentSlide;
        resumeRequired_ = true;
        view_.setProperty(L"State", Variant(kPpSlideShowPaused));
        view_.setProperty(L"State", Variant(kPpSlideShowRunning));
        resumeRequired_ = false;
        position = view_.property(L"CurrentShowPosition").asLong();
      }
    }
    if (position > 0) {
      if (transitionRecoveryAttempted_) {
        qInfo() << "PowerPoint transition recovered:" << path_
                << "slide" << position << "/" << slideCount_;
      }
      transitionDeadline_.reset();
      transitionRecoveryAttempted_ = false;
    }
    return position;
  }

  PowerPointNavigationState queryNavigationState() {
    PowerPointNavigationState state;
    if (!view_.valid() || !window_ || !IsWindow(window_)) {
      return state;
    }
    // Office may request attention again after a slide/navigation change.
    stopTaskbarFlashing();
    state.active = true;
    state.currentSlide = currentPosition();
    state.slideCount = slideCount_;
    if (state.currentSlide == 0) {
      // Preserve the last confirmed page and navigation availability while
      // the transition is pending so the controls do not flicker together.
      // Never interpret zero as a navigation boundary or an initial frame.
      state.currentSlide = cachedState_.currentSlide;
      state.clickIndex = cachedState_.clickIndex;
      state.clickCount = cachedState_.clickCount;
      state.canGoPrevious = cachedState_.canGoPrevious;
      state.canGoNext = cachedState_.canGoNext;
      state.busy = true;
      return state;
    }
    constexpr int clickStateAttempts = 5;
    for (int attempt = 0; attempt < clickStateAttempts; ++attempt) {
      try {
        state.clickIndex = view_.call(L"GetClickIndex").asLong();
        state.clickCount = view_.call(L"GetClickCount").asLong();
        break;
      } catch (const ComFailure &failure) {
        // PowerPoint can temporarily reject click-state queries while an
        // autoplay media effect is starting. Slide navigation remains valid.
        const bool transientClickState =
            isPowerPointBusy(failure) || failure.result() == DISP_E_EXCEPTION;
        if (!transientClickState) {
          throw;
        }
        if (attempt + 1 >= clickStateAttempts) {
          state.clickIndex = 0;
          state.clickCount = 0;
          break;
        }
        pumpPendingMessages();
        std::this_thread::sleep_for(50ms);
      }
    }
    const bool clickEffectRemains =
        state.clickCount > 0 && state.clickIndex >= 0 &&
        state.clickIndex < state.clickCount;
    state.canGoNext =
        state.currentSlide > 0 &&
        (state.currentSlide < state.slideCount || clickEffectRemains);
    state.canGoPrevious =
        state.currentSlide > 1 || state.clickIndex > 0 ||
        state.clickIndex == kMsoClickStateAfterAllAnimations;
    return state;
  }

  PowerPointCommandResponse navigate(bool forward) {
    try {
      const PowerPointNavigationState state = navigationState();
      if (state.busy) {
        return {PowerPointCommandResult::Busy,
                QStringLiteral("PowerPoint is busy.")};
      }
      const bool canNavigate = forward ? state.canGoNext
                                       : state.canGoPrevious;
      if (!state.active || !canNavigate) {
        cachedState_ = state;
        return {PowerPointCommandResult::Boundary, {}};
      }
      (void)view_.call(forward ? L"Next" : L"Previous");
      try {
        cachedState_ = queryNavigationState();
      } catch (...) {
      }
      return {PowerPointCommandResult::Completed, {}};
    } catch (const ComFailure &failure) {
      if (isPowerPointBusy(failure)) {
        return {PowerPointCommandResult::Busy, failure.detail()};
      }
      return {PowerPointCommandResult::Failed, failure.detail()};
    } catch (const std::exception &failure) {
      return {PowerPointCommandResult::Failed,
              QString::fromUtf8(failure.what())};
    }
  }

  DispatchObject presentation_;
  DispatchObject slideShowWindow_;
  DispatchObject view_;
  HWND window_ = nullptr;
  SIZE captureSize_{kMaximumCaptureWidth, kMaximumCaptureHeight};
  SIZE requestedSize_{kMaximumCaptureWidth, kMaximumCaptureHeight};
  int slideCount_ = 0;
  QString path_;
  PowerPointNavigationState cachedState_;
  std::optional<std::chrono::steady_clock::time_point> transitionDeadline_;
  bool transitionRecoveryAttempted_ = false;
  bool resumeRequired_ = false;
  QString transitionError_;
};

class PowerPointApplication final {
public:
  PowerPointApplication() = default;
  ~PowerPointApplication() { close(); }

  bool start(QString *errorDetail) {
    try {
      if (application_.valid()) {
        try {
          // A retained IDispatch can outlive POWERPNT.EXE. Validate it during
          // CUE preparation, before reporting the selected source as ready.
          (void)invokePowerPointWithRetry(
              [&] { return application_.property(L"Name"); });
          return true;
        } catch (const ComFailure &failure) {
          if (!isPowerPointDisconnected(failure.result())) throw;
          qWarning() << "PowerPoint connection lost; reconnecting during preparation:"
                     << failure.detail();
          discardDisconnectedApplication();
        }
      }
      if (!apartment_) {
        apartment_ = std::make_unique<ComApartment>();
      }
      CLSID classId{};
      throwIfFailed(CLSIDFromProgID(L"PowerPoint.Application", &classId),
                    QStringLiteral(
                        "Microsoft PowerPoint is not installed or registered"));
      std::scoped_lock applicationLock(gPowerPointApplicationMutex);
      const std::set<DWORD> processIdsBefore = powerPointProcessIds();
      processIdsBeforeStart_ = processIdsBefore;
      if (gPowerPointStartedProcessId != 0 &&
          !processIdsBefore.contains(gPowerPointStartedProcessId)) {
        gPowerPointStartedProcessId = 0;
      }
      ComPtr<IUnknown> active;
      ComPtr<IDispatch> dispatch;
      bool applicationWasCreated = false;
      if (SUCCEEDED(GetActiveObject(classId, nullptr, &active))) {
        throwIfFailed(active.As(&dispatch),
                      QStringLiteral("GetActiveObject PowerPoint IDispatch"));
      } else {
        throwIfFailed(CoCreateInstance(classId, nullptr, CLSCTX_LOCAL_SERVER,
                                       IID_PPV_ARGS(&dispatch)),
                      QStringLiteral(
                          "Could not start Microsoft PowerPoint"));
        applicationWasCreated = true;
      }
      application_ = DispatchObject(std::move(dispatch));
      bool applicationIsVisible = true;
      try {
        applicationIsVisible =
            application_.property(L"Visible").asBool();
      } catch (...) {
      }
      applicationProcessId_ = powerPointApplicationProcessId(
          application_, processIdsBefore, applicationWasCreated);
      const bool applicationCanBeOwned =
          applicationWasCreated ||
          (processIdsBefore.empty() && !applicationIsVisible);
      applicationCanBeOwned_ = applicationCanBeOwned;
      refreshOwnedProcessIdentity();
      ++gPowerPointAutomationCount;
      registeredApplication_ = true;
      return true;
    } catch (const ComFailure &failure) {
      if (errorDetail) {
        *errorDetail = failure.detail();
      }
    } catch (const std::exception &failure) {
      if (errorDetail) {
        *errorDetail = QString::fromUtf8(failure.what());
      }
    }
    return false;
  }

  std::unique_ptr<PowerPointSlideShow>
  openSlideShow(const QString &filePath, QString *errorDetail) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!start(errorDetail)) return {};
      auto slideShow = std::make_unique<PowerPointSlideShow>();
      HRESULT failureCode = S_OK;
      if (slideShow->open(application_, filePath, errorDetail, &failureCode)) {
        if (errorDetail) errorDetail->clear();
        refreshOwnedProcessIdentity();
        return slideShow;
      }
      if (!isPowerPointDisconnected(failureCode)) return {};
      // Office can also exit between the connection check and opening the
      // document. Retry once on the preparation worker, never from TAKE.
      slideShow.reset();
      discardDisconnectedApplication();
    }
    return {};
  }

  QImage exportFirstSlide(const QString &filePath, const QSize &targetSize,
                          QString *errorDetail) {
    if (!start(errorDetail)) {
      return {};
    }
    constexpr int maximumAttempts = 30;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
      DispatchObject presentation;
      try {
        const QFileInfo fileInfo(filePath);
        const QString validationError =
            powerPointFileValidationError(filePath);
        if (!validationError.isEmpty()) {
          throw std::runtime_error(validationError.toUtf8().constData());
        }
        DispatchObject presentations =
            application_.objectProperty(L"Presentations");
        Variant opened = invokePowerPointWithRetry([&] {
          return presentations.call(
              L"Open",
              {Variant(QDir::toNativeSeparators(fileInfo.absoluteFilePath())
                           .toStdWString()),
               Variant(kMsoTrue), Variant(kMsoFalse), Variant(kMsoFalse)});
        });
        presentation = DispatchObject(opened.asDispatch());
        refreshOwnedProcessIdentity();
        DispatchObject slides = presentation.objectProperty(L"Slides");
        if (slides.property(L"Count").asLong() <= 0) {
          throw std::runtime_error("The presentation has no slides");
        }
        float slideWidth = 16.0F;
        float slideHeight = 9.0F;
        try {
          DispatchObject pageSetup =
              presentation.objectProperty(L"PageSetup");
          slideWidth = pageSetup.property(L"SlideWidth").asFloat();
          slideHeight = pageSetup.property(L"SlideHeight").asFloat();
        } catch (...) {
        }
        const float aspect =
            std::clamp(slideWidth / slideHeight, 0.3F, 3.0F);
        constexpr int exportWidth = 320;
        const int exportHeight = (std::max)(
            1, static_cast<int>(std::lround(exportWidth / aspect)));
        DispatchObject firstSlide(
            slides.call(L"Item", {Variant(1L)}).asDispatch());
        QTemporaryDir temporaryDirectory;
        if (!temporaryDirectory.isValid()) {
          throw std::runtime_error(
              "Could not create a temporary thumbnail directory");
        }
        const QString exportedPath =
            temporaryDirectory.filePath(QStringLiteral("slide-1.png"));
        (void)firstSlide.call(
            L"Export",
            {Variant(QDir::toNativeSeparators(exportedPath).toStdWString()),
             Variant(std::wstring(L"PNG")),
             Variant(static_cast<long>(exportWidth)),
             Variant(static_cast<long>(exportHeight))});
        QImage exported(exportedPath);
        if (exported.isNull()) {
          throw std::runtime_error(
              "PowerPoint did not produce the first-slide thumbnail");
        }
        QImage result(targetSize, QImage::Format_RGBA8888);
        result.fill(Qt::black);
        const QImage scaled = exported.scaled(
            targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter painter(&result);
        painter.drawImage((targetSize.width() - scaled.width()) / 2,
                          (targetSize.height() - scaled.height()) / 2,
                          scaled);
        (void)invokePowerPointWithRetry(
            [&] { return presentation.call(L"Close"); });
        return result;
      } catch (const ComFailure &failure) {
        try {
          if (presentation.valid()) {
            (void)invokePowerPointWithRetry(
                [&] { return presentation.call(L"Close"); });
          }
        } catch (...) {
        }
        if (isPowerPointBusy(failure) && attempt + 1 < maximumAttempts) {
          pumpPendingMessages();
          std::this_thread::sleep_for(100ms);
          continue;
        }
        if (errorDetail) {
          *errorDetail = failure.detail();
        }
        return {};
      } catch (const std::exception &failure) {
        try {
          if (presentation.valid()) {
            (void)invokePowerPointWithRetry(
                [&] { return presentation.call(L"Close"); });
          }
        } catch (...) {
        }
        if (errorDetail) {
          *errorDetail = QString::fromUtf8(failure.what());
        }
        return {};
      }
    }
    return {};
  }

  PowerPointSlideExportResult exportSlides(
      const QString &filePath, const QString &outputDirectory,
      const std::atomic_bool *cancelRequested,
      const PowerPointSlideExportProgress &progress) {
    PowerPointSlideExportResult result;
    if (cancelWasRequested(cancelRequested)) {
      result.canceled = true;
      return result;
    }
    if (!start(&result.errorDetail)) {
      return result;
    }

    const QString validationError = powerPointFileValidationError(filePath);
    if (!validationError.isEmpty()) {
      result.errorDetail = validationError;
      return result;
    }
    QDir outputDir(outputDirectory);
    if (outputDirectory.isEmpty() ||
        (!outputDir.exists() && !QDir().mkpath(outputDirectory))) {
      result.errorDetail =
          QStringLiteral("Could not create the PowerPoint output directory.");
      return result;
    }

    DispatchObject presentation;
    try {
      const QFileInfo fileInfo(filePath);
      DispatchObject presentations =
          application_.objectProperty(L"Presentations");
      Variant opened = invokePowerPointWithRetry([&] {
        return presentations.call(
            L"Open",
            {Variant(QDir::toNativeSeparators(fileInfo.absoluteFilePath())
                         .toStdWString()),
             Variant(kMsoTrue), Variant(kMsoFalse), Variant(kMsoFalse)});
      });
      presentation = DispatchObject(opened.asDispatch());
      refreshOwnedProcessIdentity();
      DispatchObject slides = presentation.objectProperty(L"Slides");
      const int slideCount = slides.property(L"Count").asLong();
      if (slideCount <= 0) {
        throw std::runtime_error("The presentation has no slides");
      }

      float slideWidth = 16.0F;
      float slideHeight = 9.0F;
      try {
        DispatchObject pageSetup = presentation.objectProperty(L"PageSetup");
        slideWidth = pageSetup.property(L"SlideWidth").asFloat();
        slideHeight = pageSetup.property(L"SlideHeight").asFloat();
      } catch (...) {
      }
      const float aspect =
          std::clamp(slideWidth / slideHeight, 0.3F, 3.0F);
      int renderWidth = kMaximumCaptureWidth;
      int renderHeight = static_cast<int>(
          std::lround(kMaximumCaptureWidth / aspect));
      if (renderHeight > kMaximumCaptureHeight) {
        renderHeight = kMaximumCaptureHeight;
        renderWidth = static_cast<int>(
            std::lround(kMaximumCaptureHeight * aspect));
      }
      renderWidth = (std::max)(1, renderWidth);
      renderHeight = (std::max)(1, renderHeight);

      QString artifactStem = fileInfo.completeBaseName().trimmed();
      artifactStem.replace(
          QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]+")),
          QStringLiteral("_"));
      artifactStem.replace(
          QRegularExpression(QStringLiteral("\\s+$")), QString());
      if (artifactStem.isEmpty()) {
        artifactStem = QStringLiteral("powerpoint");
      }
      artifactStem = artifactStem.left(80);
      if (progress) {
        progress(0, slideCount);
      }
      result.slidePaths.reserve(slideCount);
      for (int index = 1; index <= slideCount; ++index) {
        if (cancelWasRequested(cancelRequested)) {
          result.canceled = true;
          result.slidePaths.clear();
          break;
        }
        DispatchObject slide(
            slides.call(L"Item", {Variant(static_cast<long>(index))})
                .asDispatch());
        const QString suffix =
            QStringLiteral("%1").arg(index, 4, 10, QLatin1Char('0'));
        const QString temporaryPath = outputDir.filePath(
            QStringLiteral("powerpoint-export-%1.png").arg(suffix));
        const QString outputPath = outputDir.filePath(
            QStringLiteral("%1-slide-%2.png").arg(artifactStem, suffix));
        (void)invokePowerPointWithRetry([&] {
          return slide.call(
              L"Export",
              {Variant(QDir::toNativeSeparators(temporaryPath)
                           .toStdWString()),
               Variant(std::wstring(L"PNG")),
               Variant(static_cast<long>(renderWidth)),
               Variant(static_cast<long>(renderHeight))});
        });

        QImage exported(temporaryPath);
        QFile::remove(temporaryPath);
        if (exported.isNull()) {
          throw std::runtime_error(
              "PowerPoint did not produce a slide image");
        }
        QImage fullHd(kMaximumCaptureWidth, kMaximumCaptureHeight,
                      QImage::Format_ARGB32);
        fullHd.fill(Qt::black);
        {
          QPainter painter(&fullHd);
          painter.drawImage((kMaximumCaptureWidth - exported.width()) / 2,
                            (kMaximumCaptureHeight - exported.height()) / 2,
                            exported);
        }
        QImageWriter writer(outputPath, "png");
        writer.setCompression(6);
        if (!writer.write(fullHd)) {
          throw std::runtime_error(
              QStringLiteral("Could not save slide %1: %2")
                  .arg(index)
                  .arg(writer.errorString())
                  .toUtf8()
                  .constData());
        }
        result.slidePaths.append(QFileInfo(outputPath).absoluteFilePath());
        if (progress) {
          progress(index, slideCount);
        }
      }

      (void)invokePowerPointWithRetry(
          [&] { return presentation.call(L"Close"); });
      presentation = DispatchObject();
      if (!result.canceled) {
        result.succeeded = result.slidePaths.size() == slideCount;
        if (!result.succeeded && result.errorDetail.isEmpty()) {
          result.errorDetail =
              QStringLiteral("PowerPoint did not export every slide.");
        }
      }
      return result;
    } catch (const ComFailure &failure) {
      result.errorDetail = QStringLiteral(
          "PowerPoint could not open or export the presentation: %1")
                               .arg(failure.detail());
    } catch (const std::exception &failure) {
      result.errorDetail = QString::fromUtf8(failure.what());
    }
    try {
      if (presentation.valid()) {
        (void)invokePowerPointWithRetry(
            [&] { return presentation.call(L"Close"); });
      }
    } catch (...) {
    }
    result.slidePaths.clear();
    return result;
  }

  void close() noexcept {
    try {
      // A slideshow can temporarily make an automation instance appear
      // visible. Re-evaluate ownership after VTR-PON has closed its own
      // presentation, before deciding whether the now-invisible process is
      // safe to quit.
      refreshOwnedProcessIdentity();
    } catch (...) {
    }
    std::unique_lock<std::recursive_mutex> applicationLock(
        gPowerPointApplicationMutex, std::defer_lock);
    bool quitRequested = false;
    const DWORD processId = applicationProcessId_;
    if (registeredApplication_) {
      applicationLock.lock();
      --gPowerPointAutomationCount;
      registeredApplication_ = false;
      if (gPowerPointAutomationCount == 0 &&
          processId != 0 && processId == gPowerPointStartedProcessId) {
        try {
          DispatchObject presentations = invokePowerPointWithRetry(
              [&] { return application_.objectProperty(L"Presentations"); });
          DispatchObject slideShowWindows = invokePowerPointWithRetry(
              [&] { return application_.objectProperty(L"SlideShowWindows"); });
          long openPresentationCount = -1;
          long openSlideShowCount = -1;
          const auto closeDeadline =
              std::chrono::steady_clock::now() + 5s;
          do {
            openPresentationCount = invokePowerPointWithRetry(
                [&] { return presentations.property(L"Count").asLong(); });
            openSlideShowCount = invokePowerPointWithRetry(
                [&] { return slideShowWindows.property(L"Count").asLong(); });
            if ((openPresentationCount == 0 && openSlideShowCount == 0) ||
                std::chrono::steady_clock::now() >= closeDeadline) {
              break;
            }
            pumpPendingMessages();
            std::this_thread::sleep_for(50ms);
          } while (gPowerPointAutomationCount == 0);
          presentations = DispatchObject();
          slideShowWindows = DispatchObject();
          if (gPowerPointAutomationCount == 0 &&
              openPresentationCount == 0 && openSlideShowCount == 0 &&
              application_.valid()) {
            for (int attempt = 0; attempt < 3; ++attempt) {
              try {
                (void)invokePowerPointWithRetry(
                    [&] { return application_.call(L"Quit"); });
                quitRequested = true;
              } catch (...) {
                if (!quitRequested) {
                  throw;
                }
                break;
              }
              pumpPendingMessages();
              if (!powerPointProcessIds().contains(processId)) {
                break;
              }
              std::this_thread::sleep_for(100ms);
            }
          }
        } catch (...) {
          // If ownership cannot be proven safely, leave PowerPoint running
          // rather than risk closing a presentation opened by the user.
        }
      }
    }
    application_ = DispatchObject();
    applicationProcessId_ = 0;
    processIdsBeforeStart_.clear();
    applicationCanBeOwned_ = false;
    apartment_.reset();
    if (quitRequested &&
        waitForPowerPointProcessExit(processId, 10s) &&
        gPowerPointStartedProcessId == processId) {
      gPowerPointStartedProcessId = 0;
    }
  }

private:
  void discardDisconnectedApplication() {
    // Do not call close(): its ownership discovery/ Quit path must never run
    // on a dead proxy or adopt an unrelated replacement Office process.
    std::scoped_lock applicationLock(gPowerPointApplicationMutex);
    if (registeredApplication_) {
      --gPowerPointAutomationCount;
      registeredApplication_ = false;
    }
    if (applicationProcessId_ != 0 &&
        gPowerPointStartedProcessId == applicationProcessId_) {
      gPowerPointStartedProcessId = 0;
    }
    application_ = DispatchObject();
    applicationProcessId_ = 0;
    processIdsBeforeStart_.clear();
    applicationCanBeOwned_ = false;
    // Keep the STA initialized: older deck objects are released on this same
    // worker after the compositor detaches them.
  }

  void refreshOwnedProcessIdentity() {
    std::scoped_lock applicationLock(gPowerPointApplicationMutex);
    if (!applicationCanBeOwned_ && processIdsBeforeStart_.empty() &&
        application_.valid()) {
      try {
        applicationCanBeOwned_ =
            !application_.property(L"Visible").asBool();
      } catch (...) {
      }
    }
    if (!applicationCanBeOwned_) {
      return;
    }
    const std::set<DWORD> currentProcessIds = powerPointProcessIds();
    DWORD candidateProcessId = applicationProcessId_;
    if (candidateProcessId == 0 ||
        !currentProcessIds.contains(candidateProcessId)) {
      if (gPowerPointStartedProcessId != 0 &&
          currentProcessIds.contains(gPowerPointStartedProcessId)) {
        candidateProcessId = gPowerPointStartedProcessId;
      } else {
        std::vector<DWORD> newProcessIds;
        std::set_difference(
            currentProcessIds.begin(), currentProcessIds.end(),
            processIdsBeforeStart_.begin(), processIdsBeforeStart_.end(),
            std::back_inserter(newProcessIds));
        candidateProcessId =
            newProcessIds.size() == 1 ? newProcessIds.front() : 0;
      }
    }
    if (candidateProcessId == 0 ||
        processIdsBeforeStart_.contains(candidateProcessId)) {
      return;
    }
    applicationProcessId_ = candidateProcessId;
    if (gPowerPointStartedProcessId == 0 ||
        gPowerPointStartedProcessId == candidateProcessId) {
      gPowerPointStartedProcessId = candidateProcessId;
    }
  }

  std::unique_ptr<ComApartment> apartment_;
  DispatchObject application_;
  bool registeredApplication_ = false;
  DWORD applicationProcessId_ = 0;
  std::set<DWORD> processIdsBeforeStart_;
  bool applicationCanBeOwned_ = false;
};

class WindowDc final {
public:
  explicit WindowDc(HWND window) : window_(window), dc_(GetDC(window)) {}
  ~WindowDc() {
    if (dc_) {
      ReleaseDC(window_, dc_);
    }
  }
  HDC get() const noexcept { return dc_; }

private:
  HWND window_ = nullptr;
  HDC dc_ = nullptr;
};

class MemoryDc final {
public:
  explicit MemoryDc(HDC compatibleWith)
      : dc_(CreateCompatibleDC(compatibleWith)) {}
  ~MemoryDc() {
    if (dc_) {
      DeleteDC(dc_);
    }
  }
  HDC get() const noexcept { return dc_; }

private:
  HDC dc_ = nullptr;
};

bool captureWindow(HWND window, int width, int height,
                   std::vector<std::uint8_t> &bgra, QString &errorDetail) {
  if (!IsWindow(window) || width <= 0 || height <= 0) {
    errorDetail = QStringLiteral(
        "The PowerPoint slide-show window is no longer available.");
    return false;
  }
  WindowDc sourceDc(window);
  if (!sourceDc.get()) {
    errorDetail = QStringLiteral("Could not read the PowerPoint window.");
    return false;
  }
  MemoryDc memoryDc(sourceDc.get());
  if (!memoryDc.get()) {
    errorDetail = QStringLiteral(
        "Could not create the PowerPoint capture buffer.");
    return false;
  }

  BITMAPINFO bitmapInfo{};
  bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmapInfo.bmiHeader.biWidth = width;
  bitmapInfo.bmiHeader.biHeight = -height;
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memoryDc.get(), &bitmapInfo,
                                    DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) {
    if (bitmap) {
      DeleteObject(bitmap);
    }
    errorDetail = QStringLiteral(
        "Could not allocate the PowerPoint capture image.");
    return false;
  }

  HGDIOBJ previousBitmap = SelectObject(memoryDc.get(), bitmap);
  PatBlt(memoryDc.get(), 0, 0, width, height, BLACKNESS);
  DwmFlush();
  BOOL captured = PrintWindow(window, memoryDc.get(),
                              PW_CLIENTONLY | kPwRenderFullContent);
  if (!captured) {
    captured = PrintWindow(window, memoryDc.get(), PW_CLIENTONLY);
  }
  if (!captured) {
    captured = BitBlt(memoryDc.get(), 0, 0, width, height, sourceDc.get(),
                      0, 0, SRCCOPY | CAPTUREBLT);
  }
  if (captured) {
    const std::size_t byteCount = static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height) * 4;
    const auto *first = static_cast<const std::uint8_t *>(bits);
    bgra.assign(first, first + byteCount);
  }
  SelectObject(memoryDc.get(), previousBitmap);
  DeleteObject(bitmap);
  if (!captured) {
    errorDetail = QStringLiteral(
        "PowerPoint window capture failed.");
    return false;
  }
  return true;
}

bool stopPipeline(GstElement *pipeline) {
  if (!pipeline) {
    return true;
  }
  if (gst_element_set_state(pipeline, GST_STATE_NULL) ==
      GST_STATE_CHANGE_FAILURE) {
    return false;
  }
  GstState current = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  const GstStateChangeReturn result = gst_element_get_state(
      pipeline, &current, &pending, kShutdownStateTimeout);
  return result != GST_STATE_CHANGE_FAILURE && current == GST_STATE_NULL &&
         pending == GST_STATE_VOID_PENDING;
}

} // namespace

class PowerPointSourceSessionManager::Impl final {
public:
  struct Session {
    int deckIndex = -1;
    QString filePath;
    std::unique_ptr<PowerPointSlideShow> slideShow;
    GstElement *pipeline = nullptr;
    GstElement *appSource = nullptr;
    GstElement *appSink = nullptr;
    GstBus *bus = nullptr;
    std::jthread captureThread;
    std::atomic<quint64> captureStartedTickMs{0};
    std::atomic<quint64> lastFrameTickMs{0};
    std::atomic_bool initialSlideReady{false};
    mutable std::mutex errorMutex;
    quint64 errorGeneration = 0;
    QString errorDetail;
    PowerPointNavigationState navigationState;
    std::mutex navigationMutex;
    int navigationCommand = 0;
    quint64 navigationGeneration = 0;
    std::optional<PowerPointCommandResponse> navigationResponse;
    std::atomic_bool released{false};
    std::atomic_bool inputReady{false};
    GstContext *context = nullptr;
    bool initializationAttempted = false;
    bool registeredAsNativeSource = false;
  };

  Impl(int outputWidth, int outputHeight)
      : outputWidth_((std::max)(1, outputWidth)),
        outputHeight_((std::max)(1, outputHeight)),
        worker_([this](std::stop_token stop) { runWorker(stop); }) {}

  ~Impl() {
    shutdown();
    gst_clear_context(&programContext_);
  }

  void setProgramContext(GstContext *context) {
    if (shutDown_ || programContext_ == context) {
      return;
    }
    gst_clear_context(&programContext_);
    programContext_ = context ? gst_context_ref(context) : nullptr;
  }

  void requestApplicationWarmup() {
    if (shutDown_ || applicationIsWarm()) return;
    {
      std::scoped_lock lock(workerMutex_);
      applicationWarmupPending_ = true;
    }
    workerWake_.notify_one();
  }

  bool applicationIsWarm() const {
    return applicationWarm_.load(std::memory_order_acquire);
  }

  bool bindProgramDeck(int deckIndex, const QString &filePath,
                       ProgramSource &source, QString *errorDetail) {
    source = {};
    if (errorDetail) {
      errorDetail->clear();
    }
    if (shutDown_ || deckIndex < 0 || deckIndex >= 2 ||
        filePath.isEmpty()) {
      if (errorDetail) {
        *errorDetail = QStringLiteral(
            "The PowerPoint ON AIR source request is invalid.");
      }
      return false;
    }
    if (!releaseProgramDeckLocked(deckIndex)) {
      if (errorDetail) {
        *errorDetail = QStringLiteral(
            "Could not release the previous PowerPoint deck.");
      }
      return false;
    }

    auto session = std::make_shared<Session>();
    session->deckIndex = deckIndex;
    session->filePath = filePath;
    session->context = programContext_ ? gst_context_ref(programContext_) : nullptr;
    session->navigationState.active = true;
    session->navigationState.busy = true;
    session->registeredAsNativeSource = true;
    gPowerPointNativeSourceCount.fetch_add(1, std::memory_order_release);
    gPowerPointAttachedSourceCount.fetch_add(1, std::memory_order_release);
    sessions_[deckIndex] = session;
    {
      std::scoped_lock lock(workerMutex_);
      workerSessions_.push_back(std::move(session));
    }
    workerWake_.notify_one();
    return true;
  }

  ProgramSource programSource(int deckIndex) const {
    const Session *session = sessionAt(deckIndex);
    if (!session || !session->inputReady.load(std::memory_order_acquire)) return {};
    return {session->pipeline, session->appSink};
  }

  bool initializeInput(const std::shared_ptr<Session> &session, QString *errorDetail) {
    const auto &filePath = session->filePath;
    const int deckIndex = session->deckIndex;
    QString detail = powerPointFileValidationError(filePath);
    if (!detail.isEmpty()) {
      *errorDetail = detail;
      return false;
    }
    // The actual slide size is negotiated by the STA worker before its first
    // frame. Creating the empty live input does not need to wait for Office.
    const SIZE captureSize{kMaximumCaptureWidth, kMaximumCaptureHeight};
    const QString prefix = QStringLiteral("powerpoint-deck-%1")
                               .arg(deckIndex);
    const QString description = QStringLiteral(
        "appsrc name=%1-source is-live=true do-timestamp=true "
        "format=time block=false "
        "caps=video/x-raw,format=BGRA,width=%2,height=%3,"
        "pixel-aspect-ratio=1/1,framerate=%4/1 ! "
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 "
        "leaky=downstream ! videoconvert ! d3d11upload ! "
        "video/x-raw(memory:D3D11Memory) ! "
        "d3d11convert add-borders=true border-color=0 ! "
        "video/x-raw(memory:D3D11Memory),format=RGBA,width=%5,"
        "height=%6,pixel-aspect-ratio=1/1 ! "
        "appsink name=%1-sink max-buffers=2 leaky-type=downstream "
        "wait-on-eos=false sync=true qos=false async=false "
        "enable-last-sample=false")
            .arg(prefix)
            .arg(captureSize.cx)
            .arg(captureSize.cy)
            .arg(kCaptureFramesPerSecond)
            .arg(outputWidth_)
            .arg(outputHeight_);
    GError *parseError = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (parseError) {
      detail = QString::fromUtf8(parseError->message);
      g_error_free(parseError);
    }
    if (!pipeline || !GST_IS_PIPELINE(pipeline)) {
      if (pipeline) {
        gst_object_unref(pipeline);
      }
      if (errorDetail) {
        *errorDetail = detail.isEmpty()
                           ? QStringLiteral(
                                 "Could not create the PowerPoint video input.")
                           : detail;
      }
      return false;
    }

    session->captureStartedTickMs = GetTickCount64();
    session->pipeline = pipeline;
    session->appSource = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (prefix + QStringLiteral("-source")).toUtf8().constData());
    session->appSink = gst_bin_get_by_name(
        GST_BIN(pipeline),
        (prefix + QStringLiteral("-sink")).toUtf8().constData());
    session->bus = gst_element_get_bus(pipeline);
    if (!session->appSource || !GST_IS_APP_SRC(session->appSource) ||
        !session->appSink || !GST_IS_APP_SINK(session->appSink) ||
        !session->bus) {
      if (errorDetail) {
        *errorDetail = QStringLiteral(
            "Could not create the PowerPoint source elements.");
      }
      return false;
    }
    if (session->context) {
      gst_element_set_context(session->pipeline, session->context);
    }
    if (gst_element_set_state(session->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      pollBus(*session);
      if (errorDetail) {
        std::scoped_lock lock(session->errorMutex);
        *errorDetail = session->errorDetail.isEmpty()
                           ? QStringLiteral(
                                 "Could not start the PowerPoint video input.")
                           : session->errorDetail;
      }
      return false;
    }
    session->inputReady.store(true, std::memory_order_release);
    return true;
  }

  bool releaseProgramDeck(int deckIndex) {
    return releaseProgramDeckLocked(deckIndex);
  }

  bool sourceIsPlaying(int deckIndex) const {
    return programSource(deckIndex).isValid();
  }

  bool sourceIsReady(int deckIndex) {
    Session *session = sessionAt(deckIndex);
    if (!session) {
      return false;
    }
    return session->initialSlideReady.load(std::memory_order_acquire);
  }

  bool sourceErrorAfter(int deckIndex, quint64 previousGeneration,
                        quint64 &currentGeneration, QString &errorDetail) {
    Session *session = sessionAt(deckIndex);
    if (!session) {
      return false;
    }
    std::scoped_lock lock(session->errorMutex);
    currentGeneration = session->errorGeneration;
    if (currentGeneration <= previousGeneration) {
      return false;
    }
    errorDetail = session->errorDetail;
    return true;
  }

  PowerPointNavigationState navigationState(int deckIndex) {
    Session *session = sessionAt(deckIndex);
    if (!session) {
      return {};
    }
    std::scoped_lock lock(session->navigationMutex);
    auto state = session->navigationState;
    state.busy = state.busy || session->navigationCommand != 0;
    return state;
  }

  void updateNavigation(Session *session) {
    try {
      const auto state = session->slideShow->navigationState();
      if (!state.active) {
        storeError(*session, QStringLiteral(
            "The PowerPoint slide-show window closed. Select the item again to prepare it."));
      }
      publishNavigation(*session, state);
    } catch (const ComFailure &failure) {
      std::scoped_lock lock(session->navigationMutex);
      PowerPointNavigationState state = session->navigationState;
      state.active = true;
      state.busy = isPowerPointBusy(failure);
      state.errorDetail = failure.detail();
      if (!state.busy) {
        storeError(*session, state.errorDetail);
      }
      session->navigationState = state;
    } catch (const std::exception &failure) {
      std::scoped_lock lock(session->navigationMutex);
      PowerPointNavigationState state = session->navigationState;
      state.active = true;
      state.errorDetail = QString::fromUtf8(failure.what());
      storeError(*session, state.errorDetail);
      session->navigationState = state;
    }
  }

  PowerPointCommandResponse next(int deckIndex) {
    return navigate(deckIndex, 1);
  }

  PowerPointCommandResponse previous(int deckIndex) {
    return navigate(deckIndex, -1);
  }

  PowerPointCommandResponse navigate(int deckIndex, int direction) {
    Session *session = sessionAt(deckIndex);
    if (!session) return {PowerPointCommandResult::Failed,
                          QStringLiteral("No PowerPoint item is ON AIR.")};
    std::scoped_lock lock(session->navigationMutex);
    if (session->navigationResponse) {
      auto response = *session->navigationResponse;
      session->navigationResponse.reset();
      return response;
    }
    if (session->navigationCommand == 0) {
      session->navigationCommand = direction;
      workerWake_.notify_one();
    }
    return {PowerPointCommandResult::Busy, {}};
  }

  void cancelNavigation(int deckIndex) {
    auto *session = sessionAt(deckIndex);
    if (!session) return;
    std::scoped_lock lock(session->navigationMutex);
    ++session->navigationGeneration;
    session->navigationCommand = 0;
    session->navigationResponse.reset();
  }

  void requestShutdown() {
    shutDown_ = true;
    for (int deckIndex = 0; deckIndex < 2; ++deckIndex) releaseProgramDeckLocked(deckIndex);
    worker_.request_stop();
    workerWake_.notify_one();
  }

  bool shutdownComplete() const { return workerFinished_.load(std::memory_order_acquire); }

  bool shutdown() {
    const bool alreadyRequested = shutDown_;
    requestShutdown();
    if (alreadyRequested && !shutdownComplete()) return false;
    if (worker_.joinable() &&
        WaitForSingleObject(worker_.native_handle(), 20000) != WAIT_OBJECT_0) return false;
    if (worker_.joinable()) worker_.join();
    return shutdownComplete();
  }

private:
  static void publishNavigation(Session &session, const PowerPointNavigationState &state) {
    {
      std::scoped_lock lock(session.navigationMutex);
      session.navigationState = state;
    }
    if (state.active && !state.busy && state.errorDetail.isEmpty() &&
        state.currentSlide > 0 && state.currentSlide <= state.slideCount &&
        !session.initialSlideReady.load(std::memory_order_acquire)) {
      session.captureStartedTickMs = GetTickCount64();
      session.initialSlideReady.store(true, std::memory_order_release);
      qInfo() << "PowerPoint native ready:" << session.filePath
              << "slide" << state.currentSlide << "/" << state.slideCount;
    }
  }

  void runWorker(std::stop_token stop) {
    // All native presentation COM objects, including their destruction, stay
    // in this STA. Keep the application alive between CUE requests so repeated
    // selections do not pay Office process startup/exit costs.
    PowerPointApplication application;
    for (;;) {
      std::vector<std::shared_ptr<Session>> sessions;
      bool warmupRequested = false;
      {
        std::unique_lock lock(workerMutex_);
        workerWake_.wait_for(lock, 100ms);
        sessions = workerSessions_;
        warmupRequested = std::exchange(applicationWarmupPending_, false);
      }
      if (warmupRequested && !stop.stop_requested() && !applicationIsWarm()) {
        PowerPointInteropOperation operation;
        QString detail;
        const auto started = std::chrono::steady_clock::now();
        if (application.start(&detail)) {
          applicationWarm_.store(true, std::memory_order_release);
          qInfo() << "PowerPoint application warmup ms:"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started).count();
        } else {
          // A speculative startup failure must not invalidate a playlist item.
          // CUE retries normally and reports any failure through its UI state.
          qWarning() << "PowerPoint application warmup failed:" << detail;
        }
      }
      for (const auto &session : sessions) {
        if (session->released.load(std::memory_order_acquire)) {
          PowerPointInteropOperation operation;
          if (releaseSession(*session)) {
            std::scoped_lock lock(workerMutex_);
            std::erase(workerSessions_, session);
          }
          continue;
        }
        if (!session->initializationAttempted) {
          session->initializationAttempted = true;
          const auto started = std::chrono::steady_clock::now();
          try {
            PowerPointInteropOperation operation;
            QString detail;
            if (!initializeInput(session, &detail)) {
              storeError(*session, detail);
              continue;
            }
            if (session->released.load(std::memory_order_acquire)) continue;
            session->slideShow = application.openSlideShow(session->filePath, &detail);
            if (!session->slideShow) {
              applicationWarm_.store(false, std::memory_order_release);
              storeError(*session, detail);
              continue;
            }
            applicationWarm_.store(true, std::memory_order_release);
            if (session->released.load(std::memory_order_acquire)) continue;
            const SIZE size = session->slideShow->captureSize();
            const auto capsText = QStringLiteral(
                "video/x-raw,format=BGRA,width=%1,height=%2,"
                "pixel-aspect-ratio=1/1,framerate=%3/1")
                .arg(size.cx).arg(size.cy).arg(kCaptureFramesPerSecond).toUtf8();
            GstCaps *caps = gst_caps_from_string(capsText.constData());
            gst_app_src_set_caps(GST_APP_SRC(session->appSource), caps);
            gst_caps_unref(caps);
            // Opening already queried the initial slide/click state.
            publishNavigation(*session, session->slideShow->cachedNavigationState());
            startCapture(*session, size);
            qInfo() << "PowerPoint native preparation ms:"
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started).count()
                    << session->filePath;
          } catch (const std::exception &failure) {
            storeError(*session, QString::fromUtf8(failure.what()));
          }
          continue;
        }
        pollBus(*session);
        if (!session->slideShow) continue;
        const auto reference = session->lastFrameTickMs.load(std::memory_order_relaxed);
        if (session->initialSlideReady.load(std::memory_order_acquire) && reference != 0 &&
            GetTickCount64() > reference + 5000) {
          storeError(*session, QStringLiteral(
              "PowerPoint capture stopped updating. Switch OFF AIR and prepare the item again."));
        }
        int command = 0;
        quint64 generation = 0;
        {
          std::scoped_lock lock(session->navigationMutex);
          command = session->navigationCommand;
          generation = session->navigationGeneration;
        }
        if (command != 0) {
          const auto response = command > 0 ? session->slideShow->next()
                                             : session->slideShow->previous();
          std::scoped_lock lock(session->navigationMutex);
          if (generation == session->navigationGeneration) {
            session->navigationResponse = response;
            session->navigationCommand = 0;
          }
        }
        updateNavigation(session.get());
      }
      pumpPendingMessages();
      if (stop.stop_requested()) {
        std::scoped_lock lock(workerMutex_);
        if (workerSessions_.empty()) break;
      }
    }
    PowerPointInteropOperation operation;
    application.close();
    applicationWarm_.store(false, std::memory_order_release);
    workerFinished_.store(true, std::memory_order_release);
  }

  bool releaseProgramDeckLocked(int deckIndex) {
    if (deckIndex < 0 || deckIndex >= 2) {
      return false;
    }
    auto &session = sessions_[deckIndex];
    if (!session) {
      return true;
    }
    // The compositor has detached this input. Its worker retains ownership
    // until COM and capture teardown finish; never join it on the UI thread.
    session->released.store(true, std::memory_order_release);
    gPowerPointAttachedSourceCount.fetch_sub(1, std::memory_order_release);
    session.reset();
    workerWake_.notify_one();
    return true;
  }
  Session *sessionAt(int deckIndex) {
    return deckIndex >= 0 && deckIndex < 2 ? sessions_[deckIndex].get()
                                           : nullptr;
  }

  const Session *sessionAt(int deckIndex) const {
    return deckIndex >= 0 && deckIndex < 2 ? sessions_[deckIndex].get()
                                           : nullptr;
  }

  static void storeError(Session &session, const QString &detail) {
    std::scoped_lock lock(session.errorMutex);
    if (detail == session.errorDetail && session.errorGeneration != 0) {
      return;
    }
    session.errorDetail = detail;
    ++session.errorGeneration;
  }

  void startCapture(Session &session, SIZE captureSize) {
    Session *sessionPointer = &session;
    const HWND window = session.slideShow->window();
    try {
    session.captureThread = std::jthread(
        [sessionPointer, window, captureSize](std::stop_token stopToken) {
          const auto interval =
              std::chrono::milliseconds(1000 / kCaptureFramesPerSecond);
          auto nextCapture = std::chrono::steady_clock::now();
          std::vector<std::uint8_t> pixels;
          while (!stopToken.stop_requested()) {
            // An HWND and a running GStreamer pipeline are not proof that
            // PowerPoint has drawn its first slide. Do not publish its blank
            // startup window to the compositor while CUE is still loading.
            if (!sessionPointer->initialSlideReady.load(std::memory_order_acquire)) {
              std::this_thread::sleep_for(interval);
              nextCapture = std::chrono::steady_clock::now();
              continue;
            }
            const quint64 capturedTickMs = GetTickCount64();
            QString detail;
            if (!captureWindow(window, captureSize.cx, captureSize.cy,
                               pixels, detail)) {
              storeError(*sessionPointer, detail);
              break;
            }
            // A black slide (or a deliberate blackout) is valid content for
            // any duration. Capture health depends on successful frame
            // delivery and the window/COM state, never on pixel brightness.
            sessionPointer->lastFrameTickMs.store(capturedTickMs,
                                                   std::memory_order_relaxed);
            GstBuffer *buffer = gst_buffer_new_allocate(
                nullptr, static_cast<gsize>(pixels.size()), nullptr);
            if (!buffer ||
                gst_buffer_fill(buffer, 0, pixels.data(), pixels.size()) !=
                    pixels.size()) {
              if (buffer) {
                gst_buffer_unref(buffer);
              }
              storeError(
                  *sessionPointer,
                  QStringLiteral(
                      "Could not allocate a PowerPoint video frame."));
              break;
            }
            GST_BUFFER_DURATION(buffer) =
                gst_util_uint64_scale_int(GST_SECOND, 1,
                                          kCaptureFramesPerSecond);
            const GstFlowReturn flow = gst_app_src_push_buffer(
                GST_APP_SRC(sessionPointer->appSource), buffer);
            if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING) {
              storeError(
                  *sessionPointer,
                  QStringLiteral(
                      "The PowerPoint video input stopped accepting frames."));
              break;
            }
            if (flow == GST_FLOW_FLUSHING && stopToken.stop_requested()) {
              break;
            }
            nextCapture += interval;
            std::this_thread::sleep_until(nextCapture);
            if (nextCapture < std::chrono::steady_clock::now() - interval) {
              nextCapture = std::chrono::steady_clock::now();
            }
          }
        });
    } catch (const std::system_error &error) {
      storeError(session, QStringLiteral("Could not start PowerPoint capture worker: %1")
                              .arg(QString::fromLocal8Bit(error.what())));
    }
  }

  void pollBus(Session &session) {
    if (!session.bus) {
      return;
    }
    while (GstMessage *message = gst_bus_pop(session.bus)) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        qWarning().noquote()
            << "GST_BUS" << (error ? error->message : "Unknown error")
            << (debug ? debug : "");
        const QString detail = QString::fromUtf8(
            error ? error->message : "Unknown PowerPoint input error");
        g_clear_error(&error);
        g_free(debug);
        storeError(session, detail);
      }
      gst_message_unref(message);
    }
  }

  bool releaseSession(Session &session) {
    if (session.captureThread.joinable()) {
      session.captureThread.request_stop();
      // PrintWindow is synchronous and cannot be canceled by a stop token.
      // Keep the entire session alive if PowerPoint stops responding, so the
      // capture thread cannot access freed buffers or GStreamer objects.
      if (WaitForSingleObject(session.captureThread.native_handle(),
                              kCaptureShutdownTimeoutMs) != WAIT_OBJECT_0) {
        storeError(session, QStringLiteral(
            "PowerPoint capture did not stop within two seconds. "
            "The source is retained until it can be released safely."));
        qWarning() << "PowerPoint capture shutdown timed out:" << session.filePath;
        return false;
      }
      session.captureThread.join();
    }
    const bool stopped = stopPipeline(session.pipeline);
    if (!stopped) {
      qWarning() << "PowerPoint source did not confirm NULL state:"
                 << session.filePath;
      // Preserve all source ownership until a later worker pass confirms NULL.
      // Clearing pipeline here would make the next retry falsely succeed.
      return false;
    }
    if (session.bus) {
      gst_object_unref(session.bus);
      session.bus = nullptr;
    }
    if (session.appSink) {
      gst_object_unref(session.appSink);
      session.appSink = nullptr;
    }
    if (session.appSource) {
      gst_object_unref(session.appSource);
      session.appSource = nullptr;
    }
    if (session.pipeline) {
      gst_object_unref(session.pipeline);
      session.pipeline = nullptr;
    }
    session.slideShow.reset();
    gst_clear_context(&session.context);
    if (session.registeredAsNativeSource) {
      session.registeredAsNativeSource = false;
      gPowerPointNativeSourceCount.fetch_sub(1, std::memory_order_release);
    }
    return stopped;
  }

  int outputWidth_ = 1920;
  int outputHeight_ = 1080;
  GstContext *programContext_ = nullptr;
  std::array<std::shared_ptr<Session>, 2> sessions_;
  bool shutDown_ = false;
  std::atomic_bool workerFinished_{false};
  std::mutex workerMutex_;
  bool applicationWarmupPending_ = false;
  std::atomic_bool applicationWarm_{false};
  std::condition_variable workerWake_;
  std::vector<std::shared_ptr<Session>> workerSessions_;
  std::jthread worker_;
};

PowerPointSourceSessionManager::PowerPointSourceSessionManager(
    int outputWidth, int outputHeight)
    : impl_(std::make_unique<Impl>(outputWidth, outputHeight)) {}

PowerPointSourceSessionManager::~PowerPointSourceSessionManager() {
  if (!impl_->shutdown()) {
    // Match the player's unconfirmed-shutdown contract: OS process teardown
    // owns the final cleanup, rather than destroying a still-running backend.
    (void)impl_.release();
  }
}

void PowerPointSourceSessionManager::setProgramContext(GstContext *context) {
  impl_->setProgramContext(context);
}

void PowerPointSourceSessionManager::requestApplicationWarmup() {
  impl_->requestApplicationWarmup();
}

bool PowerPointSourceSessionManager::applicationIsWarm() const {
  return impl_->applicationIsWarm();
}

bool PowerPointSourceSessionManager::bindProgramDeck(
    int deckIndex, const QString &filePath, ProgramSource &source,
    QString *errorDetail) {
  return impl_->bindProgramDeck(deckIndex, filePath, source, errorDetail);
}

bool PowerPointSourceSessionManager::releaseProgramDeck(int deckIndex) {
  return impl_->releaseProgramDeck(deckIndex);
}

bool PowerPointSourceSessionManager::sourceIsPlaying(int deckIndex) const {
  return impl_->sourceIsPlaying(deckIndex);
}

bool PowerPointSourceSessionManager::sourceIsReady(int deckIndex) {
  return impl_->sourceIsReady(deckIndex);
}

bool PowerPointSourceSessionManager::sourceErrorAfter(
    int deckIndex, quint64 previousGeneration, quint64 &currentGeneration,
    QString &errorDetail) {
  return impl_->sourceErrorAfter(deckIndex, previousGeneration,
                                 currentGeneration, errorDetail);
}

PowerPointNavigationState
PowerPointSourceSessionManager::navigationState(int deckIndex) {
  return impl_->navigationState(deckIndex);
}

PowerPointCommandResponse PowerPointSourceSessionManager::next(
    int deckIndex) {
  return impl_->next(deckIndex);
}

PowerPointCommandResponse PowerPointSourceSessionManager::previous(
    int deckIndex) {
  return impl_->previous(deckIndex);
}

PowerPointSourceSessionManager::ProgramSource PowerPointSourceSessionManager::programSource(int deckIndex) const {
  return impl_->programSource(deckIndex);
}
void PowerPointSourceSessionManager::requestShutdown() { impl_->requestShutdown(); }
bool PowerPointSourceSessionManager::shutdownComplete() const { return impl_->shutdownComplete(); }

void PowerPointSourceSessionManager::cancelNavigation(int deckIndex) {
  impl_->cancelNavigation(deckIndex);
}

bool PowerPointSourceSessionManager::shutdown() {
  return impl_->shutdown();
}

QString validatePowerPointSource(const QString &filePath) {
  const QString fileError = powerPointFileValidationError(filePath);
  if (!fileError.isEmpty()) {
    return fileError;
  }
  CLSID classId{};
  const HRESULT result =
      CLSIDFromProgID(L"PowerPoint.Application", &classId);
  if (FAILED(result)) {
    return QStringLiteral(
        "Microsoft PowerPoint is not installed or its COM registration is unavailable.");
  }
  return {};
}

QImage generatePowerPointThumbnail(const QString &filePath,
                                   const QSize &targetSize,
                                   QString *errorDetail) {
  PowerPointInteropOperation operationLock;
  if (errorDetail) {
    errorDetail->clear();
  }
  if (gPowerPointNativeSourceCount.load(std::memory_order_acquire) > 0) {
    if (errorDetail) {
      *errorDetail = nativeSourceActiveError();
    }
    return {};
  }
  if (targetSize.width() <= 0 || targetSize.height() <= 0) {
    if (errorDetail) {
      *errorDetail = QStringLiteral(
          "The PowerPoint thumbnail size is invalid.");
    }
    return {};
  }
  PowerPointApplication application;
  return application.exportFirstSlide(filePath, targetSize, errorDetail);
}

bool isPowerPointNativeSourceActive() {
  return gPowerPointAttachedSourceCount.load(std::memory_order_acquire) > 0;
}

bool isPowerPointInteropInProgressOnCurrentThread() {
  return gPowerPointInteropInProgress;
}

bool isPowerPointNativeSourceActiveError(const QString &errorDetail) {
  return errorDetail == nativeSourceActiveError();
}

PowerPointSlideExportResult exportPowerPointSlides(
    const QString &filePath, const QString &outputDirectory,
    const std::atomic_bool *cancelRequested,
  const PowerPointSlideExportProgress &progress) {
  // Called by the document conversion worker, never wait for Office teardown
  // in the controller's beginPowerPointConversion path.
  const auto releaseDeadline = std::chrono::steady_clock::now() + 30s;
  while (!isPowerPointNativeSourceActive() &&
         gPowerPointNativeSourceCount.load(std::memory_order_acquire) > 0 &&
         std::chrono::steady_clock::now() < releaseDeadline) {
    if (cancelWasRequested(cancelRequested)) {
      PowerPointSlideExportResult result;
      result.canceled = true;
      return result;
    }
    std::this_thread::sleep_for(25ms);
  }
  PowerPointInteropOperation operationLock;
  if (gPowerPointNativeSourceCount.load(std::memory_order_acquire) > 0) {
    PowerPointSlideExportResult result;
    result.errorDetail = nativeSourceActiveError();
    return result;
  }
  PowerPointApplication application;
  return application.exportSlides(filePath, outputDirectory, cancelRequested,
                                  progress);
}
