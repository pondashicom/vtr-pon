#pragma once

#include "file_management_settings.h"
#include "controller_scaling.h"
#include "language_text.h"
#include "settings_style.h"
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <array>

class FileManagementSettingsPage final : public QWidget {
public:
  explicit FileManagementSettingsPage(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileManagementSettingsPage"));
    vtrpon::setSettingsRole(this, "page");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(vtrpon::scalePx(8));
    title_ = new QLabel(this);
    vtrpon::setSettingsRole(title_, "pageTitle");
    layout->addWidget(title_);
    description_ = new QLabel(this);
    description_->setWordWrap(true);
    vtrpon::setSettingsRole(description_, "description");
    layout->addWidget(description_);
    for (int i = 0; i < 2; ++i) {
      auto &field = fields_[i];
      field.label = new QLabel(this);
      field.label->setWordWrap(true);
      layout->addWidget(field.label);
      auto *row = new QHBoxLayout;
      field.path = new QLineEdit(this);
      field.path->setObjectName(i == 0 ? QStringLiteral("stillCaptureDirectory")
                                      : QStringLiteral("documentConversionDirectory"));
      field.label->setBuddy(field.path);
      field.browse = new QPushButton(this);
      row->addWidget(field.path, 1);
      row->addWidget(field.browse);
      layout->addLayout(row);
      connect(field.path, &QLineEdit::editingFinished, this, [this, i] { save(i); });
      connect(field.browse, &QPushButton::clicked, this, [this, i] {
        const auto &field = fields_[i];
        const auto directory = QFileDialog::getExistingDirectory(
            this, field.label->text(), field.path->text().isEmpty()
                ? field.path->placeholderText() : field.path->text());
        if (!directory.isEmpty()) {
          field.path->setText(QDir::toNativeSeparators(directory));
          save(i);
        }
      });
      layout->addSpacing(vtrpon::scalePx(12));
    }
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    layout->addStretch();
    reload();
    retranslate();
  }

  void reload() {
    auto settings = vtrpon::makeControllerSettings();
    for (int i = 0; i < 2; ++i)
      fields_[i].path->setText(QDir::toNativeSeparators(settings.value(key(i)).toString()));
    fields_[0].path->setPlaceholderText(QDir::toNativeSeparators(vtrpon::defaultStillCaptureDirectory()));
    fields_[1].path->setPlaceholderText(QDir::toNativeSeparators(vtrpon::defaultGeneratedMediaDirectory()));
  }

protected:
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) retranslate();
  }

private:
  static QString key(int i) {
    return i == 0 ? QStringLiteral("files/stillCaptureDirectory")
                  : QStringLiteral("files/documentConversionDirectory");
  }
  void save(int i) {
    auto path = QDir::fromNativeSeparators(fields_[i].path->text().trimmed());
    if (!path.isEmpty() && (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))) {
      status_->setText(vtrpon::uiText("settings.files.absolute_path"));
      return;
    }
    if (!path.isEmpty()) path = QDir::cleanPath(path);
    auto settings = vtrpon::makeControllerSettings();
    const auto previous = settings.value(key(i));
    settings.setValue(key(i), path);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
      settings.setValue(key(i), previous);
      status_->setText(vtrpon::uiText("settings.files.save_failed"));
      return;
    }
    fields_[i].path->setText(QDir::toNativeSeparators(path));
    status_->setText(vtrpon::uiText("settings.files.saved"));
  }
  void retranslate() {
    title_->setText(vtrpon::uiText("settings.files.title"));
    description_->setText(vtrpon::uiText("settings.files.description"));
    for (int i = 0; i < 2; ++i) {
      auto &field = fields_[i];
      field.label->setText(vtrpon::uiText(i == 0 ? "settings.files.still" : "settings.files.documents"));
      field.path->setAccessibleName(field.label->text());
      field.browse->setText(vtrpon::uiText("settings.files.browse"));
      field.browse->setAccessibleName(field.label->text() + QStringLiteral(" — ") + field.browse->text());
    }
    status_->clear();
  }
  struct Field { QLabel *label; QLineEdit *path; QPushButton *browse; };
  std::array<Field, 2> fields_{};
  QLabel *title_;
  QLabel *description_;
  QLabel *status_;
};
