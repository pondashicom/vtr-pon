#include "monitor_neon_outline.h"
#include "program_panel.h"
#include "controller_button_focus.h"
#include "equalizer_editor.h"
#include "vst_editor.h"
#include "audio_delay_control.h"
#include "time_settings.h"
#include <QDebug>

#include "audio_fader_widget.h"
#include "audio_meter_widget.h"
#include "controller_scaling.h"
#include "hardware_panel_widget.h"
#include "playlist_item_display.h"
#include "range_marker_slider.h"
#include "language_text.h"
#include "skins/controller_skin.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QColorDialog>
#include <QApplication>
#include <QConicalGradient>
#include <QRadialGradient>
#include <QMenu>
#include <QSignalBlocker>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDial>
#include <QEasingCurve>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

QIcon backgroundSwatch(const QColor &color) {
  QPixmap swatch(vtrpon::scaleSize(22, 12));
  swatch.fill(Qt::transparent);
  QPainter painter(&swatch);
  painter.setPen(QColor(QStringLiteral("#92999f")));
  painter.setBrush(color);
  painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
  return QIcon(swatch);
}

constexpr int kProgramSurfaceWidth = 592;
constexpr int kProgramSurfaceHeight = 333;
constexpr int kAudioStripWidth = 224;
constexpr int kFaderBankMinimumHeight = 400;
constexpr int kPanelMargin = 12;
constexpr int kPanelSpacing = 6;
constexpr int kTransportButtonWidth = 100;
constexpr int kTransportButtonHeight = 24;
constexpr int kCompactButtonHeight = 20;
constexpr int kBgmTransportButtonWidth = 64;
constexpr int kFadeButtonWidth = 34;
constexpr int kFadeControlHeight = 24;
constexpr int kFaderButtonHeight = 20;
constexpr int kPlaybackRateButtonWidth = 38;
constexpr int kPlaybackRateButtonSpacing = 8;
constexpr int kFadeDurationWidth = 41;
constexpr int kFadeDurationStepButtonWidth = 18;
constexpr int kFadeDurationStepButtonHeight = 12;
constexpr double kMinimumManualFadeSeconds = 0.1;
constexpr double kMaximumManualFadeSeconds = 60.0;
constexpr double kDefaultManualFadeSeconds = 3.0;
constexpr int kRemainingAlertThresholdMs = 5000;
constexpr int kBgmFadeBlinkIntervalMs = 300;
constexpr auto kPlaybackRates = std::to_array<double>(
    {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0});
constexpr auto kPlaybackRateButtonNames = std::to_array<const char *>({
    "programPlaybackRate050Button", "programPlaybackRate075Button",
    "programPlaybackRate100Button", "programPlaybackRate125Button",
    "programPlaybackRate150Button", "programPlaybackRate200Button",
    "programPlaybackRate300Button",
});

QPushButton *addButton(QHBoxLayout *layout, const QString &text) {
  auto *button = new QPushButton(text);
  layout->addWidget(button);
  return button;
}

QString formatRemainingTime(qint64 remainingMs) {
  const qint64 totalCentiseconds = (std::max)(remainingMs, qint64{0}) / 10;
  const qint64 centiseconds = totalCentiseconds % 100;
  const qint64 totalSeconds = totalCentiseconds / 100;
  const qint64 seconds = totalSeconds % 60;
  const qint64 totalMinutes = totalSeconds / 60;
  const qint64 minutes = totalMinutes % 60;
  const qint64 hours = totalMinutes / 60;
  return QStringLiteral("%1:%2:%3.%4")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'))
      .arg(centiseconds, 2, 10, QLatin1Char('0'));
}

void repolish(QWidget *widget)
{
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

void setButtonActive(QPushButton *button, bool active)
{
  if (button->property("active").toBool() == active) {
    return;
  }
  button->setProperty("active", active);
  repolish(button);
}

} // namespace

ProgramPanel::FadeControlWidgets ProgramPanel::createFadeControl(
    const QString &objectNamePrefix, const QString &buttonText,
    QWidget *parent) {
  FadeControlWidgets widgets;
  widgets.control = new QWidget(parent);
  widgets.control->setObjectName(objectNamePrefix + QStringLiteral("Control"));
  widgets.control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto *controlLayout = new QHBoxLayout(widgets.control);
  controlLayout->setContentsMargins(0, 0, 0, 0);
  const int controlSpacing = vtrpon::scalePx(4);
  controlLayout->setSpacing(controlSpacing);

  widgets.button = new QPushButton(buttonText, widgets.control);
  widgets.button->setObjectName(objectNamePrefix + QStringLiteral("Button"));
  widgets.button->setProperty("programFadeButton", true);
  widgets.button->setFixedSize(
      vtrpon::scaleSize(kFadeButtonWidth, kFadeControlHeight));
  controlLayout->addWidget(widgets.button);

  auto *durationControl = new QWidget(widgets.control);
  durationControl->setObjectName(objectNamePrefix
                                 + QStringLiteral("DurationControl"));
  durationControl->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto *durationLayout = new QHBoxLayout(durationControl);
  durationLayout->setContentsMargins(0, 0, 0, 0);
  const int durationSpacing = vtrpon::scalePx(1);
  durationLayout->setSpacing(durationSpacing);

  widgets.durationSpinBox = new QDoubleSpinBox(durationControl);
  widgets.durationSpinBox->setObjectName(
      objectNamePrefix + QStringLiteral("DurationSpinBox"));
  widgets.durationSpinBox->setProperty("fadeDurationSpinBox", true);
  widgets.durationSpinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
  widgets.durationSpinBox->setDecimals(1);
  widgets.durationSpinBox->setRange(kMinimumManualFadeSeconds,
                                    kMaximumManualFadeSeconds);
  widgets.durationSpinBox->setSingleStep(0.1);
  widgets.durationSpinBox->setValue(kDefaultManualFadeSeconds);
  widgets.durationSeconds = kDefaultManualFadeSeconds;
  widgets.durationSpinBox->setSuffix(QStringLiteral(" s"));
  widgets.durationSpinBox->setFixedSize(
      vtrpon::scaleSize(kFadeDurationWidth, kFadeControlHeight));
  widgets.durationSpinBox->setKeyboardTracking(false);
  durationLayout->addWidget(widgets.durationSpinBox);

  auto *buttonColumn = new QWidget(durationControl);
  buttonColumn->setFixedSize(
      vtrpon::scaleSize(kFadeDurationStepButtonWidth, kFadeControlHeight));
  auto *buttonColumnLayout = new QVBoxLayout(buttonColumn);
  buttonColumnLayout->setContentsMargins(0, 0, 0, 0);
  buttonColumnLayout->setSpacing(0);
  const auto createStepButton = [buttonColumn](Qt::ArrowType arrowType,
                                                const QString &objectName) {
    auto *button = new QToolButton(buttonColumn);
    button->setObjectName(objectName);
    button->setProperty("fadeDurationStepButton", true);
    button->setArrowType(arrowType);
    button->setAutoRepeat(true);
    button->setAutoRepeatDelay(400);
    button->setAutoRepeatInterval(80);
    button->setFixedSize(
        vtrpon::scaleSize(kFadeDurationStepButtonWidth,
                          kFadeDurationStepButtonHeight));
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
  };
  widgets.increaseButton = createStepButton(
      Qt::UpArrow,
      objectNamePrefix + QStringLiteral("DurationIncreaseButton"));
  widgets.decreaseButton = createStepButton(
      Qt::DownArrow,
      objectNamePrefix + QStringLiteral("DurationDecreaseButton"));
  buttonColumnLayout->addWidget(widgets.increaseButton);
  buttonColumnLayout->addWidget(widgets.decreaseButton);
  durationLayout->addWidget(buttonColumn);
  durationControl->setFixedSize(
      widgets.durationSpinBox->width() + durationSpacing
          + buttonColumn->width(),
      vtrpon::scalePx(kFadeControlHeight));
  controlLayout->addWidget(durationControl);
  widgets.control->setFixedSize(
      widgets.button->width() + controlSpacing + durationControl->width(),
      vtrpon::scalePx(kFadeControlHeight));

  connect(widgets.increaseButton, &QToolButton::clicked,
          widgets.durationSpinBox, &QAbstractSpinBox::stepUp);
  connect(widgets.decreaseButton, &QToolButton::clicked,
          widgets.durationSpinBox, &QAbstractSpinBox::stepDown);
  return widgets;
}

void ProgramPanel::retranslateFadeControl(FadeControlWidgets &control,
                                          const QString &channelName,
                                          const QString &fadeName) {
  const QString actionName = QStringLiteral("%1 %2")
                                 .arg(channelName, fadeName);
  control.button->setAccessibleName(actionName);
  control.button->setToolTip(actionName);
  const QString durationName =
      vtrpon::uiText("program.manual_fade_duration.accessible")
          .arg(channelName, fadeName);
  control.durationSpinBox->setAccessibleName(durationName);
  const QString increaseText =
      vtrpon::uiText("edit.duration.increase.accessible")
          .arg(durationName);
  const QString decreaseText =
      vtrpon::uiText("edit.duration.decrease.accessible")
          .arg(durationName);
  control.increaseButton->setAccessibleName(increaseText);
  control.increaseButton->setToolTip(increaseText);
  control.decreaseButton->setAccessibleName(decreaseText);
  control.decreaseButton->setToolTip(decreaseText);
}

void ProgramPanel::startManualFade(AudioFaderWidget *fader,
                                   QPushButton *button,
                                   double durationSeconds,
                                   double targetGain,
                                   ManualFadeTarget target) {
  cancelManualFade();
  manualFadeTarget_ = target;
  activeFadeButton_ = button;
  setButtonActive(activeFadeButton_, true);
  if (target == ManualFadeTarget::Item) {
    itemAudioAutomationOverridden_ = true;
  }
  manualFadeAnimation_->setDuration(qRound(
      std::clamp(durationSeconds, kMinimumManualFadeSeconds,
                 kMaximumManualFadeSeconds)
      * 1000.0));
  manualFadeAnimation_->setStartValue(fader->linearGain());
  manualFadeAnimation_->setEndValue(targetGain);
  manualFadeAnimation_->start();
}

void ProgramPanel::cancelManualFade() {
  if (manualFadeAnimation_) {
    manualFadeAnimation_->stop();
  }
  if (activeFadeButton_) {
    setButtonActive(activeFadeButton_, false);
  }
  activeFadeButton_ = nullptr;
  manualFadeTarget_ = ManualFadeTarget::None;
}

void ProgramPanel::handleUserFaderOperation(AudioFaderWidget *fader) {
  cancelManualFade();
  if (fader == itemFader_) {
    itemAudioAutomationOverridden_ = true;
  }
}

