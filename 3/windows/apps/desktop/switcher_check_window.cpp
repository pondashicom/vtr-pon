#include "switcher_check_window.h"
#include "switcher_control.h"
#include "language_manager.h"
#include "language_text.h"
#include "background_io_queue.h"
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLibrary>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <array>
#ifdef Q_OS_WIN
#include <windows.h>
#include <mapi.h>
#endif

namespace vtrpon {
namespace {
QString reportMailSubject(const QJsonObject &report) {
  return uiText("switcher.check.mail_subject").arg(report["modelName"].toString());
}

QString reportMailBody(const QJsonObject &report, const QString &fileName, bool includeComment) {
  QStringList lines{uiText("switcher.check.mail_intro"),
                    report["modelName"].toString() + " / " + report["transport"].toString(),
                    "VTR-PON: " + report["applicationVersion"].toString(),
                    "Firmware: " + report["firmwareReportedByUser"].toString()};
  const auto results = report["operatorResults"].toObject();
  for (const auto &[key, label] : std::array<std::pair<const char *, const char *>, 5>{{
           {"connection", "switcher.check.connection"}, {"inputSend", "switcher.check.send"},
           {"inputReceive", "switcher.check.receive"}, {"fade", "switcher.check.fade"},
           {"reconnect", "switcher.check.reconnect"}}}) {
    const auto result = results[QLatin1String(key)].toString();
    const char *text = result == "worked" ? "switcher.check.worked" :
                       result == "failed" ? "switcher.check.failed" :
                       result == "unavailable" ? "switcher.check.unavailable" : "switcher.check.not_tested";
    lines << uiText(label) + ": " + uiText(text);
  }
  if (includeComment && !report["operatorComment"].toString().isEmpty())
    lines << QString() << report["operatorComment"].toString();
  lines << QString() << uiText("switcher.check.mail_attachment").arg(QFileInfo(fileName).fileName());
  return lines.join(QStringLiteral("\r\n"));
}

enum class MailComposeResult { Completed, Cancelled, Unavailable };
MailComposeResult composeReportMail(const QString &subject, const QString &body, const QString &path) {
#ifdef Q_OS_WIN
  wchar_t systemDirectory[MAX_PATH]{};
  const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
  if (!length || length >= MAX_PATH) return MailComposeResult::Unavailable;
  QLibrary mapi(QString::fromWCharArray(systemDirectory) + QStringLiteral("/mapi32.dll"));
  const auto compose = reinterpret_cast<MAPISENDMAILW *>(mapi.resolve("MAPISendMailW"));
  if (!compose) return MailComposeResult::Unavailable;
  auto mailSubject = subject.toStdWString(), mailBody = body.toStdWString();
  auto filePath = QDir::toNativeSeparators(path).toStdWString();
  auto fileName = QFileInfo(path).fileName().toStdWString();
  std::wstring recipient = L"SMTP:info@pondashi.com", recipientName = L"info@pondashi.com";
  MapiRecipDescW to{};
  to.ulRecipClass = MAPI_TO; to.lpszName = recipientName.data(); to.lpszAddress = recipient.data();
  MapiFileDescW attachment{};
  attachment.nPosition = static_cast<ULONG>(-1);
  attachment.lpszPathName = filePath.data(); attachment.lpszFileName = fileName.data();
  MapiMessageW message{};
  message.lpszSubject = mailSubject.data(); message.lpszNoteText = mailBody.data();
  message.nRecipCount = 1; message.lpRecips = &to;
  message.nFileCount = 1; message.lpFiles = &attachment;
  // Always show the mail client's composer. Only the user may send the message.
  // Run on the file worker: a modal mail provider must not block the controller.
  const auto result = compose(0, 0, &message, MAPI_DIALOG | MAPI_LOGON_UI | MAPI_FORCE_UNICODE, 0);
  if (result == SUCCESS_SUCCESS) return MailComposeResult::Completed;
  if (result == MAPI_E_USER_ABORT) return MailComposeResult::Cancelled;
#else
  Q_UNUSED(subject); Q_UNUSED(body); Q_UNUSED(path);
#endif
  return MailComposeResult::Unavailable;
}

class SwitcherCheckWindow final : public QWidget {
public:
  SwitcherCheckWindow(SwitcherController *controller, UiLanguageManager *language, QWidget *parent)
      : QWidget(parent, Qt::Window), controller_(controller), fileIo_(new BackgroundIoQueue(this)) {
    setObjectName("switcherCheckWindow");
    QFont readableFont = font(); readableFont.setPointSizeF(qMax(12.0, readableFont.pointSizeF())); setFont(readableFont);
    setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(this);
    model_ = new QLabel(this); model_->setWordWrap(true); layout->addWidget(model_);
    connection_ = new QLabel(this); connection_->setWordWrap(true);
    retry_ = new QPushButton(this); retry_->setObjectName("switcherCheckRetry"); retry_->setMinimumHeight(32);
    auto *connectionRow = new QHBoxLayout; connectionRow->addWidget(connection_, 1); connectionRow->addWidget(retry_); layout->addLayout(connectionRow);
    connect(retry_, &QPushButton::clicked, this, [this] { if (controller_) controller_->retryConnection(); });
    auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll); auto *content = new QVBoxLayout(body);
    help_ = new QLabel(body); help_->setWordWrap(true); help_->setTextFormat(Qt::PlainText); content->addWidget(help_);
    auto *form = new QFormLayout; form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    firmwareLabel_ = new QLabel(body); firmware_ = new QLineEdit(body); firmware_->setMaxLength(80);
    firmware_->setObjectName("switcherCheckFirmware"); firmwareLabel_->setBuddy(firmware_); form->addRow(firmwareLabel_, firmware_);
    for (int i = 0; i < 5; ++i) {
      labels_[i] = new QLabel(body);
      answers_[i] = new QComboBox(body); answers_[i]->setObjectName(QStringLiteral("switcherCheckResult%1").arg(i));
      for (const auto &id : {"not-tested", "worked", "failed", "unavailable"}) answers_[i]->addItem(QString(), QLatin1String(id));
      labels_[i]->setBuddy(answers_[i]); form->addRow(labels_[i], answers_[i]);
      connect(answers_[i], &QComboBox::currentIndexChanged, this, [this] { storeAnswers(); refresh(); });
    }
    content->addLayout(form);
    commentLabel_ = new QLabel(body); content->addWidget(commentLabel_);
    comment_ = new QPlainTextEdit(body); comment_->setObjectName("switcherCheckComment"); comment_->setMaximumHeight(100);
    comment_->setTabChangesFocus(true); commentLabel_->setBuddy(comment_); content->addWidget(comment_);
    previewLabel_ = new QLabel(body); content->addWidget(previewLabel_);
    preview_ = new QPlainTextEdit(body); preview_->setObjectName("switcherCheckPreview"); preview_->setReadOnly(true);
    preview_->setMinimumHeight(140); preview_->setTabChangesFocus(true); content->addWidget(preview_, 1);
    scroll->setWidget(body); layout->addWidget(scroll, 1);
    status_ = new QLabel(this); status_->setWordWrap(true); layout->addWidget(status_);
    auto *buttons = new QHBoxLayout;
    start_ = new QPushButton(this); start_->setObjectName("switcherCheckStart");
    stop_ = new QPushButton(this); stop_->setObjectName("switcherCheckStop");
    save_ = new QPushButton(this); save_->setObjectName("switcherCheckSave");
    email_ = new QPushButton(this); email_->setObjectName("switcherCheckEmail");
    folder_ = new QPushButton(this); folder_->setObjectName("switcherCheckReportFolder");
    close_ = new QPushButton(this);
    for (auto *b : {start_, stop_, close_}) { b->setMinimumHeight(32); buttons->addWidget(b); }
    auto *reportButtons = new QHBoxLayout;
    for (auto *b : {save_, email_, folder_}) { b->setMinimumHeight(32); reportButtons->addWidget(b); }
    layout->addLayout(reportButtons);
    layout->addLayout(buttons);
    QWidget::setTabOrder(preview_, save_);
    QWidget::setTabOrder(save_, email_);
    QWidget::setTabOrder(email_, folder_);
    QWidget::setTabOrder(folder_, start_);
    QWidget::setTabOrder(start_, stop_);
    QWidget::setTabOrder(stop_, close_);
    connect(close_, &QPushButton::clicked, this, &QWidget::close);
    connect(start_, &QPushButton::clicked, this, [this] {
      if (!controller_ || !controller_->startCheck()) { status_->setText(uiText("switcher.check.enable_first")); return; }
      updating_ = true;
      firmware_->clear(); comment_->clear();
      for (auto *a : answers_) a->setCurrentIndex(0);
      updating_ = false; updateCapabilities(); storeAnswers(); status_->clear(); refresh();
    });
    connect(stop_, &QPushButton::clicked, this, [this] { if (controller_) controller_->stopCheck(); refresh(); });
    connect(save_, &QPushButton::clicked, this, [this] { saveReport(); });
    connect(email_, &QPushButton::clicked, this, [this] { emailReport(); });
    connect(folder_, &QPushButton::clicked, this, [this] {
      if (!lastReportPath_.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(lastReportPath_).absolutePath())))
        status_->setText(uiText("switcher.check.folder_failed").arg(lastReportPath_));
    });
    connect(firmware_, &QLineEdit::textChanged, this, [this] { storeAnswers(); });
    connect(comment_, &QPlainTextEdit::textChanged, this, [this] {
      if (comment_->toPlainText().size() > 4096) {
        const auto cursor = comment_->textCursor();
        const QSignalBlocker blocker(comment_); comment_->setPlainText(comment_->toPlainText().left(4096)); comment_->setTextCursor(cursor);
      }
      storeAnswers();
    });
    connect(controller, &QObject::destroyed, this, &QWidget::close);
    timer_.setInterval(500); connect(&timer_, &QTimer::timeout, this, [this] { refresh(); }); timer_.start();
    if (language) connect(language, &UiLanguageManager::languageChanged, this, [this] { translate(); refresh(); });
    updating_ = true;
    const auto old = controller->checkSession().report();
    firmware_->setText(old["firmwareReportedByUser"].toString()); comment_->setPlainText(old["operatorComment"].toString());
    const auto results = old["operatorResults"].toObject();
    for (int i = 0; i < 5; ++i) answers_[i]->setCurrentIndex(qMax(0, answers_[i]->findData(results.value(QLatin1String(keys_[i])).toString())));
    updating_ = false;
    translate(); updateCapabilities(); refresh();
    const auto available = screen() ? screen()->availableGeometry().size() : QSize(1000, 800);
    resize(QSize(820, 760).boundedTo(available - QSize(40, 60))); setMinimumSize(440, 420);
  }
