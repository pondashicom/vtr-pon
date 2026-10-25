#include "playlist_item_delegate.h"
#include "playlist_item_display.h"
#include "playlist_model.h"
#include "controller_scaling.h"
#include "language_text.h"
#include <QApplication>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <algorithm>
#include <array>

namespace playlist_view {

QString conversionStageText(PlaylistItemConversionStage stage)
{
    switch (stage) {
    case PlaylistItemConversionStage::AnalysisWaiting:
        return vtrpon::uiText("playlist.analysis.waiting");
    case PlaylistItemConversionStage::Analyzing:
        return vtrpon::uiText("playlist.analysis.analyzing");
    case PlaylistItemConversionStage::Waiting:
        return vtrpon::uiText("playlist.conversion.waiting");
    case PlaylistItemConversionStage::Preparing:
        return vtrpon::uiText("playlist.conversion.preparing");
    case PlaylistItemConversionStage::Exporting:
        return vtrpon::uiText("playlist.conversion.exporting");
    case PlaylistItemConversionStage::Rendering:
        return vtrpon::uiText("playlist.conversion.rendering");
    case PlaylistItemConversionStage::Encoding:
        return vtrpon::uiText("playlist.conversion.encoding");
    }
    return vtrpon::uiText("playlist.conversion.preparing");
}

QString conversionStatusText(const PlaylistItemConversionState &state)
{
    const QString stage = conversionStageText(state.stage);
    if (state.stage == PlaylistItemConversionStage::AnalysisWaiting ||
        state.stage == PlaylistItemConversionStage::Analyzing) return stage;
    return vtrpon::uiText("playlist.conversion.item_status")
        .arg(stage).arg((state.progressPermille + 5) / 10);
}

QString mediaTypeName(PlaylistMediaType mediaType)
{
    switch (mediaType) {
    case PlaylistMediaType::Video:
        return vtrpon::uiText("media.video");
    case PlaylistMediaType::Audio:
        return vtrpon::uiText("media.audio");
    case PlaylistMediaType::StillImage:
        return vtrpon::uiText("media.image");
    case PlaylistMediaType::Presentation:
        return vtrpon::uiText("media.powerpoint");
    case PlaylistMediaType::Pdf:
        return vtrpon::uiText("media.pdf");
    case PlaylistMediaType::Unknown:
        return vtrpon::uiText("media.unknown");
    }
    return vtrpon::uiText("media.unknown");
}

PlaylistPowerPointMode effectivePowerPointMode(
    const PlaylistItemState &state)
{
    if (state.powerPointMode != PlaylistPowerPointMode::None) {
        return state.powerPointMode;
    }
    return state.sourceType == PlaylistSourceType::PowerPoint
            && state.mediaType == PlaylistMediaType::Presentation
        ? PlaylistPowerPointMode::NativeSlideShow
        : PlaylistPowerPointMode::None;
}

QString powerPointModeBadgeLabel(PlaylistPowerPointMode mode)
{
    switch (mode) {
    case PlaylistPowerPointMode::NativeSlideShow:
        return vtrpon::uiText("playlist.thumbnail.powerpoint.native");
    case PlaylistPowerPointMode::VideoConversion:
        return vtrpon::uiText("playlist.thumbnail.powerpoint.mp4");
    case PlaylistPowerPointMode::PngSequence:
        return vtrpon::uiText("playlist.thumbnail.powerpoint.png");
    case PlaylistPowerPointMode::None:
        break;
    }
    return {};
}

QColor powerPointModeBadgeColor(PlaylistPowerPointMode mode)
{
    switch (mode) {
    case PlaylistPowerPointMode::NativeSlideShow:
        return QColor(QStringLiteral("#5b3d9b"));
    case PlaylistPowerPointMode::VideoConversion:
        return QColor(QStringLiteral("#8b3b24"));
    case PlaylistPowerPointMode::PngSequence:
        return QColor(QStringLiteral("#2d6a4f"));
    case PlaylistPowerPointMode::None:
        break;
    }
    return {};
}

QColor playlistItemTint(PlaylistItemColor color)
{
    switch (color) {
    case PlaylistItemColor::Red:
        return QColor(255, 120, 120, 71);
    case PlaylistItemColor::Yellow:
        return QColor(255, 235, 120, 77);
    case PlaylistItemColor::Blue:
        return QColor(120, 180, 255, 71);
    case PlaylistItemColor::Green:
        return QColor(140, 230, 160, 71);
    case PlaylistItemColor::Default:
        return QColor();
    }
    return QColor();
}

QString playlistItemColorName(PlaylistItemColor color)
{
    switch (color) {
    case PlaylistItemColor::Default:
        return vtrpon::uiText("playlist.context.color.default");
    case PlaylistItemColor::Red:
        return vtrpon::uiText("playlist.context.color.red");
    case PlaylistItemColor::Yellow:
        return vtrpon::uiText("playlist.context.color.yellow");
    case PlaylistItemColor::Blue:
        return vtrpon::uiText("playlist.context.color.blue");
    case PlaylistItemColor::Green:
        return vtrpon::uiText("playlist.context.color.green");
    }
    return vtrpon::uiText("playlist.context.color.default");
}

QIcon playlistItemColorIcon(PlaylistItemColor color)
{
    QPixmap swatch(vtrpon::scaleSize(14, 14));
    swatch.fill(Qt::transparent);
    QPainter painter(&swatch);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect rect = swatch.rect().adjusted(1, 1, -2, -2);
    if (color == PlaylistItemColor::Default) {
        painter.setPen(QPen(QColor(QStringLiteral("#8b9499")), 1));
        painter.setBrush(QColor(QStringLiteral("#2b3033")));
        painter.drawRoundedRect(rect, 2, 2);
        painter.drawLine(rect.topLeft(), rect.bottomRight());
    } else {
        QColor fill = playlistItemTint(color);
        fill.setAlpha(255);
        painter.setPen(QPen(fill.darker(150), 1));
        painter.setBrush(fill);
        painter.drawRoundedRect(rect, 2, 2);
    }
    return QIcon(swatch);
}

QPixmap createThumbnailPlaceholder(const PlaylistItemState &state)
{
    QImage image(vtrpon::scalePx(kThumbnailWidth),
                 vtrpon::scalePx(kThumbnailHeight),
                 QImage::Format_RGBA8888);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor(QStringLiteral("#d4dadd")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSizeF(8.0);
    painter.setFont(font);
    painter.drawText(image.rect().adjusted(3, 3, -3, -15),
                     Qt::AlignCenter, mediaTypeName(state.mediaType));
    font.setBold(false);
    font.setPointSizeF(6.5);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#879095")));
    painter.drawText(image.rect().adjusted(3, 3, -3, -3),
                     Qt::AlignHCenter | Qt::AlignBottom,
                     vtrpon::uiText("media.generating"));
    return QPixmap::fromImage(image);
}

class PlaylistItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *view = qobject_cast<const QListView *>(parent());
        const bool tileMode = view
            && view->viewMode() == QListView::IconMode;
        const bool tilePressed = tileMode
            && view->property("pressedTileItemId").toULongLong()
                   == index.data(kItemIdRole).toULongLong();
        // Glow size is independent of card geometry and item spacing.
        const int glowMargin = vtrpon::scalePx(6);
        QRect itemRect = option.rect;
        const int separatorGap = !tileMode
                && index.data(kItemSeparatorBelowRole).toBool()
            ? vtrpon::scalePx(12) : 0;
        itemRect.setBottom(itemRect.bottom() - separatorGap);
        if (tileMode) {
            const QRect raisedRect = option.rect.adjusted(
                vtrpon::scalePx(3), vtrpon::scalePx(2),
                -vtrpon::scalePx(3), -vtrpon::scalePx(5));
            itemRect = raisedRect.translated(
                0, tilePressed ? vtrpon::scalePx(3) : 0);
            QColor shadowColor = option.palette.color(QPalette::Shadow);
            shadowColor.setAlpha(tilePressed ? 45 : 120);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(shadowColor);
            painter->drawRoundedRect(
                raisedRect.translated(0, vtrpon::scalePx(3)),
                vtrpon::scalePx(4), vtrpon::scalePx(4));
            painter->restore();
        }
        const int visualState = index.data(kItemVisualStateRole).toInt();
        const qreal onAirHighlightOpacity = std::clamp(
            index.data(kOnAirHighlightOpacityRole).toReal(), 0.0, 1.0);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool playoutState =
            (visualState & (ItemVisualOnAir | ItemVisualDsk |
                            ItemVisualBgmPlay)) != 0
            || onAirHighlightOpacity > 0.0;
        QStyleOptionViewItem background(option);
        background.rect = itemRect;
        initStyleOption(&background, index);
        const QIcon status = background.icon;
        background.text.clear();
        background.icon = QIcon();
        if (selected) {
            background.state.setFlag(QStyle::State_Selected, false);
        }
        QStyle *style = background.widget
            ? background.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter,
                           background.widget);

