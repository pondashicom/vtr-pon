#include "monitor_neon_outline.h"
#include "edit_panel.h"
#include "controller_button_focus.h"
#include "equalizer_editor.h"
#include "vst_editor.h"
#include "audio_delay_control.h"
#include <QGridLayout>
#include <QStandardItemModel>
#include <QDebug>

#include "audio_fader_widget.h"
#include "audio_meter_widget.h"
#include "controller_scaling.h"
#include "hardware_panel_widget.h"
#include "range_marker_slider.h"
#include "language_text.h"
#include "skins/controller_skin.h"
#include "user_info.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <utility>

class EditLivePreviewWidget final : public QWidget {
public:
  explicit EditLivePreviewWidget(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("editLivePreview"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

  void setFrame(const QImage &frame) {
    frame_ = frame;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (frame_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QSize scaled = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    painter.drawImage(QRect(QPoint((width() - scaled.width()) / 2,
                                   (height() - scaled.height()) / 2), scaled), frame_);
  }

private:
  QImage frame_;
};

namespace {

constexpr int kEditSurfaceWidth = 534;
constexpr int kEditSurfaceHeight = 300;
constexpr int kPanelMargin = 12;
constexpr int kPanelSpacing = 6;
constexpr int kSeekGap = 10;
constexpr int kControlRowGap = 14;
constexpr int kTransportButtonWidth = 42;
constexpr int kTransportButtonHeight = 20;
constexpr int kPointButtonWidth = 80;
constexpr int kPointTimeEditWidth = 100;
constexpr int kModeButtonWidth = 88;
constexpr int kEndModeButtonWidth = 70;
constexpr int kCompactButtonHeight = 20;
constexpr int kCompactEditorHeight = 24;
constexpr int kFadeDurationWidth = 41;
constexpr int kFadeDurationStepButtonWidth = 18;
constexpr int kFadeDurationStepButtonHeight = 12;
constexpr int kRepeatCountWidth = 36;
constexpr double kMeterFloorDb = -60.0;
constexpr auto kDisplayedPointTextProperty = "displayedPointText";

QString formatEditPointTime(qint64 timeMs) {
  const qint64 totalCentiseconds = (std::max)(timeMs, qint64{0}) / 10;
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

std::optional<qint64> parseEditPointTime(const QString &text) {
  static const QRegularExpression pattern(
      QStringLiteral("^(\\d{1,6}):([0-5]\\d):([0-5]\\d)\\.(\\d{2})$"));
  const QRegularExpressionMatch match = pattern.match(text.trimmed());
  if (!match.hasMatch()) {
    return std::nullopt;
  }

  bool hoursOk = false;
  const qint64 hours = match.captured(1).toLongLong(&hoursOk);
  if (!hoursOk) {
    return std::nullopt;
  }
  const qint64 minutes = match.captured(2).toLongLong();
  const qint64 seconds = match.captured(3).toLongLong();
  const qint64 centiseconds = match.captured(4).toLongLong();
  return (((hours * 60 + minutes) * 60 + seconds) * 1000)
      + centiseconds * 10;
}

QPushButton *addButton(QHBoxLayout *layout, const QString &text) {
  auto *button = new QPushButton(text);
  layout->addWidget(button);
  return button;
}

void setButtonActive(QPushButton *button, bool active) {
  if (button->property("active").toBool() == active) {
    return;
  }
  button->setProperty("active", active);
  button->style()->unpolish(button);
  button->style()->polish(button);
  button->update();
}

void flashButton(QPushButton *button) {
  constexpr int kFeedbackDurationMs = 140;
  const int generation = button->property("flashGeneration").toInt() + 1;
  button->setProperty("flashGeneration", generation);
  setButtonActive(button, true);
  QTimer::singleShot(kFeedbackDurationMs, button, [button, generation] {
    if (button->property("flashGeneration").toInt() == generation) {
      setButtonActive(button, false);
    }
  });
}

struct SpinBoxEditorWidgets {
  QWidget *control = nullptr;
  QToolButton *increaseButton = nullptr;
  QToolButton *decreaseButton = nullptr;
};

SpinBoxEditorWidgets createSpinBoxEditor(QAbstractSpinBox *spinBox,
                                         const QString &objectNamePrefix,
                                         QWidget *parent) {
  auto *control = new QWidget(parent);
  control->setObjectName(objectNamePrefix + QStringLiteral("Control"));
  auto *layout = new QHBoxLayout(control);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(vtrpon::scalePx(1));

  spinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
  layout->addWidget(spinBox);

  auto *buttonColumn = new QWidget(control);
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
    button->setFixedSize(vtrpon::scaleSize(kFadeDurationStepButtonWidth,
                                            kFadeDurationStepButtonHeight));
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
  };

  auto *increaseButton = createStepButton(
      Qt::UpArrow, objectNamePrefix + QStringLiteral("IncreaseButton"));
  auto *decreaseButton = createStepButton(
      Qt::DownArrow,
      objectNamePrefix + QStringLiteral("DecreaseButton"));
  buttonColumnLayout->addWidget(increaseButton);
  buttonColumnLayout->addWidget(decreaseButton);
  layout->addWidget(buttonColumn);

  QObject::connect(increaseButton, &QToolButton::clicked, spinBox,
                   &QAbstractSpinBox::stepUp);
  QObject::connect(decreaseButton, &QToolButton::clicked, spinBox,
                   &QAbstractSpinBox::stepDown);

  return {control, increaseButton, decreaseButton};
}

void retranslateFadeDurationButtons(QDoubleSpinBox *spinBox,
                                    QToolButton *increaseButton,
                                    QToolButton *decreaseButton) {
  const QString increaseText =
      vtrpon::uiText("edit.duration.increase.accessible")
          .arg(spinBox->accessibleName());
  const QString decreaseText =
      vtrpon::uiText("edit.duration.decrease.accessible")
          .arg(spinBox->accessibleName());
  increaseButton->setAccessibleName(increaseText);
  increaseButton->setToolTip(increaseText);
  decreaseButton->setAccessibleName(decreaseText);
  decreaseButton->setToolTip(decreaseText);
}

HardwarePanelWidget *createEditControlModule(
    const QString &title, const QString &objectName,
    const QString &accessibleName, QLayout *contentLayout, QWidget *parent) {
  auto *panel = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, parent);
  panel->setObjectName(objectName);
  panel->setAccessibleName(accessibleName);
  panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *panelLayout = new QVBoxLayout(panel);
  panelLayout->setContentsMargins(vtrpon::scalePx(10), vtrpon::scalePx(2),
                                  vtrpon::scalePx(10), vtrpon::scalePx(8));
  panelLayout->setSpacing(vtrpon::scalePx(2));
  auto *titleLabel = new QLabel(title, panel);
  titleLabel->setObjectName(QStringLiteral("editControlModuleTitle"));
  titleLabel->setAlignment(Qt::AlignCenter);
  titleLabel->setFixedHeight(vtrpon::scalePx(15));
  panelLayout->addWidget(titleLabel);
  panelLayout->addLayout(contentLayout);
  return panel;
}

QWidget *createEditMonitorWidget(QWidget *parent, QWidget *&videoSurface) {
  auto *monitor = new QWidget(parent);
  monitor->setObjectName(QStringLiteral("editMonitor"));
  monitor->setAccessibleName(vtrpon::uiText("edit.monitor_accessible"));
  monitor->setAttribute(Qt::WA_NativeWindow, true);
  monitor->setFixedSize(vtrpon::scaleSize(kEditSurfaceWidth,
                                          kEditSurfaceHeight));
  monitor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  auto *layout = new QStackedLayout(monitor);
  layout->setContentsMargins(1, 1, 1, 1);
  videoSurface = new QWidget(monitor);
  videoSurface->setObjectName(QStringLiteral("edit-video-surface"));
  videoSurface->setFocusPolicy(Qt::NoFocus);
  videoSurface->setAttribute(Qt::WA_NativeWindow, true);
  // This QWidget has no paintEvent of its own. Let Qt clear its background;
  // claiming an opaque paint event leaves old Controller pixels after resizing
  // while no GStreamer frame is being presented.
  videoSurface->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  videoSurface->setAutoFillBackground(true);
  QPalette palette = videoSurface->palette();
  palette.setColor(QPalette::Window, Qt::black);
  videoSurface->setPalette(palette);
  layout->addWidget(videoSurface);
  videoSurface->winId();
  monitor->winId();
  return monitor;
}

} // namespace

EditPanel::EditPanel(UserInfoCenter *userInfoCenter, QWidget *parent,
                     ControllerOperations *operations)
    : QWidget(parent), meter_(new AudioMeterWidget(this)),
      nominalVolumeFader_(
          new AudioFaderWidget(AudioFaderWidget::Role::NominalItem, this)),
      userInfoCenter_(userInfoCenter) {
  setObjectName(QStringLiteral("editControlPanel"));
  setAttribute(Qt::WA_StyledBackground, true);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

  videoMonitor_ = createEditMonitorWidget(this, videoSurface_);
  livePreviewWidget_ = new EditLivePreviewWidget(videoMonitor_);
  qobject_cast<QStackedLayout *>(videoMonitor_->layout())->addWidget(livePreviewWidget_);
  videoMonitor_->installEventFilter(this);
  meter_->setObjectName(QStringLiteral("editAudioMeter"));
  meter_->setAccessibleName(vtrpon::uiText("edit.meter_accessible"));
  meter_->setCompactStripMode(true);
  nominalVolumeFader_->setObjectName(
      QStringLiteral("editNominalVolumeFader"));
  nominalVolumeFader_->setAccessibleName(
      vtrpon::uiText("audio_fader.nominal_name"));
  nominalVolumeFader_->setEnabled(false);
  setFixedWidth(vtrpon::scalePx(kEditSurfaceWidth + 2 * kPanelMargin));

  seekBar_ = new RangeMarkerSlider(this);
  seekBar_->setObjectName(QStringLiteral("editSeekBar"));
  seekBar_->setAccessibleName(vtrpon::uiText("edit.seek_accessible"));
  seekBar_->setHardwareSkinMode(
      true, RangeMarkerSlider::HardwareAccent::Edit);
  seekBar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  seekBar_->setEnabled(false);

  fileLabel_ = new QLabel(vtrpon::uiText("program.file.none"), this);
  fileLabel_->setObjectName(QStringLiteral("editFileLabel"));
  fileLabel_->setAccessibleName(vtrpon::uiText("edit.file.none_accessible"));
  fileLabel_->setAlignment(Qt::AlignCenter);
  fileLabel_->setMinimumHeight(vtrpon::scalePx(22));
  fileLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *transportLayout = new QHBoxLayout;
  transportLayout->setContentsMargins(0, 0, 0, 0);
  transportLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  transportLayout->setAlignment(Qt::AlignHCenter);
  jumpToStartButton_ = addButton(transportLayout, QStringLiteral("|<"));
  jumpToInButton_ = addButton(transportLayout, QStringLiteral("IN<"));
  rewindButton_ = addButton(transportLayout, QStringLiteral("<<"));
  pauseButton_ = addButton(transportLayout, QStringLiteral("||"));
  playButton_ = addButton(transportLayout, QStringLiteral(">"));
  forwardButton_ = addButton(transportLayout, QStringLiteral(">>"));
  jumpToOutButton_ = addButton(transportLayout, QStringLiteral(">OUT"));
  jumpToEndButton_ = addButton(transportLayout, QStringLiteral(">|"));
  jumpToStartButton_->setObjectName(QStringLiteral("editJumpToStartButton"));
  jumpToInButton_->setObjectName(QStringLiteral("editJumpToInButton"));
  rewindButton_->setObjectName(QStringLiteral("editRewindButton"));
  playButton_->setObjectName(QStringLiteral("editPlayButton"));
  pauseButton_->setObjectName(QStringLiteral("editPauseButton"));
  forwardButton_->setObjectName(QStringLiteral("editForwardButton"));
  jumpToOutButton_->setObjectName(QStringLiteral("editJumpToOutButton"));
  jumpToEndButton_->setObjectName(QStringLiteral("editJumpToEndButton"));
  jumpToStartButton_->setAccessibleName(vtrpon::uiText("edit.jump_start.accessible"));
  jumpToInButton_->setAccessibleName(vtrpon::uiText("edit.jump_in.accessible"));
  rewindButton_->setAccessibleName(vtrpon::uiText("edit.rewind.accessible"));
  playButton_->setAccessibleName(vtrpon::uiText("edit.play.accessible"));
  pauseButton_->setAccessibleName(vtrpon::uiText("edit.pause.accessible"));
  forwardButton_->setAccessibleName(vtrpon::uiText("edit.forward.accessible"));
  jumpToOutButton_->setAccessibleName(vtrpon::uiText("edit.jump_out.accessible"));
  jumpToEndButton_->setAccessibleName(vtrpon::uiText("edit.jump_end.accessible"));
  jumpToStartButton_->setToolTip(vtrpon::uiText("edit.jump_start.tooltip"));
  jumpToInButton_->setToolTip(vtrpon::uiText("edit.jump_in.tooltip"));
  rewindButton_->setToolTip(vtrpon::uiText("edit.rewind.tooltip"));
  pauseButton_->setToolTip(vtrpon::uiText("common.pause"));
  playButton_->setToolTip(vtrpon::uiText("common.play"));
  forwardButton_->setToolTip(vtrpon::uiText("edit.forward.tooltip"));
  jumpToOutButton_->setToolTip(vtrpon::uiText("edit.jump_out.tooltip"));
  jumpToEndButton_->setToolTip(vtrpon::uiText("edit.jump_end.tooltip"));
  for (QPushButton *button :
       {jumpToStartButton_, jumpToInButton_, rewindButton_, pauseButton_,
        playButton_, forwardButton_, jumpToOutButton_, jumpToEndButton_}) {
    button->setFixedSize(vtrpon::scaleSize(kTransportButtonWidth,
                                           kTransportButtonHeight));
    button->setEnabled(false);
  }

  auto *pointLayout = new QHBoxLayout;
  pointLayout->setContentsMargins(0, 0, 0, 0);
  pointLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  inButton_ = addButton(pointLayout, vtrpon::uiText("edit.set_in"));
  inPointTimeEdit_ = new QLineEdit(this);
  outPointTimeEdit_ = new QLineEdit(this);
  pointLayout->addWidget(inPointTimeEdit_);
  pointLayout->addWidget(outPointTimeEdit_);
  outButton_ = addButton(pointLayout, vtrpon::uiText("edit.set_out"));
  inButton_->setObjectName(QStringLiteral("editSetInButton"));
  outButton_->setObjectName(QStringLiteral("editSetOutButton"));
  inPointTimeEdit_->setObjectName(QStringLiteral("editInPointTimeEdit"));
  outPointTimeEdit_->setObjectName(QStringLiteral("editOutPointTimeEdit"));
  inButton_->setAccessibleName(
      vtrpon::uiText("edit.set_in.accessible"));
  outButton_->setAccessibleName(
      vtrpon::uiText("edit.set_out.accessible"));
  inButton_->setToolTip(
      vtrpon::uiText("edit.set_in.tooltip"));
  outButton_->setToolTip(
      vtrpon::uiText("edit.set_out.tooltip"));
  inPointTimeEdit_->setAccessibleName(
      vtrpon::uiText("edit.in_time.accessible"));
  outPointTimeEdit_->setAccessibleName(
      vtrpon::uiText("edit.out_time.accessible"));
  inPointTimeEdit_->setToolTip(
      vtrpon::uiText("edit.in_time.tooltip"));
  outPointTimeEdit_->setToolTip(
      vtrpon::uiText("edit.out_time.tooltip"));
  for (QPushButton *button : {inButton_, outButton_}) {
    button->setFixedSize(vtrpon::scaleSize(kPointButtonWidth,
                                           kCompactButtonHeight));
    button->setEnabled(false);
  }
  for (QLineEdit *editor : {inPointTimeEdit_, outPointTimeEdit_}) {
    editor->setAlignment(Qt::AlignCenter);
    editor->setMaxLength(15);
    editor->setFixedSize(vtrpon::scaleSize(kPointTimeEditWidth,
                                           kCompactEditorHeight));
    editor->setEnabled(false);
  }
  pointLayout->setAlignment(Qt::AlignHCenter);

  startPauseButton_ = new QPushButton(vtrpon::uiText("common.pause").toUpper(), this);
  startPlayButton_ = new QPushButton(vtrpon::uiText("common.play").toUpper(), this);
  endStopButton_ = new QPushButton(vtrpon::uiText("common.stop").toUpper(), this);
  endPauseButton_ = new QPushButton(vtrpon::uiText("common.pause").toUpper(), this);
  endRepeatButton_ = new QPushButton(vtrpon::uiText("common.repeat").toUpper(), this);
  endNextButton_ = new QPushButton(vtrpon::uiText("common.next").toUpper(), this);
  endGotoButton_ = new QPushButton(vtrpon::uiText("common.goto").toUpper(), this);
  startPauseButton_->setObjectName(QStringLiteral("editStartModePauseButton"));
  startPlayButton_->setObjectName(QStringLiteral("editStartModePlayButton"));
  endStopButton_->setObjectName(QStringLiteral("editEndModeStopButton"));
  endPauseButton_->setObjectName(QStringLiteral("editEndModePauseButton"));
  endRepeatButton_->setObjectName(QStringLiteral("editEndModeRepeatButton"));
  endNextButton_->setObjectName(QStringLiteral("editEndModeNextButton"));
  endGotoButton_->setObjectName(QStringLiteral("editEndModeGotoButton"));
  startPauseButton_->setAccessibleName(
      vtrpon::uiText("edit.start_pause.accessible"));
  startPlayButton_->setAccessibleName(
      vtrpon::uiText("edit.start_play.accessible"));
  endStopButton_->setAccessibleName(
      vtrpon::uiText("edit.end_stop.accessible"));
  endPauseButton_->setAccessibleName(
      vtrpon::uiText("edit.end_pause.accessible"));
  endRepeatButton_->setAccessibleName(
      vtrpon::uiText("edit.end_repeat.accessible"));
  endNextButton_->setAccessibleName(
      vtrpon::uiText("edit.end_next.accessible"));
  endGotoButton_->setAccessibleName(
      vtrpon::uiText("edit.end_goto.accessible"));
  startPauseButton_->setToolTip(
      vtrpon::uiText("edit.start_pause.tooltip"));
  startPlayButton_->setToolTip(vtrpon::uiText("edit.start_play.tooltip"));
  endStopButton_->setToolTip(
      vtrpon::uiText("edit.end_stop.tooltip"));
  endPauseButton_->setToolTip(vtrpon::uiText("edit.end_pause.tooltip"));
  endRepeatButton_->setToolTip(
      vtrpon::uiText("edit.end_repeat.tooltip"));
  endNextButton_->setToolTip(vtrpon::uiText("edit.end_next.tooltip"));
  endGotoButton_->setToolTip(vtrpon::uiText("edit.end_goto.tooltip"));
  for (QPushButton *button :
       {startPauseButton_, startPlayButton_, endStopButton_, endPauseButton_,
        endRepeatButton_, endNextButton_, endGotoButton_}) {
    button->setProperty("modeButton", true);
    button->setCheckable(true);
    button->setFixedSize(vtrpon::scaleSize(kModeButtonWidth,
                                           kCompactButtonHeight));
    button->setEnabled(false);
  }
  for (QPushButton *button : {endStopButton_, endPauseButton_,
                              endRepeatButton_, endNextButton_,
                              endGotoButton_}) {
    button->setFixedWidth(vtrpon::scalePx(kEndModeButtonWidth));
  }
  startPauseButton_->setChecked(true);
  endPauseButton_->setChecked(true);
  auto *startGroup = new QButtonGroup(this);
  startGroup->setExclusive(true);
  startGroup->addButton(startPauseButton_);
  startGroup->addButton(startPlayButton_);
  auto *endGroup = new QButtonGroup(this);
  endGroup->setExclusive(true);
  endGroup->addButton(endStopButton_);
  endGroup->addButton(endPauseButton_);
  endGroup->addButton(endRepeatButton_);
  endGroup->addButton(endNextButton_);
  endGroup->addButton(endGotoButton_);

  fadeInCheckBox_ = new QCheckBox(vtrpon::uiText("edit.fade_in"), this);
  fadeOutCheckBox_ = new QCheckBox(vtrpon::uiText("edit.fade_out"), this);
  endCrossfadeCheckBox_ =
      new QCheckBox(vtrpon::uiText("edit.end_crossfade"), this);
  fadeInDurationSpinBox_ = new QDoubleSpinBox(this);
  fadeOutDurationSpinBox_ = new QDoubleSpinBox(this);
  endCrossfadeDurationSpinBox_ = new QDoubleSpinBox(this);
  fadeInCheckBox_->setObjectName(QStringLiteral("editFadeInCheckBox"));
  fadeOutCheckBox_->setObjectName(QStringLiteral("editFadeOutCheckBox"));
  endCrossfadeCheckBox_->setObjectName(
      QStringLiteral("editEndCrossfadeCheckBox"));
  fadeInDurationSpinBox_->setObjectName(
      QStringLiteral("editFadeInDurationSpinBox"));
  fadeOutDurationSpinBox_->setObjectName(
      QStringLiteral("editFadeOutDurationSpinBox"));
  endCrossfadeDurationSpinBox_->setObjectName(
      QStringLiteral("editEndCrossfadeDurationSpinBox"));
  fadeInCheckBox_->setAccessibleName(
      vtrpon::uiText("edit.fade_in.accessible"));
  fadeOutCheckBox_->setAccessibleName(
      vtrpon::uiText("edit.fade_out.accessible"));
  endCrossfadeCheckBox_->setAccessibleName(
      vtrpon::uiText("edit.end_crossfade.accessible"));
  fadeInDurationSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.fade_in_duration.accessible"));
  fadeOutDurationSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.fade_out_duration.accessible"));
  endCrossfadeDurationSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.end_crossfade_duration.accessible"));
  fadeInCheckBox_->setToolTip(
      vtrpon::uiText("edit.fade_in.tooltip"));
  fadeOutCheckBox_->setToolTip(
      vtrpon::uiText("edit.fade_out.tooltip"));
  endCrossfadeCheckBox_->setToolTip(
      vtrpon::uiText("edit.end_crossfade.tooltip"));
  for (QDoubleSpinBox *spinBox :
       {fadeInDurationSpinBox_, fadeOutDurationSpinBox_,
        endCrossfadeDurationSpinBox_}) {
    spinBox->setDecimals(1);
    spinBox->setRange(kMinimumItemFadeDurationMs / 1000.0,
                      kMaximumItemFadeDurationMs / 1000.0);
    spinBox->setSingleStep(0.1);
    spinBox->setValue(kDefaultItemFadeDurationMs / 1000.0);
    spinBox->setSuffix(QStringLiteral(" s"));
    spinBox->setFixedSize(vtrpon::scaleSize(kFadeDurationWidth,
                                            kCompactEditorHeight));
    spinBox->setKeyboardTracking(false);
  }
  const SpinBoxEditorWidgets fadeInDurationEditor =
      createSpinBoxEditor(fadeInDurationSpinBox_,
                          QStringLiteral("editFadeInDuration"), this);
  fadeInDurationControl_ = fadeInDurationEditor.control;
  fadeInDurationIncreaseButton_ = fadeInDurationEditor.increaseButton;
  fadeInDurationDecreaseButton_ = fadeInDurationEditor.decreaseButton;
  const SpinBoxEditorWidgets fadeOutDurationEditor =
      createSpinBoxEditor(fadeOutDurationSpinBox_,
                          QStringLiteral("editFadeOutDuration"), this);
  fadeOutDurationControl_ = fadeOutDurationEditor.control;
  fadeOutDurationIncreaseButton_ = fadeOutDurationEditor.increaseButton;
  fadeOutDurationDecreaseButton_ = fadeOutDurationEditor.decreaseButton;
  const SpinBoxEditorWidgets endCrossfadeDurationEditor =
      createSpinBoxEditor(endCrossfadeDurationSpinBox_,
                          QStringLiteral("editEndCrossfadeDuration"), this);
  endCrossfadeDurationControl_ = endCrossfadeDurationEditor.control;
  endCrossfadeDurationIncreaseButton_ =
      endCrossfadeDurationEditor.increaseButton;
  endCrossfadeDurationDecreaseButton_ =
      endCrossfadeDurationEditor.decreaseButton;
  fadeInDurationControl_->setEnabled(false);
  fadeOutDurationControl_->setEnabled(false);
  endCrossfadeDurationControl_->setEnabled(false);
  fadeInCheckBox_->setEnabled(false);
  fadeOutCheckBox_->setEnabled(false);
  endCrossfadeCheckBox_->setEnabled(false);
  gotoTargetLabel_ = new QLabel(vtrpon::uiText("edit.goto_target"), this);
  gotoTargetLabel_->setObjectName(QStringLiteral("editGotoTargetLabel"));
  gotoTargetLabel_->setEnabled(false);
  gotoTargetComboBox_ = new QComboBox(this);
  gotoTargetComboBox_->setObjectName(
      QStringLiteral("editGotoTargetComboBox"));
  gotoTargetComboBox_->setAccessibleName(
      vtrpon::uiText("edit.goto_target.accessible"));
  gotoTargetComboBox_->setToolTip(
      vtrpon::uiText("edit.goto_target.tooltip"));
  gotoTargetComboBox_->setFixedHeight(vtrpon::scalePx(kCompactEditorHeight));
  gotoTargetComboBox_->setMinimumContentsLength(24);
  gotoTargetComboBox_->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  gotoTargetComboBox_->setEnabled(false);
  repeatOptionLabel_ =
      new QLabel(vtrpon::uiText("edit.repeat_option"), this);
  repeatOptionLabel_->setObjectName(QStringLiteral("editRepeatOptionLabel"));
  repeatCountLabel_ = new QLabel(vtrpon::uiText("edit.repeat_count"), this);
  repeatCountLabel_->setObjectName(QStringLiteral("editRepeatCountLabel"));
  repeatCountSpinBox_ = new QSpinBox(this);
  repeatCountSpinBox_->setObjectName(
      QStringLiteral("editRepeatCountSpinBox"));
  repeatCountSpinBox_->setRange(kInfiniteItemRepeatCount,
                                kMaximumItemRepeatCount);
  repeatCountSpinBox_->setSpecialValueText(QString(QChar(0x221e)));
  repeatCountSpinBox_->setAlignment(Qt::AlignCenter);
  repeatCountSpinBox_->setFixedSize(
      vtrpon::scaleSize(kRepeatCountWidth, kCompactEditorHeight));
  repeatCountSpinBox_->setKeyboardTracking(false);
  const SpinBoxEditorWidgets repeatCountEditor = createSpinBoxEditor(
      repeatCountSpinBox_, QStringLiteral("editRepeatCount"), this);
  repeatCountControl_ = repeatCountEditor.control;
  repeatCountIncreaseButton_ = repeatCountEditor.increaseButton;
  repeatCountDecreaseButton_ = repeatCountEditor.decreaseButton;
  repeatEndModeLabel_ =
      new QLabel(vtrpon::uiText("edit.repeat_end_mode"), this);
  repeatEndModeLabel_->setObjectName(
      QStringLiteral("editRepeatEndModeLabel"));
  repeatEndModeComboBox_ = new QComboBox(this);
  repeatEndModeComboBox_->setObjectName(
      QStringLiteral("editRepeatEndModeComboBox"));
  repeatEndModeComboBox_->setFixedHeight(
      vtrpon::scalePx(kCompactEditorHeight));
  repeatEndModeComboBox_->setMinimumContentsLength(7);
  repeatEndModeComboBox_->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  for (QWidget *widget : std::array<QWidget *, 5>{
           repeatOptionLabel_, repeatCountLabel_, repeatCountControl_,
           repeatEndModeLabel_, repeatEndModeComboBox_}) {
    widget->setEnabled(false);
  }

  auto *startButtonsLayout = new QHBoxLayout;
  startButtonsLayout->setContentsMargins(0, 0, 0, 0);
  startButtonsLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  startButtonsLayout->addStretch(1);
  startButtonsLayout->addWidget(startPauseButton_);
  startButtonsLayout->addWidget(startPlayButton_);
  startButtonsLayout->addStretch(1);
  auto *fadeInLayout = new QHBoxLayout;
  fadeInLayout->setContentsMargins(0, 0, 0, 0);
  fadeInLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  fadeInLayout->addStretch(1);
  fadeInLayout->addWidget(fadeInCheckBox_);
  fadeInLayout->addWidget(fadeInDurationControl_);
  fadeInLayout->addStretch(1);
  auto *startModeLayout = new QVBoxLayout;
  startModeLayout->setContentsMargins(0, 0, 0, 0);
  startModeLayout->setSpacing(vtrpon::scalePx(4));
  startModeLayout->addLayout(startButtonsLayout);
  startModeLayout->addLayout(fadeInLayout);

  auto *endButtonsLayout = new QHBoxLayout;
  endButtonsLayout->setContentsMargins(0, 0, 0, 0);
  endButtonsLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  endButtonsLayout->addStretch(1);
  endButtonsLayout->addWidget(endStopButton_);
  endButtonsLayout->addWidget(endPauseButton_);
  endButtonsLayout->addWidget(endRepeatButton_);
  endButtonsLayout->addWidget(endNextButton_);
  endButtonsLayout->addWidget(endGotoButton_);
  endButtonsLayout->addStretch(1);
  auto *fadeOutLayout = new QHBoxLayout;
  fadeOutLayout->setContentsMargins(0, 0, 0, 0);
  fadeOutLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  fadeOutLayout->addStretch(1);
  fadeOutLayout->addWidget(fadeOutCheckBox_);
  fadeOutLayout->addWidget(fadeOutDurationControl_);
  fadeOutLayout->addStretch(1);
  auto *endCrossfadeLayout = new QHBoxLayout;
  endCrossfadeLayout->setContentsMargins(0, 0, 0, 0);
  endCrossfadeLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  endCrossfadeLayout->addStretch(1);
  endCrossfadeLayout->addWidget(endCrossfadeCheckBox_);
  endCrossfadeLayout->addWidget(endCrossfadeDurationControl_);
  endCrossfadeLayout->addStretch(1);
  auto *gotoTargetLayout = new QHBoxLayout;
  gotoTargetLayout->setContentsMargins(0, 0, 0, 0);
  gotoTargetLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  gotoTargetLayout->addWidget(gotoTargetLabel_);
  gotoTargetLayout->addWidget(gotoTargetComboBox_, 1);
  auto *repeatOptionsLayout = new QHBoxLayout;
  repeatOptionsLayout->setContentsMargins(0, 0, 0, 0);
  repeatOptionsLayout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  repeatOptionsLayout->addWidget(repeatOptionLabel_);
  repeatOptionsLayout->addWidget(repeatCountLabel_);
  repeatOptionsLayout->addWidget(repeatCountControl_);
  repeatOptionsLayout->addWidget(repeatEndModeLabel_);
  repeatOptionsLayout->addWidget(repeatEndModeComboBox_, 1);
  auto *targetOptionsLayout = new QGridLayout;
  targetOptionsLayout->setContentsMargins(0, 0, 0, 0);
  targetOptionsLayout->setHorizontalSpacing(0);
  targetOptionsLayout->setVerticalSpacing(vtrpon::scalePx(4));
  targetOptionsLayout->setColumnStretch(0, 1);
  targetOptionsLayout->setColumnStretch(1, 1);
  targetOptionsLayout->setColumnStretch(2, 1);
  targetOptionsLayout->addLayout(gotoTargetLayout, 0, 1);
  targetOptionsLayout->addLayout(repeatOptionsLayout, 1, 1);
  auto *endModeLayout = new QVBoxLayout;
  endModeLayout->setContentsMargins(0, 0, 0, 0);
  endModeLayout->setSpacing(vtrpon::scalePx(4));
  endModeLayout->addLayout(endButtonsLayout);
  endModeLayout->addLayout(fadeOutLayout);
  endModeLayout->addLayout(endCrossfadeLayout);
  endModeLayout->addLayout(targetOptionsLayout);

  auto *transportPanel = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  transportPanel->setObjectName(QStringLiteral("editTransportModule"));
  transportPanel->setAccessibleName(
      vtrpon::uiText("edit.transport.accessible"));
  transportPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *transportPanelLayout = new QVBoxLayout(transportPanel);
  transportPanelLayout->setContentsMargins(vtrpon::scalePx(10),
                                            vtrpon::scalePx(8),
                                            vtrpon::scalePx(10),
                                            vtrpon::scalePx(8));
  transportPanelLayout->addLayout(transportLayout);
  auto *pointPanel = createEditControlModule(
      vtrpon::uiText("edit.points.title"), QStringLiteral("editPointsModule"),
      vtrpon::uiText("edit.points.accessible"), pointLayout, this);
  auto *startModePanel = createEditControlModule(
      vtrpon::uiText("edit.start_mode.title"), QStringLiteral("editStartModeModule"),
      vtrpon::uiText("edit.start_mode.accessible"),
      startModeLayout, this);
  auto *endModePanel = createEditControlModule(
      vtrpon::uiText("edit.end_mode.title"), QStringLiteral("editEndModeModule"),
      vtrpon::uiText("edit.end_mode.accessible"), endModeLayout,
      this);

  auto *sectionHeader = new QHBoxLayout;
  sectionHeader->setContentsMargins(vtrpon::scalePx(2), 0,
                                    vtrpon::scalePx(2), 0);
  sectionHeader->setSpacing(vtrpon::scalePx(kPanelSpacing));
  auto *sectionTitle = new QLabel(vtrpon::uiText("edit.section"), this);
  sectionTitle->setObjectName(QStringLiteral("editSectionTitle"));
  sectionHeader->addWidget(sectionTitle);
  sectionHeader->addStretch(1);

  auto *leftColumn = new QWidget(this);
  leftColumn->setFixedWidth(vtrpon::scalePx(kEditSurfaceWidth));
  leftColumn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto *leftLayout = new QVBoxLayout(leftColumn);
  leftLayout->setContentsMargins(0, 0, 0, 0);
  leftLayout->setSpacing(0);
  leftLayout->addWidget(videoMonitor_);
  leftLayout->addSpacing(vtrpon::scalePx(kSeekGap));
  leftLayout->addWidget(seekBar_);
  leftLayout->addWidget(fileLabel_);

  auto *audioStrip = new HardwarePanelWidget(
      HardwarePanelWidget::Layout::SinglePlate, this);
  audioStrip->setObjectName(QStringLiteral("editNominalVolumeStrip"));
  audioStrip->setAccessibleName(
      vtrpon::uiText("edit.nominal_strip.accessible"));
  audioStrip->setAttribute(Qt::WA_StyledBackground, true);
  audioStrip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  auto *audioStripStack = new QStackedLayout(audioStrip);
  audioStripStack->setContentsMargins(0, 0, 0, 0);
  audioStripStack->setStackingMode(QStackedLayout::StackAll);

  auto *audioStripContent = new QWidget(audioStrip);
  auto *audioStripLayout = new QGridLayout(audioStripContent);
  audioStripLayout->setContentsMargins(vtrpon::scalePx(5),
                                       vtrpon::scalePx(2),
                                       vtrpon::scalePx(5),
                                       vtrpon::scalePx(2));
  audioStripLayout->setSpacing(vtrpon::scalePx(1));
  auto *faderColumn = new QWidget(audioStripContent);
  auto *faderColumnLayout = new QVBoxLayout(faderColumn);
  faderColumnLayout->setContentsMargins(0, 0, 0, vtrpon::scalePx(8));
  faderColumnLayout->setSpacing(vtrpon::scalePx(2));
  faderColumnLayout->addWidget(nominalVolumeFader_, 1);
  pflButton_ = new QPushButton(QStringLiteral("PFL"), faderColumn);
  pflButton_->setObjectName(QStringLiteral("editPflButton"));
  pflButton_->setCheckable(true);
  pflButton_->setProperty("pflActive", false);
  pflButton_->setFixedSize(vtrpon::scaleSize(44, 22));
  // The painted fader readout is 44 px wide with its centre at x=22.
  faderColumnLayout->addWidget(pflButton_, 0, Qt::AlignLeft);
  nominalEqButton_ = new QPushButton(QStringLiteral("EQ"), faderColumn);
  nominalEqButton_->setObjectName(QStringLiteral("editNominalEqButton"));
  nominalEqButton_->setProperty("programFadeButton", true);
  nominalEqButton_->setFixedSize(vtrpon::scaleSize(34, 20));
  nominalEqButton_->setEnabled(false);
  auto *nominalDelayControl = vtrpon::createAudioDelayControl(audioStripContent,
      QStringLiteral("editNominalDelaySpin"), "audio.delay.nominal");
  nominalDelaySpin_ = nominalDelayControl->spinBox();
  nominalDelaySpin_->setEnabled(false);
  auto *nominalEqRow = new QHBoxLayout;
  nominalEqRow->setSpacing(vtrpon::scalePx(2));
  nominalEqRow->addWidget(nominalEqButton_);
  nominalEqRow->addWidget(nominalDelayControl);
  audioStripLayout->addLayout(nominalEqRow, 1, 0, 1, 2, Qt::AlignHCenter);
  connect(nominalDelaySpin_, &QSpinBox::valueChanged, this, &EditPanel::nominalDelayRequested);
  audioStripLayout->addWidget(faderColumn, 0, 0);
  vtrpon::bindVstButton(nominalEqButton_, vtrpon::VstSlot::Edit);
  connect(&vtrpon::VstService::instance(), &vtrpon::VstService::changed, this, [this] {
    if (vtrpon::vstModeEnabled() && nominalEqEditor_) nominalEqEditor_->hide();
    vtrpon::updateEqButton(nominalEqButton_, nominalEqTenthsDb_, "edit.eq.title");
  });

  auto *meterContainer = new QWidget(audioStrip);
  meterContainer->setFixedWidth(vtrpon::scalePx(25));
  meterContainer->setSizePolicy(QSizePolicy::Fixed,
                                QSizePolicy::Expanding);
  auto *meterStack = new QStackedLayout(meterContainer);
  meterStack->setContentsMargins(0, 0, 0, 0);
  meterStack->setStackingMode(QStackedLayout::StackAll);
  meterStack->addWidget(meter_);
  auto *inputSignalOverlay = new QWidget(meterContainer);
  inputSignalOverlay->setObjectName(QStringLiteral("transparentOverlay"));
  auto *inputSignalLayout = new QVBoxLayout(inputSignalOverlay);
  inputSignalLayout->setContentsMargins(0, 0, 0, vtrpon::scalePx(9));
  inputSignalLayout->setSpacing(0);
  inputSignalLayout->addStretch();
  inputSignalIndicator_ = new QLabel(QStringLiteral("SIG"),
                                     inputSignalOverlay);
  inputSignalIndicator_->setObjectName(QStringLiteral("editInputSignal"));
  inputSignalIndicator_->setAccessibleName(
      vtrpon::uiText("edit.input_signal.accessible"));
  inputSignalIndicator_->setFixedSize(vtrpon::scaleSize(25, 22));
  inputSignalIndicator_->setAlignment(Qt::AlignCenter);
  inputSignalLayout->addWidget(inputSignalIndicator_);
  meterStack->addWidget(inputSignalOverlay);
  meterStack->setCurrentWidget(inputSignalOverlay);
  audioStripLayout->addWidget(meterContainer, 0, 1);

  auto *labelOverlay = new QWidget(audioStrip);
  labelOverlay->setObjectName(QStringLiteral("transparentOverlay"));
  labelOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  labelOverlay->setFixedHeight(vtrpon::scalePx(20));
  labelOverlay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *labelOverlayLayout = new QHBoxLayout(labelOverlay);
  labelOverlayLayout->setContentsMargins(vtrpon::scalePx(5),
                                         vtrpon::scalePx(2),
                                         vtrpon::scalePx(5), 0);
  labelOverlayLayout->setSpacing(0);
  auto *nominalVolumeLabel = new QLabel(vtrpon::uiText("edit.nominal_volume"),
                                        labelOverlay);
  nominalVolumeLabel->setObjectName(
      QStringLiteral("editNominalVolumeLabel"));
  nominalVolumeLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
  nominalVolumeLabel->setFixedSize(vtrpon::scaleSize(87, 20));
  labelOverlayLayout->addWidget(nominalVolumeLabel);
  audioStripStack->addWidget(audioStripContent);
  audioStripStack->addWidget(labelOverlay);
  audioStripStack->setAlignment(labelOverlay, Qt::AlignTop);
  audioStripStack->setCurrentWidget(labelOverlay);

  auto *controlLayout = new QVBoxLayout;
  controlLayout->setContentsMargins(0, 0, 0, 0);
  controlLayout->setSpacing(vtrpon::scalePx(5));
  controlLayout->addWidget(transportPanel);
  controlLayout->addWidget(pointPanel);
  controlLayout->addWidget(startModePanel);
  controlLayout->addWidget(endModePanel);
  controlLayout->addStretch(1);
  audioStrip->setFixedHeight(controlLayout->sizeHint().height());

  auto *postSeekLayout = new QHBoxLayout;
  postSeekLayout->setContentsMargins(0, 0, 0, 0);
  postSeekLayout->setSpacing(vtrpon::scalePx(kControlRowGap));
  postSeekLayout->addLayout(controlLayout, 1);
  postSeekLayout->addWidget(audioStrip, 0, Qt::AlignRight | Qt::AlignTop);
  leftLayout->addSpacing(vtrpon::scalePx(kPanelSpacing));
  leftLayout->addLayout(postSeekLayout);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin),
                             vtrpon::scalePx(kPanelMargin));
  layout->setSpacing(vtrpon::scalePx(kPanelSpacing));
  layout->addLayout(sectionHeader);
  layout->addWidget(leftColumn);
  layout->addStretch(1);

