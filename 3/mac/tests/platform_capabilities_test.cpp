#include "platform_capabilities.h"
#include "controller_operations.h"
#include "atem_addon_switcher.h"
#include "network_video.h"
#include "decklink.h"
#include "vst_service.h"
#include "playlist_panel.h"
#include <QFile>
#include <QScopeGuard>
#include "controller_settings.h"
#include "gpu_configuration.h"
#include "video_output_settings.h"
#include "virtual_camera/output.h"
#include "operator_monitor_settings_page.h"
#include "addon_management_page.h"
#include "addon_package.h"
#include "view_menu_controller.h"
#include "switcher_control.h"
#include "controller_settings_dialog.h"
#include "playlist_model.h"
#include "playlist_item_operations.h"
#include <QComboBox>
#include <QCheckBox>
#include <QMenuBar>
#include <QAction>
#include <QPushButton>
#include <QDir>
#include <QTemporaryDir>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return false; } } while(false)
bool runPlatformCapabilitiesRegression() {
  using namespace vtrpon;
  CHECK(!platformFeatureImplemented(PlatformFeature::PowerPoint));
  CHECK(platformFeatureImplemented(PlatformFeature::ExternalAddons));
  CHECK(platformFeatureImplemented(PlatformFeature::Switcher));
  CHECK(platformFeatureImplemented(PlatformFeature::Atem));
  CHECK(platformFeatureImplemented(PlatformFeature::VirtualCamera));
  CHECK(platformFeatureImplemented(PlatformFeature::OperatorMonitor));
  CHECK(platformFeatureImplemented(PlatformFeature::AddonManagement));
  CHECK(platformFeatureImplemented(PlatformFeature::NetworkVideo));
  CHECK(platformFeatureImplemented(PlatformFeature::GpuPreferences) && platformFeatureImplemented(PlatformFeature::Output4k));
  QTemporaryDir fakeAddon;
  CHECK(fakeAddon.isValid());
  QFile fake(fakeAddon.filePath("windows-addon.dll")); CHECK(fake.open(QIODevice::WriteOnly));
  fake.write("Windows binary fixture"); fake.close();
  qputenv("VTRPON_DECKLINK_ADDON_FILE", fake.fileName().toUtf8());
  const auto clearOverride = qScopeGuard([] { qunsetenv("VTRPON_DECKLINK_ADDON_FILE"); });
  CHECK(!deckLinkAddonInstalled() && !networkVideoAddonInstalled() &&
        !AtemAddonSwitcher::addonInstalled() && !VstService::instance().available());
  auto settings = makeControllerSettings();
  settings.setValue("video/programOutput4k", true);
  settings.setValue("video/gpuAdapterId", "Windows adapter");
  settings.setValue("video/softwareDecoding", true);
  settings.setValue("switcher/rolandV8Hd/enabled", true);
  settings.sync();
  settings.setValue("audio/useVstPlugins", true);
  CHECK(platformFeatureImplemented(PlatformFeature::Vst3));
  CHECK(VstService::instance().setEnabled(false));
  settings.sync(); CHECK(!settings.value("audio/useVstPlugins").toBool());
  settings.setValue("networkVideo/outputEnabled", true);
  settings.setValue("networkVideo/outputName", "Windows NDI output");
  settings.setValue("deckLink/outputEnabled", true);
  settings.setValue("deckLink/outputChannel", "Windows SDI channel");
  settings.sync();
  const auto ndi = savedNetworkVideoSettings();
  saveNetworkVideoSettings({}); CHECK(savedNetworkVideoSettings() == NetworkVideoSettings{});
  saveNetworkVideoSettings(ndi); CHECK(savedNetworkVideoSettings() == ndi);
  const auto decklink = savedDeckLinkSettings();
  saveDeckLinkSettings({}); CHECK(savedDeckLinkSettings() == DeckLinkSettings{});
  saveDeckLinkSettings(decklink); CHECK(savedDeckLinkSettings() == decklink);
  CHECK(saveGpuPreferences(settings, {"other", false}));
  CHECK((readGpuPreferences(settings) == GpuPreferences{"other", false}));
  CHECK(save4kProgramOutput(settings, false) && !load4kProgramOutput(settings));
  CHECK(initializeProgramOutputResolution(true));
  CHECK(setVirtualCameraEnabled(true) && virtualCameraEnabled() && !virtualCameraStatus().isEmpty());
  CHECK(setVirtualCameraEnabled(false) && !virtualCameraEnabled());
  const auto switcher = loadSwitcherControlSettings();
  CHECK(saveSwitcherControlSettings({}) && loadSwitcherControlSettings() == SwitcherControlSettings{});
  CHECK(saveSwitcherControlSettings(switcher) && loadSwitcherControlSettings() == switcher);
  CHECK(saveSwitcherControlSettings({}));
  { SwitcherController controller(nullptr); CHECK(!controller.switcherReady()); }
  auto supportedAtem = switcher;
  supportedAtem.atemEnabled = true;
  CHECK(saveSwitcherControlSettings(supportedAtem));
  CHECK(loadSwitcherControlSettings().atemEnabled && !loadSwitcherControlSettings().rolandV8HdEnabled);
  CHECK(saveSwitcherControlSettings({}));
  {
    ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *roland = dialog.findChild<QCheckBox *>("rolandV8HdEnabledCheckBox");
    auto *atem = dialog.findChild<QWidget *>("atemSwitcherSettingsCard");
    CHECK(roland && roland->isEnabled() && atem && atem->isHidden());
    roland->setChecked(true);
    CHECK(loadSwitcherControlSettings().rolandV8HdEnabled);
    roland->setChecked(false);
    CHECK(!loadSwitcherControlSettings().rolandV8HdEnabled);
  }

  OperatorMonitorSettings monitor;
  monitor.screen = QStringLiteral("Windows display"); monitor.information = true;
  CHECK(saveOperatorMonitorSettings(settings, monitor));
  OperatorMonitorSettingsPage page;
  auto *displays = page.findChild<QComboBox *>("operatorMonitorDisplayCombo");
  auto *info = page.findChild<QCheckBox *>("operatorMonitorInformation");
  auto *timer = page.findChild<QCheckBox *>("operatorMonitorTimerEnabled");
  CHECK(displays && displays->isEnabled() && info && info->isEnabled() && timer && timer->isEnabled());
  info->setChecked(false); // This feature is now implemented; other imported values are kept.
  timer->setChecked(!monitor.timerEnabled);
  settings.sync();
  const auto saved = loadOperatorMonitorSettings(settings);
  CHECK(saved.screen == monitor.screen && !saved.information && saved.fields == monitor.fields);
  CHECK(saved.timerEnabled != monitor.timerEnabled);

  QWidget output, control;
  QMenuBar menu;
  ViewMenuController view(&menu, &output, &control);
  auto *action = menu.findChild<QAction *>("operatorMonitorAction");
  CHECK(action && action->isEnabled() && action->isCheckable());
  auto *widget = view.createSettingsWidget(&control);
  auto *resolution = widget->findChild<QComboBox *>("videoOutputResolutionComboBox");
  CHECK(resolution && resolution->isEnabled());
  resolution->setCurrentIndex(1);
  settings.sync(); CHECK(load4kProgramOutput(settings));
  resolution->setCurrentIndex(0);
  settings.sync(); CHECK(!load4kProgramOutput(settings));
  CHECK(initializeProgramOutputResolution()); // The live startup resolution stays frozen.

  AddonManagementPage addons;
  CHECK(addons.findChild<QPushButton *>("addonCatalogRefresh")->isEnabled());
  CHECK(addons.findChild<QPushButton *>("addonInstallLocal")->isEnabled());
  CHECK(addons.findChild<QPushButton *>("addonInstall3"));
  CHECK(!addons.findChild<QPushButton *>("addonInstall3")->isEnabled());
  CHECK(!addons.findChild<QPushButton *>("addonInstall0")->isEnabled());
  QTemporaryDir root; CHECK(root.isValid());
  QString error;
  CHECK(!addons::queueInstall(root.path(), "missing.vtrponaddon", {}, {}, {}, {}, &error));
  CHECK(!error.isEmpty());
  CHECK(!addons::queueUninstall(root.path(), "atem", &error));
  CHECK(QDir(root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
  CHECK(addons::applyJob(root.filePath("pending.job.json"), 0, &error) == 0);
  CHECK(!addons::requestRestart(root.path(), {}, {}, &error));
  CHECK(QDir(root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
  PlaylistModel model;
  auto native = model.snapshot();
  PlaylistItemState item;
  item.id = 1; item.itemNumber = 1; item.playlistNumber = 1;
  item.filePath = root.filePath("Windows.pptx"); item.displayName = "Windows slides";
  item.sourceType = PlaylistSourceType::PowerPoint;
  item.mediaType = PlaylistMediaType::Presentation;
  item.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
  native.items = {item}; native.nextItemId = 2;
  CHECK(model.restoreSnapshot(native));
  ControllerOperations operations;
  PlaylistItemOperations itemOperations(&model, &operations);
  for (auto action : {PlaylistItemAction::OnAir, PlaylistItemAction::Edit, PlaylistItemAction::Dsk})
    CHECK(!itemOperations.allowItemAction(1, action));
  CHECK(itemOperations.allowItemAction(1, PlaylistItemAction::CopyItem));
  CHECK(itemOperations.copyItemState(1));
  ControllerOperations panelOperations;
  PlaylistPanel playlist(&model, nullptr, false, nullptr, false, &panelOperations);
  CHECK(panelOperations.execute(ControllerOperations::Action::SelectItem, quint64(1)).invoked());
  CHECK(playlist.selectedItemId() == 1);
  CHECK(playlist.findChild<QPushButton *>("playlistOnAirButton") &&
        !playlist.findChild<QPushButton *>("playlistOnAirButton")->isEnabled());
  return true;
}
