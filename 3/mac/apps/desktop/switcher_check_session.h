#pragma once
#include "roland_switcher_profiles.h"
#include <QJsonArray>

namespace vtrpon {
// Only switcher-specific events enter this report; no application log or paths.
class SwitcherCheckSession {
public:
  void start(RolandSwitcherModel model, const RolandModelSettings &settings);
  void stop(const QString &reason = {});
  bool active() const { return active_; }
  bool exists() const { return !started_.isEmpty(); }
  void append(const QString &kind, QString detail);
  void setAnswers(const QString &firmware, const QJsonObject &answers, const QString &comment);
  QJsonObject report() const;
private:
  bool active_ = false;
  RolandSwitcherModel model_ = RolandSwitcherModel::V8Hd;
  RolandModelSettings settings_;
  QString started_, ended_, reason_, firmware_, comment_;
  QJsonObject answers_;
  QJsonArray events_;
  int dropped_ = 0;
};
} // namespace vtrpon
