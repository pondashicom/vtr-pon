#pragma once

#include <QDialog>
#include <QtGlobal>

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QEvent;
class QGroupBox;
class QLabel;

struct ImageAddSettings {
  qint64 displayDurationMs = 5000;
  bool rememberForSession = false;

  bool operator==(const ImageAddSettings &) const = default;
};

class ImageAddDialog final : public QDialog {
public:
  explicit ImageAddDialog(QWidget *parent = nullptr);

  ImageAddSettings settings() const;

protected:
  void changeEvent(QEvent *event) override;

private:
  void retranslateUi();

  QLabel *descriptionLabel_ = nullptr;
  QGroupBox *settingsGroup_ = nullptr;
  QLabel *durationLabel_ = nullptr;
  QDoubleSpinBox *durationSpinBox_ = nullptr;
  QCheckBox *rememberCheckBox_ = nullptr;
  QDialogButtonBox *buttonBox_ = nullptr;
};
