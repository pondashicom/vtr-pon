#pragma once
#include "controller_operations.h"
#include <QApplication>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QPointer>
#include <QWheelEvent>
#include <QWidget>

class ControllerVolumeInput final : public QObject {
public:
  explicit ControllerVolumeInput(ControllerOperations *operations, QWidget *window, QObject *parent = nullptr)
      : QObject(parent), operations_(operations), window_(window) { qApp->installEventFilter(this); }
private:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (!operations_ || !qobject_cast<QWidget *>(watched)) return false;
    const bool keyEvent = event->type() == QEvent::KeyPress
        || event->type() == QEvent::ShortcutOverride;
    if (!keyEvent && event->type() != QEvent::Wheel) return false;
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!window_ || widget->window() != window_ || QApplication::activeModalWidget()) return false;
    if (widget->testAttribute(Qt::WA_InputMethodEnabled) ||
        widget->inputMethodQuery(Qt::ImEnabled).toBool()) return false;
    for (auto *focus = QApplication::focusWidget(); focus; focus = focus->parentWidget()) {
      if (focus->testAttribute(Qt::WA_InputMethodEnabled) ||
          focus->inputMethodQuery(Qt::ImEnabled).toBool() || qobject_cast<QKeySequenceEdit *>(focus)) return false;
    }
    const auto modifiers = static_cast<QInputEvent *>(event)->modifiers();
    if (modifiers != Qt::ControlModifier && modifiers != Qt::ShiftModifier)
      return false;
    const bool item = modifiers == Qt::ControlModifier;
    int steps = 0;
    if (keyEvent) {
      const int key = static_cast<QKeyEvent *>(event)->key();
      if (key != Qt::Key_Up && key != Qt::Key_Down) return false;
      if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
      }
      steps = key == Qt::Key_Up ? 1 : -1;
    } else {
      const auto *wheel = static_cast<QWheelEvent *>(event);
      // Windows can translate Shift+vertical wheel into a horizontal delta.
      const int delta = wheel->angleDelta().y() != 0
          ? wheel->angleDelta().y() : wheel->angleDelta().x();
      int &remainder = item ? itemWheelRemainder_ : masterWheelRemainder_;
      remainder += delta;
      steps = remainder / 120;
      remainder %= 120;
    }
    if (steps != 0) {
      using Op = ControllerOperations::Action;
      (void)operations_->executeFrom("Shortcut", item ? Op::AdjustItemVolume : Op::AdjustMasterVolume,
                                 steps * 10);
    }
    event->accept();
    return true;
  }

  QPointer<ControllerOperations> operations_;
  QPointer<QWidget> window_;
  int itemWheelRemainder_ = 0;
  int masterWheelRemainder_ = 0;

};
