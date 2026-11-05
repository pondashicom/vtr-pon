#pragma once
#include "operator_monitor_renderer.h"
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
    return vtrpon::operatorMonitorVideoRect(size(), presentation_);
  }
  QRect informationRect() const { return vtrpon::operatorMonitorInformationRect(size()); }
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
    vtrpon::paintOperatorMonitor(painter, size(), image, presentation_);
  }
private:
  QJsonObject presentation_;
  QString appliedScreen_;
  QRect appliedGeometry_;
};
