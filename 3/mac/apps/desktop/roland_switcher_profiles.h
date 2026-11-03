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
bool rolandSwitcherSupported(RolandSwitcherModel model);
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
  // Missing in older settings: keep the model's original transport (LAN).
  bool useUsbMidi = false;
  bool operator==(const RolandModelSettings &) const = default;
};
bool rolandHasUsbMidiChoice(RolandSwitcherModel model);
RolandTransport rolandTransport(RolandSwitcherModel model, const RolandModelSettings &settings);
bool rolandSupportsFade(RolandSwitcherModel model, RolandTransport transport);
// Minimum firmware for the explicit LAN FTB command (empty for legacy protocols).
QString rolandMinimumFadeVersion(RolandSwitcherModel model);
// One-byte PGM read, optional INPUT assignment read, then PGM recheck. Keeping
// this query independent of tally avoids AUX/PinP/DSK and FTB tally side effects.
class RolandProgramQuery {
public:
  explicit RolandProgramQuery(RolandSwitcherModel model) : model_(model) {}
  enum class Result { Invalid, More, Observed };
  void reset();
  QByteArray address() const;
  Result accept(int value);
  int input() const { return input_; } // zero-based; -1 means STILL/player/BLACK/N/A
private:
  int logicalIndex(int value) const;
  Result select(int value);
  RolandSwitcherModel model_;
  enum class Step { Program, Assignment, Confirm } step_ = Step::Program;
  int selection_ = -1, logical_ = -1, input_ = -1;
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
