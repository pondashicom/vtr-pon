#include "controller_settings.h"
#include "file_management_settings_page.h"
#include "addon_management_page.h"
#include "gpu_configuration.h"
#include <QGroupBox>
#include "test_pattern_settings_page.h"
#include "controller_settings_dialog.h"
#include "settings_style.h"
#include "vst_service.h"
#include "time_settings.h"
#include "operator_monitor_settings_page.h"
#include "virtual_camera/output.h"
#include "network_video_settings_page.h"
#include "atem_addon_switcher.h"
#include <QTimer>
#include <QFontComboBox>
#include <QFormLayout>
#include "controller_settings_io.h"
#include "controller_shortcut_settings.h"
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <memory>
#include <algorithm>
#include <vector>
#include "audio_output_settings.h"

#include "controller_scaling.h"
#include "skins/controller_skin.h"
#include "language_manager.h"
#include "language_text.h"
#include "playlist_panel.h"
#include "switcher_control.h"
#include "uvc_audio_mapping.h"
#include "uvc_device_monitor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QVBoxLayout>
#include <QWidget>

namespace {

const QString kPlaylistEditActivationModeKey =
    QStringLiteral("playlist/editActivationMode");
const QString kAutoSelectNextItemAfterOnAirKey =
    QStringLiteral("playlist/autoSelectNextItemAfterOnAir");
const QString kAllowSameItemOnAirRetakeKey =
    QStringLiteral("onAir/allowSameItemRetake");
const QString kShowPlaybackRateControlsKey =
    QStringLiteral("onAir/showPlaybackRateControls");
const QString kDisableFtbButtonKey =
    QStringLiteral("onAir/disableFtbButton");
const QString kShowBgmPlayerControlsKey =
    QStringLiteral("audio/showBgmPlayerControls");
constexpr int kNavigationIdRole = Qt::UserRole;
constexpr int kNavigationPageRole = Qt::UserRole + 1;
constexpr int kNavigationTextIdRole = Qt::UserRole + 2;

class SettingsNavigationDelegate final : public QStyledItemDelegate {
public:
  explicit SettingsNavigationDelegate(QObject *parent = nullptr)
      : QStyledItemDelegate(parent) {}

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (hasSeparatorBefore(index)) {
      size.rheight() += vtrpon::scalePx(10);
    }
    return size;
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem itemOption(option);
    if (hasSeparatorBefore(index)) {
      const int gap = vtrpon::scalePx(10);
      QColor separator = option.palette.color(QPalette::Text);
      separator.setAlpha(42);
      painter->save();
      painter->setPen(separator);
      const int y = option.rect.top() + gap / 2;
      painter->drawLine(option.rect.left() + vtrpon::scalePx(8), y,
                        option.rect.right() - vtrpon::scalePx(8), y);
      painter->restore();
      itemOption.rect.adjust(0, gap, 0, 0);
    }
    QStyledItemDelegate::paint(painter, itemOption, index);
  }

private:
  static bool hasSeparatorBefore(const QModelIndex &index) {
    const auto id = index.data(kNavigationIdRole).toString();
    return id == QLatin1StringView("video_output") ||
           id == QLatin1StringView("audio_output") ||
           id == QLatin1StringView("appearance");
  }
};

QSettings controllerSettings() {
  return vtrpon::makeControllerSettings();
}

QWidget *createPage(QWidget *parent, const QString &titleText,
                    const QString &descriptionText,
                    QLabel **titleLabel, QLabel **descriptionLabel,
                    QVBoxLayout **pageLayout) {
  auto *page = new QWidget(parent);
  page->setObjectName(QStringLiteral("settingsPage"));
  vtrpon::setSettingsRole(page, "page");
  page->setAttribute(Qt::WA_StyledBackground, true);
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(vtrpon::scalePx(8));

  auto *title = new QLabel(titleText, page);
  title->setObjectName(QStringLiteral("settingsPageTitle"));
  vtrpon::setSettingsRole(title, "pageTitle");
  layout->addWidget(title);
  auto *description = new QLabel(descriptionText, page);
  description->setObjectName(QStringLiteral("settingsDescription"));
  vtrpon::setSettingsRole(description, "description");
  description->setWordWrap(true);
  layout->addWidget(description);
  layout->addSpacing(vtrpon::scalePx(12));
  if (titleLabel) {
    *titleLabel = title;
  }
  if (descriptionLabel) {
    *descriptionLabel = description;
  }
  *pageLayout = layout;
  return page;
}

QString skinDisplayName(const vtrpon::ControllerSkinDefinition &skin) {
  if (skin.key == QStringLiteral("hardware")) {
    return vtrpon::uiText("settings.appearance.skin.hardware");
  }
  if (skin.key == QStringLiteral("material-dark")) {
    return vtrpon::uiText("settings.appearance.skin.material_dark");
  }
  if (skin.key == QStringLiteral("vtr-pon-clasic")) {
    return vtrpon::uiText("settings.appearance.skin.vtrpon_clasic");
  }
  return skin.displayName;
}

} // namespace

namespace vtrpon {

PlaylistEditActivationMode
loadPlaylistEditActivationMode(const QSettings &settings) {
  const int savedMode =
      settings
          .value(kPlaylistEditActivationModeKey,
                 static_cast<int>(PlaylistEditActivationMode::EditButton))
          .toInt();
  return savedMode == static_cast<int>(PlaylistEditActivationMode::Selection)
             ? PlaylistEditActivationMode::Selection
             : PlaylistEditActivationMode::EditButton;
}

bool loadAutoSelectNextItemAfterOnAir(const QSettings &settings) {
  return settings.value(kAutoSelectNextItemAfterOnAirKey, false).toBool();
}

bool savePlaylistSettings(QSettings &settings,
                          PlaylistEditActivationMode editActivationMode,
                          bool autoSelectNextItemAfterOnAir) {
  settings.setValue(kPlaylistEditActivationModeKey,
                    static_cast<int>(editActivationMode));
  settings.setValue(kAutoSelectNextItemAfterOnAirKey,
                    autoSelectNextItemAfterOnAir);
  settings.sync();
  return settings.status() == QSettings::NoError;
}

bool loadAllowSameItemOnAirRetake(const QSettings &settings) {
  return !loadOnAirToggleMode(settings) && settings.value(kAllowSameItemOnAirRetakeKey, false).toBool();
}

bool loadOnAirToggleMode(const QSettings &settings) {
  return settings.value(QStringLiteral("onAir/toggleMode"), false).toBool();
}

bool loadShowPlaybackRateControls(const QSettings &settings) {
  return settings.value(kShowPlaybackRateControlsKey, true).toBool();
}

bool loadDisableFtbButton(const QSettings &settings) {
  return settings.value(kDisableFtbButtonKey, false).toBool();
}

bool saveOnAirSettings(QSettings &settings,
                       bool allowSameItemOnAirRetake,
                       bool showPlaybackRateControls, bool disableFtbButton,
                       bool toggleMode) {
  settings.setValue(QStringLiteral("onAir/toggleMode"), toggleMode);
  settings.setValue(kAllowSameItemOnAirRetakeKey,
                    allowSameItemOnAirRetake && !toggleMode);
  settings.setValue(kShowPlaybackRateControlsKey,
                    showPlaybackRateControls);
  settings.setValue(kDisableFtbButtonKey, disableFtbButton);
  settings.sync();
  return settings.status() == QSettings::NoError;
}

bool loadShowBgmPlayerControls(const QSettings &settings) {
  return settings.value(kShowBgmPlayerControlsKey, false).toBool();
}

bool saveAudioSettings(QSettings &settings, bool showBgmPlayerControls) {
  settings.setValue(kShowBgmPlayerControlsKey, showBgmPlayerControls);
  settings.sync();
  return settings.status() == QSettings::NoError;
}

} // namespace vtrpon