        painter->save();
        const QColor itemTint = playlistItemTint(
            static_cast<PlaylistItemColor>(
                index.data(kItemColorRole).toInt()));
        if (itemTint.isValid()) {
            painter->fillRect(itemRect.adjusted(1, 1, -1, -1), itemTint);
        }
        QColor secondaryTint;
        if ((visualState & ItemVisualDsk) != 0) {
            secondaryTint = QColor(QStringLiteral("#357eb5"));
        }
        if (secondaryTint.isValid() && onAirHighlightOpacity < 1.0) {
            secondaryTint.setAlphaF((55.0 / 255.0)
                                    * (1.0 - onAirHighlightOpacity));
            painter->fillRect(itemRect.adjusted(1, 1, -1, -1),
                              secondaryTint);
        }
        const auto drawNeonOutline = [&](int inset, QColor color,
                                         qreal opacity = 1.0) {
            painter->save();
            QPainterPath outerArea;
            // Use the existing inter-item gap; never shrink the card for light
            // or paint over a neighbouring card.
            const int gap = view ? view->spacing() : 0;
            outerArea.addRect(QRectF(option.rect).adjusted(
                -gap, -gap, gap, gap));
            QPainterPath cardSurface;
            cardSurface.addRoundedRect(QRectF(itemRect), 3, 3);
            painter->setClipPath(outerArea.subtracted(cardSurface),
                                Qt::IntersectClip);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setBrush(Qt::NoBrush);
            const QRectF outline = QRectF(itemRect).adjusted(
                inset, inset, -inset, -inset);
            // Only the light escaping from behind the opaque card is visible.
            for (const auto &[width, alpha] :
                 std::array<std::pair<qreal, qreal>, 7>{{
                     {12.0, 0.025}, {10.0, 0.035}, {8.0, 0.05},
                     {6.0, 0.075}, {4.0, 0.11}, {2.5, 0.18},
                     {1.0, 0.28}}}) {
                color.setAlphaF(alpha * opacity);
                painter->setPen(QPen(color, width * glowMargin / 5.0));
                painter->drawRoundedRect(outline, 3, 3);
            }
            painter->restore();
        };
        const bool editOutline = (visualState & ItemVisualEdit) != 0;
        const bool onAirOutlineActive = (visualState & ItemVisualOnAir) != 0
            || onAirHighlightOpacity > 0.0;
        if (onAirHighlightOpacity > 0.0) {
            // ON AIR owns the outer frame; selection is a secondary inner line.
            drawNeonOutline(0, QColor(QStringLiteral("#ed555c")),
                            onAirHighlightOpacity);
        }

