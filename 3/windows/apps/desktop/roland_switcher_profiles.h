#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace vtrpon {
enum class RolandSwitcherModel {
  V8Hd, Vr50HdMk2, V600Uhd, V160Hd, V80Hd, V1_4K,
  V1HdPlus, V1Sdi, V1Hd, V02HdMk2, Vr4Hd, Vr6Hd, Vr120Hd
};
enum class RolandTransport { Midi, Lan, Serial };
struct RolandSwitcherProfile {
  RolandSwitcherModel model;
  const char *id;
  const char *name;
  RolandTransport transport;
  int inputs;
  int maximumProgramValue;
  const char *programAddress;
  const char *midiModelId;
  bool receive;
  bool fade;
  int passwordLength;
  const char *manual;
};
const QList<RolandSwitcherProfile> &rolandSwitcherProfiles();
const RolandSwitcherProfile &rolandSwitcherProfile(RolandSwitcherModel model);
RolandSwitcherModel rolandSwitcherModel(const QString &id);
bool isRolandSwitcherModel(const QString &id);
QString rolandTransportName(RolandTransport transport);
QString rolandInputDescription(RolandSwitcherModel model);

struct RolandModelSettings {
  QString host;
  int port = 8023;
  QString password;
  QString serialPort;
  int baudRate = 38400;
  int midiDeviceId = 0x10;
  int sourceNumber = 1;
  bool takeProgramWhenOnAir = false;
  bool takeOnAirWhenProgramSelected = false;
  bool ftbFollowsProgram = false;
  bool operator==(const RolandModelSettings &) const = default;
};
RolandModelSettings normalizeRolandSettings(RolandSwitcherModel model, RolandModelSettings settings);
QJsonObject rolandSettingsJson(const RolandModelSettings &settings, bool includePassword = false);
RolandModelSettings rolandSettingsFromJson(const QJsonObject &json);
bool validRolandProfilesJson(const QJsonObject &json);
QJsonObject publicRolandProfilesJson(QJsonObject json);
// Pure codecs shared by the MIDI adapter and protocol regression tests.
QByteArray rolandSysEx(RolandSwitcherModel model, int deviceId, int command,
                      const QByteArray &addressAndData);
bool decodeRolandSysEx(RolandSwitcherModel model, int deviceId, const QByteArray &message,
                      QByteArray *address, QByteArray *data);
} // namespace vtrpon