ControllerSettingsDialog::ControllerSettingsDialog(
    UvcDeviceMonitor *uvcDeviceMonitor,
    AudioInputDeviceMonitor *audioDeviceMonitor,
    UvcAudioMappingStore *uvcAudioMappingStore,
    vtrpon::SwitcherController *switcherController,
    vtrpon::UiLanguageManager *languageManager, QWidget *parent)
    : QDialog(parent), uvcDeviceMonitor_(uvcDeviceMonitor),
      audioDeviceMonitor_(audioDeviceMonitor),
      uvcAudioMappingStore_(uvcAudioMappingStore),
      switcherController_(switcherController),
      languageManager_(languageManager) {
  setObjectName(QStringLiteral("controllerSettingsDialog"));
  setWindowTitle(vtrpon::uiText("settings.title"));
  setWindowModality(Qt::WindowModal);
  setModal(true);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setAttribute(Qt::WA_StyledBackground, true);
  setPalette(vtrpon::controllerSkinPalette(*qApp,
      vtrpon::ControllerSkinPalette::VtrPonClasic));
  setMinimumSize(vtrpon::scalePx(720), vtrpon::scalePx(480));
  resize(vtrpon::scalePx(780), vtrpon::scalePx(540));
  setStyleSheet(vtrpon::settingsStyleSheet());

  auto *rootLayout = new QHBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  auto *navigationPane = new QWidget(this);
  navigationPane->setObjectName(QStringLiteral("settingsNavigationPane"));
  navigationPane->setAttribute(Qt::WA_StyledBackground, true);
  navigationPane->setFixedWidth(vtrpon::scaleTextPx(190));
  auto *navigationLayout = new QVBoxLayout(navigationPane);
  navigationLayout->setContentsMargins(vtrpon::scalePx(20),
                                       vtrpon::scalePx(22),
                                       vtrpon::scalePx(12),
                                       vtrpon::scalePx(20));
  navigationLayout->setSpacing(vtrpon::scalePx(18));

  settingsTitle_ = new QLabel(navigationPane);
  settingsTitle_->setObjectName(QStringLiteral("settingsTitle"));
  vtrpon::setSettingsRole(settingsTitle_, "title");
  navigationLayout->addWidget(settingsTitle_);

  navigationList_ = new QListWidget(navigationPane);
  navigationList_->setObjectName(QStringLiteral("settingsNavigationList"));
  navigationList_->setAccessibleName(
      vtrpon::uiText("settings.categories.accessible"));
  navigationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  navigationList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  navigationList_->setSelectionMode(QAbstractItemView::SingleSelection);
  navigationList_->setItemDelegate(
      new SettingsNavigationDelegate(navigationList_));
  navigationLayout->addWidget(navigationList_, 1);
  rootLayout->addWidget(navigationPane);

  auto *contentPane = new QWidget(this);
  contentPane->setObjectName(QStringLiteral("settingsContentPane"));
  contentPane->setAttribute(Qt::WA_StyledBackground, true);
  auto *contentLayout = new QVBoxLayout(contentPane);
  contentLayout->setContentsMargins(vtrpon::scalePx(32),
                                    vtrpon::scalePx(26),
                                    vtrpon::scalePx(28),
                                    vtrpon::scalePx(22));
  contentLayout->setSpacing(vtrpon::scalePx(12));

  pageStack_ = new QStackedWidget(contentPane);
  auto *pageStack = pageStack_;
  QVBoxLayout *shortcutsLayout = nullptr;
  auto *shortcutsPage = createPage(pageStack, {}, {}, &shortcutsPageTitle_,
                                  &shortcutsDescription_, &shortcutsLayout);
  shortcutsLayout = vtrpon::addSettingsGroup(shortcutsLayout, "settings.category.shortcuts", true);
  shortcutsTable_ = new QTableWidget(vtrpon::shortcutSettings::definitions().size(), 3, shortcutsPage);
  shortcutsTable_->setObjectName(QStringLiteral("shortcutBindingsTable"));
  shortcutsTable_->verticalHeader()->hide();
  shortcutsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  shortcutsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  shortcutsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  shortcutsTable_->setSelectionMode(QAbstractItemView::NoSelection);
  for (int row = 0; row < shortcutsTable_->rowCount(); ++row) {
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsEnabled);
    shortcutsTable_->setItem(row, 0, item);
    for (int column = 1; column <= 2; ++column) {
      auto *editor = new QKeySequenceEdit(shortcutsTable_);
      editor->setMaximumSequenceLength(1);
      editor->setClearButtonEnabled(true);
      editor->setMinimumWidth(vtrpon::scalePx(130));
      editor->setObjectName(QStringLiteral("shortcut_%1_%2")
          .arg(vtrpon::shortcutSettings::definitions().at(vtrpon::shortcutSettings::displayOrder().at(row)).id).arg(column));
      shortcutsTable_->setCellWidget(row, column, editor);
    }
    shortcutsTable_->setRowHeight(row, vtrpon::scalePx(36));
  }
  shortcutsLayout->addWidget(shortcutsTable_, 1);
  shortcutsStatus_ = new QLabel(shortcutsPage);
  shortcutsStatus_->setObjectName(QStringLiteral("languageStatusLabel"));
  vtrpon::setSettingsRole(shortcutsStatus_, "status");
  shortcutsStatus_->setWordWrap(true);
  shortcutsLayout->addWidget(shortcutsStatus_);
  auto *shortcutButtons = new QHBoxLayout;
  resetShortcutsButton_ = new QPushButton(shortcutsPage);
  resetShortcutsButton_->setObjectName(QStringLiteral("resetShortcutsButton"));
  saveShortcutsButton_ = new QPushButton(shortcutsPage);
  saveShortcutsButton_->setObjectName(QStringLiteral("saveShortcutsButton"));
  vtrpon::setSettingsRole(saveShortcutsButton_, "primaryButton");
  shortcutButtons->addWidget(resetShortcutsButton_);
  shortcutButtons->addStretch();
  shortcutButtons->addWidget(saveShortcutsButton_);
  shortcutsLayout->addLayout(shortcutButtons);
  reloadShortcutEditors();
  connect(saveShortcutsButton_, &QPushButton::clicked, this, &ControllerSettingsDialog::saveShortcutSettings);
  connect(resetShortcutsButton_, &QPushButton::clicked, this, [this] {
    const auto defaults = vtrpon::shortcutSettings::defaults();
    for (int row = 0; row < shortcutsTable_->rowCount(); ++row) {
      const auto keys = vtrpon::shortcutSettings::sequences(defaults, vtrpon::shortcutSettings::displayOrder().at(row));
      for (int column = 1; column <= 2; ++column)
        static_cast<QKeySequenceEdit *>(shortcutsTable_->cellWidget(row, column))->setKeySequence(keys.value(column - 1));
    }
    shortcutsStatus_->setText(vtrpon::uiText("settings.shortcuts.pending"));
  });

  QVBoxLayout *appearancePageLayout = nullptr;
  QWidget *appearancePage = createPage(
      pageStack, QString(), QString(), &appearancePageTitle_,
      &appearancePageDescription_,
      &appearancePageLayout);
  auto *appearanceCard = vtrpon::createSettingsGroup(appearancePage, "settings.category.appearance");
  appearanceCard->setObjectName(QStringLiteral("appearanceSettingsCard"));
  appearanceCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *appearanceCardLayout = new QVBoxLayout(appearanceCard);
  appearanceCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  appearanceCardLayout->setSpacing(vtrpon::scalePx(10));

  skinFieldLabel_ = new QLabel(appearanceCard);
  skinFieldLabel_->setObjectName(QStringLiteral("skinFieldLabel"));
  vtrpon::setSettingsRole(skinFieldLabel_, "fieldLabel");
  appearanceCardLayout->addWidget(skinFieldLabel_);
  auto *selectionLayout = new QHBoxLayout;
  selectionLayout->setContentsMargins(0, 0, 0, 0);
  selectionLayout->setSpacing(vtrpon::scalePx(10));
  skinComboBox_ = new QComboBox(appearanceCard);
  skinComboBox_->setObjectName(QStringLiteral("controllerSkinComboBox"));
  skinComboBox_->setAccessibleName(
      vtrpon::uiText("settings.appearance.skin"));
  for (const vtrpon::ControllerSkinDefinition &skin :
       vtrpon::availableControllerSkins()) {
    skinComboBox_->addItem(skinDisplayName(skin), skin.key);
  }
  const int currentSkinIndex =
      skinComboBox_->findData(vtrpon::currentControllerSkinKey());
  skinComboBox_->setCurrentIndex(currentSkinIndex >= 0 ? currentSkinIndex : 0);
  selectionLayout->addWidget(skinComboBox_, 1);
  applySkinButton_ = new QPushButton(appearanceCard);
  applySkinButton_->setObjectName(QStringLiteral("applySkinButton"));
  vtrpon::setSettingsRole(applySkinButton_, "primaryButton");
  selectionLayout->addWidget(applySkinButton_);
  appearanceCardLayout->addLayout(selectionLayout);
  appliedSkinLabel_ = new QLabel(appearanceCard);
  appliedSkinLabel_->setObjectName(QStringLiteral("appliedSkinLabel"));
  vtrpon::setSettingsRole(appliedSkinLabel_, "status");
  appearanceCardLayout->addWidget(appliedSkinLabel_);
  appearancePageLayout->addWidget(appearanceCard);

  auto *accessibilityCard = vtrpon::createSettingsGroup(
      appearancePage, "settings.category.accessibility");
  accessibilityCard->setObjectName(
      QStringLiteral("accessibilitySettingsCard"));
  accessibilityCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *accessibilityLayout = new QVBoxLayout(accessibilityCard);
  accessibilityLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  accessibilityLayout->setSpacing(vtrpon::scalePx(10));
  accessibilityDescription_ = new QLabel(accessibilityCard);
  vtrpon::setSettingsRole(accessibilityDescription_, "description");
  accessibilityDescription_->setWordWrap(true);
  accessibilityLayout->addWidget(accessibilityDescription_);
  textSizeLabel_ = new QLabel(accessibilityCard);
  vtrpon::setSettingsRole(textSizeLabel_, "fieldLabel");
  accessibilityLayout->addWidget(textSizeLabel_);
  textSizePreview_ = new QLabel(accessibilityCard);
  textSizePreview_->setObjectName(QStringLiteral("uiTextSizePreview"));
  vtrpon::setSettingsRole(textSizePreview_, "preview");
  textSizePreview_->setWordWrap(true);
  textSizePreview_->setMinimumHeight(vtrpon::scalePx(90));
  accessibilityLayout->addWidget(textSizePreview_);
  textSizeSlider_ = new QSlider(Qt::Horizontal, accessibilityCard);
  textSizeSlider_->setObjectName(QStringLiteral("uiTextSizeSlider"));
  textSizeSlider_->setRange(0, 2);
  textSizeSlider_->setSingleStep(1);
  textSizeSlider_->setPageStep(1);
  textSizeSlider_->setTickInterval(1);
  textSizeSlider_->setTickPosition(QSlider::TicksBelow);
  textSizeSlider_->setValue(vtrpon::uiTextSize());
  textSizeLabel_->setBuddy(textSizeSlider_);
  accessibilityLayout->addWidget(textSizeSlider_);
  auto *textSizeSteps = new QHBoxLayout;
  for (int i = 0; i < 3; ++i) {
    textSizeSteps_[i] = new QLabel(accessibilityCard);
    textSizeSteps->addWidget(
        textSizeSteps_[i], 1,
        i == 0 ? Qt::AlignLeft
               : i == 1 ? Qt::AlignHCenter : Qt::AlignRight);
  }
  accessibilityLayout->addLayout(textSizeSteps);
  textSizeStatus_ = new QLabel(accessibilityCard);
  vtrpon::setSettingsRole(textSizeStatus_, "status");
  textSizeStatus_->setWordWrap(true);
  accessibilityLayout->addWidget(textSizeStatus_);
  appearancePageLayout->addWidget(accessibilityCard);
  appearancePageLayout->addStretch(1);

  // Let the handle paint before restyling the entire application. While the
  // handle is held, defer all expensive work until after the drag finishes.
  textSizeApplyTimer_ = new QTimer(this);
  textSizeApplyTimer_->setSingleShot(true);
  textSizeApplyTimer_->setInterval(150);
  connect(textSizeSlider_, &QSlider::sliderPressed, textSizeApplyTimer_,
          &QTimer::stop);
  connect(textSizeSlider_, &QSlider::valueChanged, this, [this] {
    textSizeApplyTimer_->stop();
    if (!textSizeSlider_->isSliderDown()) {
      textSizeApplyTimer_->start();
    }
  });
  connect(textSizeSlider_, &QSlider::sliderReleased, this, [this] {
    textSizeApplyTimer_->start();
  });
  connect(textSizeApplyTimer_, &QTimer::timeout, this, [this] {
    if (textSizeSlider_->isSliderDown()) {
      return;
    }
    const int size = textSizeSlider_->value();
    if (size == vtrpon::uiTextSize()) {
      return;
    }
    auto settings = controllerSettings();
    settings.setValue("accessibility/textSize", size);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
      const QSignalBlocker blocker(textSizeSlider_);
      textSizeSlider_->setValue(vtrpon::uiTextSize());
      textSizeStatus_->setText(
          vtrpon::uiText("settings.accessibility.save_failed"));
      return;
    }
    textSizeStatus_->clear();
    applyTextSize(size);
  });

  QVBoxLayout *playlistPageLayout = nullptr;
  QWidget *playlistPage = createPage(
      pageStack, QString(), QString(), &playlistPageTitle_,
      &playlistPageDescription_, &playlistPageLayout);
  auto *playlistCard = vtrpon::createSettingsGroup(playlistPage, "settings.category.playlist");
  playlistCard->setObjectName(QStringLiteral("playlistSettingsCard"));
  playlistCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *playlistCardLayout = new QVBoxLayout(playlistCard);
  playlistCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  playlistCardLayout->setSpacing(vtrpon::scalePx(12));
  playlistEditModeFieldLabel_ = new QLabel(playlistCard);
  playlistEditModeFieldLabel_->setObjectName(
      QStringLiteral("playlistEditModeFieldLabel"));
  vtrpon::setSettingsRole(playlistEditModeFieldLabel_, "fieldLabel");
  playlistCardLayout->addWidget(playlistEditModeFieldLabel_);
  playlistEditModeComboBox_ = new QComboBox(playlistCard);
  playlistEditModeComboBox_->setObjectName(
      QStringLiteral("playlistEditModeComboBox"));
  playlistEditModeComboBox_->addItem(
      QString(), static_cast<int>(PlaylistEditActivationMode::EditButton));
  playlistEditModeComboBox_->addItem(
      QString(), static_cast<int>(PlaylistEditActivationMode::Selection));
  const int savedEditModeIndex = playlistEditModeComboBox_->findData(
      static_cast<int>(savedPlaylistEditActivationMode()));
  playlistEditModeComboBox_->setCurrentIndex(
      savedEditModeIndex >= 0 ? savedEditModeIndex : 0);
  playlistCardLayout->addWidget(playlistEditModeComboBox_);
  autoSelectNextItemCheckBox_ = new QCheckBox(playlistCard);
  autoSelectNextItemCheckBox_->setObjectName(
      QStringLiteral("playlistAutoSelectNextItemCheckBox"));
  autoSelectNextItemCheckBox_->setChecked(
      savedAutoSelectNextItemAfterOnAir());
  playlistCardLayout->addWidget(autoSelectNextItemCheckBox_);
  playlistStatusLabel_ = new QLabel(playlistCard);
  playlistStatusLabel_->setObjectName(QStringLiteral("playlistStatusLabel"));
  vtrpon::setSettingsRole(playlistStatusLabel_, "status");
  playlistStatusLabel_->setWordWrap(true);
  playlistCardLayout->addWidget(playlistStatusLabel_);
  playlistPageLayout->addWidget(playlistCard);
  playlistPageLayout->addStretch(1);

  QVBoxLayout *onAirPageLayout = nullptr;
  QWidget *onAirPage = createPage(
      pageStack, QString(), QString(), &onAirPageTitle_,
      &onAirPageDescription_, &onAirPageLayout);
  auto *onAirCard = vtrpon::createSettingsGroup(onAirPage, "settings.category.on_air");
  onAirCard->setObjectName(QStringLiteral("onAirSettingsCard"));
  onAirCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *onAirCardLayout = new QVBoxLayout(onAirCard);
  onAirCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  onAirCardLayout->setSpacing(vtrpon::scalePx(12));
  allowSameItemOnAirRetakeCheckBox_ = new QCheckBox(onAirCard);
  allowSameItemOnAirRetakeCheckBox_->setObjectName(
      QStringLiteral("allowSameItemOnAirRetakeCheckBox"));
  allowSameItemOnAirRetakeCheckBox_->setChecked(
      savedAllowSameItemOnAirRetake());
  onAirCardLayout->addWidget(allowSameItemOnAirRetakeCheckBox_);
  onAirToggleModeCheckBox_ = new QCheckBox(onAirCard);
  onAirToggleModeCheckBox_->setObjectName(QStringLiteral("onAirToggleModeCheckBox"));
  onAirToggleModeCheckBox_->setChecked(savedOnAirToggleMode());
  onAirCardLayout->addWidget(onAirToggleModeCheckBox_);
  showPlaybackRateControlsCheckBox_ = new QCheckBox(onAirCard);
  showPlaybackRateControlsCheckBox_->setObjectName(
      QStringLiteral("showPlaybackRateControlsCheckBox"));
  showPlaybackRateControlsCheckBox_->setChecked(
      savedShowPlaybackRateControls());
  onAirCardLayout->addWidget(showPlaybackRateControlsCheckBox_);
  disableFtbButtonCheckBox_ = new QCheckBox(onAirCard);
  disableFtbButtonCheckBox_->setObjectName(
      QStringLiteral("disableFtbButtonCheckBox"));
  disableFtbButtonCheckBox_->setChecked(savedDisableFtbButton());
  onAirCardLayout->addWidget(disableFtbButtonCheckBox_);
  onAirStatusLabel_ = new QLabel(onAirCard);
  onAirStatusLabel_->setObjectName(QStringLiteral("onAirStatusLabel"));
  vtrpon::setSettingsRole(onAirStatusLabel_, "status");
  onAirStatusLabel_->setWordWrap(true);
  onAirCardLayout->addWidget(onAirStatusLabel_);
  onAirPageLayout->addWidget(onAirCard);
  onAirPageLayout->addStretch(1);

  QVBoxLayout *audioPageLayout = nullptr;
  QWidget *audioPage = createPage(
      pageStack, QString(), QString(), &audioPageTitle_,
      &audioPageDescription_,
      &audioPageLayout);
  auto *mappingCard = vtrpon::createSettingsGroup(audioPage, "settings.category.audio");
  mappingCard->setObjectName(QStringLiteral("uvcAudioMappingCard"));
  mappingCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *mappingCardLayout = new QVBoxLayout(mappingCard);
  mappingCardLayout->setContentsMargins(
      vtrpon::scalePx(12), vtrpon::scalePx(12), vtrpon::scalePx(8),
      vtrpon::scalePx(12));
  mappingCardLayout->setSpacing(vtrpon::scalePx(8));
  auto *mappingScrollArea = new QScrollArea(mappingCard);
  mappingScrollArea->setObjectName(
      QStringLiteral("uvcAudioMappingScrollArea"));
  mappingScrollArea->setWidgetResizable(true);
  mappingScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *mappingRows = new QWidget(mappingScrollArea);
  mappingRows->setObjectName(QStringLiteral("uvcAudioMappingRows"));
  mappingRowsLayout_ = new QVBoxLayout(mappingRows);
  mappingRowsLayout_->setContentsMargins(vtrpon::scalePx(8),
                                         vtrpon::scalePx(6),
                                         vtrpon::scalePx(10),
                                         vtrpon::scalePx(6));
  mappingRowsLayout_->setSpacing(vtrpon::scalePx(12));
  mappingScrollArea->setWidget(mappingRows);
  mappingCardLayout->addWidget(mappingScrollArea, 1);
  mappingStatusLabel_ = new QLabel(mappingCard);
  mappingStatusLabel_->setObjectName(
      QStringLiteral("uvcAudioMappingStatusLabel"));
  vtrpon::setSettingsRole(mappingStatusLabel_, "status");
  mappingStatusLabel_->setWordWrap(true);
  mappingCardLayout->addWidget(mappingStatusLabel_);
  audioPageLayout->addWidget(mappingCard, 1);

  selectedProgramOutputId_ = vtrpon::savedProgramOutputDeviceId();
  selectedPflOutputId_ = vtrpon::savedEditPflOutputDeviceId();
  appliedProgramOutputId_ = selectedProgramOutputId_;
  programOutputStatusKey_ = QStringLiteral("settings.output.applied");
  QVBoxLayout *outputLayout = nullptr;
  auto *outputPage = createPage(pageStack, QString(), QString(),
      &outputPageTitle_, &outputPageDescription_, &outputLayout);
  auto *outputCard = vtrpon::createSettingsGroup(outputPage, "settings.category.audio_output");
  outputCard->setObjectName(QStringLiteral("audioOutputSettingsCard"));
  outputCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *outputCardLayout = new QVBoxLayout(outputCard);
  outputCardLayout->setContentsMargins(vtrpon::scalePx(20), vtrpon::scalePx(18),
      vtrpon::scalePx(20), vtrpon::scalePx(18));
  outputCardLayout->setSpacing(vtrpon::scalePx(12));
  programOutputLabel_ = new QLabel(outputCard);
  vtrpon::setSettingsRole(programOutputLabel_, "fieldLabel");
  programOutputComboBox_ = new QComboBox(outputCard);
  programOutputComboBox_->setObjectName(QStringLiteral("programOutputDeviceComboBox"));
  programOutputLabel_->setBuddy(programOutputComboBox_);
  pflOutputLabel_ = new QLabel(outputCard);
  vtrpon::setSettingsRole(pflOutputLabel_, "fieldLabel");
  pflOutputComboBox_ = new QComboBox(outputCard);
  pflOutputComboBox_->setObjectName(QStringLiteral("editPflOutputDeviceComboBox"));
  pflOutputLabel_->setBuddy(pflOutputComboBox_);
  outputWarningLabel_ = new QLabel(outputCard);
  outputWarningLabel_->setObjectName(QStringLiteral("audioOutputSameDeviceWarning"));
  outputWarningLabel_->setWordWrap(true);
  vtrpon::setSettingsRole(outputWarningLabel_, "error");
  programOutputStateLabel_ = new QLabel(outputCard);
  programOutputStateLabel_->setObjectName(QStringLiteral("programOutputDeviceState"));
  vtrpon::setSettingsRole(programOutputStateLabel_, "status");
  programOutputStateLabel_->setWordWrap(true);
  outputSaveStatusLabel_ = new QLabel(outputCard);
  outputSaveStatusLabel_->setWordWrap(true);
  vtrpon::setSettingsRole(outputSaveStatusLabel_, "error");
  outputCardLayout->addWidget(programOutputLabel_);
  outputCardLayout->addWidget(programOutputComboBox_);
  outputCardLayout->addWidget(pflOutputLabel_);
  outputCardLayout->addWidget(pflOutputComboBox_);
  defaultAudioFallbackCheckBox_ = new QCheckBox(outputCard);
  defaultAudioFallbackCheckBox_->setObjectName(QStringLiteral("defaultAudioFallbackCheckBox"));
  defaultAudioFallbackCheckBox_->setChecked(vtrpon::savedUseDefaultAudioOutputWhenMissing());
  outputCardLayout->addWidget(defaultAudioFallbackCheckBox_);
  connect(defaultAudioFallbackCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (!vtrpon::saveUseDefaultAudioOutputWhenMissing(enabled)) {
      const QSignalBlocker blocker(defaultAudioFallbackCheckBox_);
      defaultAudioFallbackCheckBox_->setChecked(!enabled);
      outputSaveStatusLabel_->setText(vtrpon::uiText("settings.output.save_failed"));
    } else outputSaveStatusLabel_->clear();
  });
  outputCardLayout->addWidget(outputWarningLabel_);
  outputCardLayout->addWidget(programOutputStateLabel_);
  outputCardLayout->addWidget(outputSaveStatusLabel_);
  outputLayout->addWidget(outputCard);

  auto *vstCard = vtrpon::createSettingsGroup(
      outputPage, "settings.group.vst");
  vstCard->setObjectName(QStringLiteral("vstSettingsCard"));
  vstCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *vstCardLayout = new QVBoxLayout(vstCard);
  vstCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  vstCardLayout->setSpacing(vtrpon::scalePx(10));
  useVstPluginsCheckBox_ = new QCheckBox(vstCard);
  useVstPluginsCheckBox_->setObjectName(
      QStringLiteral("useVstPluginsCheckBox"));
  useVstPluginsCheckBox_->setEnabled(
      vtrpon::VstService::instance().available());
  useVstPluginsCheckBox_->setChecked(vtrpon::vstModeEnabled());
  vstCardLayout->addWidget(useVstPluginsCheckBox_);
  vstAddonStatusLabel_ = new QLabel(vstCard);
  vstAddonStatusLabel_->setObjectName(QStringLiteral("vstAddonStatusLabel"));
  vtrpon::setSettingsRole(vstAddonStatusLabel_, "status");
  vstAddonStatusLabel_->setWordWrap(true);
  vstCardLayout->addWidget(vstAddonStatusLabel_);
  const bool vstAvailable = vtrpon::VstService::instance().available();
  useVstPluginsCheckBox_->setVisible(vstAvailable);
  vstAddonStatusLabel_->setVisible(vstAvailable);
  vstCard->setVisible(vstAvailable);
  outputLayout->addWidget(vstCard);
  outputLayout->addStretch();
  connect(useVstPluginsCheckBox_, &QCheckBox::toggled, this,
          [this](bool enabled) {
            if (!vtrpon::VstService::instance().setEnabled(enabled)) {
              const QSignalBlocker blocker(useVstPluginsCheckBox_);
              useVstPluginsCheckBox_->setChecked(vtrpon::vstModeEnabled());
              outputSaveStatusLabel_->setText(
                  vtrpon::uiText("settings.output.save_failed"));
            } else {
              outputSaveStatusLabel_->clear();
            }
          });
  outputDeviceMonitor_ = new AudioOutputDeviceMonitor(this);
  connect(outputDeviceMonitor_, &AudioOutputDeviceMonitor::devicesChanged,
      this, &ControllerSettingsDialog::refreshAudioOutputs);
  connect(programOutputComboBox_, &QComboBox::currentIndexChanged,
      this, [this] { saveAudioOutputSelection(true); });
  connect(pflOutputComboBox_, &QComboBox::currentIndexChanged,
      this, [this] { saveAudioOutputSelection(false); });

  QVBoxLayout *bgmPlayerPageLayout = nullptr;
  QWidget *bgmPlayerPage = createPage(
      pageStack, QString(), QString(), &bgmPlayerPageTitle_,
      &bgmPlayerPageDescription_, &bgmPlayerPageLayout);
  auto *bgmPlayerCard = vtrpon::createSettingsGroup(bgmPlayerPage, "settings.category.bgm_player");
  bgmPlayerCard->setObjectName(QStringLiteral("bgmPlayerSettingsCard"));
  bgmPlayerCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *bgmPlayerCardLayout = new QVBoxLayout(bgmPlayerCard);
  bgmPlayerCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  bgmPlayerCardLayout->setSpacing(vtrpon::scalePx(12));
  showBgmPlayerControlsCheckBox_ = new QCheckBox(bgmPlayerCard);
  showBgmPlayerControlsCheckBox_->setObjectName(
      QStringLiteral("showBgmPlayerControlsCheckBox"));
  showBgmPlayerControlsCheckBox_->setChecked(savedShowBgmPlayerControls());
  bgmPlayerCardLayout->addWidget(showBgmPlayerControlsCheckBox_);
  bgmPlayerStatusLabel_ = new QLabel(bgmPlayerCard);
  bgmPlayerStatusLabel_->setObjectName(
      QStringLiteral("bgmPlayerStatusLabel"));
  vtrpon::setSettingsRole(bgmPlayerStatusLabel_, "status");
  bgmPlayerStatusLabel_->setWordWrap(true);
  bgmPlayerCardLayout->addWidget(bgmPlayerStatusLabel_);
  bgmPlayerPageLayout->addWidget(bgmPlayerCard);
  bgmPlayerPageLayout->addStretch(1);

  QVBoxLayout *switcherPageLayout = nullptr;
  QWidget *switcherPage = createPage(
      pageStack, QString(), QString(), &switcherPageTitle_,
      &switcherPageDescription_, &switcherPageLayout);
  switcherExclusiveLabel_ = new QLabel(switcherPage);
  switcherExclusiveLabel_->setObjectName(
      QStringLiteral("switcherExclusiveLabel"));
  vtrpon::setSettingsRole(switcherExclusiveLabel_, "warning");
  switcherExclusiveLabel_->setWordWrap(true);
  switcherExclusiveLabel_->setVisible(vtrpon::AtemAddonSwitcher::addonInstalled());
  switcherPageLayout->addWidget(switcherExclusiveLabel_);
  auto *switcherCard = vtrpon::createSettingsGroup(switcherPage, "settings.group.roland");
  switcherCard->setObjectName(QStringLiteral("switcherSettingsCard"));
  switcherCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *switcherCardLayout = new QVBoxLayout(switcherCard);
  switcherCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  switcherCardLayout->setSpacing(vtrpon::scalePx(12));
  const vtrpon::SwitcherControlSettings switcherSettings =
      savedSwitcherControlSettings();
  rolandV8HdEnabledCheckBox_ = new QCheckBox(switcherCard);
  rolandV8HdEnabledCheckBox_->setObjectName(
      QStringLiteral("rolandV8HdEnabledCheckBox"));
  rolandV8HdEnabledCheckBox_->setChecked(
      switcherSettings.rolandV8HdEnabled && !switcherSettings.atemEnabled);
  switcherCardLayout->addWidget(rolandV8HdEnabledCheckBox_);
  auto *sourceLayout = new QHBoxLayout;
  sourceLayout->setContentsMargins(0, 0, 0, 0);
  sourceLayout->setSpacing(vtrpon::scalePx(10));
  rolandV8HdSourceLabel_ = new QLabel(switcherCard);
  rolandV8HdSourceLabel_->setObjectName(
      QStringLiteral("rolandV8HdSourceLabel"));
  vtrpon::setSettingsRole(rolandV8HdSourceLabel_, "fieldLabel");
  sourceLayout->addWidget(rolandV8HdSourceLabel_);
  rolandV8HdSourceSpinBox_ = new QSpinBox(switcherCard);
  rolandV8HdSourceSpinBox_->setObjectName(
      QStringLiteral("rolandV8HdSourceSpinBox"));
  rolandV8HdSourceSpinBox_->setRange(1, 8);
  rolandV8HdSourceSpinBox_->setValue(
      switcherSettings.rolandV8HdSourceNumber);
  sourceLayout->addWidget(rolandV8HdSourceSpinBox_);
  sourceLayout->addStretch(1);
  switcherCardLayout->addLayout(sourceLayout);
  rolandTakeProgramWhenOnAirCheckBox_ = new QCheckBox(switcherCard);
  rolandTakeProgramWhenOnAirCheckBox_->setObjectName(
      QStringLiteral("rolandTakeProgramWhenOnAirCheckBox"));
  rolandTakeProgramWhenOnAirCheckBox_->setChecked(
      switcherSettings.takeProgramWhenOnAir);
  switcherCardLayout->addWidget(rolandTakeProgramWhenOnAirCheckBox_);
  rolandTakeOnAirWhenProgramSelectedCheckBox_ = new QCheckBox(switcherCard);
  rolandTakeOnAirWhenProgramSelectedCheckBox_->setObjectName(
      QStringLiteral("rolandTakeOnAirWhenProgramSelectedCheckBox"));
  rolandTakeOnAirWhenProgramSelectedCheckBox_->setChecked(
      switcherSettings.takeOnAirWhenProgramSelected);
  switcherCardLayout->addWidget(
      rolandTakeOnAirWhenProgramSelectedCheckBox_);
  rolandFtbFollowsProgramCheckBox_ = new QCheckBox(switcherCard);
  rolandFtbFollowsProgramCheckBox_->setObjectName(
      QStringLiteral("rolandFtbFollowsProgramCheckBox"));
  rolandFtbFollowsProgramCheckBox_->setChecked(
      switcherSettings.ftbFollowsProgram);
  switcherCardLayout->addWidget(rolandFtbFollowsProgramCheckBox_);
  rolandConnectionLabel_ = new QLabel(switcherCard);
  rolandConnectionLabel_->setObjectName(
      QStringLiteral("rolandConnectionLabel"));
  vtrpon::setSettingsRole(rolandConnectionLabel_, "connection");
  switcherCardLayout->addWidget(rolandConnectionLabel_);
  switcherPageLayout->addWidget(switcherCard);

  auto *atemCard = vtrpon::createSettingsGroup(switcherPage, "settings.group.atem");
  atemCard->setObjectName(QStringLiteral("atemSwitcherSettingsCard"));
  atemCard->setVisible(vtrpon::AtemAddonSwitcher::addonInstalled());
  atemCard->setEnabled(vtrpon::AtemAddonSwitcher::addonInstalled());
  atemCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *atemCardLayout = new QVBoxLayout(atemCard);
  atemCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  atemCardLayout->setSpacing(vtrpon::scalePx(12));
  atemEnabledCheckBox_ = new QCheckBox(atemCard);
  atemEnabledCheckBox_->setObjectName(QStringLiteral("atemEnabledCheckBox"));
  atemEnabledCheckBox_->setChecked(switcherSettings.atemEnabled);
  atemCardLayout->addWidget(atemEnabledCheckBox_);
  auto *atemSourceLayout = new QHBoxLayout;
  atemSourceLayout->setContentsMargins(0, 0, 0, 0);
  atemSourceLayout->setSpacing(vtrpon::scalePx(10));
  atemSourceLabel_ = new QLabel(atemCard);
  atemSourceLabel_->setObjectName(QStringLiteral("atemSourceLabel"));
  vtrpon::setSettingsRole(atemSourceLabel_, "fieldLabel");
  atemSourceLayout->addWidget(atemSourceLabel_);
  atemSourceSpinBox_ = new QSpinBox(atemCard);
  atemSourceSpinBox_->setObjectName(QStringLiteral("atemSourceSpinBox"));
  atemSourceSpinBox_->setRange(1, 40);
  atemSourceSpinBox_->setValue(switcherSettings.atemSourceNumber);
  atemSourceLayout->addWidget(atemSourceSpinBox_);
  atemSourceLayout->addStretch(1);
  atemCardLayout->addLayout(atemSourceLayout);
  atemTakeProgramWhenOnAirCheckBox_ = new QCheckBox(atemCard);
  atemTakeProgramWhenOnAirCheckBox_->setObjectName(
      QStringLiteral("atemTakeProgramWhenOnAirCheckBox"));
  atemTakeProgramWhenOnAirCheckBox_->setChecked(
      switcherSettings.atemTakeProgramWhenOnAir);
  atemCardLayout->addWidget(atemTakeProgramWhenOnAirCheckBox_);
  atemTakeOnAirWhenProgramSelectedCheckBox_ = new QCheckBox(atemCard);
  atemTakeOnAirWhenProgramSelectedCheckBox_->setObjectName(
      QStringLiteral("atemTakeOnAirWhenProgramSelectedCheckBox"));
  atemTakeOnAirWhenProgramSelectedCheckBox_->setChecked(
      switcherSettings.atemTakeOnAirWhenProgramSelected);
  atemCardLayout->addWidget(atemTakeOnAirWhenProgramSelectedCheckBox_);
  atemFtbFollowsProgramCheckBox_ = new QCheckBox(atemCard);
  atemFtbFollowsProgramCheckBox_->setObjectName(
      QStringLiteral("atemFtbFollowsProgramCheckBox"));
  atemFtbFollowsProgramCheckBox_->setChecked(
      switcherSettings.atemFtbFollowsProgram);
  atemCardLayout->addWidget(atemFtbFollowsProgramCheckBox_);
  atemAddonStatusLabel_ = new QLabel(atemCard);
  atemAddonStatusLabel_->setObjectName(QStringLiteral("switcherStatusLabel"));
  vtrpon::setSettingsRole(atemAddonStatusLabel_, "status");
  atemAddonStatusLabel_->setWordWrap(true);
  atemCardLayout->addWidget(atemAddonStatusLabel_);
  atemRuntimeStatus_ = new vtrpon::AddonRuntimeStatusWidget(vtrpon::AddonRuntimeKind::Atem,
      vtrpon::AtemAddonSwitcher::addonFilePath(), atemCard);
  atemCardLayout->addWidget(atemRuntimeStatus_);
  atemConnectionLabel_ = new QLabel(atemCard);
  atemConnectionLabel_->setObjectName(
      QStringLiteral("atemConnectionLabel"));
  vtrpon::setSettingsRole(atemConnectionLabel_, "connection");
  atemCardLayout->addWidget(atemConnectionLabel_);
  switcherPageLayout->addWidget(atemCard);

  switcherStatusLabel_ = new QLabel(switcherPage);
  switcherStatusLabel_->setObjectName(QStringLiteral("switcherStatusLabel"));
  vtrpon::setSettingsRole(switcherStatusLabel_, "status");
  switcherStatusLabel_->setWordWrap(true);
  switcherPageLayout->addWidget(switcherStatusLabel_);
  switcherPageLayout->addStretch(1);

  QVBoxLayout *resetPageLayout = nullptr;
  QWidget *resetPage = createPage(pageStack, QString(), QString(),
      &resetPageTitle_, &resetPageDescription_, &resetPageLayout);
  resetPageLayout = vtrpon::addSettingsGroup(resetPageLayout, "settings.reset.title");
  resetButton_ = new QPushButton(resetPage);
  resetButton_->setObjectName(QStringLiteral("resetSettingsButton"));
  resetButton_->setMinimumHeight(vtrpon::scalePx(34));
  resetPageLayout->addWidget(resetButton_, 0, Qt::AlignLeft);
  resetPageLayout->addSpacing(vtrpon::scalePx(16));
  exportSettingsButton_ = new QPushButton(resetPage);
  exportSettingsButton_->setObjectName(QStringLiteral("exportSettingsButton"));
  importSettingsButton_ = new QPushButton(resetPage);
  importSettingsButton_->setObjectName(QStringLiteral("importSettingsButton"));
  for (auto *button : {exportSettingsButton_, importSettingsButton_}) {
    button->setMinimumHeight(vtrpon::scalePx(34));
    resetPageLayout->addWidget(button, 0, Qt::AlignLeft);
  }
  connect(exportSettingsButton_, &QPushButton::clicked, this,
          &ControllerSettingsDialog::exportSettingsRequested);
  connect(importSettingsButton_, &QPushButton::clicked, this,
          &ControllerSettingsDialog::importSettingsRequested);
  resetStatusLabel_ = new QLabel(resetPage);
  vtrpon::setSettingsRole(resetStatusLabel_, "status");
  resetStatusLabel_->setWordWrap(true);
  resetPageLayout->addWidget(resetStatusLabel_);
  resetPageLayout->addStretch(1);
  connect(resetButton_, &QPushButton::clicked, this, [this] {
    // Applying the default skin/font can change QFontComboBox before reload.
    const QSignalBlocker timeFontBlocker(timeFontCombo_);
    auto settings = controllerSettings();
    if (!vtrpon::replaceControllerSettings(settings, {})) {
      resetStatusLabel_->setText(vtrpon::uiText("settings.manage.failed"));
      return;
    }
    emit settingsReplaced();
    reloadSavedSettings();
    resetStatusLabel_->setText(vtrpon::uiText("settings.reset.succeeded"));
  });
  QVBoxLayout *updatesPageLayout = nullptr;
  QWidget *updatesPage = createPage(pageStack, QString(), QString(),
      &updatesPageTitle_, &updatesPageDescription_, &updatesPageLayout);
  auto *updateCard = vtrpon::createSettingsGroup(
      updatesPage, "settings.updates.check_title");
  auto *updateLayout = new QVBoxLayout(updateCard);
  updateLayout->setContentsMargins(vtrpon::scalePx(20), vtrpon::scalePx(18),
                                  vtrpon::scalePx(20), vtrpon::scalePx(18));
  automaticUpdateCheckBox_ = new QCheckBox(updateCard);
  automaticUpdateCheckBox_->setObjectName(QStringLiteral("automaticUpdateCheckBox"));
  automaticUpdateCheckBox_->setChecked(controllerSettings()
      .value(QStringLiteral("updates/checkAutomatically"), true).toBool());
  updateLayout->addWidget(automaticUpdateCheckBox_);
  automaticUpdateDescription_ = new QLabel(updateCard);
  automaticUpdateDescription_->setWordWrap(true);
  updateLayout->addWidget(automaticUpdateDescription_);
  updatesPageLayout->addWidget(updateCard);
  connect(automaticUpdateCheckBox_, &QCheckBox::toggled, this, [](bool enabled) {
    auto settings = controllerSettings();
    settings.setValue(QStringLiteral("updates/checkAutomatically"), enabled);
    settings.sync();
  });

  auto *noticeCard = vtrpon::createSettingsGroup(
      updatesPage, "settings.privacy.title");
  auto *noticeLayout = new QVBoxLayout(noticeCard);
  noticeLayout->setContentsMargins(vtrpon::scalePx(20), vtrpon::scalePx(18),
                                  vtrpon::scalePx(20), vtrpon::scalePx(18));
  activityNoticeCheckBox_ = new QCheckBox(noticeCard);
  activityNoticeCheckBox_->setObjectName(QStringLiteral("activityNoticeCheckBox"));
  activityNoticeCheckBox_->setChecked(controllerSettings()
      .value(QStringLiteral("privacy/sendActivityNotice"), false).toBool());
  noticeLayout->addWidget(activityNoticeCheckBox_);
  activityNoticeDescription_ = new QLabel(noticeCard);
  activityNoticeDescription_->setWordWrap(true);
  noticeLayout->addWidget(activityNoticeDescription_);
  updatesPageLayout->addWidget(noticeCard);
  updatesPageLayout->addStretch(1);
  connect(activityNoticeCheckBox_, &QCheckBox::toggled, this, [](bool enabled) {
    auto settings = controllerSettings();
    settings.setValue(QStringLiteral("privacy/sendActivityNotice"), enabled);
    settings.sync();
  });
  videoOutputPage_ = createPage(pageStack, QString(), QString(),
      &videoOutputPageTitle_, &videoOutputPageDescription_, &videoOutputPageLayout_);
  gpuGroupBox_ = new QGroupBox(QStringLiteral("GPU"), videoOutputPage_);
  gpuGroupBox_->setObjectName(QStringLiteral("gpuSettingsGroup"));
  auto *gpuLayout = new QVBoxLayout(gpuGroupBox_);
  gpuLayout->setSpacing(vtrpon::scalePx(10));
  gpuFieldLabel_ = new QLabel(gpuGroupBox_);
  vtrpon::setSettingsRole(gpuFieldLabel_, "fieldLabel");
  gpuComboBox_ = new QComboBox(gpuGroupBox_);
  gpuComboBox_->setObjectName(QStringLiteral("gpuAdapterComboBox"));
  gpuFieldLabel_->setBuddy(gpuComboBox_);
  softwareDecodingCheckBox_ = new QCheckBox(gpuGroupBox_);
  softwareDecodingCheckBox_->setObjectName(QStringLiteral("softwareDecodingCheckBox"));
  gpuRuntimeLabel_ = new QLabel(gpuGroupBox_);
  gpuRuntimeLabel_->setObjectName(QStringLiteral("gpuRuntimeLabel"));
  vtrpon::setSettingsRole(gpuRuntimeLabel_, "status");
  gpuRuntimeLabel_->setWordWrap(true);
  gpuRestartLabel_ = new QLabel(gpuGroupBox_);
  gpuRestartLabel_->setObjectName(QStringLiteral("gpuRestartLabel"));
  vtrpon::setSettingsRole(gpuRestartLabel_, "description");
  gpuRestartLabel_->setWordWrap(true);
  gpuSaveStatusLabel_ = new QLabel(gpuGroupBox_);
  vtrpon::setSettingsRole(gpuSaveStatusLabel_, "error");
  gpuSaveStatusLabel_->setWordWrap(true);
  gpuLayout->addWidget(gpuFieldLabel_);
  gpuLayout->addWidget(gpuComboBox_);
  gpuLayout->addWidget(softwareDecodingCheckBox_);
  gpuLayout->addWidget(gpuRuntimeLabel_);
  gpuLayout->addWidget(gpuRestartLabel_);
  gpuLayout->addWidget(gpuSaveStatusLabel_);
  videoOutputPageLayout_->addWidget(gpuGroupBox_);
  connect(gpuComboBox_, qOverload<int>(&QComboBox::activated), this,
          [this] { saveGpuSettings(); });
  connect(softwareDecodingCheckBox_, &QCheckBox::toggled, this,
          [this] { saveGpuSettings(); });
