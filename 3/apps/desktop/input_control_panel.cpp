#include "input_control_panel.h"
#include "controller_button_focus.h"
#include "controller_operations.h"

#include "controller_scaling.h"
#include "hardware_panel_widget.h"
#include "language_text.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QEvent>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <algorithm>

namespace {

constexpr int kPanelWidth = 558;
constexpr int kPanelMargin = 12;
constexpr int kPanelSpacing = 6;
constexpr int kControlHeight = 24;
constexpr int kButtonHeight = 20;
constexpr int kAddButtonWidth = 72;
constexpr int kCaptureButtonWidth = 88; // Same baseline as EDIT Start Mode.

} // namespace

InputControlPanel::InputControlPanel(QWidget *parent, ControllerOperations *operations)
    : QWidget(parent), operations_(operations ? operations : new ControllerOperations(this)) {
  setObjectName(QStringLiteral("inputControlPanel"));
  setAccessibleName(vtrpon::uiText("input.accessible"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(vtrpon::scalePx(kPanelWidth));
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  auto *sectionHeader = new QHBoxLayout;
  sectionHeader->setContentsMargins(vtrpon::scalePx(2), 0,
                                    vtrpon::scalePx(2), 0);
  sectionHeader->setSpacing(vtrpon::scalePx(kPanelSpacing));
  sectionTitle_ = new QLabel(this);
  sectionTitle_->setObjectName(QStringLiteral("inputControlTitle"));
  sectionHeader->addWidget(sectionTitle_);
  sectionHeader->addStretch(1);

  uvcModule_ = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  uvcModule_->setObjectName(QStringLiteral("inputUvcModule"));
  uvcModule_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *moduleLayout = new QVBoxLayout(uvcModule_);
  moduleLayout->setContentsMargins(vtrpon::scalePx(10), vtrpon::scalePx(2),
                                   vtrpon::scalePx(10), vtrpon::scalePx(8));
  moduleLayout->setSpacing(vtrpon::scalePx(2));
  moduleTitle_ = new QLabel(uvcModule_);
  moduleTitle_->setObjectName(QStringLiteral("inputControlModuleTitle"));
  moduleTitle_->setAlignment(Qt::AlignCenter);
  moduleTitle_->setFixedHeight(vtrpon::scalePx(15));
  moduleLayout->addWidget(moduleTitle_);

  auto *uvcLayout = new QHBoxLayout;
  uvcLayout->setContentsMargins(0, 0, 0, 0);
  uvcLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  uvcDeviceComboBox_ = new QComboBox(uvcModule_);
  uvcDeviceComboBox_->setObjectName(
      QStringLiteral("inputUvcDeviceComboBox"));
  uvcDeviceComboBox_->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  uvcDeviceComboBox_->setMinimumContentsLength(22);
  uvcDeviceComboBox_->setFixedHeight(vtrpon::scalePx(kControlHeight));
  uvcAddButton_ = new QPushButton(uvcModule_);
  uvcAddButton_->setObjectName(QStringLiteral("inputUvcAddButton"));
  uvcAddButton_->setFixedSize(
      vtrpon::scaleSize(kAddButtonWidth, kButtonHeight));
  uvcLayout->addStretch(1);
  uvcLayout->addWidget(uvcDeviceComboBox_);
  uvcLayout->addWidget(uvcAddButton_);
  uvcLayout->addStretch(1);
  moduleLayout->addLayout(uvcLayout);

  auto *stillModule = new HardwarePanelWidget(HardwarePanelWidget::Layout::SinglePlate, this);
  stillModule->setObjectName(QStringLiteral("inputStillModule"));
  auto *stillLayout = new QVBoxLayout(stillModule);
  stillLayout->setContentsMargins(vtrpon::scalePx(10), vtrpon::scalePx(2),
                                  vtrpon::scalePx(10), vtrpon::scalePx(8));
  stillLayout->setSpacing(vtrpon::scalePx(2));
  stillTitle_ = new QLabel(stillModule);
  stillTitle_->setObjectName(QStringLiteral("inputControlModuleTitle"));
  stillTitle_->setAlignment(Qt::AlignCenter);
  stillTitle_->setFixedHeight(vtrpon::scalePx(15));
  stillLayout->addWidget(stillTitle_);
  auto *captureRow = new QHBoxLayout;
  captureRow_ = captureRow;
  captureRow->setSpacing(vtrpon::scalePx(kPanelSpacing));
  captureRow->addStretch(1);
  previewCaptureButton_ = new QPushButton(stillModule);
  programCaptureButton_ = new QPushButton(stillModule);
  previewCaptureButton_->setObjectName(QStringLiteral("previewCaptureButton"));
  programCaptureButton_->setObjectName(QStringLiteral("programCaptureButton"));
  for (auto *button : {previewCaptureButton_, programCaptureButton_}) {
    button->setProperty("stillCaptureButton", true);
    button->setFixedHeight(vtrpon::scalePx(kButtonHeight));
    captureRow->addWidget(button);
  }
  stillDurationLabel_ = new QLabel(stillModule);
  stillDurationLabel_->setObjectName(QStringLiteral("stillCaptureDurationLabel"));
  captureRow->addWidget(stillDurationLabel_);
  stillDuration_ = new QDoubleSpinBox(stillModule);
  stillDuration_->setObjectName(QStringLiteral("stillCaptureDuration"));
  stillDuration_->setProperty("fadeDurationSpinBox", true);
  stillDuration_->setRange(0.1, 600.0);
  stillDuration_->setDecimals(1);
  stillDuration_->setSingleStep(0.5);
  stillDuration_->setValue(5.0);
  stillDuration_->setSuffix(QStringLiteral(" s"));
  stillDuration_->setKeyboardTracking(false);
  stillDuration_->setAlignment(Qt::AlignCenter);
  stillDuration_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  stillDuration_->setFixedSize(vtrpon::scaleSize(62, 24));
  stillDurationLabel_->setBuddy(stillDuration_);
  auto *durationRow = new QHBoxLayout;
  durationRow->setSpacing(vtrpon::scalePx(1));
  durationRow->addWidget(stillDuration_);
  auto *arrows = new QVBoxLayout;
  arrows->setSpacing(0);
  for (bool up : {true, false}) {
    auto *button = new QToolButton(stillModule);
    button->setObjectName(up ? QStringLiteral("stillDurationIncrease") : QStringLiteral("stillDurationDecrease"));
    button->setProperty("fadeDurationStepButton", true);
    button->setArrowType(up ? Qt::UpArrow : Qt::DownArrow);
    button->setAutoRepeat(true);
    button->setAutoRepeatDelay(400);
    button->setAutoRepeatInterval(80);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedSize(vtrpon::scaleSize(18, 12));
    connect(button, &QToolButton::clicked, stillDuration_,
            up ? &QAbstractSpinBox::stepUp : &QAbstractSpinBox::stepDown);
    arrows->addWidget(button);
  }
  durationRow->addLayout(arrows);
  captureRow->addLayout(durationRow);
  captureRow->addStretch(1);
  stillLayout->addLayout(captureRow);
  using A = ControllerOperations::Action;
  const auto bindCapture = [this](A action, QPushButton *button, bool program) {
    operations_->setGuard<>(action, this, [this] { return !stillCaptureBusy_; });
    operations_->bind<>(action, this, [this, program] {
      stillDuration_->interpretText();
      emit stillCaptureRequested(program, qRound64(stillDuration_->value() * 1000));
    });
    connect(button, &QPushButton::clicked, this,
            [this, action] { (void)operations_->execute(action); });
  };
  bindCapture(A::PreviewCapture, previewCaptureButton_, false);
  bindCapture(A::ProgramCapture, programCaptureButton_, true);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin));
  layout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  layout->addLayout(sectionHeader);
  layout->addWidget(stillModule);
  layout->addWidget(uvcModule_);
  layout->addStretch(1);

  connect(uvcAddButton_, &QPushButton::clicked, this, [this] {
    const int index = uvcDeviceComboBox_->currentIndex();
    if (index < 0) {
      return;
    }
    const QString devicePath =
        uvcDeviceComboBox_->itemData(index, Qt::UserRole).toString();
    const QString displayName =
        uvcDeviceComboBox_->itemData(index, Qt::UserRole + 1).toString();
    if (!devicePath.isEmpty() && !displayName.isEmpty()) {
      emit uvcAddRequested(displayName, devicePath);
    }
  });

  retranslateUi();
  setUvcDevices({});
  vtrpon::preserveFocusOnComboBoxClick(this);
}

