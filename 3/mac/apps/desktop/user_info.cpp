#include "user_info.h"
#include <QDebug>

#include "controller_scaling.h"
#include "language_text.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>

namespace {

QString severityName(UserInfoSeverity severity) {
  switch (severity) {
  case UserInfoSeverity::None:
    return QString();
  case UserInfoSeverity::Success:
    return vtrpon::uiText("info.severity.success");
  case UserInfoSeverity::Warning:
    return vtrpon::uiText("info.severity.warning");
  case UserInfoSeverity::Error:
    return vtrpon::uiText("info.severity.error");
  }
  return QString();
}

QString severityProperty(UserInfoSeverity severity) {
  switch (severity) {
  case UserInfoSeverity::None:
    return QStringLiteral("none");
  case UserInfoSeverity::Success:
    return QStringLiteral("success");
  case UserInfoSeverity::Warning:
    return QStringLiteral("warning");
  case UserInfoSeverity::Error:
    return QStringLiteral("error");
  }
  return QStringLiteral("none");
}

QString sourceName(const QString &source) {
  if (source == QStringLiteral("SYSTEM")) {
    return vtrpon::uiText("info.source.system");
  }
  if (source == QStringLiteral("PLAYLIST")) {
    return vtrpon::uiText("info.source.playlist");
  }
  if (source == QStringLiteral("PROGRAM")) {
    return vtrpon::uiText("info.source.program");
  }
  if (source == QStringLiteral("EDIT")) {
    return vtrpon::uiText("info.source.edit");
  }
  if (source == QStringLiteral("UVC")) {
    return vtrpon::uiText("info.source.uvc");
  }
  return source;
}

int severityPriority(UserInfoSeverity severity) {
  switch (severity) {
  case UserInfoSeverity::None:
    return 0;
  case UserInfoSeverity::Success:
    return 1;
  case UserInfoSeverity::Warning:
    return 2;
  case UserInfoSeverity::Error:
    return 3;
  }
  return 0;
}

void repolish(QWidget *widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

} // namespace

UserInfoCenter::UserInfoCenter(QObject *parent)
    : QObject(parent), displayTimer_(new QTimer(this)) {
  displayTimer_->setSingleShot(true);
  connect(displayTimer_, &QTimer::timeout, this,
          &UserInfoCenter::presentNextPendingMessage);
}

UserInfoSeverity UserInfoCenter::severity() const { return severity_; }

QString UserInfoCenter::source() const { return source_; }

QString UserInfoCenter::text() const { return text_.resolve(); }

QString UserInfoCenter::detail() const { return detail_; }

bool UserInfoCenter::hasAudioSettingsAction() const { return audioSettingsAction_; }
bool UserInfoCenter::hasPflSameDeviceConfirmation() const {
  return pflSameDeviceConfirmation_;
}

void UserInfoCenter::showPflSameDeviceConfirmation() {
  Message message;
  message.severity = UserInfoSeverity::Warning;
  message.source = QStringLiteral("EDIT");
  message.text = vtrpon::LocalizedText("edit.pfl.same_device_confirm");
  message.audioSettingsAction = true;
  message.pflSameDeviceConfirmation = true;
  present(message);
}

void UserInfoCenter::resolvePflSameDeviceConfirmation(bool accepted) {
  if (!pflSameDeviceConfirmation_) return;
  presentNextPendingMessage();
  emit pflSameDeviceDecision(accepted);
}

void UserInfoCenter::cancelPflSameDeviceConfirmation() {
  if (pflSameDeviceConfirmation_) presentNextPendingMessage();
}

void UserInfoCenter::showAudioOutputWarning(const vtrpon::LocalizedText &text,
                                           const QString &detail) {
  publish(UserInfoSeverity::Warning, QStringLiteral("SYSTEM"), text, detail, true);
}

void UserInfoCenter::showAudioOutputError(const vtrpon::LocalizedText &text,
                                         const QString &detail) {
  publish(UserInfoSeverity::Error, QStringLiteral("SYSTEM"), text, detail, true);
}

void UserInfoCenter::activateAudioSettings() {
  if (!audioSettingsAction_) return;
  if (pflSameDeviceConfirmation_) resolvePflSameDeviceConfirmation(false);
  emit audioSettingsRequested();
}

void UserInfoCenter::showSuccess(const QString &source, const QString &text,
                                 const QString &detail) {
  showSuccess(source, vtrpon::LocalizedText::fromRaw(text), detail);
}

void UserInfoCenter::showSuccess(const QString &source,
                                 const vtrpon::LocalizedText &text,
                                 const QString &detail) {
  publish(UserInfoSeverity::Success, source, text, detail);
}

void UserInfoCenter::showWarning(const QString &source, const QString &text,
                                 const QString &detail) {
  showWarning(source, vtrpon::LocalizedText::fromRaw(text), detail);
}

void UserInfoCenter::showWarning(const QString &source,
                                 const vtrpon::LocalizedText &text,
                                 const QString &detail) {
  publish(UserInfoSeverity::Warning, source, text, detail);
}

void UserInfoCenter::showError(const QString &source, const QString &text,
                               const QString &detail) {
  showError(source, vtrpon::LocalizedText::fromRaw(text), detail);
}

void UserInfoCenter::showError(const QString &source,
                               const vtrpon::LocalizedText &text,
                               const QString &detail) {
  publish(UserInfoSeverity::Error, source, text, detail);
}

void UserInfoCenter::setDisplayDuration(int durationMs) {
  displayDurationMs_ = qMax(0, durationMs);
}

void UserInfoCenter::retranslate() {
  emit messageChanged(severity_, source_, text_.resolve(), detail_);
}

void UserInfoCenter::publish(UserInfoSeverity severity, const QString &source,
                             const vtrpon::LocalizedText &text,
                             const QString &detail, bool audioSettingsAction) {
  if (severity == UserInfoSeverity::Error)
    qCritical().noquote() << "NOTICE" << source << text.resolve() << detail;
  else if (severity == UserInfoSeverity::Warning)
    qWarning().noquote() << "NOTICE" << source << text.resolve() << detail;
  else
    qInfo().noquote() << "NOTICE" << source << text.resolve() << detail;
  Message message;
  message.audioSettingsAction = audioSettingsAction;
  message.severity = severity;
  message.source = source.trimmed().isEmpty() ? QStringLiteral("SYSTEM")
                                               : source.trimmed();
  message.text = text;
  message.detail = detail;
  if (message.text.resolve().trimmed().isEmpty() || isDuplicate(message)) {
    return;
  }

  if (severity_ == UserInfoSeverity::None ||
      !displayTimer_->isActive()) {
    if (pflSameDeviceConfirmation_) {
      pendingMessages_.append(message);
      return;
    }
    present(message);
    return;
  }

  if (severityPriority(message.severity) > severityPriority(severity_)) {
    for (qsizetype index = pendingMessages_.size(); index-- > 0;) {
      if (severityPriority(pendingMessages_.at(index).severity) <
          severityPriority(message.severity)) {
        pendingMessages_.removeAt(index);
      }
    }
    present(message);
    return;
  }

  qsizetype insertIndex = pendingMessages_.size();
  for (qsizetype index = 0; index < pendingMessages_.size(); ++index) {
    if (severityPriority(message.severity) >
        severityPriority(pendingMessages_.at(index).severity)) {
      insertIndex = index;
      break;
    }
  }
  pendingMessages_.insert(insertIndex, message);
}

void UserInfoCenter::present(const Message &message) {
  severity_ = message.severity;
  source_ = message.source;
  text_ = message.text;
  detail_ = message.detail;
  audioSettingsAction_ = message.audioSettingsAction;
  pflSameDeviceConfirmation_ = message.pflSameDeviceConfirmation;
  emit messageChanged(severity_, source_, text_.resolve(), detail_);
  if (pflSameDeviceConfirmation_) {
    displayTimer_->stop();
  } else if (displayDurationMs_ > 0) {
    displayTimer_->start(displayDurationMs_);
  } else {
    displayTimer_->stop();
  }
}

bool UserInfoCenter::isDuplicate(const Message &message) const {
  if (severity_ == message.severity && source_ == message.source &&
      text_ == message.text && detail_ == message.detail &&
      audioSettingsAction_ == message.audioSettingsAction &&
      pflSameDeviceConfirmation_ == message.pflSameDeviceConfirmation) {
    return true;
  }
  for (const Message &pending : pendingMessages_) {
    if (pending.severity == message.severity &&
        pending.source == message.source && pending.text == message.text &&
        pending.detail == message.detail &&
        pending.audioSettingsAction == message.audioSettingsAction &&
        pending.pflSameDeviceConfirmation == message.pflSameDeviceConfirmation) {
      return true;
    }
  }
  return false;
}

void UserInfoCenter::presentNextPendingMessage() {
  if (pendingMessages_.isEmpty()) {
    severity_ = UserInfoSeverity::None;
    source_.clear();
    text_ = {};
    detail_.clear();
    audioSettingsAction_ = false;
    pflSameDeviceConfirmation_ = false;
    emit messageChanged(severity_, source_, text_.resolve(), detail_);
    return;
  }
  present(pendingMessages_.takeFirst());
}

UserInfoBar::UserInfoBar(UserInfoCenter *center, QWidget *parent)
    : QWidget(parent), severityLabel_(new QLabel(this)),
      sourceLabel_(new QLabel(this)), messageLabel_(new QLabel(this)), center_(center) {
  setObjectName(QStringLiteral("userInfoBar"));
  setAttribute(Qt::WA_StyledBackground, true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
  setMinimumHeight(vtrpon::scalePx(34));

  severityLabel_->setObjectName(QStringLiteral("userInfoSeverityLabel"));
  severityLabel_->setAlignment(Qt::AlignCenter);
  severityLabel_->setMinimumWidth(vtrpon::scalePx(58));
  severityLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

  sourceLabel_->setObjectName(QStringLiteral("userInfoSourceLabel"));
  sourceLabel_->setAlignment(Qt::AlignCenter);
  sourceLabel_->setMinimumWidth(vtrpon::scalePx(62));
  sourceLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

  messageLabel_->setObjectName(QStringLiteral("userInfoMessageLabel"));
  messageLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  messageLabel_->setWordWrap(true);
  messageLabel_->setOpenExternalLinks(false);
  messageLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                         Qt::TextSelectableByKeyboard |
                                         Qt::LinksAccessibleByMouse |
                                         Qt::LinksAccessibleByKeyboard);
  messageLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(vtrpon::scalePx(6), vtrpon::scalePx(3),
                             vtrpon::scalePx(8), vtrpon::scalePx(3));
  layout->setSpacing(vtrpon::scalePx(6));
  layout->addWidget(severityLabel_);
  layout->addWidget(sourceLabel_);
  layout->addWidget(messageLabel_, 1);

  if (center) {
    connect(messageLabel_, &QLabel::linkActivated, center,
            [center](const QString &link) {
              if (link == QStringLiteral("audio-settings"))
                center->activateAudioSettings();
              else if (link == QStringLiteral("pfl-yes"))
                center->resolvePflSameDeviceConfirmation(true);
              else if (link == QStringLiteral("pfl-no"))
                center->resolvePflSameDeviceConfirmation(false);
            });
    connect(center, &UserInfoCenter::messageChanged, this,
            &UserInfoBar::updateMessage);
    updateMessage(center->severity(), center->source(), center->text(),
                  center->detail());
  }
}

void UserInfoBar::updateMessage(UserInfoSeverity severity,
                                const QString &source, const QString &text,
                                const QString &detail) {
  const bool hasMessage = severity != UserInfoSeverity::None;
  severityLabel_->setVisible(hasMessage);
  sourceLabel_->setVisible(hasMessage);
  messageLabel_->setVisible(hasMessage);
  severityLabel_->setText(severityName(severity));
  const QString displayedSource = sourceName(source);
  sourceLabel_->setText(displayedSource);
  const bool hasAction = center_ && center_->hasAudioSettingsAction();
  const bool pflConfirmation = center_ && center_->hasPflSameDeviceConfirmation();
  messageLabel_->setTextFormat(hasAction ? Qt::RichText : Qt::PlainText);
  QString displayedText = text;
  if (hasAction) {
    displayedText = text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    if (pflConfirmation) {
      const auto linkColor = messageLabel_->palette().color(QPalette::WindowText).name();
      displayedText += QStringLiteral(" &nbsp; <a href=\"pfl-yes\" style=\"color: %1; text-decoration: underline;\">Yes</a>")
          .arg(linkColor);
      displayedText += QStringLiteral(" / <a href=\"pfl-no\" style=\"color: %1; text-decoration: underline;\">No</a>")
          .arg(linkColor);
    }
    displayedText += QStringLiteral(" &nbsp; <a href=\"audio-settings\" style=\"color: %1; text-decoration: underline;\">%2</a>")
        .arg(messageLabel_->palette().color(QPalette::WindowText).name(),
             vtrpon::uiText("info.action.audio_settings").toHtmlEscaped());
  }
  messageLabel_->setText(displayedText);
  if (severity == UserInfoSeverity::None) {
    setToolTip(QString());
    setAccessibleName(QString());
    setAccessibleDescription(QString());
  } else {
    setToolTip(detail);
    setAccessibleName(
        vtrpon::uiText("info.accessible.summary")
            .arg(severityName(severity), displayedSource, text));
    setAccessibleDescription(detail);
  }

  const QString property = severityProperty(severity);
  if (severityProperty_ != property) {
    severityProperty_ = property;
    setProperty("severity", property);
    repolish(this);
    repolish(severityLabel_);
  }
}
