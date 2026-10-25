#include "switcher_addon_api.h"
#include "addon_runtime_api.h"

#ifdef _WIN32
#include <windows.h>
#include <Unknwn.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#endif

#include "BMDSwitcherAPI.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
#ifdef _WIN32
using SdkBool = BOOL;
#else
using SdkBool = bool;
#endif

template <typename T> void releaseCom(T *&value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

void writeMessage(char *destination, uint32_t capacity,
                  const std::string &message) {
  if (!destination || capacity == 0) {
    return;
  }
  const std::size_t length =
      std::min<std::size_t>(message.size(), capacity - 1);
  std::memcpy(destination, message.data(), length);
  destination[length] = '\0';
}

std::string hresultMessage(const char *operation, HRESULT result) {
  char code[16]{};
  std::snprintf(code, sizeof(code), "0x%08lX",
                static_cast<unsigned long>(static_cast<uint32_t>(result)));
  return std::string(operation) + " failed (" + code + ").";
}

std::string connectFailureMessage(BMDSwitcherConnectToFailure reason,
                                  bool usbOnly) {
  switch (reason) {
  case bmdSwitcherConnectToFailureNoResponse:
    return usbOnly
               ? "No response from the ATEM switcher over USB. Check the USB cable and update the switcher firmware with ATEM Setup."
               : "No response from the ATEM switcher at the configured network address.";
  case bmdSwitcherConnectToFailureIncompatibleFirmware:
    return "The ATEM switcher firmware is incompatible with the installed ATEM Software Control runtime.";
  case bmdSwitcherConnectToFailureCorruptData:
    return "The ATEM switcher returned corrupt connection data.";
  case bmdSwitcherConnectToFailureStateSync:
    return "The ATEM switcher state could not be synchronized.";
  case bmdSwitcherConnectToFailureStateSyncTimedOut:
    return "The ATEM switcher state synchronization timed out.";
  default:
    return "The ATEM USB connection failed.";
  }
}

#ifdef _WIN32
std::wstring utf8ToWide(const char *text) {
  if (!text || *text == '\0') {
    return {};
  }
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
                                           -1, nullptr, 0);
  if (required <= 1) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(),
                      required);
  wide.pop_back();
  return wide;
}

std::string wideToUtf8(const wchar_t *text) {
  if (!text || *text == L'\0') {
    return {};
  }
  const int required =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1) {
    return {};
  }
  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), required, nullptr,
                      nullptr);
  utf8.pop_back();
  return utf8;
}

#else
std::string cfStringToUtf8(CFStringRef text) {
  if (!text) return {};
  const CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
  std::string utf8(static_cast<std::size_t>(capacity), '\0');
  if (!CFStringGetCString(text, utf8.data(), capacity, kCFStringEncodingUTF8)) return {};
  utf8.resize(std::strlen(utf8.c_str()));
  return utf8;
}
#endif

class AtemDevice {
public:
  ~AtemDevice() { close(); }

  bool open(const char *deviceAddressUtf8, std::string &error) {
    close();
    HRESULT result = S_OK;
#ifdef _WIN32
    const HRESULT initializeResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
      error = hresultMessage("COM initialization", initializeResult);
      return false;
    }
    comInitialized_ = SUCCEEDED(initializeResult);

    result = CoCreateInstance(
        CLSID_CBMDSwitcherDiscovery, nullptr, CLSCTX_INPROC_SERVER,
        IID_IBMDSwitcherDiscovery, reinterpret_cast<void **>(&discovery_));
    if (FAILED(result)) {
      error =
          "ATEM Switcher Software runtime is not installed or is incompatible.";
      close();
      return false;
    }

    // The USB runtime may finish disconnect work after the last COM release.
    // Keep its code mapped until process shutdown so that work cannot race DLL
    // unloading.
    HMODULE runtimeModule = nullptr;
    const void *runtimeVtable = *reinterpret_cast<void **>(discovery_);
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(runtimeVtable), &runtimeModule)) {
      error = "The ATEM Switcher Software runtime could not be retained.";
      close();
      return false;
    }

    const std::wstring address = utf8ToWide(deviceAddressUtf8);
    BSTR addressBstr =
        SysAllocStringLen(address.data(), static_cast<UINT>(address.size()));
    if (!addressBstr) {
      error = "Could not allocate the ATEM device address.";
      close();
      return false;
    }

