#include "audio_fader_widget.h"
#include "controller_volume_input.h"
#include "audio_meter_widget.h"
#include "edit_panel.h"
#include "program_panel.h"
#include "controller_scaling.h"
#include "controller_window_scaling.h"
#include "user_info.h"
#include "vst_service.h"
#include "midi_control_input.h"
#include "controller_settings.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QDir>
#include <QFileInfo>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDial>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QTemporaryDir>
#include <QVariantAnimation>
#include <QWidget>
#include <QWheelEvent>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vtrpon {
class MidiControlTestAccess {
public:
  static void send(MidiControlInput &input, quint32 packed) {
    input.processMessage(midiControl::decode(packed));
  }
  static void enqueue(MidiControlInput &input, quint32 packed) { input.enqueueMessage(packed); }
  static void drain(MidiControlInput &input) { input.pollMessages(); }
};
}

namespace {

constexpr int kCompactDurationWidth = 41;
constexpr int kCompactRepeatCountWidth = 36;
constexpr int kProgramAudioStripWidth = 224;
constexpr int kExtendedAudioTopInset = 22;
constexpr int kPlaybackRateButtonWidth = 38;
constexpr int kPlaybackRateButtonHeight = 20;
constexpr int kPlaybackRateButtonSpacing = 8;

bool check(bool condition, const char *expression, int line) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "line %d: check failed: %s\n", line, expression);
  return false;
}

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!check((expression), #expression, __LINE__)) {                          \
      return 1;                                                                 \
    }                                                                           \
  } while (false)

struct DurationControlIds {
  const char *spinBox;
  const char *increaseButton;
  const char *decreaseButton;
};

constexpr auto kDurationControls = std::to_array<DurationControlIds>({
    {"editFadeInDurationSpinBox", "editFadeInDurationIncreaseButton",
     "editFadeInDurationDecreaseButton"},
    {"editFadeOutDurationSpinBox", "editFadeOutDurationIncreaseButton",
     "editFadeOutDurationDecreaseButton"},
    {"editEndCrossfadeDurationSpinBox",
     "editEndCrossfadeDurationIncreaseButton",
     "editEndCrossfadeDurationDecreaseButton"},
});

struct ProgramFadeControlIds {
  const char *button;
  const char *buttonText;
  const char *spinBox;
  const char *increaseButton;
  const char *decreaseButton;
};

constexpr auto kProgramFadeControls = std::to_array<ProgramFadeControlIds>({
    {"programItemFadeInButton", "F/I", "programItemFadeInDurationSpinBox",
     "programItemFadeInDurationIncreaseButton",
     "programItemFadeInDurationDecreaseButton"},
    {"programItemFadeOutButton", "F/O", "programItemFadeOutDurationSpinBox",
     "programItemFadeOutDurationIncreaseButton",
     "programItemFadeOutDurationDecreaseButton"},
    {"programMasterFadeInButton", "F/I", "programMasterFadeInDurationSpinBox",
     "programMasterFadeInDurationIncreaseButton",
     "programMasterFadeInDurationDecreaseButton"},
    {"programMasterFadeOutButton", "F/O", "programMasterFadeOutDurationSpinBox",
     "programMasterFadeOutDurationIncreaseButton",
     "programMasterFadeOutDurationDecreaseButton"},
});

template <typename Predicate>
bool waitUntil(QApplication &application, Predicate predicate,
               int timeoutMs = 800) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMs) {
    application.processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  application.processEvents();
  return predicate();
}

void processFor(QApplication &application, int durationMs) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < durationMs) {
    application.processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  application.processEvents();
}

bool meterGreenPixelsStayInside(const QPixmap &pixmap,
                                const QRect &logicalMeterRect) {
  const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
  const qreal dpr = pixmap.devicePixelRatio();
  const QRect meterRect(qRound(logicalMeterRect.x() * dpr),
                        qRound(logicalMeterRect.y() * dpr),
                        qRound(logicalMeterRect.width() * dpr),
                        qRound(logicalMeterRect.height() * dpr));
  const QRgb meterGreen = qRgb(0x16, 0xc7, 0x33);
  int greenPixelCount = 0;
  for (int y = 0; y < image.height(); ++y) {
    const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      if (line[x] != meterGreen) {
        continue;
      }
      ++greenPixelCount;
      if (!meterRect.contains(x, y)) {
        return false;
      }
    }
  }
  return greenPixelCount > 0;
}

QRgb pixelAtLogicalPoint(const QPixmap &pixmap, const QPoint &logicalPoint) {
  const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
  const qreal dpr = pixmap.devicePixelRatio();
  const QPoint pixelPoint(qRound(logicalPoint.x() * dpr),
                          qRound(logicalPoint.y() * dpr));
  if (!image.rect().contains(pixelPoint)) {
    return 0;
  }
  return image.pixel(pixelPoint);
}

bool rgbChannelsWithin(QRgb left, QRgb right, int tolerance) {
  return std::abs(qRed(left) - qRed(right)) <= tolerance &&
         std::abs(qGreen(left) - qGreen(right)) <= tolerance &&
         std::abs(qBlue(left) - qBlue(right)) <= tolerance;
}

int verifyControllerDensity(QApplication &application,
                            const QList<QStringList> &skins) {
  const QFont baseFont = application.font();
  // Physical control sizes must agree across OS scales, without double scaling.
  for (const QSize resolution : {QSize(1920, 1080), QSize(1920, 1200),
                                 QSize(2048, 1080), QSize(2560, 1440),
                                 QSize(2560, 1600), QSize(3440, 1440),
                                 QSize(3840, 2160), QSize(7680, 4320)}) {
    const qreal expected = std::min(resolution.width() / 1920.0,
                                    resolution.height() / 1080.0);
    for (const qreal osScale : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
      const QSizeF logical = QSizeF(resolution) / osScale;
      const QSizeF available(logical.width(), logical.height() - 48);
      vtrpon::setUiScale(vtrpon::controllerScaleForGeometry(logical, available));
      CHECK(std::abs(vtrpon::uiScale() * osScale - expected) < 0.0001);
      CHECK(std::abs(vtrpon::scalePx(550) * osScale - 550 * expected)
            <= osScale / 2 + 0.001);
    }
  }
  CHECK(vtrpon::controllerScaleForGeometry({}, {}) == 1.0);
  const qreal limited = vtrpon::controllerScaleForGeometry(
      QSizeF(1920, 1080), QSizeF(1700, 700));
  CHECK(limited * 1886 <= 1700 && limited * 805 <= 700);
  vtrpon::setUiScale(2.0);
  CHECK(vtrpon::scalePx(0) == 0);
  CHECK(vtrpon::scalePx(-5) == -10);
  CHECK(vtrpon::scaledStyleSheet(QStringLiteral(
      "padding: 0px; margin: -2px; border: 1.5px; font-size: 9pt;"))
      == QStringLiteral(
          "padding: 0px; margin: -4px; border: 3px; font-size: 18.0000pt;"));

  for (const QStringList &paths : skins) {
    QString sheet;
    for (const QString &path : paths) {
      QFile file(path);
      CHECK(file.open(QIODevice::ReadOnly | QIODevice::Text));
      sheet += QString::fromUtf8(file.readAll()) + QLatin1Char('\n');
    }
    application.setProperty("controllerSkinUsesHardwareEffects",
        paths.first().contains(QStringLiteral("hardware")));
    int baselinePflWidth = 0;
    for (const qreal scale : {1.0, 2.0 / 3.0, 4.0 / 3.0, 2.0}) {
      vtrpon::setUiScale(scale);
      QFont font = baseFont;
      font.setPointSizeF(baseFont.pointSizeF() * scale);
      application.setFont(font);
      application.setStyleSheet(vtrpon::scaledStyleSheet(sheet));
      QWidget window;
      window.setObjectName(QStringLiteral("controllerWindow"));
      auto *layout = new QHBoxLayout(&window);
      UserInfoCenter info;
      auto *edit = new EditPanel(&info, &window);
      auto *program = new ProgramPanel(&window);
      layout->addWidget(edit);
      layout->addWidget(program);
      layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                                vtrpon::scalePx(12), vtrpon::scalePx(12));
      layout->setSpacing(vtrpon::scalePx(12));
      window.resize(vtrpon::scaleSize(1240, 940));
      window.ensurePolished();
      layout->activate();
      application.processEvents();
      CHECK(edit->width() == vtrpon::scalePx(558));
      CHECK(program->width() == vtrpon::scalePx(616));
      CHECK(edit->minimumSizeHint().height() <= vtrpon::scalePx(940));
      CHECK(program->minimumSizeHint().height() <= vtrpon::scalePx(940));
      auto *remaining = program->findChild<QLabel *>(
          QStringLiteral("programRemainingTime"));
      CHECK(remaining);
      CHECK(std::abs(remaining->font().pointSizeF() - 22 * scale) < 0.02);
      CHECK(remaining->height() == vtrpon::scalePx(44));
      CHECK(remaining->fontMetrics().height() <= remaining->height());
      auto *pfl = edit->findChild<QPushButton *>(QStringLiteral("editPflButton"));
      CHECK(pfl);
      if (scale == 1.0) baselinePflWidth = pfl->width();
      // QSS minimum width includes separately rounded padding and borders.
      CHECK(std::abs(pfl->width() - baselinePflWidth * scale) <= 3);
      CHECK(pfl->fontMetrics().horizontalAdvance(pfl->text()) < pfl->width());
      const QPixmap rendered = window.grab();
      CHECK(!rendered.isNull());
      const QString captureDir = qEnvironmentVariable("VTRPON_DENSITY_CAPTURE_DIR");
      if (!captureDir.isEmpty()) {
        CHECK(QDir().mkpath(captureDir));
        CHECK(rendered.save(QDir(captureDir).filePath(
            QStringLiteral("%1-%2.png")
                .arg(QFileInfo(paths.last()).dir().dirName())
                .arg(qRound(scale * 100)))));
      }
    }
  }
  vtrpon::setUiScale(1.0);
  application.setStyleSheet(QString());
  application.setFont(baseFont);
  return 0;
}

int verifyWindowScaling(QApplication &application, const QStringList &stylePaths) {
  QString sheet;
  for (const QString &path : stylePaths) {
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    sheet += QString::fromUtf8(file.readAll()) + QLatin1Char('\n');
  }
  const QFont originalFont = application.font();
  for (const qreal density : {1.0, 2.0}) {
    vtrpon::setUiScale(density);
    QFont font = originalFont;
    font.setPointSizeF(font.pointSizeF() * density);
    application.setFont(font);
    application.setStyleSheet(vtrpon::scaledStyleSheet(sheet));
    QWidget window;
    // Exercise 4K-density geometry even when the test host has a smaller screen;
    // Windows otherwise clamps the requested top-level height to its work area.
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setObjectName(QStringLiteral("controllerWindow"));
    auto *layout = new QHBoxLayout(&window);
    layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                              vtrpon::scalePx(12), vtrpon::scalePx(12));
    layout->setSpacing(vtrpon::scalePx(12));
    UserInfoCenter info;
    auto *edit = new EditPanel(&info, &window);
    auto *program = new ProgramPanel(&window);
    layout->addWidget(edit);
    layout->addWidget(program);
    window.resize(vtrpon::scaleSize(1240, 1040));
    window.show();
    application.processEvents();
    const QSize normalSize = window.size();
    const QSize editSize = edit->size();
    const int programWidth = program->width();
    auto *remaining = program->findChild<QLabel *>(QStringLiteral("programRemainingTime"));
    auto *fader = program->findChild<QSlider *>(QStringLiteral("programItemFader"));
    auto *duration = edit->findChild<QDoubleSpinBox *>(QStringLiteral("editFadeInDurationSpinBox"));
    CHECK(remaining && fader && duration);
    const int remainingHeight = remaining->height();
    const qreal remainingFontSize = remaining->font().pointSizeF();
    const WId monitorId = program->monitorWindow()->winId();
    auto *editSurface = edit->findChild<QWidget *>(QStringLiteral("edit-video-surface"));
    CHECK(editSurface);
    const WId editSurfaceId = editSurface->winId();
    fader->setValue(-123);
    duration->parentWidget()->setEnabled(true);
    duration->setEnabled(true);
    duration->setValue(2.4);
    duration->setFocus();
    QWidget *focusedControl = window.focusWidget();
    CHECK(focusedControl == duration || duration->isAncestorOf(focusedControl));
    vtrpon::ControllerWindowScaling scaling(&window);
    for (int round = 0; round < 2; ++round) {
      for (const QSize &size : {QSize(qRound(normalSize.width() * 0.75), normalSize.height()),
                              QSize(normalSize.width(), qRound(normalSize.height() * 0.75)),
                              QSize(qRound(normalSize.width() * 0.87), qRound(normalSize.height() * 0.87)),
                              normalSize}) {
        window.resize(size);
        const qreal ratio = std::min(qreal(size.width()) / normalSize.width(),
                                     qreal(size.height()) / normalSize.height());
        const bool reached = waitUntil(application, [&] { return qAbs(vtrpon::uiScale() / density - ratio) < 0.003; }, 5000);
        if (!reached) std::fprintf(stderr,
            "resize target=%dx%d actual=%dx%d normal=%dx%d scale=%f density=%f ratio=%f minimum=%dx%d\n",
            size.width(), size.height(), window.width(), window.height(), normalSize.width(), normalSize.height(),
            vtrpon::uiScale(), density, ratio, window.minimumWidth(), window.minimumHeight());
        CHECK(reached);
        CHECK(qAbs(edit->width() - qRound(editSize.width() * ratio)) <= 1);
        CHECK(qAbs(program->width() - qRound(programWidth * ratio)) <= 1);
        CHECK(qAbs(remaining->height() - qRound(remainingHeight * ratio)) <= 1);
        CHECK(qAbs(remaining->font().pointSizeF() - remainingFontSize * ratio) < 0.1);
        CHECK(remaining->fontMetrics().height() <= remaining->height());
        CHECK(edit->geometry().right() < program->geometry().left());
        CHECK(window.rect().contains(edit->geometry()));
        CHECK(window.rect().contains(program->geometry()));
        CHECK(program->monitorWindow()->winId() == monitorId);
        CHECK(editSurface->winId() == editSurfaceId);
        QImage emptySurface(editSurface->size(), QImage::Format_RGB32);
        emptySurface.fill(Qt::magenta);
        editSurface->render(&emptySurface, QPoint(), QRegion(), QWidget::DrawChildren);
        for (const QPoint &point : {QPoint(2, 2), emptySurface.rect().center(),
                                  emptySurface.rect().bottomRight() - QPoint(2, 2)}) {
          CHECK(emptySurface.pixelColor(point) == QColor(Qt::black));
        }
        CHECK(fader->value() == -123);
        CHECK(qAbs(duration->value() - 2.4) < 0.01);
        CHECK(window.focusWidget() == focusedControl);
        const QString captureDir = qEnvironmentVariable("VTRPON_DENSITY_CAPTURE_DIR");
        if (round == 0 && !captureDir.isEmpty()) {
          CHECK(QDir().mkpath(captureDir));
          CHECK(window.grab().save(QDir(captureDir).filePath(
              QStringLiteral("resize-%1-%2-%3x%4.png")
                  .arg(QFileInfo(stylePaths.last()).dir().dirName())
                  .arg(qRound(density * 100)).arg(size.width()).arg(size.height()))));
        }
      }
    }
    CHECK(edit->width() == editSize.width());
    CHECK(program->width() == programWidth);
    CHECK(remaining->height() == remainingHeight);
    window.resize(100, 100);
    CHECK(window.width() >= qRound(normalSize.width() * 0.75));
    CHECK(window.height() >= qRound(normalSize.height() * 0.75));
    window.close();
  }
  vtrpon::setUiScale(1.0);
  application.setStyleSheet(QString());
  application.setFont(originalFont);
  return 0;
}

