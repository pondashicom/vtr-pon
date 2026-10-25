#include "platform_capabilities.h"
#include "addon_management_page.h"

#include "addon_package.h"
#include "addon_runtime_status_widget.h"
#include "controller_scaling.h"
#include "language_text.h"
#include "settings_style.h"
#include "skins/controller_skin.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QShowEvent>
#include <QTemporaryFile>
#include <QTabWidget>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const QUrl kCatalog(QStringLiteral("https://pondashi.com/vtr-pon/addon/download/catalog.json"));
const QUrl kBase(QStringLiteral("https://pondashi.com/vtr-pon/addon/download/"));

bool validDownloadUrl(const QUrl &url, const QString &id, const QString &version) {
  return url.isValid() && url.scheme() == QLatin1String("https") &&
      url.host() == kBase.host() && url.port(-1) == -1 && url.userInfo().isEmpty() &&
      url.query().isEmpty() && url.fragment().isEmpty() &&
      url.path(QUrl::FullyEncoded) == kBase.path() + id + QLatin1Char('-') + version +
          QLatin1Char('-') + vtrpon::addons::platformId() + QStringLiteral(".vtrponaddon");
}
QNetworkRequest requestFor(const QUrl &url) {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(30000);
  return request;
}
} // namespace

AddonManagementPage::AddonManagementPage(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("addonManagementPage"));
  vtrpon::setSettingsRole(this, "page");
  network_ = new QNetworkAccessManager(this);
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
  auto *catalogRow = new QHBoxLayout;
  catalogStatus_ = new QLabel(this);
  catalogStatus_->setWordWrap(true);
  vtrpon::setSettingsRole(catalogStatus_, "status");
  catalogRow->addWidget(catalogStatus_, 1);
  refreshButton_ = new QPushButton(this);
  refreshButton_->setObjectName(QStringLiteral("addonCatalogRefresh"));
  catalogRow->addWidget(refreshButton_);
  layout->addLayout(catalogRow);
  const auto ids = vtrpon::addons::ids();
  rows_.resize(ids.size());
  for (int i = 0; i < int(rows_.size()); ++i) {
    auto &row = rows_[i];
    row.id = ids.at(i);
    auto *group = new QGroupBox(this);
    group->setObjectName(QStringLiteral("addonRow%1").arg(i));
    auto *groupLayout = new QVBoxLayout(group);
    row.title = new QLabel(group);
    vtrpon::setSettingsRole(row.title, "fieldLabel");
    groupLayout->addWidget(row.title);
    row.description = new QLabel(group);
    row.description->setWordWrap(true);
    vtrpon::setSettingsRole(row.description, "description");
    groupLayout->addWidget(row.description);
    row.status = new QLabel(group);
    row.status->setWordWrap(true);
    vtrpon::setSettingsRole(row.status, "status");
    groupLayout->addWidget(row.status);
#ifdef Q_OS_MACOS
    const auto kind = i == 0 ? vtrpon::AddonRuntimeKind::Atem :
        i == 1 ? vtrpon::AddonRuntimeKind::NetworkVideo : vtrpon::AddonRuntimeKind::Vst3;
    groupLayout->addWidget(new vtrpon::AddonRuntimeStatusWidget(kind,
        vtrpon::addons::addonFilePath(QCoreApplication::applicationDirPath(), row.id), group));
#endif
    auto *actions = new QHBoxLayout;
    row.install = new QPushButton(group);
    row.install->setObjectName(QStringLiteral("addonInstall%1").arg(i));
    row.uninstall = new QPushButton(group);
    row.uninstall->setObjectName(QStringLiteral("addonUninstall%1").arg(i));
    actions->addWidget(row.install);
    actions->addWidget(row.uninstall);
    actions->addStretch();
    groupLayout->addLayout(actions);
    layout->addWidget(group);
    connect(row.install, &QPushButton::clicked, this, [this, i] { downloadAndQueue(i); });
    connect(row.uninstall, &QPushButton::clicked, this, [this, i] {
      if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::AddonManagement)) return;
      const QString root = QCoreApplication::applicationDirPath();
      const bool manual = vtrpon::addons::installed(root, rows_[i].id).isEmpty();
      if (QMessageBox::question(
              this, vtrpon::uiText("settings.addons.title"),
              vtrpon::uiText(manual ? "settings.addons.confirm_manual_uninstall" :
                                       "settings.addons.confirm_uninstall")
                  .arg(rows_[i].title->text()),
              QMessageBox::Cancel | QMessageBox::Yes,
              QMessageBox::Cancel) != QMessageBox::Yes) return;
      QString error;
      if (vtrpon::addons::queueUninstall(root, rows_[i].id, &error)) {
        setMessage(vtrpon::uiText("settings.addons.pending"));
      } else {
        setMessage(vtrpon::uiText("settings.addons.operation_failed").arg(error));
      }
      reload();
    });
  }
  auto *localRow = new QHBoxLayout;
  localButton_ = new QPushButton(this);
  localButton_->setObjectName(QStringLiteral("addonInstallLocal"));
  localRow->addWidget(localButton_);
  localRow->addStretch();
  layout->addLayout(localRow);
  message_ = new QLabel(this);
  message_->setObjectName(QStringLiteral("addonOperationStatus"));
  message_->setWordWrap(true);
  vtrpon::setSettingsRole(message_, "status");
  layout->addWidget(message_);
  layout->addStretch();
  connect(refreshButton_, &QPushButton::clicked, this,
          &AddonManagementPage::refreshCatalog);
  connect(localButton_, &QPushButton::clicked, this, [this] {
    if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::AddonManagement)) return;
    const QString path = QFileDialog::getOpenFileName(
        this, vtrpon::uiText("settings.addons.local"), {},
        QStringLiteral("VTR-PON add-on (*.vtrponaddon)"));
    if (path.isEmpty()) return;
    QString error;
    busy_ = true;
    reload();
    const bool success = reviewAndQueue(path, {}, {}, {}, &error);
    if (success)
      setMessage(vtrpon::uiText("settings.addons.pending"));
    else if (!error.isEmpty())
      setMessage(vtrpon::uiText("settings.addons.operation_failed").arg(error));
    else setMessage({});
    busy_ = false;
    reload();
    if (success) promptRestart();
  });
  retranslate();
  reload();
}

