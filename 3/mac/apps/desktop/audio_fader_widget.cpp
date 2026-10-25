#include "audio_fader_widget.h"
#include "audio_fader_scale.h"

#include "controller_scaling.h"
#include "skins/controller_skin.h"
#include "language_text.h"

#include <QEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSizePolicy>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSilenceValue = vtrpon::audioFaderScale::silence;
constexpr int kUnityValue = 0;
constexpr int kMaximumValue = vtrpon::audioFaderScale::maximum;
constexpr int kTenthsPerDb = 10;
constexpr int kUnityDetentEnterPx = 2;
constexpr int kUnityDetentExitPx = 4;

QString valueText(int value)
{
    if (value == kSilenceValue) {
        return QString(QChar(0x2212)) + QChar(0x221e);
    }

    const double db = static_cast<double>(value) / kTenthsPerDb;
    return QStringLiteral("%1%2")
        .arg(value >= 0 ? QStringLiteral("+") : QString())
        .arg(db, 0, 'f', 2);
}

} // namespace

AudioFaderWidget::AudioFaderWidget(QWidget *parent)
    : AudioFaderWidget(Role::Master, parent)
{
}

AudioFaderWidget::AudioFaderWidget(Role role, QWidget *parent)
    : QSlider(Qt::Vertical, parent)
    , role_(role)
{
    setRange(kSilenceValue, kMaximumValue);
    setValue(kUnityValue);
    setSingleStep(10);
    setPageStep(50);
    setTracking(true);
    setFixedWidth(vtrpon::scalePx(61));
    setMinimumHeight(vtrpon::scalePx(250));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    QObject::connect(this, &QSlider::valueChanged, this, [this] {
        updateValueDescription();
    });
    connect(this, &QSlider::actionTriggered, this, [this](int) {
        requestUserValue(sliderPosition());
        // The action is about to commit sliderPosition. If rejected, restore
        // it to the unchanged value instead of letting QSlider bypass guards.
        setSliderPosition(value());
    });
    retranslateUi();
}

void AudioFaderWidget::setUserValueHandler(std::function<void(int)> handler)
{
    userValueHandler_ = std::move(handler);
}

void AudioFaderWidget::requestUserValue(int requestedValue)
{
    if (userValueHandler_) userValueHandler_(requestedValue);
    else setValue(requestedValue);
}

void AudioFaderWidget::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        event->accept();
        return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        // Reserved for Controller document navigation, never volume changes.
        event->ignore();
        return;
    default:
        QSlider::keyPressEvent(event);
        return;
    }
}

void AudioFaderWidget::wheelEvent(QWheelEvent *event)
{
    event->accept();
}