int verifySkin(QApplication &application, const QStringList &stylePaths,
               bool repeatControlsOnly = false) {
  QString combinedStyleSheet;
  for (const QString &stylePath : stylePaths) {
    QFile styleFile(stylePath);
    CHECK(styleFile.open(QIODevice::ReadOnly | QIODevice::Text));
    if (!combinedStyleSheet.isEmpty()) {
      combinedStyleSheet.append(QLatin1Char('\n'));
    }
    combinedStyleSheet.append(QString::fromUtf8(styleFile.readAll()));
  }
  application.setStyleSheet(combinedStyleSheet);
  const QString primaryStylePath = stylePaths.constFirst();
  const bool hardwareSkin =
      primaryStylePath.contains(QStringLiteral("skins/hardware"),
                                Qt::CaseInsensitive) ||
      primaryStylePath.contains(QStringLiteral("skins\\hardware"),
                                Qt::CaseInsensitive);
  application.setProperty("controllerSkinUsesHardwareEffects", hardwareSkin);

  UserInfoCenter userInfoCenter;
  EditPanel panel(&userInfoCenter);
  PlaylistItemSettings settings;
  settings.endMode = PlaylistEndMode::Next;
  settings.fadeInEnabled = true;
  settings.fadeOutEnabled = true;
  settings.endCrossfadeEnabled = true;
  PlaylistItemState firstTarget;
  firstTarget.id = 11;
  firstTarget.playlistNumber = 1;
  firstTarget.itemNumber = 1;
  firstTarget.displayName = QStringLiteral("Opening.mov");
  PlaylistItemState secondTarget;
  secondTarget.id = 22;
  secondTarget.playlistNumber = 2;
  secondTarget.itemNumber = 3;
  secondTarget.displayName = QStringLiteral("Interview.mov");
  const QList<PlaylistItemState> gotoTargets{firstTarget, secondTarget};
  panel.setEditorState(settings, true, true, gotoTargets);
  panel.show();
  application.processEvents();

  auto *pflButton = panel.findChild<QPushButton *>(QStringLiteral("editPflButton"));
  CHECK(pflButton != nullptr);
  panel.setPflState(false, true, QStringLiteral("PFL OFF"));
  application.processEvents();
  CHECK(pflButton->width() >= 44 && pflButton->height() >= 22);
  auto *nominalStrip = panel.findChild<QWidget *>(QStringLiteral("editNominalVolumeStrip"));
  CHECK(nominalStrip != nullptr);
  CHECK(nominalStrip->height() - pflButton->mapTo(nominalStrip, QPoint(0, pflButton->height())).y()
        >= vtrpon::scalePx(8));
  const bool clasicSkin = combinedStyleSheet.contains(QStringLiteral("VTR-PON Clasic Controller skin"));
  const auto offPixel = pixelAtLogicalPoint(pflButton->grab(), QPoint(5, 5));
  if (hardwareSkin) {
    const auto bottomPixel = pixelAtLogicalPoint(pflButton->grab(), QPoint(5, 17));
    // Keep a visible Hardware bevel while moderating the original highlight.
    CHECK(qGreen(offPixel) - qGreen(bottomPixel) >= 20);
    CHECK(qGreen(offPixel) - qGreen(bottomPixel) <= 45);
  } else {
    CHECK(rgbChannelsWithin(offPixel, clasicSkin ? qRgb(0xd9, 0xd9, 0xd9) : qRgb(0x24, 0x26, 0x2d), 3));
  }
  QList<bool> pflRequests;
  const auto pflConnection = QObject::connect(&panel, &EditPanel::pflRequested,
      &panel, [&](bool on) { pflRequests.append(on); });
  panel.setPflState(true, true, QStringLiteral("PFL ON"));
  CHECK(pflRequests.isEmpty()); // Backend feedback must not emit another command.
  application.processEvents();
  CHECK(pflButton->isChecked());
  const auto onPixel = pixelAtLogicalPoint(pflButton->grab(), QPoint(5, 5));
  if (hardwareSkin) {
    CHECK(qGreen(onPixel) > qRed(onPixel) + 30);
    const auto bottomPixel = pixelAtLogicalPoint(pflButton->grab(), QPoint(5, 17));
    CHECK(qGreen(onPixel) - qGreen(bottomPixel) >= 15);
    CHECK(qGreen(onPixel) - qGreen(bottomPixel) <= 40);
  } else {
    CHECK(rgbChannelsWithin(onPixel, clasicSkin ? qRgb(0x66, 0xcd, 0x66) : qRgb(0x13, 0x73, 0x33), 3));
  }
  pflButton->click();
  CHECK(pflRequests == QList<bool>{false});
  panel.setPflState(false, true, QStringLiteral("PFL OFF"));
  QObject::disconnect(pflConnection);

  auto *editSeekBar = panel.findChild<QWidget *>(
      QStringLiteral("editSeekBar"));
  auto *editMonitor = panel.findChild<QWidget *>(
      QStringLiteral("editMonitor"));
  auto *editFileLabel = panel.findChild<QLabel *>(
      QStringLiteral("editFileLabel"));
  auto *transportModule = panel.findChild<QWidget *>(
      QStringLiteral("editTransportModule"));
  CHECK(editSeekBar != nullptr);
  CHECK(editMonitor != nullptr);
  CHECK(editFileLabel != nullptr);
  CHECK(transportModule != nullptr);
  CHECK(transportModule->findChild<QLabel *>(
            QStringLiteral("editControlModuleTitle")) == nullptr);

  QList<int> frameStepRequests;
  QObject::connect(&panel, &EditPanel::frameStepRequested, &panel,
                   [&](int frameCount) {
                     frameStepRequests.append(frameCount);
                   });
  const auto sendMonitorWheel = [editMonitor](const QPoint &angleDelta) {
    const QPointF localPosition(editMonitor->rect().center());
    const QPointF globalPosition(
        editMonitor->mapToGlobal(editMonitor->rect().center()));
    QWheelEvent event(localPosition, globalPosition, QPoint(), angleDelta,
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(editMonitor, &event);
  };
  sendMonitorWheel(QPoint(0, -120));
  CHECK(frameStepRequests.isEmpty());
  panel.setTransportEnabled(true);
  sendMonitorWheel(QPoint(0, -60));
  CHECK(frameStepRequests.isEmpty());
  sendMonitorWheel(QPoint(0, -60));
  CHECK(frameStepRequests == QList<int>{1});
  sendMonitorWheel(QPoint(0, 120));
  CHECK(frameStepRequests == (QList<int>{1, -1}));
  sendMonitorWheel(QPoint(120, 0));
  CHECK(frameStepRequests == (QList<int>{1, -1}));
  panel.setFile(QStringLiteral("Opening.mov"),
                QStringLiteral("C:/media/Opening.mov"));
  application.processEvents();
  CHECK(editFileLabel->text() == QStringLiteral("Opening.mov"));
  CHECK(editFileLabel->toolTip() == QStringLiteral("C:/media/Opening.mov"));
  CHECK(editFileLabel->mapTo(&panel, QPoint()).y() >
        editSeekBar->mapTo(&panel, QPoint()).y());
  panel.clearFile();
  CHECK(editFileLabel->text() == QStringLiteral("No file selected"));

  for (const DurationControlIds &ids : kDurationControls) {
    auto *spinBox =
        panel.findChild<QDoubleSpinBox *>(QString::fromLatin1(ids.spinBox));
    auto *increaseButton = panel.findChild<QToolButton *>(
        QString::fromLatin1(ids.increaseButton));
    auto *decreaseButton = panel.findChild<QToolButton *>(
        QString::fromLatin1(ids.decreaseButton));
    CHECK(spinBox != nullptr);
    CHECK(increaseButton != nullptr);
    CHECK(decreaseButton != nullptr);
    CHECK(spinBox->isEnabled());
    CHECK(increaseButton->isEnabled());
    CHECK(decreaseButton->isEnabled());
    CHECK(spinBox->width() == vtrpon::scalePx(kCompactDurationWidth));
    CHECK(spinBox->buttonSymbols() == QAbstractSpinBox::NoButtons);
    CHECK(increaseButton->text().isEmpty());
    CHECK(decreaseButton->text().isEmpty());
    CHECK(increaseButton->arrowType() == Qt::UpArrow);
    CHECK(decreaseButton->arrowType() == Qt::DownArrow);
    CHECK(increaseButton->width() < spinBox->height());
    CHECK(decreaseButton->width() < spinBox->height());
    CHECK(increaseButton->height() < spinBox->height());
    CHECK(decreaseButton->height() < spinBox->height());
    CHECK(increaseButton->height() + decreaseButton->height()
          <= spinBox->height());
    CHECK(increaseButton->parentWidget() == decreaseButton->parentWidget());
    CHECK(increaseButton->y() < decreaseButton->y());
    CHECK(!increaseButton->toolTip().isEmpty());
    CHECK(!decreaseButton->toolTip().isEmpty());

    spinBox->setValue(1.0);
    QLineEdit *editor = spinBox->findChild<QLineEdit *>();
    CHECK(editor != nullptr);
    editor->setFocus();
    editor->selectAll();
    application.processEvents();
    CHECK(editor->hasFocus());

    increaseButton->setFocus();
    application.processEvents();
    increaseButton->click();
    application.processEvents();
    CHECK(std::abs(spinBox->value() - 1.1) < 0.0001);

    editor->setFocus();
    editor->selectAll();
    application.processEvents();
    decreaseButton->setFocus();
    application.processEvents();
    decreaseButton->click();
    application.processEvents();
    CHECK(std::abs(spinBox->value() - 1.0) < 0.0001);
  }

  auto *gotoButton = panel.findChild<QPushButton *>(
      QStringLiteral("editEndModeGotoButton"));
  auto *gotoCombo = panel.findChild<QComboBox *>(
      QStringLiteral("editGotoTargetComboBox"));
  auto *endCrossfade = panel.findChild<QCheckBox *>(
      QStringLiteral("editEndCrossfadeCheckBox"));
  auto *gotoLabel = panel.findChild<QLabel *>(
      QStringLiteral("editGotoTargetLabel"));
  auto *repeatOptionLabel = panel.findChild<QLabel *>(
      QStringLiteral("editRepeatOptionLabel"));
  auto *repeatCount = panel.findChild<QSpinBox *>(
      QStringLiteral("editRepeatCountSpinBox"));
  auto *repeatCountIncrease = panel.findChild<QToolButton *>(
      QStringLiteral("editRepeatCountIncreaseButton"));
  auto *repeatCountDecrease = panel.findChild<QToolButton *>(
      QStringLiteral("editRepeatCountDecreaseButton"));
  auto *repeatEndMode = panel.findChild<QComboBox *>(
      QStringLiteral("editRepeatEndModeComboBox"));
  CHECK(gotoButton != nullptr);
  CHECK(gotoCombo != nullptr);
  CHECK(endCrossfade != nullptr);
  CHECK(gotoLabel != nullptr);
  CHECK(repeatOptionLabel != nullptr);
  CHECK(repeatCount != nullptr);
  CHECK(repeatCountIncrease != nullptr);
  CHECK(repeatCountDecrease != nullptr);
  CHECK(repeatEndMode != nullptr);
  CHECK(gotoButton->isEnabled());
  CHECK(!gotoCombo->isEnabled());
  CHECK(!repeatCount->isEnabled());
  CHECK(!repeatCountIncrease->isEnabled());
  CHECK(!repeatCountDecrease->isEnabled());
  CHECK(!repeatEndMode->isEnabled());
  CHECK(gotoCombo->count() == 3);
  CHECK(gotoCombo->y() > endCrossfade->y());
  const int repeatCountTop = repeatCount->mapTo(&panel, QPoint()).y();
  const int repeatEndModeTop = repeatEndMode->mapTo(&panel, QPoint()).y();
  CHECK(repeatCountTop > gotoCombo->mapTo(&panel, QPoint()).y());
  CHECK(repeatEndModeTop == repeatCountTop);
  CHECK(repeatCount->minimum() == kInfiniteItemRepeatCount);
  CHECK(repeatCount->maximum() == kMaximumItemRepeatCount);
  CHECK(repeatCount->specialValueText() == QString(QChar(0x221e)));
  CHECK(repeatCount->width() ==
        vtrpon::scalePx(kCompactRepeatCountWidth));
  CHECK(repeatCount->buttonSymbols() == QAbstractSpinBox::NoButtons);
  CHECK(repeatCountIncrease->arrowType() == Qt::UpArrow);
  CHECK(repeatCountDecrease->arrowType() == Qt::DownArrow);
  CHECK(repeatCountIncrease->parentWidget() ==
        repeatCountDecrease->parentWidget());
  CHECK(repeatCountIncrease->y() < repeatCountDecrease->y());
  CHECK(!repeatCountIncrease->toolTip().isEmpty());
  CHECK(!repeatCountDecrease->toolTip().isEmpty());
  auto *fadeInIncrease = panel.findChild<QToolButton *>(
      QStringLiteral("editFadeInDurationIncreaseButton"));
  CHECK(fadeInIncrease != nullptr);
  CHECK(repeatCountIncrease->size() == fadeInIncrease->size());
  CHECK(repeatEndMode->count() == 4);

  quint64 requestedTargetId = 0;
  QObject::connect(&panel, &EditPanel::gotoTargetRequested, &panel,
                   [&](quint64 itemId) { requestedTargetId = itemId; });
  settings.endMode = PlaylistEndMode::Goto;
  settings.gotoTargetItemId = secondTarget.id;
  panel.setEditorState(settings, true, true, gotoTargets);
  application.processEvents();
  CHECK(gotoButton->isChecked());
  CHECK(gotoCombo->isEnabled());
  CHECK(endCrossfade->isEnabled());
  CHECK(gotoCombo->currentData().toULongLong() == secondTarget.id);
  panel.setEditorState(settings, true, true, gotoTargets, {firstTarget.id});
  CHECK(!(gotoCombo->model()->flags(gotoCombo->model()->index(1, 0)) & Qt::ItemIsEnabled));
  CHECK(!(gotoCombo->model()->flags(gotoCombo->model()->index(1, 0)) & Qt::ItemIsSelectable));
  CHECK(gotoCombo->itemText(1).contains(QStringLiteral("MEDIA OFFLINE")));
  gotoCombo->setCurrentIndex(1);
  CHECK(requestedTargetId == 0);
  CHECK(gotoCombo->currentData().toULongLong() == secondTarget.id);
  // A configured destination becoming offline is retained without emitting
  // a setting change. Recovery updates the open list in place.
  panel.setGotoTargetOffline(secondTarget.id, true);
  CHECK(gotoCombo->currentData().toULongLong() == secondTarget.id);
  CHECK(requestedTargetId == 0);
  CHECK(!(gotoCombo->model()->flags(gotoCombo->model()->index(2, 0)) & Qt::ItemIsEnabled));
  panel.setGotoTargetOffline(secondTarget.id, false);
  panel.setGotoTargetOffline(firstTarget.id, false);
  CHECK(gotoCombo->model()->flags(gotoCombo->model()->index(1, 0)) & Qt::ItemIsEnabled);
  CHECK(!gotoCombo->itemText(1).contains(QStringLiteral("MEDIA OFFLINE")));
  gotoCombo->setCurrentIndex(1);
  application.processEvents();
  CHECK(requestedTargetId == firstTarget.id);
  CHECK(!repeatCount->isEnabled());

  settings.endMode = PlaylistEndMode::Next;
  panel.setEditorState(settings, true, true, gotoTargets);
  application.processEvents();
  CHECK(!gotoCombo->isEnabled());
  CHECK(endCrossfade->isEnabled());

  int requestedRepeatCount = -1;
  PlaylistRepeatEndMode requestedRepeatEndMode = PlaylistRepeatEndMode::Pause;
  QObject::connect(
      &panel, &EditPanel::repeatOptionsRequested, &panel,
      [&](int count, PlaylistRepeatEndMode mode) {
        requestedRepeatCount = count;
        requestedRepeatEndMode = mode;
      });
  settings.endMode = PlaylistEndMode::Repeat;
  settings.repeatCount = 3;
  settings.repeatEndMode = PlaylistRepeatEndMode::Goto;
  settings.gotoTargetItemId = secondTarget.id;
  panel.setEditorState(settings, true, true, gotoTargets);
  application.processEvents();
  CHECK(repeatOptionLabel->isEnabled());
  CHECK(repeatCount->isEnabled());
  CHECK(repeatCountIncrease->isEnabled());
  CHECK(repeatCountDecrease->isEnabled());
  CHECK(repeatEndMode->isEnabled());
  CHECK(repeatCount->value() == 3);
  CHECK(static_cast<PlaylistRepeatEndMode>(
            repeatEndMode->currentData().toInt()) ==
        PlaylistRepeatEndMode::Goto);
  CHECK(gotoCombo->isEnabled());
  CHECK(endCrossfade->isEnabled());
  repeatCountIncrease->click();
  application.processEvents();
  CHECK(repeatCount->value() == 4);
  CHECK(requestedRepeatCount == 4);
  CHECK(requestedRepeatEndMode == PlaylistRepeatEndMode::Goto);
  repeatCountDecrease->click();
  application.processEvents();
  CHECK(repeatCount->value() == 3);
  repeatCount->setValue(4);
  application.processEvents();
  repeatEndMode->setCurrentIndex(
      repeatEndMode->findData(static_cast<int>(PlaylistRepeatEndMode::Next)));
  application.processEvents();
  CHECK(requestedRepeatCount == 4);
  CHECK(requestedRepeatEndMode == PlaylistRepeatEndMode::Next);

  settings.repeatCount = kInfiniteItemRepeatCount;
  settings.repeatEndMode = PlaylistRepeatEndMode::Pause;
  settings.gotoTargetItemId = 0;
  panel.setEditorState(settings, true, true, gotoTargets);
  application.processEvents();
  CHECK(repeatCount->text() == QString(QChar(0x221e)));
  CHECK(static_cast<PlaylistRepeatEndMode>(
            repeatEndMode->currentData().toInt()) ==
        PlaylistRepeatEndMode::Pause);
  CHECK(!gotoCombo->isEnabled());
  CHECK(endCrossfade->isEnabled());

  if (repeatControlsOnly) {
    return 0;
  }
  // The monitor light must follow loaded media in every skin, including unload.
  const auto verifyMonitorLight = [&](auto &target, const QString &monitorName) {
    target.show();
    application.processEvents();
    auto *monitor = target.template findChild<QWidget *>(monitorName);
    if (!monitor) return false;
    const QPoint probe = monitor->mapTo(&target, QPoint(-3, monitor->height() / 2));
    const auto unlit = pixelAtLogicalPoint(target.grab(), probe);
    target.setFile(QStringLiteral("neon-test.mp4"), QStringLiteral("G:/neon-test.mp4"));
    application.processEvents();
    const auto lit = pixelAtLogicalPoint(target.grab(), probe);
    target.clearFile();
    application.processEvents();
    return lit != unlit && pixelAtLogicalPoint(target.grab(), probe) == unlit;
  };
  {
    EditPanel editLight(nullptr);
    ProgramPanel programLight;
    CHECK(verifyMonitorLight(editLight, QStringLiteral("editMonitor")));
    CHECK(verifyMonitorLight(programLight, QStringLiteral("onAirMonitor")));
  }
  ProgramPanel programPanel;
  {
    ProgramPanel monitorSnapshot;
    monitorSnapshot.setPlaybackState(true, true);
    monitorSnapshot.setFile(QStringLiteral("monitor-test.mp4"), QStringLiteral("G:/monitor-test.mp4"));
    monitorSnapshot.setPlaybackRate(2.0);
    monitorSnapshot.setRemainingTime(10000);
    PlaylistItemSettings modes;
    modes.startMode = PlaylistStartMode::Play;
    modes.endMode = PlaylistEndMode::Next;
    monitorSnapshot.setEndMode(modes, 1);
    auto status = monitorSnapshot.operatorMonitorStatus();
    CHECK(status[0] == QStringLiteral("00:00:05.00"));
    CHECK(status[1] == QStringLiteral("monitor-test.mp4"));
    CHECK(status[2] == QStringLiteral("Play / Next"));
    CHECK(status[3] == QString::fromUtf8("2×"));
    CHECK(status[4].isEmpty());
    monitorSnapshot.setPowerPointNavigationState(true, true, true, 3, 12);
    CHECK(monitorSnapshot.operatorMonitorStatus()[4] == QStringLiteral("12 / 3"));
    monitorSnapshot.setPdfNavigationState(true, true, true, 4, 20);
    CHECK(monitorSnapshot.operatorMonitorStatus()[4] == QStringLiteral("20 / 4"));
    monitorSnapshot.setPdfNavigationState(false, false, false);
    CHECK(monitorSnapshot.operatorMonitorStatus()[4].isEmpty());
    monitorSnapshot.setPlaybackState(false, false);
    CHECK(monitorSnapshot.operatorMonitorStatus()[1] != QStringLiteral("monitor-test.mp4"));
  }
  PlaylistItemSettings programSettings;
  programSettings.endMode = PlaylistEndMode::Repeat;
  programSettings.repeatCount = 3;
  programSettings.repeatEndMode = PlaylistRepeatEndMode::Next;
  programSettings.fadeOutEnabled = true;
  programSettings.fadeOutDurationMs = 2345;
  programSettings.endCrossfadeEnabled = true;
  programSettings.endCrossfadeDurationMs = 3456;
  programPanel.setEndMode(programSettings, 2);
  programPanel.setMasterMeter(0.0, 0.0, false);
  programPanel.show();
  application.processEvents();
  auto *eqButton = programPanel.findChild<QPushButton *>(QStringLiteral("programMasterEqButton"));
  CHECK(eqButton && eqButton->isVisible() && !eqButton->isCheckable());
  auto *masterDelay = programPanel.findChild<QSpinBox *>(QStringLiteral("programMasterDelaySpin"));
  auto *itemDelay = programPanel.findChild<QSpinBox *>(QStringLiteral("programItemDelaySpin"));
  auto *nominalDelay = panel.findChild<QSpinBox *>(QStringLiteral("editNominalDelaySpin"));
  CHECK(masterDelay && itemDelay && nominalDelay);
  const auto delayStep = [](QSpinBox *delay, bool up) {
    return delay->parentWidget()->findChild<QToolButton *>(delay->objectName()
        + (up ? QStringLiteral("IncreaseButton") : QStringLiteral("DecreaseButton")));
  };
  auto *fadeDuration = programPanel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programMasterFadeInDurationSpinBox"));
  auto *fadeIncrease = programPanel.findChild<QToolButton *>(
      QStringLiteral("programMasterFadeInDurationIncreaseButton"));
  CHECK(fadeDuration && fadeIncrease);
  for (auto *delay : {masterDelay, itemDelay, nominalDelay}) {
    CHECK(delay->minimum() == 0 && delay->maximum() == 6);
    CHECK(delay->singleStep() == 1 && !delay->keyboardTracking());
    CHECK(delay->buttonSymbols() == QAbstractSpinBox::NoButtons);
    CHECK(delay->size() == fadeDuration->size());
    CHECK(delayStep(delay, true) && delayStep(delay, false));
    CHECK(delayStep(delay, true)->size() == fadeIncrease->size());
    CHECK(delayStep(delay, false)->size() == fadeIncrease->size());
    CHECK(delay->parentWidget()->size() == fadeDuration->parentWidget()->size());
  }
  CHECK(masterDelay->mapTo(&programPanel, QPoint()).x()
        >= eqButton->mapTo(&programPanel, QPoint(eqButton->width(), 0)).x());
  int masterRequested = -1, itemRequested = -1, nominalRequested = -1;
  const auto masterDelayConnection = QObject::connect(&programPanel, &ProgramPanel::masterDelayChanged,
      &programPanel, [&](int frames) { masterRequested = frames; });
  const auto itemDelayConnection = QObject::connect(&programPanel, &ProgramPanel::itemDelayChanged,
      &programPanel, [&](int frames) { itemRequested = frames; });
  const auto nominalDelayConnection = QObject::connect(&panel, &EditPanel::nominalDelayRequested,
      &panel, [&](int frames) { nominalRequested = frames; });
  const auto clickDelayStep = [&](bool up) {
    delayStep(masterDelay, up)->click();
  };
  masterDelay->setValue(3);
  clickDelayStep(true);
  CHECK(masterRequested == 4);
  clickDelayStep(false);
  CHECK(masterRequested == 3);
  masterDelay->setValue(6);
  clickDelayStep(true);
  CHECK(masterRequested == 6);
  programPanel.setItemDelayFrames(3);
  CHECK(itemDelay->value() == 3 && itemRequested == -1);
  itemDelay->setValue(5);
  CHECK(itemRequested == 5 && masterDelay->value() == 6);
  settings.nominalAudioDelayFrames = 4;
  panel.setEditorState(settings, true, true, gotoTargets);
  CHECK(nominalDelay->value() == 4 && nominalRequested == -1);
  nominalDelay->setValue(6);
  CHECK(nominalRequested == 6);
  panel.setEditorState(std::nullopt, false, false);
  CHECK(!nominalDelay->isEnabled());
  CHECK(!delayStep(nominalDelay, true)->isEnabled());
  CHECK(!delayStep(nominalDelay, false)->isEnabled());
  delayStep(nominalDelay, false)->click();
  CHECK(nominalRequested == 6);
  panel.setEditorState(settings, true, true, gotoTargets);
  programPanel.setPlaybackState(false, false);
  CHECK(!itemDelay->isEnabled() && masterDelay->isEnabled());
  CHECK(!delayStep(itemDelay, true)->isEnabled());
  CHECK(!delayStep(itemDelay, false)->isEnabled());
  programPanel.setPlaybackState(true, true);
  CHECK(itemDelay->isEnabled());
  CHECK(delayStep(itemDelay, true)->isEnabled());
  CHECK(delayStep(itemDelay, false)->isEnabled());
  delayStep(itemDelay, false)->click();
  CHECK(itemRequested == 4);
  delayStep(nominalDelay, true)->click();
  CHECK(nominalRequested == 5);
  masterDelay->setValue(0);
  programPanel.setItemDelayFrames(0);
  QObject::disconnect(masterDelayConnection);
  QObject::disconnect(itemDelayConnection);
  QObject::disconnect(nominalDelayConnection);
  CHECK(!eqButton->property("eqAdjusted").toBool());
  auto *masterGroup = programPanel.findChild<QWidget *>(QStringLiteral("programMasterFaderMeterGroup"));
  CHECK(masterGroup);
  CHECK(eqButton->mapTo(&programPanel, QPoint()).y()
        > masterGroup->mapTo(&programPanel, QPoint(0, masterGroup->height())).y());
  eqButton->click();
  application.processEvents();
  auto *eqWindow = programPanel.findChild<QDialog *>(QStringLiteral("programMasterEqWindow"));
  CHECK(eqWindow && eqWindow->isVisible() && !eqWindow->isModal());
  CHECK(!eqButton->property("eqAdjusted").toBool());
  std::array<QDial *, 3> eqDials{
      eqWindow->findChild<QDial *>(QStringLiteral("programMasterEqLOWDial")),
      eqWindow->findChild<QDial *>(QStringLiteral("programMasterEqMIDDial")),
      eqWindow->findChild<QDial *>(QStringLiteral("programMasterEqHIGHDial"))};
  for (auto *dial : eqDials) CHECK(dial && dial->value() == 0 && dial->minimum() == -30 && dial->maximum() == 30);
  CHECK(eqDials[0]->accessibleName().contains("100 Hz"));
  CHECK(eqDials[1]->accessibleName().contains("2.5 kHz"));
  CHECK(eqDials[2]->accessibleName().contains("10 kHz"));
  CHECK(eqDials[2]->y() < eqDials[1]->y() && eqDials[1]->y() < eqDials[0]->y());
  CHECK(eqDials[2]->x() == eqDials[1]->x() && eqDials[1]->x() == eqDials[0]->x());
  auto *rotationDial = eqDials[0];
  const auto dragEvent = [&](QEvent::Type type, const QPointF &point) {
    QMouseEvent event(type, point, rotationDial->mapToGlobal(point),
                     type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                     type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(rotationDial, &event);
  };
  const QPointF center = QRectF(rotationDial->rect()).center();
  const double stepPixels = vtrpon::scalePx(8);
  // Test the user's gesture: grab the center, then drag vertically.
  for (const QPointF start : {center, QPointF(8, 8)}) {
    rotationDial->setValue(0);
    dragEvent(QEvent::MouseButtonPress, start);
    CHECK(rotationDial->value() == 0);
    dragEvent(QEvent::MouseMove, start - QPointF(0, 2 * stepPixels));
    CHECK(rotationDial->value() == 2);
    dragEvent(QEvent::MouseMove, start + QPointF(0, 2 * stepPixels));
    CHECK(rotationDial->value() == -2);
    dragEvent(QEvent::MouseButtonRelease, start);
  }
  for (int direction : {-1, 1}) {
    const int limit = direction > 0 ? rotationDial->maximum() : rotationDial->minimum();
    rotationDial->setValue(limit);
    dragEvent(QEvent::MouseButtonPress, center);
    const QPointF beyond = center - QPointF(0, direction * 400);
    dragEvent(QEvent::MouseMove, beyond);
    CHECK(rotationDial->value() == limit);
    dragEvent(QEvent::MouseMove, beyond + QPointF(0, direction * stepPixels));
    CHECK(rotationDial->value() == limit - direction);
    dragEvent(QEvent::MouseButtonRelease, beyond);
  }
  rotationDial->setValue(0);
  std::array<double, 3> requestedEq{};
  const auto eqConnection = QObject::connect(&programPanel, &ProgramPanel::masterEqChanged,
      &programPanel, [&](double low, double mid, double high) { requestedEq = {low, mid, high}; });
  for (int band = 0; band < 3; ++band) {
    eqDials[band]->setValue(30);
    CHECK(requestedEq[band] == 15.0 && programPanel.masterEqGains()[band] == 15.0);
    eqDials[band]->setValue(-30);
    CHECK(requestedEq[band] == -15.0 && programPanel.masterEqGains()[band] == -15.0);
    eqDials[band]->setValue(6);
    CHECK(requestedEq[band] == 3.0 && programPanel.masterEqGains()[band] == 3.0);
    CHECK(eqButton->property("eqAdjusted").toBool());
    eqDials[band]->setValue(0);
    CHECK(!eqButton->property("eqAdjusted").toBool());
  }
  eqDials[1]->setValue(-6);
  application.processEvents();
  const QImage eqImage = eqButton->grab().toImage();
  // Sample the top-center background; the compact EQ width puts the label
  // at the former quarter-width sampling point.
  const QColor eqColor = eqImage.pixelColor(eqImage.width() / 2, eqImage.height() / 5);
  CHECK(eqColor.green() > eqColor.red() + 15 && eqColor.green() > eqColor.blue());
  const QString eqCapture = qEnvironmentVariable("VTRPON_EQ_UI_CAPTURE");
  if (!eqCapture.isEmpty()) {
    CHECK(QDir().mkpath(eqCapture));
    const QString skinName = QFileInfo(stylePaths.last()).dir().dirName();
    CHECK(eqWindow->grab().save(eqCapture + "/" + skinName + "-eq.png"));
    CHECK(programPanel.grab().save(eqCapture + "/" + skinName + "-program.png"));
  }
  eqWindow->close();
  CHECK(eqButton->property("eqAdjusted").toBool());
  eqButton->click();
  application.processEvents();
  CHECK(eqWindow->isVisible() && eqDials[1]->value() == -6);
  CHECK(programPanel.findChildren<QDialog *>(QStringLiteral("programMasterEqWindow")).size() == 1);
  auto *eqReset = eqWindow->findChild<QPushButton *>(QStringLiteral("programMasterEqResetButton"));
  CHECK(eqReset);
  eqReset->click();
  CHECK(!eqButton->property("eqAdjusted").toBool());
  for (double gain : requestedEq) CHECK(gain == 0.0);
  QObject::disconnect(eqConnection);
  eqWindow->close();
  auto *itemEqButton = programPanel.findChild<QPushButton *>(QStringLiteral("programItemEqButton"));
  CHECK(itemEqButton);
  programPanel.setPlaybackState(true, false);
  programPanel.setItemEqGains({-30, 15, 0});
  CHECK(itemEqButton->property("eqAdjusted").toBool());
  itemEqButton->click();
  auto *itemEqWindow = programPanel.findChild<QDialog *>(QStringLiteral("programItemEqWindow"));
  CHECK(itemEqWindow && itemEqWindow->isVisible());
  for (int band = 0; band < 3; ++band) {
    auto *dial = itemEqWindow->findChild<QDial *>(QStringLiteral("programItemEq%1Dial").arg(band));
    CHECK(dial && dial->minimum() == -30 && dial->maximum() == 30);
    CHECK(dial->accessibleName().contains(QString::fromLatin1(vtrpon::kEqFrequencyLabels[band])));
  }
  auto *itemLow = itemEqWindow->findChild<QDial *>(QStringLiteral("programItemEq0Dial"));
  CHECK(itemLow && itemLow->value() == -6);
  double liveLow = 0;
  const auto itemEqConnection = QObject::connect(&programPanel, &ProgramPanel::itemEqChanged,
      &programPanel, [&](double low, double, double) { liveLow = low; });
  itemLow->setValue(4);
  CHECK(liveLow == 2.0);
  programPanel.setItemEqGains({});
  CHECK(itemLow->value() == 0 && !itemEqButton->property("eqAdjusted").toBool());
  QObject::disconnect(itemEqConnection);
  itemEqWindow->close();
  auto *nominalEqButton = panel.findChild<QPushButton *>(QStringLiteral("editNominalEqButton"));
  CHECK(nominalEqButton);
  settings.nominalEqTenthsDb = {-45, 0, 25};
  panel.setEditorState(settings, true, true, gotoTargets);
  CHECK(nominalEqButton->isEnabled() && nominalEqButton->property("eqAdjusted").toBool());
  nominalEqButton->click();
  auto *nominalEqWindow = panel.findChild<QDialog *>(QStringLiteral("editNominalEqWindow"));
  CHECK(nominalEqWindow && nominalEqWindow->isVisible());
  for (int band = 0; band < 3; ++band) {
    auto *dial = nominalEqWindow->findChild<QDial *>(QStringLiteral("editNominalEq%1Dial").arg(band));
    CHECK(dial && dial->minimum() == -30 && dial->maximum() == 30);
    CHECK(dial->accessibleName().contains(QString::fromLatin1(vtrpon::kEqFrequencyLabels[band])));
  }
  auto *nominalLow = nominalEqWindow->findChild<QDial *>(QStringLiteral("editNominalEq0Dial"));
  CHECK(nominalLow && nominalLow->value() == -9);
  int requestedLow = 0;
  const auto nominalConnection = QObject::connect(&panel, &EditPanel::nominalEqRequested,
      &panel, [&](int low, int, int) { requestedLow = low; });
  nominalLow->setValue(6);
  CHECK(requestedLow == 30);
  settings.nominalEqTenthsDb = {};
  panel.setEditorState(settings, true, true, gotoTargets);
  CHECK(nominalLow->value() == 0 && !nominalEqButton->property("eqAdjusted").toBool());
  panel.setEditorState(settings, false, false, gotoTargets);
  CHECK(!nominalEqButton->isEnabled() && !nominalEqWindow->isEnabled());
  panel.setEditorState(settings, true, true, gotoTargets);
  QObject::disconnect(nominalConnection);
  if (!eqCapture.isEmpty()) {
    const QString skinName = QFileInfo(stylePaths.last()).dir().dirName();
    CHECK(nominalEqWindow->grab().save(eqCapture + "/" + skinName + "-nominal-eq.png"));
    CHECK(panel.grab().save(eqCapture + "/" + skinName + "-edit.png"));
    CHECK(programPanel.grab().save(eqCapture + "/" + skinName + "-program-item.png"));
  }
  nominalEqWindow->close();
  programPanel.setPlaybackState(false, false);
  auto *backgroundButton = programPanel.findChild<QToolButton *>(QStringLiteral("programBackgroundColorButton"));
  CHECK(backgroundButton && backgroundButton->menu());
  CHECK(backgroundButton->size() == vtrpon::scaleSize(42, 22));
  CHECK(backgroundButton->popupMode() == QToolButton::InstantPopup);
  auto *backgroundTitle = programPanel.findChild<QLabel *>(QStringLiteral("programSectionTitle"));
  CHECK(backgroundTitle && backgroundButton->geometry().center().y() >= backgroundTitle->y());
  CHECK(backgroundButton->geometry().center().y() <= backgroundTitle->geometry().bottom());
  auto *backgroundMenu = backgroundButton->menu();
  CHECK(backgroundMenu->actions().size() == 6);
  CHECK(backgroundMenu->actions().first()->text() == QStringLiteral("Background Color"));
  CHECK(!backgroundMenu->actions().first()->isEnabled());
  int backgroundRequests = 0;
  QColor requestedColor;
  QObject::connect(&programPanel, &ProgramPanel::playerBackgroundRequested, &programPanel,
      [&](const QColor &color) { ++backgroundRequests; requestedColor = color; });
  programPanel.setPlayerBackground(QColor("#0000ff"));
  CHECK(backgroundRequests == 0);
  CHECK(backgroundMenu->actions()[3]->isChecked());
  backgroundMenu->actions()[2]->trigger();
  CHECK(backgroundRequests == 1 && requestedColor == QColor("#00ff00"));
  CHECK(backgroundMenu->actions()[3]->isChecked()); // Await persisted/applied state.
  programPanel.setPlayerBackground(requestedColor);
  CHECK(backgroundMenu->actions()[2]->isChecked() && !backgroundMenu->actions()[3]->isChecked());
  CHECK(backgroundButton->icon().pixmap(vtrpon::scaleSize(22, 12)).toImage().pixelColor(5, 5) == QColor("#00ff00"));
  programPanel.setPlayerBackground(QColor("#123abc"));
  CHECK(backgroundButton->toolTip().contains(QStringLiteral("#123ABC")));
  for (int i = 0; i < 4; ++i) CHECK(!backgroundMenu->actions()[i + 1]->isChecked());
  const QString captureDirectory = qEnvironmentVariable("VTRPON_BACKGROUND_UI_CAPTURE");
  if (!captureDirectory.isEmpty()) {
    const QString skin = QFileInfo(stylePaths.last()).dir().dirName();
    CHECK(QDir().mkpath(captureDirectory));
    CHECK(programPanel.grab(QRect(0, 0, programPanel.width(), vtrpon::scalePx(48)))
        .save(captureDirectory + "/" + skin + "-header.png"));
    backgroundMenu->popup(backgroundButton->mapToGlobal(QPoint(0, backgroundButton->height())));
    application.processEvents();
    CHECK(backgroundMenu->grab().save(captureDirectory + "/" + skin + "-menu.png"));
    backgroundMenu->hide();
  }
  auto *programSeekBar = programPanel.findChild<QWidget *>(
      QStringLiteral("programSeekBar"));
  auto *remainingModule = programPanel.findChild<QWidget *>(
      QStringLiteral("programRemainingModule"));
  auto *remainingTime = programPanel.findChild<QLabel *>(
      QStringLiteral("programRemainingTime"));
  auto *totalModeButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programRemainingTotalModeButton"));
  auto *endModeModule = programPanel.findChild<QWidget *>(
      QStringLiteral("programEndModeModule"));
  auto *endModeValue = programPanel.findChild<QLabel *>(
      QStringLiteral("programEndModeValue"));
  auto *playbackRateModule = programPanel.findChild<QWidget *>(
      QStringLiteral("programPlaybackRateModule"));
  auto *bgmControlModule = programPanel.findChild<QWidget *>(
      QStringLiteral("programBgmControlModule"));
  auto *bgmSignalIndicator = programPanel.findChild<QLabel *>(
      QStringLiteral("programBgmSignalIndicator"));
  auto *bgmStopButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programBgmStopButton"));
  auto *bgmPlayButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programBgmPlayButton"));
  auto *bgmPauseButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programBgmPauseButton"));
  auto *bgmFaderButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programBgmFaderButton"));
  auto *bgmFader = dynamic_cast<AudioFaderWidget *>(
      programPanel.findChild<QWidget *>(QStringLiteral("programBgmFader")));
  auto *bgmMeter = dynamic_cast<AudioMeterWidget *>(
      programPanel.findChild<QWidget *>(QStringLiteral("programBgmAudioMeter")));
  auto *bgmFadeInControl = programPanel.findChild<QWidget *>(
      QStringLiteral("programBgmFadeInControl"));
  auto *bgmFadeOutControl = programPanel.findChild<QWidget *>(
      QStringLiteral("programBgmFadeOutControl"));
  auto *bgmFadeInButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programBgmFadeInButton"));
  auto *bgmFadeOutButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programBgmFadeOutButton"));
  auto *bgmFadeInDuration = programPanel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programBgmFadeInDurationSpinBox"));
  auto *bgmFadeBlinkTimer = programPanel.findChild<QTimer *>(
      QStringLiteral("programBgmFadeBlinkTimer"));
  const auto playbackRateButtons =
      programPanel.findChildren<QPushButton *>(
          QRegularExpression(QStringLiteral("programPlaybackRate\\d+Button")));
  auto *audioStrip = programPanel.findChild<QWidget *>(
      QStringLiteral("programMasterAudioStrip"));
  auto *effectsPanel = programPanel.findChild<QWidget *>(
      QStringLiteral("programEffectsActionPanel"));
  auto *actionPanel = programPanel.findChild<QWidget *>(
      QStringLiteral("programActionPanel"));
  auto *transportPanel = programPanel.findChild<QWidget *>(
      QStringLiteral("programTransportActionPanel"));
  auto *ftbPanel = programPanel.findChild<QWidget *>(
      QStringLiteral("programFtbActionPanel"));
  auto *ftbButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("ftbButton"));
  auto *dskButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("dskOnButton"));
  auto *dskPlayButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("dskPlayButton"));
  auto *dskPauseButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("dskPauseButton"));
  auto *offAirButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("offAirButton"));
  auto *pauseButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programPauseButton"));
  auto *playButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programPlayButton"));
  auto *powerPointPanel = programPanel.findChild<QWidget *>(
      QStringLiteral("powerPointNavigationPanel"));
  auto *powerPointPreviousButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("powerPointPreviousButton"));
  auto *powerPointNextButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("powerPointNextButton"));
  auto *powerPointPositionLabel = programPanel.findChild<QLabel *>(
      QStringLiteral("powerPointPositionLabel"));
  auto *itemFader = dynamic_cast<AudioFaderWidget *>(
      programPanel.findChild<QWidget *>(QStringLiteral("programItemFader")));
  auto *itemMeter = dynamic_cast<AudioMeterWidget *>(
      programPanel.findChild<QWidget *>(
          QStringLiteral("programItemAudioMeter")));
  auto *masterFader = dynamic_cast<AudioFaderWidget *>(
      programPanel.findChild<QWidget *>(QStringLiteral("programMasterFader")));
  auto *masterMeter = dynamic_cast<AudioMeterWidget *>(
      programPanel.findChild<QWidget *>(QStringLiteral("programAudioMeter")));
  auto *itemFaderMeterGroup = programPanel.findChild<QWidget *>(
      QStringLiteral("programItemFaderMeterGroup"));
  auto *masterFaderMeterGroup = programPanel.findChild<QWidget *>(
      QStringLiteral("programMasterFaderMeterGroup"));
  auto *itemMeterContainer = programPanel.findChild<QWidget *>(
      QStringLiteral("programItemMeterContainer"));
  auto *masterMeterContainer = programPanel.findChild<QWidget *>(
      QStringLiteral("programMasterMeterContainer"));
  auto *itemFadeInControl = programPanel.findChild<QWidget *>(
      QStringLiteral("programItemFadeInControl"));
  auto *itemFadeOutControl = programPanel.findChild<QWidget *>(
      QStringLiteral("programItemFadeOutControl"));
  auto *masterFadeInControl = programPanel.findChild<QWidget *>(
      QStringLiteral("programMasterFadeInControl"));
  auto *masterFadeOutControl = programPanel.findChild<QWidget *>(
      QStringLiteral("programMasterFadeOutControl"));
  CHECK(programSeekBar != nullptr);
  CHECK(remainingModule != nullptr);
  CHECK(remainingTime != nullptr);
  CHECK(totalModeButton != nullptr);
  CHECK(totalModeButton->isCheckable());
  CHECK(!totalModeButton->isChecked());
  CHECK(totalModeButton->text() == QStringLiteral("Total"));
  CHECK(!totalModeButton->accessibleName().isEmpty());
  CHECK(!totalModeButton->toolTip().isEmpty());
  CHECK(totalModeButton->height() == vtrpon::scalePx(19));
  CHECK(programPanel.findChild<QObject *>(
            QStringLiteral("programRemainingAlertTimer")) == nullptr);
  CHECK(endModeModule != nullptr);
  CHECK(endModeValue != nullptr);
  CHECK(playbackRateModule != nullptr);
  CHECK(playbackRateButtons.size() == 7);
  CHECK(playbackRateModule->isVisible());
  CHECK(bgmControlModule != nullptr);
  CHECK(programPanel.findChild<QLabel *>(
            QStringLiteral("programBgmStateLabel")) == nullptr);
  CHECK(bgmSignalIndicator != nullptr);
  CHECK(bgmStopButton != nullptr);
  CHECK(bgmPlayButton != nullptr);
  CHECK(bgmPauseButton != nullptr);
  CHECK(bgmFaderButton != nullptr);
  CHECK(bgmFaderButton->isCheckable());
  CHECK(bgmFader != nullptr);
  CHECK(bgmMeter != nullptr);
  CHECK(audioStrip != nullptr);
  CHECK(itemFader != nullptr);
  CHECK(itemMeter != nullptr);
  CHECK(itemFaderMeterGroup != nullptr);
  CHECK(itemFadeInControl != nullptr);
  CHECK(itemFadeOutControl != nullptr);
  CHECK(bgmFadeInControl != nullptr);
  CHECK(bgmFadeOutControl != nullptr);
  CHECK(bgmFadeInButton != nullptr);
  CHECK(bgmFadeOutButton != nullptr);
  CHECK(bgmFadeInDuration != nullptr);
  CHECK(bgmFadeBlinkTimer != nullptr);
  CHECK(bgmFadeBlinkTimer->interval() == 300);
  bgmFadeBlinkTimer->setInterval(40);
  CHECK(!programPanel.bgmControlsVisible());
  programPanel.setBgmControlsVisible(true);
  application.processEvents();
  CHECK(programPanel.bgmControlsVisible());
  CHECK(bgmControlModule->height() <= vtrpon::scalePx(90));
  CHECK(bgmFaderButton->isVisible());
  CHECK(!bgmFaderButton->isChecked());
  CHECK(bgmFaderButton->text() == QStringLiteral("Show BGM fader"));
  CHECK(itemFader->isVisible() && itemMeter->isVisible());
  CHECK(itemFadeInControl->isVisible() && itemFadeOutControl->isVisible());
  CHECK(bgmFader->isHidden() && bgmMeter->isHidden());
  CHECK(bgmFadeInControl->isHidden() && bgmFadeOutControl->isHidden());
  programPanel.setBgmPresentation(
      ProgramPanel::BgmPresentationState::Playing,
      QStringLiteral("Music"), QStringLiteral("Opening.wav"));
  application.processEvents();
  CHECK(bgmControlModule->accessibleDescription().contains(
      QStringLiteral("PLAYING")));
  CHECK(bgmPlayButton->property("active").toBool());
  CHECK(!bgmPauseButton->property("active").toBool());
  const auto bgmCenter = [bgmControlModule](QWidget *widget) {
    const QPoint topLeft = widget->mapTo(bgmControlModule, QPoint());
    return QPoint(topLeft.x() + widget->width() / 2,
                  topLeft.y() + widget->height() / 2);
  };
  const QPoint bgmStopCenter = bgmCenter(bgmStopButton);
  const QPoint bgmPlayCenter = bgmCenter(bgmPlayButton);
  const QPoint bgmPauseCenter = bgmCenter(bgmPauseButton);
  const QPoint bgmFaderButtonCenter = bgmCenter(bgmFaderButton);
  CHECK(bgmStopCenter.x() < bgmPlayCenter.x());
  CHECK(bgmPlayCenter.x() < bgmPauseCenter.x());
  CHECK(std::abs(bgmStopCenter.y() - bgmPlayCenter.y()) <= 1);
  CHECK(std::abs(bgmStopCenter.y() - bgmPauseCenter.y()) <= 1);
  CHECK(bgmFaderButton->parentWidget() == bgmControlModule);
  CHECK(std::abs(bgmFaderButtonCenter.y() - bgmPlayCenter.y()) <= 1);
  CHECK(bgmFaderButton->x() >= bgmPauseButton->x() + bgmPauseButton->width());
  CHECK(bgmControlModule->rect().contains(bgmFaderButton->geometry()));

  // BGM fades share the ITEM fader column and appear only in BGM view.
  bgmFaderButton->click();
  application.processEvents();
  CHECK(bgmFaderButton->isChecked());
  CHECK(bgmFaderButton->text() == QStringLiteral("Show ITEM fader"));
  CHECK(bgmFader->isVisible() && bgmMeter->isVisible());
  CHECK(bgmFadeInControl->isVisible() && bgmFadeOutControl->isVisible());
  CHECK(!itemFader->isVisible() && !itemMeter->isVisible());
  CHECK(itemFadeInControl->isHidden() && itemFadeOutControl->isHidden());
  CHECK(!itemDelay->isVisible() && !delayStep(itemDelay, true)->isVisible()
        && !delayStep(itemDelay, false)->isVisible());
  CHECK(bgmFadeInControl->parentWidget() == audioStrip);
  CHECK(bgmFadeOutControl->parentWidget() == audioStrip);
  const auto rectInAudioStrip = [audioStrip](QWidget *widget) {
    return QRect(widget->mapTo(audioStrip, QPoint()), widget->size());
  };
  const QRect bgmFaderGroupRect = rectInAudioStrip(itemFaderMeterGroup);
  const QRect bgmFadeInRect = rectInAudioStrip(bgmFadeInControl);
  const QRect bgmFadeOutRect = rectInAudioStrip(bgmFadeOutControl);
  CHECK(bgmFadeInRect.top() > bgmFaderGroupRect.bottom());
  CHECK(bgmFadeOutRect.top() > bgmFadeInRect.bottom());
  CHECK(std::abs(bgmFadeInRect.center().x() -
                 bgmFaderGroupRect.center().x()) <= vtrpon::scalePx(1));
  CHECK(std::abs(bgmFadeOutRect.center().x() -
                 bgmFadeInRect.center().x()) <= vtrpon::scalePx(1));
  const QPoint bgmButtonSurfaceProbe(vtrpon::scalePx(6),
                                     vtrpon::scalePx(6));
  const QRgb playingSurface = pixelAtLogicalPoint(
      bgmPlayButton->grab(), bgmButtonSurfaceProbe);
  const QRgb inactiveSurface = pixelAtLogicalPoint(
      bgmStopButton->grab(), bgmButtonSurfaceProbe);
  CHECK(!rgbChannelsWithin(playingSurface, inactiveSurface, 2));
  CHECK(qGreen(playingSurface) > qRed(playingSurface));
  CHECK(qGreen(playingSurface) > qBlue(playingSurface));
  programPanel.setBgmPresentation(
      ProgramPanel::BgmPresentationState::Paused,
      QStringLiteral("Music"), QStringLiteral("Opening.wav"));
  application.processEvents();
  CHECK(!bgmPlayButton->property("active").toBool());
  CHECK(bgmPauseButton->property("active").toBool());
  const QRgb pausedSurface = pixelAtLogicalPoint(
      bgmPauseButton->grab(), bgmButtonSurfaceProbe);
  CHECK(qGreen(pausedSurface) > qRed(pausedSurface));
  CHECK(qGreen(pausedSurface) > qBlue(pausedSurface));
  programPanel.setBgmFadePresentation(
      ProgramPanel::BgmFadeState::FadeIn);
  CHECK(!bgmPlayButton->property("active").toBool());
  CHECK(bgmPauseButton->property("active").toBool());
  CHECK(bgmFadeInButton->property("active").toBool());
  programPanel.setBgmFadePresentation(
      ProgramPanel::BgmFadeState::Idle);
  programPanel.setBgmPresentation(
      ProgramPanel::BgmPresentationState::Playing,
      QStringLiteral("Music"), QStringLiteral("Opening.wav"));
  programPanel.setBgmFadePresentation(
      ProgramPanel::BgmFadeState::FadeIn);
  CHECK(bgmPlayButton->property("active").toBool());
  CHECK(!bgmPauseButton->property("active").toBool());
  CHECK(bgmFadeBlinkTimer->isActive());
  CHECK(bgmFadeInButton->property("active").toBool());
  CHECK(!bgmFadeOutButton->property("active").toBool());
  application.processEvents();
  const QRgb fadeInSurface = pixelAtLogicalPoint(
      bgmFadeInButton->grab(), bgmButtonSurfaceProbe);
  CHECK(qGreen(fadeInSurface) > qRed(fadeInSurface));
  CHECK(qGreen(fadeInSurface) > qBlue(fadeInSurface));
  CHECK(waitUntil(application, [bgmFadeInButton] {
    return !bgmFadeInButton->property("active").toBool();
  }, 200));
  CHECK(waitUntil(application, [bgmFadeInButton] {
    return bgmFadeInButton->property("active").toBool();
  }, 200));
  programPanel.setBgmFadePresentation(
      ProgramPanel::BgmFadeState::FadeOut);
  CHECK(bgmPlayButton->property("active").toBool());
  CHECK(!bgmPauseButton->property("active").toBool());
  CHECK(!bgmFadeInButton->property("active").toBool());
  CHECK(bgmFadeOutButton->property("active").toBool());
  application.processEvents();
  const QRgb fadeOutSurface = pixelAtLogicalPoint(
      bgmFadeOutButton->grab(), bgmButtonSurfaceProbe);
  CHECK(qGreen(fadeOutSurface) > qRed(fadeOutSurface));
  CHECK(qGreen(fadeOutSurface) > qBlue(fadeOutSurface));
  CHECK(waitUntil(application, [bgmFadeOutButton] {
    return !bgmFadeOutButton->property("active").toBool();
  }, 200));
  CHECK(waitUntil(application, [bgmFadeOutButton] {
    return bgmFadeOutButton->property("active").toBool();
  }, 200));
  programPanel.setBgmFadePresentation(
      ProgramPanel::BgmFadeState::Idle, true);
  application.processEvents();
  CHECK(!bgmFadeBlinkTimer->isActive());
  CHECK(!bgmFadeInButton->property("active").toBool());
  CHECK(bgmFadeOutButton->property("active").toBool());
  CHECK(bgmPlayButton->property("active").toBool());
  CHECK(!bgmPauseButton->property("active").toBool());
  CHECK(bgmControlModule->accessibleDescription().contains(
      QStringLiteral("PLAYING")));
  CHECK(bgmControlModule->accessibleDescription().contains(
      QStringLiteral("VOLUME 0")));
  processFor(application, 120);
  CHECK(bgmFadeOutButton->property("active").toBool());
  programPanel.setBgmPresentation(
      ProgramPanel::BgmPresentationState::Stopped,
      QStringLiteral("Music"), QStringLiteral("Opening.wav"));
  application.processEvents();
  CHECK(!bgmPlayButton->property("active").toBool());
  CHECK(!bgmPauseButton->property("active").toBool());
  CHECK(!bgmFadeInButton->property("active").toBool());
  CHECK(!bgmFadeOutButton->property("active").toBool());
  if (hardwareSkin) {
    const QPoint programButtonSurfaceProbe(vtrpon::scalePx(6),
                                           vtrpon::scalePx(6));
    for (QPushButton *button :
         {bgmStopButton, bgmPlayButton, bgmPauseButton}) {
      // Fader fade buttons are shorter than transport buttons; compare the
      // same relative height in their shared vertical gradient.
      CHECK(rgbChannelsWithin(
          pixelAtLogicalPoint(button->grab(), programButtonSurfaceProbe),
          pixelAtLogicalPoint(bgmFadeInButton->grab(),
                              QPoint(programButtonSurfaceProbe.x(),
                                     qRound(qreal(programButtonSurfaceProbe.y())
                                            * bgmFadeInButton->height()
                                            / button->height()))),
          2));
    }
  }
  programPanel.setBgmSignalAppearance(true, false);
  CHECK(bgmSignalIndicator->property("active").toBool());
  CHECK(!bgmSignalIndicator->property("clipped").toBool());
  qint64 requestedBgmFadeInMs = 0;
  programPanel.operations()->bind<>(
      ControllerOperations::Action::BgmFadeInConfigured, &programPanel, [&] {
        requestedBgmFadeInMs = programPanel.configuredBgmFadeInDurationMs();
      });
  bgmFadeInDuration->setValue(1.5);
  bgmFadeInButton->click();
  CHECK(requestedBgmFadeInMs == 1500);
  bgmFaderButton->click();
  application.processEvents();
  CHECK(!bgmFaderButton->isChecked());
  CHECK(bgmFaderButton->text() == QStringLiteral("Show BGM fader"));
  CHECK(itemFader->isVisible() && itemMeter->isVisible());
  CHECK(itemFadeInControl->isVisible() && itemFadeOutControl->isVisible());
  CHECK(itemDelay->isVisible() && delayStep(itemDelay, true)->isVisible()
        && delayStep(itemDelay, false)->isVisible());
  CHECK(bgmFader->isHidden() && bgmMeter->isHidden());
  CHECK(bgmFadeInControl->isHidden() && bgmFadeOutControl->isHidden());
  bgmFaderButton->click();
  CHECK(bgmFaderButton->isChecked());
  programPanel.setBgmControlsVisible(false);
  application.processEvents();
  CHECK(!programPanel.bgmControlsVisible());
  CHECK(bgmFaderButton->isHidden() && !bgmFaderButton->isChecked());
  CHECK(itemFader->isVisible() && itemMeter->isVisible());
  CHECK(itemFadeInControl->isVisible() && itemFadeOutControl->isVisible());
  CHECK(bgmFader->isHidden() && bgmMeter->isHidden());
  CHECK(bgmFadeInControl->isHidden() && bgmFadeOutControl->isHidden());
  programPanel.setPlaybackRateControlsVisible(false);
  application.processEvents();
  CHECK(playbackRateModule->isHidden());
  programPanel.setPlaybackRateControlsVisible(true);
  application.processEvents();
  CHECK(playbackRateModule->isVisible());
  CHECK(audioStrip != nullptr);
  CHECK(effectsPanel != nullptr);
  CHECK(actionPanel != nullptr);
  CHECK(transportPanel != nullptr);
  CHECK(ftbPanel != nullptr);
  CHECK(ftbButton != nullptr);
  CHECK(dskButton != nullptr);
  CHECK(dskPlayButton != nullptr);
  CHECK(dskPauseButton != nullptr);
  CHECK(dskPlayButton->isHidden());
  CHECK(dskPauseButton->isHidden());
  CHECK(!dskPlayButton->toolTip().isEmpty());
  CHECK(!dskPauseButton->toolTip().isEmpty());
  CHECK(!dskPlayButton->accessibleName().isEmpty());
  CHECK(!dskPauseButton->accessibleName().isEmpty());
  CHECK(offAirButton != nullptr);
  CHECK(pauseButton != nullptr);
  CHECK(playButton != nullptr);
  const int transportWidthWithFtb = transportPanel->width();
  programPanel.setFtbControlsVisible(false);
  application.processEvents();
  CHECK(ftbPanel->isHidden());
  CHECK(transportPanel->width() > transportWidthWithFtb);
  CHECK(transportPanel->width() == actionPanel->contentsRect().width());
  programPanel.setFtbControlsVisible(true);
  application.processEvents();
  CHECK(ftbPanel->isVisible());
  CHECK(transportPanel->width() == transportWidthWithFtb);
  CHECK(powerPointPanel != nullptr);
  CHECK(powerPointPreviousButton != nullptr);
  CHECK(powerPointNextButton != nullptr);
  CHECK(powerPointPositionLabel != nullptr);
  CHECK(!powerPointPanel->isVisible());
  CHECK(itemFader != nullptr);
  CHECK(itemMeter != nullptr);
  CHECK(masterFader != nullptr);
  CHECK(masterMeter != nullptr);
  CHECK(itemFaderMeterGroup != nullptr);
  CHECK(masterFaderMeterGroup != nullptr);
  CHECK(itemMeterContainer != nullptr);
  CHECK(masterMeterContainer != nullptr);
  CHECK(audioStrip->width() == vtrpon::scalePx(kProgramAudioStripWidth));
  CHECK(itemFadeInControl != nullptr);
  CHECK(itemFadeOutControl != nullptr);
  CHECK(masterFadeInControl != nullptr);
  CHECK(masterFadeOutControl != nullptr);
  int audioMeterCount = 0;
  for (QWidget *child : programPanel.findChildren<QWidget *>()) {
    if (dynamic_cast<AudioMeterWidget *>(child)) {
      ++audioMeterCount;
    }
  }
  CHECK(audioMeterCount == 3);
  CHECK(programPanel.findChildren<QWidget *>(
            QStringLiteral("programItemAudioMeter")).size() == 1);
  CHECK(programPanel.findChildren<QWidget *>(
            QStringLiteral("programBgmAudioMeter")).size() == 1);
  CHECK(programPanel.findChildren<QWidget *>(
            QStringLiteral("programAudioMeter")).size() == 1);
  CHECK(itemMeter->parentWidget() == itemMeterContainer);
  CHECK(masterMeter->parentWidget() == masterMeterContainer);
  CHECK(dynamic_cast<QStackedLayout *>(audioStrip->layout()) == nullptr);
  CHECK(audioStrip->findChildren<QStackedLayout *>().size() == 2);
  CHECK(remainingTime->font().pointSizeF() >= 21.5);
  CHECK(remainingTime->height() == vtrpon::scalePx(44));
  CHECK(remainingTime->fontMetrics().height() <= remainingTime->height());
  CHECK(playbackRateModule->accessibleName() ==
        QStringLiteral("ON AIR playback speed panel"));
  constexpr auto expectedPlaybackRates = std::to_array<const char *>(
      {"0.5", "0.75", "1", "1.25", "1.5", "2", "3"});
  auto *referenceFadeButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programItemFadeInButton"));
  CHECK(referenceFadeButton != nullptr);
  for (int index = 0; index < playbackRateButtons.size(); ++index) {
    QPushButton *button = playbackRateButtons[index];
    CHECK(button->text() ==
          QString::fromLatin1(expectedPlaybackRates[index]));
    CHECK(button->isCheckable());
    CHECK(button->autoExclusive());
    CHECK(!button->isEnabled());
    CHECK(!button->accessibleName().isEmpty());
    CHECK(button->size() ==
          vtrpon::scaleSize(kPlaybackRateButtonWidth,
                            kPlaybackRateButtonHeight));
    CHECK(button->height() == referenceFadeButton->height());
    CHECK(std::abs(button->width() - referenceFadeButton->width()) <=
          vtrpon::scalePx(4));
    CHECK(button->width() >= button->minimumSizeHint().width());
    if (index > 0) {
      QPushButton *previousButton = playbackRateButtons[index - 1];
      CHECK(button->x() - previousButton->x() - previousButton->width() ==
            vtrpon::scalePx(kPlaybackRateButtonSpacing));
    }
  }
  const int playbackRateLeftInset = playbackRateButtons.front()->x();
  const int playbackRateRightInset =
      playbackRateModule->width() - playbackRateButtons.back()->x()
      - playbackRateButtons.back()->width();
  CHECK(std::abs(playbackRateLeftInset - playbackRateRightInset) <=
        vtrpon::scalePx(1));
  auto *normalRateButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programPlaybackRate100Button"));
  auto *fastRateButton = programPanel.findChild<QPushButton *>(
      QStringLiteral("programPlaybackRate125Button"));
  CHECK(normalRateButton != nullptr);
  CHECK(fastRateButton != nullptr);
  CHECK(normalRateButton->isChecked());
  programPanel.setPlaybackRateControlsEnabled(true);
  CHECK(fastRateButton->isEnabled());
  double requestedPlaybackRate = 0.0;
  QObject::connect(&programPanel, &ProgramPanel::playbackRateRequested,
                   &programPanel,
                   [&](double rate) { requestedPlaybackRate = rate; });
  normalRateButton->click();
  CHECK(requestedPlaybackRate == 0.0);
  fastRateButton->click();
  CHECK(std::abs(requestedPlaybackRate - 1.25) < 0.0001);
  CHECK(fastRateButton->isChecked());
  programPanel.setPlaybackRate(0.5);
  CHECK(programPanel.findChild<QPushButton *>(
            QStringLiteral("programPlaybackRate050Button"))->isChecked());
  programPanel.setRemainingTime(10000);
  CHECK(remainingTime->text() == QStringLiteral("00:00:20.00"));
  programPanel.setTotalRemainingTime(30000);
  CHECK(remainingTime->text() == QStringLiteral("00:00:20.00"));
  totalModeButton->click();
  CHECK(programPanel.totalModeEnabled());
  CHECK(remainingTime->text() == QStringLiteral("00:00:30.00"));
  programPanel.setPlaybackRate(2.0);
  CHECK(remainingTime->text() == QStringLiteral("00:00:30.00"));
  programPanel.setTotalRemainingInfinite();
  CHECK(remainingTime->text() == QString(QChar(0x221e)));
  CHECK(!remainingTime->property("endingSoon").toBool());
  totalModeButton->click();
  CHECK(!programPanel.totalModeEnabled());
  CHECK(remainingTime->text() == QStringLiteral("00:00:05.00"));
  programPanel.clearTotalRemainingTime();
  programPanel.setPlaybackRate(1.0);
  CHECK(remainingTime->text() == QStringLiteral("00:00:10.00"));

  programPanel.setRemainingTime(5001);
  application.processEvents();
  CHECK(!remainingTime->property("endingSoon").toBool());
  programPanel.setRemainingTime(5000);
  application.processEvents();
  CHECK(remainingTime->property("endingSoon").toBool());
  const QColor alertColor(
      hardwareSkin ? QStringLiteral("#ff6259") : QStringLiteral("#ff4a40"));
  CHECK(remainingTime->palette().color(QPalette::WindowText) ==
        alertColor);
  processFor(application, 450);
  CHECK(remainingTime->palette().color(QPalette::WindowText) ==
        alertColor);
  programPanel.setRemainingLive();
  application.processEvents();
  CHECK(!remainingTime->property("endingSoon").toBool());
  programPanel.setRemainingTime(1);
  application.processEvents();
  CHECK(remainingTime->property("endingSoon").toBool());
  CHECK(remainingTime->palette().color(QPalette::WindowText) == alertColor);
  programPanel.setRemainingTime(0);
  application.processEvents();
  CHECK(!remainingTime->property("endingSoon").toBool());

  const QRect itemFaderRect = rectInAudioStrip(itemFader);
  const QRect itemMeterRect = rectInAudioStrip(itemMeter);
  const QRect masterFaderRect = rectInAudioStrip(masterFader);
  const QRect masterMeterRect = rectInAudioStrip(masterMeter);
  CHECK(itemFaderRect.center().x() < itemMeterRect.center().x());
  CHECK(itemMeterRect.center().x() < masterFaderRect.center().x());
  CHECK(masterFaderRect.center().x() < masterMeterRect.center().x());
  CHECK(itemMeterRect.left() > itemFaderRect.right());
  CHECK(masterMeterRect.left() > masterFaderRect.right());
  const int extendedTopProbeY =
      vtrpon::scalePx(kExtendedAudioTopInset + 1);
  const QPoint faderTrackProbe(vtrpon::scalePx(22), extendedTopProbeY);
  const QPoint faderBackgroundProbe(0, extendedTopProbeY);
  CHECK(pixelAtLogicalPoint(itemFader->grab(), faderTrackProbe) !=
        pixelAtLogicalPoint(itemFader->grab(), faderBackgroundProbe));
  CHECK(pixelAtLogicalPoint(masterFader->grab(), faderTrackProbe) !=
        pixelAtLogicalPoint(masterFader->grab(), faderBackgroundProbe));
  const QPoint meterBarProbe(vtrpon::scalePx(5), extendedTopProbeY);
  const QRgb meterHigh = qRgb(0xf0, 0x19, 0x24);
  CHECK(pixelAtLogicalPoint(masterMeter->grab(), meterBarProbe) == meterHigh);
  CHECK(itemFadeInControl->mapTo(audioStrip, QPoint()).y() >
        itemFaderMeterGroup->mapTo(audioStrip, QPoint()).y());
  CHECK(itemFadeOutControl->mapTo(audioStrip, QPoint()).y() >
        itemFadeInControl->mapTo(audioStrip, QPoint()).y());
  CHECK(masterFadeInControl->mapTo(audioStrip, QPoint()).y() >
        masterFaderMeterGroup->mapTo(audioStrip, QPoint()).y());
  CHECK(masterFadeOutControl->mapTo(audioStrip, QPoint()).y() >
        masterFadeInControl->mapTo(audioStrip, QPoint()).y());
  const int itemFadeOutBottom =
      itemFadeOutControl->mapTo(audioStrip, QPoint()).y()
      + itemFadeOutControl->height();
  const int masterFadeOutBottom =
      masterFadeOutControl->mapTo(audioStrip, QPoint()).y()
      + masterFadeOutControl->height();
  const int eqTop = eqButton->mapTo(audioStrip, QPoint()).y();
  CHECK(itemFadeOutBottom == masterFadeOutBottom);
  CHECK(eqTop >= masterFadeOutBottom);
  CHECK(eqTop - masterFadeOutBottom <= vtrpon::scalePx(8));
  CHECK(audioStrip->height() - eqTop - eqButton->height() <= vtrpon::scalePx(8));

  auto *editDurationSpinBox = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("editFadeInDurationSpinBox"));
  auto *editDurationIncrease = panel.findChild<QToolButton *>(
      QStringLiteral("editFadeInDurationIncreaseButton"));
  auto *editDurationDecrease = panel.findChild<QToolButton *>(
      QStringLiteral("editFadeInDurationDecreaseButton"));
  CHECK(editDurationSpinBox != nullptr);
  CHECK(editDurationIncrease != nullptr);
  CHECK(editDurationDecrease != nullptr);
  for (const ProgramFadeControlIds &ids : kProgramFadeControls) {
    auto *button = programPanel.findChild<QPushButton *>(
        QString::fromLatin1(ids.button));
    auto *spinBox = programPanel.findChild<QDoubleSpinBox *>(
        QString::fromLatin1(ids.spinBox));
    auto *increaseButton = programPanel.findChild<QToolButton *>(
        QString::fromLatin1(ids.increaseButton));
    auto *decreaseButton = programPanel.findChild<QToolButton *>(
        QString::fromLatin1(ids.decreaseButton));
    CHECK(button != nullptr);
    CHECK(spinBox != nullptr);
    CHECK(increaseButton != nullptr);
    CHECK(decreaseButton != nullptr);
    CHECK(button->text() == QString::fromLatin1(ids.buttonText));
    CHECK(button->width() >= vtrpon::scalePx(34));
    if (!hardwareSkin) {
      CHECK(button->width() >= button->minimumSizeHint().width());
    }
    CHECK(editDurationSpinBox->height() - button->height() == vtrpon::scalePx(4));
    CHECK(std::abs(spinBox->value() - 3.0) < 0.0001);
    CHECK(std::abs(spinBox->minimum() - 0.1) < 0.0001);
    CHECK(std::abs(spinBox->maximum() - 60.0) < 0.0001);
    CHECK(std::abs(spinBox->singleStep() - 0.1) < 0.0001);
    CHECK(spinBox->decimals() == 1);
    CHECK(spinBox->suffix() == QStringLiteral(" s"));
    CHECK(spinBox->buttonSymbols() == QAbstractSpinBox::NoButtons);
    CHECK(spinBox->width() == vtrpon::scalePx(kCompactDurationWidth));
    CHECK(spinBox->size() == editDurationSpinBox->size());
    CHECK(increaseButton->size() == editDurationIncrease->size());
    CHECK(decreaseButton->size() == editDurationDecrease->size());
    CHECK(increaseButton->arrowType() == Qt::UpArrow);
    CHECK(decreaseButton->arrowType() == Qt::DownArrow);
    CHECK(increaseButton->autoRepeat());
    CHECK(decreaseButton->autoRepeat());
    CHECK(increaseButton->parentWidget() == decreaseButton->parentWidget());
    CHECK(increaseButton->y() < decreaseButton->y());
    CHECK(!button->accessibleName().isEmpty());
    CHECK(!spinBox->accessibleName().isEmpty());
    CHECK(!increaseButton->toolTip().isEmpty());
    CHECK(!decreaseButton->toolTip().isEmpty());
    increaseButton->click();
    application.processEvents();
    CHECK(std::abs(spinBox->value() - 3.1) < 0.0001);
    decreaseButton->click();
    application.processEvents();
    CHECK(std::abs(spinBox->value() - 3.0) < 0.0001);
  }

  CHECK(meterGreenPixelsStayInside(audioStrip->grab(), masterMeterRect));
  programPanel.resetAudioPresentation();
  programPanel.setItemMeter(0.0, 0.0, false);
  application.processEvents();
  CHECK(pixelAtLogicalPoint(itemMeter->grab(), meterBarProbe) == meterHigh);
  CHECK(meterGreenPixelsStayInside(audioStrip->grab(), itemMeterRect));
  programPanel.resetAudioPresentation();
  CHECK(effectsPanel->mapTo(&programPanel, QPoint()).y() <
        transportPanel->mapTo(&programPanel, QPoint()).y());
  CHECK(dskButton->parentWidget() == effectsPanel);
  CHECK(dskPlayButton->parentWidget() == effectsPanel);
  CHECK(dskPauseButton->parentWidget() == effectsPanel);
  CHECK(ftbButton->parentWidget() == ftbPanel);
  CHECK(offAirButton->parentWidget() == transportPanel);
  CHECK(pauseButton->parentWidget() == transportPanel);
  CHECK(playButton->parentWidget() == transportPanel);
  CHECK(effectsPanel->findChildren<QPushButton *>(
            QString(), Qt::FindDirectChildrenOnly).size() == 3);
  const int dskCenter = dskButton->x() + dskButton->width() / 2;
  CHECK(std::abs(dskCenter - effectsPanel->width() / 2) <= 1);
  int dskPlayRequests = 0;
  int dskPauseRequests = 0;
  QObject::connect(&programPanel, &ProgramPanel::dskPlayRequested,
                   &programPanel, [&] { ++dskPlayRequests; });
  QObject::connect(&programPanel, &ProgramPanel::dskPauseRequested,
                   &programPanel, [&] { ++dskPauseRequests; });
  programPanel.setDskOn(true);
  programPanel.setDskPlaybackState(true);
  application.processEvents();
  CHECK(dskPlayButton->isVisible());
  CHECK(dskPauseButton->isVisible());
  CHECK(dskPlayButton->size() == dskButton->size());
  CHECK(dskPauseButton->size() == dskButton->size());
  CHECK(dskButton->x() < dskPlayButton->x());
  CHECK(dskPlayButton->x() < dskPauseButton->x());
  CHECK(dskPlayButton->property("active").toBool());
  CHECK(!dskPauseButton->property("active").toBool());
  dskPlayButton->click();
  dskPauseButton->click();
  CHECK(dskPlayRequests == 1);
  CHECK(dskPauseRequests == 1);
  programPanel.setDskPlaybackState(false);
  CHECK(!dskPlayButton->property("active").toBool());
  CHECK(dskPauseButton->property("active").toBool());
  programPanel.setDskPlaybackState(false, false);
  CHECK(dskPlayButton->isVisible() && !dskPlayButton->isEnabled());
  CHECK(dskPauseButton->isVisible() && !dskPauseButton->isEnabled());
  programPanel.setDskPlaybackState(false, true);
  CHECK(dskPlayButton->isEnabled() && dskPauseButton->isEnabled());
  programPanel.setDskOn(false);
  application.processEvents();
  CHECK(dskPlayButton->isHidden());
  CHECK(dskPauseButton->isHidden());
  const int restoredDskCenter = dskButton->x() + dskButton->width() / 2;
  CHECK(std::abs(restoredDskCenter - effectsPanel->width() / 2) <= 1);
  CHECK(offAirButton->x() < playButton->x());
  CHECK(playButton->x() < pauseButton->x());
  const int playCenter = playButton->x() + playButton->width() / 2;
  CHECK(std::abs(playCenter - transportPanel->width() / 2) <= 1);
  CHECK(offAirButton->x() <= 20);
  CHECK(pauseButton->x() + pauseButton->width() >=
        transportPanel->width() - 20);
  const QMargins transportMargins =
      transportPanel->layout()->contentsMargins();
  CHECK(transportMargins.top() == vtrpon::scalePx(12));
  CHECK(transportMargins.bottom() == vtrpon::scalePx(12));
  CHECK(transportPanel->height() == offAirButton->height() +
        transportMargins.top() + transportMargins.bottom());
  const QPoint transportTopLeft =
      transportPanel->mapTo(actionPanel, QPoint());
  const QPoint ftbTopLeft = ftbPanel->mapTo(actionPanel, QPoint());
  CHECK(transportTopLeft.y() == ftbTopLeft.y());
  CHECK(transportTopLeft.x() < ftbTopLeft.x());
  CHECK(transportPanel->height() == ftbPanel->height());
  CHECK(ftbTopLeft.x() + ftbPanel->width() == actionPanel->width());
  const QMargins ftbMargins = ftbPanel->layout()->contentsMargins();
  CHECK(ftbMargins.top() == vtrpon::scalePx(12));
  CHECK(ftbMargins.bottom() == vtrpon::scalePx(12));
  const int ftbCenter = ftbButton->x() + ftbButton->width() / 2;
  CHECK(std::abs(ftbCenter - ftbPanel->width() / 2) <= 1);
  const int seekBottom = programSeekBar->mapTo(&programPanel, QPoint()).y()
                         + programSeekBar->height();
  const int remainingTop = remainingModule->mapTo(&programPanel, QPoint()).y();
  const int remainingBottom = remainingTop + remainingModule->height();
  const int endModeTop = endModeModule->mapTo(&programPanel, QPoint()).y();
  const int endModeBottom = endModeTop + endModeModule->height();
  const int playbackRateTop =
      playbackRateModule->mapTo(&programPanel, QPoint()).y();
  const int playbackRateBottom = playbackRateTop + playbackRateModule->height();
  const QPoint effectsTopLeft = effectsPanel->mapTo(&programPanel, QPoint());
  const QPoint endModeTopLeft = endModeModule->mapTo(&programPanel, QPoint());
  const int effectsBottom = effectsTopLeft.y() + effectsPanel->height();
  const QPoint audioStripTopLeft = audioStrip->mapTo(&programPanel, QPoint());
  const int audioStripBottom = audioStripTopLeft.y() + audioStrip->height();
  const int actionPanelTop = actionPanel->mapTo(&programPanel, QPoint()).y();
  CHECK(remainingTop >= seekBottom);
  CHECK(remainingTop - seekBottom <= 12);
  CHECK(endModeTop >= remainingBottom);
  CHECK(playbackRateTop >= endModeBottom);
  CHECK(effectsTopLeft.y() >= playbackRateBottom);
  CHECK(effectsTopLeft.x() == endModeTopLeft.x());
  CHECK(effectsPanel->width() == endModeModule->width());
  CHECK(effectsBottom == audioStripBottom);
  CHECK(actionPanelTop >= audioStripBottom);
  CHECK(actionPanelTop - audioStripBottom <= vtrpon::scalePx(12));
  CHECK(endModeValue->text() ==
        QStringLiteral("REPEAT(2/3)-NEXT + FO + CF"));
  int previousRequestCount = 0;
  programPanel.operations()->bind<>(
      ControllerOperations::Action::DocumentPrevious, &programPanel,
      [&] { ++previousRequestCount; });
  programPanel.operations()->connectInput(&programPanel,
      &ProgramPanel::powerPointPreviousRequested,
      ControllerOperations::Action::DocumentPrevious);
  programPanel.setOffAirButtonVisible(false);
  CHECK(offAirButton->isHidden());
  programPanel.setOffAirButtonVisible(true);
  CHECK(!offAirButton->isHidden());
  CHECK(!offAirButton->isEnabled());
  CHECK(!playButton->isEnabled());
  CHECK(!pauseButton->isEnabled());
  programPanel.setPlaybackState(true, true);
  CHECK(offAirButton->isEnabled());
  CHECK(playButton->isEnabled());
  CHECK(pauseButton->isEnabled());
  programPanel.setPowerPointNavigationState(true, true, false, 2, 3);
  programPanel.setPlaybackState(true, false);
  application.processEvents();
  CHECK(powerPointPanel->isVisible());
  CHECK(!programSeekBar->isEnabled());
  CHECK(!pauseButton->isEnabled());
  CHECK(!playButton->isEnabled());
  CHECK(powerPointPreviousButton->isEnabled());
  CHECK(!powerPointNextButton->isEnabled());
  CHECK(powerPointPositionLabel->text() == QStringLiteral("2 / 3"));
  if (hardwareSkin) {
    const QPoint programButtonSurfaceProbe(vtrpon::scalePx(6),
                                           vtrpon::scalePx(6));
    CHECK(rgbChannelsWithin(
        pixelAtLogicalPoint(powerPointPreviousButton->grab(),
                            programButtonSurfaceProbe),
        pixelAtLogicalPoint(offAirButton->grab(),
                            programButtonSurfaceProbe),
        2));
    CHECK(rgbChannelsWithin(
        pixelAtLogicalPoint(powerPointNextButton->grab(),
                            programButtonSurfaceProbe),
        pixelAtLogicalPoint(playButton->grab(),
                            programButtonSurfaceProbe),
        2));
    CHECK(rgbChannelsWithin(
        pixelAtLogicalPoint(fastRateButton->grab(),
                            programButtonSurfaceProbe),
        pixelAtLogicalPoint(referenceFadeButton->grab(),
                            QPoint(programButtonSurfaceProbe.x(),
                                   qRound(qreal(programButtonSurfaceProbe.y())
                                          * referenceFadeButton->height()
                                          / fastRateButton->height()))),
        2));
  }
  powerPointPreviousButton->click();
  CHECK(previousRequestCount == 1);
  int nextRequestCount = 0;
  programPanel.operations()->bind<>(
      ControllerOperations::Action::DocumentNext, &programPanel,
      [&] { ++nextRequestCount; });
  programPanel.operations()->connectInput(&programPanel,
      &ProgramPanel::powerPointNextRequested,
      ControllerOperations::Action::DocumentNext);
  programPanel.setPowerPointNavigationState(true, true, true, 2, 3);
  application.processEvents();
  const QImage previousReadyImage = powerPointPreviousButton->grab().toImage();
  const QImage nextReadyImage = powerPointNextButton->grab().toImage();
  programPanel.setPowerPointNavigationState(true, true, true, 2, 3, true);
  application.processEvents();
  CHECK(powerPointPanel->isVisible());
  CHECK(powerPointPreviousButton->isEnabled());
  CHECK(powerPointNextButton->isEnabled());
  CHECK(powerPointPreviousButton->grab().toImage() == previousReadyImage);
  CHECK(powerPointNextButton->grab().toImage() == nextReadyImage);
  powerPointPreviousButton->click();
  powerPointNextButton->click();
  CHECK(previousRequestCount == 1);
  CHECK(nextRequestCount == 0);
  programPanel.setPowerPointNavigationState(true, true, true, 2, 3);
  powerPointPreviousButton->click();
  powerPointNextButton->click();
  CHECK(previousRequestCount == 2);
  CHECK(nextRequestCount == 1);
  programPanel.setPowerPointNavigationState(true, false, true, 1, 3, true);
  CHECK(!powerPointPreviousButton->isEnabled());
  CHECK(powerPointNextButton->isEnabled());
  programPanel.setPowerPointNavigationState(true, true, false, 3, 3, true);
  CHECK(powerPointPreviousButton->isEnabled());
  CHECK(!powerPointNextButton->isEnabled());
  programPanel.setPdfNavigationState(true, true, true, 2, 3, true);
  CHECK(!powerPointPreviousButton->isEnabled());
  CHECK(!powerPointNextButton->isEnabled());
  programPanel.setPowerPointNavigationState(false, false, false);
  application.processEvents();
  CHECK(!powerPointPanel->isVisible());
  CHECK(programSeekBar->isEnabled());
  CHECK(pauseButton->isEnabled());
  CHECK(playButton->isEnabled());
  programPanel.setPlaybackState(false, false);
  programPanel.setPowerPointNavigationState(false, false, false);
  programPanel.setOffAirButtonVisible(false);
  CHECK(offAirButton->isHidden());
  programPanel.setOffAirButtonVisible(true);
  CHECK(!offAirButton->isHidden());
  CHECK(!offAirButton->isEnabled());
  CHECK(!playButton->isEnabled());
  CHECK(!pauseButton->isEnabled());
  programPanel.clearEndMode();
  CHECK(endModeValue->text() == QString(QChar(0x2014)));
  programPanel.close();

  panel.close();
  application.processEvents();
  return 0;
}