        int stateRailX = itemRect.left() + 2;
        const auto drawStateRail = [&](bool active, QColor color,
                                       qreal opacity = 1.0, int width = 3) {
            if (!active) {
                return;
            }
            color.setAlphaF(color.alphaF() * opacity);
            painter->fillRect(QRect(stateRailX, itemRect.top() + 3, width,
                                    itemRect.height() - 6), color);
            stateRailX += width + 1;
        };
        drawStateRail((visualState & ItemVisualOnAir) != 0
                          || onAirHighlightOpacity > 0.0,
                      QColor(QStringLiteral("#ed555c")),
                      onAirHighlightOpacity * 0.65);
        drawStateRail((visualState & ItemVisualDsk) != 0,
                      QColor(QStringLiteral("#65a7d8")));
        drawStateRail((visualState & ItemVisualEdit) != 0,
                      QColor(QStringLiteral("#65d36b")),
                      onAirOutlineActive ? 0.3 : 0.65,
                      onAirOutlineActive ? 1 : 3);
        drawStateRail((visualState & ItemVisualBgmPlay) != 0,
                      QColor(QStringLiteral("#c99a45")));
        if (selected || editOutline) {
            if (onAirOutlineActive) {
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                QColor selectionColor(QStringLiteral("#5fc36a"));
                selectionColor.setAlphaF(0.3);
                painter->setPen(QPen(selectionColor, 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(
                    QRectF(itemRect).adjusted(5, 5, -5, -5), 2, 2);
                painter->restore();
            } else {
                drawNeonOutline(0, QColor(QStringLiteral("#5fc36a")));
            }
        }

        if (tileMode) {
            const int padding = vtrpon::scalePx(6);
            const int footerHeight = vtrpon::scalePx(25);
            const int footerGap = vtrpon::scalePx(4);
            const QRect content = itemRect.adjusted(
                padding, padding, -padding, -padding);
            const int thumbnailHeight = (std::max)(
                0, content.height() - footerHeight - footerGap);
            const QRect thumbnailRect(content.left(), content.top(),
                                      content.width(), thumbnailHeight);
            const QRect footerRect(
                content.left(), thumbnailRect.bottom() + 1 + footerGap,
                content.width(), footerHeight);
            const QPixmap thumbnail =
                index.data(kThumbnailRole).value<QPixmap>();
            const bool mediaOffline =
                index.data(kItemMediaOfflineRole).toBool();
            const bool conversionActive =
                index.data(kItemConversionActiveRole).toBool();
            const int conversionProgress = std::clamp(
                index.data(kItemConversionProgressRole).toInt(), -1, 1000);

            if (mediaOffline && !conversionActive) {
                painter->fillRect(thumbnailRect,
                                  QColor(QStringLiteral("#c62828")));
                painter->setPen(Qt::white);
                QFont offlineFont = background.font;
                offlineFont.setBold(true);
                offlineFont.setPixelSize(vtrpon::scaleTextPx(11));
                painter->setFont(offlineFont);
                painter->drawText(thumbnailRect.adjusted(4, 4, -4, -4),
                                  Qt::AlignCenter,
                                  QStringLiteral("MEDIA OFFLINE"));
            } else {
                painter->fillRect(thumbnailRect, Qt::black);
                if (!thumbnail.isNull()) {
                    painter->drawPixmap(thumbnailRect, thumbnail,
                                        thumbnail.rect());
                }
            }
            painter->setPen(QColor(QStringLiteral("#454b4e")));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(thumbnailRect.adjusted(0, 0, -1, -1));

            if (conversionActive) {
                painter->fillRect(thumbnailRect.adjusted(1, 1, -1, -1),
                                  QColor(8, 12, 16, 218));
                QFont conversionFont = background.font;
                conversionFont.setBold(true);
                conversionFont.setPixelSize(vtrpon::scaleTextPx(9));
                painter->setFont(conversionFont);
                painter->setPen(Qt::white);
                painter->drawText(
                    thumbnailRect.adjusted(vtrpon::scalePx(5),
                                           vtrpon::scalePx(5),
                                           -vtrpon::scalePx(5),
                                           -vtrpon::scalePx(12)),
                    Qt::AlignCenter | Qt::TextWordWrap,
                    conversionProgress < 0
                        ? index.data(kItemConversionStageRole).toString()
                        : vtrpon::uiText("playlist.conversion.item_status")
                        .arg(index.data(kItemConversionStageRole).toString())
                        .arg((conversionProgress + 5) / 10));
                if (conversionProgress >= 0) {
                    const QRect progressTrack(
                        thumbnailRect.left() + vtrpon::scalePx(5),
                        thumbnailRect.bottom() - vtrpon::scalePx(8),
                        thumbnailRect.width() - vtrpon::scalePx(10),
                        vtrpon::scalePx(4));
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(QColor(255, 255, 255, 55));
                    painter->drawRoundedRect(progressTrack, vtrpon::scalePx(2),
                                             vtrpon::scalePx(2));
                    QRect progressFill = progressTrack;
                    progressFill.setWidth(progressTrack.width()
                                          * conversionProgress / 1000);
                    painter->setBrush(
                        background.palette.color(QPalette::Highlight));
                    painter->drawRoundedRect(progressFill, vtrpon::scalePx(2),
                                             vtrpon::scalePx(2));
                }
            }

            QFont numberFont = background.font;
            numberFont.setBold(true);
            numberFont.setPixelSize(vtrpon::scaleTextPx(11));
            painter->setFont(numberFont);
            const QString numberText =
                QString::number(index.data(kItemNumberRole).toInt());
            const int numberWidth = (std::max)(
                vtrpon::scalePx(24),
                painter->fontMetrics().horizontalAdvance(numberText)
                    + vtrpon::scalePx(10));
            const QRect numberRect(
                thumbnailRect.left() + vtrpon::scalePx(3),
                thumbnailRect.top() + vtrpon::scalePx(3), numberWidth,
                vtrpon::scalePx(18));
            painter->setPen(QColor(255, 255, 255, 205));
            painter->setBrush(QColor(10, 14, 16, 205));
            painter->drawRoundedRect(numberRect, vtrpon::scalePx(2),
                                     vtrpon::scalePx(2));
            painter->setPen(Qt::white);
            painter->drawText(numberRect, Qt::AlignCenter, numberText);

            int stateBadgeTop = thumbnailRect.top() + vtrpon::scalePx(3);
            const auto drawStateBadge = [&](bool active,
                                            const QString &label,
                                            const QColor &color,
                                            qreal opacity = 1.0) {
                if (!active) {
                    return;
                }
                QFont badgeFont = background.font;
                badgeFont.setBold(true);
                badgeFont.setPixelSize(vtrpon::scalePx(8));
                painter->setFont(badgeFont);
                const int width =
                    painter->fontMetrics().horizontalAdvance(label)
                    + vtrpon::scalePx(8);
                const QRect badgeRect(
                    thumbnailRect.right() - width - vtrpon::scalePx(3),
                    stateBadgeTop, width, vtrpon::scalePx(15));
                QColor badgeBackground = color;
                badgeBackground.setAlphaF((220.0 / 255.0) * opacity);
                QColor badgeOutline = color.lighter(155);
                badgeOutline.setAlphaF(opacity);
                painter->setPen(badgeOutline);
                painter->setBrush(badgeBackground);
                painter->drawRoundedRect(badgeRect, vtrpon::scalePx(2),
                                         vtrpon::scalePx(2));
                QColor badgeText(Qt::white);
                badgeText.setAlphaF(opacity);
                painter->setPen(badgeText);
                painter->drawText(badgeRect, Qt::AlignCenter, label);
                stateBadgeTop = badgeRect.bottom() + vtrpon::scalePx(3);
            };
            drawStateBadge((visualState & ItemVisualOnAir) != 0
                               || onAirHighlightOpacity > 0.0,
                           vtrpon::uiText("playlist.on_air"),
                           QColor(QStringLiteral("#a92e36")),
                           onAirHighlightOpacity);
            drawStateBadge((visualState & ItemVisualDsk) != 0,
                           QStringLiteral("DSK"),
                           QColor(QStringLiteral("#357eb5")));
            drawStateBadge((visualState & ItemVisualEdit) != 0,
                           vtrpon::uiText("info.source.edit"),
                           QColor(QStringLiteral("#377c43")));
            drawStateBadge((visualState & ItemVisualBgmPlay) != 0,
                           QStringLiteral("BGM PLAY"),
                           QColor(QStringLiteral("#8a6429")));

            if (index.data(kItemBgmModeRole).toBool()) {
                QFont bgmFont = background.font;
                bgmFont.setBold(true);
                bgmFont.setPixelSize(vtrpon::scalePx(8));
                painter->setFont(bgmFont);
                const QRect bgmRect(
                    thumbnailRect.left() + vtrpon::scalePx(3),
                    thumbnailRect.bottom() - vtrpon::scalePx(17),
                    vtrpon::scalePx(31), vtrpon::scalePx(14));
                painter->setPen(QPen(QColor(QStringLiteral("#f1c875")), 1));
                painter->setBrush(QColor(92, 65, 24, 230));
                painter->drawRoundedRect(bgmRect, vtrpon::scalePx(2),
                                         vtrpon::scalePx(2));
                painter->setPen(Qt::white);
                painter->drawText(bgmRect, Qt::AlignCenter,
                                  QStringLiteral("BGM"));
            }

            const auto powerPointMode =
                static_cast<PlaylistPowerPointMode>(
                    index.data(kPowerPointModeRole).toInt());
            const QString powerPointModeLabel =
                powerPointModeBadgeLabel(powerPointMode);
            const QColor powerPointModeColor =
                powerPointModeBadgeColor(powerPointMode);
            if (!conversionActive && !powerPointModeLabel.isEmpty()
                && powerPointModeColor.isValid()) {
                QFont badgeFont = background.font;
                badgeFont.setBold(true);
                badgeFont.setPixelSize(vtrpon::scalePx(8));
                painter->setFont(badgeFont);
                const int width =
                    painter->fontMetrics().horizontalAdvance(
                        powerPointModeLabel) + vtrpon::scalePx(7);
                const QRect badgeRect(
                    thumbnailRect.right() - width - vtrpon::scalePx(3),
                    thumbnailRect.bottom() - vtrpon::scalePx(17), width,
                    vtrpon::scalePx(14));
                painter->setPen(
                    QPen(powerPointModeColor.lighter(170), 1));
                QColor badgeBackground = powerPointModeColor;
                badgeBackground.setAlpha(235);
                painter->setBrush(badgeBackground);
                painter->drawRoundedRect(badgeRect, vtrpon::scalePx(2),
                                         vtrpon::scalePx(2));
                painter->setPen(Qt::white);
                painter->drawText(badgeRect, Qt::AlignCenter,
                                  powerPointModeLabel);
            }

            const bool selectedInHardwareSkin =
                selected
                && qApp->property("controllerSkinKey").toString()
                       == QStringLiteral("hardware");
            const QColor textColor = selectedInHardwareSkin
                ? QColor(Qt::white)
                : background.palette.color(
                      selected && !playoutState
                          ? QPalette::HighlightedText : QPalette::Text);
            const int statusSize = vtrpon::scalePx(12);
            const QRect statusRect(
                footerRect.right() - statusSize + 1,
                footerRect.top() + (footerRect.height() - statusSize) / 2,
                statusSize, statusSize);
            if (!status.isNull()) {
                status.paint(painter, statusRect, Qt::AlignCenter,
                             QIcon::Normal, QIcon::On);
            }
            const QRect nameRect = status.isNull()
                ? footerRect
                : footerRect.adjusted(0, 0,
                                      -statusSize - vtrpon::scalePx(4), 0);
            QFont titleFont = background.font;
            titleFont.setBold(true);
            titleFont.setPixelSize(vtrpon::scaleTextPx(12));
            painter->setFont(titleFont);
            painter->setPen(textColor);
            painter->drawText(
                nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                painter->fontMetrics().elidedText(
                    index.data(kItemNameRole).toString(), Qt::ElideMiddle,
                    nameRect.width()));

            if (index.data(kItemSeparatorBelowRole).toBool()) {
                QColor separatorColor = textColor;
                separatorColor.setAlpha(190);
                painter->setPen(QPen(separatorColor, vtrpon::scalePx(2),
                                     Qt::SolidLine, Qt::RoundCap));
                painter->drawLine(itemRect.left() + vtrpon::scalePx(6),
                                  itemRect.bottom() - 1,
                                  itemRect.right() - vtrpon::scalePx(6),
                                  itemRect.bottom() - 1);
            }
            painter->restore();
            return;
        }

        const int horizontalPadding = vtrpon::scalePx(7);
        const int verticalPadding = vtrpon::scalePx(4);
        const QRect content = itemRect.adjusted(
            horizontalPadding, verticalPadding,
            -horizontalPadding, -verticalPadding);
        const int thumbnailWidth = vtrpon::scalePx(kThumbnailWidth);
        const int thumbnailHeight = vtrpon::scalePx(kThumbnailHeight);
        const QRect thumbnailRect(
            content.left(),
            content.top() + (content.height() - thumbnailHeight) / 2,
            thumbnailWidth, thumbnailHeight);
        const QPixmap thumbnail =
            index.data(kThumbnailRole).value<QPixmap>();
        const bool mediaOffline = index.data(kItemMediaOfflineRole).toBool();
        const bool conversionActive =
            index.data(kItemConversionActiveRole).toBool();
        const int conversionProgress = std::clamp(
            index.data(kItemConversionProgressRole).toInt(), -1, 1000);
        if (mediaOffline && !conversionActive) {
            painter->fillRect(thumbnailRect,
                              QColor(QStringLiteral("#c62828")));
            painter->setPen(Qt::white);
            QFont offlineFont = painter->font();
            offlineFont.setBold(true);
            offlineFont.setPixelSize(vtrpon::scaleTextPx(12));
            painter->setFont(offlineFont);
            painter->drawText(thumbnailRect.adjusted(4, 4, -4, -4),
                              Qt::AlignCenter,
                              QStringLiteral("MEDIA OFFLINE"));
        } else {
            painter->fillRect(thumbnailRect, Qt::black);
            if (!thumbnail.isNull()) {
                painter->drawPixmap(thumbnailRect, thumbnail,
                                    thumbnail.rect());
            }
        }
        painter->setPen(QColor(QStringLiteral("#454b4e")));
        painter->drawRect(thumbnailRect.adjusted(0, 0, -1, -1));

        if (conversionActive) {
            painter->fillRect(thumbnailRect.adjusted(1, 1, -1, -1),
                              QColor(8, 12, 16, 218));
            QFont conversionFont = background.font;
            conversionFont.setBold(true);
            conversionFont.setPixelSize(vtrpon::scaleTextPx(10));
            painter->setFont(conversionFont);
            painter->setPen(Qt::white);
            const QString conversionText =
                conversionProgress < 0
                    ? index.data(kItemConversionStageRole).toString()
                    : vtrpon::uiText("playlist.conversion.item_status")
                    .arg(index.data(kItemConversionStageRole).toString())
                    .arg((conversionProgress + 5) / 10);
            painter->drawText(
                thumbnailRect.adjusted(vtrpon::scalePx(5),
                                       vtrpon::scalePx(5),
                                       -vtrpon::scalePx(5),
                                       -vtrpon::scalePx(12)),
                Qt::AlignCenter | Qt::TextWordWrap, conversionText);

            if (conversionProgress >= 0) {
                const QRect progressTrack(
                    thumbnailRect.left() + vtrpon::scalePx(5),
                    thumbnailRect.bottom() - vtrpon::scalePx(9),
                    thumbnailRect.width() - vtrpon::scalePx(10),
                    vtrpon::scalePx(5));
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(255, 255, 255, 55));
                painter->drawRoundedRect(progressTrack, vtrpon::scalePx(2),
                                         vtrpon::scalePx(2));
                QRect progressFill = progressTrack;
                progressFill.setWidth(progressTrack.width() * conversionProgress /
                                      1000);
                painter->setBrush(background.palette.color(QPalette::Highlight));
                painter->drawRoundedRect(progressFill, vtrpon::scalePx(2),
                                         vtrpon::scalePx(2));
            }
        }

        const auto powerPointMode = static_cast<PlaylistPowerPointMode>(
            index.data(kPowerPointModeRole).toInt());
        const QString powerPointModeLabel =
            powerPointModeBadgeLabel(powerPointMode);
        const QColor powerPointModeColor =
            powerPointModeBadgeColor(powerPointMode);
        if (!conversionActive && !powerPointModeLabel.isEmpty() &&
            powerPointModeColor.isValid()) {
            QFont badgeFont = background.font;
            badgeFont.setBold(true);
            badgeFont.setPixelSize(vtrpon::scalePx(9));
            painter->setFont(badgeFont);
            const int badgeHeight = vtrpon::scalePx(15);
            const int maximumBadgeWidth = thumbnailRect.width()
                - vtrpon::scalePx(6);
            const int badgeWidth = (std::min)(
                maximumBadgeWidth,
                painter->fontMetrics().horizontalAdvance(powerPointModeLabel)
                    + vtrpon::scalePx(8));
            const QRect badgeRect(
                thumbnailRect.right() - badgeWidth - vtrpon::scalePx(2),
                thumbnailRect.top() + vtrpon::scalePx(2),
                badgeWidth, badgeHeight);
            QColor badgeBackground = powerPointModeColor;
            badgeBackground.setAlpha(235);
            painter->setPen(QPen(powerPointModeColor.lighter(170), 1));
            painter->setBrush(badgeBackground);
            painter->drawRoundedRect(badgeRect, vtrpon::scalePx(2),
                                     vtrpon::scalePx(2));
            painter->setPen(Qt::white);
            painter->drawText(badgeRect.adjusted(vtrpon::scalePx(3), 0,
                                                 -vtrpon::scalePx(3), 0),
                              Qt::AlignCenter, powerPointModeLabel);
        }

        if (index.data(kItemBgmModeRole).toBool()) {
            QFont bgmFont = background.font;
            bgmFont.setBold(true);
            bgmFont.setPixelSize(vtrpon::scalePx(9));
            painter->setFont(bgmFont);
            const QRect bgmRect(
                thumbnailRect.left() + vtrpon::scalePx(3),
                thumbnailRect.bottom() - vtrpon::scalePx(18),
                vtrpon::scalePx(35), vtrpon::scalePx(16));
            painter->setPen(QPen(QColor(QStringLiteral("#f1c875")), 1));
            painter->setBrush(QColor(92, 65, 24, 230));
            painter->drawRoundedRect(bgmRect, vtrpon::scalePx(2),
                                     vtrpon::scalePx(2));
            painter->setPen(Qt::white);
            painter->drawText(bgmRect, Qt::AlignCenter,
                              QStringLiteral("BGM"));
        }

        const bool selectedInHardwareSkin =
            selected && qApp->property("controllerSkinKey").toString()
                            == QStringLiteral("hardware");
        const QColor textColor = selectedInHardwareSkin
            ? QColor(Qt::white)
            : background.palette.color(
                  selected && !playoutState ? QPalette::HighlightedText
                                            : QPalette::Text);
        QColor secondaryTextColor = textColor;
        secondaryTextColor.setAlpha(185);
        QColor subduedTextColor = textColor;
        subduedTextColor.setAlpha(145);

        const int infoLeft = thumbnailRect.right() + vtrpon::scalePx(10);
        const int modeColumnWidth = vtrpon::scalePx(kItemModeColumnWidth);
        const QRect modeColumn(content.right() - modeColumnWidth + 1,
                               content.top(), modeColumnWidth,
                               content.height());
        const QRect infoRect(
            infoLeft, content.top(),
            (std::max)(0, modeColumn.left() - vtrpon::scalePx(9) - infoLeft),
            content.height());

        const int statusSize = vtrpon::scalePx(12);
        const QRect statusRect(infoRect.right() - statusSize + 1,
                               infoRect.bottom() - statusSize + 1,
                               statusSize, statusSize);
        if (!status.isNull()) {
            status.paint(painter, statusRect, Qt::AlignCenter,
                         QIcon::Normal, QIcon::On);
        }

        int badgeRight = infoRect.right();
        const auto drawBadge = [&](bool active, const QString &label,
                                   QColor color, qreal opacity = 1.0) {
            if (!active) {
                return;
            }
            QFont badgeFont = background.font;
            badgeFont.setBold(true);
            badgeFont.setPixelSize(vtrpon::scalePx(9));
            painter->setFont(badgeFont);
            const int width = painter->fontMetrics().horizontalAdvance(label)
                + vtrpon::scalePx(8);
            const QRect badgeRect(badgeRight - width + 1, content.top(), width,
                                  vtrpon::scalePx(16));
            QColor badgeBackground = color;
            badgeBackground.setAlphaF((190.0 / 255.0) * opacity);
            QColor badgeOutline = color.lighter(150);
            badgeOutline.setAlphaF(opacity);
            painter->setPen(badgeOutline);
            painter->setBrush(badgeBackground);
            painter->drawRoundedRect(badgeRect, vtrpon::scalePx(2),
                                     vtrpon::scalePx(2));
            QColor badgeText(Qt::white);
            badgeText.setAlphaF(opacity);
            painter->setPen(badgeText);
            painter->drawText(badgeRect, Qt::AlignCenter, label);
            badgeRight = badgeRect.left() - vtrpon::scalePx(4);
        };
        drawBadge((visualState & ItemVisualEdit) != 0,
                  vtrpon::uiText("info.source.edit"),
                  QColor(QStringLiteral("#377c43")));
        drawBadge((visualState & ItemVisualDsk) != 0, QStringLiteral("DSK"),
                  QColor(QStringLiteral("#357eb5")));
        drawBadge((visualState & ItemVisualOnAir) != 0
                      || onAirHighlightOpacity > 0.0,
                  vtrpon::uiText("playlist.on_air"),
                  QColor(QStringLiteral("#a92e36")),
                  onAirHighlightOpacity);
        drawBadge((visualState & ItemVisualBgmPlay) != 0,
                  QStringLiteral("BGM PLAY"),
                  QColor(QStringLiteral("#8a6429")));

        const int titleHeight = (std::max)(vtrpon::scalePx(19),
            vtrpon::scaleTextPx(13) + vtrpon::scalePx(6));
        const QRect numberRect(infoRect.left(), infoRect.top(),
                               vtrpon::scalePx(kItemNumberWidth), titleHeight);
        QColor numberBackground = textColor;
        numberBackground.setAlpha(28);
        QColor numberOutline = textColor;
        numberOutline.setAlpha(95);
        painter->setPen(numberOutline);
        painter->setBrush(numberBackground);
        painter->drawRoundedRect(numberRect, vtrpon::scalePx(2),
                                 vtrpon::scalePx(2));

        QFont numberFont = background.font;
        numberFont.setBold(true);
        numberFont.setPixelSize(vtrpon::scaleTextPx(11));
        painter->setFont(numberFont);
        painter->setPen(textColor);
        painter->drawText(numberRect, Qt::AlignCenter,
                          QString::number(index.data(kItemNumberRole).toInt()));

        const int nameLeft = numberRect.right() + vtrpon::scalePx(7);
        const int nameRight = (visualState == ItemVisualNone
                               && onAirHighlightOpacity <= 0.0)
            ? infoRect.right() : badgeRight;
        const QRect nameRect(nameLeft, infoRect.top(),
                             (std::max)(0, nameRight - nameLeft + 1),
                             titleHeight);
        QFont titleFont = background.font;
        titleFont.setBold(true);
        titleFont.setPixelSize(vtrpon::scaleTextPx(12));
        painter->setFont(titleFont);
        painter->setPen(textColor);
        painter->drawText(
            nameRect, Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(
                index.data(kItemNameRole).toString(), Qt::ElideMiddle,
                nameRect.width()));

        const int metadataTop = infoRect.top() + titleHeight
            + vtrpon::scalePx(2);
        const int metadataHeight = infoRect.bottom() - metadataTop + 1;
        const int metadataRowHeight = metadataHeight / 3;
        const int metadataGap = vtrpon::scalePx(8);
        const int metadataColumnWidth =
            (infoRect.width() - metadataGap) / 2;
        const auto drawMetadata = [&](const QRect &rect,
                                      const QString &label,
                                      const QString &value,
                                      bool fixedPitch = false) {
            QFont labelFont = background.font;
            labelFont.setBold(true);
            labelFont.setPixelSize(vtrpon::scaleTextPx(9));
            painter->setFont(labelFont);
            const int labelWidth = painter->fontMetrics().horizontalAdvance(
                QStringLiteral("TYPE")) + vtrpon::scalePx(5);
            const QRect labelRect(rect.left(), rect.top(), labelWidth,
                                  rect.height());
            painter->setPen(subduedTextColor);
            painter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                              label);

            QFont valueFont = background.font;
            valueFont.setPixelSize(vtrpon::scaleTextPx(10));
            if (fixedPitch) {
                valueFont.setStyleHint(QFont::Monospace);
                valueFont.setFixedPitch(true);
            }
            painter->setFont(valueFont);
            painter->setPen(secondaryTextColor);
            const QRect valueRect = rect.adjusted(labelWidth, 0, 0, 0);
            painter->drawText(
                valueRect, Qt::AlignLeft | Qt::AlignVCenter,
                painter->fontMetrics().elidedText(value, Qt::ElideRight,
                                                  valueRect.width()));
        };