#ifndef Q_OS_WIN
  gpuComboBox_->setEnabled(false);
  softwareDecodingCheckBox_->setEnabled(false);
#endif
  videoOutputPageLayout_->addStretch(1);
  pageStack->addWidget(playlistPage);
  pageStack->addWidget(onAirPage);
  pageStack->addWidget(outputPage);
  pageStack->addWidget(videoOutputPage_);
  pageStack->addWidget(audioPage);
  pageStack->addWidget(bgmPlayerPage);
  pageStack->addWidget(switcherPage);
  pageStack->addWidget(shortcutsPage);
  auto *appearanceScroll = new QScrollArea(pageStack);
  appearanceScroll->setObjectName(QStringLiteral("appearanceSettingsScroll"));
  appearanceScroll->setWidgetResizable(true);
  appearanceScroll->setFrameShape(QFrame::NoFrame);
  appearanceScroll->setWidget(appearancePage);
  pageStack->addWidget(appearanceScroll);
  testPatternPage_ = new TestPatternSettingsPage(pageStack);
  testPatternPage_->changed = [this] { emit testPatternsChanged(); };
  pageStack->addWidget(testPatternPage_);
  QVBoxLayout *timePageLayout = nullptr;
  auto *timePage = createPage(pageStack, {}, {}, &timePageTitle_,
                              &timeDescription_, &timePageLayout);
  auto *timeCard = vtrpon::createSettingsGroup(
      timePage, "settings.time.title");
  timeCard->setObjectName(QStringLiteral("timeSettingsCard"));
  timeCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *timeLayout = new QVBoxLayout(timeCard);
  timeLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  timeLayout->setSpacing(vtrpon::scalePx(10));
  auto *timeForm = new QFormLayout;
  timeZoneLabel_ = new QLabel(timeCard);
  vtrpon::setSettingsRole(timeZoneLabel_, "fieldLabel");
  timeZoneCombo_ = new QComboBox(timeCard);
  timeZoneCombo_->setObjectName(QStringLiteral("timeZoneCombo"));
  timeZoneCombo_->addItem(QString(), QString());
  for (const auto &id : QTimeZone::availableTimeZoneIds())
    timeZoneCombo_->addItem(QString::fromUtf8(id), QString::fromUtf8(id));
  timeZoneCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  timeZoneCombo_->setMinimumContentsLength(24);
  timeZoneLabel_->setBuddy(timeZoneCombo_);
  timeForm->addRow(timeZoneLabel_, timeZoneCombo_);
  timeSyncButton_ = new QPushButton(timeCard);
  timeSyncButton_->setObjectName(QStringLiteral("timeSyncButton"));
  timeForm->addRow(QString(), timeSyncButton_);
  timeRemainingLabel_ = new QLabel(timeCard);
  vtrpon::setSettingsRole(timeRemainingLabel_, "fieldLabel");
  timeRemainingCombo_ = new QComboBox(timeCard);
  timeRemainingCombo_->setObjectName(QStringLiteral("timeRemainingDefaultCombo"));
  timeRemainingCombo_->addItem(QString(), false);
  timeRemainingCombo_->addItem(QString(), true);
  timeRemainingLabel_->setBuddy(timeRemainingCombo_);
  timeForm->addRow(timeRemainingLabel_, timeRemainingCombo_);
  timeFontLabel_ = new QLabel(timeCard);
  vtrpon::setSettingsRole(timeFontLabel_, "fieldLabel");
  timeFontCombo_ = new QFontComboBox(timeCard);
  timeFontCombo_->setObjectName(QStringLiteral("currentTimeFontCombo"));
  timeFontLabel_->setBuddy(timeFontCombo_);
  timeForm->addRow(timeFontLabel_, timeFontCombo_);
  timeSizeLabel_ = new QLabel(timeCard);
  vtrpon::setSettingsRole(timeSizeLabel_, "fieldLabel");
  timeSizeSpin_ = new QSpinBox(timeCard);
  timeSizeSpin_->setObjectName(QStringLiteral("currentTimeFontSize"));
  timeSizeSpin_->setRange(8, 48);
  timeSizeSpin_->setSuffix(QStringLiteral(" pt"));
  timeSizeLabel_->setBuddy(timeSizeSpin_);
  timeForm->addRow(timeSizeLabel_, timeSizeSpin_);
  timeLayout->addLayout(timeForm);
  timeStatus_ = new QLabel(timeCard);
  vtrpon::setSettingsRole(timeStatus_, "status");
  timeStatus_->setWordWrap(true);
  timeLayout->addWidget(timeStatus_);

  auto *languageCard = vtrpon::createSettingsGroup(
      timePage, "settings.category.language");
  languageCard->setObjectName(QStringLiteral("languageSettingsCard"));
  languageCard->setAttribute(Qt::WA_StyledBackground, true);
  auto *languageCardLayout = new QVBoxLayout(languageCard);
  languageCardLayout->setContentsMargins(
      vtrpon::scalePx(20), vtrpon::scalePx(18), vtrpon::scalePx(20),
      vtrpon::scalePx(18));
  languageCardLayout->setSpacing(vtrpon::scalePx(10));
  languagePageDescription_ = new QLabel(languageCard);
  vtrpon::setSettingsRole(languagePageDescription_, "description");
  languagePageDescription_->setWordWrap(true);
  languageCardLayout->addWidget(languagePageDescription_);
  languageFieldLabel_ = new QLabel(languageCard);
  languageFieldLabel_->setObjectName(QStringLiteral("languageFieldLabel"));
  vtrpon::setSettingsRole(languageFieldLabel_, "fieldLabel");
  languageCardLayout->addWidget(languageFieldLabel_);
  languageComboBox_ = new QComboBox(languageCard);
  languageComboBox_->setObjectName(QStringLiteral("languageComboBox"));
  for (const vtrpon::UiLanguageDefinition &language :
       vtrpon::availableUiLanguages()) {
    languageComboBox_->addItem(language.nativeDisplayName,
                               static_cast<int>(language.language));
  }
  if (languageManager_) {
    const int currentIndex = languageComboBox_->findData(
        static_cast<int>(languageManager_->currentLanguage()));
    languageComboBox_->setCurrentIndex(currentIndex >= 0 ? currentIndex : 0);
  }
  languageCardLayout->addWidget(languageComboBox_);
  languageStatusLabel_ = new QLabel(languageCard);
  languageStatusLabel_->setObjectName(QStringLiteral("languageStatusLabel"));
  vtrpon::setSettingsRole(languageStatusLabel_, "status");
  languageStatusLabel_->setWordWrap(true);
  languageCardLayout->addWidget(languageStatusLabel_);
  timePageLayout->addWidget(languageCard);
  timePageLayout->addWidget(timeCard);
  timePageLayout->addStretch();
  pageStack->addWidget(timePage);
  operatorMonitorPage_ = new OperatorMonitorSettingsPage(pageStack);
  operatorMonitorPage_->changed = [this] { emit operatorMonitorSettingsChanged(); };
  operatorMonitorPage_->timerSettingsRequested = [this] {
    accept();
    emit operatorTimerSettingsRequested();
  };
  auto *operatorMonitorScroll = new QScrollArea(pageStack);
  operatorMonitorScroll->setObjectName(QStringLiteral("operatorMonitorSettingsScroll"));
  operatorMonitorScroll->setWidgetResizable(true);
  operatorMonitorScroll->setFrameShape(QFrame::NoFrame);
  operatorMonitorScroll->setWidget(operatorMonitorPage_);
  pageStack->addWidget(operatorMonitorScroll);
  fileManagementPage_ = new FileManagementSettingsPage(pageStack);
  pageStack->addWidget(fileManagementPage_);
  addonManagementPage_ = new AddonManagementPage(pageStack);
  auto *addonManagementScroll = new QScrollArea(pageStack);
  addonManagementScroll->setObjectName(QStringLiteral("addonManagementScroll"));
  addonManagementScroll->setWidgetResizable(true);
  addonManagementScroll->setFrameShape(QFrame::NoFrame);
  addonManagementScroll->setWidget(addonManagementPage_);
  pageStack->addWidget(addonManagementScroll);
  pageStack->addWidget(resetPage);
  pageStack->addWidget(updatesPage);
  virtualCameraPage_ = new QWidget(pageStack);
  virtualCameraPage_->setObjectName(QStringLiteral("settingsPage"));
  vtrpon::setSettingsRole(virtualCameraPage_, "page");
  auto *virtualCameraPageLayout = new QVBoxLayout(virtualCameraPage_);
  virtualCameraPageLayout->setContentsMargins(0, 0, 0, 0);
  auto *virtualCameraTitle = new QLabel(virtualCameraPage_);
  virtualCameraTitle->setObjectName(QStringLiteral("settingsPageTitle"));
  vtrpon::setSettingsRole(virtualCameraTitle, "pageTitle");
  virtualCameraPageLayout->addWidget(virtualCameraTitle);
  auto *virtualCameraLayout = vtrpon::addSettingsGroup(
      virtualCameraPageLayout, "virtual_camera.title");
  virtualCameraCheckBox_ = new QCheckBox(virtualCameraPage_);
  virtualCameraCheckBox_->setObjectName(QStringLiteral("virtualCameraEnabled"));
  virtualCameraCheckBox_->setChecked(vtrpon::virtualCameraEnabled());
  virtualCameraLayout->addWidget(virtualCameraCheckBox_);
  virtualCameraDescription_ = new QLabel(virtualCameraPage_);
  vtrpon::setSettingsRole(virtualCameraDescription_, "description");
  virtualCameraDescription_->setWordWrap(true);
  virtualCameraLayout->addWidget(virtualCameraDescription_);
  virtualCameraStatus_ = new QLabel(virtualCameraPage_);
  vtrpon::setSettingsRole(virtualCameraStatus_, "status");
  virtualCameraStatus_->setWordWrap(true);
  virtualCameraLayout->addWidget(virtualCameraStatus_);
  virtualCameraSaveStatus_ = new QLabel(virtualCameraPage_);
  virtualCameraSaveStatus_->setObjectName(QStringLiteral("virtualCameraSaveStatus"));
  vtrpon::setSettingsRole(virtualCameraSaveStatus_, "status");
  virtualCameraSaveStatus_->setWordWrap(true);
  virtualCameraLayout->addWidget(virtualCameraSaveStatus_);
  virtualCameraLayout->addStretch();
  pageStack->insertWidget(4, virtualCameraPage_);
  connect(virtualCameraCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (vtrpon::setVirtualCameraEnabled(enabled)) {
      virtualCameraSaveStatus_->clear();
      return;
    }
    const QSignalBlocker blocker(virtualCameraCheckBox_);
    virtualCameraCheckBox_->setChecked(vtrpon::virtualCameraEnabled());
    virtualCameraSaveStatus_->setText(vtrpon::uiText("virtual_camera.save_failed"));
  });
  auto *virtualCameraTimer = new QTimer(this);
  connect(virtualCameraTimer, &QTimer::timeout, this, [this] {
    virtualCameraStatus_->setText(vtrpon::virtualCameraStatus());
    const QSignalBlocker blocker(virtualCameraCheckBox_);
    virtualCameraCheckBox_->setChecked(vtrpon::virtualCameraEnabled());
  });
  virtualCameraTimer->start(500);
  if (vtrpon::networkVideoAddonInstalled()) {
    networkVideoPage_ = new NetworkVideoSettingsPage(pageStack);
    pageStack->insertWidget(4, networkVideoPage_);
  }
  const auto addNavigationItem = [this](const char *id, QWidget *page,
                                        const char *textId) {
    auto *item = new QListWidgetItem(navigationList_);
    item->setData(kNavigationIdRole, QString::fromLatin1(id));
    item->setData(kNavigationPageRole, pageStack_->indexOf(page));
    item->setData(kNavigationTextIdRole, QString::fromLatin1(textId));
    item->setFlags(item->flags() &
                   ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
  };
  addNavigationItem("playlist", playlistPage,
                    "settings.navigation.playlist");
  addNavigationItem("on_air", onAirPage,
                    "settings.navigation.on_air");
  addNavigationItem("video_output", videoOutputPage_,
                    "settings.category.video_output");
  if (networkVideoPage_) {
    addNavigationItem("network_video", networkVideoPage_,
                      "settings.group.ndi");
  }
  addNavigationItem("virtual_camera", virtualCameraPage_,
                    "virtual_camera.title");
  addNavigationItem("switcher", switcherPage,
                    "settings.category.switcher");
  addNavigationItem("operator_monitor", operatorMonitorScroll,
                    "operator_monitor.settings");
  addNavigationItem("audio_output", outputPage,
                    "settings.category.audio_output");
  addNavigationItem("uvc_audio", audioPage,
                    "settings.category.audio");
  addNavigationItem("bgm_player", bgmPlayerPage,
                    "settings.category.bgm_player");
  addNavigationItem("appearance", appearanceScroll,
                    "settings.category.appearance");
  addNavigationItem("time_language", timePage,
                    "settings.category.time_language");
  addNavigationItem("shortcuts", shortcutsPage,
                    "settings.category.shortcuts");
  addNavigationItem("test_pattern", testPatternPage_,
                    "menu.tools.test_pattern");
  addNavigationItem("file_management", fileManagementPage_, "settings.files.title");
  addNavigationItem("addons", addonManagementScroll, "settings.addons.title");
  addNavigationItem("save_reset", resetPage,
                    "settings.category.save_reset");
  addNavigationItem("updates_notices", updatesPage,
                    "settings.updates.title");
  navigationList_->setCurrentRow(0);
  reloadTimeSettings();
  connect(timeZoneCombo_, &QComboBox::currentIndexChanged, this, &ControllerSettingsDialog::saveTimeSettings);
  connect(timeRemainingCombo_, &QComboBox::currentIndexChanged, this, &ControllerSettingsDialog::saveTimeSettings);
  connect(timeFontCombo_, &QFontComboBox::currentFontChanged, this, &ControllerSettingsDialog::saveTimeSettings);
  connect(timeSizeSpin_, &QSpinBox::valueChanged, this, &ControllerSettingsDialog::saveTimeSettings);
  connect(timeSyncButton_, &QPushButton::clicked, this, &ControllerSettingsDialog::timeSynchronizationRequested);
  contentLayout->addWidget(pageStack, 1);

  auto *footerLayout = new QHBoxLayout;
  footerLayout->addStretch(1);
  closeButton_ = new QPushButton(contentPane);
  closeButton_->setObjectName(QStringLiteral("closeSettingsButton"));
  footerLayout->addWidget(closeButton_);
  contentLayout->addLayout(footerLayout);
  rootLayout->addWidget(contentPane, 1);

  setTabOrder(navigationList_, playlistEditModeComboBox_);
  setTabOrder(playlistEditModeComboBox_, autoSelectNextItemCheckBox_);
  setTabOrder(autoSelectNextItemCheckBox_,
              allowSameItemOnAirRetakeCheckBox_);
  setTabOrder(allowSameItemOnAirRetakeCheckBox_, onAirToggleModeCheckBox_);
  setTabOrder(onAirToggleModeCheckBox_,
              showPlaybackRateControlsCheckBox_);
  setTabOrder(showPlaybackRateControlsCheckBox_, disableFtbButtonCheckBox_);
  setTabOrder(disableFtbButtonCheckBox_,
              programOutputComboBox_);
  setTabOrder(programOutputComboBox_, pflOutputComboBox_);
  setTabOrder(pflOutputComboBox_, showBgmPlayerControlsCheckBox_);
  setTabOrder(showBgmPlayerControlsCheckBox_, rolandV8HdEnabledCheckBox_);
  setTabOrder(rolandV8HdEnabledCheckBox_, rolandV8HdSourceSpinBox_);
  setTabOrder(rolandV8HdSourceSpinBox_,
              rolandTakeProgramWhenOnAirCheckBox_);
  setTabOrder(rolandTakeProgramWhenOnAirCheckBox_,
              rolandTakeOnAirWhenProgramSelectedCheckBox_);
  setTabOrder(rolandTakeOnAirWhenProgramSelectedCheckBox_,
              rolandFtbFollowsProgramCheckBox_);
  setTabOrder(rolandFtbFollowsProgramCheckBox_, atemEnabledCheckBox_);
  setTabOrder(atemEnabledCheckBox_, atemSourceSpinBox_);
  setTabOrder(atemSourceSpinBox_, atemTakeProgramWhenOnAirCheckBox_);
  setTabOrder(atemTakeProgramWhenOnAirCheckBox_,
              atemTakeOnAirWhenProgramSelectedCheckBox_);
  setTabOrder(atemTakeOnAirWhenProgramSelectedCheckBox_,
              atemFtbFollowsProgramCheckBox_);
  setTabOrder(atemFtbFollowsProgramCheckBox_, skinComboBox_);
  setTabOrder(skinComboBox_, applySkinButton_);
  setTabOrder(applySkinButton_, textSizeSlider_);
  setTabOrder(textSizeSlider_, timeZoneCombo_);
  setTabOrder(timeZoneCombo_, timeSyncButton_);
  setTabOrder(timeSyncButton_, timeRemainingCombo_);
  setTabOrder(timeRemainingCombo_, timeFontCombo_);
  setTabOrder(timeFontCombo_, timeSizeSpin_);
  setTabOrder(timeSizeSpin_, languageComboBox_);
  setTabOrder(languageComboBox_, resetButton_);
  setTabOrder(resetButton_, exportSettingsButton_);
  setTabOrder(exportSettingsButton_, importSettingsButton_);
  setTabOrder(importSettingsButton_, automaticUpdateCheckBox_);
  setTabOrder(automaticUpdateCheckBox_, activityNoticeCheckBox_);
  setTabOrder(activityNoticeCheckBox_, closeButton_);
  connect(navigationList_, &QListWidget::currentRowChanged, this,
          [this] { showCurrentNavigationPage(); });
  connect(applySkinButton_, &QPushButton::clicked, this,
          &ControllerSettingsDialog::applySelectedSkin);
  connect(languageComboBox_, qOverload<int>(&QComboBox::currentIndexChanged),
          this, &ControllerSettingsDialog::applySelectedLanguage);
  connect(playlistEditModeComboBox_,
          qOverload<int>(&QComboBox::currentIndexChanged), this,
          &ControllerSettingsDialog::savePlaylistSettings);
  connect(autoSelectNextItemCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::savePlaylistSettings);
  connect(allowSameItemOnAirRetakeCheckBox_, &QCheckBox::toggled, this,
          [this](bool checked) {
            if (checked) {
              const QSignalBlocker blocker(onAirToggleModeCheckBox_);
              onAirToggleModeCheckBox_->setChecked(false);
            }
            saveOnAirSettings();
          });
  connect(onAirToggleModeCheckBox_, &QCheckBox::toggled, this,
          [this](bool checked) {
            if (checked) {
              const QSignalBlocker blocker(allowSameItemOnAirRetakeCheckBox_);
              allowSameItemOnAirRetakeCheckBox_->setChecked(false);
            }
            saveOnAirSettings();
          });
  connect(showPlaybackRateControlsCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveOnAirSettings);
  connect(disableFtbButtonCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveOnAirSettings);
  connect(showBgmPlayerControlsCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveAudioSettings);
  connect(rolandV8HdEnabledCheckBox_, &QCheckBox::toggled, this,
          [this](bool enabled) {
            if (enabled && atemEnabledCheckBox_->isChecked()) {
              const QSignalBlocker blocker(atemEnabledCheckBox_);
              atemEnabledCheckBox_->setChecked(false);
            }
            saveSwitcherControlSettings();
          });
  connect(rolandV8HdSourceSpinBox_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this] {
            rolandTakeProgramWhenOnAirCheckBox_->setText(
                vtrpon::uiText(
                    "settings.switcher.roland_v8hd.take_program_when_on_air")
                    .arg(rolandV8HdSourceSpinBox_->value()));
            rolandTakeOnAirWhenProgramSelectedCheckBox_->setText(
                vtrpon::uiText(
                    "settings.switcher.roland_v8hd.take_on_air_when_program_selected")
                    .arg(rolandV8HdSourceSpinBox_->value()));
            saveSwitcherControlSettings();
          });
  connect(rolandTakeProgramWhenOnAirCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveSwitcherControlSettings);
  connect(rolandTakeOnAirWhenProgramSelectedCheckBox_,
          &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveSwitcherControlSettings);
  connect(rolandFtbFollowsProgramCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveSwitcherControlSettings);
  connect(atemEnabledCheckBox_, &QCheckBox::toggled, this,
          [this](bool enabled) {
            if (!vtrpon::AtemAddonSwitcher::addonInstalled()) return;
            if (enabled && rolandV8HdEnabledCheckBox_->isChecked()) {
              const QSignalBlocker blocker(rolandV8HdEnabledCheckBox_);
              rolandV8HdEnabledCheckBox_->setChecked(false);
            }
            saveSwitcherControlSettings();
          });
  connect(atemSourceSpinBox_, qOverload<int>(&QSpinBox::valueChanged), this,
          [this] {
            atemTakeProgramWhenOnAirCheckBox_->setText(
                vtrpon::uiText(
                    "settings.switcher.atem.take_program_when_on_air")
                    .arg(atemSourceSpinBox_->value()));
            atemTakeOnAirWhenProgramSelectedCheckBox_->setText(
                vtrpon::uiText(
                    "settings.switcher.atem.take_on_air_when_program_selected")
                    .arg(atemSourceSpinBox_->value()));
            saveSwitcherControlSettings();
          });
  connect(atemTakeProgramWhenOnAirCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveSwitcherControlSettings);
  connect(atemTakeOnAirWhenProgramSelectedCheckBox_, &QCheckBox::toggled,
          this, &ControllerSettingsDialog::saveSwitcherControlSettings);
  connect(atemFtbFollowsProgramCheckBox_, &QCheckBox::toggled, this,
          &ControllerSettingsDialog::saveSwitcherControlSettings);
  connect(closeButton_, &QPushButton::clicked, this,
          &ControllerSettingsDialog::accept);
  if (uvcDeviceMonitor_) {
    connect(uvcDeviceMonitor_, &UvcDeviceMonitor::devicesChanged, this,
            &ControllerSettingsDialog::refreshUvcAudioMappings);
  }
  if (audioDeviceMonitor_) {
    connect(audioDeviceMonitor_, &AudioInputDeviceMonitor::devicesChanged,
            this, &ControllerSettingsDialog::refreshUvcAudioMappings);
  }
  if (switcherController_) {
    connect(switcherController_,
            &vtrpon::SwitcherController::connectionStateChanged, this,
            &ControllerSettingsDialog::updateSwitcherConnectionState);
  }
  retranslateUi();
  updateSwitcherConnectionState(
      switcherController_ && switcherController_->switcherReady(),
      switcherController_ ? switcherController_->switcherConnectionDetail()
                          : QStringLiteral("Disabled"));
}

