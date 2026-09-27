#include "update_notification.h"
#include "controller_settings.h"
#include "product_identity.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace {

const QUrl kUpdatesUrl(QStringLiteral("https://pondashi.com/vtr-pon/updates.json"));
const QUrl kDownloadUrl(QStringLiteral("https://pondashi.com/vtr-pon/download/windows/"));
const QUrl kActivityUrl(QStringLiteral("https://pondashi.com/vtr-pon/metrics/activity.php"));
constexpr qsizetype kMaxCatalogBytes = 256 * 1024;

} // namespace

namespace vtrpon {

UpdateChecker::UpdateChecker(const QString &currentVersion,
                             Completion completion, QObject *parent)
    : QObject(parent), currentVersion_(currentVersion),
      completion_(std::move(completion)) {}

void UpdateChecker::check(bool manual) {
  if (!manual && !makeControllerSettings()
                     .value(QStringLiteral("updates/checkAutomatically"), true).toBool())
    return;
  sendActivityNoticeIfDue();
  if (reply_) {
    manualPending_ |= manual;
    return;
  }
  manualPending_ = manual;
  QNetworkRequest request(kUpdatesUrl);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::AlwaysNetwork);
  request.setTransferTimeout(10000);
  request.setRawHeader("Accept", "application/json");
  auto *reply = network_.get(request);
  reply_ = reply;
  QTimer::singleShot(10000, reply, [reply] {
    if (!reply->isFinished()) reply->abort();
  });
  auto body = std::make_shared<QByteArray>();
  auto tooLarge = std::make_shared<bool>(false);
  connect(reply, &QIODevice::readyRead, this, [reply, body, tooLarge] {
    body->append(reply->readAll());
    if (body->size() > kMaxCatalogBytes) {
      *tooLarge = true;
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, body, tooLarge] {
    body->append(reply->readAll());
    const bool manual = manualPending_;
    manualPending_ = false;
    reply_ = nullptr;
    UpdateCatalog catalog;
    UpdateCheckStatus status = UpdateCheckStatus::Failed;
    std::optional<UpdateRelease> release;
    if (!*tooLarge && body->size() <= kMaxCatalogBytes &&
        reply->error() == QNetworkReply::NoError &&
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
        parseUpdateCatalog(*body, &catalog) &&
        compareSemVer(currentVersion_, currentVersion_)) {
      release = newerUpdate(catalog, currentVersion_);
      status = release ? UpdateCheckStatus::Available : UpdateCheckStatus::Current;
    }
    reply->deleteLater();
    completion_(manual, status, release);
  });
}

void UpdateChecker::sendActivityNoticeIfDue() {
  if (activityPending_ ||
      !isDistributionDirectory(QCoreApplication::applicationDirPath())) return;
  auto settings = makeControllerSettings();
  if (!settings.value(QStringLiteral("privacy/sendActivityNotice"), false).toBool()) return;
  const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
  if (settings.value(QStringLiteral("privacy/activityLastSentUtcDay")).toString() == today)
    return;
  QNetworkRequest request(kActivityUrl);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  request.setTransferTimeout(5000);
  auto *activityReply = network_.post(request,
      QJsonDocument(QJsonObject{{QStringLiteral("version"), currentVersion_}})
          .toJson(QJsonDocument::Compact));
  activityPending_ = true;
  connect(activityReply, &QNetworkReply::finished, this,
          [this, activityReply, today] {
    activityPending_ = false;
    if (activityReply->error() == QNetworkReply::NoError &&
        activityReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 204) {
      auto settings = makeControllerSettings();
      settings.setValue(QStringLiteral("privacy/activityLastSentUtcDay"), today);
      settings.sync();
    }
    activityReply->deleteLater();
  });
}

UpdateWindow::UpdateWindow(const QString &currentVersion,
                           std::function<bool()> japanese, QWidget *parent)
    : QWidget(parent, Qt::Window), currentVersion_(currentVersion),
      japanese_(std::move(japanese)) {
  setObjectName(QStringLiteral("updateWindow"));
  setAttribute(Qt::WA_StyledBackground, true);
  setMinimumSize(480, 160);
  resize(620, 380);
  setWindowModality(Qt::NonModal);
  auto *layout = new QVBoxLayout(this);
  heading_ = new QLabel(this);
  heading_->setObjectName(QStringLiteral("updateHeading"));
  heading_->setWordWrap(true);
  layout->addWidget(heading_);
  version_ = new QLabel(this);
  version_->setWordWrap(true);
  layout->addWidget(version_);
  notes_ = new QPlainTextEdit(this);
  notes_->setObjectName(QStringLiteral("updateReleaseNotes"));
  notes_->setReadOnly(true);
  layout->addWidget(notes_);
  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  download_ = new QPushButton(this);
  download_->setObjectName(QStringLiteral("updateDownloadButton"));
  connect(download_, &QPushButton::clicked, this,
          [] { QDesktopServices::openUrl(kDownloadUrl); });
  buttons->addWidget(download_);
  close_ = new QPushButton(this);
  connect(close_, &QPushButton::clicked, this, &QWidget::hide);
  buttons->addWidget(close_);
  layout->addLayout(buttons);
  retranslate();
}

bool UpdateWindow::japanese() const {
  return japanese_ && japanese_();
}

void UpdateWindow::setResult(UpdateCheckStatus status,
                             const std::optional<UpdateRelease> &release) {
  status_ = status;
  release_ = release;
  retranslate();
  resize(status_ == UpdateCheckStatus::Available ? QSize(620, 380)
                                                  : QSize(500, 180));
}

void UpdateWindow::retranslate() {
  const bool ja = japanese();
  setWindowTitle(ja ? QStringLiteral("VTR-PON の更新") : QStringLiteral("VTR-PON Updates"));
  close_->setText(ja ? QStringLiteral("閉じる") : QStringLiteral("Close"));
  download_->setText(ja ? QStringLiteral("ダウンロードページを開く")
                        : QStringLiteral("Open Download Page"));
  download_->setVisible(status_ == UpdateCheckStatus::Available);
  if (status_ == UpdateCheckStatus::Available && release_) {
    heading_->setText(ja ? QStringLiteral("新しいバージョンを利用できます")
                         : QStringLiteral("A new version is available"));
    version_->setText((ja ? QStringLiteral("現在: %1   新版: %2   公開日: %3")
                         : QStringLiteral("Current: %1   New: %2   Published: %3"))
                          .arg(currentVersion_, release_->version, release_->publishedAt));
    QStringList lines;
    for (const QString &note : ja ? release_->notesJa : release_->notesEn)
      lines.append(QStringLiteral("• ") + note);
    notes_->setPlainText(lines.join(QLatin1Char('\n')));
  } else {
    heading_->setText(status_ == UpdateCheckStatus::Failed
                          ? (ja ? QStringLiteral("更新を確認できませんでした")
                                : QStringLiteral("Could not check for updates"))
                          : (ja ? QStringLiteral("最新のバージョンです")
                                : QStringLiteral("You are up to date")));
    version_->setText((ja ? QStringLiteral("現在のバージョン: %1")
                          : QStringLiteral("Current version: %1")).arg(currentVersion_));
    notes_->setPlainText(status_ == UpdateCheckStatus::Failed
                             ? (ja ? QStringLiteral("ネットワーク接続を確認して、後でもう一度お試しください。")
                                   : QStringLiteral("Check your network connection and try again later."))
                             : QString());
  }
  notes_->setVisible(status_ != UpdateCheckStatus::Current);
}

} // namespace vtrpon
