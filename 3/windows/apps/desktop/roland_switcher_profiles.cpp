#include "roland_switcher_profiles.h"
#include <QHostAddress>
#include <QRegularExpression>

namespace vtrpon {
const QList<RolandSwitcherProfile> &rolandSwitcherProfiles() {
  // Addresses are current-operation parameters, never PANEL LOCK or memory slots.
  static const QList<RolandSwitcherProfile> profiles{
    {RolandSwitcherModel::V8Hd, "v8hd", "V-8HD", RolandTransport::Midi, 8, 7, "001200", "00000068", true, true, 0, "V-8HD_reference_v2_eng03_W.pdf"},
    {RolandSwitcherModel::Vr50HdMk2, "vr50hd-mk2", "VR-50HD MK II", RolandTransport::Lan, 4, 4, "", "", true, true, 0, "VR-50HD-MK2_reference_eng02_W.pdf"},
    {RolandSwitcherModel::V600Uhd, "v600uhd", "V-600UHD", RolandTransport::Lan, 8, 7, "", "", false, true, 4, "V-600UHD_reference_eng07_W.pdf"},
    {RolandSwitcherModel::V160Hd, "v160hd", "V-160HD", RolandTransport::Lan, 16, 51, "002100", "", true, false, 4, "V-160HD_Control_jpn04_W.pdf"},
    {RolandSwitcherModel::V80Hd, "v80hd", "V-80HD", RolandTransport::Lan, 8, 56, "001500", "", true, false, 8, "V-80HD_Control_eng01_W.pdf"},
    {RolandSwitcherModel::V1_4K, "v1-4k", "V-1-4K", RolandTransport::Lan, 5, 15, "001200", "", true, false, 8, "V-1-4K_Remote_Control_eng01_W.pdf"},
    {RolandSwitcherModel::V1HdPlus, "v1hd-plus", "V-1HD+", RolandTransport::Serial, 4, 3, "", "", true, true, 0, "V-1HD_plus_reference_eng02_W.pdf"},
    {RolandSwitcherModel::V1Sdi, "v1sdi", "V-1SDI", RolandTransport::Midi, 4, 3, "710308", "00000031", true, true, 0, "V-1SDI_reference_v15_eng02_W.pdf"},
    {RolandSwitcherModel::V1Hd, "v1hd", "V-1HD", RolandTransport::Midi, 4, 3, "710008", "00000020", true, true, 0, "V-1HD_RC_guide_e01_W.pdf"},
    {RolandSwitcherModel::V02HdMk2, "v02hd-mk2", "V-02HD MK II", RolandTransport::Midi, 2, 1, "002500", "000000000a", true, true, 0, "V-02HD_MKII_reference_eng03_W.pdf"},
    {RolandSwitcherModel::Vr4Hd, "vr4hd", "VR-4HD", RolandTransport::Midi, 4, 3, "000700", "00000029", true, true, 0, "VR-4HD_reference_v2_eng01_W.pdf"},
    {RolandSwitcherModel::Vr6Hd, "vr6hd", "VR-6HD", RolandTransport::Lan, 6, 28, "000f00", "", true, false, 4, "VR-6HD_Control_eng02_W.pdf"},
    {RolandSwitcherModel::Vr120Hd, "vr120hd", "VR-120HD", RolandTransport::Lan, 12, 36, "001b00", "", true, true, 4, "VR-120HD_Control_eng04_W.pdf"}
  };
  return profiles;
}
const RolandSwitcherProfile &rolandSwitcherProfile(RolandSwitcherModel model) {
  for (const auto &p : rolandSwitcherProfiles()) if (p.model == model) return p;
  return rolandSwitcherProfiles().first();
}
RolandSwitcherModel rolandSwitcherModel(const QString &id) {
  for (const auto &p : rolandSwitcherProfiles()) if (id == QLatin1String(p.id)) return p.model;
  return RolandSwitcherModel::V8Hd;
}
bool isRolandSwitcherModel(const QString &id) {
  for (const auto &p : rolandSwitcherProfiles()) if (id == QLatin1String(p.id)) return true;
  return false;
}
QString rolandTransportName(RolandTransport transport) {
  return transport == RolandTransport::Lan ? QStringLiteral("LAN")
      : transport == RolandTransport::Midi ? QStringLiteral("USB MIDI") : QStringLiteral("RS-232 / USB–RS-232");
}
QString rolandInputDescription(RolandSwitcherModel model) {
  if (model == RolandSwitcherModel::V160Hd) return QStringLiteral("1–8: HDMI 1–8 / 9–16: SDI 1–8");
  if (model == RolandSwitcherModel::V80Hd) return QStringLiteral("1–4: HDMI IN 1–4 / 5–8: SDI IN 1–4");
  if (model == RolandSwitcherModel::Vr120Hd) return QStringLiteral("1–6: HDMI IN 1–6 / 7–12: SDI IN 1–6");
  if (model == RolandSwitcherModel::V600Uhd) return QStringLiteral("XPT 1–8");
  if (model == RolandSwitcherModel::V1_4K) return QStringLiteral("HDMI IN 1–5");
  return QStringLiteral("INPUT 1–%1").arg(rolandSwitcherProfile(model).inputs);
}
RolandModelSettings normalizeRolandSettings(RolandSwitcherModel model, RolandModelSettings s) {
  const auto &p = rolandSwitcherProfile(model);
  s.host = s.host.trimmed().left(15);
  s.port = qBound(1, s.port, 65535);
  s.password = s.password.left(32);
  s.serialPort = s.serialPort.trimmed().toUpper().left(12);
  if (s.baudRate != 38400 && s.baudRate != 115200) s.baudRate = 38400;
  s.midiDeviceId = model == RolandSwitcherModel::V1Hd ? qBound(0, s.midiDeviceId, 31) : 0x10;
  s.sourceNumber = qBound(1, s.sourceNumber, p.inputs);
  s.takeOnAirWhenProgramSelected &= p.receive;
  s.ftbFollowsProgram &= p.fade;
  return s;
}
QJsonObject rolandSettingsJson(const RolandModelSettings &s, bool includePassword) {
  QJsonObject result{{"host", s.host}, {"port", s.port}, {"serialPort", s.serialPort},
    {"baudRate", s.baudRate}, {"midiDeviceId", s.midiDeviceId}, {"sourceNumber", s.sourceNumber},
    {"takeProgramWhenOnAir", s.takeProgramWhenOnAir},
    {"takeOnAirWhenProgramSelected", s.takeOnAirWhenProgramSelected}, {"ftbFollowsProgram", s.ftbFollowsProgram}};
  if (includePassword) result.insert("password", s.password);
  return result;
}
RolandModelSettings rolandSettingsFromJson(const QJsonObject &o) {
  return {o["host"].toString(), o["port"].toInt(8023), o["password"].toString(),
    o["serialPort"].toString(), o["baudRate"].toInt(38400), o["midiDeviceId"].toInt(16),
    o["sourceNumber"].toInt(1), o["takeProgramWhenOnAir"].toBool(),
    o["takeOnAirWhenProgramSelected"].toBool(), o["ftbFollowsProgram"].toBool()};
}
bool validRolandProfilesJson(const QJsonObject &json) {
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (!isRolandSwitcherModel(it.key()) || !it.value().isObject()) return false;
    const auto o = it.value().toObject();
    const auto defaults = rolandSettingsJson({}, true);
    for (auto f = o.begin(); f != o.end(); ++f) {
      if (!defaults.contains(f.key()) || f.value().type() != defaults[f.key()].type()) return false;
      if (f.value().isDouble() && f.value().toDouble() != f.value().toInt()) return false;
      if (f.value().isString() && (f.value().toString().size() > 32 || f.value().toString().contains(QChar::Null))) return false;
    }
    auto s = rolandSettingsFromJson(o);
    const auto model = rolandSwitcherModel(it.key());
    if (s != normalizeRolandSettings(model, s)) return false;
    if (!s.host.isEmpty() && QHostAddress(s.host).protocol() != QAbstractSocket::IPv4Protocol) return false;
    if (!s.serialPort.isEmpty() && !QRegularExpression(QStringLiteral("^COM[1-9][0-9]{0,3}$")).match(s.serialPort).hasMatch()) return false;
  }
  return true;
}
QJsonObject publicRolandProfilesJson(QJsonObject json) {
  for (auto it = json.begin(); it != json.end(); ++it) {
    auto o = it.value().toObject(); o.remove("password"); it.value() = o;
  }
  return json;
}
QByteArray rolandSysEx(RolandSwitcherModel model, int deviceId, int command, const QByteArray &body) {
  QByteArray message = QByteArray::fromHex("f041");
  message.append(char(deviceId));
  message += QByteArray::fromHex(rolandSwitcherProfile(model).midiModelId);
  message.append(char(command)); message += body;
  int sum = 0;
  for (unsigned char b : body) sum += b;
  message.append(char((-sum) & 0x7f)); message.append(char(0xf7));
  return message;
}
bool decodeRolandSysEx(RolandSwitcherModel model, int deviceId, const QByteArray &m, QByteArray *address, QByteArray *data) {
  QByteArray prefix = QByteArray::fromHex("f041"); prefix.append(char(deviceId));
  prefix += QByteArray::fromHex(rolandSwitcherProfile(model).midiModelId); prefix.append(char(0x12));
  if (!m.startsWith(prefix) || m.size() < prefix.size() + 6 || m.back() != char(0xf7)) return false;
  int sum = 0;
  for (qsizetype i = prefix.size(); i < m.size() - 1; ++i) {
    const auto byte = static_cast<unsigned char>(m[i]); if (byte > 127) return false; sum += byte;
  }
  if ((sum & 0x7f) != 0) return false;
  *address = m.mid(prefix.size(), 3); *data = m.mid(prefix.size()+3, m.size()-prefix.size()-5);
  return true;
}
} // namespace vtrpon
