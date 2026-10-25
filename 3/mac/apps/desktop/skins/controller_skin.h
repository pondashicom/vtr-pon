#pragma once

#include <QList>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QStringView>

class QApplication;

namespace vtrpon {

enum class ControllerSkinPalette {
  System,
  MaterialDark,
  VtrPonClasic,
};

struct ControllerSkinDefinition {
  QString key;
  QString displayName;
  QStringList styleSheetResources;
  bool usesHardwareEffects = false;
  ControllerSkinPalette palette = ControllerSkinPalette::System;
};

const QList<ControllerSkinDefinition> &availableControllerSkins();
const ControllerSkinDefinition *controllerSkinDefinition(QStringView key);
QString defaultControllerSkinKey();
QString configuredControllerSkinKey();
QString currentControllerSkinKey();
bool currentControllerSkinUsesHardwareEffects();

// Reuse a skin's palette in a local window without changing the Controller.
QPalette controllerSkinPalette(const QApplication &application,
                               ControllerSkinPalette skinPalette);

bool applyControllerSkin(QApplication &application, QStringView key,
                         QString *errorMessage = nullptr);
bool saveControllerSkinPreference(QStringView key,
                                  QString *errorMessage = nullptr);

} // namespace vtrpon