void AddonManagementPage::setMessage(const QString &text) {
  message_->setText(text);
}

void AddonManagementPage::reload() {
  const QString root = QCoreApplication::applicationDirPath();
  for (auto &row : rows_) {
    const auto record = vtrpon::addons::installed(root, row.id);
    const bool pending = QFileInfo::exists(
        vtrpon::addons::stateDirectory(root) + QLatin1Char('/') + row.id +
        QStringLiteral(".job.json"));
    const bool present = QFileInfo::exists(vtrpon::addons::addonFilePath(root, row.id));
    const QString current = record.value(QStringLiteral("manifest")).toObject()
                                .value(QStringLiteral("version")).toString();
    QString status;
    if (pending) status = vtrpon::uiText("settings.addons.waiting");
    else if (!record.isEmpty())
      status = vtrpon::uiText("settings.addons.installed").arg(current);
    else if (present) status = vtrpon::uiText("settings.addons.manual");
    else status = vtrpon::uiText("settings.addons.not_installed");
    QFile failure(vtrpon::addons::stateDirectory(root) + QLatin1Char('/') +
                  row.id + QStringLiteral(".error.txt"));
    if (!pending && failure.open(QIODevice::ReadOnly))
      status += QStringLiteral("\n") +
                vtrpon::uiText("settings.addons.last_error")
                    .arg(QString::fromUtf8(failure.readAll()).trimmed());
    if (!row.availableVersion.isEmpty())
      status += QStringLiteral("  ·  ") +
                vtrpon::uiText("settings.addons.available").arg(row.availableVersion);
    row.status->setText(status);
    const bool newer = record.isEmpty() ||
        vtrpon::addons::compareVersions(current, row.availableVersion) < 0;
    row.install->setEnabled(!busy_ && !pending && !row.url.isEmpty() &&
                            (!present || !record.isEmpty()) && newer);
    row.install->setText(vtrpon::uiText(record.isEmpty() ?
                           "settings.addons.install" : "settings.addons.update"));
    row.uninstall->setEnabled(!busy_ && !pending &&
                               (!record.isEmpty() || present));
  }
  if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::AddonManagement)) {
    for (auto &row : rows_) { row.install->setEnabled(false); row.uninstall->setEnabled(false); }
    localButton_->setEnabled(false); refreshButton_->setEnabled(false);
    catalogStatus_->setText(vtrpon::uiText(vtrpon::platformFeatureUnavailableTextId(vtrpon::PlatformFeature::AddonManagement)));
    return;
  }
  localButton_->setEnabled(!busy_);
  refreshButton_->setEnabled(!busy_ && !catalogBusy_);
}