protected:
  void closeEvent(QCloseEvent *event) override {
    if (controller_) controller_->stopCheck();
    QWidget::closeEvent(event);
  }
private:
  void updateCapabilities() {
    if (!controller_) return;
    const auto report = controller_->checkSession().report();
    const auto model = report.isEmpty() ? controller_->settings().rolandModel : rolandSwitcherModel(report["model"].toString());
    const auto &p = rolandSwitcherProfile(model);
    updating_ = true;
    for (const auto i : {2, 3}) {
      const bool supported = i == 2 ? p.receive : p.fade;
      answers_[i]->setEnabled(supported);
      if (!supported) answers_[i]->setCurrentIndex(3);
    }
    updating_ = false;
  }
  void storeAnswers() {
    if (updating_ || !controller_ || !controller_->checkSession().exists()) return;
    QJsonObject results;
    for (int i = 0; i < 5; ++i) results.insert(QLatin1String(keys_[i]), answers_[i]->currentData().toString());
    controller_->checkSession().setAnswers(firmware_->text(), results, comment_->toPlainText());
  }
  void translate() {
    setWindowTitle(uiText("switcher.check.title")); help_->setText(uiText("switcher.check.instructions"));
    firmwareLabel_->setText(uiText("switcher.check.firmware"));
    for (int i = 0; i < 5; ++i) {
      labels_[i]->setText(uiText(labelKeys_[i])); answers_[i]->setAccessibleName(labels_[i]->text());
      for (int j = 0; j < 4; ++j) answers_[i]->setItemText(j, uiText(resultKeys_[j]));
    }
    commentLabel_->setText(uiText("switcher.check.comment")); comment_->setAccessibleName(commentLabel_->text());
    previewLabel_->setText(uiText("switcher.check.preview")); preview_->setAccessibleName(previewLabel_->text());
    start_->setText(uiText("switcher.check.start")); stop_->setText(uiText("switcher.check.stop"));
    save_->setText(uiText("switcher.check.save")); close_->setText(uiText("switcher.check.close"));
    email_->setText(uiText("switcher.check.email")); email_->setToolTip(uiText("switcher.check.email_help"));
    folder_->setText(uiText("switcher.check.folder"));
    retry_->setText(uiText("switcher.check.retry"));
  }
  void refresh() {
    if (!controller_) return;
    const auto &session = controller_->checkSession(); const auto report = session.report();
    const auto model = report.isEmpty() ? controller_->settings().rolandModel : rolandSwitcherModel(report["model"].toString());
    const auto &p = rolandSwitcherProfile(model);
    model_->setText(QString::fromLatin1(p.name) + " / " + rolandTransportName(p.transport) + "\n" +
                    uiText(session.active() ? "switcher.check.recording" : "switcher.check.stopped"));
    connection_->setText(uiText(controller_->switcherReady() ? "settings.switcher.connection.ready" : "settings.switcher.connection.no_connection") +
                         " / " + controller_->switcherConnectionDetail());
    const auto settings = controller_->settings();
    start_->setEnabled(!session.active() && !saving_ && !preparingEmail_ && settings.rolandV8HdEnabled && !settings.atemEnabled);
    retry_->setEnabled(settings.rolandV8HdEnabled && !settings.atemEnabled);
    stop_->setEnabled(session.active()); save_->setEnabled(session.exists() && !saving_ && !preparingEmail_);
    email_->setEnabled(session.exists() && !session.active() && !saving_ && !preparingEmail_);
    folder_->setEnabled(!lastReportPath_.isEmpty());
    firmware_->setEnabled(session.exists()); comment_->setEnabled(session.exists());
    for (int i = 0; i < 5; ++i) answers_[i]->setEnabled(session.exists() && (i != 2 || p.receive) && (i != 3 || p.fade));
    const auto text = report.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(report).toJson());
    if (preview_->toPlainText() != text) preview_->setPlainText(text);
  }
  void saveReport() {
    if (!controller_) return;
    storeAnswers(); const auto report = controller_->checkSession().report(); if (report.isEmpty()) return;
    const auto suggested = "vtr-pon-switcher-" + report["model"].toString() + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".json";
    const auto path = QFileDialog::getSaveFileName(this, uiText("switcher.check.save"), suggested, "JSON (*.json)");
    if (path.isEmpty()) return;
    const auto data = QJsonDocument(report).toJson(); auto error = std::make_shared<QString>();
    saving_ = true; refresh();
    fileIo_->enqueue([path, data, error] {
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) *error = file.errorString();
    }, [this, path, error](const QString &workerError) {
      saving_ = false; const auto detail = workerError.isEmpty() ? *error : workerError;
      if (detail.isEmpty()) lastReportPath_ = path;
      status_->setText(detail.isEmpty() ? uiText("switcher.check.saved").arg(path) : uiText("switcher.check.save_failed").arg(detail)); refresh();
    });
  }
  void emailReport() {
    if (!controller_ || controller_->checkSession().active() || saving_ || preparingEmail_) return;
    storeAnswers(); const auto report = controller_->checkSession().report();
    if (report.isEmpty()) return;
    auto directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty()) { status_->setText(uiText("switcher.check.no_report_folder")); return; }
    directory = QDir(directory).filePath("VTR-PON/SwitcherReports");
    const auto fileName = "vtr-pon-switcher-" + report["model"].toString() + "-" +
        QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz") + "-" +
        QUuid::createUuid().toString(QUuid::Id128).left(8) + ".json";
    const auto path = QDir(directory).filePath(fileName);
    const auto data = QJsonDocument(report).toJson(); auto error = std::make_shared<QString>();
    preparingEmail_ = true; status_->setText(uiText("switcher.check.preparing_email")); refresh();
    fileIo_->enqueue([directory, path, data, error] {
      if (!QDir().mkpath(directory)) { *error = directory; return; }
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) *error = file.errorString();
    }, [this, report, path, error](const QString &workerError) {
      const auto detail = workerError.isEmpty() ? *error : workerError;
      if (!detail.isEmpty()) {
        preparingEmail_ = false; status_->setText(uiText("switcher.check.save_failed").arg(detail)); refresh(); return;
      }
      lastReportPath_ = path;
      status_->setText(uiText("switcher.check.email_ready").arg(path)); refresh();
      const auto subject = reportMailSubject(report), body = reportMailBody(report, path, true);
      auto result = std::make_shared<MailComposeResult>(MailComposeResult::Unavailable);
      fileIo_->enqueue([subject, body, path, result] { *result = composeReportMail(subject, body, path); },
                       [this, report, path, result](const QString &composeError) {
        preparingEmail_ = false;
        if (composeError.isEmpty() && *result == MailComposeResult::Cancelled)
          status_->setText(uiText("switcher.check.email_cancelled").arg(path));
        else if (composeError.isEmpty() && *result == MailComposeResult::Completed)
          status_->setText(uiText("switcher.check.email_finished").arg(path));
        else {
          const bool opened = QDesktopServices::openUrl(switcherCheckReportMailUrl(report, path));
          status_->setText(uiText(opened ? "switcher.check.email_manual_attachment" : "switcher.check.email_failed").arg(path));
        }
        refresh();
      });
    });
  }
  inline static const std::array<const char *, 5> keys_{"connection", "inputSend", "inputReceive", "fade", "reconnect"};
  inline static const std::array<const char *, 5> labelKeys_{"switcher.check.connection", "switcher.check.send", "switcher.check.receive", "switcher.check.fade", "switcher.check.reconnect"};
  inline static const std::array<const char *, 4> resultKeys_{"switcher.check.not_tested", "switcher.check.worked", "switcher.check.failed", "switcher.check.unavailable"};
  QPointer<SwitcherController> controller_;
  BackgroundIoQueue *fileIo_;
  QTimer timer_;
  QLabel *model_, *connection_, *help_, *firmwareLabel_, *commentLabel_, *previewLabel_, *status_;
  QLineEdit *firmware_;
  QPlainTextEdit *comment_, *preview_;
  std::array<QLabel *, 5> labels_{};
  std::array<QComboBox *, 5> answers_{};
  QPushButton *start_, *stop_, *save_, *email_, *folder_, *close_, *retry_;
  QString lastReportPath_;
  bool updating_ = false, saving_ = false, preparingEmail_ = false;
};
}
QUrl switcherCheckReportMailUrl(const QJsonObject &report, const QString &fileName) {
  // Keep protocol events and long comments in the JSON attachment, not in a URL.
  return QUrl::fromEncoded("mailto:info@pondashi.com?subject=" + QUrl::toPercentEncoding(reportMailSubject(report)) +
                          "&body=" + QUrl::toPercentEncoding(reportMailBody(report, fileName, false)));
}
void showSwitcherCheckWindow(SwitcherController *controller, UiLanguageManager *language, QWidget *parent) {
  if (!controller) return;
  auto *window = parent ? parent->findChild<QWidget *>("switcherCheckWindow") : nullptr;
  if (!window) window = new SwitcherCheckWindow(controller, language, parent);
  window->show(); window->raise(); window->activateWindow();
}
} // namespace vtrpon