ProgramPanel::ProgramPanel(QWidget *parent, ControllerOperations *operations)
    : QWidget(parent), monitorWindow_(new QWidget(this)) {
  setObjectName(QStringLiteral("programControlPanel"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(vtrpon::scalePx(kProgramSurfaceWidth + 2 * kPanelMargin));
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

  monitorWindow_->setObjectName(QStringLiteral("onAirMonitor"));
  monitorWindow_->setAttribute(Qt::WA_NativeWindow, true);
  monitorWindow_->setFixedSize(vtrpon::scaleSize(kProgramSurfaceWidth,
                                                 kProgramSurfaceHeight));
  monitorWindow_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  monitorWindow_->winId();

  auto *sectionHeader = new QHBoxLayout;
  sectionHeader->setContentsMargins(2, 0, 2, 0);
  sectionHeader->setSpacing(vtrpon::scalePx(kPanelSpacing));
  auto *sectionTitle = new QLabel(vtrpon::uiText("program.section"), this);
  sectionTitle->setObjectName(QStringLiteral("programSectionTitle"));
  sectionHeader->addWidget(sectionTitle);
  sectionHeader->addStretch(1);
  backgroundColorButton_ = new QToolButton(this);
  backgroundColorButton_->setObjectName(QStringLiteral("programBackgroundColorButton"));
  backgroundColorButton_->setFixedSize(vtrpon::scaleSize(42, 22));
  backgroundColorButton_->setIconSize(vtrpon::scaleSize(22, 12));
  backgroundColorButton_->setFocusPolicy(Qt::StrongFocus);
  backgroundColorButton_->setPopupMode(QToolButton::InstantPopup);
  backgroundColorMenu_ = new QMenu(backgroundColorButton_);
  backgroundColorMenu_->setObjectName(QStringLiteral("programBackgroundColorMenu"));
  backgroundColorButton_->setMenu(backgroundColorMenu_);
  backgroundColorTitleAction_ = backgroundColorMenu_->addAction(QString());
  backgroundColorTitleAction_->setObjectName(QStringLiteral("programBackgroundColorTitle"));
  backgroundColorTitleAction_->setEnabled(false);
  QFont backgroundTitleFont = backgroundColorTitleAction_->font();
  backgroundTitleFont.setBold(true);
  backgroundColorTitleAction_->setFont(backgroundTitleFont);
  const QColor colors[] = {QColor(Qt::black), QColor("#00ff00"),
                           QColor("#0000ff"), QColor(Qt::white)};
  for (int i = 0; i < 4; ++i) {
    auto *action = backgroundColorMenu_->addAction(backgroundSwatch(colors[i]), QString());
    backgroundColorActions_[i] = action;
    action->setData(colors[i]);
    action->setCheckable(true);
    connect(action, &QAction::triggered, this, [this, color = colors[i]] {
      // Selection reflects applied settings, including when saving is rejected.
      updateBackgroundPresentation();
      emit playerBackgroundRequested(color);
    });
  }
  otherBackgroundColorAction_ = backgroundColorMenu_->addAction(QString());
  connect(otherBackgroundColorAction_, &QAction::triggered, this, [this] {
    const QColor color = QColorDialog::getColor(playerBackgroundColor_, this,
        vtrpon::uiText("settings.background.picker"));
    if (color.isValid()) emit playerBackgroundRequested(color);
  });
  sectionHeader->addWidget(backgroundColorButton_);

  fileLabel_ = new QLabel(vtrpon::uiText("program.file.none"), this);
  fileLabel_->setObjectName(QStringLiteral("onAirFileLabel"));
  fileLabel_->setAccessibleName(vtrpon::uiText("program.file.none_accessible"));
  fileLabel_->setAlignment(Qt::AlignCenter);
  fileLabel_->setMinimumHeight(vtrpon::scalePx(22));
  fileLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *actionPanel = new QWidget(this);
  actionPanel->setObjectName(QStringLiteral("programActionPanel"));
  actionPanel->setAccessibleName(vtrpon::uiText("program.actions.accessible"));
  actionPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *actionLayout = new QHBoxLayout(actionPanel);
  actionLayout->setContentsMargins(0, 0, 0, 0);
  actionLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));

  auto *effectsPanel = new QWidget(this);
  effectsPanel->setObjectName(QStringLiteral("programEffectsActionPanel"));
  effectsPanel->setAccessibleName(
      vtrpon::uiText("program.effects.accessible"));
  effectsPanel->setAttribute(Qt::WA_StyledBackground, true);
  effectsPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *effectsButtons = new QHBoxLayout(effectsPanel);
  effectsButtons->setContentsMargins(vtrpon::scalePx(10),
                                     vtrpon::scalePx(6),
                                     vtrpon::scalePx(10),
                                     vtrpon::scalePx(6));
  effectsButtons->setSpacing(vtrpon::scalePx(kPanelSpacing));
  effectsButtons->setAlignment(Qt::AlignHCenter);
  dskOnButton_ = addButton(effectsButtons, QStringLiteral("DSK"));
  dskPlayButton_ = addButton(effectsButtons,
                             vtrpon::uiText("common.play").toUpper());
  dskPauseButton_ = addButton(effectsButtons,
                              vtrpon::uiText("common.pause").toUpper());

  auto *transportPanel = new QWidget(actionPanel);
  transportPanel->setObjectName(
      QStringLiteral("programTransportActionPanel"));
  transportPanel->setAccessibleName(
      vtrpon::uiText("program.transport.accessible"));
  transportPanel->setAttribute(Qt::WA_StyledBackground, true);
  transportPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *transportButtons = new QHBoxLayout(transportPanel);
  transportButtons->setContentsMargins(vtrpon::scalePx(10),
                                       vtrpon::scalePx(12),
                                       vtrpon::scalePx(10),
                                       vtrpon::scalePx(12));
  offAirButton_ = addButton(transportButtons,
                            vtrpon::uiText("program.off_air"));
  transportButtons->addStretch(1);
  playButton_ = addButton(transportButtons,
                          vtrpon::uiText("common.play").toUpper());
  transportButtons->addStretch(1);
  pauseButton_ = addButton(transportButtons,
                           vtrpon::uiText("common.pause").toUpper());

  ftbPanel_ = new QWidget(actionPanel);
  ftbPanel_->setObjectName(QStringLiteral("programFtbActionPanel"));
  ftbPanel_->setAccessibleName(vtrpon::uiText("program.ftb.accessible"));
  ftbPanel_->setAttribute(Qt::WA_StyledBackground, true);
  ftbPanel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto *ftbLayout = new QHBoxLayout(ftbPanel_);
  ftbLayout->setContentsMargins(vtrpon::scalePx(10),
                                vtrpon::scalePx(12),
                                vtrpon::scalePx(10),
                                vtrpon::scalePx(12));
  ftbButton_ = addButton(ftbLayout, QStringLiteral("FTB"));

  actionLayout->addWidget(transportPanel, 1);
  actionLayout->addWidget(ftbPanel_);

  offAirButton_->setObjectName(QStringLiteral("offAirButton"));
  ftbButton_->setObjectName(QStringLiteral("ftbButton"));
  dskOnButton_->setObjectName(QStringLiteral("dskOnButton"));
  dskPlayButton_->setObjectName(QStringLiteral("dskPlayButton"));
  dskPauseButton_->setObjectName(QStringLiteral("dskPauseButton"));
  pauseButton_->setObjectName(QStringLiteral("programPauseButton"));
  playButton_->setObjectName(QStringLiteral("programPlayButton"));
  offAirButton_->setAccessibleName(vtrpon::uiText("program.off_air.accessible"));
  ftbButton_->setAccessibleName(vtrpon::uiText("program.ftb.accessible"));
  pauseButton_->setAccessibleName(vtrpon::uiText("program.pause.accessible"));
  playButton_->setAccessibleName(vtrpon::uiText("program.play.accessible"));
  playButton_->setToolTip(vtrpon::uiText("program.play.tooltip"));
  dskOnButton_->setAccessibleName(vtrpon::uiText("program.dsk.inactive_accessible"));
  dskPlayButton_->setAccessibleName(
      vtrpon::uiText("program.dsk.play.tooltip"));
  dskPauseButton_->setAccessibleName(
      vtrpon::uiText("program.dsk.pause.tooltip"));
  dskPlayButton_->setToolTip(vtrpon::uiText("program.dsk.play.tooltip"));
  dskPauseButton_->setToolTip(vtrpon::uiText("program.dsk.pause.tooltip"));
  dskOnButton_->setCheckable(true);
  dskOnButton_->setProperty("modeButton", true);
  for (QPushButton *button :
       {offAirButton_, ftbButton_, dskOnButton_, dskPlayButton_,
        dskPauseButton_, pauseButton_, playButton_}) {
    button->setFixedSize(vtrpon::scaleSize(kTransportButtonWidth,
                                           kTransportButtonHeight));
  }
  setTabOrder(dskOnButton_, dskPlayButton_);
  setTabOrder(dskPlayButton_, dskPauseButton_);
  setTabOrder(dskPauseButton_, offAirButton_);
  setTabOrder(offAirButton_, playButton_);
  setTabOrder(playButton_, pauseButton_);
  setTabOrder(pauseButton_, ftbButton_);

  seekBar_ = new RangeMarkerSlider(this);
  seekBar_->setObjectName(QStringLiteral("programSeekBar"));
  seekBar_->setAccessibleName(vtrpon::uiText("program.seek.accessible"));
  seekBar_->setHardwareSkinMode(true);
  seekBar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *remainingTimePanel = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  remainingTimePanel->setObjectName(
      QStringLiteral("programRemainingModule"));
  remainingTimePanel->setAccessibleName(
      vtrpon::uiText("program.remaining.panel_accessible"));
  remainingTimePanel->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Fixed);
  auto *remainingTimeLayout = new QVBoxLayout(remainingTimePanel);
  remainingTimeLayout->setContentsMargins(vtrpon::scalePx(10),
                                           vtrpon::scalePx(2),
                                           vtrpon::scalePx(10),
                                           vtrpon::scalePx(8));
  remainingTimeLayout->setSpacing(vtrpon::scalePx(2));
  auto *remainingTimeHeader = new QWidget(remainingTimePanel);
  remainingTimeHeader->setObjectName(
      QStringLiteral("programRemainingHeader"));
  remainingTimeHeader->setFixedHeight(vtrpon::scalePx(19));
  auto *remainingTimeHeaderLayout = new QHBoxLayout(remainingTimeHeader);
  remainingTimeHeaderLayout->setContentsMargins(0, 0, 0, 0);
  remainingTimeHeaderLayout->setSpacing(0);
  auto *remainingTimeHeaderBalance = new QWidget(remainingTimeHeader);
  remainingTimeHeaderBalance->setFixedWidth(vtrpon::scalePx(48));
  remainingTimeHeaderLayout->addWidget(remainingTimeHeaderBalance);
  auto *remainingTimeTitle = new QLabel(
      vtrpon::uiText("program.remaining.title"), remainingTimeHeader);
  remainingTimeTitle->setObjectName(
      QStringLiteral("programControlModuleTitle"));
  remainingTimeTitle->setAlignment(Qt::AlignCenter);
  remainingTimeTitle->setFixedHeight(vtrpon::scalePx(19));
  remainingTimeHeaderLayout->addWidget(remainingTimeTitle, 1);
  totalModeButton_ = new QPushButton(
      vtrpon::uiText("program.remaining.total_mode"),
      remainingTimeHeader);
  totalModeButton_->setObjectName(
      QStringLiteral("programRemainingTotalModeButton"));
  totalModeButton_->setProperty("remainingModeButton", true);
  totalModeButton_->setCheckable(true);
  totalModeButton_->setFixedSize(vtrpon::scaleSize(48, 19));
  remainingTimeHeaderLayout->addWidget(totalModeButton_);
  remainingTimeLayout->addWidget(remainingTimeHeader);

  remainingTimeLabel_ = new QLabel(remainingTimePanel);
  remainingTimeLabel_->setObjectName(QStringLiteral("programRemainingTime"));
  remainingTimeLabel_->setAlignment(Qt::AlignCenter);
  remainingTimeLabel_->setFixedHeight(vtrpon::scalePx(44));
  remainingTimeLabel_->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
  QFont remainingTimeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  remainingTimeFont.setPointSizeF(22.0 * vtrpon::uiScale());
  remainingTimeFont.setWeight(QFont::DemiBold);
  remainingTimeLabel_->setFont(remainingTimeFont);
  remainingTimeLayout->addWidget(remainingTimeLabel_);
  connect(totalModeButton_, &QPushButton::toggled, this,
          [this] { updateRemainingPresentation(); });
  applyTimeSettings();
  setRemainingTime(0);

  auto *endModePanel = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  endModePanel->setObjectName(QStringLiteral("programEndModeModule"));
  endModePanel->setAccessibleName(
      vtrpon::uiText("program.end_mode.panel_accessible"));
  endModePanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *endModeLayout = new QVBoxLayout(endModePanel);
  endModeLayout->setContentsMargins(vtrpon::scalePx(10),
                                    vtrpon::scalePx(2),
                                    vtrpon::scalePx(10),
                                    vtrpon::scalePx(8));
  endModeLayout->setSpacing(vtrpon::scalePx(2));
  auto *endModeTitle = new QLabel(vtrpon::uiText("edit.end_mode.title"),
                                  endModePanel);
  endModeTitle->setObjectName(QStringLiteral("programControlModuleTitle"));
  endModeTitle->setAlignment(Qt::AlignCenter);
  endModeTitle->setFixedHeight(vtrpon::scalePx(15));
  endModeLayout->addWidget(endModeTitle);

  endModeLabel_ = new QLabel(endModePanel);
  endModeLabel_->setObjectName(QStringLiteral("programEndModeValue"));
  endModeLabel_->setAlignment(Qt::AlignCenter);
  endModeLabel_->setMinimumHeight(vtrpon::scalePx(30));
  endModeLabel_->setWordWrap(true);
  endModeLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  endModeLayout->addWidget(endModeLabel_);
  clearEndMode();

  playbackRatePanel_ = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  playbackRatePanel_->setObjectName(
      QStringLiteral("programPlaybackRateModule"));
  playbackRatePanel_->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Fixed);
  auto *playbackRateLayout = new QVBoxLayout(playbackRatePanel_);
  playbackRateLayout->setContentsMargins(vtrpon::scalePx(10),
                                         vtrpon::scalePx(2),
                                         vtrpon::scalePx(10),
                                         vtrpon::scalePx(8));
  playbackRateLayout->setSpacing(vtrpon::scalePx(2));
  playbackRateTitle_ = new QLabel(playbackRatePanel_);
  playbackRateTitle_->setObjectName(
      QStringLiteral("programControlModuleTitle"));
  playbackRateTitle_->setAlignment(Qt::AlignCenter);
  playbackRateTitle_->setFixedHeight(vtrpon::scalePx(15));
  playbackRateLayout->addWidget(playbackRateTitle_);

  auto *playbackRateButtonsLayout = new QHBoxLayout;
  playbackRateButtonsLayout->setContentsMargins(0, 0, 0, 0);
  playbackRateButtonsLayout->setSpacing(
      vtrpon::scalePx(kPlaybackRateButtonSpacing));
  playbackRateButtonsLayout->setAlignment(Qt::AlignHCenter);
  for (std::size_t index = 0; index < kPlaybackRates.size(); ++index) {
    const double rate = kPlaybackRates[index];
    auto *button = new QPushButton(QString::number(rate), playbackRatePanel_);
    button->setObjectName(
        QString::fromLatin1(kPlaybackRateButtonNames[index]));
    button->setProperty("playbackRateButton", true);
    button->setProperty("modeButton", true);
    button->setProperty("playbackRate", rate);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedSize(vtrpon::scaleSize(kPlaybackRateButtonWidth,
                                           kCompactButtonHeight));
    playbackRateButtons_[index] = button;
    playbackRateButtonsLayout->addWidget(button, 1);
    connect(button, &QPushButton::clicked, this, [this, rate] {
      if (std::abs(playbackRate_ - rate) < 0.0001) {
        return;
      }
      emit playbackRateRequested(rate);
    });
  }
  playbackRateLayout->addLayout(playbackRateButtonsLayout);
  setPlaybackRate(1.0);
  setPlaybackRateControlsEnabled(false);

  bgmControlPanel_ = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  bgmControlPanel_->setObjectName(QStringLiteral("programBgmControlModule"));
  bgmControlPanel_->setSizePolicy(QSizePolicy::Expanding,
                                  QSizePolicy::Fixed);
  auto *bgmLayout = new QVBoxLayout(bgmControlPanel_);
  bgmLayout->setContentsMargins(vtrpon::scalePx(8), vtrpon::scalePx(3),
                                vtrpon::scalePx(8), vtrpon::scalePx(5));
  bgmLayout->setSpacing(vtrpon::scalePx(4));

  auto *bgmHeader = new QHBoxLayout;
  bgmHeader->setContentsMargins(0, 0, 0, 0);
  bgmHeader->setSpacing(vtrpon::scalePx(5));
  bgmControlTitle_ = new QLabel(bgmControlPanel_);
  bgmControlTitle_->setObjectName(QStringLiteral("programControlModuleTitle"));
  bgmHeader->addWidget(bgmControlTitle_);
  bgmItemLabel_ = new QLabel(bgmControlPanel_);
  bgmItemLabel_->setObjectName(QStringLiteral("programBgmItemLabel"));
  bgmItemLabel_->setMinimumWidth(0);
  bgmItemLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  bgmItemLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  bgmHeader->addWidget(bgmItemLabel_, 1);
  bgmSignalIndicator_ = new QLabel(QStringLiteral("SIG"), bgmControlPanel_);
  bgmSignalIndicator_->setObjectName(QStringLiteral("programBgmSignalIndicator"));
  bgmSignalIndicator_->setProperty("active", false);
  bgmSignalIndicator_->setProperty("clipped", false);
  bgmSignalIndicator_->setAlignment(Qt::AlignCenter);
  bgmSignalIndicator_->setFixedSize(vtrpon::scaleSize(28, 18));
  bgmHeader->addWidget(bgmSignalIndicator_);
  bgmLayout->addLayout(bgmHeader);

  auto *bgmTransportLayout = new QHBoxLayout;
  bgmTransportLayout->setContentsMargins(0, 0, 0, 0);
  bgmTransportLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  bgmTransportLayout->addStretch(1);
  bgmFadeInControl_ = createFadeControl(
      QStringLiteral("programBgmFadeIn"), QStringLiteral("F/I"),
      bgmControlPanel_);
  bgmFadeOutControl_ = createFadeControl(
      QStringLiteral("programBgmFadeOut"), QStringLiteral("F/O"),
      bgmControlPanel_);
  bgmStopButton_ = new QPushButton(bgmControlPanel_);
  bgmPlayButton_ = new QPushButton(bgmControlPanel_);
  bgmPauseButton_ = new QPushButton(bgmControlPanel_);
  bgmTransportLayout->addWidget(bgmStopButton_);
  bgmTransportLayout->addWidget(bgmPlayButton_);
  bgmTransportLayout->addWidget(bgmPauseButton_);
  bgmFaderButton_ = new QPushButton(bgmControlPanel_);
  bgmFaderButton_->setObjectName(QStringLiteral("programBgmFaderButton"));
  bgmFaderButton_->setCheckable(true);
  bgmFaderButton_->setProperty("bgmTransportButton", true);
  bgmFaderButton_->setFixedHeight(vtrpon::scalePx(kCompactButtonHeight));
  bgmFaderButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  bgmTransportLayout->addWidget(bgmFaderButton_);
  bgmTransportLayout->addStretch(1);
  bgmStopButton_->setObjectName(QStringLiteral("programBgmStopButton"));
  bgmPlayButton_->setObjectName(QStringLiteral("programBgmPlayButton"));
  bgmPauseButton_->setObjectName(QStringLiteral("programBgmPauseButton"));
  for (QPushButton *button :
       {bgmStopButton_, bgmPlayButton_, bgmPauseButton_}) {
    button->setProperty("bgmTransportButton", true);
    button->setFixedSize(vtrpon::scaleSize(kBgmTransportButtonWidth,
                                          kCompactButtonHeight));
  }
  bgmFadeBlinkTimer_ = new QTimer(this);
  bgmFadeBlinkTimer_->setObjectName(
      QStringLiteral("programBgmFadeBlinkTimer"));
  bgmFadeBlinkTimer_->setInterval(kBgmFadeBlinkIntervalMs);
  connect(bgmFadeBlinkTimer_, &QTimer::timeout, this, [this] {
    bgmFadeBlinkOn_ = !bgmFadeBlinkOn_;
    updateBgmFadeButtonAppearance();
  });
  bgmLayout->addLayout(bgmTransportLayout);
  bgmControlPanel_->setVisible(false);

  powerPointNavigationPanel_ = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  powerPointNavigationPanel_->setObjectName(
      QStringLiteral("powerPointNavigationPanel"));
  powerPointNavigationPanel_->setSizePolicy(QSizePolicy::Expanding,
                                             QSizePolicy::Fixed);
  auto *powerPointLayout = new QVBoxLayout(powerPointNavigationPanel_);
  powerPointLayout->setContentsMargins(vtrpon::scalePx(10),
                                       vtrpon::scalePx(2),
                                       vtrpon::scalePx(10),
                                       vtrpon::scalePx(8));
  powerPointLayout->setSpacing(vtrpon::scalePx(2));
  powerPointNavigationTitle_ =
      new QLabel(powerPointNavigationPanel_);
  powerPointNavigationTitle_->setObjectName(
      QStringLiteral("programControlModuleTitle"));
  powerPointNavigationTitle_->setAlignment(Qt::AlignCenter);
  powerPointNavigationTitle_->setFixedHeight(vtrpon::scalePx(15));
  powerPointLayout->addWidget(powerPointNavigationTitle_);
  auto *powerPointButtonLayout = new QHBoxLayout;
  powerPointButtonLayout->setContentsMargins(0, 0, 0, 0);
  powerPointButtonLayout->setSpacing(vtrpon::scalePx(6));
  powerPointPreviousButton_ =
      addButton(powerPointButtonLayout, QString());
  powerPointPositionLabel_ =
      new QLabel(QStringLiteral("0 / 0"), powerPointNavigationPanel_);
  powerPointPositionLabel_->setObjectName(
      QStringLiteral("powerPointPositionLabel"));
  powerPointPositionLabel_->setAlignment(Qt::AlignCenter);
  powerPointPositionLabel_->setMinimumWidth(vtrpon::scalePx(68));
  powerPointButtonLayout->addWidget(powerPointPositionLabel_);
  powerPointNextButton_ = addButton(powerPointButtonLayout, QString());
  powerPointPreviousButton_->setObjectName(
      QStringLiteral("powerPointPreviousButton"));
  powerPointNextButton_->setObjectName(
      QStringLiteral("powerPointNextButton"));
  for (QPushButton *button :
       {powerPointPreviousButton_, powerPointNextButton_}) {
    button->setMinimumHeight(vtrpon::scalePx(kTransportButtonHeight));
  }
  powerPointLayout->addLayout(powerPointButtonLayout);
  powerPointNavigationPanel_->hide();

  auto *audioStrip = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::DualPlate, this);
  audioStrip->setObjectName(QStringLiteral("programMasterAudioStrip"));
  audioStrip->setAccessibleName(vtrpon::uiText("program.audio.accessible"));
  audioStrip->setAttribute(Qt::WA_StyledBackground, true);
  audioStrip_ = audioStrip;
  audioStrip->setFixedWidth(vtrpon::scalePx(kAudioStripWidth));
  audioStrip->setMinimumHeight(vtrpon::scalePx(kFaderBankMinimumHeight));
  audioStrip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  auto *audioStripLayout = new QGridLayout(audioStrip);
  audioStripLayout->setContentsMargins(vtrpon::scalePx(5),
                                       vtrpon::scalePx(3),
                                       vtrpon::scalePx(5),
                                       vtrpon::scalePx(4));
  audioStripLayout->setHorizontalSpacing(vtrpon::scalePx(4));
  audioStripLayout->setVerticalSpacing(vtrpon::scalePx(4));

  auto *itemLabel = new QLabel(vtrpon::uiText("program.item"), audioStrip);
  itemLabel->setObjectName(QStringLiteral("itemFaderLabel"));
  itemLabel->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
  itemLabel->setFixedHeight(vtrpon::scalePx(16));
  audioStripLayout->addWidget(itemLabel, 0, 0);

  auto *itemFaderMeterGroup = new QWidget(audioStrip);
  itemFaderMeterGroup->setObjectName(
      QStringLiteral("programItemFaderMeterGroup"));
  itemFaderMeterGroup->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Expanding);
  auto *itemFaderMeterLayout = new QHBoxLayout(itemFaderMeterGroup);
  itemFaderMeterLayout->setContentsMargins(0, 0, 0, 0);
  itemFaderMeterLayout->setSpacing(vtrpon::scalePx(8));
  itemFaderMeterLayout->setAlignment(Qt::AlignHCenter);

  itemFader_ = new AudioFaderWidget(AudioFaderWidget::Role::Item,
                                    itemFaderMeterGroup);
  itemFader_->setObjectName(QStringLiteral("programItemFader"));
  itemFader_->setExtendedTopRange(true);
  itemFaderMeterLayout->addWidget(itemFader_);
  bgmFader_ = new AudioFaderWidget(AudioFaderWidget::Role::Item,
                                  itemFaderMeterGroup);
  bgmFader_->setObjectName(QStringLiteral("programBgmFader"));
  bgmFader_->setProperty("blueFaderCap", true);
  bgmFader_->setAccessibleName(QStringLiteral("BGM"));
  bgmFader_->setExtendedTopRange(true);
  bgmFader_->hide();
  itemFaderMeterLayout->addWidget(bgmFader_);
  connect(bgmFader_, &QSlider::valueChanged, this,
          [this] { emit bgmVolumeChanged(bgmFader_->linearGain()); });

  auto *itemMeterContainer = new QWidget(itemFaderMeterGroup);
  itemMeterContainer->setObjectName(
      QStringLiteral("programItemMeterContainer"));
  itemMeterContainer->setFixedWidth(vtrpon::scalePx(25));
  itemMeterContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  auto *itemMeterStack = new QStackedLayout(itemMeterContainer);
  itemMeterStack->setContentsMargins(0, 0, 0, 0);
  itemMeterStack->setStackingMode(QStackedLayout::StackAll);
  itemMeter_ = new AudioMeterWidget(itemMeterContainer);
  itemMeter_->setObjectName(QStringLiteral("programItemAudioMeter"));
  itemMeter_->setAccessibleName(
      vtrpon::uiText("program.item_meter.accessible"));
  itemMeter_->setCompactStripMode(true);
  itemMeter_->setExtendedTopRange(true);
  itemMeterStack->addWidget(itemMeter_);
  auto *inputSignalOverlay = new QWidget(itemMeterContainer);
  inputSignalOverlay->setObjectName(QStringLiteral("transparentOverlay"));
  auto *inputLayout = new QVBoxLayout(inputSignalOverlay);
  inputLayout->setContentsMargins(0, 0, 0, vtrpon::scalePx(9));
  inputLayout->setSpacing(0);
  inputLayout->addStretch();
  inputSignalIndicator_ = new QLabel(QStringLiteral("SIG"), inputSignalOverlay);
  inputSignalIndicator_->setObjectName(
      QStringLiteral("programItemInputSignal"));
  inputSignalIndicator_->setAccessibleName(
      vtrpon::uiText("program.input_signal.accessible"));
  inputSignalIndicator_->setFixedSize(vtrpon::scaleSize(25, 22));
  inputSignalIndicator_->setAlignment(Qt::AlignCenter);
  inputLayout->addWidget(inputSignalIndicator_);
  itemMeterStack->addWidget(inputSignalOverlay);
  itemMeterStack->setCurrentWidget(inputSignalOverlay);
  itemFaderMeterLayout->addWidget(itemMeterContainer);
  bgmMeter_ = new AudioMeterWidget(itemFaderMeterGroup);
  bgmMeter_->setObjectName(QStringLiteral("programBgmAudioMeter"));
  bgmMeter_->setAccessibleName(QStringLiteral("BGM Post-Fader"));
  bgmMeter_->setToolTip(QStringLiteral("BGM Post-Fader"));
  bgmMeter_->setFixedWidth(vtrpon::scalePx(25));
  bgmMeter_->setCompactStripMode(true);
  bgmMeter_->setExtendedTopRange(true);
  bgmMeter_->hide();
  itemFaderMeterLayout->addWidget(bgmMeter_);
  audioStripLayout->addWidget(itemFaderMeterGroup, 1, 0);

  itemFadeInControl_ = createFadeControl(
      QStringLiteral("programItemFadeIn"), QStringLiteral("F/I"), audioStrip);
  itemFadeOutControl_ = createFadeControl(
      QStringLiteral("programItemFadeOut"), QStringLiteral("F/O"), audioStrip);
  audioStripLayout->addWidget(itemFadeInControl_.control, 2, 0,
                              Qt::AlignHCenter);
  audioStripLayout->addWidget(itemFadeOutControl_.control, 3, 0,
                              Qt::AlignHCenter);

  audioStripLayout->addWidget(bgmFadeInControl_.control, 2, 0, Qt::AlignHCenter);
  audioStripLayout->addWidget(bgmFadeOutControl_.control, 3, 0, Qt::AlignHCenter);
  bgmFadeInControl_.control->hide();
  bgmFadeOutControl_.control->hide();
  bgmFadeInControl_.button->setFixedHeight(vtrpon::scalePx(kFaderButtonHeight));
  bgmFadeOutControl_.button->setFixedHeight(vtrpon::scalePx(kFaderButtonHeight));

  auto *separator = new QFrame(audioStrip);
  separator->setObjectName(QStringLiteral("faderSeparator"));
  separator->setFrameShape(QFrame::VLine);
  separator->setFrameShadow(QFrame::Plain);
  audioStripLayout->addWidget(separator, 0, 1, 5, 1);

  auto *masterLabel = new QLabel(vtrpon::uiText("program.master"), audioStrip);
  masterLabel->setObjectName(QStringLiteral("masterFaderLabel"));
  masterLabel->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
  masterLabel->setFixedHeight(vtrpon::scalePx(16));
  audioStripLayout->addWidget(masterLabel, 0, 2);

  auto *masterFaderMeterGroup = new QWidget(audioStrip);
  masterFaderMeterGroup->setObjectName(
      QStringLiteral("programMasterFaderMeterGroup"));
  masterFaderMeterGroup->setSizePolicy(QSizePolicy::Expanding,
                                       QSizePolicy::Expanding);
  auto *masterFaderMeterLayout = new QHBoxLayout(masterFaderMeterGroup);
  masterFaderMeterLayout->setContentsMargins(0, 0, 0, 0);
  masterFaderMeterLayout->setSpacing(vtrpon::scalePx(8));
  masterFaderMeterLayout->setAlignment(Qt::AlignHCenter);

  masterFader_ = new AudioFaderWidget(masterFaderMeterGroup);
  masterFader_->setObjectName(QStringLiteral("programMasterFader"));
  masterFader_->setExtendedTopRange(true);
  masterFaderMeterLayout->addWidget(masterFader_);

  auto *masterMeterContainer = new QWidget(masterFaderMeterGroup);
  masterMeterContainer->setObjectName(
      QStringLiteral("programMasterMeterContainer"));
  masterMeterContainer->setFixedWidth(vtrpon::scalePx(25));
  masterMeterContainer->setSizePolicy(QSizePolicy::Fixed,
                                      QSizePolicy::Expanding);
  auto *masterMeterStack = new QStackedLayout(masterMeterContainer);
  masterMeterStack->setContentsMargins(0, 0, 0, 0);
  masterMeterStack->setStackingMode(QStackedLayout::StackAll);
  masterMeter_ = new AudioMeterWidget(masterMeterContainer);
  masterMeter_->setObjectName(QStringLiteral("programAudioMeter"));
  masterMeter_->setAccessibleName(
      vtrpon::uiText("program.master_meter.accessible"));
  masterMeter_->setCompactStripMode(true);
  masterMeter_->setExtendedTopRange(true);
  masterMeterStack->addWidget(masterMeter_);
  auto *muteOverlay = new QWidget(masterMeterContainer);
  muteOverlay->setObjectName(QStringLiteral("transparentOverlay"));
  auto *muteLayout = new QVBoxLayout(muteOverlay);
  muteLayout->setContentsMargins(0, 0, 0, vtrpon::scalePx(9));
  muteLayout->setSpacing(0);
  muteLayout->addStretch();
  muteButton_ = new QPushButton(vtrpon::uiText("program.mute"), muteOverlay);
  muteButton_->setObjectName(QStringLiteral("programMasterMute"));
  muteButton_->setAccessibleName(vtrpon::uiText("program.mute.accessible"));
  muteButton_->setToolTip(vtrpon::uiText("program.mute.tooltip"));
  muteButton_->setCheckable(true);
  muteButton_->setFixedSize(vtrpon::scaleSize(25, 22));
  muteLayout->addWidget(muteButton_);
  masterMeterStack->addWidget(muteOverlay);
  masterMeterStack->setCurrentWidget(muteOverlay);
  masterFaderMeterLayout->addWidget(masterMeterContainer);
  audioStripLayout->addWidget(masterFaderMeterGroup, 1, 2);

  masterFadeInControl_ = createFadeControl(
      QStringLiteral("programMasterFadeIn"), QStringLiteral("F/I"), audioStrip);
  masterFadeOutControl_ = createFadeControl(
      QStringLiteral("programMasterFadeOut"), QStringLiteral("F/O"), audioStrip);
  // Keep the 24 px duration controls and row pitch; smaller action buttons
  // leave more clearance above and below their hit areas.
  for (QPushButton *button : {itemFadeInControl_.button, itemFadeOutControl_.button,
                             masterFadeInControl_.button, masterFadeOutControl_.button}) {
    button->setFixedHeight(vtrpon::scalePx(kFaderButtonHeight));
  }
  audioStripLayout->addWidget(masterFadeInControl_.control, 2, 2,
                              Qt::AlignHCenter);
  audioStripLayout->addWidget(masterFadeOutControl_.control, 3, 2,
                              Qt::AlignHCenter);
  masterEqButton_ = new QPushButton(QStringLiteral("EQ"), audioStrip);
  masterEqButton_->setObjectName(QStringLiteral("programMasterEqButton"));
  masterEqButton_->setProperty("programFadeButton", true);
  masterEqButton_->setFixedSize(vtrpon::scaleSize(kFadeButtonWidth, kFaderButtonHeight));
  auto *masterDelayControl = vtrpon::createAudioDelayControl(audioStrip,
      QStringLiteral("programMasterDelaySpin"), "audio.delay.master");
  masterDelaySpin_ = masterDelayControl->spinBox();
  auto *masterEqRow = new QHBoxLayout;
  masterEqRow->setSpacing(vtrpon::scalePx(4));
  masterEqRow->addWidget(masterEqButton_);
  masterEqRow->addWidget(masterDelayControl);
  audioStripLayout->addLayout(masterEqRow, 4, 2, Qt::AlignHCenter);
  connect(masterDelaySpin_, &QSpinBox::valueChanged, this, &ProgramPanel::masterDelayChanged);
  itemEqButton_ = new QPushButton(QStringLiteral("EQ"), audioStrip);
  itemEqButton_->setObjectName(QStringLiteral("programItemEqButton"));
  itemEqButton_->setProperty("programFadeButton", true);
  itemEqButton_->setFixedSize(vtrpon::scaleSize(kFadeButtonWidth, kFaderButtonHeight));
  itemEqButton_->setEnabled(false);
  auto *itemDelayControl = vtrpon::createAudioDelayControl(audioStrip,
      QStringLiteral("programItemDelaySpin"), "audio.delay.item");
  itemDelaySpin_ = itemDelayControl->spinBox();
  itemDelaySpin_->setEnabled(false);
  auto *itemEqRow = new QHBoxLayout;
  itemEqRow->setSpacing(vtrpon::scalePx(4));
  itemEqRow->addWidget(itemEqButton_);
  itemEqRow->addWidget(itemDelayControl);
  audioStripLayout->addLayout(itemEqRow, 4, 0, Qt::AlignHCenter);
  connect(itemDelaySpin_, &QSpinBox::valueChanged, this, &ProgramPanel::itemDelayChanged);
  vtrpon::bindVstButton(itemEqButton_, vtrpon::VstSlot::Item);
  vtrpon::bindVstButton(masterEqButton_, vtrpon::VstSlot::Master);
  connect(&vtrpon::VstService::instance(), &vtrpon::VstService::changed, this, [this] {
    if (vtrpon::vstModeEnabled()) {
      if (itemEqEditor_) itemEqEditor_->hide();
      if (masterEqWindow_) masterEqWindow_->hide();
    }
    vtrpon::updateEqButton(itemEqButton_, itemEqTenthsDb_, "item.eq.title");
    updateMasterEqPresentation();
  });
  connect(bgmFaderButton_, &QPushButton::toggled, this,
          [this, itemLabel, itemMeterContainer, itemDelayControl](bool bgm) {
    bgmFaderSelected_ = bgm;
    itemLabel->setText(bgm ? QStringLiteral("BGM") : vtrpon::uiText("program.item"));
    bgmFaderButton_->setText(vtrpon::uiText(bgm ? "program.bgm.show_item_fader" : "program.bgm.show_fader"));
    itemFader_->setVisible(!bgm);
    itemMeterContainer->setVisible(!bgm);
    bgmFader_->setVisible(bgm);
    bgmMeter_->setVisible(bgm);
    bgmFadeInControl_.control->setVisible(bgm);
    bgmFadeOutControl_.control->setVisible(bgm);
    itemFadeInControl_.control->setVisible(!bgm);
    itemFadeOutControl_.control->setVisible(!bgm);
    itemEqButton_->setVisible(!bgm);
    itemDelayControl->setVisible(!bgm);
  });
  audioStripLayout->setColumnStretch(0, 1);
  audioStripLayout->setColumnStretch(2, 1);
  audioStripLayout->setRowStretch(1, 1);

  auto *faderBank = new QHBoxLayout;
  faderBank->setContentsMargins(0, 0, 0, 0);
  faderBank->setSpacing(vtrpon::scalePx(12));
  auto *playoutStatus = new QVBoxLayout;
  playoutStatus->setContentsMargins(0, 0, 0, 0);
  playoutStatus->setSpacing(vtrpon::scalePx(kPanelSpacing));
  playoutStatus->addWidget(remainingTimePanel);
  playoutStatus->addWidget(endModePanel);
  playoutStatus->addWidget(playbackRatePanel_);
  playoutStatus->addWidget(bgmControlPanel_);
  playoutStatus->addWidget(powerPointNavigationPanel_);
  playoutStatus->addStretch(1);
  playoutStatus->addWidget(effectsPanel);
  faderBank->addLayout(playoutStatus, 1);
  auto *audioColumn = new QVBoxLayout;
  audioColumn->setContentsMargins(0, 0, 0, 0);
  audioColumn->setSpacing(vtrpon::scalePx(kPanelSpacing));
  audioColumn->addWidget(audioStrip, 1);
  operatorTimerPanel_ = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  operatorTimerPanel_->setObjectName(QStringLiteral("operatorTimerControlModule"));
  operatorTimerPanel_->setFixedWidth(vtrpon::scalePx(kAudioStripWidth));
  operatorTimerPanel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto *timerLayout = new QVBoxLayout(operatorTimerPanel_);
  timerLayout->setContentsMargins(vtrpon::scalePx(10), vtrpon::scalePx(2),
                                 vtrpon::scalePx(10), vtrpon::scalePx(8));
  timerLayout->setSpacing(vtrpon::scalePx(2));
  operatorTimerTitle_ = new QLabel(operatorTimerPanel_);
  operatorTimerTitle_->setAlignment(Qt::AlignCenter);
  operatorTimerTitle_->setObjectName(QStringLiteral("operatorTimerTitle"));
  operatorTimerTitle_->setFixedHeight(vtrpon::scalePx(15));
  timerLayout->addWidget(operatorTimerTitle_);
  operatorTimerRemaining_ = new QLabel(QStringLiteral("05:00"), operatorTimerPanel_);
  operatorTimerRemaining_->setObjectName(QStringLiteral("operatorTimerRemaining"));
  operatorTimerRemaining_->setFixedSize(vtrpon::scaleSize(132, 28));
  operatorTimerRemaining_->setFont(remainingTimeFont);
  operatorTimerRemaining_->setAlignment(Qt::AlignCenter);
  auto *timerControls = new QGridLayout;
  timerControls->setSpacing(vtrpon::scalePx(4));
  for (int column = 0; column < 3; ++column) timerControls->setColumnStretch(column, 1);
  timerControls->addWidget(operatorTimerRemaining_, 0, 0, 1, 2, Qt::AlignLeft);
  operatorTimerSettings_ = new QPushButton(operatorTimerPanel_);
  operatorTimerSettings_->setObjectName(QStringLiteral("operatorTimerSettingsButton"));
  operatorTimerSettings_->setProperty("operatorTimerButton", true);
  operatorTimerSettings_->setAutoDefault(false);
  operatorTimerSettings_->setFixedHeight(vtrpon::scalePx(kCompactButtonHeight));
  timerControls->addWidget(operatorTimerSettings_, 0, 2);
  connect(operatorTimerSettings_, &QPushButton::clicked, this, &ProgramPanel::operatorTimerSettingsRequested);
  operatorTimerStart_ = new QPushButton(operatorTimerPanel_);
  operatorTimerPause_ = new QPushButton(operatorTimerPanel_);
  operatorTimerReset_ = new QPushButton(operatorTimerPanel_);
  operatorTimerStart_->setObjectName(QStringLiteral("operatorTimerStart"));
  operatorTimerPause_->setObjectName(QStringLiteral("operatorTimerPause"));
  operatorTimerReset_->setObjectName(QStringLiteral("operatorTimerReset"));
  int timerColumn = 0;
  for (auto *button : {operatorTimerStart_, operatorTimerPause_, operatorTimerReset_}) {
    button->setProperty("operatorTimerButton", true);
    button->setAutoDefault(false);
    button->setFixedHeight(vtrpon::scalePx(kCompactButtonHeight));
    timerControls->addWidget(button, 1, timerColumn++);
  }
  timerLayout->addLayout(timerControls);
  connect(operatorTimerStart_, &QPushButton::clicked, this, &ProgramPanel::operatorTimerStartRequested);
  connect(operatorTimerPause_, &QPushButton::clicked, this, &ProgramPanel::operatorTimerPauseRequested);
  connect(operatorTimerReset_, &QPushButton::clicked, this, &ProgramPanel::operatorTimerResetRequested);
  setTabOrder(operatorTimerSettings_, operatorTimerStart_);
  setTabOrder(operatorTimerStart_, operatorTimerPause_);
  setTabOrder(operatorTimerPause_, operatorTimerReset_);
  setTabOrder(operatorTimerReset_, offAirButton_);
  audioColumn->addWidget(operatorTimerPanel_);
  operatorTimerPanel_->hide();
  faderBank->addLayout(audioColumn);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin));
  layout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  layout->addLayout(sectionHeader);
  layout->addWidget(monitorWindow_);
  layout->addWidget(fileLabel_);
  layout->addWidget(seekBar_);
  layout->addLayout(faderBank, 1);
  layout->addWidget(actionPanel);
  layout->activate();

  manualFadeAnimation_ = new QVariantAnimation(this);
  manualFadeAnimation_->setObjectName(
      QStringLiteral("programManualFadeAnimation"));
  manualFadeAnimation_->setEasingCurve(QEasingCurve::Linear);
  connect(manualFadeAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            AudioFaderWidget *fader = nullptr;
            if (manualFadeTarget_ == ManualFadeTarget::Item) {
              fader = itemFader_;
            } else if (manualFadeTarget_ == ManualFadeTarget::Master) {
              fader = masterFader_;
            }
            if (fader) {
              fader->setLinearGain(value.toDouble());
            }
          });
  connect(manualFadeAnimation_, &QVariantAnimation::finished, this, [this] {
    if (activeFadeButton_) {
      setButtonActive(activeFadeButton_, false);
    }
    activeFadeButton_ = nullptr;
    manualFadeTarget_ = ManualFadeTarget::None;
  });

  connect(offAirButton_, &QPushButton::clicked, this,
          &ProgramPanel::offAirRequested);
  connect(playButton_, &QPushButton::clicked, this,
          &ProgramPanel::playRequested);
  connect(pauseButton_, &QPushButton::clicked, this,
          &ProgramPanel::pauseRequested);
  connect(ftbButton_, &QPushButton::clicked, this, &ProgramPanel::ftbRequested);
  connect(dskOnButton_, &QPushButton::clicked, this,
          &ProgramPanel::dskToggleRequested);
  connect(dskPlayButton_, &QPushButton::clicked, this,
          &ProgramPanel::dskPlayRequested);
  connect(dskPauseButton_, &QPushButton::clicked, this,
          &ProgramPanel::dskPauseRequested);
  connect(powerPointPreviousButton_, &QPushButton::clicked, this,
          &ProgramPanel::powerPointPreviousRequested);
  connect(powerPointNextButton_, &QPushButton::clicked, this,
          &ProgramPanel::powerPointNextRequested);
  connect(seekBar_, &QSlider::sliderMoved, this,
          [this] { emit seekRequested(seekBar_->timelinePosition()); });
  connect(seekBar_, &QSlider::sliderReleased, this,
          [this] { emit seekRequested(seekBar_->timelinePosition()); });
  connect(itemFader_, &QSlider::valueChanged, this,
          [this] { emit itemVolumeChanged(itemFader_->linearGain()); });
  connect(masterFader_, &QSlider::valueChanged, this,
          [this] { emit masterVolumeChanged(masterFader_->linearGain()); });
  connect(bgmStopButton_, &QPushButton::clicked, this,
          &ProgramPanel::bgmStopRequested);
  connect(bgmPlayButton_, &QPushButton::clicked, this,
          &ProgramPanel::bgmPlayRequested);
  connect(bgmPauseButton_, &QPushButton::clicked, this,
          &ProgramPanel::bgmPauseRequested);
  operations_ = operations ? operations : new ControllerOperations(this);
  using A = ControllerOperations::Action;
  // Availability is Controller state, independent of whether this window is
  // shown. The same guards apply to buttons, shortcuts and external inputs.
  operations_->setGuard<>(A::DocumentPrevious, this, [this] {
    return documentNavigationVisible_ && !documentNavigationBusy_
        && documentCanGoPrevious_;
  });
  operations_->setGuard<>(A::DocumentNext, this, [this] {
    return documentNavigationVisible_ && !documentNavigationBusy_
        && documentCanGoNext_;
  });
  operations_->setGuard<>(A::OffAir, this, [this] {
    return hasOnAirMedia_;
  });
  operations_->setGuard<>(A::ToggleFtb, this, [this] {
    return ftbControlsEnabled_;
  });
  operations_->setGuard<>(A::ToggleDsk, this, [this] {
    return dskOn_ || dskCueState_ == DskCueState::Loading
        || dskSelectionAvailable_;
  });
  for (const auto action : {A::ProgramPlay, A::ProgramPause,
                            A::ProgramTogglePlayback}) {
    operations_->setGuard<>(action, this, [this] {
      return hasOnAirMedia_ && !documentNavigationVisible_;
    });
  }
  operations_->setGuard<>(A::OpenItemEq, this, [this] {
    return hasOnAirMedia_ && !bgmFaderSelected_;
  });
  operations_->bindMember(A::OpenItemEq, this, &ProgramPanel::showItemEqWindow);
  operations_->bindMember(A::OpenMasterEq, this, &ProgramPanel::showMasterEqWindow);
  connect(itemEqButton_, &QPushButton::clicked, this,
          [this] { (void)operations_->execute(A::OpenItemEq); });
  connect(masterEqButton_, &QPushButton::clicked, this,
          [this] { (void)operations_->execute(A::OpenMasterEq); });
  const auto cacheFadeDuration = [this](FadeControlWidgets &control) {
    connect(control.durationSpinBox, &QDoubleSpinBox::valueChanged, this,
            [&control](double value) { control.durationSeconds = value; });
  };
  cacheFadeDuration(bgmFadeInControl_);
  cacheFadeDuration(bgmFadeOutControl_);
  connect(bgmFadeInControl_.button, &QPushButton::clicked, this,
          [this] { (void)operations_->execute(A::BgmFadeInConfigured); });
  connect(bgmFadeOutControl_.button, &QPushButton::clicked, this,
          [this] { (void)operations_->execute(A::BgmFadeOutConfigured); });
  const auto bindVolume = [this](A set, A adjust, A reset, AudioFaderWidget *fader) {
    operations_->setGuard<int>(set, this, [fader](int value) {
      return fader->isEnabled() && value >= fader->minimum()
          && value <= fader->maximum();
    });
    operations_->setGuard<int>(adjust, this,
                              [fader](int) { return fader->isEnabled(); });
    operations_->setGuard<>(reset, this,
                           [fader] { return fader->isEnabled(); });
    const auto applyValue = [this, fader](int value) {
      if (fader != bgmFader_) handleUserFaderOperation(fader);
      fader->setValue(value);
      return true;
    };
    operations_->bind<int>(set, this, applyValue);
    operations_->bind<int>(adjust, this, [fader, applyValue](int delta) {
      const auto value = std::clamp(qint64(fader->value()) + delta,
                                   qint64(fader->minimum()), qint64(fader->maximum()));
      return applyValue(int(value));
    });
    operations_->bind<>(reset, this, [applyValue] { return applyValue(0); });
    fader->setUserValueHandler([this, set](int value) { (void)operations_->execute(set, value); });
  };
  bindVolume(A::SetItemVolume, A::AdjustItemVolume, A::ResetItemVolume, itemFader_);
  bindVolume(A::SetBgmVolume, A::AdjustBgmVolume, A::ResetBgmVolume, bgmFader_);
  bindVolume(A::SetMasterVolume, A::AdjustMasterVolume, A::ResetMasterVolume, masterFader_);
  const auto bindFade = [this, &cacheFadeDuration](A action, AudioFaderWidget *fader,
                             FadeControlWidgets &control, bool fadeIn,
                             ManualFadeTarget target) {
    auto *button = control.button;
    cacheFadeDuration(control);
    operations_->setGuard<>(action, this, [this, target] {
      return target != ManualFadeTarget::Item || !bgmFaderSelected_;
    });
    operations_->bind<>(action, this, [this, fader, button, &control, fadeIn, target] {
      const double gain = fadeIn ? (target == ManualFadeTarget::Item ? itemNominalGain_ : 1.0) : 0.0;
      startManualFade(fader, button, control.durationSeconds, gain, target);
      return true;
    });
    connect(button, &QPushButton::clicked, this,
            [this, action] { (void)operations_->execute(action); });
  };
  bindFade(A::ItemFadeIn, itemFader_, itemFadeInControl_, true, ManualFadeTarget::Item);
  bindFade(A::ItemFadeOut, itemFader_, itemFadeOutControl_, false, ManualFadeTarget::Item);
  bindFade(A::MasterFadeIn, masterFader_, masterFadeInControl_, true, ManualFadeTarget::Master);
  bindFade(A::MasterFadeOut, masterFader_, masterFadeOutControl_, false, ManualFadeTarget::Master);
  operations_->setGuard<bool>(A::MasterMute, this,
      [this](bool) { return muteButton_->isEnabled(); });
  operations_->bind<bool>(A::MasterMute, this, [this](bool muted) {
    muteButton_->setChecked(muted);
    emit masterMuteChanged(muted);
    return true;
  });
  connect(muteButton_, &QPushButton::clicked, this,
          [this](bool muted) { (void)operations_->execute(A::MasterMute, muted); });

  itemFader_->installEventFilter(this);
  masterFader_->installEventFilter(this);
  setTabOrder(seekBar_, totalModeButton_);
  setTabOrder(totalModeButton_, playbackRateButtons_.front());
  for (std::size_t index = 1; index < playbackRateButtons_.size(); ++index) {
    setTabOrder(playbackRateButtons_[index - 1],
                playbackRateButtons_[index]);
  }
  setTabOrder(playbackRateButtons_.back(), bgmStopButton_);
  setTabOrder(bgmStopButton_, bgmPlayButton_);
  setTabOrder(bgmPlayButton_, bgmPauseButton_);
  setTabOrder(bgmPauseButton_, bgmFaderButton_);
  setTabOrder(bgmFaderButton_, bgmFader_);
  setTabOrder(bgmFader_, bgmFadeOutControl_.button);
  setTabOrder(bgmFadeOutControl_.button,
              bgmFadeOutControl_.durationSpinBox);
  setTabOrder(bgmFadeOutControl_.durationSpinBox,
              bgmFadeOutControl_.increaseButton);
  setTabOrder(bgmFadeOutControl_.increaseButton,
              bgmFadeOutControl_.decreaseButton);
  setTabOrder(bgmFadeOutControl_.decreaseButton,
              bgmFadeInControl_.button);
  setTabOrder(bgmFadeInControl_.button,
              bgmFadeInControl_.durationSpinBox);
  setTabOrder(bgmFadeInControl_.durationSpinBox,
              bgmFadeInControl_.increaseButton);
  setTabOrder(bgmFadeInControl_.increaseButton,
              bgmFadeInControl_.decreaseButton);
  setTabOrder(bgmFadeInControl_.decreaseButton,
              powerPointPreviousButton_);
  setTabOrder(powerPointPreviousButton_, powerPointNextButton_);
  setTabOrder(powerPointNextButton_, itemFader_);
  setTabOrder(itemFader_, masterFader_);
  setTabOrder(masterFader_, muteButton_);
  setTabOrder(muteButton_, masterEqButton_);
  setTabOrder(muteButton_, itemFadeInControl_.button);
  setTabOrder(itemFadeInControl_.button,
              itemFadeInControl_.durationSpinBox);
  setTabOrder(itemFadeInControl_.durationSpinBox,
              itemFadeInControl_.increaseButton);
  setTabOrder(itemFadeInControl_.increaseButton,
              itemFadeInControl_.decreaseButton);
  setTabOrder(itemFadeInControl_.decreaseButton,
              masterFadeInControl_.button);
  setTabOrder(masterFadeInControl_.button,
              masterFadeInControl_.durationSpinBox);
  setTabOrder(masterFadeInControl_.durationSpinBox,
              masterFadeInControl_.increaseButton);
  setTabOrder(masterFadeInControl_.increaseButton,
              masterFadeInControl_.decreaseButton);
  setTabOrder(masterFadeInControl_.decreaseButton,
              itemFadeOutControl_.button);
  setTabOrder(itemFadeOutControl_.button,
              itemFadeOutControl_.durationSpinBox);
  setTabOrder(itemFadeOutControl_.durationSpinBox,
              itemFadeOutControl_.increaseButton);
  setTabOrder(itemFadeOutControl_.increaseButton,
              itemFadeOutControl_.decreaseButton);
  setTabOrder(itemFadeOutControl_.decreaseButton,
              masterFadeOutControl_.button);
  setTabOrder(masterFadeOutControl_.button,
              masterFadeOutControl_.durationSpinBox);
  setTabOrder(masterFadeOutControl_.durationSpinBox,
              masterFadeOutControl_.increaseButton);
  setTabOrder(masterFadeOutControl_.increaseButton,
              masterFadeOutControl_.decreaseButton);
  setTabOrder(masterFadeOutControl_.decreaseButton, dskOnButton_);

  setPlaybackState(false, false);
  setFtbButtonActive(false);
  updateDskControls();
  setMuteAppearance(false, masterFaderAtSilence());
  setInputSignalAppearance(false, false);
  resetAudioPresentation();
  retranslateUi();
  vtrpon::preserveFocusOnButtonClick(this);
}