#else
    discovery_ = CreateBMDSwitcherDiscoveryInstance();
    if (!discovery_) {
      error = "ATEM Switcher Software runtime is not installed or is incompatible.";
      return false;
    }
    CFStringRef addressBstr = CFStringCreateWithCString(kCFAllocatorDefault,
        deviceAddressUtf8 ? deviceAddressUtf8 : "", kCFStringEncodingUTF8);
    if (!addressBstr) {
      error = "The ATEM device address is not valid UTF-8.";
      close();
      return false;
    }
#endif
    BMDSwitcherConnectToFailure failureReason =
        bmdSwitcherConnectToFailureNoResponse;
    result = discovery_->ConnectTo(addressBstr, &switcher_, &failureReason);
#ifdef _WIN32
    SysFreeString(addressBstr);
#else
    CFRelease(addressBstr);
#endif
    if (FAILED(result)) {
      error = connectFailureMessage(failureReason, !deviceAddressUtf8 || !*deviceAddressUtf8);
      close();
      return false;
    }

    IBMDSwitcherMixEffectBlockIterator *iterator = nullptr;
    result = switcher_->CreateIterator(
        IID_IBMDSwitcherMixEffectBlockIterator,
        reinterpret_cast<void **>(&iterator));
    if (FAILED(result) || !iterator) {
      error = "The ATEM switcher has no compatible Mix Effect block iterator.";
      close();
      return false;
    }
    result = iterator->Next(&mixEffectBlock_);
    iterator->Release();
    if (result != S_OK || !mixEffectBlock_) {
      error = "The ATEM switcher has no Mix Effect block.";
      close();
      return false;
    }
    return true;
  }

  void close() {
    releaseCom(mixEffectBlock_);
    releaseCom(switcher_);
    releaseCom(discovery_);
    pendingFtb_ = false;

#ifdef _WIN32
    if (comInitialized_) {
      CoUninitialize();
      comInitialized_ = false;
    }
#endif
  }

  bool productName(std::string &name, std::string &error) const {
    if (!switcher_) {
      error = "ATEM switcher is not connected.";
      return false;
    }
#ifdef _WIN32
    BSTR productName = nullptr;
#else
    CFStringRef productName = nullptr;
#endif
    const HRESULT result = switcher_->GetProductName(&productName);
    if (FAILED(result)) {
      error = hresultMessage("GetProductName", result);
      return false;
    }
#ifdef _WIN32
    name = wideToUtf8(productName);
    SysFreeString(productName);
#else
    name = cfStringToUtf8(productName);
    if (productName) CFRelease(productName);
#endif
    return true;
  }

  bool programInput(int64_t &sourceNumber, std::string &error) {
    if (!synchronizeFadeToBlack(error)) {
      return false;
    }
    if (!mixEffectBlock_) {
      error = "ATEM switcher is not connected.";
      return false;
    }
    BMDSwitcherInputId inputId = 0;
    const HRESULT result = mixEffectBlock_->GetProgramInput(&inputId);
    if (FAILED(result)) {
      error = hresultMessage("GetProgramInput", result);
      return false;
    }
    sourceNumber = static_cast<int64_t>(inputId);
    return true;
  }

  bool setProgramInput(int64_t sourceNumber, std::string &error) const {
    if (!mixEffectBlock_) {
      error = "ATEM switcher is not connected.";
      return false;
    }
    if (sourceNumber < 0) {
      error = "ATEM input ID cannot be negative.";
      return false;
    }
    const HRESULT result = mixEffectBlock_->SetProgramInput(
        static_cast<BMDSwitcherInputId>(sourceNumber));
    if (FAILED(result)) {
      error = hresultMessage("SetProgramInput", result);
      return false;
    }
    return true;
  }

  bool setFadeToBlack(bool enabled, std::string &error) {
    pendingFtb_ = true;
    desiredFtb_ = enabled;
    return synchronizeFadeToBlack(error);
  }

  bool fadeToBlackState(bool &fullyBlack, bool &inTransition,
                        std::string &error) {
    if (!synchronizeFadeToBlack(error)) {
      return false;
    }
    if (!mixEffectBlock_) {
      error = "ATEM switcher is not connected.";
      return false;
    }

    SdkBool transitionValue = false;
    HRESULT result =
        mixEffectBlock_->GetFadeToBlackInTransition(&transitionValue);
    if (FAILED(result)) {
      error = hresultMessage("GetFadeToBlackInTransition", result);
      return false;
    }
    SdkBool fullyBlackValue = false;
    result = mixEffectBlock_->GetFadeToBlackFullyBlack(&fullyBlackValue);
    if (FAILED(result)) {
      error = hresultMessage("GetFadeToBlackFullyBlack", result);
      return false;
    }
    fullyBlack = fullyBlackValue != false;
    inTransition = transitionValue != false;
    return true;
  }