  connect(jumpToStartButton_, &QPushButton::clicked, this,
          &EditPanel::jumpToStartRequested);
  connect(jumpToInButton_, &QPushButton::clicked, this,
          &EditPanel::jumpToInRequested);
  connect(rewindButton_, &QPushButton::pressed, this,
          &EditPanel::scanBackwardRequested);
  connect(rewindButton_, &QPushButton::released, this,
          &EditPanel::scanStopRequested);
  connect(rewindButton_, &QPushButton::pressed, this,
          [this] { setButtonActive(rewindButton_, true); });
  connect(rewindButton_, &QPushButton::released, this,
          [this] { setButtonActive(rewindButton_, false); });
  connect(pauseButton_, &QPushButton::clicked, this,
          &EditPanel::pauseRequested);
  connect(playButton_, &QPushButton::clicked, this, &EditPanel::playRequested);
  connect(forwardButton_, &QPushButton::pressed, this,
          &EditPanel::scanForwardRequested);
  connect(forwardButton_, &QPushButton::released, this,
          &EditPanel::scanStopRequested);
  connect(forwardButton_, &QPushButton::pressed, this,
          [this] { setButtonActive(forwardButton_, true); });
  connect(forwardButton_, &QPushButton::released, this,
          [this] { setButtonActive(forwardButton_, false); });
  connect(jumpToOutButton_, &QPushButton::clicked, this,
          &EditPanel::jumpToOutRequested);
  connect(jumpToEndButton_, &QPushButton::clicked, this,
          &EditPanel::jumpToEndRequested);
  for (QPushButton *button :
       {jumpToStartButton_, jumpToInButton_, jumpToOutButton_,
        jumpToEndButton_}) {
    connect(button, &QPushButton::clicked, this,
            [button] { flashButton(button); });
  }
  connect(inButton_, &QPushButton::clicked, this, &EditPanel::setInRequested);
  connect(outButton_, &QPushButton::clicked, this, &EditPanel::setOutRequested);
  connect(inPointTimeEdit_, &QLineEdit::editingFinished, this, [this] {
    commitEditPointTime(inPointTimeEdit_, true);
  });
  connect(outPointTimeEdit_, &QLineEdit::editingFinished, this, [this] {
    commitEditPointTime(outPointTimeEdit_, false);
  });
  connect(startPauseButton_, &QPushButton::clicked, this,
          [this] { emit startModeRequested(PlaylistStartMode::Pause); });
  connect(startPlayButton_, &QPushButton::clicked, this,
          [this] { emit startModeRequested(PlaylistStartMode::Play); });
  connect(endStopButton_, &QPushButton::clicked, this,
          [this] { emit endModeRequested(PlaylistEndMode::Stop); });
  connect(endPauseButton_, &QPushButton::clicked, this,
          [this] { emit endModeRequested(PlaylistEndMode::Pause); });
  connect(endRepeatButton_, &QPushButton::clicked, this,
          [this] { emit endModeRequested(PlaylistEndMode::Repeat); });
  connect(endNextButton_, &QPushButton::clicked, this,
          [this] { emit endModeRequested(PlaylistEndMode::Next); });
  connect(endGotoButton_, &QPushButton::clicked, this,
          [this] { emit endModeRequested(PlaylistEndMode::Goto); });
  connect(gotoTargetComboBox_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            if (index < 0) return;
            if (offlineGotoTargets_.contains(
                    gotoTargetComboBox_->itemData(index).toULongLong())) {
              const QSignalBlocker blocker(gotoTargetComboBox_);
              gotoTargetComboBox_->setCurrentIndex(gotoTargetComboBox_->findData(
                  QVariant::fromValue<qulonglong>(displayedSettings_
                      ? displayedSettings_->gotoTargetItemId : 0)));
              return;
            }
            emit gotoTargetRequested(
                gotoTargetComboBox_->itemData(index).toULongLong());
          });
  connect(repeatCountSpinBox_, &QSpinBox::valueChanged, this,
          [this](int count) {
            emit repeatOptionsRequested(
                count, static_cast<PlaylistRepeatEndMode>(
                           repeatEndModeComboBox_->currentData().toInt()));
          });
  connect(repeatEndModeComboBox_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            emit repeatOptionsRequested(
                repeatCountSpinBox_->value(),
                static_cast<PlaylistRepeatEndMode>(
                    repeatEndModeComboBox_->itemData(index).toInt()));
          });
  connect(fadeInCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
    fadeInDurationControl_->setEnabled(fadeInCheckBox_->isEnabled() && enabled);
    emit fadeInRequested(enabled,
                         qRound64(fadeInDurationSpinBox_->value() * 1000.0));
  });
  connect(fadeOutCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
    fadeOutDurationControl_->setEnabled(fadeOutCheckBox_->isEnabled() && enabled);
    emit fadeOutRequested(enabled,
                          qRound64(fadeOutDurationSpinBox_->value() * 1000.0));
  });
  connect(endCrossfadeCheckBox_, &QCheckBox::toggled, this,
          [this](bool enabled) {
            endCrossfadeDurationControl_->setEnabled(
                endCrossfadeCheckBox_->isEnabled() && enabled);
            emit endCrossfadeRequested(
                enabled,
                qRound64(endCrossfadeDurationSpinBox_->value() * 1000.0));
          });
  connect(fadeInDurationSpinBox_, &QDoubleSpinBox::valueChanged, this,
          [this](double seconds) {
            emit fadeInRequested(fadeInCheckBox_->isChecked(),
                                 qRound64(seconds * 1000.0));
          });
  connect(fadeOutDurationSpinBox_, &QDoubleSpinBox::valueChanged, this,
          [this](double seconds) {
            emit fadeOutRequested(fadeOutCheckBox_->isChecked(),
                                  qRound64(seconds * 1000.0));
          });
  connect(endCrossfadeDurationSpinBox_, &QDoubleSpinBox::valueChanged, this,
          [this](double seconds) {
            emit endCrossfadeRequested(
                endCrossfadeCheckBox_->isChecked(),
                qRound64(seconds * 1000.0));
          });
  operations_ = operations ? operations : new ControllerOperations(this);
  using A = ControllerOperations::Action;
  operations_->setGuard<>(A::OpenNominalEq, this,
                          [this] { return nominalSettingsEnabled_; });
  operations_->bindMember(A::OpenNominalEq, this, &EditPanel::openNominalEq);
  connect(nominalEqButton_, &QPushButton::clicked, this,
          [this] { (void)operations_->execute(A::OpenNominalEq); });
  operations_->setGuard<>(A::TogglePfl, this, [this] { return pflAvailable_; });
  operations_->bind<>(A::TogglePfl, this, [this] {
    pflRequestedEnabled_ = !pflRequestedEnabled_;
    { const QSignalBlocker blocker(pflButton_); pflButton_->setChecked(pflRequestedEnabled_); }
    emit pflRequested(pflRequestedEnabled_);
  });
  connect(pflButton_, &QPushButton::clicked, this,
          [this] { (void)operations_->execute(A::TogglePfl); });
  operations_->bind<int>(A::SetEditVolume, this, [this](int value) {
    if (!nominalVolumeFader_->isEnabled() || value < nominalVolumeFader_->minimum()
        || value > nominalVolumeFader_->maximum()) return false;
    nominalVolumeFader_->setValue(value);
    return true;
  });
  operations_->bind<int>(A::AdjustEditVolume, this, [this](int delta) {
    const auto value = std::clamp(qint64(nominalVolumeFader_->value()) + delta,
        qint64(nominalVolumeFader_->minimum()), qint64(nominalVolumeFader_->maximum()));
    return operations_->execute(A::SetEditVolume, int(value));
  });
  operations_->bind<>(A::ResetEditVolume, this,
      [this] { return operations_->execute(A::SetEditVolume, 0); });
  nominalVolumeFader_->setUserValueHandler(
      [this](int value) { (void)operations_->execute(A::SetEditVolume, value); });
  connect(nominalVolumeFader_, &QSlider::valueChanged, this,
          &EditPanel::nominalVolumeRequested);
  connect(seekBar_, &QSlider::sliderMoved, this,
          [this] { emit seekRequested(seekBar_->timelinePosition()); });
  connect(seekBar_, &QSlider::sliderReleased, this,
          [this] { emit seekRequested(seekBar_->timelinePosition()); });

  setTabOrder(startPauseButton_, startPlayButton_);
  setTabOrder(startPlayButton_, fadeInCheckBox_);
  setTabOrder(fadeInCheckBox_, fadeInDurationSpinBox_);
  setTabOrder(fadeInDurationSpinBox_, fadeInDurationIncreaseButton_);
  setTabOrder(fadeInDurationIncreaseButton_, fadeInDurationDecreaseButton_);
  setTabOrder(fadeInDurationDecreaseButton_, endStopButton_);
  setTabOrder(endStopButton_, endPauseButton_);
  setTabOrder(endPauseButton_, endRepeatButton_);
  setTabOrder(endRepeatButton_, endNextButton_);
  setTabOrder(endNextButton_, endGotoButton_);
  setTabOrder(endGotoButton_, fadeOutCheckBox_);
  setTabOrder(fadeOutCheckBox_, fadeOutDurationSpinBox_);
  setTabOrder(fadeOutDurationSpinBox_, fadeOutDurationIncreaseButton_);
  setTabOrder(fadeOutDurationIncreaseButton_, fadeOutDurationDecreaseButton_);
  setTabOrder(fadeOutDurationDecreaseButton_, endCrossfadeCheckBox_);
  setTabOrder(endCrossfadeCheckBox_, endCrossfadeDurationSpinBox_);
  setTabOrder(endCrossfadeDurationSpinBox_,
              endCrossfadeDurationIncreaseButton_);
  setTabOrder(endCrossfadeDurationIncreaseButton_,
              endCrossfadeDurationDecreaseButton_);
  setTabOrder(endCrossfadeDurationDecreaseButton_, gotoTargetComboBox_);
  setTabOrder(gotoTargetComboBox_, repeatCountSpinBox_);
  setTabOrder(repeatCountSpinBox_, repeatCountIncreaseButton_);
  setTabOrder(repeatCountIncreaseButton_, repeatCountDecreaseButton_);
  setTabOrder(repeatCountDecreaseButton_, repeatEndModeComboBox_);

  setAudioLevel(kMeterFloorDb, kMeterFloorDb, false);
  setInputSignalAppearance(false, false);
  retranslateUi();
  vtrpon::preserveFocusOnButtonClick(this);
  vtrpon::preserveFocusOnComboBoxClick(this);
}

