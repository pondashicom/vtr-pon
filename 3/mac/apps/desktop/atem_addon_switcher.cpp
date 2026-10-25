#include "platform_capabilities.h"
#include "atem_addon_switcher.h"

#include "switcher_addon_api.h"
#include "addon_platform.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstddef>

namespace {

#ifdef Q_OS_MACOS
constexpr auto kAddonFileName = "vtr-pon-atem-addon.dylib";
#else
constexpr auto kAddonFileName = "vtr-pon-atem-addon.dll";
#endif
constexpr auto kAtemAddonId = "vtr-pon-atem-addon";
constexpr std::size_t kMessageCapacity = 1024;

QString messageFromBuffer(const std::array<char, kMessageCapacity> &buffer,
                          const QString &fallback) {
  const auto length = std::find(buffer.begin(), buffer.end(), '\0') - buffer.begin();
  const QString message = QString::fromUtf8(buffer.data(), length).trimmed();
  return message.isEmpty() ? fallback : message;
}

} // namespace

namespace vtrpon {

AtemAddonSwitcher::AtemAddonSwitcher(bool receiveProgramInputChanges,
                                     QObject *parent)
    : SwitcherDevice(parent),
      receiveProgramInputChanges_(receiveProgramInputChanges) {
  pollTimer_.setInterval(250);
  connect(&pollTimer_, &QTimer::timeout, this,
          [this] { pollProgramInput(); });
}

AtemAddonSwitcher::~AtemAddonSwitcher() { close(); }

QString AtemAddonSwitcher::addonFilePath() {
  const QString overrideDirectory =
      qEnvironmentVariable("VTRPON_SWITCHER_ADDON_DIR").trimmed();
#ifdef Q_OS_MACOS
  if (overrideDirectory.isEmpty())
    return addons::addonFilePath(QCoreApplication::applicationDirPath(), QString::fromLatin1(kAtemAddonId));
#endif
  const QString addonDirectory =
      overrideDirectory.isEmpty()
          ? QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("switcher-addons"))
          : overrideDirectory;
  return QDir(addonDirectory).filePath(QString::fromLatin1(kAddonFileName));
}

bool AtemAddonSwitcher::addonInstalled() {
  if (!platformFeatureImplemented(PlatformFeature::Atem)) return false;
  // Installation changes take effect on the next application launch.
  static const bool installed = QFileInfo(addonFilePath()).isFile();
  return installed;
}

bool AtemAddonSwitcher::loadAddon(QString *errorDetail) {
  // This method runs on SwitcherWorker in the application. Signature checks
  // and vendor SDK calls must never block the Controller or on-air callbacks.
  if (!addons::verifyInstalledBinary(addonFilePath(), errorDetail)) return false;
  library_.setFileName(addonFilePath());
  if (!library_.load()) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("ATEM add-on could not be loaded: %1")
                         .arg(library_.errorString());
    }
    return false;
  }

  auto getApi = reinterpret_cast<VtrPonGetSwitcherAddonApiFn>(
      library_.resolve("vtrpon_switcher_addon_get_api"));
  if (!getApi) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("ATEM add-on entry point was not found.");
    }
    library_.unload();
    return false;
  }

  api_ = getApi(VTRPON_SWITCHER_ADDON_ABI_VERSION);
  const std::size_t requiredSize =
      offsetof(VtrPonSwitcherAddonApiV1, setFadeToBlack) +
      sizeof(VtrPonSwitcherAddonApiV1::setFadeToBlack);
  if (!api_ || api_->abiVersion != VTRPON_SWITCHER_ADDON_ABI_VERSION ||
      api_->structSize < requiredSize || !api_->addonId ||
      QByteArray(api_->addonId) != QByteArray(kAtemAddonId) || !api_->create ||
      !api_->destroy || !api_->open || !api_->close ||
      !api_->getProductName || !api_->getProgramInput ||
      !api_->setProgramInput || !api_->setFadeToBlack) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("ATEM add-on ABI is incompatible.");
    }
    api_ = nullptr;
    library_.unload();
    return false;
  }
  return true;
}

