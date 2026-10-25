#include "switcher_check_session.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QRegularExpression>
#include <QSysInfo>

namespace vtrpon {
namespace { QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); } }
void SwitcherCheckSession::start(RolandSwitcherModel model, const RolandModelSettings &settings) {
  model_ = model; settings_ = settings; started_ = now(); ended_.clear(); reason_.clear();
  firmware_.clear(); comment_.clear(); answers_ = {}; events_ = {}; dropped_ = 0; active_ = true;
  append("SESSION", "Recording started. Operator observations are independent of protocol responses.");
}
void SwitcherCheckSession::stop(const QString &reason) {
  if (!active_) return;
  append("SESSION", reason.isEmpty() ? QStringLiteral("Recording stopped.") : reason);
  reason_ = reason; ended_ = now(); active_ = false;
}
void SwitcherCheckSession::append(const QString &kind, QString detail) {
  if (!active_) return;
  if (!settings_.password.isEmpty()) detail.replace(settings_.password, QStringLiteral("[password omitted]"));
  if (!settings_.host.isEmpty()) detail.replace(settings_.host, QStringLiteral("[address omitted]"));
  static const QRegularExpression ipv4(QStringLiteral("\\b(?:[0-9]{1,3}\\.){3}[0-9]{1,3}\\b"));
  detail.replace(ipv4, QStringLiteral("[address omitted]"));
  if (events_.size() >= 2000) { events_.removeFirst(); ++dropped_; }
  events_.append(QJsonObject{{"time", now()}, {"kind", kind.left(32)}, {"detail", detail.left(512)}});
}
void SwitcherCheckSession::setAnswers(const QString &firmware, const QJsonObject &answers, const QString &comment) {
  firmware_ = firmware.left(80); comment_ = comment.left(4096); answers_ = {};
  for (const auto &key : {"connection", "inputSend", "inputReceive", "fade", "reconnect"}) {
    const auto result = answers.value(QLatin1String(key)).toString();
    answers_.insert(QLatin1String(key), result == "worked" || result == "failed" || result == "unavailable" ? result : "not-tested");
  }
}
QJsonObject SwitcherCheckSession::report() const {
  if (!exists()) return {};
  const auto &p = rolandSwitcherProfile(model_);
  return {{"format", "vtr-pon-switcher-check"}, {"version", 1},
    {"applicationVersion", QCoreApplication::applicationVersion()},
    {"implementation", "roland-trial-1"}, {"build", QStringLiteral(__DATE__ " " __TIME__)},
    {"os", QSysInfo::prettyProductName()}, {"architecture", QSysInfo::buildCpuArchitecture()},
    {"model", QLatin1String(p.id)}, {"modelName", QLatin1String(p.name)},
    {"transport", rolandTransportName(p.transport)},
    {"firmwareReportedByUser", firmware_}, {"startedAt", started_}, {"endedAt", ended_},
    {"recording", active_}, {"stopReason", reason_},
    {"capabilities", QJsonObject{{"inputSend", true}, {"inputReceive", p.receive}, {"fade", p.fade}}},
    {"settings", QJsonObject{{"sourceNumber", settings_.sourceNumber}, {"tcpPort", settings_.port},
      {"baudRate", settings_.baudRate}, {"midiDeviceId", settings_.midiDeviceId},
      {"inputSendEnabled", settings_.takeProgramWhenOnAir},
      {"inputReceiveEnabled", settings_.takeOnAirWhenProgramSelected}, {"fadeEnabled", settings_.ftbFollowsProgram}}},
    {"operatorResults", answers_}, {"operatorComment", comment_},
    {"droppedEventCount", dropped_}, {"events", events_}};
}
} // namespace vtrpon