QWidget *EditPanel::videoSurface() const { return videoSurface_; }

void EditPanel::setLiveFrame(const QImage &frame) {
  if (!livePreviewWidget_) return;
  livePreviewWidget_->setFrame(frame);
  qobject_cast<QStackedLayout *>(videoMonitor_->layout())->setCurrentWidget(livePreviewWidget_);
}

void EditPanel::clearLiveFrame() {
  if (!livePreviewWidget_) return;
  livePreviewWidget_->setFrame({});
  qobject_cast<QStackedLayout *>(videoMonitor_->layout())->setCurrentWidget(videoSurface_);
}

void EditPanel::setFile(const QString &fileName, const QString &absolutePath) {
  fileName_ = fileName;
  fileAbsolutePath_ = absolutePath;
  updateFilePresentation();
}

void EditPanel::clearFile() {
  fileName_.clear();
  fileAbsolutePath_.clear();
  updateFilePresentation();
}

void EditPanel::updateFilePresentation() {
  update();
  if (fileName_.isEmpty()) {
    fileLabel_->setText(vtrpon::uiText("program.file.none"));
    fileLabel_->setToolTip(QString());
    fileLabel_->setAccessibleName(
        vtrpon::uiText("edit.file.none_accessible"));
    return;
  }
  fileLabel_->setText(fileName_);
  fileLabel_->setToolTip(fileAbsolutePath_);
  fileLabel_->setAccessibleName(
      vtrpon::uiText("edit.file.current_accessible").arg(fileName_));
}

