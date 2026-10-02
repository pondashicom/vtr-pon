#pragma once

#include <QDialog>
#include <QColor>

class AudioInputDeviceMonitor;
class AudioOutputDeviceMonitor;
class QCheckBox;
class QComboBox;
class QEvent;
class QGroupBox;
class QFontComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSettings;
class QShowEvent;
class QSpinBox;
class QSlider;
class QStackedWidget;
class QTimer;
class QTableWidget;
class TestPatternSettingsPage;
class OperatorMonitorSettingsPage;
class NetworkVideoSettingsPage; class DeckLinkSettingsPage;
class QVBoxLayout;
class UvcAudioMappingStore;
class UvcDeviceMonitor;
enum class PlaylistEditActivationMode : int;

namespace vtrpon {
class AddonRuntimeStatusWidget;
struct SwitcherControlSettings;
class SwitcherController;
class UiLanguageManager;
PlaylistEditActivationMode
loadPlaylistEditActivationMode(const QSettings &settings);
bool loadAutoSelectNextItemAfterOnAir(const QSettings &settings);
bool loadPreferAudioAlbumArt(const QSettings &settings);
bool savePlaylistSettings(QSettings &settings,
                          PlaylistEditActivationMode editActivationMode,
                          bool autoSelectNextItemAfterOnAir,
                          bool preferAudioAlbumArt);
bool loadAllowSameItemOnAirRetake(const QSettings &settings);
bool loadOnAirToggleMode(const QSettings &settings);
bool loadShowPlaybackRateControls(const QSettings &settings);
bool loadDisableFtbButton(const QSettings &settings);
bool saveOnAirSettings(QSettings &settings,
                       bool allowSameItemOnAirRetake,
                       bool showPlaybackRateControls, bool disableFtbButton,
                       bool toggleMode = false);
bool loadShowBgmPlayerControls(const QSettings &settings);
bool saveAudioSettings(QSettings &settings, bool showBgmPlayerControls);
}

class FileManagementSettingsPage;
class AddonManagementPage;

class ControllerSettingsDialog final : public QDialog {
  Q_OBJECT

public:
  ControllerSettingsDialog(UvcDeviceMonitor *uvcDeviceMonitor,
                           AudioInputDeviceMonitor *audioDeviceMonitor,
                           UvcAudioMappingStore *uvcAudioMappingStore,
                           vtrpon::SwitcherController *switcherController,
                           vtrpon::UiLanguageManager *languageManager,
                           QWidget *parent = nullptr);

