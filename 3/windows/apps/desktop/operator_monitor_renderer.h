#pragma once
#include <QPainter>
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>

namespace vtrpon {
inline QRect operatorMonitorVideoRect(const QSize &size, const QJsonObject &presentation) {
    const bool band = presentation.value("information").toBool() ||
                      presentation.value("timer").toObject().value("enabled").toBool() ||
                      !presentation.value("cue").toString().isEmpty();
    const int areaHeight = band ? size.height() * 4 / 5 : size.height();
    const QSize fitted = QSize(16, 9).scaled(QSize(size.width(), areaHeight), Qt::KeepAspectRatio);
    return QRect(QPoint((size.width() - fitted.width()) / 2, (areaHeight - fitted.height()) / 2), fitted);
}
inline QRect operatorMonitorInformationRect(const QSize &size) {
  return QRect(0, size.height() * 4 / 5, size.width(), size.height() - size.height() * 4 / 5);
}
inline void paintOperatorMonitor(QPainter &painter, const QSize &size,
                                 const QImage &image, const QJsonObject &presentation) {
    painter.fillRect(QRect(QPoint(), size), Qt::black);
    if (!image.isNull()) {
      // Fit the video above the information band without nearest-neighbor aliasing.
      painter.setRenderHint(QPainter::SmoothPixmapTransform);
      painter.drawImage(operatorMonitorVideoRect(size, presentation), image);
    }
    const QString cue = presentation.value("cue").toString();
    const QRect band = operatorMonitorInformationRect(size);
    const int margin = qMax(6, band.height() / 12);
    const QRect content = band.adjusted(margin, margin, -margin, -margin);
    painter.setPen(Qt::white);
    if (!cue.isEmpty()) {
      QFont font = painter.font();
      font.setBold(true);
      int size = qMax(12, int(content.height() * 0.58));
      for (; size > 8; --size) {
        font.setPixelSize(size);
        const QRect bounds = QFontMetrics(font).boundingRect(
            QRect(0, 0, content.width(), 10000), Qt::AlignCenter | Qt::TextWordWrap | Qt::TextWrapAnywhere, cue);
        if (bounds.height() <= content.height() && bounds.width() <= content.width()) break;
      }
      font.setPixelSize(size);
      painter.setFont(font);
      painter.setClipRect(content);
      painter.drawText(content, Qt::AlignCenter | Qt::TextWordWrap | Qt::TextWrapAnywhere, cue);
      return;
    }
    const QJsonObject timer = presentation.value("timer").toObject();
    if (timer.value("enabled").toBool()) {
      const QJsonArray fields = presentation.value("information").toBool()
          ? presentation.value("fields").toArray() : QJsonArray();
      const bool pages = !fields.isEmpty();
      QRect timerRect = content;
      if (pages) timerRect.setWidth(content.width() * 3 / 4 - margin);
      // The warning frame follows the available display area, never the digits.
      const int frameWidth = qMin(timerRect.width(), timerRect.height() * 3);
      const QRect timerFrame(timerRect.left() + (timerRect.width() - frameWidth) / 2,
                             timerRect.top(), frameWidth, timerRect.height());
      const QRect textRect = timerFrame.adjusted(margin, 0, -margin, 0);
      painter.setPen(Qt::white);
      QFont font = painter.font();
      font.setBold(true);
      font.setPixelSize(qMax(12, int(timerRect.height() * 0.75)));
      const QString text = timer.value("text").toString();
      while (font.pixelSize() > 10 && QFontMetrics(font).horizontalAdvance(text) > textRect.width())
        font.setPixelSize(font.pixelSize() - 1);
      painter.setFont(font);
      painter.drawText(textRect, Qt::AlignCenter, text);
      const QColor warningColor(timer.value("warningColor").toString());
      if (timer.value("warning").toInt() > 0 && warningColor.isValid()) {
        const qreal lineWidth = qMax(2, band.height() / 54);
        const qreal inset = lineWidth / 2.0;
        painter.setPen(QPen(warningColor, lineWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(timerFrame).adjusted(inset, inset, -inset, -inset));
      }
      if (pages) {
        font.setBold(false);
        font.setPixelSize(qMax(10, band.height() / 6));
        painter.setFont(font);
        painter.setPen(Qt::white);
        const QRect pageRect(timerRect.right() + margin, content.top(),
                             content.right() - timerRect.right() - margin, content.height());
        const auto field = fields.first().toObject();
        painter.drawText(pageRect, Qt::AlignCenter | Qt::TextWordWrap,
            field.value("label").toString() + QLatin1Char('\n') + field.value("value").toString());
      }
      return;
    }
    if (!presentation.value("information").toBool()) return;
    const QJsonArray fields = presentation.value("fields").toArray();
    if (fields.isEmpty()) return;
    const auto drawField = [&](const QRect &rect, const QJsonObject &field, bool large) {
      QFont labelFont = painter.font();
      labelFont.setBold(false);
      labelFont.setPixelSize(qMax(9, band.height() / 8));
      painter.setFont(labelFont);
      painter.setPen(QColor(190, 190, 190));
      const int labelHeight = QFontMetrics(labelFont).height();
      painter.drawText(rect.adjusted(0, 0, 0, 0), Qt::AlignTop | Qt::AlignLeft,
          QFontMetrics(labelFont).elidedText(field.value("label").toString(), Qt::ElideRight, rect.width()));
      QFont valueFont = labelFont;
      valueFont.setBold(large);
      valueFont.setPixelSize(qMax(10, int(rect.height() * (large ? 0.45 : 0.48))));
      const QString text = field.value("value").toString();
      while (valueFont.pixelSize() > 10 && QFontMetrics(valueFont).horizontalAdvance(text) > rect.width())
        valueFont.setPixelSize(valueFont.pixelSize() - 1);
      painter.setFont(valueFont);
      painter.setPen(Qt::white);
      painter.drawText(rect.adjusted(0, labelHeight, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
          QFontMetrics(valueFont).elidedText(text, Qt::ElideRight, rect.width()));
    };
    int start = 0;
    QRect details = content;
    if (fields.first().toObject().value("key") == QJsonValue("remaining")) {
      const int remainingWidth = fields.size() == 1 ? content.width() : content.width() * 28 / 100;
      drawField(QRect(content.topLeft(), QSize(remainingWidth, content.height())), fields.first().toObject(), true);
      details.setLeft(content.left() + remainingWidth + margin);
      start = 1;
    }
    // File name gets a full row; the remaining compact values share the next row.
    if (start < fields.size() && fields[start].toObject().value("key") == QJsonValue("file")) {
      const int rowHeight = fields.size() - start == 1 ? details.height() : details.height() / 2;
      drawField(QRect(details.topLeft(), QSize(details.width(), rowHeight)), fields[start].toObject(), false);
      details.setTop(details.top() + rowHeight);
      ++start;
    }
    const int count = fields.size() - start;
    for (int i = 0; i < count; ++i) {
      QRect cell(details.left() + i * details.width() / count, details.top(),
                 details.width() / count - margin, details.height());
      drawField(cell, fields[start + i].toObject(), false);
    }
}
}