void EditPanel::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
}

bool EditPanel::eventFilter(QObject *watched, QEvent *event) {
  if (watched != videoMonitor_) {
    return QWidget::eventFilter(watched, event);
  }
  if (event->type() == QEvent::Leave) {
    wheelAngleRemainder_ = 0;
    return QWidget::eventFilter(watched, event);
  }
  if (event->type() != QEvent::Wheel || !seekBar_->isEnabled()) {
    return QWidget::eventFilter(watched, event);
  }

  auto *wheelEvent = static_cast<QWheelEvent *>(event);
  const int verticalDelta = wheelEvent->angleDelta().y();
  if (verticalDelta == 0) {
    return QWidget::eventFilter(watched, event);
  }

  constexpr int kWheelDeltaPerStep = 120;
  wheelAngleRemainder_ += verticalDelta;
  const int wheelSteps = wheelAngleRemainder_ / kWheelDeltaPerStep;
  wheelAngleRemainder_ %= kWheelDeltaPerStep;
  wheelEvent->accept();
  if (wheelSteps != 0) {
    // Match the established Electron EDIT behavior: wheel down advances and
    // wheel up moves backward.
    emit frameStepRequested(-wheelSteps);
  }
  return true;
}

void EditPanel::paintEvent(QPaintEvent *event) {
  QWidget::paintEvent(event);
  if (!videoSurface_ || fileName_.isEmpty()) {
    return;
  }
  QWidget *monitor = videoSurface_->parentWidget();
  const QRectF frameRect(monitor->mapTo(this, QPoint(0, 0)), monitor->size());
  QPainter painter(this);
  vtrpon::drawMonitorNeonOutline(painter, frameRect,
                                QColor(QStringLiteral("#5fc36a")));
}

