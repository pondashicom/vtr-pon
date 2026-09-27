#include "image_add_dialog.h"

#include "controller_scaling.h"
#include "language_text.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

ImageAddDialog::ImageAddDialog(QWidget *parent)
    : QDialog(parent), descriptionLabel_(new QLabel(this)),
      settingsGroup_(new QGroupBox(this)),
      durationLabel_(new QLabel(settingsGroup_)),
      durationSpinBox_(new QDoubleSpinBox(settingsGroup_)),
      rememberCheckBox_(new QCheckBox(this)),
      buttonBox_(new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this)) {
  setObjectName(QStringLiteral("imageAddDialog"));
  setModal(true);
  setMinimumWidth(vtrpon::scalePx(540));

  descriptionLabel_->setObjectName(
      QStringLiteral("imageAddDescription"));
  descriptionLabel_->setWordWrap(true);

  settingsGroup_->setObjectName(QStringLiteral("imageSettingsGroup"));
  durationSpinBox_->setObjectName(
      QStringLiteral("imageDisplayDuration"));
  durationSpinBox_->setRange(0.1, 600.0);
  durationSpinBox_->setDecimals(1);
  durationSpinBox_->setSingleStep(0.5);
  durationSpinBox_->setValue(5.0);
  durationSpinBox_->setSuffix(QStringLiteral(" s"));
  durationLabel_->setBuddy(durationSpinBox_);

  auto *settingsLayout = new QFormLayout(settingsGroup_);
  settingsLayout->addRow(durationLabel_, durationSpinBox_);

  rememberCheckBox_->setObjectName(
      QStringLiteral("imageRememberForSession"));

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(descriptionLabel_);
  layout->addWidget(settingsGroup_);
  layout->addWidget(rememberCheckBox_);
  layout->addWidget(buttonBox_);

  connect(buttonBox_, &QDialogButtonBox::accepted, this,
          &QDialog::accept);
  connect(buttonBox_, &QDialogButtonBox::rejected, this,
          &QDialog::reject);

  retranslateUi();
  if (QPushButton *addButton =
          buttonBox_->button(QDialogButtonBox::Ok)) {
    addButton->setDefault(true);
    addButton->setAutoDefault(true);
    setTabOrder(durationSpinBox_, rememberCheckBox_);
    setTabOrder(rememberCheckBox_, addButton);
    if (QPushButton *cancelButton =
            buttonBox_->button(QDialogButtonBox::Cancel)) {
      setTabOrder(addButton, cancelButton);
    }
  }
}

ImageAddSettings ImageAddDialog::settings() const {
  return ImageAddSettings{
      .displayDurationMs = qRound64(durationSpinBox_->value() * 1000.0),
      .rememberForSession = rememberCheckBox_->isChecked(),
  };
}

void ImageAddDialog::changeEvent(QEvent *event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
}

void ImageAddDialog::retranslateUi() {
  setWindowTitle(vtrpon::uiText("image.add.title"));
  descriptionLabel_->setText(vtrpon::uiText("image.add.description"));
  settingsGroup_->setTitle(vtrpon::uiText("image.add.settings"));
  durationLabel_->setText(vtrpon::uiText("image.add.display_duration"));
  durationSpinBox_->setAccessibleName(durationLabel_->text());
  durationSpinBox_->setAccessibleDescription(descriptionLabel_->text());
  rememberCheckBox_->setText(
      vtrpon::uiText("image.add.remember_session"));
  if (QPushButton *addButton =
          buttonBox_->button(QDialogButtonBox::Ok)) {
    addButton->setText(vtrpon::uiText("common.add"));
  }
  if (QPushButton *cancelButton =
          buttonBox_->button(QDialogButtonBox::Cancel)) {
    cancelButton->setText(vtrpon::uiText("common.cancel"));
  }
}
