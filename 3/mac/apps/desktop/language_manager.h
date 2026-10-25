#pragma once

// Runtime display-language selection and persistence.

#include <QList>
#include <QObject>
#include <QString>

#include <memory>

class QCoreApplication;
class QTranslator;

namespace vtrpon {

enum class UiLanguage {
  English,
  Japanese,
};

struct UiLanguageDefinition {
  UiLanguage language;
  QString settingsValue;
  QString nativeDisplayName;
  QString translationResource;
};

const QList<UiLanguageDefinition> &availableUiLanguages();

class UiLanguageManager final : public QObject {
  Q_OBJECT

public:
  explicit UiLanguageManager(QCoreApplication *application,
                             QObject *parent = nullptr);
  ~UiLanguageManager() override;

  bool initialize(QString *errorMessage = nullptr);
  bool setLanguage(UiLanguage language, QString *errorMessage = nullptr);
  UiLanguage currentLanguage() const;

signals:
  void languageChanged(vtrpon::UiLanguage language);

private:
  const UiLanguageDefinition *definition(UiLanguage language) const;
  UiLanguage configuredLanguage() const;

  QCoreApplication *application_ = nullptr;
  std::unique_ptr<QTranslator> translator_;
  UiLanguage currentLanguage_ = UiLanguage::English;
};

} // namespace vtrpon