void ControllerSettingsDialog::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  refreshGpuSettings();
  // Account for the grouped page's minimum height before centering the dialog.
  layout()->activate();
  resize(size().expandedTo(minimumSizeHint()));
  if (QWidget *owner = parentWidget()) {
    QRect geometry(QPoint(0, 0), size());
    geometry.moveCenter(owner->frameGeometry().center());
    if (QScreen *screen = owner->screen()) {
      const QRect available = screen->availableGeometry();
      if (geometry.left() < available.left()) {
        geometry.moveLeft(available.left());
      }
      if (geometry.top() < available.top()) {
        geometry.moveTop(available.top());
      }
      if (geometry.right() > available.right()) {
        geometry.moveRight(available.right());
      }
      if (geometry.bottom() > available.bottom()) {
        geometry.moveBottom(available.bottom());
      }
    }
    move(geometry.topLeft());
  }
  raise();
  activateWindow();
  navigationList_->setFocus(Qt::OtherFocusReason);
}

void ControllerSettingsDialog::changeEvent(QEvent *event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
}

void ControllerSettingsDialog::retranslateUi() {
  for (int row = 0; row < navigationList_->count(); ++row) {
    auto *item = navigationList_->item(row);
    const QByteArray textId =
        item->data(kNavigationTextIdRole).toString().toUtf8();
    item->setText(vtrpon::uiText(textId.constData()));
  }
  if (networkVideoPage_) networkVideoPage_->retranslate();
  addonManagementPage_->retranslate();
  virtualCameraCheckBox_->setText(vtrpon::uiText("virtual_camera.enable"));
  virtualCameraDescription_->setText(vtrpon::uiText("virtual_camera.description"));
  if (!virtualCameraSaveStatus_->text().isEmpty())
    virtualCameraSaveStatus_->setText(vtrpon::uiText("virtual_camera.save_failed"));
  operatorMonitorPage_->retranslate();
  virtualCameraPage_->findChild<QLabel *>(QStringLiteral("settingsPageTitle"))->setText(
      vtrpon::uiText("virtual_camera.title"));
  vtrpon::retranslateSettingsGroups(this);
  timePageTitle_->setText(
      vtrpon::uiText("settings.category.time_language"));
  timeDescription_->setText(
      vtrpon::uiText("settings.time_language.description"));
  timeZoneLabel_->setText(vtrpon::uiText("settings.time.zone"));
  timeZoneCombo_->setItemText(0, vtrpon::uiText("settings.time.system"));
  timeSyncButton_->setText(vtrpon::uiText("settings.time.sync"));
  timeRemainingLabel_->setText(vtrpon::uiText("settings.time.remaining"));
  timeRemainingCombo_->setItemText(0, vtrpon::uiText("settings.time.item"));
  timeRemainingCombo_->setItemText(1, vtrpon::uiText("settings.time.total"));
  timeFontLabel_->setText(vtrpon::uiText("settings.time.font"));
  timeSizeLabel_->setText(vtrpon::uiText("settings.time.size"));
  testPatternPage_->retranslate();
  shortcutsPageTitle_->setText(vtrpon::uiText("settings.category.shortcuts"));
  shortcutsDescription_->setText(vtrpon::uiText("settings.shortcuts.description"));
  shortcutsTable_->setHorizontalHeaderLabels({vtrpon::uiText("settings.shortcuts.action"),
      vtrpon::uiText("settings.shortcuts.key1"), vtrpon::uiText("settings.shortcuts.key2")});
  for (int row = 0; row < shortcutsTable_->rowCount(); ++row) {
    const auto &id = vtrpon::shortcutSettings::definitions().at(vtrpon::shortcutSettings::displayOrder().at(row)).id;
    QString name;
    if (id.startsWith(QStringLiteral("SelectItem")))
      name = vtrpon::uiText("settings.shortcuts.SelectItem").arg(id.mid(10));
    else if (id.startsWith(QStringLiteral("ActivatePlaylist")))
      name = vtrpon::uiText("settings.shortcuts.ActivatePlaylist").arg(id.mid(16));
    else name = vtrpon::uiText((QStringLiteral("settings.shortcuts.") + id).toUtf8().constData());
    shortcutsTable_->item(row, 0)->setText(name);
    for (int column = 1; column <= 2; ++column) {
      shortcutsTable_->cellWidget(row, column)->setAccessibleName(name + QStringLiteral(" %1").arg(column));
      if (auto *lineEdit = shortcutsTable_->cellWidget(row, column)->findChild<QLineEdit *>())
        lineEdit->setPlaceholderText(vtrpon::uiText("settings.shortcuts.placeholder"));
    }
  }
  saveShortcutsButton_->setText(vtrpon::uiText("settings.shortcuts.save"));
  resetShortcutsButton_->setText(vtrpon::uiText("settings.shortcuts.defaults"));
  videoOutputPageTitle_->setText(vtrpon::uiText("settings.category.video_output"));
  videoOutputPageDescription_->setText(vtrpon::uiText("settings.video_output.description"));
  refreshGpuSettings();
  resetPageTitle_->setText(vtrpon::uiText("settings.reset.title"));
  resetPageDescription_->setText(vtrpon::uiText("settings.reset.description"));
  updatesPageTitle_->setText(vtrpon::uiText("settings.updates.title"));
  updatesPageDescription_->setText(vtrpon::uiText("settings.updates.description"));
  automaticUpdateCheckBox_->setText(vtrpon::uiText("settings.updates.check_automatic"));
  automaticUpdateDescription_->setText(vtrpon::uiText("settings.updates.check_description"));
  resetButton_->setText(vtrpon::uiText("settings.reset.button"));
  exportSettingsButton_->setText(vtrpon::uiText("settings.export.button"));
  importSettingsButton_->setText(vtrpon::uiText("settings.import.button"));
  setWindowTitle(vtrpon::uiText("settings.title"));
  settingsTitle_->setText(vtrpon::uiText("settings.title"));
  navigationList_->setAccessibleName(
      vtrpon::uiText("settings.categories.accessible"));
  outputPageTitle_->setText(vtrpon::uiText("settings.category.audio_output"));
  defaultAudioFallbackCheckBox_->setText(vtrpon::uiText("settings.output.fallback_default"));
  useVstPluginsCheckBox_->setText(vtrpon::uiText("vst.use"));
  vstAddonStatusLabel_->setText(vtrpon::uiText(vtrpon::VstService::instance().available()
      ? "vst.addon_ready" : "vst.addon_missing"));
  outputPageDescription_->setText(vtrpon::uiText("settings.output.description"));
  programOutputLabel_->setText(vtrpon::uiText("settings.output.program_device"));
  pflOutputLabel_->setText(vtrpon::uiText("settings.output.pfl_device"));
  programOutputComboBox_->setAccessibleName(programOutputLabel_->text());
  pflOutputComboBox_->setAccessibleName(pflOutputLabel_->text());
  refreshAudioOutputs();

  accessibilityDescription_->setText(vtrpon::uiText("settings.accessibility.description"));
  textSizeLabel_->setText(vtrpon::uiText("settings.accessibility.text_size"));
  textSizeSlider_->setAccessibleName(textSizeLabel_->text());
  textSizePreview_->setText(vtrpon::uiText("settings.accessibility.preview"));
  for (int i = 0; i < 3; ++i)
    textSizeSteps_[i]->setText(vtrpon::uiText(i == 0 ? "settings.accessibility.small" : i == 1 ? "settings.accessibility.medium" : "settings.accessibility.large"));
  appearancePageTitle_->setText(
      vtrpon::uiText("settings.category.appearance"));
  appearancePageDescription_->setText(
      vtrpon::uiText("settings.appearance.description"));
  activityNoticeCheckBox_->setText(vtrpon::uiText("settings.privacy.activity_consent"));
  activityNoticeDescription_->setText(vtrpon::uiText("settings.privacy.activity_description"));
  skinFieldLabel_->setText(vtrpon::uiText("settings.appearance.skin"));
  skinComboBox_->setAccessibleName(
      vtrpon::uiText("settings.appearance.skin"));
  for (int index = 0; index < skinComboBox_->count(); ++index) {
    const auto *skin = vtrpon::controllerSkinDefinition(
        skinComboBox_->itemData(index).toString());
    if (skin) {
      skinComboBox_->setItemText(index, skinDisplayName(*skin));
    }
  }
  applySkinButton_->setText(vtrpon::uiText("common.apply"));
  applySkinButton_->setAccessibleDescription(
      vtrpon::uiText("settings.appearance.apply_accessible"));
  appliedSkinLabel_->setAccessibleName(
      vtrpon::uiText("settings.appearance.status_accessible"));
  setStatusText(
      vtrpon::uiText("settings.appearance.current_skin")
          .arg(skinComboBox_->currentText()),
      false);

  playlistPageTitle_->setText(
      vtrpon::uiText("settings.playlist.title"));
  playlistPageDescription_->setText(
      vtrpon::uiText("settings.playlist.description"));
  playlistEditModeFieldLabel_->setText(
      vtrpon::uiText("settings.playlist.edit_mode.field"));
  playlistEditModeComboBox_->setAccessibleName(
      vtrpon::uiText("settings.playlist.edit_mode.accessible"));
  const int editButtonIndex = playlistEditModeComboBox_->findData(
      static_cast<int>(PlaylistEditActivationMode::EditButton));
  if (editButtonIndex >= 0) {
    playlistEditModeComboBox_->setItemText(
        editButtonIndex,
        vtrpon::uiText("settings.playlist.edit_mode.button"));
  }
  const int selectionIndex = playlistEditModeComboBox_->findData(
      static_cast<int>(PlaylistEditActivationMode::Selection));
  if (selectionIndex >= 0) {
    playlistEditModeComboBox_->setItemText(
        selectionIndex,
        vtrpon::uiText("settings.playlist.edit_mode.selection"));
  }
  autoSelectNextItemCheckBox_->setText(
      vtrpon::uiText("settings.playlist.auto_select_next"));
  autoSelectNextItemCheckBox_->setAccessibleDescription(
      vtrpon::uiText("settings.playlist.auto_select_next.description"));
  playlistStatusLabel_->setAccessibleName(
      vtrpon::uiText("settings.playlist.status_accessible"));
  playlistStatusLabel_->setText(
      vtrpon::uiText("settings.playlist.saved_automatically"));

  onAirPageTitle_->setText(vtrpon::uiText("settings.on_air.title"));
  onAirPageDescription_->setText(
      vtrpon::uiText("settings.on_air.description"));
  onAirToggleModeCheckBox_->setText(vtrpon::uiText("settings.on_air.toggle_mode"));
  allowSameItemOnAirRetakeCheckBox_->setText(
      vtrpon::uiText("settings.on_air.allow_same_item_retake"));
  allowSameItemOnAirRetakeCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.on_air.allow_same_item_retake.description"));
  showPlaybackRateControlsCheckBox_->setText(
      vtrpon::uiText("settings.on_air.show_playback_rate_controls"));
  showPlaybackRateControlsCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.on_air.show_playback_rate_controls.description"));
  disableFtbButtonCheckBox_->setText(
      vtrpon::uiText("settings.on_air.disable_ftb_button"));
  disableFtbButtonCheckBox_->setAccessibleDescription(
      vtrpon::uiText("settings.on_air.disable_ftb_button.description"));
  onAirStatusLabel_->setAccessibleName(
      vtrpon::uiText("settings.on_air.status_accessible"));
  onAirStatusLabel_->setText(
      vtrpon::uiText("settings.on_air.saved_automatically"));

  audioPageTitle_->setText(vtrpon::uiText("settings.audio.title"));
  audioPageDescription_->setText(
      vtrpon::uiText("settings.audio.description"));
  mappingStatusLabel_->setAccessibleName(
      vtrpon::uiText("settings.audio.status_accessible"));

  bgmPlayerPageTitle_->setText(
      vtrpon::uiText("settings.bgm_player.title"));
  bgmPlayerPageDescription_->setText(
      vtrpon::uiText("settings.bgm_player.description"));
  showBgmPlayerControlsCheckBox_->setText(
      vtrpon::uiText("settings.bgm_player.show"));
  showBgmPlayerControlsCheckBox_->setAccessibleDescription(
      vtrpon::uiText("settings.bgm_player.show.description"));
  bgmPlayerStatusLabel_->setAccessibleName(
      vtrpon::uiText("settings.bgm_player.status_accessible"));
  bgmPlayerStatusLabel_->setText(
      vtrpon::uiText("settings.bgm_player.saved_automatically"));

  switcherPageTitle_->setText(
      vtrpon::uiText("settings.switcher.title"));
  switcherPageDescription_->setText(
      vtrpon::uiText("settings.switcher.description"));
  rolandV8HdEnabledCheckBox_->setText(
      vtrpon::uiText("settings.switcher.roland_v8hd.enable"));
  rolandV8HdEnabledCheckBox_->setAccessibleDescription(
      vtrpon::uiText("settings.switcher.roland_v8hd.enable.description"));
  rolandV8HdEnabledCheckBox_->setToolTip(
      vtrpon::AtemAddonSwitcher::addonInstalled() ? vtrpon::uiText("settings.switcher.exclusive") : QString());
  rolandV8HdSourceLabel_->setText(
      vtrpon::uiText("settings.switcher.roland_v8hd.source"));
  rolandV8HdSourceSpinBox_->setAccessibleName(
      vtrpon::uiText("settings.switcher.roland_v8hd.source.accessible"));
  rolandTakeProgramWhenOnAirCheckBox_->setText(
      vtrpon::uiText(
          "settings.switcher.roland_v8hd.take_program_when_on_air")
          .arg(rolandV8HdSourceSpinBox_->value()));
  rolandTakeProgramWhenOnAirCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.switcher.roland_v8hd.take_program_when_on_air.description"));
  rolandTakeOnAirWhenProgramSelectedCheckBox_->setText(
      vtrpon::uiText(
          "settings.switcher.roland_v8hd.take_on_air_when_program_selected")
          .arg(rolandV8HdSourceSpinBox_->value()));
  rolandTakeOnAirWhenProgramSelectedCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.switcher.roland_v8hd.take_on_air_when_program_selected.description"));
  rolandFtbFollowsProgramCheckBox_->setText(
      vtrpon::uiText("settings.switcher.roland_v8hd.ftb_follows"));
  rolandFtbFollowsProgramCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.switcher.roland_v8hd.ftb_follows.description"));
  atemEnabledCheckBox_->setText(
      vtrpon::uiText("settings.switcher.atem.enable"));
  atemEnabledCheckBox_->setAccessibleDescription(
      vtrpon::uiText("settings.switcher.atem.enable.description"));
  atemEnabledCheckBox_->setToolTip(
      vtrpon::uiText("settings.switcher.exclusive"));
  atemSourceLabel_->setText(
      vtrpon::uiText("settings.switcher.atem.source"));
  atemSourceSpinBox_->setAccessibleName(
      vtrpon::uiText("settings.switcher.atem.source.accessible"));
  atemTakeProgramWhenOnAirCheckBox_->setText(
      vtrpon::uiText("settings.switcher.atem.take_program_when_on_air")
          .arg(atemSourceSpinBox_->value()));
  atemTakeProgramWhenOnAirCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.switcher.atem.take_program_when_on_air.description"));
  atemTakeOnAirWhenProgramSelectedCheckBox_->setText(
      vtrpon::uiText(
          "settings.switcher.atem.take_on_air_when_program_selected")
          .arg(atemSourceSpinBox_->value()));
  atemTakeOnAirWhenProgramSelectedCheckBox_->setAccessibleDescription(
      vtrpon::uiText(
          "settings.switcher.atem.take_on_air_when_program_selected.description"));
  atemFtbFollowsProgramCheckBox_->setText(
      vtrpon::uiText("settings.switcher.atem.ftb_follows"));
  atemFtbFollowsProgramCheckBox_->setAccessibleDescription(
      vtrpon::uiText("settings.switcher.atem.ftb_follows.description"));
  atemAddonStatusLabel_->setText(vtrpon::uiText(
      vtrpon::AtemAddonSwitcher::addonInstalled()
          ? "settings.switcher.atem.addon_installed"
          : "settings.switcher.atem.addon_missing"));
  atemRuntimeStatus_->retranslate();
  switcherExclusiveLabel_->setText(
      vtrpon::uiText("settings.switcher.exclusive"));
  switcherStatusLabel_->setAccessibleName(
      vtrpon::uiText("settings.switcher.status_accessible"));
  switcherStatusLabel_->setText(
      vtrpon::uiText("settings.switcher.saved_automatically"));
  updateSwitcherConnectionState(
      switcherController_ && switcherController_->switcherReady(),
      switcherController_ ? switcherController_->switcherConnectionDetail()
                          : QStringLiteral("Disabled"));

  languagePageDescription_->setText(
      vtrpon::uiText("settings.language.description"));
  languageFieldLabel_->setText(
      vtrpon::uiText("settings.language.field"));
  languageComboBox_->setAccessibleName(
      vtrpon::uiText("settings.language.combo_accessible"));
  closeButton_->setText(vtrpon::uiText("common.close"));
  refreshUvcAudioMappings();
}