  static PlaylistEditActivationMode savedPlaylistEditActivationMode();
  static bool savedAutoSelectNextItemAfterOnAir();
  static bool savedPreferAudioAlbumArt();
  static bool savedAllowSameItemOnAirRetake();
  static bool savedOnAirToggleMode();
  bool onAirToggleMode() const;
  static bool savedShowPlaybackRateControls();
  static bool savedDisableFtbButton();
  static bool savedShowBgmPlayerControls();
  static QColor savedPlayerBackgroundColor();
  static vtrpon::SwitcherControlSettings savedSwitcherControlSettings();
  PlaylistEditActivationMode playlistEditActivationMode() const;
  bool autoSelectNextItemAfterOnAir() const;
  bool preferAudioAlbumArt() const;
  bool allowSameItemOnAirRetake() const;
  bool showPlaybackRateControls() const;
  bool disableFtbButton() const;
  bool showBgmPlayerControls() const;
  void setProgramOutputState(const QString &appliedId, const QString &statusKey);
  void showAudioOutputPage();
  void setTimeSynchronizationState(bool busy, const QString &status = {});
  QWidget *videoOutputPage() const;
  void addVideoOutputControls(QWidget *controls);
  void reloadSavedSettings();
  void setSettingsFileStatus(const QString &text);
  void refreshSavedAudioOutputs();
  vtrpon::SwitcherControlSettings switcherControlSettings() const;

signals:
  void addonRestartRequested();
  void operatorMonitorSettingsChanged();
  void operatorTimerSettingsRequested();
  void timeSettingsChanged(bool remainingDefaultChanged);
  void timeSynchronizationRequested();
  void shortcutsChanged();
  void testPatternsChanged();
  void settingsReplaced();
  void exportSettingsRequested();
  void importSettingsRequested();
  void showBgmPlayerControlsChanged(bool visible);
  void audioThumbnailPreferenceChanged(bool preferAudioAlbumArt);
  void programOutputDeviceChanged(const QString &id);
  void editPflOutputDeviceChanged(const QString &id);
  void switcherControlSettingsChanged(
      const vtrpon::SwitcherControlSettings &settings);

protected:
  void showEvent(QShowEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  FileManagementSettingsPage *fileManagementPage_ = nullptr;
  AddonManagementPage *addonManagementPage_ = nullptr;
  OperatorMonitorSettingsPage *operatorMonitorPage_ = nullptr;
  QWidget *virtualCameraPage_ = nullptr;
  QCheckBox *virtualCameraCheckBox_ = nullptr;
  QLabel *virtualCameraDescription_ = nullptr;
  QLabel *virtualCameraStatus_ = nullptr;
  QLabel *virtualCameraSaveStatus_ = nullptr;
  QCheckBox *activityNoticeCheckBox_ = nullptr;
  QLabel *activityNoticeDescription_ = nullptr;
  QLabel *updatesPageTitle_ = nullptr;
  QLabel *updatesPageDescription_ = nullptr;
  QCheckBox *automaticUpdateCheckBox_ = nullptr;
  QLabel *automaticUpdateDescription_ = nullptr;
  void reloadTimeSettings();
  void saveTimeSettings();
  void reloadShortcutEditors();
  void saveShortcutSettings();
  void refreshGpuSettings();
  void saveGpuSettings();
  void applySelectedSkin();
  void applySelectedLanguage();
  void savePlaylistSettings();
  void saveOnAirSettings();
  void saveAudioSettings();
  void saveSwitcherControlSettings();
  void updateSwitcherConnectionState(bool ready, const QString &detail);
  void refreshSwitcherConnectionIndicators(bool ready,
                                            const QString &detail);
  void retranslateUi();
  void refreshUvcAudioMappings();
  void saveUvcAudioMapping(QComboBox *comboBox);
  void setStatusText(const QString &text, bool error);
  void setMappingStatusText(const QString &text, bool error);
  void refreshAudioOutputs();
  void updateAudioOutputWarning();
  void saveAudioOutputSelection(bool program);
  void showCurrentNavigationPage();
  int navigationRow(const QString &id) const;

  UvcDeviceMonitor *uvcDeviceMonitor_ = nullptr;
  AudioInputDeviceMonitor *audioDeviceMonitor_ = nullptr;
  UvcAudioMappingStore *uvcAudioMappingStore_ = nullptr;
  vtrpon::SwitcherController *switcherController_ = nullptr;
  vtrpon::UiLanguageManager *languageManager_ = nullptr;
  QLabel *timePageTitle_ = nullptr;
  QLabel *timeDescription_ = nullptr;
  QLabel *timeZoneLabel_ = nullptr;
  QLabel *timeRemainingLabel_ = nullptr;
  QLabel *timeFontLabel_ = nullptr;
  QLabel *timeSizeLabel_ = nullptr;
  QLabel *timeStatus_ = nullptr;
  QComboBox *timeZoneCombo_ = nullptr;
  QComboBox *timeRemainingCombo_ = nullptr;
  QFontComboBox *timeFontCombo_ = nullptr;
  QSpinBox *timeSizeSpin_ = nullptr;
  QPushButton *timeSyncButton_ = nullptr;
  QListWidget *navigationList_ = nullptr;
  QStackedWidget *pageStack_ = nullptr;
  NetworkVideoSettingsPage *networkVideoPage_ = nullptr;
  DeckLinkSettingsPage *deckLinkPage_ = nullptr;
  vtrpon::AddonRuntimeStatusWidget *atemRuntimeStatus_ = nullptr;
  TestPatternSettingsPage *testPatternPage_ = nullptr;
  QLabel *settingsTitle_ = nullptr;
  QLabel *shortcutsPageTitle_ = nullptr;
  QLabel *shortcutsDescription_ = nullptr;
  QLabel *shortcutsStatus_ = nullptr;
  QTableWidget *shortcutsTable_ = nullptr;
  QPushButton *saveShortcutsButton_ = nullptr;
  QPushButton *resetShortcutsButton_ = nullptr;
  void applyTextSize(int size);
  QLabel *accessibilityDescription_ = nullptr;
  QLabel *textSizeLabel_ = nullptr;
  QLabel *textSizePreview_ = nullptr;
  QLabel *textSizeStatus_ = nullptr;
  QSlider *textSizeSlider_ = nullptr;
  QTimer *textSizeApplyTimer_ = nullptr;
  QLabel *textSizeSteps_[3]{};
  QLabel *appearancePageTitle_ = nullptr;
  QLabel *appearancePageDescription_ = nullptr;
  QLabel *skinFieldLabel_ = nullptr;
  QComboBox *skinComboBox_ = nullptr;
  QPushButton *applySkinButton_ = nullptr;
  QLabel *appliedSkinLabel_ = nullptr;
  QLabel *audioPageTitle_ = nullptr;
  QLabel *audioPageDescription_ = nullptr;
  AudioOutputDeviceMonitor *outputDeviceMonitor_ = nullptr;
  QLabel *outputPageTitle_ = nullptr;
  QLabel *outputPageDescription_ = nullptr;
  QLabel *programOutputLabel_ = nullptr;
  QLabel *pflOutputLabel_ = nullptr;
  QComboBox *programOutputComboBox_ = nullptr;
  QComboBox *pflOutputComboBox_ = nullptr;
  QCheckBox *defaultAudioFallbackCheckBox_ = nullptr;
  QCheckBox *useVstPluginsCheckBox_ = nullptr;
  QLabel *vstAddonStatusLabel_ = nullptr;
  QLabel *outputWarningLabel_ = nullptr;
  QLabel *programOutputStateLabel_ = nullptr;
  QLabel *outputSaveStatusLabel_ = nullptr;
  QString selectedProgramOutputId_;
  QString selectedPflOutputId_;
  QString appliedProgramOutputId_;
  QString programOutputStatusKey_;
  QVBoxLayout *mappingRowsLayout_ = nullptr;
  QLabel *mappingStatusLabel_ = nullptr;
  QLabel *bgmPlayerPageTitle_ = nullptr;
  QLabel *bgmPlayerPageDescription_ = nullptr;
  QCheckBox *showBgmPlayerControlsCheckBox_ = nullptr;
  QLabel *bgmPlayerStatusLabel_ = nullptr;
  QLabel *switcherPageTitle_ = nullptr;
  QLabel *switcherPageDescription_ = nullptr;
  QLabel *switcherExclusiveLabel_ = nullptr;
  QCheckBox *rolandV8HdEnabledCheckBox_ = nullptr;
  QLabel *rolandV8HdSourceLabel_ = nullptr;
  QSpinBox *rolandV8HdSourceSpinBox_ = nullptr;
  QCheckBox *rolandTakeProgramWhenOnAirCheckBox_ = nullptr;
  QCheckBox *rolandTakeOnAirWhenProgramSelectedCheckBox_ = nullptr;
  QCheckBox *rolandFtbFollowsProgramCheckBox_ = nullptr;
  QLabel *rolandConnectionLabel_ = nullptr;
  QCheckBox *atemEnabledCheckBox_ = nullptr;
  QLabel *atemSourceLabel_ = nullptr;
  QSpinBox *atemSourceSpinBox_ = nullptr;
  QCheckBox *atemTakeProgramWhenOnAirCheckBox_ = nullptr;
  QCheckBox *atemTakeOnAirWhenProgramSelectedCheckBox_ = nullptr;
  QCheckBox *atemFtbFollowsProgramCheckBox_ = nullptr;
  QLabel *atemAddonStatusLabel_ = nullptr;
  QLabel *atemConnectionLabel_ = nullptr;
  QLabel *switcherStatusLabel_ = nullptr;
  QLabel *languagePageDescription_ = nullptr;
  QLabel *languageFieldLabel_ = nullptr;
  QComboBox *languageComboBox_ = nullptr;
  QLabel *languageStatusLabel_ = nullptr;
  QLabel *playlistPageTitle_ = nullptr;
  QLabel *playlistPageDescription_ = nullptr;
  QLabel *playlistEditModeFieldLabel_ = nullptr;
  QComboBox *playlistEditModeComboBox_ = nullptr;
  QCheckBox *autoSelectNextItemCheckBox_ = nullptr;
  QLabel *playlistAudioThumbnailFieldLabel_ = nullptr;
  QComboBox *playlistAudioThumbnailComboBox_ = nullptr;
  QLabel *playlistStatusLabel_ = nullptr;
  QLabel *onAirPageTitle_ = nullptr;
  QLabel *onAirPageDescription_ = nullptr;
  QCheckBox *onAirToggleModeCheckBox_ = nullptr;
  QCheckBox *allowSameItemOnAirRetakeCheckBox_ = nullptr;
  QCheckBox *showPlaybackRateControlsCheckBox_ = nullptr;
  QCheckBox *disableFtbButtonCheckBox_ = nullptr;
  QLabel *onAirStatusLabel_ = nullptr;
  QWidget *videoOutputPage_ = nullptr;
  QVBoxLayout *videoOutputPageLayout_ = nullptr;
  QLabel *videoOutputPageTitle_ = nullptr;
  QLabel *videoOutputPageDescription_ = nullptr;
  QGroupBox *gpuGroupBox_ = nullptr;
  QLabel *gpuFieldLabel_ = nullptr;
  QComboBox *gpuComboBox_ = nullptr;
  QCheckBox *softwareDecodingCheckBox_ = nullptr;
  QLabel *gpuRuntimeLabel_ = nullptr;
  QLabel *gpuRestartLabel_ = nullptr;
  QLabel *gpuSaveStatusLabel_ = nullptr;
  QLabel *resetPageTitle_ = nullptr;
  QLabel *resetPageDescription_ = nullptr;
  QLabel *resetStatusLabel_ = nullptr;
  QPushButton *resetButton_ = nullptr;
  QPushButton *exportSettingsButton_ = nullptr;
  QPushButton *importSettingsButton_ = nullptr;
  QPushButton *closeButton_ = nullptr;
};
