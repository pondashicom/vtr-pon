#pragma once

#include "operator_monitor_settings.h"
#include "controller_settings.h"
#include "controller_scaling.h"
#include "hardware_panel_widget.h"
#include "language_text.h"
#include <QColorDialog>
#include <QDialog>
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <functional>

class OperatorTimerSettingsDialog final : public QDialog {
public:
  std::function<void()> changed;
  std::function<void(const QString &)> cueSent;
  std::function<void()> cueCleared;

  explicit OperatorTimerSettingsDialog(QWidget *parent = nullptr) : QDialog(parent) {
    setObjectName(QStringLiteral("operatorTimerSettingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(false);
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                                    vtrpon::scalePx(12), vtrpon::scalePx(12));
    auto *panel = new HardwarePanelWidget(HardwarePanelWidget::Layout::SinglePlate, this);
    panel->setObjectName(QStringLiteral("operatorTimerSettingsPanel"));
    outerLayout->addWidget(panel);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(vtrpon::scalePx(18), vtrpon::scalePx(18),
                               vtrpon::scalePx(18), vtrpon::scalePx(18));
    layout->setSpacing(vtrpon::scalePx(12));
    cueLabel_ = new QLabel(this);
    layout->addWidget(cueLabel_);
    auto *cueRow = new QHBoxLayout;
    cue_ = new QLineEdit(this);
    cue_->setObjectName(QStringLiteral("operatorMonitorCueInput"));
    cue_->setMaxLength(500);
    cueLabel_->setBuddy(cue_);
    send_ = new QPushButton(this);
    send_->setObjectName(QStringLiteral("operatorMonitorCueSend"));
    clear_ = new QPushButton(this);
    clear_->setObjectName(QStringLiteral("operatorMonitorCueClear"));
    send_->setProperty("operatorTimerButton", true);
    clear_->setProperty("operatorTimerButton", true);
    send_->setAutoDefault(false);
    clear_->setAutoDefault(false);
    cueRow->addWidget(cue_, 1);
    cueRow->addWidget(send_);
    cueRow->addWidget(clear_);
    layout->addLayout(cueRow);
    auto *timerLayout = new QGridLayout;
    timerLayout->setHorizontalSpacing(vtrpon::scalePx(8));
    timerLayout->setVerticalSpacing(vtrpon::scalePx(8));
    timerLayout->setColumnStretch(2, 1);
    durationLabel_ = new QLabel(this);
    durationMinutes_ = new QSpinBox(this);
    durationMinutes_->setRange(0, 999);
    durationMinutes_->setObjectName(QStringLiteral("operatorMonitorTimerDurationMinutes"));
    durationSeconds_ = new QSpinBox(this);
    durationSeconds_->setRange(0, 59);
    durationSeconds_->setObjectName(QStringLiteral("operatorMonitorTimerDurationSeconds"));
    durationLabel_->setBuddy(durationMinutes_);
    auto *durationRow = new QHBoxLayout;
    durationRow->setSpacing(vtrpon::scalePx(8));
    durationRow->addWidget(createStepControl(durationMinutes_, 0));
    durationRow->addWidget(createStepControl(durationSeconds_, 1));
    durationRow->addStretch();
    timerLayout->addWidget(durationLabel_, 0, 0);
    timerLayout->addLayout(durationRow, 0, 1);
    for (size_t i = 0; i < 2; ++i) {
      warningLabels_[i] = new QLabel(this);
      warnings_[i] = new QSpinBox(this);
      warnings_[i]->setRange(0, 999);
      warnings_[i]->setObjectName(QStringLiteral("operatorMonitorTimerWarning%1").arg(i + 1));
      warningLabels_[i]->setBuddy(warnings_[i]);
      colorButtons_[i] = new QPushButton(this);
      colorButtons_[i]->setObjectName(QStringLiteral("operatorMonitorTimerColor%1").arg(i + 1));
      colorButtons_[i]->setProperty("operatorTimerButton", true);
      colorButtons_[i]->setAutoDefault(false);
      timerLayout->addWidget(warningLabels_[i], int(i + 1), 0);
      auto *warningRow = new QHBoxLayout;
      warningRow->setSpacing(vtrpon::scalePx(8));
      warningRow->addWidget(createStepControl(warnings_[i], i + 2));
      warningRow->addWidget(colorButtons_[i]);
      warningRow->addStretch();
      timerLayout->addLayout(warningRow, int(i + 1), 1);
      connect(colorButtons_[i], &QPushButton::clicked, this, [this, i] {
        const QColor color = QColorDialog::getColor(colors_[i], this, warningLabels_[i]->text());
        if (color.isValid()) { colors_[i] = color; updateColorButtons(); }
      });
    }
    layout->addLayout(timerLayout);
    timerSave_ = new QPushButton(this);
    timerSave_->setObjectName(QStringLiteral("operatorMonitorTimerSave"));
    timerSave_->setProperty("operatorTimerButton", true);
    timerSave_->setAutoDefault(false);
    layout->addWidget(timerSave_, 0, Qt::AlignLeft);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("operatorTimerSettingsStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    layout->addStretch();
    connect(timerSave_, &QPushButton::clicked, this, [this] { save(); });
    connect(send_, &QPushButton::clicked, this, [this] {
      const QString text = cue_->text().trimmed();
      if (!text.isEmpty() && cueSent) cueSent(text);
    });
    connect(clear_, &QPushButton::clicked, this, [this] {
      cue_->clear();
      if (cueCleared) cueCleared();
    });
    connect(cue_, &QLineEdit::textChanged, this, [this] {
      send_->setEnabled(!cue_->text().trimmed().isEmpty());
    });
    reload();
    retranslate();
    send_->setEnabled(false);
    resize(vtrpon::scaleSize(540, 310).expandedTo(minimumSizeHint()));
  }

  void setCueText(const QString &text) { cue_->setText(text); }
  void reload() {
    auto settings = vtrpon::makeControllerSettings();
    const auto saved = vtrpon::loadOperatorMonitorSettings(settings);
    durationMinutes_->setValue(saved.timerDurationSeconds / 60);
    durationSeconds_->setValue(saved.timerDurationSeconds % 60);
    warnings_[0]->setValue(saved.timerWarning1Minutes);
    warnings_[1]->setValue(saved.timerWarning2Minutes);
    colors_ = {saved.timerWarning1Color, saved.timerWarning2Color};
    updateColorButtons();
    status_->clear();
  }

protected:
  void changeEvent(QEvent *event) override {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
      retranslate();
      status_->clear();
    }
  }

private:
  QWidget *createStepControl(QSpinBox *spin, size_t index) {
    auto *control = new QWidget(this);
    control->setObjectName(spin->objectName() + QStringLiteral("Control"));
    control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *layout = new QHBoxLayout(control);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(vtrpon::scalePx(1));
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setKeyboardTracking(false);
    spin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    layout->addWidget(spin);
    auto *column = new QWidget(control);
    auto *buttons = new QVBoxLayout(column);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(0);
    const auto createButton = [&](Qt::ArrowType arrow, const QString &suffix) {
      auto *button = new QToolButton(column);
      button->setObjectName(spin->objectName() + suffix);
      button->setProperty("fadeDurationStepButton", true);
      button->setArrowType(arrow);
      button->setAutoRepeat(true);
      button->setAutoRepeatDelay(400);
      button->setAutoRepeatInterval(80);
      button->setFixedSize(vtrpon::scaleSize(18, 12));
      button->setFocusPolicy(Qt::StrongFocus);
      buttons->addWidget(button);
      return button;
    };
    increases_[index] = createButton(Qt::UpArrow, QStringLiteral("IncreaseButton"));
    decreases_[index] = createButton(Qt::DownArrow, QStringLiteral("DecreaseButton"));
    column->setFixedSize(vtrpon::scaleSize(18, 24));
    layout->addWidget(column, 0, Qt::AlignVCenter);
    connect(increases_[index], &QToolButton::clicked, spin, &QAbstractSpinBox::stepUp);
    connect(decreases_[index], &QToolButton::clicked, spin, &QAbstractSpinBox::stepDown);
    setTabOrder(spin, increases_[index]);
    setTabOrder(increases_[index], decreases_[index]);
    return control;
  }
  void retranslate() {
    setWindowTitle(vtrpon::uiText("operator_timer.settings"));
    cueLabel_->setText(vtrpon::uiText("operator_monitor.cue"));
    send_->setText(vtrpon::uiText("operator_monitor.send"));
    clear_->setText(vtrpon::uiText("operator_monitor.clear"));
    durationLabel_->setText(vtrpon::uiText("operator_timer.duration"));
    durationMinutes_->setSuffix(vtrpon::uiText("operator_timer.minutes"));
    durationSeconds_->setSuffix(vtrpon::uiText("operator_timer.seconds"));
    durationMinutes_->setAccessibleName(durationLabel_->text() + durationMinutes_->suffix());
    durationSeconds_->setAccessibleName(durationLabel_->text() + durationSeconds_->suffix());
    warningLabels_[0]->setText(vtrpon::uiText("operator_timer.warning1"));
    warningLabels_[1]->setText(vtrpon::uiText("operator_timer.warning2"));
    timerSave_->setText(vtrpon::uiText("operator_timer.save"));
    for (size_t i = 0; i < 2; ++i) {
      warnings_[i]->setSuffix(vtrpon::uiText("operator_timer.minutes"));
      colorButtons_[i]->setText(vtrpon::uiText("operator_timer.color"));
      colorButtons_[i]->setAccessibleName(warningLabels_[i]->text() + QStringLiteral(" ") + vtrpon::uiText("operator_timer.color"));
    }
    for (size_t i = 0; i < increases_.size(); ++i) {
      const QString name = i == 0 ? durationMinutes_->accessibleName()
          : i == 1 ? durationSeconds_->accessibleName() : warningLabels_[i - 2]->text();
      const QString increase = vtrpon::uiText("operator_timer.increase").arg(name);
      const QString decrease = vtrpon::uiText("operator_timer.decrease").arg(name);
      increases_[i]->setAccessibleName(increase);
      increases_[i]->setToolTip(increase);
      decreases_[i]->setAccessibleName(decrease);
      decreases_[i]->setToolTip(decrease);
    }
  }
  void updateColorButtons() {
    for (size_t i = 0; i < 2; ++i) {
      QPixmap swatch(vtrpon::scaleSize(22, 14));
      swatch.fill(colors_[i]);
      colorButtons_[i]->setIcon(QIcon(swatch));
    }
  }
  void save() {
    durationMinutes_->interpretText();
    durationSeconds_->interpretText();
    const int duration = durationMinutes_->value() * 60 + durationSeconds_->value();
    if (duration == 0) {
      status_->setText(vtrpon::uiText("operator_timer.duration_positive"));
      return;
    }
    for (auto *warning : warnings_) warning->interpretText();
    if (warnings_[1]->value() > warnings_[0]->value()) {
      status_->setText(vtrpon::uiText("operator_timer.warning_order"));
      return;
    }
    // Merge into current settings so another window's enable/display choices survive.
    auto settings = vtrpon::makeControllerSettings();
    auto saved = vtrpon::loadOperatorMonitorSettings(settings);
    saved.timerDurationSeconds = duration;
    saved.timerWarning1Minutes = warnings_[0]->value();
    saved.timerWarning2Minutes = warnings_[1]->value();
    saved.timerWarning1Color = colors_[0];
    saved.timerWarning2Color = colors_[1];
    if (!vtrpon::saveOperatorMonitorSettings(settings, saved)) {
      status_->setText(vtrpon::uiText("settings.manage.failed"));
      return;
    }
    if (changed) changed();
    status_->setText(vtrpon::uiText("operator_timer.saved"));
  }
  QLabel *cueLabel_, *durationLabel_, *status_;
  QLineEdit *cue_;
  QPushButton *send_, *clear_, *timerSave_;
  QSpinBox *durationMinutes_, *durationSeconds_;
  std::array<QLabel *, 2> warningLabels_{};
  std::array<QSpinBox *, 2> warnings_{};
  std::array<QPushButton *, 2> colorButtons_{};
  std::array<QColor, 2> colors_{};
  std::array<QToolButton *, 4> increases_{};
  std::array<QToolButton *, 4> decreases_{};
};