void AddonManagementPage::retranslate() {
  title_->setText(vtrpon::uiText("settings.addons.title"));
  #ifdef Q_OS_MACOS
  description_->setText(vtrpon::uiText("settings.addons.description_mac"));
#else
  description_->setText(vtrpon::uiText("settings.addons.description"));
#endif
  refreshButton_->setText(vtrpon::uiText("settings.addons.refresh"));
  localButton_->setText(vtrpon::uiText("settings.addons.local"));
  rows_[0].title->setText(QStringLiteral("ATEM"));
  rows_[1].title->setText(QStringLiteral("NDI"));
  rows_[2].title->setText(QStringLiteral("VST3"));
  if (rows_.size() > 3) rows_[3].title->setText(QStringLiteral("DeckLink"));
  rows_[0].description->setText(vtrpon::uiText("settings.addons.atem"));
  rows_[1].description->setText(vtrpon::uiText("settings.addons.ndi"));
  rows_[2].description->setText(vtrpon::uiText("settings.addons.vst3"));
  if (rows_.size() > 3) rows_[3].description->setText(vtrpon::uiText("decklink.description"));
  for (auto &row : rows_)
    row.uninstall->setText(vtrpon::uiText("settings.addons.uninstall"));
  reload();
}

void AddonManagementPage::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) retranslate();
}

void AddonManagementPage::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  if (!catalogRequested_) {
    catalogRequested_ = true;
    refreshCatalog();
  }
}

void AddonManagementPage::refreshCatalog() {
  if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::AddonManagement)) { reload(); return; }
  if (busy_ || catalogBusy_) return;
  catalogBusy_ = true;
  catalogStatus_->setText(vtrpon::uiText("settings.addons.checking"));
  reload();
  auto *reply = network_->get(requestFor(kCatalog));
  connect(reply, &QIODevice::readyRead, reply, [reply] {
    if (reply->bytesAvailable() > 65536) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    catalogBusy_ = false;
    for (auto &row : rows_) {
      row.availableVersion.clear(); row.url.clear(); row.sha256.clear();
    }
    const QByteArray data = reply->readAll();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    bool valid = reply->error() == QNetworkReply::NoError &&
                 reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                 data.size() <= 65536 && error.error == QJsonParseError::NoError &&
                 document.isObject() && vtrpon::addons::hasUniqueJsonKeys(data);
    QVector<vtrpon::addons::CatalogEntry> entries;
    QString detail;
    if (valid) valid = vtrpon::addons::readCatalog(data, &entries, &detail);
    if (valid) for (const auto &entry : entries) for (auto &row : rows_) if (row.id == entry.id) {
      row.availableVersion = entry.version; row.sha256 = entry.sha256; row.url = entry.url;
    }
    if (!valid) {
      for (auto &row : rows_) {
        row.availableVersion.clear(); row.sha256.clear(); row.url.clear();
      }
    }
    catalogStatus_->setText(vtrpon::uiText(valid ?
        (entries.isEmpty() ? "settings.addons.catalog_no_platform" : "settings.addons.catalog_ready") : "settings.addons.catalog_unavailable"));
    reply->deleteLater();
    reload();
  });
}

void AddonManagementPage::downloadAndQueue(int index) {
  if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::AddonManagement)) return;
  if (busy_ || index < 0 || index >= int(rows_.size())) return;
  const auto row = rows_.at(index);
  if (!validDownloadUrl(QUrl(row.url), row.id, row.availableVersion)) return;
  busy_ = true;
  setMessage(vtrpon::uiText("settings.addons.downloading").arg(row.title->text()));
  reload();
  auto *reply = network_->get(requestFor(QUrl(row.url)));
  connect(reply, &QIODevice::readyRead, reply, [reply] {
    if (reply->bytesAvailable() > 300 * 1024 * 1024) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, row] {
    const QByteArray data = reply->readAll();
    QString error;
    bool success = reply->error() == QNetworkReply::NoError &&
                   reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                   data.size() <= 300 * 1024 * 1024;
    if (success) {
      QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/vtrpon-addon-XXXXXX"));
      if (!temporary.open() || temporary.write(data) != data.size() ||
          !temporary.flush()) {
        success = false;
        error = QStringLiteral("Could not save the downloaded package.");
      } else {
        success = reviewAndQueue(temporary.fileName(), row.id,
                                 row.availableVersion, row.sha256, &error);
      }
    }
    if (success) setMessage(vtrpon::uiText("settings.addons.pending"));
    else if (!error.isEmpty() || reply->error() != QNetworkReply::NoError)
      setMessage(vtrpon::uiText("settings.addons.operation_failed")
                     .arg(error.isEmpty() ? reply->errorString() : error));
    else setMessage({});
    busy_ = false;
    reply->deleteLater();
    reload();
    if (success) promptRestart();
  });
}