int verifyManualFadeBehavior(QApplication &application) {
  ProgramPanel panel;
  panel.show();
  application.processEvents();

  auto *itemFader = dynamic_cast<AudioFaderWidget *>(
      panel.findChild<QWidget *>(QStringLiteral("programItemFader")));
  auto *masterFader = dynamic_cast<AudioFaderWidget *>(
      panel.findChild<QWidget *>(QStringLiteral("programMasterFader")));
  auto *animation = panel.findChild<QVariantAnimation *>(
      QStringLiteral("programManualFadeAnimation"));
  auto *itemFadeInButton = panel.findChild<QPushButton *>(
      QStringLiteral("programItemFadeInButton"));
  auto *itemFadeOutButton = panel.findChild<QPushButton *>(
      QStringLiteral("programItemFadeOutButton"));
  auto *masterFadeInButton = panel.findChild<QPushButton *>(
      QStringLiteral("programMasterFadeInButton"));
  auto *masterFadeOutButton = panel.findChild<QPushButton *>(
      QStringLiteral("programMasterFadeOutButton"));
  auto *itemFadeInDuration = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programItemFadeInDurationSpinBox"));
  auto *itemFadeOutDuration = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programItemFadeOutDurationSpinBox"));
  auto *masterFadeInDuration = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programMasterFadeInDurationSpinBox"));
  auto *masterFadeOutDuration = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programMasterFadeOutDurationSpinBox"));
  CHECK(itemFader != nullptr);
  CHECK(masterFader != nullptr);
  CHECK(animation != nullptr);
  CHECK(itemFadeInButton != nullptr);
  CHECK(itemFadeOutButton != nullptr);
  CHECK(masterFadeInButton != nullptr);
  CHECK(masterFadeOutButton != nullptr);
  CHECK(itemFadeInDuration != nullptr);
  CHECK(itemFadeOutDuration != nullptr);
  CHECK(masterFadeInDuration != nullptr);
  CHECK(masterFadeOutDuration != nullptr);
  for (QDoubleSpinBox *spinBox :
       {itemFadeInDuration, itemFadeOutDuration,
        masterFadeInDuration, masterFadeOutDuration}) {
    spinBox->setValue(0.1);
  }

  // ITEM FI returns to the ON AIR nominal level, not the last manual value.
  panel.setItemFaderValue(-120);
  itemFader->setValue(-300);
  itemFadeInButton->click();
  CHECK(itemFader->value() == -300);
  CHECK(animation->state() == QAbstractAnimation::Running);
  CHECK(itemFadeInButton->property("active").toBool());
  CHECK(waitUntil(application, [itemFader] {
    return itemFader->value() == -120;
  }));
  CHECK(!itemFadeInButton->property("active").toBool());

  itemFadeOutButton->click();
  CHECK(waitUntil(application, [itemFader] {
    return itemFader->value() == itemFader->minimum();
  }));
  itemFadeInButton->click();
  CHECK(waitUntil(application, [itemFader] {
    return itemFader->value() == -120;
  }));

  // MASTER FI always returns to unity and FO reaches silence.
  masterFader->setValue(-300);
  masterFadeInButton->click();
  CHECK(masterFader->value() == -300);
  CHECK(waitUntil(application, [masterFader] {
    return masterFader->value() == 0;
  }));
  masterFadeOutButton->click();
  CHECK(waitUntil(application, [masterFader] {
    return masterFader->value() == masterFader->minimum();
  }));

  // A later FI/FO button stops the previous fade and owns the animation.
  panel.setItemFaderValue(0);
  masterFader->setValue(0);
  itemFadeOutDuration->setValue(0.4);
  itemFadeOutButton->click();
  CHECK(waitUntil(application, [itemFader] {
    return itemFader->value() < -5;
  }));
  const int interruptedItemValue = itemFader->value();
  masterFadeOutButton->click();
  CHECK(!itemFadeOutButton->property("active").toBool());
  CHECK(masterFadeOutButton->property("active").toBool());
  CHECK(waitUntil(application, [masterFader] {
    return masterFader->value() == masterFader->minimum();
  }));
  processFor(application, 350);
  CHECK(itemFader->value() == interruptedItemValue);

  // The next ON AIR cancels ITEM only.
  panel.setItemFaderValue(0);
  itemFadeOutButton->click();
  CHECK(waitUntil(application, [itemFader] {
    return itemFader->value() < -5;
  }));
  panel.setItemFaderValue(-120);
  CHECK(animation->state() == QAbstractAnimation::Stopped);
  processFor(application, 350);
  CHECK(itemFader->value() == -120);

  masterFader->setValue(0);
  masterFadeOutDuration->setValue(0.2);
  masterFadeOutButton->click();
  processFor(application, 30);
  panel.setItemFaderValue(-60);
  CHECK(animation->state() == QAbstractAnimation::Running);
  CHECK(waitUntil(application, [masterFader] {
    return masterFader->value() == masterFader->minimum();
  }));

  ControllerVolumeInput volumeInput(panel.findChild<ControllerOperations *>(), &panel);
  // Either fader can stop a running manual fade; the user input remains.
  masterFadeInDuration->setValue(0.4);
  masterFadeInButton->click();
  CHECK(waitUntil(application, [masterFader] {
    return masterFader->value() > masterFader->minimum();
  }));
  itemFader->setFocus();
  const int itemBeforeKey = itemFader->value();
  QKeyEvent itemKeyPress(QEvent::KeyPress, Qt::Key_Down, Qt::ControlModifier);
  QApplication::sendEvent(itemFader, &itemKeyPress);
  const int interruptedMasterValue = masterFader->value();
  CHECK(itemFader->value() < itemBeforeKey);
  CHECK(animation->state() == QAbstractAnimation::Stopped);
  processFor(application, 350);
  CHECK(masterFader->value() == interruptedMasterValue);

  panel.setItemFaderValue(0);
  itemFader->setValue(itemFader->minimum());
  itemFadeInDuration->setValue(0.4);
  itemFadeInButton->click();
  CHECK(waitUntil(application, [itemFader] {
    return itemFader->value() > itemFader->minimum();
  }));
  masterFader->setValue(0);
  masterFader->setFocus();
  const int masterBeforeKey = masterFader->value();
  QKeyEvent masterKeyPress(QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier);
  QApplication::sendEvent(masterFader, &masterKeyPress);
  const int interruptedItemFadeValue = itemFader->value();
  CHECK(masterFader->value() < masterBeforeKey);
  CHECK(animation->state() == QAbstractAnimation::Stopped);
  processFor(application, 350);
  CHECK(itemFader->value() == interruptedItemFadeValue);

  // Manual ITEM input suppresses automatic audio updates until next ON AIR.
  panel.setItemFaderValue(0);
  panel.setItemFadeProgress(0.5);
  itemFader->setFocus();
  QKeyEvent overrideKeyPress(QEvent::KeyPress, Qt::Key_Up, Qt::ControlModifier);
  QApplication::sendEvent(itemFader, &overrideKeyPress);
  const int overriddenValue = itemFader->value();
  panel.setItemFadeProgress(0.1);
  CHECK(itemFader->value() == overriddenValue);
  panel.setItemFaderValue(-120);
  panel.setItemFadeProgress(0.5);
  CHECK(itemFader->value() == -180);

  panel.close();
  application.processEvents();
  return 0;
}

