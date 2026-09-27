#pragma once

#include <QString>
#include <QStyle>
#include <QVariant>
#include <QWidget>
#include <QGroupBox>
#include <QVBoxLayout>
#include "controller_scaling.h"
#include "language_text.h"

namespace vtrpon {

// The dialog owns the stylesheet. Pages only assign semantic roles; objectName
// remains available for automation and behavior. See settings.qss for the roles.
QString settingsStyleSheet();

inline QGroupBox *createSettingsGroup(QWidget *parent, const char *titleKey) {
  auto *group = new QGroupBox(uiText(titleKey), parent);
  group->setProperty("settingsTitleKey", QString::fromLatin1(titleKey));
  return group;
}

inline QVBoxLayout *addSettingsGroup(QVBoxLayout *pageLayout,
                                    const char *titleKey, bool expanding = false) {
  auto *group = createSettingsGroup(pageLayout->parentWidget(), titleKey);
  if (!expanding) group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto *layout = new QVBoxLayout(group);
  layout->setContentsMargins(scalePx(16), scalePx(16), scalePx(16), scalePx(16));
  layout->setSpacing(scalePx(12));
  pageLayout->addWidget(group, expanding ? 1 : 0);
  if (!expanding) pageLayout->addStretch();
  return layout;
}

inline void retranslateSettingsGroups(QWidget *page) {
  for (auto *group : page->findChildren<QGroupBox *>()) {
    const auto key = group->property("settingsTitleKey").toByteArray();
    if (!key.isEmpty()) group->setTitle(uiText(key.constData()));
  }
}

inline void setSettingsRole(QWidget *widget, const char *role) {
  widget->setProperty("settingsRole", QString::fromLatin1(role));
  if (qstrcmp(role, "page") == 0 || qstrcmp(role, "card") == 0)
    widget->setAttribute(Qt::WA_StyledBackground, true);
  if (widget->testAttribute(Qt::WA_WState_Polished)) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
  }
}

} // namespace vtrpon