void EditPanel::retranslateUi() {
  vtrpon::retranslateAudioDelayControl(nominalDelaySpin_, "audio.delay.nominal");
  vtrpon::updateEqButton(nominalEqButton_, nominalEqTenthsDb_, "edit.eq.title");
  findChild<QWidget *>(QStringLiteral("editMonitor"))
      ->setAccessibleName(vtrpon::uiText("edit.monitor_accessible"));
  meter_->setAccessibleName(vtrpon::uiText("edit.meter_accessible"));
  seekBar_->setAccessibleName(vtrpon::uiText("edit.seek_accessible"));
  updateFilePresentation();
  findChild<QLabel *>(QStringLiteral("editSectionTitle"))
      ->setText(vtrpon::uiText("edit.section"));

  const auto setModuleText = [this](const char *objectName,
                                    const char *titleId,
                                    const char *accessibleId) {
    QWidget *module = findChild<QWidget *>(QString::fromLatin1(objectName));
    if (!module) {
      return;
    }
    module->setAccessibleName(vtrpon::uiText(accessibleId));
    if (QLabel *title = module->findChild<QLabel *>(
            QStringLiteral("editControlModuleTitle"))) {
      title->setText(vtrpon::uiText(titleId));
    }
  };
  findChild<QWidget *>(QStringLiteral("editTransportModule"))
      ->setAccessibleName(vtrpon::uiText("edit.transport.accessible"));
  setModuleText("editPointsModule", "edit.points.title",
                "edit.points.accessible");
  setModuleText("editStartModeModule", "edit.start_mode.title",
                "edit.start_mode.accessible");
  setModuleText("editEndModeModule", "edit.end_mode.title",
                "edit.end_mode.accessible");

  const auto setButtonText = [](QPushButton *button, const char *nameId,
                                const char *tooltipId) {
    button->setAccessibleName(vtrpon::uiText(nameId));
    button->setToolTip(vtrpon::uiText(tooltipId));
  };
  setButtonText(jumpToStartButton_, "edit.jump_start.accessible",
                "edit.jump_start.tooltip");
  setButtonText(jumpToInButton_, "edit.jump_in.accessible",
                "edit.jump_in.tooltip");
  setButtonText(rewindButton_, "edit.rewind.accessible",
                "edit.rewind.tooltip");
  setButtonText(playButton_, "edit.play.accessible", "common.play");
  setButtonText(pauseButton_, "edit.pause.accessible", "common.pause");
  setButtonText(forwardButton_, "edit.forward.accessible",
                "edit.forward.tooltip");
  setButtonText(jumpToOutButton_, "edit.jump_out.accessible",
                "edit.jump_out.tooltip");
  setButtonText(jumpToEndButton_, "edit.jump_end.accessible",
                "edit.jump_end.tooltip");

  inButton_->setText(vtrpon::uiText("edit.set_in"));
  outButton_->setText(vtrpon::uiText("edit.set_out"));
  setButtonText(inButton_, "edit.set_in.accessible", "edit.set_in.tooltip");
  setButtonText(outButton_, "edit.set_out.accessible", "edit.set_out.tooltip");
  inPointTimeEdit_->setAccessibleName(vtrpon::uiText("edit.in_time.accessible"));
  outPointTimeEdit_->setAccessibleName(vtrpon::uiText("edit.out_time.accessible"));
  inPointTimeEdit_->setToolTip(vtrpon::uiText("edit.in_time.tooltip"));
  outPointTimeEdit_->setToolTip(vtrpon::uiText("edit.out_time.tooltip"));

  startPauseButton_->setText(vtrpon::uiText("common.pause").toUpper());
  startPlayButton_->setText(vtrpon::uiText("common.play").toUpper());
  endStopButton_->setText(vtrpon::uiText("common.stop").toUpper());
  endPauseButton_->setText(vtrpon::uiText("common.pause").toUpper());
  endRepeatButton_->setText(vtrpon::uiText("common.repeat").toUpper());
  endNextButton_->setText(vtrpon::uiText("common.next").toUpper());
  endGotoButton_->setText(vtrpon::uiText("common.goto").toUpper());
  setButtonText(startPauseButton_, "edit.start_pause.accessible",
                "edit.start_pause.tooltip");
  setButtonText(startPlayButton_, "edit.start_play.accessible",
                "edit.start_play.tooltip");
  setButtonText(endStopButton_, "edit.end_stop.accessible",
                "edit.end_stop.tooltip");
  setButtonText(endPauseButton_, "edit.end_pause.accessible",
                "edit.end_pause.tooltip");
  setButtonText(endRepeatButton_, "edit.end_repeat.accessible",
                "edit.end_repeat.tooltip");
  setButtonText(endNextButton_, "edit.end_next.accessible",
                "edit.end_next.tooltip");
  setButtonText(endGotoButton_, "edit.end_goto.accessible",
                "edit.end_goto.tooltip");

  fadeInCheckBox_->setText(vtrpon::uiText("edit.fade_in"));
  fadeOutCheckBox_->setText(vtrpon::uiText("edit.fade_out"));
  endCrossfadeCheckBox_->setText(vtrpon::uiText("edit.end_crossfade"));
  fadeInCheckBox_->setAccessibleName(vtrpon::uiText("edit.fade_in.accessible"));
  fadeOutCheckBox_->setAccessibleName(vtrpon::uiText("edit.fade_out.accessible"));
  endCrossfadeCheckBox_->setAccessibleName(
      vtrpon::uiText("edit.end_crossfade.accessible"));
  fadeInDurationSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.fade_in_duration.accessible"));
  fadeOutDurationSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.fade_out_duration.accessible"));
  endCrossfadeDurationSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.end_crossfade_duration.accessible"));
  retranslateFadeDurationButtons(
      fadeInDurationSpinBox_, fadeInDurationIncreaseButton_,
      fadeInDurationDecreaseButton_);
  retranslateFadeDurationButtons(
      fadeOutDurationSpinBox_, fadeOutDurationIncreaseButton_,
      fadeOutDurationDecreaseButton_);
  retranslateFadeDurationButtons(
      endCrossfadeDurationSpinBox_, endCrossfadeDurationIncreaseButton_,
      endCrossfadeDurationDecreaseButton_);
  fadeInCheckBox_->setToolTip(vtrpon::uiText("edit.fade_in.tooltip"));
  fadeOutCheckBox_->setToolTip(vtrpon::uiText("edit.fade_out.tooltip"));
  endCrossfadeCheckBox_->setToolTip(
      vtrpon::uiText("edit.end_crossfade.tooltip"));
  gotoTargetLabel_->setText(vtrpon::uiText("edit.goto_target"));
  gotoTargetComboBox_->setAccessibleName(
      vtrpon::uiText("edit.goto_target.accessible"));
  gotoTargetComboBox_->setToolTip(
      vtrpon::uiText("edit.goto_target.tooltip"));
  updateGotoTargetItems(displayedSettings_
                            ? displayedSettings_->gotoTargetItemId
                            : 0);
  repeatOptionLabel_->setText(vtrpon::uiText("edit.repeat_option"));
  repeatCountLabel_->setText(vtrpon::uiText("edit.repeat_count"));
  repeatEndModeLabel_->setText(vtrpon::uiText("edit.repeat_end_mode"));
  repeatCountSpinBox_->setAccessibleName(
      vtrpon::uiText("edit.repeat_count.accessible"));
  repeatCountSpinBox_->setToolTip(
      vtrpon::uiText("edit.repeat_count.tooltip"));
  const QString repeatCountIncreaseText =
      vtrpon::uiText("edit.repeat_count.increase.accessible");
  repeatCountIncreaseButton_->setAccessibleName(repeatCountIncreaseText);
  repeatCountIncreaseButton_->setToolTip(repeatCountIncreaseText);
  const QString repeatCountDecreaseText =
      vtrpon::uiText("edit.repeat_count.decrease.accessible");
  repeatCountDecreaseButton_->setAccessibleName(repeatCountDecreaseText);
  repeatCountDecreaseButton_->setToolTip(repeatCountDecreaseText);
  repeatEndModeComboBox_->setAccessibleName(
      vtrpon::uiText("edit.repeat_end_mode.accessible"));
  repeatEndModeComboBox_->setToolTip(
      vtrpon::uiText("edit.repeat_end_mode.tooltip"));
  updateRepeatEndModeItems(displayedSettings_
                               ? displayedSettings_->repeatEndMode
                               : PlaylistRepeatEndMode::Pause);

  if (QWidget *strip = findChild<QWidget *>(
          QStringLiteral("editNominalVolumeStrip"))) {
    strip->setAccessibleName(vtrpon::uiText("edit.nominal_strip.accessible"));
  }
  inputSignalIndicator_->setAccessibleName(
      vtrpon::uiText("edit.input_signal.accessible"));
  findChild<QLabel *>(QStringLiteral("editNominalVolumeLabel"))
      ->setText(vtrpon::uiText("edit.nominal_volume"));
  updateEditPointButtonAppearance();
  setAudioLevel(audioLeftPeakDb_, audioRightPeakDb_, audioClipped_);
  setInputSignalAppearance(inputSignalActive_, inputSignalClipped_);
}