void AddonManagementPage::promptRestart() {
  QMessageBox question(QMessageBox::Question,
      vtrpon::uiText("settings.addons.restart_title"),
      vtrpon::uiText("settings.addons.restart_question"),
      QMessageBox::Yes | QMessageBox::No, this);
  question.setObjectName(QStringLiteral("addonRestartDialog"));
  question.setPalette(vtrpon::controllerSkinPalette(
      *qApp, vtrpon::ControllerSkinPalette::VtrPonClasic));
  question.setStyleSheet(vtrpon::settingsStyleSheet(question.objectName()));
  question.setDefaultButton(QMessageBox::No);
  question.button(QMessageBox::Yes)->setText(QStringLiteral("Yes"));
  question.button(QMessageBox::No)->setText(QStringLiteral("No"));
  if (question.exec() == QMessageBox::Yes) emit restartRequested();
}

bool AddonManagementPage::reviewAndQueue(const QString &path,
                                          const QString &expectedId,
                                          const QString &expectedVersion,
                                          const QString &expectedSha256,
                                          QString *error) {
  if (!vtrpon::platformFeatureImplemented(vtrpon::PlatformFeature::AddonManagement)) {
    if (error) *error = vtrpon::uiText(vtrpon::platformFeatureUnavailableTextId(vtrpon::PlatformFeature::AddonManagement));
    return false;
  }
  vtrpon::addons::Package package;
  if (!vtrpon::addons::readPackage(path, &package, error)) return false;
  if ((!expectedId.isEmpty() && package.manifest.value(QStringLiteral("id")).toString() != expectedId) ||
      (!expectedVersion.isEmpty() && package.manifest.value(QStringLiteral("version")).toString() != expectedVersion) ||
      (!expectedSha256.isEmpty() && package.sha256 != expectedSha256)) {
    *error = QStringLiteral("The package does not match the catalog.");
    return false;
  }
  if (package.agreements.isEmpty()) {
    *error = vtrpon::uiText("settings.addons.agreement_missing");
    return false;
  }
  if (!confirmAgreement(package)) return false;
  return vtrpon::addons::queueInstall(
      QCoreApplication::applicationDirPath(), path,
      package.manifest.value(QStringLiteral("id")).toString(),
      package.manifest.value(QStringLiteral("version")).toString(),
      package.sha256, vtrpon::addons::consentReceipt(package), error);
}

bool AddonManagementPage::confirmAgreement(const vtrpon::addons::Package &package) {
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("addonAgreementDialog"));
  dialog.setAttribute(Qt::WA_StyledBackground, true);
  dialog.setPalette(vtrpon::controllerSkinPalette(
      *qApp, vtrpon::ControllerSkinPalette::VtrPonClasic));
  dialog.setStyleSheet(vtrpon::settingsStyleSheet(dialog.objectName()));
  dialog.setWindowTitle(vtrpon::uiText("settings.addons.agreement_title"));
  dialog.resize(vtrpon::scalePx(760), vtrpon::scalePx(580));
  auto *layout = new QVBoxLayout(&dialog);
  auto *heading = new QLabel(vtrpon::uiText("settings.addons.agreement_intro")
      .arg(package.manifest.value(QStringLiteral("id")).toString(),
           package.manifest.value(QStringLiteral("version")).toString()), &dialog);
  heading->setWordWrap(true);
  layout->addWidget(heading);
  auto *documents = new QTabWidget(&dialog);
  documents->setObjectName(QStringLiteral("addonAgreementDocuments"));
  auto addDocument = [documents](const vtrpon::addons::File &file, const QString &prefix) {
    auto *viewer = new QTextEdit(documents);
    viewer->setReadOnly(true);
    viewer->setAcceptRichText(false);
    viewer->setPlainText(QString::fromUtf8(file.contents));
    documents->addTab(viewer, prefix + file.path.section(QLatin1Char('/'), -1));
  };
  for (const auto &file : package.agreements)
    addDocument(file, vtrpon::uiText("settings.addons.agreement_tab"));
  for (const auto &file : package.notices)
    addDocument(file, vtrpon::uiText("settings.addons.notice_tab"));
  layout->addWidget(documents, 1);
  auto *check = new QCheckBox(vtrpon::uiText("settings.addons.agreement_check"), &dialog);
  check->setObjectName(QStringLiteral("addonAgreementCheck"));
  layout->addWidget(check);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  auto *agree = buttons->addButton(vtrpon::uiText("settings.addons.agreement_accept"),
                                   QDialogButtonBox::AcceptRole);
  agree->setObjectName(QStringLiteral("addonAgreementAccept"));
  agree->setEnabled(false);
  connect(check, &QCheckBox::toggled, agree, &QWidget::setEnabled);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  return dialog.exec() == QDialog::Accepted && check->isChecked();
}