void InputControlPanel::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  } else if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
    updateCaptureButtonWidths();
  }
}

void InputControlPanel::updateCaptureButtonWidths() {
  if (!previewCaptureButton_ || !programCaptureButton_) return;
  previewCaptureButton_->ensurePolished();
  programCaptureButton_->ensurePolished();
  const int width = std::max({vtrpon::scalePx(kCaptureButtonWidth),
                              previewCaptureButton_->sizeHint().width(),
                              programCaptureButton_->sizeHint().width()});
  previewCaptureButton_->setFixedWidth(width);
  programCaptureButton_->setFixedWidth(width);
  // Match the centered capture controls, including the duration step buttons.
  if (captureRow_ && stillDurationLabel_ && uvcDeviceComboBox_ && uvcAddButton_) {
    stillDurationLabel_->ensurePolished();
    captureRow_->invalidate();
    uvcDeviceComboBox_->setFixedWidth(captureRow_->sizeHint().width() -
                                    vtrpon::scalePx(kPanelSpacing) -
                                    uvcAddButton_->width());
  }
}

void InputControlPanel::setStillCaptureBusy(bool busy) {
  stillCaptureBusy_ = busy;
  previewCaptureButton_->setEnabled(!busy);
  programCaptureButton_->setEnabled(!busy);
}

