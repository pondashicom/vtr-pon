#pragma once
#include "operator_monitor_settings.h"
#include "platform_capabilities.h"
#include "controller_settings.h"
#include "controller_scaling.h"
#include "language_text.h"
#include "settings_style.h"
#include "network_video.h"
#include "decklink.h"
#include "virtual_camera/output.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QTimer>
#include <functional>

class OperatorMonitorSettingsPage final : public QWidget {
public:
  std::function<void()> changed;
  std::function<void()> timerSettingsRequested;
  explicit OperatorMonitorSettingsPage(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("operatorMonitorSettingsPage"));
    vtrpon::setSettingsRole(this, "page");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(vtrpon::scalePx(12));
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("settingsPageTitle"));
    vtrpon::setSettingsRole(title_, "pageTitle");
    layout->addWidget(title_);
    auto *destinations = vtrpon::createSettingsGroup(this, "output.destinations");
    auto *destinationLayout = new QVBoxLayout(destinations);
    const std::array<const char *, 4> names{"operatorMonitorWindowOutput", "operatorMonitorDeckLinkOutput",
        "operatorMonitorNdiOutput", "operatorMonitorVirtualCameraOutput"};
    for (unsigned i = 0; i < outputs_.size(); ++i) {
      outputs_[i] = new QCheckBox(destinations);
      outputs_[i]->setObjectName(QString::fromLatin1(names[i]));
      destinationLayout->addWidget(outputs_[i]);
      connect(outputs_[i], &QCheckBox::toggled, this, [this, i](bool checked) { saveOutput(i, checked); });
    }
    outputHelp_ = new QLabel(destinations); outputHelp_->setWordWrap(true);
    vtrpon::setSettingsRole(outputHelp_, "description"); destinationLayout->addWidget(outputHelp_);
    layout->addWidget(destinations);
    auto *monitorGroup = vtrpon::createSettingsGroup(this, "operator_monitor.settings");
    layout->addWidget(monitorGroup);
    auto *monitorLayout = new QVBoxLayout(monitorGroup);
    monitorLayout->setSpacing(vtrpon::scalePx(12));
    displayLabel_ = new QLabel(this);
    vtrpon::setSettingsRole(displayLabel_, "fieldLabel");
    monitorLayout->addWidget(displayLabel_);
    displays_ = new QComboBox(this);
    displays_->setObjectName(QStringLiteral("operatorMonitorDisplayCombo"));
    displays_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    displays_->setMinimumContentsLength(20);
    displayLabel_->setBuddy(displays_);
    monitorLayout->addWidget(displays_);
    information_ = new QCheckBox(this);
    information_->setObjectName(QStringLiteral("operatorMonitorInformation"));
    monitorLayout->addWidget(information_);
    auto *fieldLayout = new QGridLayout;
    for (size_t i = 0; i < fields_.size(); ++i) {
      fields_[i] = new QCheckBox(this);
      fields_[i]->setObjectName(QStringLiteral("operatorMonitorField_") +
                               QString::fromLatin1(vtrpon::operatorMonitorFields[i]));
      fieldLayout->addWidget(fields_[i], int(i < 3 ? i : i - 3), i < 3 ? 0 : 1);
      connect(fields_[i], &QCheckBox::toggled, this, [this] { save(); });
    }
    monitorLayout->addLayout(fieldLayout);
    auto *timerGroup = vtrpon::createSettingsGroup(this, "operator_timer.title");
    layout->addWidget(timerGroup);
    auto *timerLayout = new QVBoxLayout(timerGroup);
    timerLayout->setSpacing(vtrpon::scalePx(12));
    timerSettingsButton_ = new QPushButton(this);
    timerSettingsButton_->setObjectName(QStringLiteral("operatorMonitorTimerSettingsButton"));
    timerSettingsButton_->setAutoDefault(false);
    timerLayout->addWidget(timerSettingsButton_, 0, Qt::AlignLeft);
    connect(timerSettingsButton_, &QPushButton::clicked, this, [this] {
      if (timerSettingsRequested) timerSettingsRequested();
    });
    timerEnabled_ = new QCheckBox(this);
    timerEnabled_->setObjectName(QStringLiteral("operatorMonitorTimerEnabled"));
    timerLayout->addWidget(timerEnabled_);
    status_ = new QLabel(this);
    vtrpon::setSettingsRole(status_, "status");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    layout->addStretch();
    connect(timerEnabled_, &QCheckBox::toggled, this, [this] { save(); });
    connect(displays_, &QComboBox::activated, this, [this] { save(); });
    connect(information_, &QCheckBox::toggled, this, [this] { save(); });
    connect(qApp, &QGuiApplication::screenAdded, this, [this] { refreshDisplays(); });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { refreshDisplays(); });
    reload();
    retranslate();
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { refreshOutputs(); });
    timer->start(500);
  }
  void reload() {
    auto settings = vtrpon::makeControllerSettings();
    saved_ = vtrpon::loadOperatorMonitorSettings(settings);
    const QSignalBlocker blocker(information_);
    information_->setChecked(saved_.information);
    const QSignalBlocker timerBlocker(timerEnabled_);
    timerEnabled_->setChecked(saved_.timerEnabled);
    for (size_t i = 0; i < fields_.size(); ++i) {
      const QSignalBlocker fieldBlocker(fields_[i]);
      fields_[i]->setChecked(saved_.fields[i]);
    }
    updateEnabledControls();
    refreshDisplays();
    refreshOutputs();
  }
  void retranslate() {
    vtrpon::retranslateSettingsGroups(this);
    title_->setText(vtrpon::uiText("operator_monitor.settings"));
    const std::array<const char *, 4> labels{"output.window", "output.decklink", "output.ndi", "output.virtual_camera"};
    for (unsigned i = 0; i < outputs_.size(); ++i) outputs_[i]->setText(vtrpon::uiText(labels[i]));
    outputHelp_->setText(vtrpon::uiText("output.return_help"));
    displayLabel_->setText(vtrpon::uiText("operator_monitor.display"));
    information_->setText(vtrpon::uiText("operator_monitor.information"));
    timerEnabled_->setText(vtrpon::uiText("operator_timer.enable"));
    timerSettingsButton_->setText(vtrpon::uiText("operator_timer.open_settings"));
    for (size_t i = 0; i < fields_.size(); ++i)
      fields_[i]->setText(vtrpon::uiText((QByteArray("operator_monitor.") + vtrpon::operatorMonitorFields[i]).constData()));
    refreshDisplays();
    if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::OperatorMonitor))
      status_->setText(vtrpon::uiText(vtrpon::platformFeatureUnavailableTextId(vtrpon::PlatformFeature::OperatorMonitor)));
  }
