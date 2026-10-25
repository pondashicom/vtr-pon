#pragma once
#include "controller_scaling.h"
#include "equalizer_profile.h"
#include "skins/controller_skin.h"
#include "language_text.h"
#include "vst_editor.h"
#include <QApplication>
#include <QDialog>
#include <QDial>
#include <QPainter>
#include <QConicalGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
namespace vtrpon {
inline bool hardwareEqAppearance() {
  return qApp->property("controllerSkinUsesHardwareEffects").isValid()
      ? qApp->property("controllerSkinUsesHardwareEffects").toBool()
      : vtrpon::currentControllerSkinUsesHardwareEffects();
}

class MasterEqWindow : public QDialog {
public:
  using QDialog::QDialog;
protected:
  void paintEvent(QPaintEvent *event) override {
    QDialog::paintEvent(event);
    if (!hardwareEqAppearance()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF face = QRectF(rect()).adjusted(1, 1, -1, -1);
    QLinearGradient finish(face.topLeft(), face.bottomRight());
    finish.setColorAt(0, QColor("#343633"));
    finish.setColorAt(0.45, QColor("#222420"));
    finish.setColorAt(1, QColor("#151713"));
    p.setBrush(finish);
    p.setPen(QPen(QColor("#77766a"), 1));
    p.drawRoundedRect(face, 3, 3);
    // Fine horizontal machining and recessed corner screws.
    p.setPen(QColor(255, 255, 255, 5));
    for (int y = 3; y < height() - 2; y += 3) p.drawLine(2, y, width() - 3, y);
    const qreal inset = vtrpon::scalePx(7);
    for (const QPointF corner : {QPointF(inset, inset), QPointF(width()-inset, inset),
                                QPointF(inset, height()-inset), QPointF(width()-inset, height()-inset)}) {
      p.setPen(QPen(QColor("#11120f"), 1));
      p.setBrush(QColor("#77786e"));
      p.drawEllipse(corner, 2.5, 2.5);
      p.drawLine(corner + QPointF(-1.5, 1), corner + QPointF(1.5, -1));
    }
  }
};

// Relative vertical dragging works even when grabbed at the center. The
// indicator keeps QDial's end stops without absolute-angle wraparound.
class MasterEqDial final : public QDial {
public:
  using QDial::QDial;

protected:
  void paintEvent(QPaintEvent *event) override {
    if (!hardwareEqAppearance()) {
      QDial::paintEvent(event);
      return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(width() / 64.0, height() / 64.0);
    const auto radial = [](double degrees, double radius) {
      const double radians = degrees * std::acos(-1.0) / 180.0;
      return QPointF(std::sin(radians) * radius, -std::cos(radians) * radius);
    };
    for (int tick = 0; tick <= 24; ++tick) {
      const double angle = -150.0 + tick * 12.5;
      const bool major = tick % 4 == 0;
      p.setPen(QPen(QColor(tick == 12 ? "#dfbc72" : "#a5a69b"), major ? 1.2 : 0.7));
      p.drawLine(radial(angle, major ? 25 : 27), radial(angle, 30));
    }
    p.setPen(QPen(QColor("#080a08"), 2));
    p.setBrush(QColor("#10120f"));
    p.drawEllipse(QPointF(0, 1.5), 23, 23);
    QConicalGradient metal(QPointF(0, 0), 40);
    metal.setColorAt(0, QColor("#edece2"));
    metal.setColorAt(0.22, QColor("#777b76"));
    metal.setColorAt(0.46, QColor("#faf9ee"));
    metal.setColorAt(0.7, QColor("#858983"));
    metal.setColorAt(1, QColor("#edece2"));
    p.setBrush(metal);
    p.setPen(QPen(QColor("#bbbeb4"), 0.8));
    p.drawEllipse(QPointF(), 21.5, 21.5);
    for (int flute = 0; flute < 40; ++flute) {
      p.setPen(QPen(QColor(20, 24, 20, 135), 0.7));
      p.drawLine(radial(flute * 9, 19), radial(flute * 9, 21));
    }
    QLinearGradient cap(-16, -17, 16, 18);
    cap.setColorAt(0, QColor("#e3e5db"));
    cap.setColorAt(0.5, QColor("#b8bcb3"));
    cap.setColorAt(1, QColor("#82877f"));
    p.setBrush(cap);
    p.setPen(QPen(QColor("#60665d"), 0.8));
    p.drawEllipse(QPointF(), 18, 18);
    const double angle = -150.0 + 300.0 * (value() - minimum()) / (maximum() - minimum());
    p.setPen(QPen(QColor("#252923"), 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(radial(angle, 6), radial(angle, 16));
    if (hasFocus()) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(QColor("#dfbc72"), 0.8, Qt::DotLine));
      p.drawEllipse(QPointF(), 23, 23);
    }
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) {
      event->ignore();
      return;
    }
    setFocus(Qt::MouseFocusReason);
    dragging_ = true;
    position_ = value();
    lastY_ = event->position().y();
    setSliderDown(true);
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (!dragging_) return;
    const double delta = lastY_ - event->position().y();
    lastY_ = event->position().y();
    // Eight logical pixels per 0.5 dB; discard overshoot at either stop so
    // reversing direction immediately responds, even outside the widget.
    position_ = std::clamp(position_ + delta / vtrpon::scalePx(8),
                           double(minimum()), double(maximum()));
    setValue(qRound(position_));
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && dragging_) {
      dragging_ = false;
      setSliderDown(false);
      event->accept();
    } else {
      event->ignore();
    }
  }