void InputControlPanel::retranslateUi() {
  stillTitle_->setText(vtrpon::uiText("input.still.title"));
  previewCaptureButton_->setText(vtrpon::uiText("input.still.preview"));
  programCaptureButton_->setText(vtrpon::uiText("input.still.program"));
  stillDurationLabel_->setText(vtrpon::uiText("input.still.duration_label"));
  updateCaptureButtonWidths();
  stillDuration_->setAccessibleName(vtrpon::uiText("input.still.duration"));
  for (bool up : {true, false}) {
    auto *button = findChild<QToolButton *>(up ? QStringLiteral("stillDurationIncrease") : QStringLiteral("stillDurationDecrease"));
    button->setAccessibleName(vtrpon::uiText(up ? "operator_timer.increase" : "operator_timer.decrease")
                                  .arg(vtrpon::uiText("input.still.duration")));
  }
  setAccessibleName(vtrpon::uiText("input.accessible"));
  sectionTitle_->setText(vtrpon::uiText("input.title"));
  uvcModule_->setAccessibleName(vtrpon::uiText(deckLinkInputEnabled_ ? "decklink.input_title" : networkInputEnabled_ ? "input.network.title" : "input.uvc.accessible"));
  moduleTitle_->setText(vtrpon::uiText(deckLinkInputEnabled_ ? "decklink.input_title" : networkInputEnabled_ ? "input.network.title" : "input.uvc.title"));
  uvcDeviceComboBox_->setAccessibleName(
      vtrpon::uiText(deckLinkInputEnabled_ ? "decklink.input_devices" : networkInputEnabled_ ? "input.network.devices_accessible" : "input.uvc.devices_accessible"));
  uvcAddButton_->setAccessibleName(
      vtrpon::uiText(deckLinkInputEnabled_ ? "decklink.input_add" : networkInputEnabled_ ? "input.network.add" : "input.uvc.add_accessible"));
  uvcAddButton_->setToolTip(vtrpon::uiText(deckLinkInputEnabled_ ? "decklink.input_add" : networkInputEnabled_ ? "input.network.add" : "input.uvc.add_tooltip"));
  if (uvcDeviceComboBox_->count() >= 1 &&
      uvcDeviceComboBox_->itemData(0).toString().isEmpty()) {
    uvcDeviceComboBox_->setItemText(0, vtrpon::uiText("input.uvc.none"));
  }
  uvcAddButton_->setText(
      vtrpon::uiText(uvcCaptureBusy_ ? "common.capturing" : "common.add"));
}

void InputControlPanel::setUvcDevices(const QList<UvcDevice> &devices) {
  uvcDevices_ = devices;
  refreshDevices();
}

void InputControlPanel::setNetworkVideoSources(const QList<vtrpon::NetworkVideoSource> &sources, bool enabled) {
  const auto visible = enabled ? sources : QList<vtrpon::NetworkVideoSource>{};
  if (networkSources_ == visible && networkInputEnabled_ == enabled) return;
  networkSources_ = visible;
  networkInputEnabled_ = enabled;
  refreshDevices();
  retranslateUi();
}