PlaylistEditActivationMode
ControllerSettingsDialog::savedPlaylistEditActivationMode() {
  const QSettings settings = controllerSettings();
  return vtrpon::loadPlaylistEditActivationMode(settings);
}

bool ControllerSettingsDialog::savedAutoSelectNextItemAfterOnAir() {
  const QSettings settings = controllerSettings();
  return vtrpon::loadAutoSelectNextItemAfterOnAir(settings);
}

bool ControllerSettingsDialog::savedAllowSameItemOnAirRetake() {
  const QSettings settings = controllerSettings();
  return vtrpon::loadAllowSameItemOnAirRetake(settings);
}

bool ControllerSettingsDialog::savedOnAirToggleMode() {
  return vtrpon::loadOnAirToggleMode(controllerSettings());
}

bool ControllerSettingsDialog::onAirToggleMode() const {
  return onAirToggleModeCheckBox_->isChecked();
}

bool ControllerSettingsDialog::savedShowPlaybackRateControls() {
  const QSettings settings = controllerSettings();
  return vtrpon::loadShowPlaybackRateControls(settings);
}

bool ControllerSettingsDialog::savedDisableFtbButton() {
  const QSettings settings = controllerSettings();
  return vtrpon::loadDisableFtbButton(settings);
}

bool ControllerSettingsDialog::savedShowBgmPlayerControls() {
  const QSettings settings = controllerSettings();
  return vtrpon::loadShowBgmPlayerControls(settings);
}