bool AtemAddonSwitcher::open(QString *errorDetail) {
  close();
  if (!addonInstalled()) {
    if (errorDetail) *errorDetail = QStringLiteral("ATEM add-on is not installed.");
    return false;
  }
  if (!loadAddon(errorDetail)) {
    return false;
  }

  instance_ = api_->create();
  if (!instance_) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("ATEM add-on could not create a device.");
    }
    close();
    return false;
  }

  std::array<char, kMessageCapacity> error{};
  if (!api_->open(instance_, "", error.data(),
                  static_cast<uint32_t>(error.size()))) {
    if (errorDetail) {
      *errorDetail = messageFromBuffer(
          error, QStringLiteral("ATEM USB connection failed."));
    }
    close();
    return false;
  }

  std::array<char, kMessageCapacity> productName{};
  error.fill(0);
  if (api_->getProductName(instance_, productName.data(),
                           static_cast<uint32_t>(productName.size()),
                           error.data(),
                           static_cast<uint32_t>(error.size()))) {
    productName_ = messageFromBuffer(productName, {});
  }

  if (!pollProgramInput(errorDetail)) return false;
  pollTimer_.start();
  return true;
}

void AtemAddonSwitcher::close() {
  pollTimer_.stop();
  if (api_ && instance_) {
    api_->close(instance_);
    api_->destroy(instance_);
  }
  instance_ = nullptr;
  api_ = nullptr;
  lastProgramInputNumber_ = -1;
  productName_.clear();
  if (library_.isLoaded()) {
    library_.unload();
  }
}

bool AtemAddonSwitcher::setProgramInput(int sourceNumber,
                                        QString *errorDetail) {
  if (!api_ || !instance_) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("ATEM add-on is not connected.");
    }
    return false;
  }
  std::array<char, kMessageCapacity> error{};
  if (!api_->setProgramInput(instance_, sourceNumber, error.data(),
                             static_cast<uint32_t>(error.size()))) {
    const auto detail = messageFromBuffer(error, QStringLiteral("ATEM Program input change failed."));
    if (errorDetail) *errorDetail = detail;
    failConnection(detail);
    return false;
  }
  return true;
}

bool AtemAddonSwitcher::setFadeToBlack(bool enabled, QString *errorDetail) {
  if (!api_ || !instance_) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("ATEM add-on is not connected.");
    }
    return false;
  }
  std::array<char, kMessageCapacity> error{};
  if (!api_->setFadeToBlack(instance_, enabled ? 1 : 0, error.data(),
                            static_cast<uint32_t>(error.size()))) {
    const auto detail = messageFromBuffer(error, QStringLiteral("ATEM Fade To Black change failed."));
    if (errorDetail) *errorDetail = detail;
    failConnection(detail);
    return false;
  }
  return true;
}

QString AtemAddonSwitcher::connectionDetail() const {
  return productName_.isEmpty() ? QStringLiteral("Ready")
                                : QStringLiteral("Ready: %1").arg(productName_);
}

bool AtemAddonSwitcher::pollProgramInput(QString *errorDetail) {
  if (!api_ || !instance_) {
    return false;
  }
  int64_t sourceNumber = 0;
  std::array<char, kMessageCapacity> error{};
  if (!api_->getProgramInput(instance_, &sourceNumber, error.data(),
                             static_cast<uint32_t>(error.size()))) {
    const auto detail = messageFromBuffer(error, QStringLiteral("ATEM Program input polling failed."));
    if (errorDetail) *errorDetail = detail;
    failConnection(detail);
    return false;
  }
  if (lastProgramInputNumber_ == sourceNumber) {
    return true;
  }
  lastProgramInputNumber_ = sourceNumber;
  if (receiveProgramInputChanges_ && sourceNumber >= 1 &&
      sourceNumber <= 40) {
    emit programInputChanged(static_cast<int>(sourceNumber));
  }
  return true;
}

void AtemAddonSwitcher::failConnection(const QString &detail) {
  close();
  emit connectionOpened(false, detail);
  emit errorOccurred(detail);
}

} // namespace vtrpon
