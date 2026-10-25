#include "roland_remote_switcher.h"
#include "roland_v8hd_midi_switcher.h"
#include "switcher_check_session.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QThread>
#include <iostream>
#include <functional>

namespace {
bool ok = true;
void check(bool result, const char *message) {
  if (!result) std::cerr << "FAIL: Roland trial: " << message << '\n';
  ok &= result;
}
bool waitFor(const std::function<bool()> &condition, int limit = 2000) {
  QElapsedTimer t; t.start();
  while (!condition() && t.elapsed() < limit) { QCoreApplication::processEvents(); QThread::msleep(1); }
  return condition();
}
void settle(int ms) { QElapsedTimer t; t.start(); waitFor([&] { return t.elapsed() >= ms; }, ms + 100); }
QByteArray frame(const QByteArray &s) { return QByteArray(1, char(2)) + s + ';'; }
}
namespace vtrpon {
class RolandMidiTrialTest {
public:
  static void run() {
    const auto query = rolandSysEx(RolandSwitcherModel::V1Hd, 0x10, 0x11, QByteArray::fromHex("710008000001"));
    check(query == QByteArray::fromHex("f04110000000201171000800000106f7"), "V-1HD documented model ID, address and checksum");
    check(rolandSysEx(RolandSwitcherModel::V02HdMk2, 0x10, 0x12, QByteArray::fromHex("00250001")) ==
          QByteArray::fromHex("f04110000000000a12002500015af7"), "V-02HD MK II five-byte model ID");
    {
      RolandV8HdMidiSwitcher legacy(true); int changes = 0;
      QObject::connect(&legacy, &SwitcherDevice::programInputChanged, &legacy, [&](int) { ++changes; });
      legacy.parseDataSetMessage(rolandSysEx(RolandSwitcherModel::V8Hd, 16, 0x12, QByteArray::fromHex("0c00000100000000000000")));
      check(changes == 0, "V-8HD initial tally cannot trigger ON AIR on diagnostic reconnect");
      legacy.parseDataSetMessage(rolandSysEx(RolandSwitcherModel::V8Hd, 16, 0x12, QByteArray::fromHex("0c00000001000000000000")));
      check(changes == 1, "V-8HD subsequent input change still works");
    }
    for (auto model : {RolandSwitcherModel::V1Hd, RolandSwitcherModel::V1Sdi, RolandSwitcherModel::V02HdMk2, RolandSwitcherModel::Vr4Hd}) {
      RolandV8HdMidiSwitcher device(model, {}, true);
      int ready = 0, changes = 0;
      QObject::connect(&device, &SwitcherDevice::connectionOpened, &device, [&](bool value, const QString &) { if (value) ++ready; });
      QObject::connect(&device, &SwitcherDevice::programInputChanged, &device, [&](int value) { if (value == 2) ++changes; });
      const auto address = QByteArray::fromHex(rolandSwitcherProfile(model).programAddress);
      const auto response = [&](int input) { return rolandSysEx(model, 0x10, 0x12, address + QByteArray(1, char(input))); };
      device.queryPending_ = true; device.parseDataSetMessage(response(0));
      check(ready == 1 && changes == 0, "MIDI baseline establishes ready without ON AIR");
      device.queryPending_ = true; device.parseDataSetMessage(response(1));
      check(changes == 1, "MIDI input transition detected");
      device.queryPending_ = true; device.parseDataSetMessage(response(1));
      check(changes == 1, "unchanged MIDI input never retriggers");
      auto corrupt = response(0); corrupt[corrupt.size()-2] ^= 1;
      device.queryPending_ = true; device.parseDataSetMessage(corrupt);
      check(device.queryPending_ && changes == 1, "bad checksum is not accepted");
      device.parseDataSetMessage(rolandSysEx(model, 1, 0x12, address + QByteArray(1, 0)));
      check(device.queryPending_, "wrong MIDI device ID ignored");
      device.lastProgramInputNumber_ = 2; device.parseDataSetMessage(response(1));
      check(changes == 1, "local input echo cannot take again");
      device.queryPending_ = true;
      const auto split = response(0); device.receiveMidiBytes(split.left(5));
      check(device.queryPending_, "partial MIDI frame retained");
      device.receiveMidiBytes(QByteArray(1, char(0xf8)) + split.mid(5));
      check(!device.queryPending_, "split MIDI frame with real-time byte reassembled");
      device.close(); check(!device.modelReady_ && !device.queryPending_, "MIDI close clears baseline and pending query");
    }
  }
};
class RolandRemoteProtocolTest {
public:
  static void runSerial() {
    RolandModelSettings settings; settings.sourceNumber = 4;
    RolandRemoteSwitcher device(RolandSwitcherModel::V1HdPlus, settings, true);
    QList<QByteArray> sent;
    device.writeForTest_ = [&](const QByteArray &wire) { sent.append(wire); return true; };
    device.active_ = true;
    bool ready = false; int changes = 0;
    QObject::connect(&device, &SwitcherDevice::connectionOpened, &device, [&](bool value, const QString &) { ready = value; });
    QObject::connect(&device, &SwitcherDevice::programInputChanged, &device, [&](int) { ++changes; });
    device.enqueue({"VER;", RolandRemoteSwitcher::Reply::Version});
    check(sent.last() == frame("VER"), "RS-232 uses STX framing");
    device.receive("VER:V-1HD PLUS,1.0;");
    check(sent.last() == frame("TLY"), "V-1HD+ queries documented tally status");
    device.receive(frame("TLY:1,0,2,0"));
    check(ready && changes == 0, "serial baseline does not trigger ON AIR");
    device.setProgramInput(4); settle(5);
    check(sent.last() == frame("PGM:3"), "serial input is zero-based");
    device.setFadeToBlack(true); device.setFadeToBlack(false);
    device.receive(QByteArray(1, char(6))); settle(5);
    check(sent.last() == frame("OFD:-63"), "serial FTB black uses negative knob endpoint");
    device.receive(QByteArray(1, char(6))); settle(5);
    check(sent.last() == frame("OFD:0"), "serial FTB restores knob center");
    device.receive(QByteArray(1, char(6))); settle(5);
    device.pending_.reply = RolandRemoteSwitcher::Reply::Program;
    device.receive(frame("TLY:0,0,0,0"));
    device.pending_.reply = RolandRemoteSwitcher::Reply::Program;
    device.receive(frame("TLY:0,0,0,1"));
    check(changes == 0, "serial fade returning the same source does not retrigger");
    device.close();
  }
};
}

