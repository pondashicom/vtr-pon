#include "document_add_method_dialog.h"

#include "controller_scaling.h"
#include "language_text.h"

#include <QByteArray>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace {

QWidget *methodRow(QRadioButton *button, QLabel *description,
                   QWidget *parent) {
  auto *row = new QWidget(parent);
  auto *layout = new QVBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(vtrpon::scalePx(2));
  layout->addWidget(button);
  description->setWordWrap(true);
  description->setIndent(vtrpon::scalePx(22));
  layout->addWidget(description);
  return row;
}

QString upperFirst(QString text) {
  if (!text.isEmpty()) {
    text[0] = text.at(0).toUpper();
  }
  return text;
}

} // namespace

DocumentAddMethodDialog::DocumentAddMethodDialog(
    const QString &translationPrefix, const QString &objectNamePrefix,
    QWidget *parent)
    : DocumentAddMethodDialog(translationPrefix, objectNamePrefix,
                              QStringLiteral("page"), parent) {}

DocumentAddMethodDialog::DocumentAddMethodDialog(
    const QString &translationPrefix, const QString &objectNamePrefix,
    const QString &pageName, QWidget *parent)
    : QDialog(parent), translationPrefix_(translationPrefix),
      objectNamePrefix_(objectNamePrefix), pageName_(pageName),
      descriptionLabel_(new QLabel(this)), methodGroup_(new QGroupBox(this)),
      methodButtons_(new QButtonGroup(this)),
      nativeButton_(new QRadioButton(methodGroup_)),
      nativeDescription_(new QLabel(methodGroup_)),
      videoButton_(new QRadioButton(methodGroup_)),
      videoDescription_(new QLabel(methodGroup_)),
      pngButton_(new QRadioButton(methodGroup_)),
      pngDescription_(new QLabel(methodGroup_)),
      timingSettingsGroup_(new QGroupBox(this)),
      pageDurationLabel_(new QLabel(timingSettingsGroup_)),
      pageDurationSpinBox_(new QDoubleSpinBox(timingSettingsGroup_)),
      crossDissolveCheckBox_(new QCheckBox(timingSettingsGroup_)),
      crossDissolveDurationLabel_(new QLabel(timingSettingsGroup_)),
      crossDissolveDurationSpinBox_(
          new QDoubleSpinBox(timingSettingsGroup_)),
      rememberCheckBox_(new QCheckBox(this)),
      buttonBox_(new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this)) {
  setObjectName(prefixedObjectName(QStringLiteral("AddMethodDialog")));
  setModal(true);
  setMinimumWidth(vtrpon::scalePx(620));

  descriptionLabel_->setWordWrap(true);
  methodButtons_->addButton(nativeButton_,
                            static_cast<int>(DocumentAddMethod::Native));
  methodButtons_->addButton(
      videoButton_, static_cast<int>(DocumentAddMethod::VideoConversion));
  methodButtons_->addButton(
      pngButton_, static_cast<int>(DocumentAddMethod::PngSequence));
  nativeButton_->setObjectName(
      prefixedObjectName(QStringLiteral("NativeMethod")));
  videoButton_->setObjectName(
      prefixedObjectName(QStringLiteral("VideoMethod")));
  pngButton_->setObjectName(
      prefixedObjectName(QStringLiteral("PngMethod")));
  nativeButton_->setChecked(true);

  auto *methodLayout = new QVBoxLayout(methodGroup_);
  methodLayout->addWidget(
      methodRow(nativeButton_, nativeDescription_, methodGroup_));
  methodLayout->addWidget(
      methodRow(videoButton_, videoDescription_, methodGroup_));
  methodLayout->addWidget(
      methodRow(pngButton_, pngDescription_, methodGroup_));

  pageDurationSpinBox_->setObjectName(prefixedObjectName(
      upperFirst(pageName_) + QStringLiteral("Duration")));
  pageDurationSpinBox_->setRange(0.1, 600.0);
  pageDurationSpinBox_->setDecimals(1);
  pageDurationSpinBox_->setSingleStep(0.5);
  pageDurationSpinBox_->setValue(5.0);
  pageDurationSpinBox_->setSuffix(QStringLiteral(" s"));
  pageDurationLabel_->setBuddy(pageDurationSpinBox_);

  crossDissolveCheckBox_->setObjectName(
      prefixedObjectName(QStringLiteral("CrossDissolve")));
  crossDissolveCheckBox_->setChecked(true);
  crossDissolveDurationSpinBox_->setObjectName(
      prefixedObjectName(QStringLiteral("CrossDissolveDuration")));
  crossDissolveDurationSpinBox_->setRange(0.1,
                                          pageDurationSpinBox_->value());
  crossDissolveDurationSpinBox_->setDecimals(1);
  crossDissolveDurationSpinBox_->setSingleStep(0.1);
  crossDissolveDurationSpinBox_->setValue(1.0);
  crossDissolveDurationSpinBox_->setSuffix(QStringLiteral(" s"));
  crossDissolveDurationLabel_->setBuddy(
      crossDissolveDurationSpinBox_);
  auto *timingSettingsLayout = new QFormLayout(timingSettingsGroup_);
  timingSettingsLayout->addRow(pageDurationLabel_, pageDurationSpinBox_);
  timingSettingsLayout->addRow(crossDissolveCheckBox_);
  timingSettingsLayout->addRow(crossDissolveDurationLabel_,
                               crossDissolveDurationSpinBox_);

  rememberCheckBox_->setObjectName(
      prefixedObjectName(QStringLiteral("RememberForSession")));
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(descriptionLabel_);
  layout->addWidget(methodGroup_);
  layout->addWidget(timingSettingsGroup_);
  layout->addWidget(rememberCheckBox_);
  layout->addWidget(buttonBox_);

  connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(methodButtons_, &QButtonGroup::idToggled, this,
          [this](int, bool checked) {
            if (checked) {
              updateConversionControls();
            }
          });
  connect(crossDissolveCheckBox_, &QCheckBox::toggled, this,
          [this] { updateConversionControls(); });
  connect(pageDurationSpinBox_,
          qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double duration) {
            crossDissolveDurationSpinBox_->setMaximum(duration);
          });

  retranslateUi();
  updateConversionControls();
}