QWidget *ProgramPanel::monitorWindow() const { return monitorWindow_; }

void ProgramPanel::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
}

void ProgramPanel::paintEvent(QPaintEvent *event) {
  QWidget::paintEvent(event);
  if (!monitorWindow_ || fileName_.isEmpty()) {
    return;
  }
  QWidget *monitor = monitorWindow_;
  const QRectF frameRect(monitor->mapTo(this, QPoint(0, 0)), monitor->size());
  QPainter painter(this);
  vtrpon::drawMonitorNeonOutline(painter, frameRect,
                                QColor(QStringLiteral("#ed555c")));
}

bool ProgramPanel::eventFilter(QObject *watched, QEvent *event) {
  AudioFaderWidget *fader = nullptr;
  if (watched == itemFader_) {
    fader = itemFader_;
  } else if (watched == masterFader_) {
    fader = masterFader_;
  }
  if (!fader) {
    return QWidget::eventFilter(watched, event);
  }

  bool userOperation = false;
  if (event->type() == QEvent::MouseButtonPress
      || event->type() == QEvent::MouseButtonDblClick) {
    const auto *mouseEvent = static_cast<QMouseEvent *>(event);
    userOperation = mouseEvent->button() == Qt::LeftButton;
  }
  if (userOperation) {
    handleUserFaderOperation(fader);
  }
  return QWidget::eventFilter(watched, event);
}