vtrpon::SwitcherControlSettings
ControllerSettingsDialog::savedSwitcherControlSettings() {
  return vtrpon::loadSwitcherControlSettings();
}

PlaylistEditActivationMode
ControllerSettingsDialog::playlistEditActivationMode() const {
  return playlistEditModeComboBox_ &&
                 playlistEditModeComboBox_->currentData().toInt() ==
                     static_cast<int>(PlaylistEditActivationMode::Selection)
             ? PlaylistEditActivationMode::Selection
             : PlaylistEditActivationMode::EditButton;
}

bool ControllerSettingsDialog::autoSelectNextItemAfterOnAir() const {
  return autoSelectNextItemCheckBox_ && autoSelectNextItemCheckBox_->isChecked();
}

bool ControllerSettingsDialog::allowSameItemOnAirRetake() const {
  return allowSameItemOnAirRetakeCheckBox_ &&
         allowSameItemOnAirRetakeCheckBox_->isChecked();
}

bool ControllerSettingsDialog::showPlaybackRateControls() const {
  return showPlaybackRateControlsCheckBox_ &&
         showPlaybackRateControlsCheckBox_->isChecked();
}

bool ControllerSettingsDialog::disableFtbButton() const {
  return disableFtbButtonCheckBox_ && disableFtbButtonCheckBox_->isChecked();
}

