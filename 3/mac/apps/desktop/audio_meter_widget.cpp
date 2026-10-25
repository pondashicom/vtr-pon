#include "audio_meter_widget.h"

#include "controller_scaling.h"
#include "language_text.h"

#include <QEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMeterFloorDb = -60.0;
constexpr double kReleaseDbPerSecond = 72.0;
constexpr double kCellFadeOutSeconds = 0.08;
constexpr qint64 kClipHoldMs = 500;

} // namespace

AudioMeterWidget::AudioMeterWidget(QWidget *parent)
    : QWidget(parent)
{
    setAccessibleName(vtrpon::uiText("audio_meter.stereo_accessible"));
    setFixedWidth(vtrpon::scalePx(64));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void AudioMeterWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        setAccessibleName(vtrpon::uiText("audio_meter.stereo_accessible"));
    }
}

void AudioMeterWidget::setCompactStripMode(bool compact)
{
    if (compactStripMode_ == compact) {
        return;
    }
    compactStripMode_ = compact;
    setFixedWidth(vtrpon::scalePx(compact ? 25 : 64));
    update();
}

void AudioMeterWidget::setExtendedTopRange(bool extended)
{
    if (extendedTopRange_ == extended) {
        return;
    }
    extendedTopRange_ = extended;
    update();
}

bool AudioMeterWidget::setPeakLevelsDbfs(double leftPeakDb, double rightPeakDb,
                                         bool clipped)
{
    if (!clipClock_.isValid()) {
        clipClock_.start();
    }
    if (clipped) {
        clipUntilMs_ = clipClock_.elapsed() + kClipHoldMs;
    }
    const bool clipIndicatorOn = clipClock_.elapsed() < clipUntilMs_;
    bool visualChanged = clipIndicatorOn_ != clipIndicatorOn;
    clipIndicatorOn_ = clipIndicatorOn;

    if (!releaseClock_.isValid()) {
        releaseClock_.start();
        leftPeakDb_ = leftPeakDb;
        rightPeakDb_ = rightPeakDb;
        visualChanged |= updateCellBrightness(
            leftCellBrightness_, leftPeakDb_, 0.0, true);
        visualChanged |= updateCellBrightness(
            rightCellBrightness_, rightPeakDb_, 0.0, true);
    } else {
        const double elapsedSeconds = animationPending_
            ? releaseClock_.nsecsElapsed() / 1.0e9 : 0.0;
        releaseClock_.restart();
        const double maximumDrop = kReleaseDbPerSecond * elapsedSeconds;
        const auto applyRelease = [maximumDrop](double current, double target) {
            return target >= current
                ? target : (std::max)(target, current - maximumDrop);
        };
        leftPeakDb_ = applyRelease(leftPeakDb_, leftPeakDb);
        rightPeakDb_ = applyRelease(rightPeakDb_, rightPeakDb);
        visualChanged |= updateCellBrightness(
            leftCellBrightness_, leftPeakDb_, elapsedSeconds);
        visualChanged |= updateCellBrightness(
            rightCellBrightness_, rightPeakDb_, elapsedSeconds);
    }

    if (visualChanged) {
        update();
    }
    animationPending_ = clipIndicatorOn_
        || leftPeakDb_ > leftPeakDb
        || rightPeakDb_ > rightPeakDb
        || cellFadePending(leftCellBrightness_, leftPeakDb_)
        || cellFadePending(rightCellBrightness_, rightPeakDb_);
    return animationPending_;
}