        const auto metadataRect = [&](int column, int row) {
            return QRect(infoRect.left() + column * (metadataColumnWidth + metadataGap),
                         metadataTop + row * metadataRowHeight,
                         metadataColumnWidth, metadataRowHeight);
        };
        drawMetadata(metadataRect(0, 0), QStringLiteral("RES"),
                     index.data(kItemResolutionRole).toString());
        drawMetadata(metadataRect(0, 1), QStringLiteral("DUR"),
                     index.data(kItemDurationRole).toString(), true);
        drawMetadata(metadataRect(0, 2), vtrpon::uiText("playlist.item.volume_label"),
                     index.data(kItemVolumeRole).toString());
        drawMetadata(metadataRect(1, 0), vtrpon::uiText("playlist.item.in_label"),
                     index.data(kItemInPointRole).toString(), true);
        drawMetadata(metadataRect(1, 1), vtrpon::uiText("playlist.item.out_label"),
                     index.data(kItemOutPointRole).toString(), true);
        drawMetadata(metadataRect(1, 2), vtrpon::uiText("playlist.item.type_label"),
                     index.data(kItemMediaTypeRole).toString());
        const QRect modeContent = modeColumn.adjusted(vtrpon::scalePx(9), 0,
                                                      -vtrpon::scalePx(2), 0);
        const int modeRowHeight = modeContent.height() / 2;
        const QRect startRect(modeContent.left(), modeContent.top(),
                              modeContent.width(), modeRowHeight);
        const QRect endRect(modeContent.left(),
                            modeContent.top() + modeRowHeight,
                            modeContent.width(),
                            modeContent.height() - modeRowHeight);
        const bool live = index.data(kItemLiveRole).toBool();
        const QString startLabel = vtrpon::uiText("playlist.item.start_label");
        const QString endLabel = vtrpon::uiText("playlist.item.end_label");
        QFont modeLabelFont = background.font;
        modeLabelFont.setBold(true);
        modeLabelFont.setPixelSize(vtrpon::scaleTextPx(9));
        const QFontMetrics modeLabelMetrics(modeLabelFont);
        const int modeLabelWidth = (std::max)(
            modeLabelMetrics.horizontalAdvance(startLabel),
            modeLabelMetrics.horizontalAdvance(endLabel));
        const auto drawMode = [&](const QRect &rect, const QString &label,
                                  const QString &value) {
            const QRect labelRect(rect.left(), rect.top(), modeLabelWidth,
                                  rect.height());
            painter->setFont(modeLabelFont);
            painter->setPen(subduedTextColor);
            painter->drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter, label);