bool ControllerSettingsDialog::showBgmPlayerControls() const {
  return showBgmPlayerControlsCheckBox_ &&
         showBgmPlayerControlsCheckBox_->isChecked();
}

vtrpon::SwitcherControlSettings
ControllerSettingsDialog::switcherControlSettings() const {
  vtrpon::SwitcherControlSettings settings;
  settings.rolandV8HdEnabled =
      rolandV8HdEnabledCheckBox_ && rolandV8HdEnabledCheckBox_->isChecked();
  settings.rolandV8HdSourceNumber =
      rolandV8HdSourceSpinBox_ ? rolandV8HdSourceSpinBox_->value() : 1;
  settings.takeProgramWhenOnAir =
      rolandTakeProgramWhenOnAirCheckBox_ &&
      rolandTakeProgramWhenOnAirCheckBox_->isChecked();
  settings.takeOnAirWhenProgramSelected =
      rolandTakeOnAirWhenProgramSelectedCheckBox_ &&
      rolandTakeOnAirWhenProgramSelectedCheckBox_->isChecked();
  settings.ftbFollowsProgram =
      rolandFtbFollowsProgramCheckBox_ &&
      rolandFtbFollowsProgramCheckBox_->isChecked();
  settings.atemEnabled =
      atemEnabledCheckBox_ && atemEnabledCheckBox_->isChecked();
  settings.atemSourceNumber =
      atemSourceSpinBox_ ? atemSourceSpinBox_->value() : 1;
  settings.atemTakeProgramWhenOnAir =
      atemTakeProgramWhenOnAirCheckBox_ &&
      atemTakeProgramWhenOnAirCheckBox_->isChecked();
  settings.atemTakeOnAirWhenProgramSelected =
      atemTakeOnAirWhenProgramSelectedCheckBox_ &&
      atemTakeOnAirWhenProgramSelectedCheckBox_->isChecked();
  settings.atemFtbFollowsProgram =
      atemFtbFollowsProgramCheckBox_ &&
      atemFtbFollowsProgramCheckBox_->isChecked();
  return settings;
}

void ControllerSettingsDialog::savePlaylistSettings() {
  QSettings settings = controllerSettings();
  const bool error = !vtrpon::savePlaylistSettings(
      settings, playlistEditActivationMode(), autoSelectNextItemAfterOnAir());
  playlistStatusLabel_->setProperty("error", error);
  playlistStatusLabel_->setText(vtrpon::uiText(
      error ? "settings.playlist.save_failed"
            : "settings.playlist.saved_automatically"));
  playlistStatusLabel_->style()->unpolish(playlistStatusLabel_);
  playlistStatusLabel_->style()->polish(playlistStatusLabel_);
  playlistStatusLabel_->update();
}

void ControllerSettingsDialog::saveOnAirSettings() {
  QSettings settings = controllerSettings();
  const bool error = !vtrpon::saveOnAirSettings(
      settings, allowSameItemOnAirRetake(), showPlaybackRateControls(),
      disableFtbButton(), onAirToggleMode());
  onAirStatusLabel_->setProperty("error", error);
  onAirStatusLabel_->setText(vtrpon::uiText(
      error ? "settings.on_air.save_failed"
            : "settings.on_air.saved_automatically"));
  onAirStatusLabel_->style()->unpolish(onAirStatusLabel_);
  onAirStatusLabel_->style()->polish(onAirStatusLabel_);
  onAirStatusLabel_->update();
}

void ControllerSettingsDialog::saveAudioSettings() {
  QSettings settings = controllerSettings();
  const bool error = !vtrpon::saveAudioSettings(
      settings, showBgmPlayerControls());
  bgmPlayerStatusLabel_->setProperty("error", error);
  bgmPlayerStatusLabel_->setText(vtrpon::uiText(
      error ? "settings.bgm_player.save_failed"
            : "settings.bgm_player.saved_automatically"));
  bgmPlayerStatusLabel_->style()->unpolish(bgmPlayerStatusLabel_);
  bgmPlayerStatusLabel_->style()->polish(bgmPlayerStatusLabel_);
  bgmPlayerStatusLabel_->update();
  if (!error) {
    emit showBgmPlayerControlsChanged(showBgmPlayerControls());
  }
}

void ControllerSettingsDialog::saveSwitcherControlSettings() {
  const vtrpon::SwitcherControlSettings settings = switcherControlSettings();
  const bool error = !vtrpon::saveSwitcherControlSettings(settings);
  switcherStatusLabel_->setProperty("error", error);
  switcherStatusLabel_->setText(vtrpon::uiText(
      error ? "settings.switcher.save_failed"
            : "settings.switcher.saved_automatically"));
  switcherStatusLabel_->style()->unpolish(switcherStatusLabel_);
  switcherStatusLabel_->style()->polish(switcherStatusLabel_);
  switcherStatusLabel_->update();
  if (!error) {
    emit switcherControlSettingsChanged(settings);
    updateSwitcherConnectionState(
        switcherController_ && switcherController_->switcherReady(),
        switcherController_ ? switcherController_->switcherConnectionDetail()
                            : QStringLiteral("Disabled"));
  }
}

void ControllerSettingsDialog::updateSwitcherConnectionState(
    bool ready, const QString &detail) {
  refreshSwitcherConnectionIndicators(ready, detail);
}

void ControllerSettingsDialog::refreshSwitcherConnectionIndicators(
    bool ready, const QString &detail) {
  if (!rolandConnectionLabel_ || !atemConnectionLabel_) {
    return;
  }
  const auto updateIndicator = [&](QLabel *label, bool enabled) {
    const bool deviceReady = enabled && ready;
    label->setProperty("enabled", enabled);
    label->setProperty("ready", deviceReady);
    label->setText(vtrpon::uiText(
        !enabled ? "settings.switcher.connection.disabled"
                 : deviceReady ? "settings.switcher.connection.ready"
                               : "settings.switcher.connection.no_connection"));
    label->setToolTip(enabled && !deviceReady && !detail.isEmpty() &&
                              detail != QStringLiteral("Disabled")
                          ? detail
                          : QString());
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->update();
  };
  updateIndicator(rolandConnectionLabel_,
                  rolandV8HdEnabledCheckBox_->isChecked());
  updateIndicator(atemConnectionLabel_, atemEnabledCheckBox_->isChecked());
}

void ControllerSettingsDialog::applySelectedLanguage() {
  if (!languageManager_ || !languageComboBox_) {
    return;
  }
  const auto selectedLanguage = static_cast<vtrpon::UiLanguage>(
      languageComboBox_->currentData().toInt());
  if (selectedLanguage == languageManager_->currentLanguage()) {
    return;
  }

  QString errorMessage;
  const bool saved =
      languageManager_->setLanguage(selectedLanguage, &errorMessage);
  if (languageManager_->currentLanguage() != selectedLanguage) {
    const QSignalBlocker blocker(languageComboBox_);
    const int previousIndex = languageComboBox_->findData(
        static_cast<int>(languageManager_->currentLanguage()));
    languageComboBox_->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    languageStatusLabel_->setProperty("error", true);
    languageStatusLabel_->setText(
        vtrpon::uiText("settings.language.change_failed")
            .arg(errorMessage));
  } else {
    languageStatusLabel_->setProperty("error", !saved);
    languageStatusLabel_->setText(
        saved ? vtrpon::uiText("settings.language.changed")
                    .arg(languageComboBox_->currentText())
              : errorMessage);
  }
  languageStatusLabel_->style()->unpolish(languageStatusLabel_);
  languageStatusLabel_->style()->polish(languageStatusLabel_);
  languageStatusLabel_->update();
}

void ControllerSettingsDialog::applySelectedSkin() {
  const QString skinKey = skinComboBox_->currentData().toString();
  QString errorMessage;
  if (!vtrpon::applyControllerSkin(*qApp, skinKey, &errorMessage)) {
    setStatusText(
        vtrpon::uiText("settings.appearance.apply_failed").arg(errorMessage),
        true);
    return;
  }
  if (!vtrpon::saveControllerSkinPreference(skinKey, &errorMessage)) {
    setStatusText(
        vtrpon::uiText("settings.appearance.save_failed").arg(errorMessage),
        true);
    return;
  }
  setStatusText(
      vtrpon::uiText("settings.appearance.current_skin")
          .arg(skinComboBox_->currentText()),
      false);
}

void ControllerSettingsDialog::refreshUvcAudioMappings() {
  while (QLayoutItem *item = mappingRowsLayout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }

  const QList<UvcDevice> uvcDevices =
      uvcDeviceMonitor_ ? uvcDeviceMonitor_->devices() : QList<UvcDevice>{};
  const QList<AudioInputDevice> audioDevices =
      audioDeviceMonitor_ ? audioDeviceMonitor_->devices()
                          : QList<AudioInputDevice>{};
  if (uvcDevices.isEmpty()) {
    auto *emptyLabel = new QLabel(
        vtrpon::uiText("settings.audio.no_uvc_devices"));
    emptyLabel->setObjectName(QStringLiteral("noUvcDevicesLabel"));
    vtrpon::setSettingsRole(emptyLabel, "emptyState");
    emptyLabel->setWordWrap(true);
    mappingRowsLayout_->addWidget(emptyLabel);
    mappingRowsLayout_->addStretch(1);
  } else {
    QHash<QString, int> uvcNameCounts;
    for (const UvcDevice &uvcDevice : uvcDevices) {
      ++uvcNameCounts[uvcDevice.displayName];
    }
    QHash<QString, int> uvcNameIndexes;
    for (const UvcDevice &uvcDevice : uvcDevices) {
      QString visibleUvcName = uvcDevice.displayName;
      if (uvcNameCounts.value(uvcDevice.displayName) > 1) {
        visibleUvcName += QStringLiteral(" (%1)")
                              .arg(++uvcNameIndexes[uvcDevice.displayName]);
      }
      auto *row = new QWidget;
      auto *rowLayout = new QVBoxLayout(row);
      rowLayout->setContentsMargins(0, 0, 0, 0);
      rowLayout->setSpacing(vtrpon::scalePx(5));
      auto *uvcLabel = new QLabel(visibleUvcName, row);
      uvcLabel->setObjectName(QStringLiteral("uvcVideoDeviceLabel"));
      vtrpon::setSettingsRole(uvcLabel, "fieldLabel");
      uvcLabel->setToolTip(uvcDevice.devicePath);
      rowLayout->addWidget(uvcLabel);
      auto *audioComboBox = new QComboBox(row);
      audioComboBox->setObjectName(
          QStringLiteral("uvcAudioDeviceComboBox"));
      audioComboBox->setAccessibleName(
          vtrpon::uiText("settings.audio.input_for").arg(visibleUvcName));
      audioComboBox->setProperty("uvcDevicePath", uvcDevice.devicePath);
      audioComboBox->addItem(vtrpon::uiText("settings.audio.no_audio"),
                             QString());
      for (const AudioInputDevice &audioDevice : audioDevices) {
        audioComboBox->addItem(audioDevice.displayName,
                               audioDevice.deviceId);
        audioComboBox->setItemData(audioComboBox->count() - 1,
                                   audioDevice.deviceId, Qt::ToolTipRole);
      }
      const QString mappedDeviceId =
          uvcAudioMappingStore_
              ? uvcAudioMappingStore_->audioDeviceIdFor(uvcDevice.devicePath)
              : QString();
      int mappedIndex = audioComboBox->findData(mappedDeviceId);
      if (!mappedDeviceId.isEmpty() && mappedIndex < 0) {
        audioComboBox->addItem(
            vtrpon::uiText("settings.audio.unavailable_input")
                .arg(mappedDeviceId),
            mappedDeviceId);
        mappedIndex = audioComboBox->count() - 1;
      }
      audioComboBox->setCurrentIndex(mappedIndex >= 0 ? mappedIndex : 0);
      connect(audioComboBox,
              qOverload<int>(&QComboBox::currentIndexChanged), this,
              [this, audioComboBox] { saveUvcAudioMapping(audioComboBox); });
      rowLayout->addWidget(audioComboBox);
      mappingRowsLayout_->addWidget(row);
    }
    mappingRowsLayout_->addStretch(1);
  }

  if (!uvcDeviceMonitor_ || !uvcDeviceMonitor_->isAvailable()) {
    setMappingStatusText(
        uvcDeviceMonitor_ ? uvcDeviceMonitor_->errorDetail()
                          : vtrpon::uiText(
                                "settings.audio.uvc_monitor_unavailable"),
        true);
  } else if (!audioDeviceMonitor_ || !audioDeviceMonitor_->isAvailable()) {
    setMappingStatusText(
        audioDeviceMonitor_
            ? audioDeviceMonitor_->errorDetail()
            : vtrpon::uiText(
                  "settings.audio.input_monitor_unavailable"),
        true);
  } else {
    setMappingStatusText(vtrpon::uiText(
                             "settings.audio.saved_automatically"),
                         false);
  }
}

void ControllerSettingsDialog::saveUvcAudioMapping(QComboBox *comboBox) {
  if (!comboBox || !uvcAudioMappingStore_) {
    return;
  }
  const QString uvcDevicePath =
      comboBox->property("uvcDevicePath").toString();
  const QString audioDeviceId = comboBox->currentData().toString();
  QString errorDetail;
  if (!uvcAudioMappingStore_->setAudioDeviceIdFor(
          uvcDevicePath, audioDeviceId, &errorDetail)) {
    setMappingStatusText(errorDetail, true);
    return;
  }
  setMappingStatusText(
      vtrpon::uiText("settings.audio.saved_automatically"),
                       false);
}

void ControllerSettingsDialog::setStatusText(const QString &text, bool error) {
  appliedSkinLabel_->setProperty("error", error);
  appliedSkinLabel_->setText(text);
  appliedSkinLabel_->style()->unpolish(appliedSkinLabel_);
  appliedSkinLabel_->style()->polish(appliedSkinLabel_);
  appliedSkinLabel_->update();
}

