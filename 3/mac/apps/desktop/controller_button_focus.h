#pragma once

#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

namespace vtrpon {

inline void preserveFocusOnComboBoxClick(QWidget *panel) {
  // Return popup focus to the original widget after a mouse selection.
  // Tab still allows deliberate keyboard access to the dropdown.
  for (auto *combo : panel->findChildren<QComboBox *>()) {
    if (combo->window() == panel->window() && !combo->isEditable() &&
        combo->focusPolicy() != Qt::NoFocus)
      combo->setFocusPolicy(Qt::TabFocus);
  }
}

inline void preserveFocusOnButtonClick(QWidget *panel) {
  // Mouse operations must leave playlist navigation (or text editing) focused.
  // Keep the existing Tab order for deliberate keyboard access to buttons.
  for (auto *button : panel->findChildren<QPushButton *>()) {
    if (button->window() == panel->window() &&
        button->focusPolicy() != Qt::NoFocus)
      button->setFocusPolicy(Qt::TabFocus);
  }
  for (auto *button : panel->findChildren<QToolButton *>()) {
    if (button->window() == panel->window() &&
        button->focusPolicy() != Qt::NoFocus)
      button->setFocusPolicy(Qt::TabFocus);
  }
}

} // namespace vtrpon
