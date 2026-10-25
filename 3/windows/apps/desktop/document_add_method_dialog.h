#pragma once

#include <QDialog>
#include <QString>

class QButtonGroup;
class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QEvent;
class QGroupBox;
class QLabel;
class QRadioButton;

enum class DocumentAddMethod {
  Native,
  VideoConversion,
  PngSequence,
};

struct DocumentAddSettings {
  DocumentAddMethod method = DocumentAddMethod::Native;
  double pageDurationSeconds = 5.0;
  bool crossDissolveEnabled = true;
  double crossDissolveDurationSeconds = 1.0;
  bool rememberForSession = false;

  bool operator==(const DocumentAddSettings &) const = default;
};

class DocumentAddMethodDialog : public QDialog {
public:
  DocumentAddMethodDialog(const QString &translationPrefix,
                          const QString &objectNamePrefix,
                          QWidget *parent = nullptr);
  DocumentAddMethodDialog(const QString &translationPrefix,
                          const QString &objectNamePrefix,
                          const QString &pageName,
                          QWidget *parent = nullptr);

  DocumentAddSettings documentSettings() const;

protected:
  void changeEvent(QEvent *event) override;

private:
  QString translated(const char *suffix) const;
  QString prefixedObjectName(const QString &suffix) const;
  void updateConversionControls();
  void retranslateUi();

  QString translationPrefix_;
  QString objectNamePrefix_;
  QString pageName_;
  QLabel *descriptionLabel_ = nullptr;
  QGroupBox *methodGroup_ = nullptr;
  QButtonGroup *methodButtons_ = nullptr;
  QRadioButton *nativeButton_ = nullptr;
  QLabel *nativeDescription_ = nullptr;
  QRadioButton *videoButton_ = nullptr;
  QLabel *videoDescription_ = nullptr;
  QRadioButton *pngButton_ = nullptr;
  QLabel *pngDescription_ = nullptr;
  QGroupBox *timingSettingsGroup_ = nullptr;
  QLabel *pageDurationLabel_ = nullptr;
  QDoubleSpinBox *pageDurationSpinBox_ = nullptr;
  QCheckBox *crossDissolveCheckBox_ = nullptr;
  QLabel *crossDissolveDurationLabel_ = nullptr;
  QDoubleSpinBox *crossDissolveDurationSpinBox_ = nullptr;
  QCheckBox *rememberCheckBox_ = nullptr;
  QDialogButtonBox *buttonBox_ = nullptr;
};