void ControllerSettingsDialog::refreshAudioOutputs() {
  if (!outputDeviceMonitor_) return;
  const auto devices = outputDeviceMonitor_->devices();
  const auto fill = [&](QComboBox *combo, const QString &selected, bool program) {
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(vtrpon::uiText(program ? "settings.output.default" : "settings.output.none"), QString());
    if (!program) combo->addItem(vtrpon::uiText("settings.output.default"),
                                QString::fromLatin1(vtrpon::kSystemDefaultPflDeviceId));
    for (const auto &device : devices) combo->addItem(device.name, device.id);
    int index = combo->findData(selected);
    if (index < 0) {
      combo->addItem(vtrpon::uiText("settings.output.unavailable").arg(selected), selected);
      index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
  };
  fill(programOutputComboBox_, selectedProgramOutputId_, true);
  fill(pflOutputComboBox_, selectedPflOutputId_, false);
  updateAudioOutputWarning();
}

int ControllerSettingsDialog::navigationRow(const QString &id) const {
  for (int row = 0; row < navigationList_->count(); ++row) {
    if (navigationList_->item(row)->data(kNavigationIdRole).toString() == id) {
      return row;
    }
  }
  return -1;
}

void ControllerSettingsDialog::showCurrentNavigationPage() {
  const auto *item = navigationList_->currentItem();
  if (!item || !pageStack_) {
    return;
  }
  const int page = item->data(kNavigationPageRole).toInt();
  if (page >= 0 && page < pageStack_->count()) {
    pageStack_->setCurrentIndex(page);
  }
}

void ControllerSettingsDialog::showAudioOutputPage() {
  navigationList_->setCurrentRow(navigationRow(QStringLiteral("audio_output")));
  programOutputComboBox_->setFocus(Qt::OtherFocusReason);
}

void ControllerSettingsDialog::refreshSavedAudioOutputs() {
  selectedProgramOutputId_ = vtrpon::savedProgramOutputDeviceId();
  selectedPflOutputId_ = vtrpon::savedEditPflOutputDeviceId();
  refreshAudioOutputs();
}

void ControllerSettingsDialog::updateAudioOutputWarning() {
  const auto defaultId = outputDeviceMonitor_->defaultDeviceId();
  const bool same = vtrpon::sameAudioOutputDevice(selectedProgramOutputId_, selectedPflOutputId_, defaultId)
      || vtrpon::sameAudioOutputDevice(appliedProgramOutputId_, selectedPflOutputId_, defaultId);
  outputWarningLabel_->setText(vtrpon::uiText("settings.output.same_device"));
  outputWarningLabel_->setVisible(same);
  QString appliedName = appliedProgramOutputId_.isEmpty()
      ? vtrpon::uiText("settings.output.default") : appliedProgramOutputId_;
  const auto resolved = appliedProgramOutputId_.isEmpty() ? defaultId : appliedProgramOutputId_;
  for (const auto &device : outputDeviceMonitor_->devices()) {
    if (device.id == resolved) {
      appliedName = appliedProgramOutputId_.isEmpty()
          ? vtrpon::uiText("settings.output.default") + QStringLiteral(" — ") + device.name : device.name;
      break;
    }
  }
  const auto key = programOutputStatusKey_.toUtf8();
  programOutputStateLabel_->setText(vtrpon::uiText("settings.output.current_program").arg(appliedName)
      + QLatin1Char('\n') + vtrpon::uiText(key.constData()));
}

void ControllerSettingsDialog::setProgramOutputState(const QString &appliedId, const QString &statusKey) {
  if (appliedProgramOutputId_ == appliedId && programOutputStatusKey_ == statusKey) return;
  appliedProgramOutputId_ = appliedId;
  programOutputStatusKey_ = statusKey;
  updateAudioOutputWarning();
}

void ControllerSettingsDialog::saveAudioOutputSelection(bool program) {
  auto *combo = program ? programOutputComboBox_ : pflOutputComboBox_;
  if (combo->currentIndex() < 0) return;
  const auto selected = combo->currentData().toString();
  auto &previous = program ? selectedProgramOutputId_ : selectedPflOutputId_;
  if (previous == selected) return;
  const bool saved = program ? vtrpon::saveProgramOutputDeviceId(selected)
                            : vtrpon::saveEditPflOutputDeviceId(selected);
  if (!saved) {
    outputSaveStatusLabel_->setText(vtrpon::uiText("settings.output.save_failed"));
    refreshAudioOutputs();
    return;
  }
  previous = selected;
  outputSaveStatusLabel_->clear();
  if (program) {
    programOutputStatusKey_ = QStringLiteral("settings.output.pending");
    emit programOutputDeviceChanged(selected);
  } else emit editPflOutputDeviceChanged(selected);
  updateAudioOutputWarning();
}

void ControllerSettingsDialog::setMappingStatusText(const QString &text,
                                                    bool error) {
  mappingStatusLabel_->setProperty("error", error);
  mappingStatusLabel_->setText(text);
  mappingStatusLabel_->style()->unpolish(mappingStatusLabel_);
  mappingStatusLabel_->style()->polish(mappingStatusLabel_);
  mappingStatusLabel_->update();
}

void ControllerSettingsDialog::refreshGpuSettings() {
  if (!gpuComboBox_) return;
  const QSignalBlocker blockGpu(gpuComboBox_);
  const QSignalBlocker blockSoftware(softwareDecodingCheckBox_);
  auto settings = controllerSettings();
  const auto preferences = vtrpon::readGpuPreferences(settings);
  gpuFieldLabel_->setText(vtrpon::uiText("settings.gpu.device"));
  gpuComboBox_->setAccessibleName(gpuFieldLabel_->text());
  gpuComboBox_->clear();
  gpuComboBox_->addItem(vtrpon::uiText("settings.gpu.auto"), QString());
  for (const auto &adapter : vtrpon::availableGpuAdapters())
    gpuComboBox_->addItem(adapter.name, adapter.id);
  int selected = gpuComboBox_->findData(preferences.adapterId);
  if (selected < 0) {
    gpuComboBox_->addItem(vtrpon::uiText("settings.gpu.missing_choice"), preferences.adapterId);
    selected = gpuComboBox_->count() - 1;
  }
  gpuComboBox_->setCurrentIndex(selected);
  softwareDecodingCheckBox_->setText(vtrpon::uiText("settings.gpu.software"));
  softwareDecodingCheckBox_->setChecked(preferences.softwareDecoding);
  const auto *app = QCoreApplication::instance();
  const QString active = app->property("gpu.activeName").toString();
  QString runtime = active.isEmpty() ? vtrpon::uiText("settings.gpu.not_started")
      : vtrpon::uiText("settings.gpu.current").arg(active,
          vtrpon::uiText(app->property("gpu.activeSoftwareDecoding").toBool()
              ? "settings.gpu.mode_software" : "settings.gpu.mode_hardware"));
  if (app->property("gpu.requestedAdapterMissing").toBool())
    runtime += QLatin1Char('\n') + vtrpon::uiText("settings.gpu.fallback");
  gpuRuntimeLabel_->setText(runtime);
  gpuRestartLabel_->setText(vtrpon::uiText(preferences == vtrpon::startupGpuPreferences()
      ? "settings.gpu.restart_note" : "settings.gpu.restart_pending"));
#ifndef Q_OS_WIN
  gpuRuntimeLabel_->setText(vtrpon::uiText("settings.gpu.windows_only"));
#endif
}

void ControllerSettingsDialog::saveGpuSettings() {
  auto settings = controllerSettings();
  const bool saved = vtrpon::saveGpuPreferences(settings,
      {gpuComboBox_->currentData().toString(), softwareDecodingCheckBox_->isChecked()});
  gpuSaveStatusLabel_->setText(saved ? QString() : vtrpon::uiText("settings.gpu.save_failed"));
  refreshGpuSettings();
}

void ControllerSettingsDialog::reloadTimeSettings() {
  const QSignalBlocker zone(timeZoneCombo_), remaining(timeRemainingCombo_), font(timeFontCombo_), size(timeSizeSpin_);
  auto settings = controllerSettings();
  timeZoneCombo_->setCurrentIndex(std::max(0, timeZoneCombo_->findData(settings.value("time/timeZone", "Asia/Tokyo").toString())));
  timeRemainingCombo_->setCurrentIndex(vtrpon::savedRemainingTotal() ? 1 : 0);
  timeFontCombo_->setCurrentFont(vtrpon::savedClockFont());
  timeSizeSpin_->setValue(vtrpon::savedClockFont().pointSize());
}

void ControllerSettingsDialog::saveTimeSettings() {
  const bool remainingChanged = vtrpon::savedRemainingTotal() != timeRemainingCombo_->currentData().toBool();
  auto settings = controllerSettings();
  settings.setValue("time/timeZone", timeZoneCombo_->currentData());
  settings.setValue("time/remainingTotal", timeRemainingCombo_->currentData());
  settings.setValue("time/fontFamily", timeFontCombo_->currentFont().family());
  settings.setValue("time/fontSize", timeSizeSpin_->value());
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    timeStatus_->setText(vtrpon::uiText("settings.time.save_failed"));
    return;
  }
  timeStatus_->clear();
  emit timeSettingsChanged(remainingChanged);
}

void ControllerSettingsDialog::setTimeSynchronizationState(bool busy, const QString &status) {
  timeSyncButton_->setEnabled(!busy);
  timeStatus_->setText(busy ? vtrpon::uiText("settings.time.syncing") : status);
}

void ControllerSettingsDialog::applyTextSize(int size) {
  const qreal previousScale = vtrpon::uiTextScale();
  vtrpon::setUiTextSize(size);
  auto font = qApp->font();
  const qreal ratio = vtrpon::uiTextScale() / previousScale;
  if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * ratio);
  else font.setPixelSize(qRound(font.pixelSize() * ratio));
  qApp->setFont(font);
  qApp->setFont(font, "QMenu");
  qApp->setFont(font, "QMenuBar");
  const auto source = qApp->property("controllerUnscaledStyleSheet");
  if (source.isValid()) qApp->setStyleSheet(vtrpon::scaledStyleSheet(source.toString()));
  setStyleSheet(vtrpon::settingsStyleSheet());
  navigationList_->parentWidget()->setFixedWidth(vtrpon::scaleTextPx(190));
  for (auto *widget : QApplication::allWidgets()) widget->update();
}

void ControllerSettingsDialog::reloadSavedSettings() {
  fileManagementPage_->reload();
  addonManagementPage_->reload();
  // Application font changes must not save the controls' previous values.
  const QSignalBlocker timeFontBlocker(timeFontCombo_);
  vtrpon::VstService::instance().reloadSettings();
  {
    const QSignalBlocker blocker(useVstPluginsCheckBox_);
    useVstPluginsCheckBox_->setChecked(vtrpon::vstModeEnabled());
  }
  textSizeApplyTimer_->stop();
  const int size = std::clamp(controllerSettings().value("accessibility/textSize", 0).toInt(), 0, 2);
  const QSignalBlocker textSizeBlocker(textSizeSlider_);
  textSizeSlider_->setValue(size);
  applyTextSize(size);
  if (networkVideoPage_) networkVideoPage_->reload();
  operatorMonitorPage_->reload();
  testPatternPage_->reload();
  reloadTimeSettings();
  reloadShortcutEditors();
  // Prevent intermediate widget values from overwriting the restored snapshot.
  std::vector<std::unique_ptr<QSignalBlocker>> blockers;
  for (auto *child : findChildren<QObject *>())
    blockers.push_back(std::make_unique<QSignalBlocker>(child));
  skinComboBox_->setCurrentIndex(skinComboBox_->findData(vtrpon::currentControllerSkinKey()));
  automaticUpdateCheckBox_->setChecked(controllerSettings()
      .value(QStringLiteral("updates/checkAutomatically"), true).toBool());
  activityNoticeCheckBox_->setChecked(controllerSettings()
      .value(QStringLiteral("privacy/sendActivityNotice"), false).toBool());
  if (languageManager_)
    languageComboBox_->setCurrentIndex(languageComboBox_->findData(static_cast<int>(languageManager_->currentLanguage())));
  playlistEditModeComboBox_->setCurrentIndex(playlistEditModeComboBox_->findData(static_cast<int>(savedPlaylistEditActivationMode())));
  autoSelectNextItemCheckBox_->setChecked(savedAutoSelectNextItemAfterOnAir());
  onAirToggleModeCheckBox_->setChecked(savedOnAirToggleMode());
  allowSameItemOnAirRetakeCheckBox_->setChecked(savedAllowSameItemOnAirRetake());
  showPlaybackRateControlsCheckBox_->setChecked(savedShowPlaybackRateControls());
  disableFtbButtonCheckBox_->setChecked(savedDisableFtbButton());
  showBgmPlayerControlsCheckBox_->setChecked(savedShowBgmPlayerControls());
  virtualCameraCheckBox_->setChecked(vtrpon::virtualCameraEnabled());
  virtualCameraSaveStatus_->clear();
  defaultAudioFallbackCheckBox_->setChecked(vtrpon::savedUseDefaultAudioOutputWhenMissing());
  const auto saved = savedSwitcherControlSettings();
  rolandV8HdEnabledCheckBox_->setChecked(saved.rolandV8HdEnabled);
  rolandTakeProgramWhenOnAirCheckBox_->setChecked(saved.takeProgramWhenOnAir);
  rolandTakeOnAirWhenProgramSelectedCheckBox_->setChecked(saved.takeOnAirWhenProgramSelected);
  rolandFtbFollowsProgramCheckBox_->setChecked(saved.ftbFollowsProgram);
  atemEnabledCheckBox_->setChecked(saved.atemEnabled);
  atemTakeProgramWhenOnAirCheckBox_->setChecked(saved.atemTakeProgramWhenOnAir);
  atemTakeOnAirWhenProgramSelectedCheckBox_->setChecked(saved.atemTakeOnAirWhenProgramSelected);
  atemFtbFollowsProgramCheckBox_->setChecked(saved.atemFtbFollowsProgram);
  rolandV8HdSourceSpinBox_->setValue(saved.rolandV8HdSourceNumber);
  atemSourceSpinBox_->setValue(saved.atemSourceNumber);
  refreshSavedAudioOutputs();
  // Retranslation recreates mapping rows; release blockers first.
  blockers.clear();
  retranslateUi();
}

QColor ControllerSettingsDialog::savedPlayerBackgroundColor() {
  auto settings = controllerSettings();
  const QColor color(settings.value(QStringLiteral("player/backgroundColor"),
                                     QStringLiteral("#000000")).toString());
  return color.isValid() ? QColor(color.rgb()) : QColor(Qt::black);
}

void ControllerSettingsDialog::setSettingsFileStatus(const QString &text) {
  resetStatusLabel_->setText(text);
}

QWidget *ControllerSettingsDialog::videoOutputPage() const {
  return videoOutputPage_;
}

void ControllerSettingsDialog::addVideoOutputControls(QWidget *controls) {
  if (controls->layout()) {
    videoOutputPageLayout_->removeWidget(gpuGroupBox_);
    controls->layout()->addWidget(gpuGroupBox_);
    videoOutputPageLayout_->insertWidget(videoOutputPageLayout_->count() - 1, controls);
    if (auto *fullscreen = controls->findChild<QCheckBox *>(
            QStringLiteral("videoOutputControllerFullScreenCheckBox"))) {
      QWidget::setTabOrder(fullscreen, gpuComboBox_);
      QWidget::setTabOrder(gpuComboBox_, softwareDecodingCheckBox_);
    }
    return;
  }
  const int gpuIndex = videoOutputPageLayout_->indexOf(gpuGroupBox_);
  videoOutputPageLayout_->insertWidget(gpuIndex, controls);
  videoOutputPageLayout_->insertSpacing(gpuIndex + 1, vtrpon::scalePx(8));
}

void ControllerSettingsDialog::reloadShortcutEditors() {
  auto settings = controllerSettings();
  const auto values = vtrpon::shortcutSettings::load(settings);
  for (int row = 0; row < shortcutsTable_->rowCount(); ++row) {
    const auto keys = vtrpon::shortcutSettings::sequences(values, vtrpon::shortcutSettings::displayOrder().at(row));
    for (int column = 1; column <= 2; ++column)
      static_cast<QKeySequenceEdit *>(shortcutsTable_->cellWidget(row, column))->setKeySequence(keys.value(column - 1));
  }
  shortcutsStatus_->clear();
}

void ControllerSettingsDialog::saveShortcutSettings() {
  QJsonObject bindings;
  for (int row = 0; row < shortcutsTable_->rowCount(); ++row) {
    QJsonArray keys;
    for (int column = 1; column <= 2; ++column) {
      const auto sequence = static_cast<QKeySequenceEdit *>(shortcutsTable_->cellWidget(row, column))->keySequence();
      if (!sequence.isEmpty()) keys.append(sequence.toString(QKeySequence::PortableText));
    }
    bindings.insert(vtrpon::shortcutSettings::definitions().at(vtrpon::shortcutSettings::displayOrder().at(row)).id, keys);
  }
  auto error = vtrpon::shortcutSettings::validationError(bindings);
  if (!error.isEmpty()) {
    for (int row = 0; row < shortcutsTable_->rowCount(); ++row)
      error.replace(vtrpon::shortcutSettings::definitions().at(vtrpon::shortcutSettings::displayOrder().at(row)).id, shortcutsTable_->item(row, 0)->text());
    shortcutsStatus_->setText(vtrpon::uiText("settings.shortcuts.conflict").arg(error));
    return;
  }
  auto settings = controllerSettings();
  auto values = vtrpon::controllerSettingsSnapshot(settings);
  values.insert(vtrpon::shortcutSettings::key(), bindings);
  if (!vtrpon::replaceControllerSettings(settings, values)) {
    shortcutsStatus_->setText(vtrpon::uiText("settings.manage.failed"));
    return;
  }
  shortcutsStatus_->setText(vtrpon::uiText("settings.shortcuts.saved"));
  emit shortcutsChanged();
}