            QFont valueFont = background.font;
            valueFont.setBold(true);
            valueFont.setPixelSize(vtrpon::scaleTextPx(9));
            painter->setFont(valueFont);
            const QColor modeTextColor = live ? subduedTextColor : textColor;
            painter->setPen(modeTextColor);
            const int boxHeight = (std::max)(vtrpon::scalePx(22),
                vtrpon::scaleTextPx(9) + vtrpon::scalePx(8));
            const int boxLeft = rect.left() + modeLabelWidth + vtrpon::scalePx(6);
            const QRect valueRect(boxLeft, rect.top() + (rect.height() - boxHeight) / 2,
                                  (std::max)(0, rect.right() - boxLeft + 1), boxHeight);
            painter->setPen(numberOutline);
            painter->setBrush(numberBackground);
            painter->drawRoundedRect(valueRect, vtrpon::scalePx(2),
                                     vtrpon::scalePx(2));
            painter->setPen(modeTextColor);
            painter->drawText(
                valueRect.adjusted(vtrpon::scalePx(3), 0, -vtrpon::scalePx(3), 0),
                Qt::AlignCenter,
                painter->fontMetrics().elidedText(value, Qt::ElideRight,
                                                  valueRect.width() - vtrpon::scalePx(6)));
        };
        drawMode(startRect, startLabel, index.data(kItemStartModeRole).toString());
        drawMode(endRect, endLabel, index.data(kItemEndModeRole).toString());
        if (separatorGap > 0) {
            const int lineHeight = vtrpon::scalePx(3);
            int nextItemTop = option.rect.bottom() + 1
                + (view ? 2 * view->spacing() : 0);
            if (view && index.row() + 1 < index.model()->rowCount(index.parent())) {
                const QRect nextRect = view->visualRect(
                    index.siblingAtRow(index.row() + 1));
                if (nextRect.isValid()) {
                    nextItemTop = nextRect.top();
                }
            }
            const int clearance = nextItemTop - (itemRect.bottom() + 1);
            const QRectF separatorRect(
                itemRect.left(),
                itemRect.bottom() + 1 + (clearance - lineHeight) / 2.0,
                itemRect.width(), lineHeight);
            painter->fillRect(separatorRect,
                              option.palette.color(QPalette::Text));
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        const auto *view = qobject_cast<const QListView *>(parent());
        if (view && view->viewMode() == QListView::IconMode) {
            return vtrpon::scaleSize(kPlaylistTileWidth,
                                     kPlaylistTileHeight);
        }
        const QSize base = QStyledItemDelegate::sizeHint(option, index);
        const int textHeight = vtrpon::scaleTextPx(13) + vtrpon::scalePx(6)
            + vtrpon::scalePx(2) + 3 * (vtrpon::scaleTextPx(10) + vtrpon::scalePx(3))
            + vtrpon::scalePx(8);
        return QSize(base.width(), (std::max)(vtrpon::scalePx(kPlaylistItemHeight),
                                              textHeight)
            + (index.data(kItemSeparatorBelowRole).toBool()
                   ? vtrpon::scalePx(12) : 0));
    }
};