DocumentAddSettings DocumentAddMethodDialog::documentSettings() const {
  DocumentAddSettings result;
  const int methodId = methodButtons_->checkedId();
  if (methodId == static_cast<int>(DocumentAddMethod::VideoConversion)) {
    result.method = DocumentAddMethod::VideoConversion;
  } else if (methodId ==
             static_cast<int>(DocumentAddMethod::PngSequence)) {
    result.method = DocumentAddMethod::PngSequence;
  }
  result.pageDurationSeconds = pageDurationSpinBox_->value();
  result.crossDissolveEnabled = crossDissolveCheckBox_->isChecked();
  result.crossDissolveDurationSeconds =
      crossDissolveDurationSpinBox_->value();
  result.rememberForSession = rememberCheckBox_->isChecked();
  return result;
}

void DocumentAddMethodDialog::changeEvent(QEvent *event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
}

QString DocumentAddMethodDialog::translated(const char *suffix) const {
  const QByteArray id =
      (translationPrefix_ + QLatin1Char('.') + QLatin1String(suffix))
          .toUtf8();
  return vtrpon::uiText(id.constData());
}

QString DocumentAddMethodDialog::prefixedObjectName(
    const QString &suffix) const {
  return objectNamePrefix_ + suffix;
}

void DocumentAddMethodDialog::updateConversionControls() {
  const bool video = videoButton_->isChecked();
  const bool pngSequence = pngButton_->isChecked();
  pageDurationSpinBox_->setAccessibleDescription(
      pngSequence ? pngDescription_->text() : videoDescription_->text());
  timingSettingsGroup_->setEnabled(video || pngSequence);
  pageDurationLabel_->setEnabled(video || pngSequence);
  pageDurationSpinBox_->setEnabled(video || pngSequence);
  crossDissolveCheckBox_->setEnabled(video);
  crossDissolveDurationLabel_->setEnabled(
      video && crossDissolveCheckBox_->isChecked());
  crossDissolveDurationSpinBox_->setEnabled(
      video && crossDissolveCheckBox_->isChecked());
}

void DocumentAddMethodDialog::retranslateUi() {
  setWindowTitle(translated("title"));
  descriptionLabel_->setText(translated("description"));
  methodGroup_->setTitle(translated("method_group"));
  nativeButton_->setText(translated("native"));
  nativeDescription_->setText(translated("native.description"));
  videoButton_->setText(translated("video"));
  videoDescription_->setText(translated("video.description"));
  pngButton_->setText(translated("png"));
  pngDescription_->setText(translated("png.description"));
  timingSettingsGroup_->setTitle(translated("video_settings"));
  const QByteArray pageDurationSuffix =
      (pageName_ + QStringLiteral("_duration")).toUtf8();
  pageDurationLabel_->setText(translated(pageDurationSuffix.constData()));
  pageDurationSpinBox_->setAccessibleName(pageDurationLabel_->text());
  pageDurationSpinBox_->setAccessibleDescription(
      pngButton_->isChecked() ? pngDescription_->text()
                              : videoDescription_->text());
  crossDissolveCheckBox_->setText(translated("cross_dissolve"));
  crossDissolveDurationLabel_->setText(
      translated("cross_dissolve_duration"));
  crossDissolveDurationSpinBox_->setAccessibleName(
      crossDissolveDurationLabel_->text());
  rememberCheckBox_->setText(translated("remember_session"));
  if (QPushButton *addButton = buttonBox_->button(QDialogButtonBox::Ok)) {
    addButton->setText(vtrpon::uiText("common.add"));
  }
  if (QPushButton *cancelButton =
          buttonBox_->button(QDialogButtonBox::Cancel)) {
    cancelButton->setText(vtrpon::uiText("common.cancel"));
  }
}