void AudioMeterWidget::resetToSilence()
{
    const bool visualChanged = leftPeakDb_ != kMeterFloorDb
        || rightPeakDb_ != kMeterFloorDb
        || clipIndicatorOn_
        || std::any_of(leftCellBrightness_.cbegin(), leftCellBrightness_.cend(),
                       [](double brightness) { return brightness != 0.0; })
        || std::any_of(rightCellBrightness_.cbegin(), rightCellBrightness_.cend(),
                       [](double brightness) { return brightness != 0.0; });

    leftPeakDb_ = kMeterFloorDb;
    rightPeakDb_ = kMeterFloorDb;
    leftCellBrightness_.fill(0.0);
    rightCellBrightness_.fill(0.0);
    releaseClock_.invalidate();
    clipClock_.invalidate();
    clipUntilMs_ = 0;
    clipIndicatorOn_ = false;
    animationPending_ = false;

    if (visualChanged) {
        update();
    }
}

void AudioMeterWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (!compactStripMode_) {
        painter.fillRect(rect(), QColor(QStringLiteral("#101214")));
    }
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int meterTop = vtrpon::scalePx(
        compactStripMode_ ? (extendedTopRange_ ? 22 : 43) : 10);
    const int meterBottom = (std::max)(
        meterTop + 1,
        height() - vtrpon::scalePx(compactStripMode_ ? 55 : 26));
    const int meterHeight = meterBottom - meterTop;
    const int barWidth = vtrpon::scalePx(compactStripMode_ ? 7 : 8);
    const int leftBarX = vtrpon::scalePx(compactStripMode_ ? 4 : 9);
    const int rightBarX = vtrpon::scalePx(compactStripMode_ ? 14 : 21);
    const int scaleX = vtrpon::scalePx(38);
    const int clipLedSize = vtrpon::scalePx(6);

    const QRect clipLed((width() - clipLedSize) / 2,
                        vtrpon::scalePx(
                            compactStripMode_ ? (extendedTopRange_ ? 8 : 29) : 1),
                        clipLedSize, clipLedSize);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QColor(clipIndicatorOn_ ? QStringLiteral("#48171a")
                                          : QStringLiteral("#151718")));
    painter.setBrush(QColor(clipIndicatorOn_ ? QStringLiteral("#85252b")
                                            : QStringLiteral("#292c2e")));
    painter.drawEllipse(clipLed);
    painter.restore();

    const auto yForDb = [meterTop, meterBottom, meterHeight](double db) {
        const double bounded = std::clamp(db, kMeterFloorDb, 0.0);
        const double proportion = (bounded - kMeterFloorDb) / -kMeterFloorDb;
        return meterBottom
            - static_cast<int>(std::lround(proportion * meterHeight));
    };

    if (!compactStripMode_) {
        const int ticks[] = {0, -6, -12, -18, -24, -30, -40, -50, -60};
        QFont meterFont = painter.font();
        meterFont.setPixelSize(vtrpon::scalePx(10));
        painter.setFont(meterFont);
        const int tickInset = vtrpon::scalePx(3);
        const int tickTextTop = vtrpon::scalePx(8);
        const int tickTextHeight = vtrpon::scalePx(16);
        const int tickTextRightMargin = vtrpon::scalePx(2);
        for (const int db : ticks) {
            const int y = yForDb(db);
            painter.setPen(QColor(QStringLiteral("#454a4d")));
            painter.drawLine(tickInset, y, scaleX - tickInset, y);
            painter.setPen(QColor(QStringLiteral("#b7babc")));
            painter.drawText(QRect(scaleX, y - tickTextTop,
                                   width() - scaleX - tickTextRightMargin,
                                   tickTextHeight),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             QString::number(db));
        }
    }

    const QColor barBackground = compactStripMode_
        ? QColor(0x09, 0x0a, 0x0a) : QColor(0x33, 0x33, 0x33);
    const QColor unlitColor = compactStripMode_
        ? QColor(0x17, 0x19, 0x1a) : QColor(0x55, 0x55, 0x55);
    const QColor highColor = compactStripMode_
        ? QColor(0xf0, 0x19, 0x24) : QColor(0xc0, 0x50, 0x50);
    const QColor middleColor = compactStripMode_
        ? QColor(0xf5, 0xe5, 0x00) : QColor(210, 160, 120);
    const QColor lowColor = compactStripMode_
        ? QColor(0x16, 0xc7, 0x33) : QColor(90, 130, 90);
    const QColor segmentBorder = compactStripMode_
        ? QColor(0x11, 0x13, 0x13) : QColor(0x44, 0x44, 0x44);
    const QColor outlineColor = compactStripMode_
        ? QColor(0x05, 0x06, 0x06) : QColor(0x60, 0x66, 0x6a);

    const auto drawBar = [&](int x, const auto &cellBrightness) {
        const QRect outline(x, meterTop, barWidth, meterHeight);
        painter.fillRect(outline, barBackground);
        for (int index = 0; index < kSegmentCount; ++index) {
            const int top = meterTop
                + static_cast<int>(std::lround(
                    static_cast<double>(index) * meterHeight / kSegmentCount));
            const int bottom = meterTop
                + static_cast<int>(std::lround(
                    static_cast<double>(index + 1) * meterHeight / kSegmentCount));
            const double segmentDb = -static_cast<double>(index)
                * -kMeterFloorDb / (kSegmentCount - 1);
            const QColor &litColor = segmentDb >= -9.0
                ? highColor : segmentDb >= -20.0 ? middleColor : lowColor;
            const double brightness = std::clamp(cellBrightness[index], 0.0, 1.0);
            const auto blendChannel = [brightness](int unlit, int lit) {
                return static_cast<int>(std::lround(
                    unlit + (lit - unlit) * brightness));
            };
            const QColor color(blendChannel(unlitColor.red(), litColor.red()),
                               blendChannel(unlitColor.green(), litColor.green()),
                               blendChannel(unlitColor.blue(), litColor.blue()));
            painter.fillRect(QRect(x, top, barWidth,
                                   (std::max)(1, bottom - top)), color);
            if (index > 0) {
                painter.setPen(segmentBorder);
                painter.drawLine(x, top, x + barWidth - 1, top);
            }
        }

        painter.setPen(outlineColor);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(outline.adjusted(0, 0, -1, -1));
    };

    drawBar(leftBarX, leftCellBrightness_);
    drawBar(rightBarX, rightCellBrightness_);

    painter.setPen(compactStripMode_
                       ? QColor(QStringLiteral("#7c8387"))
                       : QColor(QStringLiteral("#d7d9da")));
    const int channelLabelTop = meterBottom + vtrpon::scalePx(5);
    const int channelLabelHeight = vtrpon::scalePx(17);
    painter.drawText(QRect(leftBarX, channelLabelTop,
                           barWidth, channelLabelHeight),
                     Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("L"));
    painter.drawText(QRect(rightBarX, channelLabelTop,
                           barWidth, channelLabelHeight),
                     Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("R"));
}