void AudioFaderWidget::changeEvent(QEvent *event)
{
    QSlider::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void AudioFaderWidget::retranslateUi()
{
    const char *nameId = role_ == Role::NominalItem
        ? "audio_fader.nominal_name"
        : role_ == Role::Item ? "audio_fader.item_name"
                              : "audio_fader.master_name";
    setAccessibleName(vtrpon::uiText(nameId));
    updateValueDescription();
}

double AudioFaderWidget::linearGain() const
{
    if (value() == kSilenceValue) {
        return 0.0;
    }
    return std::pow(10.0, static_cast<double>(value()) / (20.0 * kTenthsPerDb));
}

void AudioFaderWidget::setLinearGain(double gain)
{
    int displayValue = kSilenceValue;
    if (std::isfinite(gain) && gain > 0.0) {
        displayValue = static_cast<int>(std::lround(
            20.0 * kTenthsPerDb * std::log10(gain)));
    }

    setValue(std::clamp(displayValue, minimum(), maximum()));
}

void AudioFaderWidget::setExtendedTopRange(bool extended)
{
    if (extendedTopRange_ == extended) {
        return;
    }
    extendedTopRange_ = extended;
    update();
}

void AudioFaderWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool hardwareSkin =
        vtrpon::currentControllerSkinUsesHardwareEffects();
    const bool masterFader = role_ == Role::Master;

    const QRect track = trackRect();
    const int trackCenterX = track.center().x();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(hardwareSkin ? QStringLiteral("#030506")
                                         : QStringLiteral("#0a0b0c")));
    const int trackOuterX = vtrpon::scalePx(3);
    const int trackOuterY = vtrpon::scalePx(2);
    const qreal trackOuterRadius = vtrpon::scalePx(3);
    painter.drawRoundedRect(
        track.adjusted(-trackOuterX, -trackOuterY,
                       trackOuterX, trackOuterY),
        trackOuterRadius, trackOuterRadius);

    QLinearGradient railGradient(track.left(), 0, track.right(), 0);
    railGradient.setColorAt(0.0, QColor(QStringLiteral("#020303")));
    railGradient.setColorAt(0.42,
                            QColor(hardwareSkin ? QStringLiteral("#4a5357")
                                                : QStringLiteral("#292d2f")));
    railGradient.setColorAt(0.55, QColor(QStringLiteral("#101315")));
    railGradient.setColorAt(1.0, QColor(QStringLiteral("#020303")));
    painter.setBrush(railGradient);
    const qreal trackRadius = vtrpon::scalePx(2);
    painter.drawRoundedRect(track, trackRadius, trackRadius);
    painter.setPen(QPen(QColor(hardwareSkin ? QStringLiteral("#7c8588")
                                            : QStringLiteral("#565b5e")),
                        vtrpon::uiScale()));
    const int trackLineInset = vtrpon::scalePx(2);
    painter.drawLine(trackCenterX, track.top() + trackLineInset,
                     trackCenterX, track.bottom() - trackLineInset);

    struct ScaleMark {
        int value;
        const char *label;
        bool unity;
    };
    const ScaleMark marks[] = {
        {60, "+6", false}, {30, "+3", false}, {0, "0", true},
        {-30, "-3", false}, {-60, "-6", false}, {-100, "-10", false},
        {-200, "-20", false}, {-300, "-30", false}, {-400, "-40", false},
        {-500, "-50", false}, {kSilenceValue, nullptr, false},
    };

    QFont scaleFont = painter.font();
    scaleFont.setPixelSize(vtrpon::scalePx(10));
    scaleFont.setBold(false);
    scaleFont.setLetterSpacing(QFont::PercentageSpacing, 100.0);
    painter.setFont(scaleFont);
    int scaleLabelWidth = 0;
    for (const ScaleMark &mark : marks) {
        const QString label = mark.value == kSilenceValue
            ? QString(QChar(0x221e))
            : QString::fromLatin1(mark.label);
        scaleLabelWidth = (std::max)(
            scaleLabelWidth, painter.fontMetrics().horizontalAdvance(label));
    }
    for (const ScaleMark &mark : marks) {
        const int y = yForValue(mark.value);
        const QColor color = mark.unity
            ? QColor(hardwareSkin ? QStringLiteral("#f0e9d8")
                                  : QStringLiteral("#c9cdcf"))
            : QColor(hardwareSkin ? QStringLiteral("#9ca6a8")
                                  : QStringLiteral("#888f92"));
        painter.setPen(QPen(color, vtrpon::uiScale()));
        painter.drawLine(vtrpon::scalePx(mark.unity ? 2 : 5), y,
                         track.left() - vtrpon::scalePx(5), y);
        painter.drawLine(track.right() + vtrpon::scalePx(4), y,
                         track.right() + vtrpon::scalePx(8), y);
        painter.drawText(QRect(track.right() + vtrpon::scalePx(10),
                               y - vtrpon::scalePx(7),
                               scaleLabelWidth, vtrpon::scalePx(14)),
                         Qt::AlignRight | Qt::AlignVCenter,
                         mark.value == kSilenceValue
                             ? QString(QChar(0x221e))
                             : QString::fromLatin1(mark.label));
    }

    const QRect cap = faderCapRect();
    painter.save();
    const bool itemFader = !masterFader;
    const bool blueCap = property("blueFaderCap").toBool();
    const QColor accent(hardwareSkin
        ? (masterFader ? QStringLiteral("#ff645b")
                       : QStringLiteral("#e8eceb"))
        : (itemFader ? QStringLiteral("#66b889")
                     : QStringLiteral("#e0a05a")));

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 105));
    const qreal capShadowOffset = vtrpon::scalePx(2);
    const qreal capOuterRadius = vtrpon::scalePx(6);
    painter.drawRoundedRect(
        QRectF(cap).translated(0.0, capShadowOffset),
        capOuterRadius, capOuterRadius);

    QLinearGradient knobGradient(cap.topLeft(), cap.bottomLeft());
    if (blueCap) {
        knobGradient.setColorAt(0.0, QColor(QStringLiteral("#90c8ec")));
        knobGradient.setColorAt(0.2, QColor(QStringLiteral("#62a9d8")));
        knobGradient.setColorAt(0.72, QColor(QStringLiteral("#3983b5")));
        knobGradient.setColorAt(1.0, QColor(QStringLiteral("#24577c")));
    } else if (hardwareSkin && masterFader) {
        knobGradient.setColorAt(0.0, QColor(QStringLiteral("#ff7065")));
        knobGradient.setColorAt(0.18, QColor(QStringLiteral("#d83936")));
        knobGradient.setColorAt(0.72, QColor(QStringLiteral("#9a171b")));
        knobGradient.setColorAt(1.0, QColor(QStringLiteral("#5e0c10")));
    } else if (hardwareSkin) {
        knobGradient.setColorAt(0.0, QColor(QStringLiteral("#f2f0e9")));
        knobGradient.setColorAt(0.2, QColor(QStringLiteral("#c9cbc7")));
        knobGradient.setColorAt(0.72, QColor(QStringLiteral("#8f9492")));
        knobGradient.setColorAt(1.0, QColor(QStringLiteral("#555d5e")));
    } else {
        knobGradient.setColorAt(0.0, QColor(QStringLiteral("#373d41")));
        knobGradient.setColorAt(1.0, QColor(QStringLiteral("#252a2d")));
    }
    painter.setBrush(knobGradient);
    painter.setPen(QPen(
        QColor(blueCap ? QStringLiteral("#acd9f3") : hardwareSkin
                   ? (masterFader ? QStringLiteral("#ff8c82")
                                  : QStringLiteral("#f6f3e9"))
                   : QStringLiteral("#4d565b")),
        vtrpon::uiScale()));
    const qreal capRadius = vtrpon::scalePx(5);
    const qreal halfStroke = 0.5 * vtrpon::uiScale();
    painter.drawRoundedRect(QRectF(cap).adjusted(halfStroke, halfStroke,
                                               -halfStroke, -halfStroke),
                            capRadius, capRadius);

    if (hardwareSkin) {
        painter.setPen(QPen(blueCap ? QColor(QStringLiteral("#285d80"))
                                   : masterFader ? QColor(QStringLiteral("#6f1014"))
                                       : QColor(QStringLiteral("#6d7473")),
                            vtrpon::uiScale()));
        const int gripInset = vtrpon::scalePx(3);
        for (int offset : {-7, -4, 4, 7}) {
            const qreal y = cap.center().y() + vtrpon::scalePx(offset);
            painter.drawLine(QPointF(cap.left() + gripInset, y),
                             QPointF(cap.right() - gripInset, y));
        }
    }

    const qreal positionLineInset = vtrpon::scalePx(3);
    const qreal positionLineHalfHeight = vtrpon::scalePx(2);
    const QRectF positionLine(cap.left() + positionLineInset,
                              cap.center().y() - positionLineHalfHeight,
                              cap.width() - 2.0 * positionLineInset,
                              2.0 * positionLineHalfHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, hardwareSkin ? 155 : 90));
    const qreal positionLineRadius = vtrpon::scalePx(2);
    painter.drawRoundedRect(
        positionLine.translated(0.0, vtrpon::scalePx(1)),
        positionLineRadius, positionLineRadius);
    painter.setBrush(hardwareSkin
                         ? QColor(masterFader ? QStringLiteral("#fff0e3")
                                              : QStringLiteral("#263033"))
                         : accent);
    painter.drawRoundedRect(positionLine,
                            positionLineRadius, positionLineRadius);

    painter.setPen(QPen(hardwareSkin
                            ? QColor(masterFader
                                         ? QStringLiteral("#ffffff")
                                         : QStringLiteral("#f8f6ec"))
                            : accent.lighter(125),
                        vtrpon::uiScale()));
    const qreal positionLineHighlightInset = vtrpon::scalePx(2);
    painter.drawLine(
        QPointF(positionLine.left() + positionLineHighlightInset,
                positionLine.top()),
        QPointF(positionLine.right() - positionLineHighlightInset,
                positionLine.top()));
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(QStringLiteral("#90caf9")),
                            vtrpon::scalePx(2)));
        painter.drawRoundedRect(
            QRectF(cap).adjusted(-vtrpon::scalePx(2),
                                 -vtrpon::scalePx(2),
                                 vtrpon::scalePx(2),
                                 vtrpon::scalePx(2)),
            capRadius + vtrpon::scalePx(2),
            capRadius + vtrpon::scalePx(2));
    }
    painter.restore();

    const int readoutWidth = vtrpon::scalePx(44);
    const QRect readout(track.center().x() - readoutWidth / 2,
                        height() - vtrpon::scalePx(31),
                        readoutWidth, vtrpon::scalePx(22));
    painter.setPen(QPen(QColor(hardwareSkin ? QStringLiteral("#596368")
                                            : QStringLiteral("#292c2e")),
                        vtrpon::uiScale()));
    painter.setBrush(QColor(hardwareSkin ? QStringLiteral("#030606")
                                         : QStringLiteral("#0b0c0d")));
    const qreal readoutRadius = vtrpon::scalePx(3);
    painter.drawRoundedRect(readout, readoutRadius, readoutRadius);
    QFont readoutFont = painter.font();
    readoutFont.setPixelSize(vtrpon::scalePx(10));
    readoutFont.setBold(true);
    painter.setFont(readoutFont);
    painter.setPen(QColor(hardwareSkin
                              ? (masterFader ? QStringLiteral("#ffd0cb")
                                             : QStringLiteral("#e7eee9"))
                              : (itemFader ? QStringLiteral("#b8cbbb")
                                           : QStringLiteral("#c5c8ca"))));
    painter.drawText(readout, Qt::AlignCenter, valueText(value()));
}

void AudioFaderWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QSlider::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    const QRect cap = faderCapRect();
    dragging_ = true;
    unityDetentLatched_ = value() == kUnityValue;
    dragOffsetY_ = cap.contains(event->position().toPoint())
        ? event->position().toPoint().y() - cap.center().y()
        : 0;
    if (!cap.contains(event->position().toPoint())) {
        setValueFromY(event->position().toPoint().y());
    }
    event->accept();
}

void AudioFaderWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging_) {
        QSlider::mouseMoveEvent(event);
        return;
    }
    setValueFromY(event->position().toPoint().y() - dragOffsetY_);
    event->accept();
}

void AudioFaderWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        unityDetentLatched_ = false;
        event->accept();
        return;
    }
    QSlider::mouseReleaseEvent(event);
}

void AudioFaderWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        unityDetentLatched_ = true;
        requestUserValue(kUnityValue);
        event->accept();
        return;
    }
    QSlider::mouseDoubleClickEvent(event);
}

QRect AudioFaderWidget::trackRect() const
{
    const int trackWidth = vtrpon::scalePx(5);
    const int trackTop = vtrpon::scalePx(extendedTopRange_ ? 22 : 43);
    const int trackBottom = (std::max)(
        trackTop + 1, height() - vtrpon::scalePx(55));
    const int trackCenterX = vtrpon::scalePx(22);
    return QRect(trackCenterX - trackWidth / 2, trackTop,
                 trackWidth, trackBottom - trackTop + 1);
}

