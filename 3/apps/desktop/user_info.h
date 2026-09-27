#pragma once

#include "language_text.h"

#include <QObject>
#include <QList>
#include <QMetaType>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

enum class UserInfoSeverity {
  None,
  Success,
  Warning,
  Error,
};

class UserInfoCenter final : public QObject {
  Q_OBJECT

public:
  explicit UserInfoCenter(QObject *parent = nullptr);

  UserInfoSeverity severity() const;
  QString source() const;
  QString text() const;
  QString detail() const;
  bool hasAudioSettingsAction() const;
  bool hasPflSameDeviceConfirmation() const;
  void showPflSameDeviceConfirmation();
  void resolvePflSameDeviceConfirmation(bool accepted);
  void cancelPflSameDeviceConfirmation();
  void showAudioOutputWarning(const vtrpon::LocalizedText &text,
                              const QString &detail = QString());
  void showAudioOutputError(const vtrpon::LocalizedText &text,
                            const QString &detail = QString());
  void activateAudioSettings();

  void showSuccess(const QString &source, const QString &text,
                   const QString &detail = QString());
  void showSuccess(const QString &source, const vtrpon::LocalizedText &text,
                   const QString &detail = QString());
  void showWarning(const QString &source, const QString &text,
                   const QString &detail = QString());
  void showWarning(const QString &source, const vtrpon::LocalizedText &text,
                   const QString &detail = QString());
  void showError(const QString &source, const QString &text,
                 const QString &detail = QString());
  void showError(const QString &source, const vtrpon::LocalizedText &text,
                 const QString &detail = QString());
  void setDisplayDuration(int durationMs);
  void retranslate();

signals:
  void audioSettingsRequested();
  void pflSameDeviceDecision(bool accepted);
  void messageChanged(UserInfoSeverity severity, const QString &source,
                      const QString &text, const QString &detail);

private:
  struct Message {
    UserInfoSeverity severity = UserInfoSeverity::None;
    QString source;
    vtrpon::LocalizedText text;
    QString detail;
    bool audioSettingsAction = false;
    bool pflSameDeviceConfirmation = false;
  };

  void publish(UserInfoSeverity severity, const QString &source,
               const vtrpon::LocalizedText &text, const QString &detail,
               bool audioSettingsAction = false);
  void present(const Message &message);
  bool isDuplicate(const Message &message) const;
  void presentNextPendingMessage();

  UserInfoSeverity severity_ = UserInfoSeverity::None;
  QString source_;
  vtrpon::LocalizedText text_;
  QString detail_;
  bool audioSettingsAction_ = false;
  bool pflSameDeviceConfirmation_ = false;
  QList<Message> pendingMessages_;
  QTimer *displayTimer_ = nullptr;
  int displayDurationMs_ = 5000;
};

class UserInfoBar final : public QWidget {
  Q_OBJECT

public:
  explicit UserInfoBar(UserInfoCenter *center, QWidget *parent = nullptr);

private:
  void updateMessage(UserInfoSeverity severity, const QString &source,
                     const QString &text, const QString &detail);

  QLabel *severityLabel_ = nullptr;
  QLabel *sourceLabel_ = nullptr;
  QLabel *messageLabel_ = nullptr;
  QPointer<UserInfoCenter> center_;
  QString severityProperty_;
};

Q_DECLARE_METATYPE(UserInfoSeverity)
