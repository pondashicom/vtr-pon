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
  static void runLanUsbModels() {
    struct Fixture { RolandSwitcherModel model; int inputs; const char *query; const char *program; const char *address; };
    const Fixture fixtures[]{
      {RolandSwitcherModel::V160Hd,16,"f041100000000002110021000000015ef7","f041100000000002120021000f50f7","002100"},
      {RolandSwitcherModel::V80Hd,8,"f04110010514110015000000016af7","f04110010514120015000764f7","001500"},
      {RolandSwitcherModel::V1_4K,5,"f04110010610110012000000016df7","f0411001061012001200046af7","001200"},
      {RolandSwitcherModel::Vr6Hd,6,"f04110000000002c11000f0000000170f7","f04110000000002c12000f00056cf7","000f00"},
      {RolandSwitcherModel::Vr120Hd,12,"f04110000000002511001b0000000164f7","f04110000000002512001b000b5af7","001b00"}
    };
    for (const auto &f : fixtures) {
      RolandModelSettings settings; settings.useUsbMidi = true; settings.sourceNumber = f.inputs;
      check(rolandTransport(f.model,{}) == RolandTransport::Lan && rolandTransport(f.model,settings) == RolandTransport::Midi,"LAN remains default; USB is opt-in");
      RolandV8HdMidiSwitcher device(f.model,settings,true);
      QByteArray sent; device.writeForTest_ = [&](const QByteArray &m) { sent=m; return true; };
      int ready=0; QList<int> changes;
      QObject::connect(&device,&SwitcherDevice::connectionOpened,&device,[&](bool v,const QString &) { if(v) ++ready; });
      QObject::connect(&device,&SwitcherDevice::programInputChanged,&device,[&](int v) { changes.append(v); });
      const auto response=[&](int raw) { return rolandSysEx(f.model,16,0x12,QByteArray::fromHex(f.address)+char(raw)); };
      device.requestTally(); check(sent==QByteArray::fromHex(f.query),"literal official single-byte PGM RQ1 fixture");
      auto corrupt=response(0); corrupt[corrupt.size()-2]^=1; device.receiveMidiBytes(corrupt);
      device.receiveMidiBytes(rolandSysEx(f.model,1,0x12,QByteArray::fromHex(f.address)+char(0)));
      device.receiveMidiBytes(rolandSysEx(f.model,16,0x12,QByteArray::fromHex(f.address)+QByteArray::fromHex("0001")));
      device.receiveMidiBytes(response(0x7f));
      device.receiveMidiBytes(rolandSysEx(f.model,16,0x12,QByteArray::fromHex("0c000001")));
      check(device.queryPending_ && ready==0,"checksum, device, size, value and unrequested tally cannot establish readiness");
      const auto initial=response(0); device.receiveMidiBytes(initial.left(5)); device.receiveMidiBytes(QByteArray(1,char(0xf8))+initial.mid(5));
      check(ready==1 && changes.isEmpty(),"fragmented initial source is baseline only");
      const auto reply=[&](int value) { device.requestTally(); device.receiveMidiBytes(response(value)); };
      reply(f.inputs-1); check(changes==QList<int>{f.inputs},"last physical input including SDI observed");
      reply(f.inputs-1); check(changes.size()==1,"duplicate status cannot retrigger");
      check(device.setProgramInput(f.inputs) && sent==QByteArray::fromHex(f.program),"literal PGM DT1 fixture, model ID and checksum");
      reply(0); check(device.localTarget_==f.inputs && changes.size()==1,"stale status suppressed during local operation");
      reply(f.inputs-1); check(device.localTarget_==0 && changes.size()==1,"local reflection is not another take");
      reply(f.inputs); check(changes.size()==2 && changes.last()==0,"STILL clears physical target presence");
      reply(f.inputs-1); check(changes.size()==3 && changes.last()==f.inputs,"return from STILL takes once");
      const auto before=sent; check(!device.setFadeToBlack(true) && sent==before,"LAN FTB is not sent over MIDI");
      device.close(); reply(f.inputs-1); check(ready==2 && changes.size()==3,"reconnect has a fresh silent baseline");
      QMetaObject::invokeMethod(&device.responseTimeout_,"timeout",Qt::DirectConnection);
      check(!device.modelReady_ && !device.queryPending_,"response failure clears readiness");
      RolandV8HdMidiSwitcher disabled(f.model,settings,false); disabled.writeForTest_=[](const QByteArray &) { return true; };
      disabled.requestTally(); disabled.receiveMidiBytes(response(0)); disabled.requestTally(); disabled.receiveMidiBytes(response(f.inputs-1));
      check(disabled.modelReady_,"MIDI handshake required even with receive linking disabled");
    }
  }
  static void runParameterMaps() {
    struct Mapping { RolandSwitcherModel model; int logicalFirst; int logicalLast; const char *lastAddress; int nativeLast; int assignMaximum; };
    const Mapping cases[]{
      {RolandSwitcherModel::V160Hd,0x20,0x33,"00002d",15,0x20},
      {RolandSwitcherModel::V80Hd,0x29,0x38,"00000f",7,0x2c},
      {RolandSwitcherModel::V1_4K,0x07,0x0e,"000007",4,4},
      {RolandSwitcherModel::Vr6Hd,0x17,0x1c,"000005",5,0x17},
      {RolandSwitcherModel::Vr120Hd,0x1d,0x24,"000007",11,0x20}
    };
    for(const auto &f:cases) {
      RolandProgramQuery query(f.model);
      check(query.accept(f.nativeLast)==RolandProgramQuery::Result::Observed && query.input()==f.nativeLast,"last native HDMI/SDI PGM source");
      query.reset(); check(query.accept(f.nativeLast+1)==RolandProgramQuery::Result::Observed && query.input()==-1,"first still is not a physical input");
      query.reset(); check(query.accept(f.logicalLast)==RolandProgramQuery::Result::More && query.address()==QByteArray::fromHex(f.lastAddress),"last logical slot has its documented assignment address");
      const int assigned=f.model==RolandSwitcherModel::V1_4K?1:f.nativeLast;
      check(query.accept(assigned)==RolandProgramQuery::Result::More,"assignment requires fresh PGM confirmation");
      check(query.accept(f.logicalLast)==RolandProgramQuery::Result::Observed && query.input()==(f.model==RolandSwitcherModel::V1_4K?-1:f.nativeLast),"logical source resolves without treating slot number as HDMI");
      query.reset(); query.accept(f.logicalFirst); query.accept(0);
      check(query.accept(f.nativeLast)==RolandProgramQuery::Result::Observed && query.input()==f.nativeLast,"PGM changed during assignment read follows newest source");
      query.reset(); query.accept(f.logicalFirst); check(query.accept(f.assignMaximum+1)==RolandProgramQuery::Result::Invalid,"invalid assignment rejected");
      query.reset(); query.accept(f.logicalFirst); query.accept(0); query.accept(f.logicalFirst);
      check(query.input()==0,"first logical INPUT maps to native HDMI1 when assigned");
    }
    RolandProgramQuery v160(RolandSwitcherModel::V160Hd);
    v160.accept(0x29); check(v160.address()==QByteArray::fromHex("000009"),"V-160HD INPUT10 assignment boundary");
    v160.reset(); v160.accept(0x2a); check(v160.address()==QByteArray::fromHex("000024"),"V-160HD INPUT11 noncontiguous assignment boundary");
    RolandProgramQuery v1(RolandSwitcherModel::V1_4K);
    check(v1.accept(15)==RolandProgramQuery::Result::Observed && v1.input()==-1,"V-1-4K direct BLACK");
    v1.reset(); v1.accept(11); v1.accept(0); v1.accept(11); check(v1.input()==4,"V-1-4K slot5 assignment0 means HDMI5, not HDMI1");
    v1.reset(); v1.accept(12); check(v1.accept(0)==RolandProgramQuery::Result::Invalid,"V-1-4K slots6-8 have no native HDMI");
  }
  static void run() {
    runLanUsbModels(); runParameterMaps();
    {
      // V-1SDI Reference p.11/20: A/PGM is valid only in PGM/PST;
      // OUTPUT FADER ASSIGN=2 controls audio alone, and must not be used as FTB.
      RolandV8HdMidiSwitcher device(RolandSwitcherModel::V1Sdi,{},true);
      QByteArray sent; device.writeForTest_=[&](const QByteArray &wire) { sent=wire; return true; };
      const auto mode=QByteArray::fromHex("f041100000003112700100010ef7");
      const auto pgm=QByteArray::fromHex("f0411000000031127103080004f7");
      device.requestTally();
      check(sent==QByteArray::fromHex("f0411000000031117001000000010ef7"),"V-1SDI starts with documented panel mode RQ1");
      device.receiveMidiBytes(mode);
      check(sent==QByteArray::fromHex("f0411000000031117001210000016df7"),"V-1SDI reads output fader assignment");
      device.receiveMidiBytes(QByteArray::fromHex("f041100000003112700121026cf7"));
      check(sent==QByteArray::fromHex("f04110000000311171030800000103f7"),"V-1SDI reads A/PGM after prerequisites");
      device.receiveMidiBytes(pgm); const auto before=sent;
      check(device.modelReady_ && !device.setFadeToBlack(true) && sent==before,"audio-only fader assignment refuses FTB without a write");
      device.requestTally(); device.receiveMidiBytes(QByteArray::fromHex("f041100000003112700100000ff7"));
      check(!device.modelReady_ && !device.setProgramInput(1),"V-1SDI A/B mode cannot control the wrong active bus");
      device.close(); device.requestTally(); device.receiveMidiBytes(mode);
      device.receiveMidiBytes(QByteArray::fromHex("f041100000003112700121006ef7")); device.receiveMidiBytes(pgm);
      check(device.setFadeToBlack(true) && sent==QByteArray::fromHex("f041100000003112710306000006f7"),"V-1SDI black endpoint is a two-byte value");
      check(device.setFadeToBlack(false) && sent==QByteArray::fromHex("f041100000003112710306010005f7"),"V-1SDI neutral endpoint is 128, encoded as 01 00");
    }
    const auto query = rolandSysEx(RolandSwitcherModel::V1Hd, 0x10, 0x11, QByteArray::fromHex("710008000001"));
    check(query == QByteArray::fromHex("f04110000000201171000800000106f7"), "V-1HD documented model ID, address and checksum");
    check(rolandSysEx(RolandSwitcherModel::V02HdMk2, 0x10, 0x12, QByteArray::fromHex("00250001")) ==
          QByteArray::fromHex("f04110000000000a12002500015af7"), "V-02HD MK II five-byte model ID");
    {
      RolandV8HdMidiSwitcher legacy(true); int changes=0, ready=0;
      legacy.writeForTest_=[](const QByteArray &) { return true; };
      QObject::connect(&legacy,&SwitcherDevice::programInputChanged,&legacy,[&](int) { ++changes; });
      QObject::connect(&legacy,&SwitcherDevice::connectionOpened,&legacy,[&](bool v,const QString &) { if(v) ++ready; });
      check(legacy.opensAsynchronously() && !legacy.setProgramInput(1),"V-8HD cannot operate before a real status reply");
      legacy.requestTally();
      legacy.parseDataSetMessage(QByteArray::fromHex("f0411000000068120201050177f7"));
      legacy.parseDataSetMessage(QByteArray::fromHex("f041100000006812001200006ef7"));
      check(ready==1 && changes==0,"V-8HD PGM source baseline");
      legacy.requestTally();
      legacy.parseDataSetMessage(QByteArray::fromHex("f0411000000068120201050177f7"));
      legacy.parseDataSetMessage(rolandSysEx(RolandSwitcherModel::V8Hd,16,0x12,QByteArray::fromHex("0c00000001000000000000")));
      check(changes==0 && legacy.queryPending_,"physical HDMI tally cannot be a logical INPUT selection");
      legacy.parseDataSetMessage(QByteArray::fromHex("f041100000006812001200016df7"));
      check(changes==1,"V-8HD subsequent actual PGM source still works");
      legacy.requestTally(); legacy.parseDataSetMessage(QByteArray::fromHex("f0411000000068120201050078f7"));
      check(!legacy.modelReady_ && !legacy.setProgramInput(1),"A/B mode cannot be reported as verified PGM linking");
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
  static void runLanAuthentication() {
    for (auto model : {RolandSwitcherModel::V600Uhd, RolandSwitcherModel::V160Hd,
                       RolandSwitcherModel::V80Hd, RolandSwitcherModel::V1_4K,
                       RolandSwitcherModel::Vr6Hd, RolandSwitcherModel::Vr120Hd}) {
      RolandModelSettings settings;
      settings.password = rolandSwitcherProfile(model).passwordLength == 8 ? "Password" : "Pass";
      RolandRemoteSwitcher device(model, settings, true);
      QList<QByteArray> sent; QStringList errors, diagnostics;
      device.writeForTest_ = [&](const QByteArray &wire) { sent.append(wire); return true; };
      QObject::connect(&device, &SwitcherDevice::errorOccurred, &device, [&](const QString &s) { errors.append(s); });
      QObject::connect(&device, &SwitcherDevice::diagnosticEvent, &device, [&](const QString &kind, const QString &s) { if (kind == "AUTH") diagnostics.append(s); });
      const auto begin = [&] { device.close(); sent.clear(); errors.clear(); device.active_ = device.authenticating_ = true; };
      begin();
      device.receive(QByteArray::fromHex("fffb01") + "Enter pass");
      check(!device.passwordSent_, "split prompt does not send password early");
      device.receive("word:");
      const auto passwordLine = settings.password.toLatin1() + (model == RolandSwitcherModel::V80Hd || model == RolandSwitcherModel::Vr120Hd ? "\r\n" : "\n");
      check(sent.count(passwordLine) == 1, "model-specific password framing after prompt");
      settle(180);
      check(device.authenticating_ && device.pending_.reply == RolandRemoteSwitcher::Reply::None, "elapsed time cannot establish authentication");
      device.receive(settings.password.toLatin1() + "\r\n");
      check(errors.isEmpty() && device.authenticating_, "password echo is not a repeated prompt");
      const auto welcome = QByteArray("Welcome to ") + (model == RolandSwitcherModel::V600Uhd ? "Roland " : "") + rolandSwitcherProfile(model).name + '.';
      device.receive(welcome.left(7)); check(device.authenticating_, "split Welcome cannot authenticate prematurely");
      device.receive(welcome.mid(7)); settle(5);
      check(!device.authenticating_ && device.pending_.reply == RolandRemoteSwitcher::Reply::Version, "unterminated model Welcome releases version query");
      check(sent.last() == QByteArray("VER;") + device.lanLineEnding(false), "normal LAN commands have model-specific line endings");
      check(diagnostics == QStringList{"Password sent (omitted).", "Authentication accepted."}, "authentication diagnostics contain only fixed messages, never password echoes");
      if (device.needsQueryAck()) {
        device.receive(frame("ACK"));
        check(device.pending_.reply == RolandRemoteSwitcher::Reply::Version, "query ACK before version data cannot complete it alone");
      }
      device.receive(frame(QByteArray("VER:") + rolandSwitcherProfile(model).name + ",1.20")); settle(5);
      check(rolandSwitcherProfile(model).receive ? device.pending_.reply == RolandRemoteSwitcher::Reply::Program : device.ready_,
            "documented ACK-plus-data and data-only query families both complete");

      for (const auto &failure : {QByteArray("Authentication error\r\n"), QByteArray("Wait a moment\r\n"), QByteArray("Enter password:")}) {
        begin(); device.receive("Enter password:"); device.receive(failure);
        check(!device.active_ && !device.reconnect_.isActive() && sent.count(passwordLine) == 1 && !errors.isEmpty(), "rejected or locked-out authentication stops without retrying password");
      }
      begin(); device.receive("Enter password:"); device.responseTimeout();
      check(!device.active_ && !device.reconnect_.isActive() && errors.last().contains("authentication result"), "silent password response stops automatic retries");
      begin(); device.responseTimeout();
      check(device.reconnect_.isActive() && errors.last().contains("prompt"), "unprompted second session has its own diagnostic and may reconnect");
      begin(); device.authenticating_ = false; device.connecting_ = true; device.responseTimeout();
      check(errors.last().contains("TCP connection timed out") && !errors.last().contains("No response to"), "TCP timeout is distinct from command timeout");
      device.close();
      for (const auto &echo : (rolandSwitcherProfile(model).passwordLength == 8 ?
          QList<QByteArray>{"VER:ABCD", "Welcome "} : QList<QByteArray>{"VER:"})) {
        settings.password = QString::fromLatin1(echo);
        RolandRemoteSwitcher echoed(model, settings, true);
        QList<QByteArray> echoWrites;
        echoed.writeForTest_ = [&](const QByteArray &wire) { echoWrites.append(wire); return true; };
        echoed.active_ = echoed.authenticating_ = true;
        echoed.receive("Enter password:"); echoed.receive(echo + "\r\n"); settle(5);
        check(echoed.authenticating_ && echoed.pending_.reply == RolandRemoteSwitcher::Reply::None && echoWrites.size() == 1,
              "password echo resembling an auth marker cannot release command traffic");
        echoed.receive(frame(QByteArray("VER:") + rolandSwitcherProfile(model).name + ",1.20")); settle(5);
        check(!echoed.authenticating_ && echoed.pending_.reply == RolandRemoteSwitcher::Reply::Version,
              "complete matching version-as-login is accepted and still formally verified");
        echoed.close();
      }
    }
  }
  static void runScalarState() {
    RolandModelSettings settings; settings.sourceNumber=2;
    RolandRemoteSwitcher device(RolandSwitcherModel::V80Hd,settings,true);
    QList<QByteArray> sent; QList<int> changes;
    device.writeForTest_=[&](const QByteArray &wire) { sent.append(wire); return true; }; device.active_=true;
    QObject::connect(&device,&SwitcherDevice::programInputChanged,&device,[&](int v) { changes.append(v); });
    const auto respond=[&](const QByteArray &value) {
      settle(3); const auto address=device.pending_.body.mid(4,6);
      device.receive(frame("DTH:"+address+','+value)); device.receive(frame("ACK")); settle(3);
    };
    device.enqueue({device.statusQuery(),RolandRemoteSwitcher::Reply::Program});
    check(waitFor([&] { return !sent.isEmpty(); }) && sent.last()=="RQH:001500,000001;\r\n","LAN starts at actual PGM source");
    respond("00"); check(device.ready_ && changes.isEmpty(),"initial off-target state is silent baseline");
    device.poll(); respond("01"); check(changes==QList<int>{2},"native HDMI2 PGM selection detected");
    device.poll(); settle(3); device.receive(frame("DTH:0C0001,00"));
    check(device.pending_.response.isEmpty(),"tally/AUX/FTB unsolicited data cannot override PGM selection");
    respond("01"); check(changes.size()==1,"FTB without a source change never retakes");
    device.poll(); respond("08"); check(changes.size()==2 && changes.last()==0,"STILL clears configured target");
    device.poll(); respond("29");
    check(device.pending_.body=="RQH:000000,000001;","logical INPUT1 reads assignment instead of assuming HDMI1");
    respond("01"); check(device.pending_.body=="RQH:001500,000001;","assignment rechecks PGM selection");
    respond("29"); check(changes.size()==3 && changes.last()==2,"reassigned logical INPUT1 resolves to HDMI2");
    device.poll(); respond("29"); respond("01"); respond("02");
    check(changes.size()==4 && changes.last()==0,"source change during assignment read follows latest HDMI3");
    device.poll(); settle(3); device.receive(frame("ACK"));
    check(device.pending_.reply==RolandRemoteSwitcher::Reply::Program && device.pending_.response.isEmpty(),"ACK alone cannot complete read");
    device.receive(frame("DTH:001500,01")); settle(3);
    check(changes.size()==5,"ACK before data completes read");
    device.poll(); settle(3); device.receive(frame("DTH:001500,01")); device.setProgramInput(2); settle(3);
    check(device.pending_.reply==RolandRemoteSwitcher::Reply::Program,"write waits for query ACK");
    device.receive(frame("ACK")); settle(3); check(device.pending_.reply==RolandRemoteSwitcher::Reply::Ack,"query ACK releases write without acknowledging it");
    device.receive(frame("ACK")); settle(3);
    device.poll(); settle(3); const auto before=sent.size(); device.responseTimeout();
    check(sent.size()==before+1 && device.pending_.readRetries==1 && device.ready_,"one dropped V-80HD read is retried on the same session");
    respond("01"); check(!device.reconnect_.isActive(),"read retry can recover");
    device.poll(); settle(3); device.responseTimeout(); device.responseTimeout();
    check(device.reconnect_.isActive() && !device.ready_,"second read timeout fails and clears readiness"); device.close();
    for (auto model:{RolandSwitcherModel::V160Hd,RolandSwitcherModel::V80Hd,RolandSwitcherModel::V1_4K,RolandSwitcherModel::Vr6Hd,RolandSwitcherModel::Vr120Hd}) {
      RolandRemoteSwitcher fade(model,settings,false); fade.active_=fade.ready_=true;
      fade.writeForTest_=[](const QByteArray &) { return true; }; QString error;
      check(!fade.setFadeToBlack(true,&error),"FTB cannot use an unknown firmware version");
      fade.firmwareVersion_="1.00";
      if(model!=RolandSwitcherModel::V1_4K) check(!fade.setFadeToBlack(true,&error),"older firmware cannot receive unsupported FTB command");
      fade.firmwareVersion_=rolandMinimumFadeVersion(model);
      check(fade.setFadeToBlack(true,&error),"documented minimum firmware accepts explicit FTB"); settle(3);
      check(fade.pending_.body=="FTB:ON;","basic FTB uses explicit ON, never toggle or panel lock");
      const int queued=fade.queue_.size(); fade.responseTimeout();
      check(!fade.ready_ && fade.queue_.isEmpty() && queued==0,"unacknowledged write is never retried"); fade.close();
    }
  }
  static void runSerial() {
    RolandModelSettings settings; settings.sourceNumber = 4;
    RolandRemoteSwitcher device(RolandSwitcherModel::V1HdPlus, settings, true);
    QList<QByteArray> sent;
    device.writeForTest_ = [&](const QByteArray &wire) { sent.append(wire); return true; };
    device.active_ = true;
    bool ready = false; int changes = 0;
    QObject::connect(&device, &SwitcherDevice::connectionOpened, &device, [&](bool value, const QString &) { ready = value; });
    QObject::connect(&device, &SwitcherDevice::programInputChanged, &device, [&](int) { ++changes; });
    device.enqueue({"VER;", RolandRemoteSwitcher::Reply::Version}); settle(5);
    check(sent.last() == frame("VER"), "RS-232 uses STX framing");
    device.receive("VER:V-1HD PLUS,1.0;"); settle(5);
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
  RolandMidiTrialTest::run(); RolandRemoteProtocolTest::runLanAuthentication(); RolandRemoteProtocolTest::runScalarState(); RolandRemoteProtocolTest::runSerial();
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
    int currentInput = 0, programSelection = 0;
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
          buffer.remove(0, end + 1); authenticated = true;
          const auto welcome = QByteArray("Welcome to ") + (f.model == RolandSwitcherModel::V600Uhd ? "Roland " : "") + f.name + '.';
          QTimer::singleShot(220, client, [client, welcome] { client->write(welcome); });
        }
        while (buffer.contains(';')) {
          const int end = buffer.indexOf(';'); const auto body = buffer.left(end + 1).trimmed(); buffer.remove(0, end + 1); commands.append(body);
          check(!body.startsWith(char(2)), "LAN omits STX according to command reference");
          if (body == "VER;") {
            const auto reply = frame(QByteArray("VER:") + (wrongModel ? "OTHER" : f.name)
                + ",9.90");
            client->write(reply.left(4)); QTimer::singleShot(5, client, [client, reply, needsAck = f.model == RolandSwitcherModel::V80Hd || f.model == RolandSwitcherModel::V1_4K] { client->write(reply.mid(4) + (needsAck ? frame("ACK") : QByteArray())); });
          } else if (body.startsWith("RQH:")) {
            const auto address = body.mid(4, 6);
            check(body.endsWith(",000001;"), "LAN never requires unsupported multi-byte parameter reads");
            if (currentInput >= 0) programSelection = currentInput;
            int value = 0;
            if (address == f.address) value = programSelection;
            else {
              check(address.startsWith("0C"), "state query uses documented program or tally address");
              const int index = address.mid(2).toInt(nullptr, 16);
              check(index >= 0 && index < f.tallies, "tally query stays within the model's documented range");
              value = currentInput == index ? 1 : 0;
            }
            if (!ignoreQuery) {
              const auto answer = frame("DTH:" + address + ',' + QByteArray::number(value, 16).rightJustified(2, '0').toUpper());
              client->write(answer);
              if (f.model == RolandSwitcherModel::V80Hd || f.model == RolandSwitcherModel::V1_4K)
                QTimer::singleShot(3, client, [client] { client->write(frame("ACK")); });
            }
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
    if (f.model == RolandSwitcherModel::V1_4K) {
      const int beforeBlack = changes; currentInput = 15;
      check(waitFor([&] { return changes == beforeBlack + 1; }), "V-1-4K explicit BLACK is a real source change");
      check(!commands.contains("RQH:0C000F,000001;"), "BLACK does not require a fabricated tally address");
      currentInput = f.source - 1;
      check(waitFor([&] { return changes == beforeBlack + 2; }), "return from explicit BLACK is observed");
    }
    check(!device.setProgramInput(f.inputs+1), "physical source range enforced");
    check(device.setProgramInput(f.source), "program input queues");
    const auto expected = *f.address ? QByteArray("DTH:") + f.address + ',' + QByteArray::number(f.source-1,16).rightJustified(2,'0').toUpper() + ';' : QByteArrayLiteral("PGM:1;");
    check(waitFor([&] { return commands.contains(expected); }), "input command uses documented address and value");
    settle(350);
    const int before = changes; settle(300); check(changes == before, "local input echo never retriggers");
    const bool fade = device.setFadeToBlack(true);
    check(fade, "all LAN models with documented explicit FTB accept fading");
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
    if (rolandHasUsbMidiChoice(f.model) && f.model != RolandSwitcherModel::Vr120Hd) {
      check(waitFor([&] { return commands.contains("FTB:ON;"); }),"documented modern LAN model sends FTB ON"); settle(30);
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
  check(rolandTransport(RolandSwitcherModel::V160Hd, rolandSettingsFromJson(QJsonObject{{"host", "127.0.0.1"}})) == RolandTransport::Lan,
        "legacy profile without USB key stays on LAN");
  settings.useUsbMidi = true; settings.ftbFollowsProgram = true;
  report.start(RolandSwitcherModel::Vr120Hd, settings);
  const auto usbReport = report.report();
  check(usbReport["transport"] == "USB MIDI" && !usbReport["capabilities"].toObject()["fade"].toBool() &&
        !usbReport["settings"].toObject()["fadeEnabled"].toBool(), "USB report records selected transport and actual FTB capability");
  check(rolandSupportsFade(RolandSwitcherModel::Vr120Hd, RolandTransport::Lan), "LAN VR-120HD FTB remains supported");
  profiles["vr120hd"] = rolandSettingsJson(settings, true);
  check(validRolandProfilesJson(profiles) && rolandSettingsFromJson(publicRolandProfilesJson(profiles)["vr120hd"].toObject()).useUsbMidi,
        "USB choice round-trips through public settings without passwords");
  check(!validRolandProfilesJson({{"v600uhd", QJsonObject{{"useUsbMidi", true}}}}), "unsupported USB choice rejected on import");
  check(!validRolandProfilesJson({{"v160hd", QJsonObject{{"useUsbMidi", "yes"}}}}), "non-boolean USB choice rejected on import");
  profiles["v160hd"] = QJsonObject{{"sourceNumber", 16.5}};
  check(!validRolandProfilesJson(profiles), "fractional source numbers rejected");
  if (ok) std::cout << "PASS: Roland trial profiles, protocol, MIDI baseline and diagnostic report\n";
  return ok;
}
