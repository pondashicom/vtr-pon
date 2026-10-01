#pragma once

#include <QWidget>
#include <array>

class QLabel;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class QShowEvent;
namespace vtrpon::addons { struct Package; }

class AddonManagementPage final : public QWidget {
public:
  explicit AddonManagementPage(QWidget *parent = nullptr);
  void reload();
  void retranslate();

protected:
  void changeEvent(QEvent *event) override;
  void showEvent(QShowEvent *event) override;

private:
  struct Row {
    QString id;
    QLabel *title = nullptr;
    QLabel *description = nullptr;
    QLabel *status = nullptr;
    QPushButton *install = nullptr;
    QPushButton *uninstall = nullptr;
    QString availableVersion;
    QString url;
    QString sha256;
  };
  void refreshCatalog();
  void downloadAndQueue(int index);
  bool reviewAndQueue(const QString &path, const QString &expectedId,
                      const QString &expectedVersion, const QString &expectedSha256,
                      QString *error);
  bool confirmAgreement(const vtrpon::addons::Package &package);
  void setMessage(const QString &text);

  std::array<Row, 4> rows_{};
  QLabel *title_ = nullptr;
  QLabel *description_ = nullptr;
  QLabel *catalogStatus_ = nullptr;
  QLabel *message_ = nullptr;
  QPushButton *refreshButton_ = nullptr;
  QPushButton *localButton_ = nullptr;
  QNetworkAccessManager *network_ = nullptr;
  bool busy_ = false;
  bool catalogBusy_ = false;
  bool catalogRequested_ = false;
};