bool runRolandSwitcherTrialRegression() {
  using namespace vtrpon;
  ok = true;
  check(rolandSwitcherProfiles().size() == 13, "all requested models including VR-120HD");
  RolandMidiTrialTest::run(); RolandRemoteProtocolTest::runSerial();
  struct Fixture { RolandSwitcherModel model; const char *name; const char *address; int inputs; int tallies; bool rawAck; int source = 2; };
  const QList<Fixture> fixtures{
    {RolandSwitcherModel::V600Uhd, "V-600UHD", "", 8, 8, false},
    {RolandSwitcherModel::V160Hd, "V-160HD", "002100", 16, 52, true},
    {RolandSwitcherModel::V80Hd, "V-80HD", "001500", 8, 57, false},
    {RolandSwitcherModel::V1_4K, "V-1-4K", "001200", 5, 15, false},
    {RolandSwitcherModel::Vr6Hd, "VR-6HD", "000F00", 6, 29, true},
    {RolandSwitcherModel::Vr120Hd, "VR-120HD", "001B00", 12, 37, true, 12}
  };
  for (const auto &f : fixtures) {
    QTcpServer server; check(server.listen(QHostAddress::LocalHost), "LAN simulator listens");
    QPointer<QTcpSocket> peer; QByteArray buffer; QList<QByteArray> commands;
    bool authenticated = false, wrongModel = false, ignoreQuery = false, holdAck = false, rejectFade = false;
    int currentInput = 0;
    const QByteArray secret = rolandSwitcherProfile(f.model).passwordLength == 8 ? "P@55W0rd" : "P@55";
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
      peer = server.nextPendingConnection(); auto *client = peer.data(); buffer.clear(); authenticated = false;
      client->write("Password:");
      QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
      QObject::connect(client, &QTcpSocket::readyRead, &server, [&, client] {
        buffer += client->readAll();
        if (!authenticated) {
          const int end = buffer.indexOf('\n'); if (end < 0) return;
          check(buffer.left(end).trimmed() == secret, "correct password delivered to transport");
          buffer.remove(0, end + 1); authenticated = true; client->write("Welcome\r\n");
        }
        while (buffer.contains(';')) {
          const int end = buffer.indexOf(';'); const auto body = buffer.left(end + 1); buffer.remove(0, end + 1); commands.append(body);
          check(!body.startsWith(char(2)), "LAN omits STX according to command reference");
          if (body == "VER;") {
            const auto reply = frame(QByteArray("VER:") + (wrongModel ? "OTHER" : f.name)
                + (f.model == RolandSwitcherModel::Vr120Hd ? ",2.10" : ",1.00"));
            client->write(reply.left(4)); QTimer::singleShot(5, client, [client, reply] { client->write(reply.mid(4)); });
          } else if (body.startsWith("RQH:")) {
            check(body == QByteArray("RQH:0C0000,") + QByteArray::number(f.tallies,16).rightJustified(6,'0').toUpper() + ';', "correct per-model input query address");
            if (!ignoreQuery) { QByteArray tallies(f.tallies, char(0)); if (currentInput >= 0) tallies[currentInput] = char(1); client->write(frame("DTH:0C0000," + tallies.toHex())); }
          } else {
            if (body.startsWith("DTH:")) currentInput = body.mid(body.indexOf(',')+1,2).toInt(nullptr,16);
            else if (body.startsWith("PGM:")) currentInput = body.mid(4,1).toInt();
            if (!holdAck) client->write(body.startsWith("FTB:") ? (rejectFade ? frame("ERR:0") : QByteArrayLiteral("ACK;\n"))
                : f.rawAck ? QByteArray(1,char(6)) : f.model == RolandSwitcherModel::V600Uhd ? QByteArrayLiteral("ACK\n") : frame("ACK"));
          }
        }
      });
    });
    RolandModelSettings settings; settings.host = "127.0.0.1"; settings.port = server.serverPort(); settings.password = QString::fromLatin1(secret); settings.sourceNumber = f.source;
    RolandRemoteSwitcher device(f.model, settings, true);
    bool ready = false; int changes = 0; QStringList diagnostics;
    QObject::connect(&device, &SwitcherDevice::connectionOpened, &server, [&](bool value, const QString &) { ready = value; });
    QObject::connect(&device, &SwitcherDevice::programInputChanged, &server, [&](int) { ++changes; });
    QObject::connect(&device, &SwitcherDevice::diagnosticEvent, &server, [&](const QString &, const QString &value) { diagnostics.append(value); });
    check(device.open() && waitFor([&] { return ready; }), "LAN authentication and baseline complete");
    check(changes == 0, "LAN initial connection never takes ON AIR");
    check(!diagnostics.join('\n').contains(QString::fromLatin1(secret)), "authentication material omitted from diagnostics");
    currentInput = f.source - 1;
    if (*f.address) check(waitFor([&] { return changes == 1; }), "external input change detected");
    if (*f.address) {
      currentInput = -1; settle(300); // All tally goes dark during a hardware output fade.
      currentInput = f.source - 1; settle(300);
      check(changes == 1, "output fade returning the same source does not retrigger ON AIR");
      currentInput = f.inputs; // A documented STILL tally is active.
      check(waitFor([&] { return changes == 2; }), "change to STILL clears target presence");
      currentInput = f.source - 1;
      check(waitFor([&] { return changes == 3; }), "return from STILL detects the physical input");
    }
    check(!device.setProgramInput(f.inputs+1), "physical source range enforced");
    check(device.setProgramInput(f.source), "program input queues");
    const auto expected = *f.address ? QByteArray("DTH:") + f.address + ',' + QByteArray::number(f.source-1,16).rightJustified(2,'0').toUpper() + ';' : QByteArrayLiteral("PGM:1;");
    check(waitFor([&] { return commands.contains(expected); }), "input command uses documented address and value");
    settle(350);
    const int before = changes; settle(300); check(changes == before, "local input echo never retriggers");
    const bool fade = device.setFadeToBlack(true);
    check(fade == (f.model == RolandSwitcherModel::V600Uhd || f.model == RolandSwitcherModel::Vr120Hd), "only models with established FTB commands accept fading");
    if (f.model == RolandSwitcherModel::V600Uhd)
      check(waitFor([&] { return commands.contains("FDE:1;"); }), "V-600UHD documented fade command");
    if (f.model == RolandSwitcherModel::Vr120Hd) {
      check(waitFor([&] { return commands.contains("FTB:ON;"); }), "VR-120HD explicitly enables output fade");
      check(device.setFadeToBlack(false), "VR-120HD fade off queues behind fade on");
      check(waitFor([&] { return commands.contains("FTB:OFF;"); }), "VR-120HD explicitly disables output fade");
      check(!commands.contains("FTB;") && !commands.contains("DTH:020333,01;"), "VR-120HD never toggles fade or writes panel lock");
      for (int source : {1, 6, 7, 12}) {
        const auto command = QByteArray("DTH:001B00,") + QByteArray::number(source-1,16).rightJustified(2,'0').toUpper() + ';';
        const auto count = commands.count(command);
        check(device.setProgramInput(source), "VR-120HD HDMI and SDI boundary inputs accepted");
        check(waitFor([&] { return commands.count(command) > count; }), "VR-120HD HDMI and SDI boundaries map to physical source values");
      }
      settle(100);
      rejectFade = true;
      const int fadeOffCount = commands.count("FTB:OFF;");
      check(device.setFadeToBlack(true) && device.setFadeToBlack(false), "VR-120HD queues explicit fade states");
      check(waitFor([&] { return !ready; }), "VR-120HD reports rejection of an unsupported firmware command");
      const int fadeOnCount = commands.count("FTB:ON;");
      check(waitFor([&] { return ready; }, 5000), "VR-120HD reconnects after a command rejection");
      check(commands.count("FTB:ON;") == fadeOnCount && commands.count("FTB:OFF;") == fadeOffCount, "VR-120HD reconnect never replays rejected or queued fades");
    }
    if (f.model == RolandSwitcherModel::V80Hd) {
      holdAck = true; device.setProgramInput(3);
      check(waitFor([&] { return commands.contains("DTH:001500,02;"); }), "held setter reaches transport");
      device.setProgramInput(4); peer->write(frame("ERR:4"));
      check(waitFor([&] { return !ready; }), "device error clears readiness");
      check(!commands.contains("DTH:001500,03;"), "queued operation discarded after failure");
      holdAck = false; const int beforeReconnect = changes;
      check(waitFor([&] { return ready; }, 5000), "generic LAN automatically reconnects");
      check(changes == beforeReconnect && !commands.contains("DTH:001500,03;"), "reconnect baseline never replays old commands or takes ON AIR");
    }
    device.close();
    if (f.model == RolandSwitcherModel::Vr6Hd || f.model == RolandSwitcherModel::Vr120Hd) {
      wrongModel = true;
      check(device.open() && waitFor([&] { return authenticated; }), "wrong-model connection authenticates");
      settle(400); check(!ready, "wrong model cannot become ready"); device.close();
      wrongModel = false; ignoreQuery = true;
      check(device.open(), "missing-status connection starts"); settle(4400);
      check(!ready, "no status response cannot establish ready"); device.close();
    }
  }
  SwitcherCheckSession report;
  RolandModelSettings settings; settings.password = "SECRET"; settings.host = "192.168.0.10";
  report.start(RolandSwitcherModel::V160Hd, settings);
  report.append("CONNECTION", "192.168.0.10 SECRET");
  report.setAnswers("1.00", {{"inputSend", "worked"}, {"inputReceive", "failed"}}, "Only input switching worked.");
  const auto data = QJsonDocument(report.report()).toJson();
  check(!data.contains("SECRET") && !data.contains("192.168.0.10") && !report.report()["settings"].toObject().contains("host"), "report excludes connection credentials and IP address");
  check(report.report()["operatorResults"].toObject()["inputReceive"] == "failed", "operator observations preserved separately");
  for (int i=0;i<2100;++i) report.append("RX", QString::number(i));
  check(report.report()["events"].toArray().size() == 2000 && report.report()["droppedEventCount"].toInt() > 0, "report memory is bounded and truncation visible");
  report.stop(); const auto frozen = report.report(); report.append("TX", "late event");
  check(!report.active() && report.report() == frozen, "stopped report is frozen");
  settings.sourceNumber = 12; settings.password = "P@55";
  report.start(RolandSwitcherModel::Vr120Hd, settings);
  report.append("AUTH", "P@55 192.168.0.10");
  const auto vr120 = report.report();
  check(vr120["model"] == "vr120hd" && vr120["modelName"] == "VR-120HD" && vr120["transport"] == "LAN", "VR-120HD report identifies model and transport");
  check(vr120["settings"].toObject()["sourceNumber"] == 12 && vr120["capabilities"].toObject()["fade"].toBool(), "VR-120HD report includes SDI input and fade capability");
  check(!QJsonDocument(vr120).toJson().contains("P@55"), "VR-120HD network password omitted from report");
  report.stop(); settings.password = "SECRET";
  QJsonObject profiles{{"v160hd", rolandSettingsJson(settings, true)}};
  check(!QJsonDocument(publicRolandProfilesJson(profiles)).toJson().contains("SECRET"), "settings export removes network passwords");
  profiles["v160hd"] = QJsonObject{{"sourceNumber", 16.5}};
  check(!validRolandProfilesJson(profiles), "fractional source numbers rejected");
  if (ok) std::cout << "PASS: Roland trial profiles, protocol, MIDI baseline and diagnostic report\n";
  return ok;
}