private:
  bool synchronizeFadeToBlack(std::string &error) {
    if (!pendingFtb_) {
      return true;
    }
    if (!mixEffectBlock_) {
      error = "ATEM switcher is not connected.";
      return false;
    }

    SdkBool inTransition = false;
    HRESULT result =
        mixEffectBlock_->GetFadeToBlackInTransition(&inTransition);
    if (FAILED(result)) {
      error = hresultMessage("GetFadeToBlackInTransition", result);
      return false;
    }
    if (inTransition) {
      return true;
    }

    SdkBool fullyBlack = false;
    result = mixEffectBlock_->GetFadeToBlackFullyBlack(&fullyBlack);
    if (FAILED(result)) {
      error = hresultMessage("GetFadeToBlackFullyBlack", result);
      return false;
    }
    if ((fullyBlack != false) == desiredFtb_) {
      pendingFtb_ = false;
      return true;
    }

    result = mixEffectBlock_->PerformFadeToBlack();
    if (FAILED(result)) {
      error = hresultMessage("PerformFadeToBlack", result);
      return false;
    }
    return true;
  }

#ifdef _WIN32
  bool comInitialized_ = false;
#endif
  bool pendingFtb_ = false;
  bool desiredFtb_ = false;
  IBMDSwitcherDiscovery *discovery_ = nullptr;
  IBMDSwitcher *switcher_ = nullptr;
  IBMDSwitcherMixEffectBlock *mixEffectBlock_ = nullptr;
};

void *VTRPON_SWITCHER_ADDON_CALL createDevice() { return new AtemDevice; }

void VTRPON_SWITCHER_ADDON_CALL destroyDevice(void *instance) {
  delete static_cast<AtemDevice *>(instance);
}

int VTRPON_SWITCHER_ADDON_CALL openDevice(void *instance,
                                         const char *deviceAddressUtf8,
                                         char *errorUtf8,
                                         uint32_t errorCapacity) {
  if (!instance) {
    writeMessage(errorUtf8, errorCapacity, "ATEM add-on instance is null.");
    return 0;
  }
  std::string error;
  const bool succeeded =
      static_cast<AtemDevice *>(instance)->open(deviceAddressUtf8, error);
  writeMessage(errorUtf8, errorCapacity, error);
  return succeeded ? 1 : 0;
}

void VTRPON_SWITCHER_ADDON_CALL closeDevice(void *instance) {
  if (instance) {
    static_cast<AtemDevice *>(instance)->close();
  }
}

int VTRPON_SWITCHER_ADDON_CALL getProductName(
    void *instance, char *nameUtf8, uint32_t nameCapacity, char *errorUtf8,
    uint32_t errorCapacity) {
  if (!instance) {
    writeMessage(errorUtf8, errorCapacity, "ATEM add-on instance is null.");
    return 0;
  }
  std::string name;
  std::string error;
  const bool succeeded =
      static_cast<AtemDevice *>(instance)->productName(name, error);
  writeMessage(nameUtf8, nameCapacity, name);
  writeMessage(errorUtf8, errorCapacity, error);
  return succeeded ? 1 : 0;
}

int VTRPON_SWITCHER_ADDON_CALL getProgramInput(
    void *instance, int64_t *sourceNumber, char *errorUtf8,
    uint32_t errorCapacity) {
  if (!instance || !sourceNumber) {
    writeMessage(errorUtf8, errorCapacity,
                 "ATEM add-on instance or output is null.");
    return 0;
  }
  std::string error;
  const bool succeeded =
      static_cast<AtemDevice *>(instance)->programInput(*sourceNumber, error);
  writeMessage(errorUtf8, errorCapacity, error);
  return succeeded ? 1 : 0;
}

int VTRPON_SWITCHER_ADDON_CALL setProgramInput(
    void *instance, int64_t sourceNumber, char *errorUtf8,
    uint32_t errorCapacity) {
  if (!instance) {
    writeMessage(errorUtf8, errorCapacity, "ATEM add-on instance is null.");
    return 0;
  }
  std::string error;
  const bool succeeded = static_cast<AtemDevice *>(instance)->setProgramInput(
      sourceNumber, error);
  writeMessage(errorUtf8, errorCapacity, error);
  return succeeded ? 1 : 0;
}