QRect AudioFaderWidget::faderCapRect() const
{
    const bool hardwareSkin =
        vtrpon::currentControllerSkinUsesHardwareEffects();
    const int capWidth = vtrpon::scalePx(20);
    const int capHeight = vtrpon::scalePx(hardwareSkin ? 40 : 34);
    const int centerX = trackRect().center().x();
    return QRect(centerX - capWidth / 2,
                 yForValue(value()) - capHeight / 2,
                 capWidth, capHeight);
}

int AudioFaderWidget::yForValue(int sliderValue) const
{
    const QRect track = trackRect();
    const int travel = (std::max)(1, track.height() - 1);
    const double travelFromBottom = vtrpon::audioFaderScale::positionForValue(sliderValue);
    return track.bottom() - static_cast<int>(std::lround(
        std::clamp(travelFromBottom, 0.0, 1.0) * travel));
}

void AudioFaderWidget::setValueFromY(int y)
{
    const QRect track = trackRect();
    const int travel = (std::max)(1, track.height() - 1);
    const int boundedY = std::clamp(y, track.top(), track.bottom());
    const int unityY = yForValue(kUnityValue);
    const int unityDistance = std::abs(boundedY - unityY);
    if (unityDetentLatched_) {
        if (unityDistance <= vtrpon::scalePx(kUnityDetentExitPx)) {
            requestUserValue(kUnityValue);
            return;
        }
        unityDetentLatched_ = false;
    }
    if (unityDistance <= vtrpon::scalePx(kUnityDetentEnterPx)) {
        unityDetentLatched_ = true;
        requestUserValue(kUnityValue);
        return;
    }

    const double travelFromBottom = std::clamp(
        static_cast<double>(track.bottom() - boundedY) / travel, 0.0, 1.0);
    const int sliderValue = vtrpon::audioFaderScale::valueForPosition(travelFromBottom);
    requestUserValue(std::clamp(sliderValue, minimum(), maximum()));
}

void AudioFaderWidget::updateValueDescription()
{
    if (role_ == Role::NominalItem) {
        setAccessibleDescription(vtrpon::uiText("audio_fader.nominal_description")
                                     .arg(valueText(value())));
        setToolTip(vtrpon::uiText("audio_fader.nominal_tooltip")
                       .arg(valueText(value())));
    } else if (role_ == Role::Item) {
        setAccessibleDescription(vtrpon::uiText("audio_fader.item_description")
                                     .arg(valueText(value())));
        setToolTip(vtrpon::uiText("audio_fader.item_tooltip")
                       .arg(valueText(value())));
    } else {
        setAccessibleDescription(vtrpon::uiText("audio_fader.master_description")
                                     .arg(valueText(value())));
        setToolTip(vtrpon::uiText("audio_fader.master_tooltip")
                       .arg(valueText(value())));
    }
    update();
}