void ProgramPanel::setPlayerBackground(const QColor &color) {
  playerBackgroundColor_ = color.isValid() ? QColor(color.rgb()) : QColor(Qt::black);
  updateBackgroundPresentation();
}

void ProgramPanel::updateBackgroundPresentation() {
  backgroundColorButton_->setIcon(backgroundSwatch(playerBackgroundColor_));
  const QString description = vtrpon::uiText("settings.category.player_background")
      + QStringLiteral(" (%1)").arg(playerBackgroundColor_.name().toUpper());
  backgroundColorButton_->setAccessibleName(description);
  backgroundColorButton_->setToolTip(description);
  for (auto *action : backgroundColorActions_) {
    const QSignalBlocker blocker(action);
    action->setChecked(action->data().value<QColor>() == playerBackgroundColor_);
  }
  otherBackgroundColorAction_->setIcon(backgroundSwatch(playerBackgroundColor_));
}

void ProgramPanel::setItemDelayFrames(int frames) {
  const QSignalBlocker blocker(itemDelaySpin_);
  itemDelaySpin_->setValue(frames);
}

void ProgramPanel::setItemEqGains(const std::array<int, 3> &gains) {
  itemEqTenthsDb_ = gains;
  vtrpon::updateEqButton(itemEqButton_, gains, "item.eq.title");
  if (itemEqEditor_) itemEqEditor_->setGains(gains);
}