void EditPanel::setStatus(const QString &text, bool error) {
  if (!userInfoCenter_ || !error) {
    pendingStatusDetail_.clear();
    return;
  }
  static constexpr auto knownErrors = std::to_array<std::pair<const char *, const char *>>({
      {"Could not create the EDIT player (ON AIR will continue)", "edit.error.player_create"},
      {"Could not unload the item from EDIT.", "edit.error.unload"},
      {"Could not go to the EDIT In point.", "edit.error.go_in"},
      {"Could not go to the EDIT Out point.", "edit.error.go_out"},
      {"Could not determine the EDIT end position.", "edit.error.end_position"},
      {"The EDIT player is unavailable (ON AIR will continue)", "edit.error.player_unavailable"},
      {"The file to load into EDIT could not be found.", "edit.error.file_missing"},
      {"Failed to load the file into EDIT.", "edit.error.load"},
      {"The In/Out points of an item currently on air cannot be changed.", "edit.error.points_locked"},
      {"Could not determine the current EDIT position.", "edit.error.current_position"},
      {"Set the In/Out point within the media duration.", "edit.error.point_outside_media"},
      {"Set the In point before the Out point and the end of the media.", "edit.error.in_order"},
      {"Set the Out point after the In point.", "edit.error.out_order"},
      {"Could not save the In/Out point.", "edit.error.save_points"},
      {"Could not save the Start Mode.", "edit.error.save_start_mode"},
      {"Could not save the End Mode.", "edit.error.save_end_mode"},
      {"Could not save the GOTO destination.",
       "edit.error.save_goto_target"},
      {"Could not save the repeat options.",
       "edit.error.save_repeat_options"},
      {"Could not save the fade-in settings.", "edit.error.save_fade_in"},
      {"Could not save the fade-out settings.", "edit.error.save_fade_out"},
      {"Could not save the end crossfade settings.",
       "edit.error.save_end_crossfade"},
      {"Could not save the nominal volume.", "edit.error.save_nominal"},
      {"EDIT Play failed.", "edit.error.play"},
      {"EDIT Pause failed.", "edit.error.pause"},
      {"Failed to stop EDIT.", "edit.error.stop"},
      {"Could not determine the EDIT scan position.", "edit.error.scan_position"},
      {"Failed to prepare EDIT for rewind.", "edit.error.prepare_rewind"},
      {"EDIT scan failed.", "edit.error.scan"},
      {"Failed to restore normal EDIT playback.", "edit.error.restore_playback"},
      {"EDIT seek failed.", "edit.error.seek"},
      {"EDIT ERROR | Only EDIT playback was stopped", "edit.error.playback_only_stopped"},
  });
  for (const auto &[english, id] : knownErrors) {
    if (text == QLatin1StringView(english)) {
      setStatus(vtrpon::LocalizedText(id), true);
      return;
    }
  }
  QString userText = text;
  for (const QString &prefix : {QStringLiteral("EDIT ERROR | "),
                                QStringLiteral("EDIT | "),
                                QStringLiteral("EDIT ")}) {
    if (userText.startsWith(prefix)) {
      userText.remove(0, prefix.size());
      break;
    }
  }
  userInfoCenter_->showError(QStringLiteral("EDIT"), userText,
                             pendingStatusDetail_);
  pendingStatusDetail_.clear();
}

