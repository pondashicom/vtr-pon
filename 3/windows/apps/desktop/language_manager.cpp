#include "controller_settings.h"
#include "language_manager.h"

#include "language_text.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QTranslator>

#include <algorithm>

namespace {

constexpr auto kLanguageSetting = "ui/language";

QSettings controllerSettings() {
  return vtrpon::makeControllerSettings();
}

} // namespace

namespace vtrpon {

const QList<UiLanguageDefinition> &availableUiLanguages() {
  static const QList<UiLanguageDefinition> definitions = {
      {UiLanguage::English, QStringLiteral("en"), QStringLiteral("English"),
       QStringLiteral(":/i18n/vtrpon_en.qm")},
      {UiLanguage::Japanese, QStringLiteral("ja"), QStringLiteral("日本語"),
       QStringLiteral(":/i18n/vtrpon_ja.qm")},
  };
  return definitions;
}

UiLanguageManager::UiLanguageManager(QCoreApplication *application,
                                     QObject *parent)
    : QObject(parent), application_(application) {}

UiLanguageManager::~UiLanguageManager() {
  if (application_ && translator_) {
    application_->removeTranslator(translator_.get());
  }
}

bool UiLanguageManager::initialize(QString *errorMessage) {
  if (setLanguage(configuredLanguage(), errorMessage)) {
    return true;
  }
  if (configuredLanguage() != UiLanguage::English) {
    return setLanguage(UiLanguage::English, errorMessage);
  }
  return false;
}

bool UiLanguageManager::setLanguage(UiLanguage language,
                                    QString *errorMessage) {
  const UiLanguageDefinition *selected = definition(language);
  if (!application_ || !selected) {
    if (errorMessage) {
      *errorMessage = uiText("settings.language.unknown");
    }
    return false;
  }

  auto nextTranslator = std::make_unique<QTranslator>();
  const bool loaded = nextTranslator->load(selected->translationResource);
  qInfo().noquote() << "READ_SOURCE translation" << selected->translationResource
                    << "loaded=" << loaded;
  if (!loaded) {
    if (errorMessage) {
      *errorMessage = uiText("settings.language.load_failed")
                          .arg(selected->translationResource);
    }
    return false;
  }

  if (translator_) {
    application_->removeTranslator(translator_.get());
  }
  translator_ = std::move(nextTranslator);
  application_->installTranslator(translator_.get());
  currentLanguage_ = language;
  emit languageChanged(language);

  QSettings settings = controllerSettings();
  settings.setValue(QString::fromLatin1(kLanguageSetting),
                    selected->settingsValue);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    if (errorMessage) {
      *errorMessage = uiText("settings.language.save_failed");
    }
    return false;
  }
  if (errorMessage) {
    errorMessage->clear();
  }
  return true;
}

UiLanguage UiLanguageManager::currentLanguage() const {
  return currentLanguage_;
}

const UiLanguageDefinition *
UiLanguageManager::definition(UiLanguage language) const {
  const auto &definitions = availableUiLanguages();
  const auto found = std::find_if(
      definitions.cbegin(), definitions.cend(),
      [language](const UiLanguageDefinition &candidate) {
        return candidate.language == language;
      });
  return found == definitions.cend() ? nullptr : &*found;
}

UiLanguage UiLanguageManager::configuredLanguage() const {
  const QString value =
      controllerSettings()
          .value(QString::fromLatin1(kLanguageSetting), QStringLiteral("en"))
          .toString();
  for (const UiLanguageDefinition &definition : availableUiLanguages()) {
    if (definition.settingsValue == value) {
      return definition.language;
    }
  }
  return UiLanguage::English;
}

} // namespace vtrpon