void ProgramPanel::showItemEqWindow() {
  if (vtrpon::showVstEditor(itemEqButton_, vtrpon::VstSlot::Item)) return;
  if (!itemEqEditor_) {
    itemEqEditor_ = new vtrpon::EqualizerEditor(this, QStringLiteral("programItemEq"),
        "item.eq.title", [this](std::array<int, 3> gains) {
          if (!hasOnAirMedia_) return;
          setItemEqGains(gains);
          emit itemEqChanged(gains[0] / 10.0, gains[1] / 10.0, gains[2] / 10.0);
        });
  }
  itemEqEditor_->setGains(itemEqTenthsDb_);
  itemEqEditor_->openEditor();
}

void ProgramPanel::showMasterEqWindow() {
  if (vtrpon::showVstEditor(masterEqButton_, vtrpon::VstSlot::Master)) return;
  if (!masterEqWindow_) {
    // Parent ownership keeps one modeless window and its settings for the
    // lifetime of the panel. Closing it hides it; it never disables the EQ.
    masterEqWindow_ = new vtrpon::MasterEqWindow(this, Qt::Window | Qt::CustomizeWindowHint
        | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    masterEqWindow_->setObjectName(QStringLiteral("programMasterEqWindow"));
    masterEqWindow_->setModal(false);
    auto *layout = new QVBoxLayout(masterEqWindow_);
    layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                               vtrpon::scalePx(12), vtrpon::scalePx(12));
    layout->setSpacing(vtrpon::scalePx(4));
    constexpr std::array<const char *, 3> names{"LOW", "MID", "HIGH"};
    const auto &frequencies = vtrpon::kEqFrequencyLabels;
    for (int band = 2; band >= 0; --band) {
      auto *title = new QLabel(QStringLiteral("%1  /  %2")
                                  .arg(QString::fromLatin1(names[band]),
                                       QString::fromLatin1(frequencies[band])),
                              masterEqWindow_);
      title->setAlignment(Qt::AlignCenter);
      layout->addWidget(title);
      auto *dial = new vtrpon::MasterEqDial(masterEqWindow_);
      masterEqDials_[band] = dial;
      dial->setObjectName(QStringLiteral("programMasterEq%1Dial")
                              .arg(QString::fromLatin1(names[band])));
      dial->setAccessibleName(title->text());
      dial->setRange(-vtrpon::kEqMaximumTenthsDb / 5, vtrpon::kEqMaximumTenthsDb / 5);
      dial->setSingleStep(1);
      dial->setPageStep(2);
      dial->setNotchesVisible(true);
      dial->setWrapping(false);
      dial->setFixedSize(vtrpon::scaleSize(60, 60));
      dial->setValue(qRound(masterEqDb_[band] * 2.0));
      layout->addWidget(dial, 0, Qt::AlignHCenter);
      auto *value = new QLabel(masterEqWindow_);
      value->setAlignment(Qt::AlignCenter);
      masterEqValueLabels_[band] = value;
      layout->addWidget(value);
      connect(dial, &QDial::valueChanged, this, [this, band](int value) {
        masterEqDb_[band] = value / 2.0;
        updateMasterEqPresentation();
        emit masterEqChanged(masterEqDb_[0], masterEqDb_[1], masterEqDb_[2]);
      });
    }
    masterEqResetButton_ = new QPushButton(masterEqWindow_);
    masterEqResetButton_->setObjectName(QStringLiteral("programMasterEqResetButton"));
    masterEqResetButton_->setAutoDefault(false);
    masterEqResetButton_->setMinimumHeight(vtrpon::scalePx(24));
    // Leave room for the native title bar text and Windows caption buttons.
    masterEqResetButton_->setMinimumWidth(vtrpon::scalePx(176));
    layout->addWidget(masterEqResetButton_);
    connect(masterEqResetButton_, &QPushButton::clicked, this, [this] {
      masterEqDb_.fill(0.0);
      for (auto *dial : masterEqDials_) {
        const QSignalBlocker blocker(dial);
        dial->setValue(0);
      }
      updateMasterEqPresentation();
      emit masterEqChanged(0.0, 0.0, 0.0);
    });
    setTabOrder(masterEqDials_[2], masterEqDials_[1]);
    setTabOrder(masterEqDials_[1], masterEqDials_[0]);
    setTabOrder(masterEqDials_[0], masterEqResetButton_);
    layout->setSizeConstraint(QLayout::SetFixedSize);
  }
  updateMasterEqPresentation();
  masterEqWindow_->showNormal();
  masterEqWindow_->raise();
  masterEqWindow_->activateWindow();
}