QString formatPlaylistPoint(qint64 pointMs)
{
    const qint64 totalCentiseconds = (std::max)(qint64{0}, pointMs) / 10;
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

QString formatItemPoints(const PlaylistItemSettings &settings)
{
    return vtrpon::uiText("playlist.item.points")
        .arg(formatPlaylistPoint(settings.inPointMs),
             formatPlaylistPoint(settings.outPointMs.value_or(0)));
}

QString startModeName(PlaylistStartMode mode)
{
    switch (mode) {
    case PlaylistStartMode::Pause:
        return vtrpon::uiText("common.pause");
    case PlaylistStartMode::Play:
        return vtrpon::uiText("common.play");
    }
    return QString();
}

QString formatItemStartMode(const PlaylistItemSettings &settings)
{
    QString start = startModeName(settings.startMode);
    if (settings.fadeInEnabled) {
        start += vtrpon::uiText("playlist.item.fade_in");
    }
    return start;
}

QString formatItemVolume(const PlaylistItemSettings &settings)
{
    return settings.nominalVolumeTenthsDb == kMinimumItemVolumeTenthsDb
        ? QString(QChar(0x2212)) + QChar(0x221e)
        : QStringLiteral("%1 dB").arg(settings.nominalVolumeTenthsDb / 10.0,
                                      0, 'f', 1);
}

QString formatItemModes(const PlaylistItemSettings &settings,
                        int currentPlayCount)
{
    return vtrpon::uiText("playlist.item.modes")
        .arg(formatItemStartMode(settings),
             vtrpon::formatPlaylistItemEndMode(settings, currentPlayCount),
             formatItemVolume(settings));
}

QString playlistMapStopReasonText(PlaylistMapStopReason reason)
{
    switch (reason) {
    case PlaylistMapStopReason::None:
        return QString();
    case PlaylistMapStopReason::StartModePause:
        return vtrpon::uiText("playlist.map.note.start_pause");
    case PlaylistMapStopReason::DestinationStartModePause:
        return vtrpon::uiText("playlist.map.note.destination_pause");
    case PlaylistMapStopReason::LiveOrUnknownDuration:
        return vtrpon::uiText("playlist.map.note.live_or_unknown");
    case PlaylistMapStopReason::InfiniteRepeat:
        return vtrpon::uiText("playlist.map.note.infinite_repeat");
    case PlaylistMapStopReason::Loop:
        return vtrpon::uiText("playlist.map.note.loop");
    case PlaylistMapStopReason::DestinationUnavailable:
        return vtrpon::uiText("playlist.map.note.destination_unavailable");
    case PlaylistMapStopReason::DestinationBgm:
        return vtrpon::uiText("playlist.map.note.destination_bgm");
    }
    return QString();
}

QIcon createStatusIcon(StatusIconState state)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (state == StatusIconState::OnAir) {
        painter.setPen(QColor(QStringLiteral("#ff6b6b")));
        painter.setBrush(QColor(QStringLiteral("#d62f3a")));
        painter.drawEllipse(QRectF(2.0, 2.0, 8.0, 8.0));
    } else if (state == StatusIconState::CueReady) {
        painter.setPen(QPen(QColor(QStringLiteral("#f0b55b")), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(2.0, 2.0, 8.0, 8.0));
    } else if (state == StatusIconState::CuePending) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#65a7d8")));
        painter.drawEllipse(QRectF(3.0, 3.0, 6.0, 6.0));
    } else if (state == StatusIconState::CueError) {
        painter.setPen(QPen(QColor(QStringLiteral("#ef7d6d")), 2.0));
        painter.drawLine(QPointF(3.0, 3.0), QPointF(9.0, 9.0));
        painter.drawLine(QPointF(9.0, 3.0), QPointF(3.0, 9.0));
    }
    return QIcon(pixmap);
}

