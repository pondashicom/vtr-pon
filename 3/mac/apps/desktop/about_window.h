#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;

namespace vtrpon {

class AboutWindow final : public QDialog {
public:
  explicit AboutWindow(QWidget *parent = nullptr);
  void retranslate();

private:
  void openLocalDocument(const QString &name);

  QLabel *version_ = nullptr;
  QLabel *status_ = nullptr;
  QPushButton *license_ = nullptr;
  QPushButton *notices_ = nullptr;
  QPushButton *website_ = nullptr;
};

} // namespace vtrpon