int verifyProgramOperationGuards() {
  ControllerOperations operations;
  ProgramPanel panel(nullptr, &operations);
  using A = ControllerOperations::Action;
  using S = ControllerOperations::Status;
  int accepted = 0;
  int notifications = 0;
  int documentButtonClicks = 0;
  QString source;
  for (const auto action : {A::DocumentPrevious, A::DocumentNext, A::OffAir,
                            A::ToggleFtb, A::ToggleDsk, A::ProgramPlay,
                            A::ProgramPause, A::ProgramTogglePlayback}) {
    operations.bind<>(action, &panel, [&] { ++accepted; });
  }
  QObject::connect(&operations, &ControllerOperations::operationAboutToExecute,
      &panel, [&](A, const QVariantList &, const QString &inputSource) {
        ++notifications;
        source = inputSource;
      });
  auto *nextButton = panel.findChild<QPushButton *>(
      QStringLiteral("powerPointNextButton"));
  auto *bgmFaderButton = panel.findChild<QPushButton *>(
      QStringLiteral("programBgmFaderButton"));
  auto *bgmFadeDuration = panel.findChild<QDoubleSpinBox *>(
      QStringLiteral("programBgmFadeInDurationSpinBox"));
  CHECK(nextButton && bgmFaderButton && bgmFadeDuration);
  QObject::connect(nextButton, &QPushButton::clicked, &panel,
                   [&] { ++documentButtonClicks; });

  CHECK(operations.executeFrom("Shortcut", A::DocumentNext).status == S::Rejected);
  CHECK(operations.executeFrom("Switcher", A::OffAir).status == S::Rejected);
  CHECK(operations.execute(A::ToggleDsk).status == S::Rejected);
  CHECK(operations.execute(A::OpenItemEq).status == S::Rejected);
  CHECK(operations.execute(A::ProgramPlay).status == S::Rejected);
  CHECK(operations.execute(A::ProgramPause).status == S::Rejected);
  CHECK(operations.execute(A::ProgramTogglePlayback).status == S::Rejected);
  CHECK(accepted == 0 && notifications == 0);
  panel.setPowerPointNavigationState(true, false, true, 1, 3, true);
  panel.triggerShortcutAction(A::DocumentNext);
  CHECK(accepted == 0 && notifications == 0);
  panel.setPowerPointNavigationState(true, false, true, 1, 3);
  CHECK(operations.execute(A::DocumentPrevious).status == S::Rejected);
  // A hidden/minimized Controller must not change semantic availability, and
  // shortcut dispatch must never synthesize a button click.
  CHECK(!panel.isVisible());
  panel.triggerShortcutAction(A::DocumentNext);
  CHECK(accepted == 1 && notifications == 1);
  CHECK(source == QStringLiteral("Shortcut") && documentButtonClicks == 0);
  panel.setPdfNavigationState(true, true, true, 2, 3, true);
  CHECK(operations.execute(A::DocumentPrevious).status == S::Rejected);
  CHECK(accepted == 1 && notifications == 1);

  panel.setFtbControlsVisible(false);
  CHECK(operations.execute(A::ToggleFtb).status == S::Rejected);
  panel.setPlaybackState(true, true);
  CHECK(operations.execute(A::ProgramPlay).status == S::Rejected);
  CHECK(operations.execute(A::ProgramPause).status == S::Rejected);
  CHECK(operations.execute(A::ProgramTogglePlayback).status == S::Rejected);
  panel.setOffAirButtonVisible(false);
  // The separate OFF AIR button is hidden in ON AIR toggle mode, where the
  // playlist's ON AIR button still requests the same OFF AIR operation.
  CHECK(operations.executeFrom("Button", A::OffAir).invoked());
  CHECK(operations.executeFrom("Shortcut", A::OffAir).invoked());
  CHECK(accepted == 3 && notifications == 3);
  panel.setPdfNavigationState(false, false, false);
  CHECK(operations.executeFrom("Shortcut", A::ProgramTogglePlayback).invoked());
  panel.setFtbControlsVisible(true);
  panel.setOffAirButtonVisible(true);
  CHECK(operations.executeFrom("Switcher", A::ToggleFtb).invoked());
  CHECK(operations.execute(A::OffAir).invoked());
  panel.setDskSelectionAvailable(true);
  CHECK(operations.execute(A::ToggleDsk).invoked());
  panel.setDskSelectionAvailable(false);
  panel.setDskCueState(ProgramPanel::DskCueState::Loading);
  CHECK(operations.execute(A::ToggleDsk).invoked());
  panel.setDskCueState(ProgramPanel::DskCueState::Empty);
  panel.setDskOn(true);
  CHECK(operations.execute(A::ToggleDsk).invoked());

  panel.setBgmControlsVisible(true);
  CHECK(panel.bgmControlsVisible());
  CHECK(!panel.configuredBgmFadeAvailable());
  CHECK(operations.execute(A::ItemFadeOut).invoked());
  bgmFaderButton->setChecked(true);
  CHECK(panel.configuredBgmFadeAvailable());
  bgmFadeDuration->setValue(1.7);
  CHECK(panel.configuredBgmFadeInDurationMs() == 1700);
  const int notificationsBeforeUnavailable = notifications;
  CHECK(operations.execute(A::ItemFadeIn).status == S::Rejected);
  CHECK(operations.execute(A::OpenItemEq).status == S::Rejected);
  CHECK(notifications == notificationsBeforeUnavailable);
  CHECK(operations.execute(A::MasterFadeOut).invoked());
  panel.setBgmControlsVisible(false);
  CHECK(!panel.configuredBgmFadeAvailable());
  CHECK(operations.execute(A::ItemFadeIn).invoked());

  auto *itemFader = static_cast<AudioFaderWidget *>(panel.findChild<QWidget *>(
      QStringLiteral("programItemFader")));
  auto *masterFader = static_cast<AudioFaderWidget *>(panel.findChild<QWidget *>(
      QStringLiteral("programMasterFader")));
  auto *bgmFader = static_cast<AudioFaderWidget *>(panel.findChild<QWidget *>(
      QStringLiteral("programBgmFader")));
  auto *mute = panel.findChild<QPushButton *>(QStringLiteral("programMasterMute"));
  CHECK(itemFader && masterFader && bgmFader && mute);
  const std::array<std::array<A, 3>, 3> volumeActions{{
      {A::SetItemVolume, A::AdjustItemVolume, A::ResetItemVolume},
      {A::SetMasterVolume, A::AdjustMasterVolume, A::ResetMasterVolume},
      {A::SetBgmVolume, A::AdjustBgmVolume, A::ResetBgmVolume}}};
  const std::array<AudioFaderWidget *, 3> faders{itemFader, masterFader, bgmFader};
  for (size_t index = 0; index < faders.size(); ++index) {
    auto *fader = faders[index];
    const auto actions = volumeActions[index];
    const int before = notifications;
    CHECK(operations.execute(actions[0], fader->maximum() + 1).status == S::Rejected);
    CHECK(notifications == before);
    fader->setEnabled(false);
    CHECK(operations.execute(actions[0], 0).status == S::Rejected);
    CHECK(operations.execute(actions[1], 10).status == S::Rejected);
    CHECK(operations.execute(actions[2]).status == S::Rejected);
    CHECK(notifications == before);
    fader->setEnabled(true);
    // Hidden faders retain their volume controls; each input is one operation.
    CHECK(operations.executeFrom("Shortcut", actions[1], -10).invoked());
    CHECK(notifications == before + 1);
    CHECK(operations.execute(actions[2]).invoked());
    CHECK(notifications == before + 2 && fader->value() == 0);
  }
  const int beforeMute = notifications;
  mute->setEnabled(false);
  CHECK(operations.execute(A::MasterMute, true).status == S::Rejected);
  CHECK(notifications == beforeMute);
  mute->setEnabled(true);
  CHECK(operations.execute(A::MasterMute, true).invoked());
  CHECK(notifications == beforeMute + 1 && mute->isChecked());
  return 0;
}