bool AudioMeterWidget::updateCellBrightness(CellBrightness &brightness, double peakDb,
                                            double elapsedSeconds, bool initialize)
{
    const int activeSegments = std::clamp(
        static_cast<int>(std::lround(peakDb - kMeterFloorDb)),
        0, kSegmentCount);
    const double fadeStep = initialize ? 1.0 : elapsedSeconds / kCellFadeOutSeconds;
    bool changed = false;
    for (int index = 0; index < kSegmentCount; ++index) {
        const double previous = brightness[index];
        if (index >= kSegmentCount - activeSegments) {
            brightness[index] = 1.0;
        } else {
            brightness[index] = (std::max)(
                0.0, brightness[index] - fadeStep);
        }
        changed |= brightness[index] != previous;
    }
    return changed;
}

bool AudioMeterWidget::cellFadePending(const CellBrightness &brightness,
                                       double peakDb)
{
    const int activeSegments = std::clamp(
        static_cast<int>(std::lround(peakDb - kMeterFloorDb)),
        0, kSegmentCount);
    for (int index = 0; index < kSegmentCount; ++index) {
        const double settled = index >= kSegmentCount - activeSegments ? 1.0 : 0.0;
        if (brightness[index] != settled) {
            return true;
        }
    }
    return false;
}
