#include "controller_scaling.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace vtrpon {
namespace {

qreal gUiScale = 1.0;
int gUiTextSize = 0;

QString scaleStyleDimensions(const QString &styleSheet, qreal scale)
{
    static const QRegularExpression dimensionPattern(
        QStringLiteral("(-?\\d+(?:\\.\\d+)?)(px|pt)\\b"));

    QString result;
    qsizetype position = 0;
    QRegularExpressionMatchIterator matches =
        dimensionPattern.globalMatch(styleSheet);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += styleSheet.sliced(position, match.capturedStart() - position);
        const qreal value = match.captured(1).toDouble() * scale;
        if (match.captured(2) == QStringLiteral("pt")) {
            // Keep fractional point sizes; rounding before scaling distorts text.
            result += QString::number(value, 'f', 4);
        } else {
            const int rounded = qRound(value);
            result += QString::number(value > 0 ? std::max(1, rounded)
                                      : value < 0 ? std::min(-1, rounded) : 0);
        }
        result += match.captured(2);
        position = match.capturedEnd();
    }
    result += styleSheet.sliced(position);
    return result;
}

} // namespace

qreal controllerScaleForGeometry(const QSizeF &screenSize,
                                 const QSizeF &availableSize)
{
    if (screenSize.isEmpty() || availableSize.isEmpty()) {
        return 1.0;
    }
    // Preserve the composition of a 1920x1080 desktop, including at 4K/100%.
    // Do not multiply by devicePixelRatio here: Qt applies it when drawing.
    // Use the shorter axis so 16:10 and ultrawide screens do not stretch the UI.
    const qreal densityScale = std::min(screenSize.width() / 1920.0,
                                       screenSize.height() / 1080.0);
    // Retain the existing content-fit guard for taskbars and reduced work areas.
    const qreal fitScale = std::min(availableSize.width() / 1886.0,
                                   availableSize.height() / 805.0);
    return std::min(densityScale, fitScale);
}

void setUiScale(qreal scale)
{
    gUiScale = std::isfinite(scale) && scale > 0 ? scale : 1.0;
}

qreal uiScale()
{
    return gUiScale;
}

int scalePx(int value)
{
    const int rounded = qRound(value * gUiScale);
    return value > 0 ? std::max(1, rounded)
         : value < 0 ? std::min(-1, rounded) : 0;
}

void setUiTextSize(int size) { gUiTextSize = std::clamp(size, 0, 2); }
int uiTextSize() { return gUiTextSize; }
qreal uiTextScale() { return 1.0 + 0.2 * gUiTextSize; }
int scaleTextPx(int value) { return std::max(1, qRound(value * gUiScale * uiTextScale())); }

QSize scaleSize(int width, int height)
{
    return QSize(scalePx(width), scalePx(height));
}

QString scaledStyleSheet(const QString &styleSheet)
{
    if (gUiTextSize == 0) return scaledStyleSheet(styleSheet, gUiScale);
    // Change only font declarations; fixed control geometry remains independent.
    // DELAY is compact; large time readouts already fill their available space.
    static const QRegularExpression blocks(QStringLiteral("([^{}]+)\\{([^{}]*)\\}"));
    static const QRegularExpression fonts(QStringLiteral("(font-size\\s*:\\s*)([0-9.]+)(px|pt)"));
    QString adjusted;
    qsizetype end = 0;
    auto matches = blocks.globalMatch(styleSheet);
    while (matches.hasNext()) {
        const auto block = matches.next();
        adjusted += styleSheet.sliced(end, block.capturedStart() - end);
        QString body = block.captured(2);
        const QString selector = block.captured(1);
        if (!selector.contains(QStringLiteral("audioDelayControl")) &&
            !selector.contains(QStringLiteral("programRemainingTime")) &&
            !selector.contains(QStringLiteral("operatorTimerRemaining"))) {
            auto sizes = fonts.globalMatch(body);
            QList<QRegularExpressionMatch> replacements;
            while (sizes.hasNext()) replacements.append(sizes.next());
            for (auto it = replacements.crbegin(); it != replacements.crend(); ++it) {
                body.replace(it->capturedStart(2), it->capturedLength(2),
                             it->captured(3) == QStringLiteral("px")
                                 ? QString::number(qRound(it->captured(2).toDouble() * uiTextScale()))
                                 : QString::number(it->captured(2).toDouble() * uiTextScale(), 'f', 3));
            }
        }
        adjusted += block.captured(1) + QLatin1Char('{') + body + QLatin1Char('}');
        end = block.capturedEnd();
    }
    adjusted += styleSheet.sliced(end);
    return scaledStyleSheet(adjusted, gUiScale);
}

QString scaledStyleSheet(const QString &styleSheet, qreal scale)
{
    if (qFuzzyCompare(scale, qreal{1.0})) {
        return styleSheet;
    }
    return scaleStyleDimensions(styleSheet, scale);
}

} // namespace vtrpon