int verifyEditOperationGuards() {
  ControllerOperations operations;
  UserInfoCenter info;
  EditPanel panel(&info, nullptr, &operations);
  using A = ControllerOperations::Action;
  using S = ControllerOperations::Status;
  auto *pfl = panel.findChild<QPushButton *>(QStringLiteral("editPflButton"));
  auto *nominalEq = panel.findChild<QPushButton *>(
      QStringLiteral("editNominalEqButton"));
  CHECK(pfl && nominalEq);
  int notifications = 0;
  int simulatedClicks = 0;
  QString source;
  QList<bool> pflRequests;
  QObject::connect(&operations, &ControllerOperations::operationAboutToExecute,
      &panel, [&](A, const QVariantList &, const QString &inputSource) {
        ++notifications;
        source = inputSource;
      });
  QObject::connect(&panel, &EditPanel::pflRequested, &panel,
      [&](bool enabled) { pflRequests.append(enabled); });
  QObject::connect(pfl, &QPushButton::clicked, &panel,
      [&] { ++simulatedClicks; });
  QObject::connect(nominalEq, &QPushButton::clicked, &panel,
      [&] { ++simulatedClicks; });

  panel.setPflState(false, false, QStringLiteral("Unavailable"));
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).status == S::Rejected);
  CHECK(operations.executeFrom("Shortcut", A::OpenNominalEq).status == S::Rejected);
  CHECK(notifications == 0 && pflRequests.isEmpty());
  CHECK(!panel.findChild<QDialog *>(QStringLiteral("editNominalEqWindow")));

  panel.setPflState(false, true, QStringLiteral("PFL OFF"));
  CHECK(!panel.isVisible());
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).invoked());
  CHECK(pflRequests == QList<bool>{true});
  CHECK(pfl->isChecked());
  CHECK(notifications == 1 && source == QStringLiteral("Shortcut"));
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).invoked());
  CHECK((pflRequests == QList<bool>{true, false}));
  CHECK(!pfl->isChecked());
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).invoked());
  CHECK(pfl->isChecked());
  // Backend rejection resets the pending request, so the next shortcut asks
  // to enable PFL again rather than toggling stale button state.
  panel.setPflState(false, true, QStringLiteral("PFL OFF"));
  CHECK(pflRequests.size() == 3 && notifications == 3);
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).invoked());
  CHECK((pflRequests == QList<bool>{true, false, true, true}));
  panel.setPflState(true, true, QStringLiteral("PFL ON"));
  CHECK(pflRequests.size() == 4 && notifications == 4);
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).invoked());
  CHECK(!pflRequests.last() && !pfl->isChecked());
  panel.setPflState(true, false, QStringLiteral("Unavailable"));
  CHECK(operations.executeFrom("Shortcut", A::TogglePfl).status == S::Rejected);
  CHECK(pflRequests.size() == 5 && notifications == 5 && pfl->isChecked());
  CHECK(simulatedClicks == 0);

  // This existing UI fixture uses the built-in equalizer. Never launch an
  // external plug-in as part of the shortcut route regression.
  CHECK(!vtrpon::vstModeEnabled());
  panel.setEditorState(PlaylistItemSettings{}, true, true);
  CHECK(operations.executeFrom("Shortcut", A::OpenNominalEq).invoked());
  auto *eqWindow = panel.findChild<QDialog *>(QStringLiteral("editNominalEqWindow"));
  CHECK(eqWindow && eqWindow->isVisible());
  CHECK(notifications == 6 && simulatedClicks == 0);
  eqWindow->close();
  panel.setEditorState(PlaylistItemSettings{}, false, false);
  CHECK(operations.executeFrom("Shortcut", A::OpenNominalEq).status == S::Rejected);
  CHECK(!eqWindow->isVisible() && notifications == 6);
  return 0;
}

