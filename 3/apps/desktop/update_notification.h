#pragma once

#include "update_catalog.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <optional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QNetworkReply;

namespace vtrpon {

enum class UpdateCheckStatus { Failed, Current, Available };

class UpdateChecker final : public QObject {
public:
  using Completion = std::function<void(bool manual, UpdateCheckStatus,
                                        std::optional<UpdateRelease>)>;
  UpdateChecker(const QString &currentVersion, Completion completion,
                QObject *parent = nullptr);
  UpdateChecker(const QString &currentVersion, Completion completion,
                const QUrl &updatesUrl, QObject *parent = nullptr);
  void check(bool manual);

private:
  void sendActivityNoticeIfDue();
  QString currentVersion_;
  QUrl updatesUrl_;
  Completion completion_;
  QNetworkAccessManager network_;
  QPointer<QNetworkReply> reply_;
  bool manualPending_ = false;
  bool activityPending_ = false;
};

class UpdateWindow final : public QWidget {
public:
  UpdateWindow(const QString &currentVersion, std::function<bool()> japanese,
               QWidget *parent = nullptr);
  void setResult(UpdateCheckStatus status, const std::optional<UpdateRelease> &release);
  void retranslate();

private:
  bool japanese() const;
  QString currentVersion_;
  std::function<bool()> japanese_;
  UpdateCheckStatus status_ = UpdateCheckStatus::Current;
  std::optional<UpdateRelease> release_;
  QLabel *heading_ = nullptr;
  QLabel *version_ = nullptr;
  QPlainTextEdit *notes_ = nullptr;
  QPushButton *download_ = nullptr;
  QPushButton *close_ = nullptr;
};

} // namespace vtrpon