  void wheelEvent(QWheelEvent *event) override {
    wheelRemainder_ += event->angleDelta().y();
    const int steps = wheelRemainder_ / 120;
    wheelRemainder_ %= 120;
    setValue(value() + steps * singleStep());
    position_ = value();
    event->accept();
  }

private:
  bool dragging_ = false;
  double lastY_ = 0.0;
  double position_ = 0.0;
  int wheelRemainder_ = 0;
};


// Shared by nominal ITEM and live ITEM EQ; changing the displayed item never
// emits an edit. The owner supplies the guarded model update callback.
class EqualizerEditor final : public MasterEqWindow {
public:
  EqualizerEditor(QWidget *parent, const QString &prefix, const char *titleKey,
                  std::function<void(std::array<int, 3>)> changed)
      : MasterEqWindow(parent, Qt::Window | Qt::CustomizeWindowHint
            | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint),
        titleKey_(titleKey), changed_(std::move(changed)) {
    setObjectName(prefix + QStringLiteral("Window"));
    setProperty("itemEqWindow", true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(scalePx(12), scalePx(12), scalePx(12), scalePx(12));
    layout->setSpacing(scalePx(4));
    const std::array<const char *, 3> names{"LOW", "MID", "HIGH"};
    for (int band = 2; band >= 0; --band) {
      auto *label = new QLabel(QStringLiteral("%1 / %2").arg(QString::fromLatin1(names[band]),
          QString::fromLatin1(kEqFrequencyLabels[band])), this);
      label->setAlignment(Qt::AlignCenter);
      layout->addWidget(label);
      auto *dial = new MasterEqDial(this);
      dials_[band] = dial;
      dial->setObjectName(prefix + QString::number(band) + QStringLiteral("Dial"));
      dial->setAccessibleName(label->text());
      dial->setRange(-kEqMaximumTenthsDb / 5, kEqMaximumTenthsDb / 5);
      dial->setSingleStep(1);
      dial->setPageStep(2);
      dial->setNotchesVisible(true);
      dial->setWrapping(false);
      dial->setFixedSize(scaleSize(60, 60));
      layout->addWidget(dial, 0, Qt::AlignHCenter);
      values_[band] = new QLabel(this);
      values_[band]->setAlignment(Qt::AlignCenter);
      layout->addWidget(values_[band]);
      connect(dial, &QDial::valueChanged, this, [this, band](int value) {
        gains_[band] = value * 5;
        refresh();
        changed_(gains_);
      });
    }
    reset_ = new QPushButton(this);
    reset_->setObjectName(QStringLiteral("programMasterEqResetButton"));
    reset_->setAutoDefault(false);
    reset_->setMinimumSize(scaleSize(176, 24));
    layout->addWidget(reset_);
    connect(reset_, &QPushButton::clicked, this, [this] {
      setGains({});
      changed_(gains_);
    });
    layout->setSizeConstraint(QLayout::SetFixedSize);
    setGains({});
  }
  void setGains(const std::array<int, 3> &gains) {
    gains_ = gains;
    for (int band = 0; band < 3; ++band) {
      const QSignalBlocker blocker(dials_[band]);
      dials_[band]->setValue(qRound(gains[band] / 5.0));
    }
    refresh();
  }
  void openEditor() { refresh(); showNormal(); raise(); activateWindow(); }
protected:
  void changeEvent(QEvent *event) override {
    MasterEqWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) refresh();
  }
private:
  void refresh() {
    setWindowTitle(uiText(titleKey_));
    reset_->setText(uiText("program.eq.reset"));
    for (int band = 0; band < 3; ++band) {
      const QString value = (gains_[band] > 0 ? QStringLiteral("+") : QString())
          + locale().toString(gains_[band] / 10.0, 'f', 1) + QStringLiteral(" dB");
      values_[band]->setText(value);
      dials_[band]->setToolTip(value);
      dials_[band]->setAccessibleDescription(value);
    }
  }
  const char *titleKey_;
  std::function<void(std::array<int, 3>)> changed_;
  std::array<int, 3> gains_{};
  std::array<QDial *, 3> dials_{};
  std::array<QLabel *, 3> values_{};
  QPushButton *reset_ = nullptr;
};

inline void updateEqButton(QPushButton *button, const std::array<int, 3> &gains,
                           const char *titleKey) {
  const bool adjusted = std::any_of(gains.begin(), gains.end(), [](int value) { return value != 0; });
  button->setProperty("eqAdjusted", adjusted);
  button->setToolTip(uiText(titleKey) + QStringLiteral(" / ")
                    + uiText(adjusted ? "item.eq.adjusted" : "item.eq.flat"));
  button->setAccessibleName(button->toolTip());
  button->style()->unpolish(button);
  button->style()->polish(button);
  button->update();
  updateVstButton(button);
}
} // namespace vtrpon