int verifyOperationInputs(QApplication &application) {
  ControllerOperations operations;
  ProgramPanel panel(nullptr, &operations);
  ControllerVolumeInput volumeInput(&operations, &panel);
  UserInfoCenter info;
  EditPanel edit(&info, nullptr, &operations);
  using A = ControllerOperations::Action;
  using S = ControllerOperations::Status;
  auto *item = static_cast<AudioFaderWidget *>(panel.findChild<QWidget *>(QStringLiteral("programItemFader")));
  auto *master = static_cast<AudioFaderWidget *>(panel.findChild<QWidget *>(QStringLiteral("programMasterFader")));
  auto *animation = panel.findChild<QVariantAnimation *>(QStringLiteral("programManualFadeAnimation"));
  CHECK(item && master && animation);
  CHECK(operations.execute(A::SetEditVolume, -100).status == S::Rejected);
  CHECK(operations.execute(A::SetMasterVolume, -100).invoked());
  CHECK(master->value() == -100);
  CHECK(operations.execute(A::AdjustMasterVolume, 10).invoked());
  CHECK(master->value() == -90);
  CHECK(operations.execute(A::SetMasterVolume, 100000).status == S::Rejected);
  CHECK(master->value() == -90);
  CHECK(operations.execute(A::ResetMasterVolume).invoked());
  CHECK(master->value() == 0);
  CHECK(operations.execute(A::MasterFadeOut).invoked());
  CHECK(animation->state() == QAbstractAnimation::Running);
  CHECK(operations.execute(A::SetMasterVolume, -120).invoked());
  CHECK(animation->state() == QAbstractAnimation::Stopped);
  CHECK(master->value() == -120);
  panel.setItemFaderValue(0);
  panel.setItemFadeProgress(0.5);
  CHECK(operations.execute(A::SetItemVolume, -100).invoked());
  panel.setItemFadeProgress(0.1);
  CHECK(item->value() == -100);
  CHECK(operations.execute(A::MasterFadeOut).invoked());
  QKeyEvent plainKey(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
  QApplication::sendEvent(item, &plainKey);
  QWheelEvent plainWheel(QPointF(), QPointF(), QPoint(), QPoint(0, 120),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(master, &plainWheel);
  CHECK(item->value() == -100 && master->value() == -120);
  CHECK(animation->state() == QAbstractAnimation::Running);
  QLineEdit textInput(&panel);
  QKeyEvent key(QEvent::KeyPress, Qt::Key_Up, Qt::ControlModifier);
  QApplication::sendEvent(&textInput, &key);
  CHECK(animation->state() == QAbstractAnimation::Running);
  CHECK(item->value() == -100);
  QWheelEvent wheel(QPointF(), QPointF(), QPoint(), QPoint(120, 0),
                    Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(&textInput, &wheel);
  CHECK(master->value() == -120 && item->value() == -100);
  QKeyEvent itemUp(QEvent::KeyPress, Qt::Key_Up, Qt::ControlModifier);
  QApplication::sendEvent(item, &itemUp);
  CHECK(item->value() == -90);
  CHECK(animation->state() == QAbstractAnimation::Stopped);
  CHECK(operations.execute(A::AdjustItemVolume, -10).invoked());
  QKeyEvent masterDown(QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier);
  QApplication::sendEvent(item, &masterDown);
  CHECK(master->value() == -130 && item->value() == -100);
  CHECK(operations.execute(A::MasterFadeOut).invoked());
  QKeyEvent homeKey(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
  QApplication::sendEvent(item, &homeKey);
  CHECK(item->value() == item->minimum());
  CHECK(animation->state() == QAbstractAnimation::Stopped);
  CHECK(operations.execute(A::SetItemVolume, -100).invoked());
  CHECK(operations.execute(A::MasterFadeOut).invoked());
  // Stopping the shared layer also stops component keyboard mutations.
  operations.disable();
  QKeyEvent disabledKey(QEvent::KeyPress, Qt::Key_Up, Qt::ControlModifier);
  QApplication::sendEvent(item, &disabledKey);
  CHECK(item->value() == -100);
  for (auto *fader : {item, master}) {
    const int valueBefore = fader->value();
    for (const int key : {Qt::Key_Home, Qt::Key_End}) {
      QKeyEvent disabledEdgeKey(QEvent::KeyPress, key, Qt::NoModifier);
      QApplication::sendEvent(fader, &disabledEdgeKey);
      CHECK(fader->value() == valueBefore);
      CHECK(animation->state() == QAbstractAnimation::Running);
    }
  }
  application.processEvents();
  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  if (application.arguments().contains("--midi-master-only")) {
    namespace midi = vtrpon::midiControl;
    QTemporaryDir directory; CHECK(directory.isValid());
    application.setProperty("vtrpon.settingsFile", directory.filePath("midi.ini"));
    auto settings = vtrpon::makeControllerSettings();
    auto config = midi::defaults(); config.insert("enabled", true); config.insert("device", "test-input");
    const QJsonObject binding{{"type", "cc"}, {"channel", 1}, {"number", 7}, {"pickup", true}, {"minimum", -960}, {"maximum", 60}, {"scale", "fader"}};
    config.insert("bindings", QJsonObject{{"SetMasterVolume", binding}, {"TakeOnAir", QJsonObject{{"type", "note"}, {"channel", 1}, {"number", 60}}}, {"OffAir", QJsonObject{{"type", "note"}, {"channel", 1}, {"number", 61}}}});
    settings.setValue(midi::settingsKey(), config.toVariantMap()); settings.sync();
    QWidget window; ProgramPanel panel(&window);
    auto *fader = panel.findChild<QSlider *>("programMasterFader"); CHECK(fader && fader->isEnabled());
    int buttons = 0;
    QStringList events;
    vtrpon::MidiControlInput input(panel.operations(), [&](const QString &id) { ++buttons; events.append(id); });
    input.setVolumeValueProvider([&](const QString &) {
      const auto gain = panel.masterGain();
      return gain <= 0 ? -960 : int(std::lround(200.0 * std::log10(gain)));
    });
    const auto send = [&](int value) { vtrpon::MidiControlTestAccess::send(input, 0xb0 | (7 << 8) | (quint32(value) << 16)); };
    input.setConfigurationMode(true);
    for (int raw = 0; raw <= 127; ++raw) send(raw);
    CHECK(fader->value() == 0);
    input.reload();
    vtrpon::MidiControlTestAccess::send(input, 0x7f3c90);
    input.setConfigurationMode(false);
    vtrpon::MidiControlTestAccess::send(input, 0x7f3c90); CHECK(buttons == 0);
    vtrpon::MidiControlTestAccess::send(input, 0x003c80);
    vtrpon::MidiControlTestAccess::send(input, 0x7f3c90); CHECK(buttons == 1);
    send(10); CHECK(fader->value() == 0);
    send(102); CHECK(fader->value() == 0);
    send(127); CHECK(fader->value() == 60);
    send(0); CHECK(fader->value() == -960);
    QDialog modal; modal.setModal(true); modal.open(); application.processEvents();
    send(127); CHECK(fader->value() == -960);
    modal.close(); application.processEvents();
    send(126); CHECK(fader->value() > 0);
    fader->setValue(-100); send(20); CHECK(fader->value() == -100);
    send(110); CHECK(fader->value() > -100);
    fader->setValue(0);
    input.setConfigurationMode(true); send(102); input.setConfigurationMode(false);
    send(102); send(110); CHECK(fader->value() > 0);
    int volumeOperations = 0;
    QObject::connect(panel.operations(), &ControllerOperations::operationAboutToExecute, &window,
        [&](ControllerOperations::Action action, const QVariantList &, const QString &) {
          if (action == ControllerOperations::Action::SetMasterVolume) { ++volumeOperations; events.append("volume"); }
        });
    fader->setValue(0); events.clear();
    const int beforeBurst = buttons;
    vtrpon::MidiControlTestAccess::enqueue(input, 0x003c80);
    for (int packet = 0; packet < 20000; ++packet) {
      vtrpon::MidiControlTestAccess::enqueue(input, 0xb0 | (7 << 8) | (quint32(packet % 128) << 16));
      if (packet == 2000) vtrpon::MidiControlTestAccess::enqueue(input, 0x7f3c90);
      if (packet == 4000) vtrpon::MidiControlTestAccess::enqueue(input, 0x003c80);
      if (packet == 6000) vtrpon::MidiControlTestAccess::enqueue(input, 0x7f3d90);
      if (packet == 8000) vtrpon::MidiControlTestAccess::enqueue(input, 0x003d80);
    }
    vtrpon::MidiControlTestAccess::enqueue(input, 0x7f07b0);
    vtrpon::MidiControlTestAccess::drain(input);
    CHECK(buttons == beforeBurst + 2);
    CHECK(events == QStringList({"TakeOnAir", "OffAir", "volume"}));
    CHECK(volumeOperations == 1 && fader->value() == 60);
    input.setConfigurationMode(true);
    vtrpon::MidiControlTestAccess::send(input, 0x7f3c90);
    vtrpon::MidiControlTestAccess::enqueue(input, 0x003c80);
    input.setConfigurationMode(false);
    const int beforeReleasedButton = buttons;
    vtrpon::MidiControlTestAccess::enqueue(input, 0x7f3c90);
    vtrpon::MidiControlTestAccess::drain(input);
    CHECK(buttons == beforeReleasedButton + 1);
    input.stop();
    std::puts("MIDI save/resume, held buttons, Pickup, modal suspension, real MASTER fader and 20,000-CC burst with both buttons: PASS");
    return 0;
  }
  const bool repeatControlsOnly = argc == 5 &&
      QString::fromLocal8Bit(argv[4]) == QStringLiteral("--repeat-controls-only");
  const bool liveControlsOnly = argc == 5 &&
      QString::fromLocal8Bit(argv[4]) == QStringLiteral("--live-controls-only");
  CHECK(argc == 4 || repeatControlsOnly || liveControlsOnly);
  QTranslator englishTranslator;
  CHECK(englishTranslator.load(
      QCoreApplication::applicationDirPath()
      + QStringLiteral("/vtrpon_en.qm")));
  CHECK(application.installTranslator(&englishTranslator));
  const QString hardwareStyle = QString::fromLocal8Bit(argv[1]);
  const QString materialDarkStyle = QString::fromLocal8Bit(argv[2]);
  const QString vtrPonClasicStyle = QString::fromLocal8Bit(argv[3]);
  if (liveControlsOnly) {
    UserInfoCenter info;
    EditPanel panel(&info);
    PlaylistItemSettings settings;
    settings.startMode = PlaylistStartMode::Play;
    settings.fadeInEnabled = true;
    panel.setTransportEnabled(false);
    panel.setEditorState(settings, false, true, {}, {}, true);
    const auto enabled = [&](const char *name) {
      const auto *widget = panel.findChild<QWidget *>(QString::fromLatin1(name));
      return widget && widget->isEnabled();
    };
    CHECK(!enabled("editPlayButton") && !enabled("editPauseButton"));
    CHECK(!enabled("editSetInButton") && !enabled("editSetOutButton"));
    CHECK(!enabled("editInPointTimeEdit") && !enabled("editOutPointTimeEdit"));
    auto *startPlay = panel.findChild<QPushButton *>("editStartModePlayButton");
    CHECK(startPlay && startPlay->isChecked() && !startPlay->isEnabled());
    CHECK(!enabled("editStartModePauseButton"));
    CHECK(enabled("editFadeInCheckBox") && enabled("editFadeInDurationSpinBox"));
    CHECK(!enabled("editEndModeStopButton") && !enabled("editEndModePauseButton") &&
          !enabled("editEndModeRepeatButton") && !enabled("editEndModeNextButton") &&
          !enabled("editEndModeGotoButton") && !enabled("editFadeOutCheckBox"));
    CHECK(enabled("editNominalVolumeFader") && enabled("editNominalEqButton"));
    CHECK(!enabled("editNominalDelaySpin"));
    auto *liveWidget = panel.findChild<QWidget *>("editLivePreview");
    CHECK(liveWidget);
    auto *monitor = qobject_cast<QStackedLayout *>(liveWidget->parentWidget()->layout());
    CHECK(monitor);
    QImage frame(640, 360, QImage::Format_RGBA8888);
    frame.fill(Qt::red);
    panel.setLiveFrame(frame);
    CHECK(monitor->currentWidget() == liveWidget);
    panel.show();
    application.processEvents();
    panel.setEnabled(false);
    application.processEvents();
    const QImage shutdownFrame = liveWidget->grab().toImage();
    CHECK(!shutdownFrame.isNull() &&
          shutdownFrame.pixelColor(shutdownFrame.width() / 2,
                                   shutdownFrame.height() / 2) == Qt::red);
    panel.clearLiveFrame();
    CHECK(monitor->currentWidget() == panel.videoSurface());
    std::fprintf(stdout, "EDIT live controls and preview: PASS\n");
    return 0;
  }
  if (repeatControlsOnly) {
    CHECK(verifySkin(application, {hardwareStyle}, true) == 0);
    CHECK(verifySkin(application, {materialDarkStyle}, true) == 0);
    CHECK(verifySkin(application, {materialDarkStyle, vtrPonClasicStyle}, true) == 0);
    std::fprintf(stdout, "Repeat controls in all three skins: PASS\n");
    return 0;
  }
  CHECK(verifyControllerDensity(application,
      {{hardwareStyle}, {materialDarkStyle},
       {materialDarkStyle, vtrPonClasicStyle}}) == 0);
  CHECK(verifyWindowScaling(application, {hardwareStyle}) == 0);
  CHECK(verifyWindowScaling(application, {materialDarkStyle}) == 0);
  CHECK(verifyWindowScaling(application, {materialDarkStyle, vtrPonClasicStyle}) == 0);
  CHECK(verifySkin(application, {hardwareStyle}) == 0);
  CHECK(verifySkin(application, {materialDarkStyle}) == 0);
  CHECK(verifySkin(application,
                   {materialDarkStyle, vtrPonClasicStyle}) == 0);
  CHECK(verifyManualFadeBehavior(application) == 0);
  CHECK(verifyProgramOperationGuards() == 0);
  CHECK(verifyEditOperationGuards() == 0);
  CHECK(verifyOperationInputs(application) == 0);
  CHECK(application.removeTranslator(&englishTranslator));
  QTranslator japaneseTranslator;
  CHECK(japaneseTranslator.load(
      QCoreApplication::applicationDirPath()
      + QStringLiteral("/vtrpon_ja.qm")));
  CHECK(application.installTranslator(&japaneseTranslator));
  ProgramPanel japanesePanel;
  auto *japaneseEqButton = japanesePanel.findChild<QPushButton *>(QStringLiteral("programMasterEqButton"));
  CHECK(japaneseEqButton && japaneseEqButton->toolTip().contains(QStringLiteral("フラット")));
  japaneseEqButton->click();
  auto *japaneseEqWindow = japanesePanel.findChild<QDialog *>(QStringLiteral("programMasterEqWindow"));
  CHECK(japaneseEqWindow && japaneseEqWindow->windowTitle() == QStringLiteral("マスターEQ"));
  auto *japaneseEqReset = japaneseEqWindow->findChild<QPushButton *>(QStringLiteral("programMasterEqResetButton"));
  CHECK(japaneseEqReset && japaneseEqReset->text() == QStringLiteral("フラットに戻す"));
  japaneseEqWindow->close();
  auto *japaneseBackgroundButton = japanesePanel.findChild<QToolButton *>(QStringLiteral("programBackgroundColorButton"));
  CHECK(japaneseBackgroundButton && japaneseBackgroundButton->menu());
  CHECK(japaneseBackgroundButton->toolTip().contains(QStringLiteral("プレイヤー背景色")));
  CHECK(japaneseBackgroundButton->menu()->actions().first()->text() == QStringLiteral("背景色"));
  CHECK(japaneseBackgroundButton->menu()->actions()[2]->text() == QStringLiteral("デジタルグリーン"));
  CHECK(japaneseBackgroundButton->menu()->actions().size() == 6);
  auto *playbackRateModule = japanesePanel.findChild<QWidget *>(
      QStringLiteral("programPlaybackRateModule"));
  CHECK(playbackRateModule != nullptr);
  auto *playbackRateTitle = playbackRateModule->findChild<QLabel *>(
      QStringLiteral("programControlModuleTitle"));
  CHECK(playbackRateTitle != nullptr);
  CHECK(playbackRateTitle->text() == QStringLiteral("再生速度"));
  auto *itemFaderLabel = japanesePanel.findChild<QLabel *>(
      QStringLiteral("itemFaderLabel"));
  CHECK(itemFaderLabel != nullptr);
  CHECK(itemFaderLabel->text() == QStringLiteral("アイテム"));
  return 0;
}