private:
  void refreshOutputs() {
    auto settings = vtrpon::makeControllerSettings();
    const auto ndi = vtrpon::savedNetworkVideoSettings();
    const auto deck = vtrpon::savedDeckLinkSettings();
    const std::array<bool, 4> values{settings.value("operatorMonitor/windowEnabled", false).toBool(),
        deck.outputEnabled && deck.returnMonitorEnabled, ndi.outputEnabled && ndi.returnMonitorEnabled,
        vtrpon::virtualCameraEnabled() && vtrpon::virtualCameraReturnMonitor()};
    for (unsigned i = 0; i < outputs_.size(); ++i) {
      const QSignalBlocker blocker(outputs_[i]); outputs_[i]->setChecked(values[i]);
    }
    outputs_[1]->setEnabled(vtrpon::deckLinkAddonInstalled());
    outputs_[2]->setEnabled(vtrpon::networkVideoAddonInstalled());
    displays_->setEnabled(values[0]); displayLabel_->setEnabled(values[0]);
  }
  void saveOutput(unsigned i, bool checked) {
    bool ok = true;
    if (i == 0) {
      auto settings = vtrpon::makeControllerSettings();
      settings.setValue("operatorMonitor/windowEnabled", checked); settings.sync();
      ok = settings.status() == QSettings::NoError;
    } else if (i == 1) {
      auto settings = vtrpon::savedDeckLinkSettings();
      if (checked && !settings.outputEnabled) {
        settings.compositeEnabled = settings.fillEnabled = settings.keyEnabled = false;
        settings.outputEnabled = true;
      }
      settings.returnMonitorEnabled = checked;
      vtrpon::saveDeckLinkSettings(settings);
      ok = vtrpon::savedDeckLinkSettings() == settings;
    } else if (i == 2) {
      auto settings = vtrpon::savedNetworkVideoSettings();
      if (checked && !settings.outputEnabled) {
        settings.compositeEnabled = settings.fillEnabled = settings.keyEnabled = settings.alphaEnabled = false;
        settings.outputEnabled = true;
      }
      settings.returnMonitorEnabled = checked;
      vtrpon::saveNetworkVideoSettings(settings);
      ok = vtrpon::savedNetworkVideoSettings() == settings;
    } else {
      if (checked) ok = vtrpon::setVirtualCameraReturnMonitor(true) && vtrpon::setVirtualCameraEnabled(true);
      else ok = vtrpon::setVirtualCameraEnabled(false);
    }
    refreshOutputs();
    status_->setText(ok ? QString() : vtrpon::uiText("settings.manage.failed"));
    if (ok && changed) changed();
  }
  void updateEnabledControls() {
    const bool supported = vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::OperatorMonitor);
    displays_->setEnabled(supported);
    information_->setEnabled(supported);
    for (size_t i = 0; i < fields_.size(); ++i)
      fields_[i]->setEnabled(supported && saved_.information && (!saved_.timerEnabled || i == 4));
  }
  void refreshDisplays() {
    const QSignalBlocker blocker(displays_);
    displays_->clear();
    displays_->addItem(vtrpon::uiText("operator_monitor.window"), QString());
    for (QScreen *screen : QGuiApplication::screens())
      displays_->addItem(QStringLiteral("%1 (%2 × %3)").arg(screen->name())
          .arg(screen->size().width()).arg(screen->size().height()), screen->name());
    if (displays_->findData(saved_.screen) < 0)
      displays_->addItem(vtrpon::uiText("operator_monitor.missing_display").arg(saved_.screen), saved_.screen);
    displays_->setCurrentIndex(displays_->findData(saved_.screen));
  }
  bool save() {
    auto settings = vtrpon::makeControllerSettings();
    saved_ = vtrpon::loadOperatorMonitorSettings(settings);
    if (vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::OperatorMonitor)) {
      saved_.screen = displays_->currentData().toString();
      saved_.information = information_->isChecked();
      for (size_t i = 0; i < fields_.size(); ++i) saved_.fields[i] = fields_[i]->isChecked();
    }
    saved_.timerEnabled = timerEnabled_->isChecked();
    updateEnabledControls();
    if (!vtrpon::saveOperatorMonitorSettings(settings, saved_)) {
      status_->setText(vtrpon::uiText("settings.manage.failed"));
      return false;
    }
    status_->setText(vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::OperatorMonitor)
        ? QString() : vtrpon::uiText(vtrpon::platformFeatureUnavailableTextId(vtrpon::PlatformFeature::OperatorMonitor)));
    if (changed) changed();
    return true;
  }
  vtrpon::OperatorMonitorSettings saved_;
  QLabel *title_, *displayLabel_, *status_;
  QComboBox *displays_;
  QCheckBox *information_;
  std::array<QCheckBox *, 5> fields_{};
  QCheckBox *timerEnabled_;
  QPushButton *timerSettingsButton_;
  std::array<QCheckBox *, 4> outputs_{};
  QLabel *outputHelp_;
};