void EditPanel::setStatus(const vtrpon::LocalizedText &text, bool error) {
  if (!userInfoCenter_ || !error) {
    pendingStatusDetail_.clear();
    return;
  }
  userInfoCenter_->showError(QStringLiteral("EDIT"), text,
                             pendingStatusDetail_);
  pendingStatusDetail_.clear();
}

void EditPanel::setStatusToolTip(const QString &text) {
  pendingStatusDetail_ = text;
}

void EditPanel::setTransportEnabled(bool enabled) {
  seekBar_->setEnabled(enabled);
  for (QPushButton *button :
       {jumpToStartButton_, jumpToInButton_, rewindButton_, pauseButton_,
        playButton_, forwardButton_, jumpToOutButton_, jumpToEndButton_}) {
    button->setEnabled(enabled);
  }
  if (!enabled) {
    setButtonActive(rewindButton_, false);
    setButtonActive(forwardButton_, false);
  }
  setPlaybackState(false);
}

void EditPanel::setPlaybackState(bool playbackActive) {
  if (playButton_->property("active").toBool() != playbackActive)
    qInfo() << "STATE EDIT playing=" << playbackActive;
  setButtonActive(playButton_, playbackActive);
  setButtonActive(pauseButton_, !playbackActive && pauseButton_->isEnabled());
}

void EditPanel::setEditorState(
    const std::optional<PlaylistItemSettings> &settings,
    bool editPointsEnabled, bool otherSettingsEnabled,
    const QList<PlaylistItemState> &gotoTargets,
    const QSet<quint64> &offlineGotoTargets, bool liveSource) {
  displayedSettings_ = settings;
  gotoTargets_ = gotoTargets;
  offlineGotoTargets_ = offlineGotoTargets;
  const PlaylistItemSettings displayed =
      settings.value_or(PlaylistItemSettings{});
  startPauseButton_->setChecked(displayed.startMode ==
                                PlaylistStartMode::Pause);
  startPlayButton_->setChecked(displayed.startMode == PlaylistStartMode::Play);
  endStopButton_->setChecked(displayed.endMode == PlaylistEndMode::Stop);
  endPauseButton_->setChecked(displayed.endMode == PlaylistEndMode::Pause);
  endRepeatButton_->setChecked(displayed.endMode == PlaylistEndMode::Repeat);
  endNextButton_->setChecked(displayed.endMode == PlaylistEndMode::Next);
  endGotoButton_->setChecked(displayed.endMode == PlaylistEndMode::Goto);
  const QSignalBlocker fadeInCheckBlocker(fadeInCheckBox_);
  const QSignalBlocker fadeInDurationBlocker(fadeInDurationSpinBox_);
  const QSignalBlocker fadeOutCheckBlocker(fadeOutCheckBox_);
  const QSignalBlocker fadeOutDurationBlocker(fadeOutDurationSpinBox_);
  const QSignalBlocker endCrossfadeCheckBlocker(endCrossfadeCheckBox_);
  const QSignalBlocker endCrossfadeDurationBlocker(
      endCrossfadeDurationSpinBox_);
  const QSignalBlocker repeatCountBlocker(repeatCountSpinBox_);
  const QSignalBlocker repeatEndModeBlocker(repeatEndModeComboBox_);
  const QSignalBlocker nominalVolumeBlocker(nominalVolumeFader_);
  fadeInCheckBox_->setChecked(displayed.fadeInEnabled);
  fadeInDurationSpinBox_->setValue(displayed.fadeInDurationMs / 1000.0);
  fadeOutCheckBox_->setChecked(displayed.fadeOutEnabled);
  fadeOutDurationSpinBox_->setValue(displayed.fadeOutDurationMs / 1000.0);
  endCrossfadeCheckBox_->setChecked(displayed.endCrossfadeEnabled);
  endCrossfadeDurationSpinBox_->setValue(
      displayed.endCrossfadeDurationMs / 1000.0);
  repeatCountSpinBox_->setValue(displayed.repeatCount);
  updateRepeatEndModeItems(displayed.repeatEndMode);
  nominalVolumeFader_->setValue(displayed.nominalVolumeTenthsDb);
  {
    const QSignalBlocker blocker(nominalDelaySpin_);
    nominalDelaySpin_->setValue(displayed.nominalAudioDelayFrames);
  }
  nominalEqTenthsDb_ = displayed.nominalEqTenthsDb;
  vtrpon::updateEqButton(nominalEqButton_, nominalEqTenthsDb_, "edit.eq.title");
  if (nominalEqEditor_) nominalEqEditor_->setGains(nominalEqTenthsDb_);
  updateGotoTargetItems(displayed.gotoTargetItemId);
  for (QPushButton *button : {inButton_, outButton_}) {
    button->setEnabled(editPointsEnabled);
  }
  inPointTimeEdit_->setEnabled(editPointsEnabled);
  outPointTimeEdit_->setEnabled(editPointsEnabled);
  for (QPushButton *button : {startPauseButton_, startPlayButton_,
                              endStopButton_, endPauseButton_,
                              endRepeatButton_, endNextButton_,
                              endGotoButton_}) {
    button->setEnabled(otherSettingsEnabled && !liveSource);
  }
  fadeInCheckBox_->setEnabled(otherSettingsEnabled);
  fadeOutCheckBox_->setEnabled(otherSettingsEnabled && !liveSource);
  const bool endCrossfadeAvailable =
      otherSettingsEnabled && !liveSource &&
      (displayed.endMode == PlaylistEndMode::Next ||
       displayed.endMode == PlaylistEndMode::Goto ||
       displayed.endMode == PlaylistEndMode::Repeat);
  endCrossfadeCheckBox_->setEnabled(endCrossfadeAvailable);
  fadeInDurationControl_->setEnabled(otherSettingsEnabled &&
                                     displayed.fadeInEnabled);
  fadeOutDurationControl_->setEnabled(otherSettingsEnabled && !liveSource &&
                                      displayed.fadeOutEnabled);
  endCrossfadeDurationControl_->setEnabled(
      endCrossfadeAvailable && displayed.endCrossfadeEnabled);
  const bool gotoTargetAvailable =
      otherSettingsEnabled && !liveSource &&
      (displayed.endMode == PlaylistEndMode::Goto ||
       (displayed.endMode == PlaylistEndMode::Repeat &&
        displayed.repeatEndMode == PlaylistRepeatEndMode::Goto));
  gotoTargetLabel_->setEnabled(gotoTargetAvailable);
  gotoTargetComboBox_->setEnabled(gotoTargetAvailable);
  const bool repeatOptionsAvailable =
      otherSettingsEnabled && !liveSource && displayed.endMode == PlaylistEndMode::Repeat;
  for (QWidget *widget : std::array<QWidget *, 5>{
           repeatOptionLabel_, repeatCountLabel_, repeatCountControl_,
           repeatEndModeLabel_, repeatEndModeComboBox_}) {
    widget->setEnabled(repeatOptionsAvailable);
  }
  nominalSettingsEnabled_ = otherSettingsEnabled;
  nominalVolumeFader_->setEnabled(otherSettingsEnabled);
  nominalEqButton_->setEnabled(otherSettingsEnabled);
  nominalDelaySpin_->setEnabled(otherSettingsEnabled && !liveSource);
  if (nominalEqEditor_) {
    nominalEqEditor_->setEnabled(otherSettingsEnabled);
    if (!settings) nominalEqEditor_->hide();
  }
  if (settings) {
    seekBar_->setRangeMarkers(settings->inPointMs, settings->outPointMs);
  } else {
    seekBar_->clearRangeMarkers();
  }
  updateEditPointTimeDisplays();
  updateEditPointButtonAppearance();
}