int VTRPON_SWITCHER_ADDON_CALL setFadeToBlack(
    void *instance, int enabled, char *errorUtf8, uint32_t errorCapacity) {
  if (!instance) {
    writeMessage(errorUtf8, errorCapacity, "ATEM add-on instance is null.");
    return 0;
  }
  std::string error;
  const bool succeeded = static_cast<AtemDevice *>(instance)->setFadeToBlack(
      enabled != 0, error);
  writeMessage(errorUtf8, errorCapacity, error);
  return succeeded ? 1 : 0;
}

int VTRPON_SWITCHER_ADDON_CALL getFadeToBlackState(
    void *instance, int *fullyBlack, int *inTransition, char *errorUtf8,
    uint32_t errorCapacity) {
  if (!instance || !fullyBlack || !inTransition) {
    writeMessage(errorUtf8, errorCapacity,
                 "ATEM add-on instance or output is null.");
    return 0;
  }
  bool fullyBlackValue = false;
  bool inTransitionValue = false;
  std::string error;
  const bool succeeded =
      static_cast<AtemDevice *>(instance)->fadeToBlackState(
          fullyBlackValue, inTransitionValue, error);
  if (succeeded) {
    *fullyBlack = fullyBlackValue ? 1 : 0;
    *inTransition = inTransitionValue ? 1 : 0;
  }
  writeMessage(errorUtf8, errorCapacity, error);
  return succeeded ? 1 : 0;
}

const VtrPonSwitcherAddonApiV1 kApi{
    VTRPON_SWITCHER_ADDON_ABI_VERSION,
    sizeof(VtrPonSwitcherAddonApiV1),
    "vtr-pon-atem-addon",
    "Blackmagic ATEM",
    &createDevice,
    &destroyDevice,
    &openDevice,
    &closeDevice,
    &getProductName,
    &getProgramInput,
    &setProgramInput,
    &setFadeToBlack,
    &getFadeToBlackState,
};

} // namespace

extern "C" VTRPON_SWITCHER_ADDON_EXPORT const VtrPonSwitcherAddonApiV1 *
    VTRPON_SWITCHER_ADDON_CALL
    vtrpon_switcher_addon_get_api(uint32_t requestedAbiVersion) {
  return requestedAbiVersion == VTRPON_SWITCHER_ADDON_ABI_VERSION ? &kApi
                                                                  : nullptr;
}

extern "C" VTRPON_ADDON_RUNTIME_EXPORT int32_t
vtrpon_addon_runtime_status_v1(char *detail, uint32_t capacity) {
  if (detail && capacity) *detail = '\0';
#ifdef _WIN32
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    writeMessage(detail, capacity, hresultMessage("COM initialization", initialized));
    return VtrPonAddonRuntimeError;
  }
  IBMDSwitcherDiscovery *discovery = nullptr;
  const HRESULT result = CoCreateInstance(CLSID_CBMDSwitcherDiscovery, nullptr,
      CLSCTX_INPROC_SERVER, IID_IBMDSwitcherDiscovery,
      reinterpret_cast<void **>(&discovery));
  releaseCom(discovery);
  if (SUCCEEDED(initialized)) CoUninitialize();
  if (SUCCEEDED(result)) return VtrPonAddonRuntimeReady;
  writeMessage(detail, capacity, hresultMessage("ATEM runtime activation", result));
  return result == REGDB_E_CLASSNOTREG || result == CO_E_DLLNOTFOUND
      ? VtrPonAddonRuntimeMissing : VtrPonAddonRuntimeIncompatible;
#else
  // Creating discovery only: never call ConnectTo from a settings diagnostic.
  IBMDSwitcherDiscovery *discovery = CreateBMDSwitcherDiscoveryInstance();
  if (discovery) {
    releaseCom(discovery);
    return VtrPonAddonRuntimeReady;
  }
  struct stat info{};
  const bool installed = stat("/Library/Application Support/Blackmagic Design/Switchers/BMDSwitcherAPI.bundle", &info) == 0;
  writeMessage(detail, capacity, installed
      ? "Installed ATEM Software Control runtime is incompatible or could not be loaded."
      : "ATEM Software Control runtime is not installed.");
  return installed ? VtrPonAddonRuntimeIncompatible : VtrPonAddonRuntimeMissing;
#endif
}