void ProgramPanel::updateMasterEqPresentation() {
  if (vtrpon::updateVstButton(masterEqButton_)) {
    if (masterEqWindow_) masterEqWindow_->hide();
    return;
  }
  const bool adjusted = std::any_of(masterEqDb_.begin(), masterEqDb_.end(),
                                    [](double gain) { return gain != 0.0; });
  masterEqButton_->setToolTip(vtrpon::uiText(adjusted
      ? "program.eq.adjusted" : "program.eq.flat"));
  masterEqButton_->setAccessibleName(masterEqButton_->toolTip());
  if (!masterEqButton_->property("eqAdjusted").isValid()
      || masterEqButton_->property("eqAdjusted").toBool() != adjusted) {
    masterEqButton_->setProperty("eqAdjusted", adjusted);
    masterEqButton_->style()->unpolish(masterEqButton_);
    masterEqButton_->style()->polish(masterEqButton_);
    masterEqButton_->update();
  }
  if (masterEqWindow_) {
    masterEqWindow_->setWindowTitle(vtrpon::uiText("program.eq.title"));
    masterEqResetButton_->setText(vtrpon::uiText("program.eq.reset"));
    for (int band = 0; band < 3; ++band) {
      const double gain = masterEqDb_[band];
      const QString text = (gain > 0.0 ? QStringLiteral("+") : QString())
          + masterEqWindow_->locale().toString(gain, 'f', 1) + QStringLiteral(" dB");
      masterEqValueLabels_[band]->setText(text);
      masterEqDials_[band]->setToolTip(text);
      masterEqDials_[band]->setAccessibleDescription(text);
    }
  }
}

void ProgramPanel::retranslateUi() {
  vtrpon::retranslateAudioDelayControl(itemDelaySpin_, "audio.delay.item");
  vtrpon::retranslateAudioDelayControl(masterDelaySpin_, "audio.delay.master");
  vtrpon::updateEqButton(itemEqButton_, itemEqTenthsDb_, "item.eq.title");
  updateMasterEqPresentation();
  operatorTimerTitle_->setText(vtrpon::uiText("operator_timer.title"));
  operatorTimerSettings_->setText(vtrpon::uiText("settings.title"));
  operatorTimerSettings_->setToolTip(vtrpon::uiText("operator_timer.settings"));
  operatorTimerStart_->setText(vtrpon::uiText("operator_timer.start"));
  operatorTimerPause_->setText(vtrpon::uiText("common.pause"));
  operatorTimerReset_->setText(vtrpon::uiText("operator_timer.reset"));
  operatorTimerRemaining_->setAccessibleName(vtrpon::uiText("operator_monitor.remaining"));
  setOperatorTimerPresentation(operatorTimerPresentation_);
  backgroundColorTitleAction_->setText(vtrpon::uiText("settings.background.color"));
  const char *colorIds[] = {"settings.background.black", "settings.background.green",
      "settings.background.blue", "settings.background.white"};
  for (int i = 0; i < 4; ++i)
    backgroundColorActions_[i]->setText(vtrpon::uiText(colorIds[i]));
  otherBackgroundColorAction_->setText(vtrpon::uiText("settings.background.other"));
  backgroundColorMenu_->setTitle(vtrpon::uiText("settings.category.player_background"));
  updateBackgroundPresentation();
  findChild<QLabel *>(QStringLiteral("programSectionTitle"))
      ->setText(vtrpon::uiText("program.section"));
  findChild<QWidget *>(QStringLiteral("programActionPanel"))
      ->setAccessibleName(vtrpon::uiText("program.actions.accessible"));
  findChild<QWidget *>(QStringLiteral("programEffectsActionPanel"))
      ->setAccessibleName(vtrpon::uiText("program.effects.accessible"));
  findChild<QWidget *>(QStringLiteral("programTransportActionPanel"))
      ->setAccessibleName(vtrpon::uiText("program.transport.accessible"));
  findChild<QWidget *>(QStringLiteral("programFtbActionPanel"))
      ->setAccessibleName(vtrpon::uiText("program.ftb.accessible"));
  itemMeter_->setAccessibleName(vtrpon::uiText("program.item_meter.accessible"));
  masterMeter_->setAccessibleName(vtrpon::uiText("program.master_meter.accessible"));
  offAirButton_->setText(vtrpon::uiText("program.off_air"));
  ftbButton_->setText(QStringLiteral("FTB"));
  dskOnButton_->setText(QStringLiteral("DSK"));
  dskPlayButton_->setText(vtrpon::uiText("common.play").toUpper());
  dskPauseButton_->setText(vtrpon::uiText("common.pause").toUpper());
  dskPlayButton_->setAccessibleName(
      vtrpon::uiText("program.dsk.play.tooltip"));
  dskPauseButton_->setAccessibleName(
      vtrpon::uiText("program.dsk.pause.tooltip"));
  dskPlayButton_->setToolTip(vtrpon::uiText("program.dsk.play.tooltip"));
  dskPauseButton_->setToolTip(vtrpon::uiText("program.dsk.pause.tooltip"));
  pauseButton_->setText(vtrpon::uiText("common.pause").toUpper());
  playButton_->setText(vtrpon::uiText("common.play").toUpper());
  offAirButton_->setAccessibleName(vtrpon::uiText("program.off_air.accessible"));
  ftbButton_->setAccessibleName(vtrpon::uiText("program.ftb.accessible"));
  pauseButton_->setAccessibleName(vtrpon::uiText("program.pause.accessible"));
  playButton_->setAccessibleName(vtrpon::uiText("program.play.accessible"));
  seekBar_->setAccessibleName(vtrpon::uiText("program.seek.accessible"));
  totalModeButton_->setText(
      vtrpon::uiText("program.remaining.total_mode"));
  totalModeButton_->setAccessibleName(
      vtrpon::uiText("program.remaining.total_mode.accessible"));
  totalModeButton_->setToolTip(
      vtrpon::uiText("program.remaining.total_mode.tooltip"));
  QWidget *remainingModule =
      findChild<QWidget *>(QStringLiteral("programRemainingModule"));
  remainingModule->setAccessibleName(
      vtrpon::uiText("program.remaining.panel_accessible"));
  remainingModule->findChild<QLabel *>(
      QStringLiteral("programControlModuleTitle"))
      ->setText(vtrpon::uiText("program.remaining.title"));
  QWidget *endModeModule =
      findChild<QWidget *>(QStringLiteral("programEndModeModule"));
  endModeModule->setAccessibleName(
      vtrpon::uiText("program.end_mode.panel_accessible"));
  endModeModule->findChild<QLabel *>(
      QStringLiteral("programControlModuleTitle"))
      ->setText(vtrpon::uiText("edit.end_mode.title"));
  playbackRatePanel_->setAccessibleName(
      vtrpon::uiText("program.playback_rate.panel_accessible"));
  playbackRateTitle_->setText(
      vtrpon::uiText("program.playback_rate.title"));
  for (QPushButton *button : playbackRateButtons_) {
    const QString rate = button->text();
    const QString accessibleName =
        vtrpon::uiText("program.playback_rate.button.accessible").arg(rate);
    button->setAccessibleName(accessibleName);
    button->setToolTip(accessibleName);
  }
  bgmControlPanel_->setAccessibleName(
      vtrpon::uiText("program.bgm.panel_accessible"));
  bgmControlTitle_->setText(vtrpon::uiText("program.bgm.title"));
  bgmStopButton_->setText(vtrpon::uiText("common.stop").toUpper());
  bgmPlayButton_->setText(vtrpon::uiText("common.play").toUpper());
  bgmPauseButton_->setText(vtrpon::uiText("common.pause").toUpper());
  bgmStopButton_->setAccessibleName(
      vtrpon::uiText("program.bgm.stop.accessible"));
  bgmPlayButton_->setAccessibleName(
      vtrpon::uiText("program.bgm.play.accessible"));
  bgmPauseButton_->setAccessibleName(
      vtrpon::uiText("program.bgm.pause.accessible"));
  bgmSignalIndicator_->setAccessibleName(
      vtrpon::uiText("program.bgm.signal.accessible"));
  retranslateFadeControl(bgmFadeInControl_, QStringLiteral("BGM"),
                         vtrpon::uiText("edit.fade_in"));
  retranslateFadeControl(bgmFadeOutControl_, QStringLiteral("BGM"),
                         vtrpon::uiText("edit.fade_out"));
  const bool pdfNavigation =
      documentNavigationKind_ == DocumentNavigationKind::Pdf;
  powerPointNavigationPanel_->setAccessibleName(
      vtrpon::uiText(pdfNavigation ? "program.pdf.panel_accessible"
                                   : "program.powerpoint.panel_accessible"));
  powerPointNavigationTitle_->setText(
      vtrpon::uiText(pdfNavigation ? "program.pdf.title"
                                   : "program.powerpoint.title"));
  powerPointPreviousButton_->setText(
      vtrpon::uiText(pdfNavigation ? "program.pdf.previous"
                                   : "program.powerpoint.previous"));
  powerPointPreviousButton_->setAccessibleName(
      vtrpon::uiText(pdfNavigation
                         ? "program.pdf.previous.accessible"
                         : "program.powerpoint.previous.accessible"));
  powerPointNextButton_->setText(
      vtrpon::uiText(pdfNavigation ? "program.pdf.next"
                                   : "program.powerpoint.next"));
  powerPointNextButton_->setAccessibleName(
      vtrpon::uiText(pdfNavigation ? "program.pdf.next.accessible"
                                   : "program.powerpoint.next.accessible"));
  findChild<QWidget *>(QStringLiteral("programMasterAudioStrip"))
      ->setAccessibleName(vtrpon::uiText("program.audio.accessible"));
  inputSignalIndicator_->setText(QStringLiteral("SIG"));
  inputSignalIndicator_->setAccessibleName(
      vtrpon::uiText("program.input_signal.accessible"));
  muteButton_->setText(vtrpon::uiText("program.mute"));
  muteButton_->setAccessibleName(vtrpon::uiText("program.mute.accessible"));
  muteButton_->setToolTip(vtrpon::uiText("program.mute.tooltip"));
  findChild<QLabel *>(QStringLiteral("itemFaderLabel"))
      ->setText(bgmFaderButton_->isChecked() ? QStringLiteral("BGM") : vtrpon::uiText("program.item"));
  bgmFaderButton_->setText(vtrpon::uiText(bgmFaderButton_->isChecked() ? "program.bgm.show_item_fader" : "program.bgm.show_fader"));
  findChild<QLabel *>(QStringLiteral("masterFaderLabel"))
      ->setText(vtrpon::uiText("program.master"));
  retranslateFadeControl(itemFadeInControl_,
                         vtrpon::uiText("program.item"),
                         vtrpon::uiText("edit.fade_in"));
  retranslateFadeControl(itemFadeOutControl_,
                         vtrpon::uiText("program.item"),
                         vtrpon::uiText("edit.fade_out"));
  retranslateFadeControl(masterFadeInControl_,
                         vtrpon::uiText("program.master"),
                         vtrpon::uiText("edit.fade_in"));
  retranslateFadeControl(masterFadeOutControl_,
                         vtrpon::uiText("program.master"),
                         vtrpon::uiText("edit.fade_out"));
  if (playPauseShortcut_.isEmpty()) {
    playButton_->setToolTip(vtrpon::uiText("program.play.tooltip"));
  } else {
    setPlayPauseShortcutLabel(playPauseShortcut_);
  }
  updateFilePresentation();
  updateRemainingPresentation();
  updateEndModePresentation();
  updateDskControls();
  setInputSignalAppearance(inputSignalActive_, inputSignalClipped_);
  updateBgmPresentation();
}

void ProgramPanel::setPlayPauseShortcutLabel(const QString &shortcut) {
  playPauseShortcut_ = shortcut;
  pauseButton_->setToolTip(
      vtrpon::uiText("program.pause.shortcut_tooltip").arg(shortcut));
  playButton_->setToolTip(
      vtrpon::uiText("program.play.shortcut_tooltip").arg(shortcut));
}

void ProgramPanel::setFile(const QString &fileName,
                           const QString &absolutePath) {
  fileName_ = fileName;
  fileAbsolutePath_ = absolutePath;
  programFault_ = false;
  programFaultDetail_.clear();
  updateFilePresentation();
}

void ProgramPanel::clearFile() {
  fileName_.clear();
  fileAbsolutePath_.clear();
  programFault_ = false;
  programFaultDetail_.clear();
  updateFilePresentation();
}