const QIcon &statusIcon(StatusIconState state)
{
    static const std::array<QIcon, 5> icons = {
        createStatusIcon(StatusIconState::None),
        createStatusIcon(StatusIconState::CuePending),
        createStatusIcon(StatusIconState::CueReady),
        createStatusIcon(StatusIconState::CueError),
        createStatusIcon(StatusIconState::OnAir),
    };
    return icons[static_cast<std::size_t>(state)];
}

QIcon createPlaylistViewModeIcon(bool tileView)
{
    QPixmap pixmap(vtrpon::scaleSize(16, 16));
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor(QStringLiteral("#d7dcde")),
             (std::max)(1, vtrpon::scalePx(2)), Qt::SolidLine,
             Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const qreal scale = vtrpon::uiScale();
    if (tileView) {
        const QRectF gridRect(3.0 * scale, 3.0 * scale,
                              10.0 * scale, 10.0 * scale);
        painter.drawRect(gridRect);
        painter.drawLine(QPointF(8.0 * scale, 3.0 * scale),
                         QPointF(8.0 * scale, 13.0 * scale));
        painter.drawLine(QPointF(3.0 * scale, 8.0 * scale),
                         QPointF(13.0 * scale, 8.0 * scale));
    } else {
        for (qreal y : {4.0, 8.0, 12.0}) {
            painter.drawLine(QPointF(3.0 * scale, y * scale),
                             QPointF(13.0 * scale, y * scale));
        }
    }
    return QIcon(pixmap);
}

QAbstractItemDelegate *createPlaylistItemDelegate(QObject *parent)
{
    return new PlaylistItemDelegate(parent);
}

} // namespace playlist_view
