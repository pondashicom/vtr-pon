#pragma once
#include <QWidget>
#include <QPainter>
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>

class OperatorMonitorWidget final : public QWidget {
public:
  QImage image;
  void setPresentation(const QJsonObject &value) {
    presentation_ = value;
    update();
  }
  QRect videoRect() const {
    const bool band = presentation_.value("information").toBool() ||
                      presentation_.value("timer").toObject().value("enabled").toBool() ||
                      !presentation_.value("cue").toString().isEmpty();
    const int areaHeight = band ? height() * 4 / 5 : height();
    const QSize size = QSize(16, 9).scaled(QSize(width(), areaHeight), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - size.width()) / 2, (areaHeight - size.height()) / 2), size);
  }
  QRect informationRect() const { return QRect(0, height() * 4 / 5, width(), height() - height() * 4 / 5); }
  void applyDisplay() {
    const QString requested = presentation_.value("screen").toString();
    QScreen *target = nullptr;
    for (auto *screen : QGuiApplication::screens())
      if (!requested.isEmpty() && screen->name() == requested) target = screen;
    const QString effective = target ? requested : QString();
    const QRect geometry = target ? target->availableGeometry() : QRect();
    if (effective == appliedScreen_ && geometry == appliedGeometry_) return;
    appliedScreen_ = effective;
    appliedGeometry_ = geometry;
    if (!target) target = QGuiApplication::primaryScreen();
    if (!target) return;
    // Display selection moves a regular, user-operable window. Never take over
    // a screen or remove the title bar / close button.
    setWindowFlag(Qt::FramelessWindowHint, false);
    winId();
    windowHandle()->setScreen(target);
    const QRect available = target->availableGeometry();
    QSize windowSize(960, 540);
    const QSize limit(available.width() * 3 / 4, available.height() * 3 / 4);
    if (windowSize.width() > limit.width() || windowSize.height() > limit.height())
      windowSize.scale(limit, Qt::KeepAspectRatio);
    resize(windowSize);
    showNormal();
    move(available.center() - QPoint(frameGeometry().width() / 2,
                                    frameGeometry().height() / 2));
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (!image.isNull()) {
      // Fit the video above the information band without nearest-neighbor aliasing.
      painter.setRenderHint(QPainter::SmoothPixmapTransform);
      painter.drawImage(videoRect(), image);
    }
    const QString cue = presentation_.value("cue").toString();
    const QRect band = informationRect();
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
    const QJsonObject timer = presentation_.value("timer").toObject();
    if (timer.value("enabled").toBool()) {
      const QJsonArray fields = presentation_.value("information").toBool()
          ? presentation_.value("fields").toArray() : QJsonArray();
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
    if (!presentation_.value("information").toBool()) return;
    const QJsonArray fields = presentation_.value("fields").toArray();
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
private:
  QJsonObject presentation_;
  QString appliedScreen_;
  QRect appliedGeometry_;
};
