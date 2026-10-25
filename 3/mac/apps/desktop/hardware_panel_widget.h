#pragma once

#include "controller_scaling.h"
#include "skins/controller_skin.h"

#include <QLinearGradient>
#include <QPalette>
#include <QPaintEvent>
#include <QPainter>
#include <QWidget>

class HardwarePanelWidget final : public QWidget {
public:
  enum class Layout { SinglePlate, DualPlate };

  explicit HardwarePanelWidget(Layout layout, QWidget *parent = nullptr)
      : QWidget(parent), layout_(layout) {}

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal halfStroke = 0.5 * vtrpon::uiScale();
    const QRectF chassis =
        QRectF(rect()).adjusted(halfStroke, halfStroke, -halfStroke, -halfStroke);
    const qreal outerRadius = vtrpon::scalePx(5);
    if (!vtrpon::currentControllerSkinUsesHardwareEffects()) {
      painter.setPen(QPen(palette().color(QPalette::Mid), vtrpon::uiScale()));
      painter.setBrush(palette().color(QPalette::Base));
      painter.drawRoundedRect(chassis, outerRadius, outerRadius);
      return;
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawRoundedRect(chassis.translated(0, vtrpon::scalePx(2)),
                            outerRadius, outerRadius);

    QLinearGradient chassisGradient(chassis.topLeft(), chassis.bottomLeft());
    chassisGradient.setColorAt(0.0, QColor(QStringLiteral("#43535a")));
    chassisGradient.setColorAt(0.03, QColor(QStringLiteral("#263940")));
    chassisGradient.setColorAt(0.14, QColor(QStringLiteral("#1b3038")));
    chassisGradient.setColorAt(0.17, QColor(QStringLiteral("#10171b")));
    chassisGradient.setColorAt(1.0, QColor(QStringLiteral("#090c0f")));
    painter.setBrush(chassisGradient);
    painter.setPen(QPen(QColor(QStringLiteral("#68757a")), vtrpon::uiScale()));
    painter.drawRoundedRect(chassis, outerRadius, outerRadius);

    const QRectF inner = chassis.adjusted(vtrpon::scalePx(3),
                                          vtrpon::scalePx(3),
                                          -vtrpon::scalePx(3),
                                          -vtrpon::scalePx(3));
    const auto drawChannelPlate = [&](const QRectF &plate) {
      QLinearGradient face(plate.topLeft(), plate.bottomLeft());
      face.setColorAt(0.0, QColor(QStringLiteral("#17272d")));
      face.setColorAt(1.0, QColor(QStringLiteral("#080d10")));
      painter.setPen(QPen(QColor(QStringLiteral("#26343a")), vtrpon::uiScale()));
      painter.setBrush(face);
      painter.drawRoundedRect(plate, vtrpon::scalePx(2),
                              vtrpon::scalePx(2));
    };

    if (layout_ == Layout::DualPlate) {
      const qreal dividerX = inner.center().x();
      const qreal dividerGap = vtrpon::scalePx(2);
      const QRectF itemPlate(inner.left(), inner.top(),
                             dividerX - dividerGap - inner.left(),
                             inner.height());
      const QRectF masterPlate(dividerX + dividerGap, inner.top(),
                               inner.right() - dividerX - dividerGap,
                               inner.height());
      drawChannelPlate(itemPlate);
      drawChannelPlate(masterPlate);

      QLinearGradient divider(dividerX - dividerGap, 0,
                              dividerX + dividerGap, 0);
      divider.setColorAt(0.0, QColor(QStringLiteral("#080b0d")));
      divider.setColorAt(0.45, QColor(QStringLiteral("#536168")));
      divider.setColorAt(0.55, QColor(QStringLiteral("#1b2327")));
      divider.setColorAt(1.0, QColor(QStringLiteral("#050708")));
      painter.setPen(Qt::NoPen);
      painter.setBrush(divider);
      painter.drawRect(QRectF(dividerX - dividerGap, inner.top(),
                              dividerGap * 2.0, inner.height()));
    } else {
      drawChannelPlate(inner);
    }

  }

private:
  Layout layout_;
};
