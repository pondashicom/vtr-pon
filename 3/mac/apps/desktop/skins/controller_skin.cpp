#include "../controller_settings.h"
#include "controller_skin.h"

#include "../controller_scaling.h"

#include <QApplication>
#include <QFile>
#include <QDebug>
#include <QIODevice>
#include <QPalette>
#include <QSettings>
#include <QWidget>

#include <algorithm>
#include <optional>

namespace {

const QList<vtrpon::ControllerSkinDefinition> &skinDefinitions() {
  static const QList<vtrpon::ControllerSkinDefinition> definitions = {
      {QStringLiteral("hardware"), QStringLiteral("Studio Hardware"),
       {QStringLiteral(":/vtrpon/apps/desktop/skins/hardware/controller.qss")},
       true, vtrpon::ControllerSkinPalette::System},
      {QStringLiteral("material-dark"), QStringLiteral("Material Dark"),
       {QStringLiteral(
           ":/vtrpon/apps/desktop/skins/material-dark/controller.qss")},
       false, vtrpon::ControllerSkinPalette::MaterialDark},
      {QStringLiteral("vtr-pon-clasic"), QStringLiteral("VTR-PON Clasic"),
       {QStringLiteral(
            ":/vtrpon/apps/desktop/skins/material-dark/controller.qss"),
        QStringLiteral(
            ":/vtrpon/apps/desktop/skins/vtr-pon-clasic/controller.qss")},
       false, vtrpon::ControllerSkinPalette::VtrPonClasic},
  };
  return definitions;
}

std::optional<QPalette> &systemPaletteStorage() {
  static std::optional<QPalette> palette;
  return palette;
}

QPalette skinPaletteFor(const QApplication &application,
                        vtrpon::ControllerSkinPalette skinPalette) {
  std::optional<QPalette> &systemPalette = systemPaletteStorage();
  if (!systemPalette) {
    systemPalette = application.palette();
  }

  QPalette palette = *systemPalette;
  if (skinPalette == vtrpon::ControllerSkinPalette::MaterialDark) {
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#111318")));
    palette.setColor(QPalette::WindowText,
                     QColor(QStringLiteral("#E3E2E8")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#1B1D23")));
    palette.setColor(QPalette::AlternateBase,
                     QColor(QStringLiteral("#24262D")));
    palette.setColor(QPalette::ToolTipBase,
                     QColor(QStringLiteral("#2F3038")));
    palette.setColor(QPalette::ToolTipText,
                     QColor(QStringLiteral("#E3E2E8")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#E3E2E8")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#2F3038")));
    palette.setColor(QPalette::ButtonText,
                     QColor(QStringLiteral("#E3E2E8")));
    palette.setColor(QPalette::BrightText,
                     QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Light, QColor(QStringLiteral("#5F636D")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#3A3C44")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#45474F")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#0B0D12")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#000000")));
    palette.setColor(QPalette::Highlight,
                     QColor(QStringLiteral("#0B57D0")));
    palette.setColor(QPalette::HighlightedText,
                     QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#A8C7FA")));
    palette.setColor(QPalette::LinkVisited,
                     QColor(QStringLiteral("#C4B5FD")));
    palette.setColor(QPalette::PlaceholderText,
                     QColor(QStringLiteral("#8E9099")));

    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     QColor(QStringLiteral("#73757E")));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     QColor(QStringLiteral("#73757E")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     QColor(QStringLiteral("#73757E")));
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     QColor(QStringLiteral("#202127")));
    palette.setColor(QPalette::Disabled, QPalette::Base,
                     QColor(QStringLiteral("#17191E")));
    palette.setColor(QPalette::Disabled, QPalette::Highlight,
                     QColor(QStringLiteral("#34363D")));
  } else if (skinPalette ==
             vtrpon::ControllerSkinPalette::VtrPonClasic) {
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#292828")));
    palette.setColor(QPalette::WindowText,
                     QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#2E2E2E")));
    palette.setColor(QPalette::AlternateBase,
                     QColor(QStringLiteral("#383838")));
    palette.setColor(QPalette::ToolTipBase,
                     QColor(QStringLiteral("#444444")));
    palette.setColor(QPalette::ToolTipText,
                     QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#D9D9D9")));
    palette.setColor(QPalette::ButtonText,
                     QColor(QStringLiteral("#141414")));
    palette.setColor(QPalette::BrightText,
                     QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Light, QColor(QStringLiteral("#A6A6A6")));
    palette.setColor(QPalette::Midlight,
                     QColor(QStringLiteral("#4F4F4F")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#555555")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#181818")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#000000")));
    palette.setColor(QPalette::Highlight,
                     QColor(QStringLiteral("#2964A3")));
    palette.setColor(QPalette::HighlightedText,
                     QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#66CD66")));
    palette.setColor(QPalette::LinkVisited,
                     QColor(QStringLiteral("#D2724B")));
    palette.setColor(QPalette::PlaceholderText,
                     QColor(QStringLiteral("#BBBBBB")));

    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     QColor(QStringLiteral("#8A8A8A")));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     QColor(QStringLiteral("#8A8A8A")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     QColor(QStringLiteral("#B0B0B0")));
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     QColor(QStringLiteral("#555555")));
    palette.setColor(QPalette::Disabled, QPalette::Base,
                     QColor(QStringLiteral("#2E2E2E")));
    palette.setColor(QPalette::Disabled, QPalette::Highlight,
                     QColor(QStringLiteral("#444444")));
  }
  return palette;
}

QString &currentSkinKeyStorage() {
  static QString key = QStringLiteral("hardware");
  return key;
}

QSettings controllerSettings() {
  return vtrpon::makeControllerSettings();
}

constexpr auto kControllerSkinSetting = "appearance/controllerSkin";

} // namespace

namespace vtrpon {

QPalette controllerSkinPalette(const QApplication &application,
                               ControllerSkinPalette skinPalette) {
  return skinPaletteFor(application, skinPalette);
}

const QList<ControllerSkinDefinition> &availableControllerSkins() {
  return skinDefinitions();
}

const ControllerSkinDefinition *controllerSkinDefinition(QStringView key) {
  const auto &definitions = skinDefinitions();
  const auto found =
      std::find_if(definitions.cbegin(), definitions.cend(),
                   [key](const ControllerSkinDefinition &definition) {
                     return QStringView(definition.key) == key;
                   });
  return found == definitions.cend() ? nullptr : &*found;
}

QString defaultControllerSkinKey() {
  return skinDefinitions().constFirst().key;
}

QString configuredControllerSkinKey() {
  const QString configuredKey =
      controllerSettings()
          .value(QString::fromLatin1(kControllerSkinSetting))
          .toString();
  return controllerSkinDefinition(configuredKey) ? configuredKey
                                                 : defaultControllerSkinKey();
}

QString currentControllerSkinKey() { return currentSkinKeyStorage(); }

bool currentControllerSkinUsesHardwareEffects() {
  const ControllerSkinDefinition *definition =
      controllerSkinDefinition(currentSkinKeyStorage());
  return definition && definition->usesHardwareEffects;
}

bool applyControllerSkin(QApplication &application, QStringView key,
                         QString *errorMessage) {
  const ControllerSkinDefinition *definition = controllerSkinDefinition(key);
  if (!definition) {
    if (errorMessage) {
      *errorMessage =
          QStringLiteral("Unknown Controller skin: %1").arg(key.toString());
    }
    return false;
  }

  QString combinedStyleSheet;
  for (const QString &styleSheetResource : definition->styleSheetResources) {
    QFile styleSheetFile(styleSheetResource);
    const bool opened = styleSheetFile.open(QIODevice::ReadOnly | QIODevice::Text);
    qInfo().noquote() << "READ_SOURCE skin" << styleSheetResource
                      << "opened=" << opened;
    if (!opened) {
      if (errorMessage) {
        *errorMessage =
            QStringLiteral("Could not load Controller skin: %1 (%2)")
                .arg(definition->key, styleSheetResource);
      }
      return false;
    }
    if (!combinedStyleSheet.isEmpty()) {
      combinedStyleSheet.append(QLatin1Char('\n'));
    }
    combinedStyleSheet.append(QString::fromUtf8(styleSheetFile.readAll()));
  }

  currentSkinKeyStorage() = definition->key;
  application.setProperty("controllerSkinKey", definition->key);
  application.setProperty("controllerSkinUsesHardwareEffects",
                          definition->usesHardwareEffects);
  application.setPalette(controllerSkinPalette(application, definition->palette));
  application.setProperty("controllerUnscaledStyleSheet", combinedStyleSheet);
  application.setStyleSheet(
      vtrpon::scaledStyleSheet(combinedStyleSheet));

  for (QWidget *widget : QApplication::allWidgets()) {
    widget->update();
  }
  if (errorMessage) {
    errorMessage->clear();
  }
  return true;
}

bool saveControllerSkinPreference(QStringView key, QString *errorMessage) {
  if (!controllerSkinDefinition(key)) {
    if (errorMessage) {
      *errorMessage =
          QStringLiteral("Unknown Controller skin: %1").arg(key.toString());
    }
    return false;
  }

  QSettings settings = controllerSettings();
  settings.setValue(QString::fromLatin1(kControllerSkinSetting),
                    key.toString());
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    if (errorMessage) {
      *errorMessage =
          QStringLiteral("Could not save the Controller skin setting.");
    }
    return false;
  }
  if (errorMessage) {
    errorMessage->clear();
  }
  return true;
}

} // namespace vtrpon