void InputControlPanel::setDeckLinkSources(const QList<vtrpon::NetworkVideoSource> &sources, bool enabled) {
  const auto visible = enabled ? sources : QList<vtrpon::NetworkVideoSource>{};
  if (deckLinkSources_ == visible && deckLinkInputEnabled_ == enabled) return;
  deckLinkSources_ = visible;
  deckLinkInputEnabled_ = enabled;
  refreshDevices(); retranslateUi();
}

void InputControlPanel::refreshDevices() {
  const auto &devices = uvcDevices_;
  const QString previousPath = uvcDeviceComboBox_->currentData().toString();
  const QSignalBlocker blocker(uvcDeviceComboBox_);
  uvcDeviceComboBox_->clear();

  QHash<QString, int> nameCounts;
  for (const UvcDevice &device : devices) {
    ++nameCounts[device.displayName];
  }
  QHash<QString, int> nameIndexes;
  for (const UvcDevice &device : devices) {
    QString visibleName = device.displayName;
    if (nameCounts.value(device.displayName) > 1) {
      visibleName += QStringLiteral(" (%1)")
                         .arg(++nameIndexes[device.displayName]);
    }
    uvcDeviceComboBox_->addItem(visibleName, device.devicePath);
    const int index = uvcDeviceComboBox_->count() - 1;
    uvcDeviceComboBox_->setItemData(index, device.displayName,
                                    Qt::UserRole + 1);
    uvcDeviceComboBox_->setItemData(index, device.devicePath,
                                    Qt::ToolTipRole);
  }
  if (devices.isEmpty()) {
    uvcDeviceComboBox_->addItem(vtrpon::uiText("input.uvc.none"));
    if (auto *model = qobject_cast<QStandardItemModel *>(uvcDeviceComboBox_->model()))
      model->item(0)->setEnabled(false);
  }
  if (!networkSources_.isEmpty()) {
    uvcDeviceComboBox_->insertSeparator(uvcDeviceComboBox_->count());
    for (const auto &source : networkSources_) {
      uvcDeviceComboBox_->addItem(source.name, source.path);
      const int index = uvcDeviceComboBox_->count() - 1;
      uvcDeviceComboBox_->setItemData(index, source.name, Qt::UserRole + 1);
      uvcDeviceComboBox_->setItemData(index, source.name, Qt::ToolTipRole);
    }
  }
  if (!deckLinkSources_.isEmpty()) {
    uvcDeviceComboBox_->insertSeparator(uvcDeviceComboBox_->count());
    for (const auto &source : deckLinkSources_) {
      uvcDeviceComboBox_->addItem(source.name, source.path);
      const int index = uvcDeviceComboBox_->count() - 1;
      uvcDeviceComboBox_->setItemData(index, source.name, Qt::UserRole + 1);
      uvcDeviceComboBox_->setItemData(index, source.name, Qt::ToolTipRole);
    }
  }
  int firstAvailable = 0;
  while (firstAvailable < uvcDeviceComboBox_->count() && uvcDeviceComboBox_->itemData(firstAvailable).toString().isEmpty()) ++firstAvailable;
  const int previousIndex = previousPath.isEmpty() ? -1 : uvcDeviceComboBox_->findData(previousPath);
  uvcDeviceComboBox_->setCurrentIndex(previousIndex >= 0 ? previousIndex
      : firstAvailable < uvcDeviceComboBox_->count() ? firstAvailable : 0);
  const bool available = !devices.isEmpty() || !networkSources_.isEmpty() || !deckLinkSources_.isEmpty();
  uvcDeviceComboBox_->setEnabled(available && !uvcCaptureBusy_);
  uvcAddButton_->setEnabled(available && !uvcCaptureBusy_);
}

void InputControlPanel::setUvcCaptureBusy(bool busy) {
  uvcCaptureBusy_ = busy;
  const bool available =
      !uvcDeviceComboBox_->currentData().toString().isEmpty();
  uvcDeviceComboBox_->setEnabled(available && !busy);
  uvcAddButton_->setEnabled(available && !busy);
  uvcAddButton_->setText(
      vtrpon::uiText(busy ? "common.capturing" : "common.add"));
}