QStringList ProgramPanel::operatorMonitorStatus() const {
  if (!hasOnAirMedia_) return {QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"), QStringLiteral("—"), QString()};
  QString modes = QStringLiteral("—");
  if (endModeSettings_) {
    const QString start = vtrpon::uiText(endModeSettings_->startMode == PlaylistStartMode::Play
        ? "common.play" : "common.pause");
    modes = start + QStringLiteral(" / ") +
        vtrpon::formatPlaylistItemEndMode(*endModeSettings_, currentPlayCount_);
  }
  const QString pages = documentNavigationVisible_ && documentCurrentPage_ > 0 && documentPageCount_ > 0
      ? QStringLiteral("%1 / %2").arg(documentPageCount_).arg(documentCurrentPage_) : QString();
  return {remainingTimeLabel_->text(), fileName_, modes,
          QStringLiteral("%1×").arg(playbackRate_, 0, 'g', 3), pages};
}

void ProgramPanel::setProgramFault(const QString &detail) {
  programFault_ = true;
  programFaultDetail_ = detail;
  updateFilePresentation();
}

void ProgramPanel::clearProgramFault() {
  programFault_ = false;
  programFaultDetail_.clear();
  updateFilePresentation();
}

void ProgramPanel::updateFilePresentation() {
  update();
  if (programFault_) {
    fileLabel_->setText(fileName_.isEmpty()
                            ? vtrpon::uiText("program.error.label")
                            : vtrpon::uiText("program.error.file_label")
                                  .arg(fileName_));
    fileLabel_->setToolTip(programFaultDetail_);
    fileLabel_->setAccessibleName(
        vtrpon::uiText("program.error.accessible").arg(programFaultDetail_));
  } else if (fileName_.isEmpty()) {
    fileLabel_->setText(vtrpon::uiText("program.file.none"));
    fileLabel_->setToolTip(QString());
    fileLabel_->setAccessibleName(
        vtrpon::uiText("program.file.none_accessible"));
  } else {
    fileLabel_->setText(fileName_);
    fileLabel_->setToolTip(fileAbsolutePath_);
    fileLabel_->setAccessibleName(
        vtrpon::uiText("program.file.current_accessible").arg(fileName_));
  }
  fileLabel_->setProperty("fault", programFault_);
  repolish(fileLabel_);
}

void ProgramPanel::setTimelineDuration(qint64 durationMs) {
  seekBar_->setTimelineDuration(durationMs);
}

void ProgramPanel::setTimelinePosition(qint64 positionMs) {
  seekBar_->setTimelinePosition(positionMs);
}

qint64 ProgramPanel::timelinePosition() const {
  return seekBar_->timelinePosition();
}

bool ProgramPanel::isTimelineSliderDown() const {
  return seekBar_->isSliderDown();
}

void ProgramPanel::setRangeMarkers(qint64 inPointMs,
                                   std::optional<qint64> outPointMs) {
  seekBar_->setRangeMarkers(inPointMs, outPointMs);
}

void ProgramPanel::clearRangeMarkers() { seekBar_->clearRangeMarkers(); }

void ProgramPanel::setRemainingTime(qint64 remainingMs) {
  remainingLive_ = false;
  remainingMs_ = remainingMs;
  updateRemainingPresentation();
}

void ProgramPanel::updateRemainingPresentation() {
  const bool totalMode = totalModeEnabled() &&
                         (totalRemainingMs_ || totalRemainingInfinite_);
  const qint64 wallClockRemainingMs = totalMode && totalRemainingMs_
      ? (std::max)(*totalRemainingMs_, qint64{0})
      : static_cast<qint64>(std::ceil(
            static_cast<double>((std::max)(remainingMs_, qint64{0})) /
            playbackRate_));
  const bool infinite = totalMode && totalRemainingInfinite_;
  const bool live = !totalMode && remainingLive_;
  const QString value = infinite
      ? QString(QChar(0x221e))
      : live ? vtrpon::uiText("common.live")
             : formatRemainingTime(wallClockRemainingMs);
  if (remainingTimeLabel_->text() != value) {
    remainingTimeLabel_->setText(value);
  }
  const QString accessibleName = vtrpon::uiText(
      totalMode ? "program.remaining.total_accessible"
                : "program.remaining.accessible").arg(value);
  if (remainingTimeLabel_->accessibleName() != accessibleName) {
    remainingTimeLabel_->setAccessibleName(accessibleName);
  }

  const bool endingSoon = !live && !infinite && wallClockRemainingMs > 0 &&
                          wallClockRemainingMs <= kRemainingAlertThresholdMs;
  if (remainingTimeLabel_->property("endingSoon").toBool() != endingSoon) {
    remainingTimeLabel_->setProperty("endingSoon", endingSoon);
    repolish(remainingTimeLabel_);
  }
}

void ProgramPanel::setRemainingLive() {
  remainingLive_ = true;
  updateRemainingPresentation();
}

void ProgramPanel::setTotalRemainingTime(qint64 remainingMs) {
  const qint64 boundedRemainingMs = (std::max)(remainingMs, qint64{0});
  if (totalRemainingMs_ == boundedRemainingMs &&
      !totalRemainingInfinite_) {
    return;
  }
  totalRemainingMs_ = boundedRemainingMs;
  totalRemainingInfinite_ = false;
  updateRemainingPresentation();
}

void ProgramPanel::setTotalRemainingInfinite() {
  if (!totalRemainingMs_ && totalRemainingInfinite_) {
    return;
  }
  totalRemainingMs_.reset();
  totalRemainingInfinite_ = true;
  updateRemainingPresentation();
}

void ProgramPanel::clearTotalRemainingTime() {
  if (!totalRemainingMs_ && !totalRemainingInfinite_) {
    return;
  }
  totalRemainingMs_.reset();
  totalRemainingInfinite_ = false;
  updateRemainingPresentation();
}

void ProgramPanel::applyTimeSettings() {
  totalModeButton_->setChecked(vtrpon::savedRemainingTotal());
}

bool ProgramPanel::totalModeEnabled() const {
  return totalModeButton_ && totalModeButton_->isChecked();
}

void ProgramPanel::setPowerPointNavigationState(
    bool visible, bool canGoPrevious, bool canGoNext, int currentSlide,
    int slideCount, bool busy) {
  documentNavigationKind_ = DocumentNavigationKind::PowerPoint;
  applyDocumentNavigationState(visible, canGoPrevious, canGoNext,
                               currentSlide, slideCount, busy);
}

void ProgramPanel::setPdfNavigationState(
    bool visible, bool canGoPrevious, bool canGoNext, int currentPage,
    int pageCount, bool busy) {
  documentNavigationKind_ = DocumentNavigationKind::Pdf;
  applyDocumentNavigationState(visible, canGoPrevious, canGoNext,
                               currentPage, pageCount, busy);
}

void ProgramPanel::applyDocumentNavigationState(
    bool visible, bool canGoPrevious, bool canGoNext, int currentPosition,
    int positionCount, bool busy) {
  documentCurrentPage_ = visible ? currentPosition : 0;
  documentPageCount_ = visible ? positionCount : 0;
  const bool pdfNavigation =
      documentNavigationKind_ == DocumentNavigationKind::Pdf;
  powerPointNavigationPanel_->setAccessibleName(
      vtrpon::uiText(pdfNavigation ? "program.pdf.panel_accessible"
                                   : "program.powerpoint.panel_accessible"));
  powerPointNavigationTitle_->setText(
      vtrpon::uiText(pdfNavigation ? "program.pdf.title"
                                   : "program.powerpoint.title"));
  powerPointPreviousButton_->setText(
      vtrpon::uiText(pdfNavigation ? "program.pdf.previous"
                                   : "program.powerpoint.previous"));
  powerPointPreviousButton_->setAccessibleName(
      vtrpon::uiText(pdfNavigation
                         ? "program.pdf.previous.accessible"
                         : "program.powerpoint.previous.accessible"));
  powerPointNextButton_->setText(
      vtrpon::uiText(pdfNavigation ? "program.pdf.next"
                                   : "program.powerpoint.next"));
  powerPointNextButton_->setAccessibleName(
      vtrpon::uiText(pdfNavigation ? "program.pdf.next.accessible"
                                   : "program.powerpoint.next.accessible"));
  powerPointNavigationPanel_->setVisible(visible);
  seekBar_->setEnabled(!visible);
  documentNavigationVisible_ = visible;
  documentNavigationBusy_ = visible && busy;
  documentCanGoPrevious_ = visible && canGoPrevious;
  documentCanGoNext_ = visible && canGoNext;
  pauseButton_->setEnabled(hasOnAirMedia_ && !visible);
  playButton_->setEnabled(hasOnAirMedia_ && !visible);
  if (!visible) {
    powerPointPreviousButton_->setEnabled(false);
    powerPointNextButton_->setEnabled(false);
    powerPointPositionLabel_->setText(QStringLiteral("0 / 0"));
    powerPointPositionLabel_->setAccessibleName(
        vtrpon::uiText(pdfNavigation
                           ? "program.pdf.position_unavailable"
                           : "program.powerpoint.position_unavailable"));
    return;
  }

  // PowerPoint's transient COM/transition wait must not extinguish both
  // buttons. Keep page-edge availability visible; gate input separately.
  const bool disableForBusy = pdfNavigation && busy;
  powerPointPreviousButton_->setEnabled(!disableForBusy && canGoPrevious);
  powerPointNextButton_->setEnabled(!disableForBusy && canGoNext);
  const bool positionAvailable = currentPosition > 0 && positionCount > 0;
  const QString position = positionAvailable
      ? QStringLiteral("%1 / %2").arg(currentPosition).arg(positionCount)
      : QStringLiteral("- / -");
  powerPointPositionLabel_->setText(position);
  powerPointPositionLabel_->setAccessibleName(
      positionAvailable
          ? vtrpon::uiText(pdfNavigation
                               ? "program.pdf.position_accessible"
                               : "program.powerpoint.position_accessible")
                .arg(currentPosition)
                .arg(positionCount)
          : vtrpon::uiText(pdfNavigation
                               ? "program.pdf.position_unavailable"
                               : "program.powerpoint.position_unavailable"));
  powerPointPreviousButton_->setToolTip(
      vtrpon::uiText(pdfNavigation ? "program.pdf.previous.tooltip"
                                   : "program.powerpoint.previous.tooltip")
          .arg(position));
  powerPointNextButton_->setToolTip(
      canGoNext
          ? vtrpon::uiText(pdfNavigation ? "program.pdf.next.tooltip"
                                         : "program.powerpoint.next.tooltip")
                .arg(position)
          : vtrpon::uiText(pdfNavigation ? "program.pdf.last.tooltip"
                                         : "program.powerpoint.last.tooltip")
                .arg(position));
  const QString stateDescription = busy
      ? vtrpon::uiText(pdfNavigation ? "program.pdf.busy"
                                     : "program.powerpoint.busy")
      : position;
  powerPointPreviousButton_->setAccessibleDescription(stateDescription);
  powerPointNextButton_->setAccessibleDescription(stateDescription);
}

void ProgramPanel::setEndMode(const PlaylistItemSettings &settings,
                              int currentPlayCount) {
  endModeSettings_ = settings;
  currentPlayCount_ = (std::max)(currentPlayCount, 0);
  updateEndModePresentation();
}

void ProgramPanel::clearEndMode() {
  endModeSettings_.reset();
  currentPlayCount_ = 0;
  updateEndModePresentation();
}

void ProgramPanel::setOffAirButtonVisible(bool visible) {
  offAirButton_->setVisible(visible);
}

void ProgramPanel::setFtbControlsVisible(bool visible) {
  ftbControlsEnabled_ = visible;
  if (ftbPanel_) {
    ftbPanel_->setVisible(visible);
  }
}

void ProgramPanel::setPlaybackRateControlsVisible(bool visible) {
  if (playbackRatePanel_) {
    playbackRatePanel_->setVisible(visible);
  }
}

void ProgramPanel::setPlaybackRateControlsEnabled(bool enabled) {
  for (QPushButton *button : playbackRateButtons_) {
    button->setEnabled(enabled);
  }
}

void ProgramPanel::setPlaybackRate(double rate) {
  if (!std::isfinite(rate) || rate <= 0.0) {
    return;
  }
  playbackRate_ = rate;
  for (QPushButton *button : playbackRateButtons_) {
    const bool selected =
        std::abs(button->property("playbackRate").toDouble() - rate) < 0.0001;
    button->setChecked(selected);
  }
  updateRemainingPresentation();
}

void ProgramPanel::setOperatorTimerPresentation(const QJsonObject &presentation) {
  operatorTimerPresentation_ = presentation;
  const bool enabled = presentation.value("enabled").toBool();
  operatorTimerPanel_->setVisible(enabled);
  operatorTimerRemaining_->setText(presentation.value("text").toString(QStringLiteral("05:00")));
  // These independent controls remain operable whenever the feature is enabled.
  // Repeated start/pause requests are idempotent in OperatorCountdownTimer.
  operatorTimerStart_->setEnabled(enabled);
  operatorTimerPause_->setEnabled(enabled);
  operatorTimerReset_->setEnabled(enabled);
  const bool running = presentation.value("running").toBool();
  const bool started = presentation.value("started").toBool();
  setButtonActive(operatorTimerStart_, enabled && running);
  setButtonActive(operatorTimerPause_, enabled && started && !running);
  setButtonActive(operatorTimerReset_, false);
  const QColor warningColor(presentation.value("warningColor").toString());
  const QString warningBorder = presentation.value("warning").toInt() > 0 && warningColor.isValid()
      ? QStringLiteral("border-color: %1;").arg(warningColor.name()) : QString();
  remainingTimeLabel_->ensurePolished();
  QFont timerFont = remainingTimeLabel_->font();
  timerFont.setPointSizeF(timerFont.pointSizeF() * 0.8);
  // Fit the compact frame and overtime digits using rounded font metrics.
  const int availableWidth = operatorTimerRemaining_->width() - vtrpon::scalePx(16);
  const int availableHeight = operatorTimerRemaining_->height() - 2 * vtrpon::scalePx(2);
  while ((QFontMetrics(timerFont).horizontalAdvance(operatorTimerRemaining_->text()) > availableWidth ||
          QFontMetrics(timerFont).height() > availableHeight) &&
         timerFont.pointSizeF() > 1.0) {
    timerFont.setPointSizeF(std::max(qreal{1.0},
        timerFont.pointSizeF() - 0.25 * vtrpon::uiScale()));
  }
  const qreal timerPointSize = timerFont.pointSizeF() / vtrpon::uiScale();
  operatorTimerRemaining_->setFont(timerFont);
  const QString style = vtrpon::scaledStyleSheet(QStringLiteral(
      "QLabel#operatorTimerRemaining { font-size: %1pt; %2 }"
      "QWidget#operatorTimerControlModule QPushButton { padding: 1px 2px; min-width: 0px; }")
      .arg(timerPointSize, 0, 'f', 4).arg(warningBorder));
  if (operatorTimerPanel_->styleSheet() != style) operatorTimerPanel_->setStyleSheet(style);
  const int timerHeight = enabled ? operatorTimerPanel_->sizeHint().height() + vtrpon::scalePx(kPanelSpacing) : 0;
  audioStrip_->setMinimumHeight(vtrpon::scalePx(kFaderBankMinimumHeight) - timerHeight);
  itemFader_->setMinimumHeight(vtrpon::scalePx(enabled ? 140 : 250));
  bgmFader_->setMinimumHeight(vtrpon::scalePx(enabled ? 140 : 250));
  masterFader_->setMinimumHeight(vtrpon::scalePx(enabled ? 140 : 250));
}

void ProgramPanel::setBgmControlsVisible(bool visible) {
  bgmControlsEnabled_ = visible;
  if (!visible) bgmFaderButton_->setChecked(false);
  bgmFaderButton_->setVisible(visible);
  if (bgmControlPanel_) {
    bgmControlPanel_->setVisible(visible);
  }
}

bool ProgramPanel::bgmControlsVisible() const {
  return bgmControlsEnabled_;
}

bool ProgramPanel::configuredBgmFadeAvailable() const {
  return bgmControlsEnabled_ && bgmFaderSelected_;
}

qint64 ProgramPanel::configuredBgmFadeInDurationMs() const {
  return qRound64(bgmFadeInControl_.durationSeconds * 1000.0);
}

qint64 ProgramPanel::configuredBgmFadeOutDurationMs() const {
  return qRound64(bgmFadeOutControl_.durationSeconds * 1000.0);
}

void ProgramPanel::setBgmPresentation(BgmPresentationState state,
                                      const QString &playlistName,
                                      const QString &itemName,
                                      const QString &detail,
                                      bool volumeAtSilence) {
  if (bgmPresentationState_ != state || bgmItemName_ != itemName)
    qInfo() << "STATE BGM presentation=" << static_cast<int>(state)
            << "item=" << itemName << "detail=" << detail;
  bgmPresentationState_ = state;
  bgmPlaylistName_ = playlistName;
  bgmItemName_ = itemName;
  bgmDetail_ = detail;
  bgmVolumeAtSilence_ = volumeAtSilence;
  updateBgmPresentation();
}

void ProgramPanel::setBgmFadePresentation(BgmFadeState state,
                                          bool volumeAtSilence) {
  bgmFadeState_ = state;
  bgmVolumeAtSilence_ = volumeAtSilence;
  updateBgmPresentation();
}

void ProgramPanel::updateBgmFadeButtonAppearance() {
  const bool fadingIn = bgmFadeState_ == BgmFadeState::FadeIn;
  const bool fadingOut = bgmFadeState_ == BgmFadeState::FadeOut;
  setButtonActive(bgmFadeInControl_.button,
                  fadingIn && bgmFadeBlinkOn_);
  setButtonActive(bgmFadeOutControl_.button,
                  fadingOut ? bgmFadeBlinkOn_
                            : !fadingIn && bgmVolumeAtSilence_);
}

void ProgramPanel::updateBgmPresentation() {
  if (!bgmControlPanel_ || !bgmItemLabel_) {
    return;
  }
  const char *stateId = "program.bgm.state.stopped";
  switch (bgmPresentationState_) {
  case BgmPresentationState::Stopped:
    stateId = "program.bgm.state.stopped";
    break;
  case BgmPresentationState::Loading:
    stateId = "program.bgm.state.loading";
    break;
  case BgmPresentationState::Playing:
    stateId = "program.bgm.state.playing";
    break;
  case BgmPresentationState::Paused:
    stateId = "program.bgm.state.paused";
    break;
  case BgmPresentationState::Error:
    stateId = "program.bgm.state.error";
    break;
  }
  setButtonActive(bgmPlayButton_,
                  bgmPresentationState_ == BgmPresentationState::Playing);
  setButtonActive(bgmPauseButton_,
                  bgmPresentationState_ == BgmPresentationState::Paused);
  const bool fading = bgmFadeState_ != BgmFadeState::Idle;
  if (fading) {
    if (!bgmFadeBlinkTimer_->isActive()) {
      bgmFadeBlinkOn_ = true;
      bgmFadeBlinkTimer_->start();
    }
  } else {
    bgmFadeBlinkTimer_->stop();
    bgmFadeBlinkOn_ = false;
  }
  updateBgmFadeButtonAppearance();
  QString stateText = vtrpon::uiText(stateId);
  if (bgmFadeState_ == BgmFadeState::FadeIn) {
    stateText = QStringLiteral("%1 / %2")
                    .arg(stateText,
                         vtrpon::uiText("program.bgm.state.fade_in"));
  } else if (bgmFadeState_ == BgmFadeState::FadeOut) {
    stateText = QStringLiteral("%1 / %2")
                    .arg(stateText,
                         vtrpon::uiText("program.bgm.state.fade_out"));
  } else if (bgmVolumeAtSilence_) {
    stateText = QStringLiteral("%1 / %2")
                    .arg(stateText,
                         vtrpon::uiText("program.bgm.state.faded_out"));
  }
  bgmControlPanel_->setProperty(
      "error", bgmPresentationState_ == BgmPresentationState::Error);
  repolish(bgmControlPanel_);
  const QString fullName = bgmItemName_.isEmpty()
      ? vtrpon::uiText("program.bgm.no_item")
      : bgmPlaylistName_.isEmpty()
            ? bgmItemName_
            : QStringLiteral("%1 / %2").arg(bgmPlaylistName_, bgmItemName_);
  const int availableWidth = (std::max)(bgmItemLabel_->width(),
                                        vtrpon::scalePx(80));
  bgmItemLabel_->setText(
      bgmItemLabel_->fontMetrics().elidedText(fullName, Qt::ElideMiddle,
                                              availableWidth));
  bgmItemLabel_->setToolTip(bgmDetail_.isEmpty() ? fullName : bgmDetail_);
  bgmItemLabel_->setAccessibleName(
      vtrpon::uiText("program.bgm.current.accessible").arg(fullName));
  bgmControlPanel_->setAccessibleDescription(
      QStringLiteral("%1. %2")
          .arg(vtrpon::uiText("program.bgm.state.accessible").arg(stateText),
               vtrpon::uiText("program.bgm.current.accessible")
                   .arg(fullName)));
}

void ProgramPanel::setBgmSignalAppearance(bool active, bool clipped) {
  bgmSignalActive_ = active;
  bgmSignalClipped_ = clipped;
  if (!bgmSignalIndicator_) {
    return;
  }
  bgmSignalIndicator_->setProperty("active", active);
  bgmSignalIndicator_->setProperty("clipped", clipped);
  bgmSignalIndicator_->setAccessibleDescription(
      vtrpon::uiText(clipped ? "program.bgm.signal.clipped"
                             : active ? "program.bgm.signal.active"
                                      : "program.bgm.signal.inactive"));
  repolish(bgmSignalIndicator_);
}

void ProgramPanel::updateEndModePresentation() {
  const QString value = endModeSettings_
      ? vtrpon::formatPlaylistItemEndMode(*endModeSettings_,
                                         currentPlayCount_).toUpper()
      : QString(QChar(0x2014));
  if (endModeLabel_->text() != value) {
    endModeLabel_->setText(value);
  }
  const QString accessibleName =
      vtrpon::uiText("program.end_mode.accessible").arg(value);
  if (endModeLabel_->accessibleName() != accessibleName) {
    endModeLabel_->setAccessibleName(accessibleName);
  }
}

void ProgramPanel::setItemFaderValue(int value) {
  if (manualFadeTarget_ == ManualFadeTarget::Item) {
    cancelManualFade();
  }
  itemAudioAutomationOverridden_ = false;
  itemFader_->setValue(value);
  itemNominalGain_ = itemFader_->linearGain();
}

void ProgramPanel::setItemFadeProgress(qreal progress) {
  if (itemAudioAutomationOverridden_) {
    return;
  }
  const qreal clampedProgress = std::clamp(progress, qreal{0.0}, qreal{1.0});
  itemFader_->setLinearGain(itemNominalGain_ * clampedProgress);
}

double ProgramPanel::itemGain() const { return itemFader_->linearGain(); }
int ProgramPanel::itemDelayFrames() const { return itemDelaySpin_->value(); }

void ProgramPanel::setBgmFaderGain(double gain) {
  const QSignalBlocker blocker(bgmFader_);
  bgmFader_->setLinearGain(gain);
}

double ProgramPanel::bgmGain() const { return bgmFader_->linearGain(); }

void ProgramPanel::setBgmMeter(double leftPeakDb, double rightPeakDb, bool clipped) {
  bgmMeter_->setPeakLevelsDbfs(leftPeakDb, rightPeakDb, clipped);
}

double ProgramPanel::masterGain() const { return masterFader_->linearGain(); }

bool ProgramPanel::masterFaderAtSilence() const {
  return masterFader_->value() == masterFader_->minimum();
}

void ProgramPanel::setPlaybackState(bool hasOnAirMedia, bool playing) {
  itemEqButton_->setEnabled(hasOnAirMedia);
  itemDelaySpin_->setEnabled(hasOnAirMedia);
  if (itemEqEditor_) itemEqEditor_->setEnabled(hasOnAirMedia);
  if (hasOnAirMedia_ != hasOnAirMedia ||
      playButton_->property("active").toBool() != (hasOnAirMedia && playing))
    qInfo() << "STATE PROGRAM playback hasMedia=" << hasOnAirMedia
            << "playing=" << playing;
  hasOnAirMedia_ = hasOnAirMedia;
  offAirButton_->setEnabled(hasOnAirMedia_);
  pauseButton_->setEnabled(hasOnAirMedia_ && !documentNavigationVisible_);
  playButton_->setEnabled(hasOnAirMedia_ && !documentNavigationVisible_);
  setButtonActive(offAirButton_, false);
  setButtonActive(pauseButton_, hasOnAirMedia && !playing);
  setButtonActive(playButton_, hasOnAirMedia && playing);
}

void ProgramPanel::triggerShortcutAction(ControllerOperations::Action action,
                                         const char *inputSource) {
  using A = ControllerOperations::Action;
  switch (action) {
  case A::DocumentNext:
  case A::DocumentPrevious:
  case A::ToggleFtb:
  case A::ToggleDsk:
  case A::OffAir:
  case A::MasterFadeIn:
  case A::MasterFadeOut:
  case A::ItemFadeIn:
  case A::ItemFadeOut:
  case A::BgmFadeInConfigured:
  case A::BgmFadeOutConfigured:
    break;
  case A::BgmFadeIn: action = A::BgmFadeInConfigured; break;
  case A::BgmFadeOut: action = A::BgmFadeOutConfigured; break;
  default: return;
  }
  (void)operations_->executeFrom(inputSource, action);
}

void ProgramPanel::setFtbButtonActive(bool active) {
  if (ftbButton_->property("active").toBool() != active)
    qInfo() << "STATE FTB active=" << active;
  setButtonActive(ftbButton_, active);
}

void ProgramPanel::setDskSelectionAvailable(bool available) {
  dskSelectionAvailable_ = available;
  updateDskControls();
}

void ProgramPanel::setDskCueState(DskCueState state, const QString &fileName,
                                  const QString &detail) {
  dskCueState_ = state;
  dskFileName_ = fileName;
  dskDetail_ = detail;
  updateDskControls();
}

void ProgramPanel::setDskOn(bool on) {
  dskOn_ = on;
  if (!on) {
    dskPlaying_ = false;
  }
  updateDskControls();
}

void ProgramPanel::setDskPlaybackState(bool playing, bool enabled) {
  dskPlaying_ = playing;
  dskTransportEnabled_ = enabled;
  updateDskControls();
}

void ProgramPanel::setDskPendingOn(bool pending) {
  dskPendingOn_ = pending;
  updateDskControls();
}

void ProgramPanel::updateDskControls() {
  const bool loading = dskCueState_ == DskCueState::Loading;
  dskOnButton_->setEnabled(dskOn_ || loading || dskSelectionAvailable_);
  dskOnButton_->setChecked(dskOn_);
  dskOnButton_->setText(QStringLiteral("DSK"));
  dskPlayButton_->setVisible(dskOn_);
  dskPauseButton_->setVisible(dskOn_);
  dskPlayButton_->setEnabled(dskOn_ && dskTransportEnabled_);
  dskPauseButton_->setEnabled(dskOn_ && dskTransportEnabled_);
  setButtonActive(dskPlayButton_, dskOn_ && dskPlaying_);
  setButtonActive(dskPauseButton_, dskOn_ && !dskPlaying_);
  if (dskOn_) {
    dskOnButton_->setToolTip(vtrpon::uiText("program.dsk.off_tooltip"));
    dskOnButton_->setAccessibleName(
        vtrpon::uiText("program.dsk.on_accessible"));
    return;
  }
  if (loading) {
    if (dskPendingOn_) {
      dskOnButton_->setToolTip(
          vtrpon::uiText("program.dsk.queued_tooltip")
              .arg(dskFileName_));
      dskOnButton_->setAccessibleName(
          vtrpon::uiText("program.dsk.queued_accessible"));
    } else {
      dskOnButton_->setToolTip(
          vtrpon::uiText("program.dsk.preparing_tooltip")
              .arg(dskFileName_));
      dskOnButton_->setAccessibleName(
          vtrpon::uiText("program.dsk.preparing_accessible"));
    }
    return;
  }
  if (dskCueState_ == DskCueState::Error && !dskDetail_.isEmpty()) {
    dskOnButton_->setToolTip(
        vtrpon::uiText("program.dsk.error_tooltip").arg(dskDetail_));
    dskOnButton_->setAccessibleName(
        dskSelectionAvailable_
            ? vtrpon::uiText("program.dsk.retry_accessible")
                  .arg(dskDetail_)
            : vtrpon::uiText("program.dsk.select_accessible")
                  .arg(dskDetail_));
    return;
  }
  dskOnButton_->setToolTip(
      dskSelectionAvailable_
          ? vtrpon::uiText("program.dsk.send_tooltip")
          : vtrpon::uiText("program.dsk.select_tooltip"));
  dskOnButton_->setAccessibleName(
      dskSelectionAvailable_
          ? vtrpon::uiText("program.dsk.inactive_ready_accessible")
          : vtrpon::uiText("program.dsk.inactive_accessible"));
}

void ProgramPanel::setMuteAppearance(bool explicitlyMuted,
                                     bool effectivelyMuted) {
  muteButton_->setChecked(explicitlyMuted);
  muteButton_->setProperty("effectivelyMuted", effectivelyMuted);
  muteButton_->setProperty("explicitlyMuted", explicitlyMuted);
  repolish(muteButton_);
}

void ProgramPanel::setInputSignalAppearance(bool active, bool clipped) {
  inputSignalActive_ = active;
  inputSignalClipped_ = clipped;
  inputSignalIndicator_->setProperty("signalActive", active);
  inputSignalIndicator_->setProperty("signalClipped", clipped);
  repolish(inputSignalIndicator_);
  inputSignalIndicator_->setAccessibleDescription(
      clipped  ? vtrpon::uiText("audio.input.clipping")
      : active ? vtrpon::uiText("audio.input.detected")
               : vtrpon::uiText("audio.input.none"));
}

bool ProgramPanel::setItemMeter(double leftPeakDb, double rightPeakDb,
                                bool clipped) {
  return itemMeter_->setPeakLevelsDbfs(leftPeakDb, rightPeakDb, clipped);
}

bool ProgramPanel::setMasterMeter(double leftPeakDb, double rightPeakDb,
                                  bool clipped) {
  return masterMeter_->setPeakLevelsDbfs(leftPeakDb, rightPeakDb, clipped);
}

void ProgramPanel::resetAudioPresentation() {
  bgmMeter_->resetToSilence();
  itemMeter_->resetToSilence();
  masterMeter_->resetToSilence();
  setInputSignalAppearance(false, false);
}