void EditPanel::updateGotoTargetItems(quint64 selectedTargetItemId) {
  if (!gotoTargetComboBox_) {
    return;
  }
  const QSignalBlocker blocker(gotoTargetComboBox_);
  gotoTargetComboBox_->clear();
  gotoTargetComboBox_->addItem(
      vtrpon::uiText("edit.goto_target.placeholder"),
      QVariant::fromValue<qulonglong>(0));
  int selectedIndex = 0;
  for (const PlaylistItemState &target : gotoTargets_) {
    gotoTargetComboBox_->addItem(
        vtrpon::uiText("edit.goto_target.item")
            .arg(target.playlistNumber)
            .arg(target.itemNumber, 3, 10, QLatin1Char('0'))
            .arg(target.displayName),
        QVariant::fromValue<qulonglong>(target.id));
    updateGotoTargetAvailability(gotoTargetComboBox_->count() - 1, target);
    if (target.id == selectedTargetItemId) {
      selectedIndex = gotoTargetComboBox_->count() - 1;
    }
  }
  gotoTargetComboBox_->setCurrentIndex(selectedIndex);
}

void EditPanel::updateGotoTargetAvailability(
    int index, const PlaylistItemState &target) {
  const bool offline = offlineGotoTargets_.contains(target.id);
  const QString label = vtrpon::uiText("edit.goto_target.item")
      .arg(target.playlistNumber)
      .arg(target.itemNumber, 3, 10, QLatin1Char('0'))
      .arg(target.displayName);
  gotoTargetComboBox_->setItemText(index,
      offline ? label + QStringLiteral(" [MEDIA OFFLINE]") : label);
  if (auto *model = qobject_cast<QStandardItemModel *>(gotoTargetComboBox_->model())) {
    if (auto *item = model->item(index)) {
      item->setEnabled(!offline);
      item->setSelectable(!offline);
    }
  }
}

void EditPanel::setGotoTargetOffline(quint64 itemId, bool offline) {
  if (offline) offlineGotoTargets_.insert(itemId);
  else offlineGotoTargets_.remove(itemId);
  // Update in place so an open popup and the saved destination stay intact.
  const QSignalBlocker blocker(gotoTargetComboBox_);
  for (int index = 0; index < gotoTargets_.size(); ++index) {
    if (gotoTargets_[index].id == itemId) {
      updateGotoTargetAvailability(index + 1, gotoTargets_[index]);
      break;
    }
  }
}

void EditPanel::updateRepeatEndModeItems(PlaylistRepeatEndMode selectedMode) {
  if (!repeatEndModeComboBox_) {
    return;
  }
  const QSignalBlocker blocker(repeatEndModeComboBox_);
  repeatEndModeComboBox_->clear();
  const auto addMode = [this](const char *textId, PlaylistRepeatEndMode mode) {
    repeatEndModeComboBox_->addItem(
        vtrpon::uiText(textId), static_cast<int>(mode));
  };
  addMode("common.stop", PlaylistRepeatEndMode::Stop);
  addMode("common.pause", PlaylistRepeatEndMode::Pause);
  addMode("common.next", PlaylistRepeatEndMode::Next);
  addMode("common.goto", PlaylistRepeatEndMode::Goto);
  const int selectedIndex = repeatEndModeComboBox_->findData(
      static_cast<int>(selectedMode));
  const int pauseIndex = repeatEndModeComboBox_->findData(
      static_cast<int>(PlaylistRepeatEndMode::Pause));
  repeatEndModeComboBox_->setCurrentIndex(
      selectedIndex >= 0 ? selectedIndex : pauseIndex);
}

void EditPanel::resetForLoadedMedia() {
  setTimelineDuration(-1);
  seekBar_->setTimelinePosition(0);
  resetAudioPresentation();
}

void EditPanel::setTimelineDuration(qint64 durationMs) {
  const bool durationChanged = timelineDurationMs_ != durationMs;
  timelineDurationMs_ = durationMs;
  seekBar_->setTimelineDuration(durationMs);
  if (durationChanged) {
    updateEditPointTimeDisplays();
    updateEditPointButtonAppearance();
  }
}

void EditPanel::setTimelinePosition(qint64 positionMs) {
  seekBar_->setTimelinePosition(positionMs);
}

bool EditPanel::isTimelineSliderDown() const {
  return seekBar_->isSliderDown();
}

void EditPanel::setAudioLevel(double leftPeakDb, double rightPeakDb,
                              bool clipped) {
  audioLeftPeakDb_ = leftPeakDb;
  audioRightPeakDb_ = rightPeakDb;
  audioClipped_ = clipped;
  meter_->setPeakLevelsDbfs(leftPeakDb, rightPeakDb, clipped);
  meter_->setAccessibleDescription(
      vtrpon::uiText("edit.audio_level.accessible")
          .arg(leftPeakDb, 0, 'f', 1)
          .arg(rightPeakDb, 0, 'f', 1)
          .arg(clipped ? vtrpon::uiText("audio.clipping_suffix")
                       : QString()));
}

void EditPanel::setInputSignalAppearance(bool active, bool clipped) {
  inputSignalActive_ = active;
  inputSignalClipped_ = clipped;
  inputSignalIndicator_->setProperty("signalActive", active);
  inputSignalIndicator_->setProperty("signalClipped", clipped);
  inputSignalIndicator_->style()->unpolish(inputSignalIndicator_);
  inputSignalIndicator_->style()->polish(inputSignalIndicator_);
  inputSignalIndicator_->update();
  inputSignalIndicator_->setAccessibleDescription(
      clipped  ? vtrpon::uiText("audio.input.clipping")
      : active ? vtrpon::uiText("audio.input.detected")
               : vtrpon::uiText("audio.input.none"));
}

void EditPanel::resetAudioPresentation() {
  meter_->resetToSilence();
  setAudioLevel(kMeterFloorDb, kMeterFloorDb, false);
  setInputSignalAppearance(false, false);
}

void EditPanel::setPflState(bool enabled, bool canEnable, const QString &status) {
  pflRequestedEnabled_ = enabled;
  pflAvailable_ = canEnable;
  const QSignalBlocker blocker(pflButton_);
  pflButton_->setChecked(enabled);
  pflButton_->setEnabled(canEnable);
  pflButton_->setToolTip(status);
  pflButton_->setAccessibleName(QStringLiteral("PFL %1").arg(
      vtrpon::uiText(enabled ? "common.on" : "common.off")));
  pflButton_->setAccessibleDescription(status);
  if (pflButton_->property("pflActive").toBool() != enabled) {
    pflButton_->setProperty("pflActive", enabled);
    pflButton_->style()->unpolish(pflButton_);
    pflButton_->style()->polish(pflButton_);
    pflButton_->update();
  }
}

void EditPanel::openNominalEq() {
  if (vtrpon::showVstEditor(nominalEqButton_, vtrpon::VstSlot::Edit)) return;
  if (!nominalEqEditor_) {
    nominalEqEditor_ = new vtrpon::EqualizerEditor(this, QStringLiteral("editNominalEq"),
        "edit.eq.title", [this](std::array<int, 3> gains) {
          if (!nominalSettingsEnabled_) return;
          emit nominalEqRequested(gains[0], gains[1], gains[2]);
        });
  }
  nominalEqEditor_->setGains(nominalEqTenthsDb_);
  nominalEqEditor_->openEditor();
}

void EditPanel::commitEditPointTime(QLineEdit *editor, bool inPoint) {
  if (!displayedSettings_ || !editor || !editor->isEnabled()) {
    updateEditPointTimeDisplays();
    return;
  }

  const QString enteredText = editor->text().trimmed();
  if (enteredText ==
      editor->property(kDisplayedPointTextProperty).toString()) {
    updateEditPointTimeDisplays();
    return;
  }

  const std::optional<qint64> parsed = parseEditPointTime(enteredText);
  if (!parsed) {
    updateEditPointTimeDisplays();
    setStatusToolTip(
        vtrpon::uiText("edit.time_format.tooltip"));
    setStatus(vtrpon::LocalizedText("edit.time_format.invalid"), true);
    return;
  }

  const qint64 displayedPoint =
      inPoint
          ? displayedSettings_->inPointMs
          : displayedSettings_->outPointMs.value_or(
                (std::max)(timelineDurationMs_, qint64{0}));
  if (*parsed == displayedPoint) {
    updateEditPointTimeDisplays();
    return;
  }

  if (inPoint) {
    emit inPointTimeRequested(*parsed);
  } else {
    emit outPointTimeRequested(*parsed);
  }
}

void EditPanel::updateEditPointTimeDisplays() {
  const auto setDisplayedText = [](QLineEdit *editor, const QString &text) {
    editor->setText(text);
    editor->setProperty(kDisplayedPointTextProperty, text);
  };
  if (!displayedSettings_) {
    setDisplayedText(inPointTimeEdit_, QString());
    setDisplayedText(outPointTimeEdit_, QString());
    return;
  }

  setDisplayedText(inPointTimeEdit_,
                   formatEditPointTime(displayedSettings_->inPointMs));
  const std::optional<qint64> outPoint =
      displayedSettings_->outPointMs
          ? displayedSettings_->outPointMs
          : timelineDurationMs_ >= 0
                ? std::optional<qint64>{timelineDurationMs_}
                : std::nullopt;
  setDisplayedText(outPointTimeEdit_,
                   outPoint ? formatEditPointTime(*outPoint) : QString());
}

void EditPanel::updateEditPointButtonAppearance() {
  const bool customIn =
      displayedSettings_ && displayedSettings_->inPointMs != 0;
  const bool customOut =
      displayedSettings_ && displayedSettings_->outPointMs &&
      timelineDurationMs_ >= 0 &&
      *displayedSettings_->outPointMs != timelineDurationMs_;

  setButtonActive(inButton_, customIn);
  setButtonActive(outButton_, customOut);
  inButton_->setAccessibleDescription(
      customIn ? vtrpon::uiText("edit.in_point.custom")
               : vtrpon::uiText("edit.in_point.default"));
  outButton_->setAccessibleDescription(
      customOut ? vtrpon::uiText("edit.out_point.custom")
                : vtrpon::uiText("edit.out_point.default"));
}
int EditPanel::nominalVolumeValue() const { return nominalVolumeFader_->value(); }
