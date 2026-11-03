#include "mac_roland_midi.h"
#include "mac_midi_service.h"
#include "mac_midi_input.h"
#include "roland_v8hd_midi_switcher.h"
#include "controller_settings.h"
#include "controller_operations.h"
#include "mac_midi_source.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <cstdio>
#include <mutex>
#include <QJsonDocument>
#include <chrono>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL mac-roland line %d: %s\n", __LINE__, #x); return 1; } } while(false)
namespace {
void pump(int ms) {
  QElapsedTimer timer; timer.start();
  while (timer.elapsed() < ms) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
template<class F> bool until(F predicate, int ms = 3500) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < ms) pump(5);
  return predicate();
}
struct Wire { std::mutex mutex; QByteArray bytes; QList<qint64> times; };
class VirtualSwitcher {
public:
  MIDIClientRef client = 0;
  MIDIEndpointRef source = 0, destination = 0;
  SInt32 sourceUid = 0, destinationUid = 0;
  std::shared_ptr<Wire> wire = std::make_shared<Wire>();
  QString name;
  int generation = 0;
  VirtualSwitcher(QString modelName = "VTR-PON Test V-8HD") : name(std::move(modelName)) { MIDIClientCreateWithBlock(CFSTR("VTR-PON Roland test"), &client, nullptr); create(); }
  ~VirtualSwitcher() { disconnect(); if (client) MIDIClientDispose(client); }
  bool create() {
    const auto inbox = wire;
    CFStringRef endpointName = name.toCFString();
    if (MIDIDestinationCreateWithBlock(client, endpointName, &destination,
        ^(const MIDIPacketList *list, void *) {
          std::lock_guard lock(inbox->mutex);
          auto *packet = &list->packet[0];
          for (UInt32 i = 0; i < list->numPackets; ++i, packet = MIDIPacketNext(packet)) {
            inbox->bytes.append(reinterpret_cast<const char *>(packet->data), packet->length);
            inbox->times.append(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
          }
        }) != noErr) return false;
    if (MIDISourceCreate(client, endpointName, &source) != noErr) return false;
    CFRelease(endpointName);
    if (sourceUid) {
      if (MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, sourceUid) != noErr ||
          MIDIObjectSetIntegerProperty(destination, kMIDIPropertyUniqueID, destinationUid) != noErr) return false;
    } else {
      MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &sourceUid);
      MIDIObjectGetIntegerProperty(destination, kMIDIPropertyUniqueID, &destinationUid);
    }
    return true;
  }
  void disconnect() {
    if (source) MIDIEndpointDispose(source);
    if (destination) MIDIEndpointDispose(destination);
    source = destination = 0;
  }
  bool send(const QByteArray &bytes) {
    alignas(MIDIPacketList) std::array<char, 4096> storage{};
    auto *list = reinterpret_cast<MIDIPacketList *>(storage.data());
    auto *packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, storage.size(), packet, 0, bytes.size(),
        reinterpret_cast<const Byte *>(bytes.constData()));
    return source && packet && MIDIReceived(source, list) == noErr;
  }
  QByteArray bytes() { std::lock_guard lock(wire->mutex); return wire->bytes; }
  void clear() { ++generation; std::lock_guard lock(wire->mutex); wire->bytes.clear(); wire->times.clear(); }
  QList<qint64> times() { std::lock_guard lock(wire->mutex); return wire->times; }
};
QByteArray dataset(QByteArray body) {
  int sum = 0;
  for (const char byte : body) sum += static_cast<unsigned char>(byte);
  return QByteArray::fromHex("f041100000006812") + body + char((128 - sum % 128) % 128) + char(0xf7);
}
MIDISysexSendRequest *held = nullptr;
OSStatus stalled(MIDISysexSendRequest *request) { held = request; return noErr; }
void releaseHeld() { auto *request = std::exchange(held, nullptr); if (request) request->completionProc(request); }
}

int runMacRolandMidiRegression() {
  auto &app = *qobject_cast<QApplication *>(QCoreApplication::instance());
  // Initial discovery belongs to the permanent run loop, including this test's
  // first call and endpoints later recreated by another process/test fixture.
  MIDIClientRef service = 0; CHECK(vtrpon::macMidi::client(service) == noErr);
  VirtualSwitcher device;
  CHECK(device.source && device.destination);
  CHECK(until([&] { return MIDIGetNumberOfDestinations() > 0; }));
  const auto id = QStringLiteral("coremidi:%1").arg(device.sourceUid);
  auto queue = std::make_shared<vtrpon::midiControl::MessageQueue>();
  vtrpon::MacMidiInput generic(queue);
  const auto devices = vtrpon::MacMidiInput::devices();
  const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto &candidate) { return candidate.id == id; });
  CHECK(found != devices.end());
  CHECK(generic.open(*found).isEmpty());

  vtrpon::RolandV8HdMidiSwitcher roland(true);
  QList<int> selections;
  int errors = 0, ready = 0;
  QObject::connect(&roland, &vtrpon::SwitcherDevice::programInputChanged, &app, [&](int input) { selections.append(input); });
  QObject::connect(&roland, &vtrpon::SwitcherDevice::errorOccurred, &app, [&](const QString &) { ++errors; });
  QObject::connect(&roland, &vtrpon::SwitcherDevice::connectionOpened, &app, [&](bool ok, const QString &) { if (ok) ++ready; });
  QString error;
  CHECK(roland.opensAsynchronously() && roland.open(&error));
  CHECK(generic.reserved() && ready==0 && !roland.setProgramInput(1,&error));
  const auto query=QByteArray::fromHex("f0411000000068110012000000016df7");
  const auto modeQuery=QByteArray::fromHex("f04110000000681102010500000177f7");
  CHECK(until([&] { return device.bytes().contains(modeQuery); }));
  CHECK(device.send(dataset(QByteArray::fromHex("02010501"))));
  CHECK(until([&] { return device.bytes().contains(query); }));
  auto invalidInitial=dataset(QByteArray::fromHex("00120000")); invalidInitial[invalidInitial.size()-2]^=1;
  CHECK(device.send(invalidInitial)); pump(40); CHECK(ready==0);
  int current=0, seenQueries=0, seenModes=device.bytes().count(modeQuery), seenGeneration=device.generation;
  bool fragment=false;
  QTimer replies; replies.setInterval(5);
  QObject::connect(&replies,&QTimer::timeout,&app,[&] {
    if(seenGeneration!=device.generation) { seenGeneration=device.generation; seenQueries=seenModes=0; }
    const int modes=device.bytes().count(modeQuery);
    if(modes>seenModes) { seenModes=modes; device.send(dataset(QByteArray::fromHex("02010501"))); }
    const int n=device.bytes().count(query);
    if(n<=seenQueries) return;
    seenQueries=n; const auto response=dataset(QByteArray::fromHex("001200")+char(current));
    if(fragment) {
      fragment=false; device.send(response.left(9));
      QTimer::singleShot(15,&replies,[&,response] { device.send(QByteArray(1,char(0xf8))+response.mid(9)); });
    } else device.send(response);
  });
  replies.start(); CHECK(until([&] { return ready==1; })); CHECK(selections.isEmpty());
  CHECK(roland.setProgramInput(6,&error));
  CHECK(until([&] { return device.bytes().contains(dataset(QByteArray::fromHex("00120005"))); }));
  CHECK(!roland.setProgramInput(0,&error) && !roland.setProgramInput(9,&error));
  CHECK(roland.setFadeToBlack(true,&error));
  CHECK(until([&] { return device.bytes().contains(QByteArray::fromHex("b02300b0223f")); }));
  CHECK(device.bytes().contains(dataset(QByteArray::fromHex("02010700"))));
  CHECK(roland.setFadeToBlack(false,&error));
  CHECK(until([&] { return device.bytes().contains(QByteArray::fromHex("b02300b02200")); }));
  CHECK(device.send(QByteArray::fromHex("c003"))); pump(50); CHECK(selections.isEmpty());
  current=5; pump(300); CHECK(selections.isEmpty()); // Local echo is observed silently.
  CHECK(device.send(QByteArray::fromHex("c003"))); pump(80); CHECK(selections.isEmpty());
  current=3; CHECK(until([&] { return selections==QList<int>{4}; }));
  // Physical HDMI tally belongs to a different namespace than logical INPUT.
  const auto tally=dataset(QByteArray::fromHex("0c00000000000000010000"));
  CHECK(device.send(tally)); pump(80); CHECK(selections==QList<int>{4});
  fragment=true; current=5; CHECK(until([&] { return selections==(QList<int>{4,6}); }));
  pump(300); CHECK(selections.size()==2);
  auto bad=dataset(QByteArray::fromHex("00120001")); bad[bad.size()-2]^=1; CHECK(device.send(bad));
  auto otherModel=dataset(QByteArray::fromHex("00120001")); otherModel[6]=0x69; CHECK(device.send(otherModel)); pump(80); CHECK(selections.size()==2);
  const auto genericPending=queue->take(); CHECK(genericPending.received==0);
  generic.close(); CHECK(!generic.open(*found).isEmpty());
  std::puts("PASS V-8HD official RQ1/DT1, real status readiness, Program Change/tally rejection, fragmented PGM, FTB and generic reservation");

  const int previousReady=ready; device.disconnect(); CHECK(until([&] { return errors>0; }));
  const int countBefore=selections.size(); device.clear(); current=2; CHECK(device.create());
  CHECK(until([&] { return ready>previousReady; })); CHECK(selections.size()==countBefore);
  CHECK(device.send(QByteArray::fromHex("c003"))); pump(80); CHECK(selections.size()==countBefore);
  current=3; CHECK(until([&] { return selections.size()==countBefore+1 && selections.last()==4; }));
  current=4; CHECK(until([&] { return selections.last()==5; }));
  CHECK(roland.setProgramInput(7,&error)); current=1; pump(300); CHECK(selections.last()==5);
  CHECK(until([&] { return selections.last()==2; },6000)); // A failed local switch cannot suppress later observations forever.
  CHECK(roland.setProgramInput(8,&error));
  CHECK(until([&] { return device.bytes().contains(dataset(QByteArray::fromHex("00120007"))); }));
  roland.close(); const auto afterClose=selections.size(); pump(100); CHECK(selections.size()==afterClose);
  CHECK(!vtrpon::macMidi::sourceReserved(device.source));
  CHECK(generic.open({id,"VTR-PON Test V-8HD",device.source}).isEmpty()); generic.close();
  CHECK(roland.open(&error)); current=2;
  CHECK(until([&] { return ready>previousReady+1; })); CHECK(selections.size()==afterClose);
  roland.close();
  std::puts("PASS V-8HD persistent-ID reconnect, silent fresh baseline, subsequent operation, local deadline and close");

  {
    vtrpon::MacRolandMidi transport(stalled);
    CHECK(transport.open(false, &error));
    CHECK(transport.send(dataset(QByteArray::fromHex("00120000")), &error));
    CHECK(held && !held->complete);
    QList<QByteArray> messages;
    CHECK(until([&] { return !transport.poll(messages, &error); }, 2600));
    CHECK(error.contains("timed out"));
    QElapsedTimer closeTime; closeTime.start(); transport.close(); CHECK(closeTime.elapsed() < 100);
    CHECK(held->complete && held->data[0] == 0xf0); // No freed request or payload.
  }
  releaseHeld(); // Driver may complete AFTER transport destruction.
  {
    vtrpon::MacRolandMidi transport(stalled);
    CHECK(transport.open(false, &error));
    CHECK(transport.send(dataset(QByteArray::fromHex("00120000")), &error));
    for (int i = 0; i < 128; ++i) CHECK(transport.send(QByteArray::fromHex("b02200"), &error));
    CHECK(!transport.send(QByteArray::fromHex("b02200"), &error));
    CHECK(error.contains("queue is full"));
    transport.close();
  }
  releaseHeld();
  {
    vtrpon::MacRolandMidi transport;
    CHECK(transport.open(true, &error));
    QByteArray flood;
    for (int i = 0; i < 200; ++i) flood += QByteArray::fromHex("c001");
    CHECK(device.send(flood)); pump(100);
    QList<QByteArray> messages;
    CHECK(!transport.poll(messages, &error) && error.contains("overflow"));
    transport.close();
  }
  std::puts("PASS output deadline, bounded queue/receive overflow, nonblocking cancellation and post-destruction completion");
  {
    CHECK(vtrpon::saveSwitcherControlSettings({}));
    vtrpon::SwitcherController controller(nullptr);
    vtrpon::SwitcherControlSettings configuration;
    configuration.rolandV8HdEnabled = configuration.takeProgramWhenOnAir = true;
    configuration.takeOnAirWhenProgramSelected = configuration.ftbFollowsProgram = true;
    configuration.rolandV8HdSourceNumber = 5;
    CHECK(vtrpon::saveSwitcherControlSettings(configuration));
    CHECK(vtrpon::loadSwitcherControlSettings() == configuration);
    controller.applySettings(configuration);
    CHECK(until([&] { return controller.switcherReady(); }));
    device.clear();
    CHECK(QMetaObject::invokeMethod(&controller, "handleProgramOnAirChanged", Qt::DirectConnection, Q_ARG(bool, true)));
    CHECK(until([&] { return device.bytes().contains(dataset(QByteArray::fromHex("00120004"))); }));
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue(vtrpon::midiControl::settingsKey(), QJsonObject{{"enabled", true}, {"device", id},
      {"bindings", QJsonObject{{"TakeOnAir", QJsonObject{{"type", "note"}, {"channel", 1}, {"number", 60}}}}}}.toVariantMap());
    settings.sync();
    ControllerOperations operations;
    int genericActions = 0;
    vtrpon::MidiControlInput input(&operations, [&](const QString &) { ++genericActions; });
    CHECK(until([&] { return input.statusKey() == "midi.open_failed"; }));
    CHECK(input.errorDetail().contains("reserved"));
    CHECK(device.send(QByteArray::fromHex("903c7f"))); pump(50); CHECK(genericActions == 0);
    MacMidiSource separate(QStringLiteral("VTR-PON independent MIDI controller"));
    CHECK(separate.endpoint);
    auto midi = settings.value(vtrpon::midiControl::settingsKey()).toJsonObject();
    midi["device"] = separate.id();
    settings.setValue(vtrpon::midiControl::settingsKey(), midi.toVariantMap()); settings.sync(); input.reload();
    CHECK(until([&] { return input.statusKey() == "midi.connected"; }));
    CHECK(separate.voice(0x90, 60, 127)); CHECK(until([&] { return genericActions == 1; }));
    device.disconnect();
    CHECK(until([&] { return !controller.switcherReady(); }));
    CHECK(device.create());
    CHECK(until([&] { return controller.switcherReady(); }));
    device.clear();
    CHECK(QMetaObject::invokeMethod(&controller, "handleProgramOnAirChanged", Qt::DirectConnection, Q_ARG(bool, true)));
    CHECK(until([&] { return device.bytes().contains(dataset(QByteArray::fromHex("00120004"))); }));
    QElapsedTimer shutdown; shutdown.start(); controller.requestShutdown(); CHECK(shutdown.elapsed() < 100);
    CHECK(until([&] { return controller.isShutdownComplete(); }));
    CHECK(!vtrpon::macMidi::sourceReserved(device.source));
    input.stop();
    // A saved ON setting also recovers when the device is missing at launch.
    device.disconnect();
    vtrpon::SwitcherController restored(nullptr);
    CHECK(until([&] { return restored.switcherConnectionDetail().contains("not found"); }));
    CHECK(!restored.switcherReady());
    CHECK(device.create());
    CHECK(until([&] { return restored.switcherReady(); }));
    restored.requestShutdown();
    CHECK(until([&] { return restored.isShutdownComplete(); }));
    CHECK(vtrpon::saveSwitcherControlSettings({}));
  }
  std::puts("PASS Controller worker commands, saved settings/missing-device recovery, separate generic MIDI input, async shutdown");
  std::puts("MAC_ROLAND_MIDI result=PASS");
  return 0;
}

int runMacRolandProfilesRegression() {
  using namespace vtrpon;
  auto &app = *qobject_cast<QApplication *>(QCoreApplication::instance());
  for (const auto model : {RolandSwitcherModel::V1Sdi, RolandSwitcherModel::V02HdMk2, RolandSwitcherModel::Vr4Hd}) {
    const auto &p = rolandSwitcherProfile(model);
    VirtualSwitcher device(QStringLiteral("VTR-PON Test ") + QLatin1String(p.name));
    CHECK(device.source && device.destination);
    QTimer prerequisites; prerequisites.setInterval(5);
    int generation=-1, modes=0, assignments=0;
    QObject::connect(&prerequisites,&QTimer::timeout,&app,[&] {
      if(model!=RolandSwitcherModel::V1Sdi) return;
      if(generation!=device.generation) { generation=device.generation; modes=assignments=0; }
      const auto bytes=device.bytes();
      const int nextModes=bytes.count(QByteArray::fromHex("f0411000000031117001000000010ef7"));
      if(nextModes>modes) { modes=nextModes; device.send(QByteArray::fromHex("f041100000003112700100010ef7")); }
      const int nextAssignments=bytes.count(QByteArray::fromHex("f0411000000031117001210000016df7"));
      if(nextAssignments>assignments) { assignments=nextAssignments; device.send(QByteArray::fromHex("f041100000003112700121006ef7")); }
    }); prerequisites.start();
    const auto address = QByteArray::fromHex(p.programAddress);
    const auto query = rolandSysEx(model, 0x10, 0x11, address + QByteArray::fromHex("000001"));
    const auto response = [&](int source) { return rolandSysEx(model, 0x10, 0x12, address + QByteArray(1, char(source-1))); };
    RolandV8HdMidiSwitcher adapter(model, {}, true);
    int ready = 0, errors = 0;
    QList<int> changes;
    QObject::connect(&adapter, &SwitcherDevice::connectionOpened, &app, [&](bool value, const QString &) { if (value) ++ready; });
    QObject::connect(&adapter, &SwitcherDevice::errorOccurred, &app, [&](const QString &) { ++errors; });
    QObject::connect(&adapter, &SwitcherDevice::programInputChanged, &app, [&](int input) { changes.append(input); });
    QString error;
    CHECK(adapter.opensAsynchronously() && adapter.open(&error));
    CHECK(until([&] { return device.bytes().contains(query); }));
    CHECK(ready == 0 && macMidi::sourceReserved(device.source));
    auto corrupt = response(1); corrupt[corrupt.size()-2] ^= 1;
    CHECK(device.send(corrupt)); pump(40); CHECK(ready == 0);
    CHECK(device.send(rolandSysEx(model, 0x01, 0x12, address + QByteArray(1, char(0))))); pump(40); CHECK(ready == 0);
    CHECK(device.send(response(1))); CHECK(until([&] { return ready == 1; }));
    CHECK(changes.isEmpty());
    int queries = device.bytes().count(query);
    const auto reply = [&](int source) {
      if (!until([&] { return device.bytes().count(query) > queries; })) return false;
      queries = device.bytes().count(query);
      if (!device.send(response(source))) return false;
      pump(45); return true;
    };
    CHECK(reply(2)); CHECK(changes == QList<int>{2});
    CHECK(reply(2)); CHECK(changes.size() == 1);
    CHECK(adapter.setProgramInput(1, &error));
    CHECK(until([&] { return device.bytes().contains(rolandSysEx(model, 16, 0x12, address + QByteArray(1, char(0)))); }));
    CHECK(reply(2)); CHECK(changes.size() == 1); // old state while the local target is pending
    CHECK(reply(1)); CHECK(changes.size() == 1); // local echo is never a new take
    CHECK(reply(2)); CHECK(changes.size() == 2 && changes.last() == 2);
    CHECK(!adapter.setProgramInput(0, &error) && !adapter.setProgramInput(p.inputs+1, &error));
    CHECK(adapter.setFadeToBlack(true, &error));
    CHECK(adapter.setFadeToBlack(false, &error));
    const auto fadeOn = model == RolandSwitcherModel::V1Sdi ? QByteArray::fromHex("7103060000")
        : model == RolandSwitcherModel::V02HdMk2 ? QByteArray::fromHex("00250300") : QByteArray::fromHex("00070501");
    const auto fadeOff = model == RolandSwitcherModel::V1Sdi ? QByteArray::fromHex("7103060100")
        : model == RolandSwitcherModel::V02HdMk2 ? QByteArray::fromHex("00250340") : QByteArray::fromHex("00070500");
    CHECK(until([&] { return device.bytes().contains(rolandSysEx(model,16,0x12,fadeOn)) && device.bytes().contains(rolandSysEx(model,16,0x12,fadeOff)); }));
    const auto times = device.times();
    for (int i=1; i<times.size(); ++i) CHECK(times[i]-times[i-1] >= 15); // >=20ms pacing with callback scheduling tolerance
    const int before = changes.size();
    device.disconnect(); CHECK(until([&] { return errors > 0; }));
    device.clear(); CHECK(device.create());
    CHECK(until([&] { return device.bytes().contains(query); }));
    CHECK(device.send(response(2))); CHECK(until([&] { return ready == 2; }));
    CHECK(changes.size() == before);
    queries = device.bytes().count(query);
    CHECK(reply(1)); CHECK(changes.size() == before+1 && changes.last() == 1);
    if (model == RolandSwitcherModel::V1Sdi) {
      // A lost response recovers on the same persistent endpoints; no stale input/FTB is replayed.
      const int oldErrors = errors;
      CHECK(until([&] { return errors > oldErrors; }, 3500));
      device.clear();
      CHECK(until([&] { return device.bytes().contains(query); }, 4000));
      CHECK(device.send(response(1))); CHECK(until([&] { return ready == 3; }));
      CHECK(changes.size() == before+1);
      CHECK(!device.bytes().contains(rolandSysEx(model,16,0x12,fadeOn)));
    }
    adapter.close(); CHECK(!macMidi::sourceReserved(device.source));
    std::printf("PASS Mac USB %s: handshake/checksum/device-ID, input/echo, FTB/pacing, same-ID reconnect and reservation\n",p.name);
  }
  {
    VirtualSwitcher device("VTR-PON Test VR-4HD");
    const auto model = RolandSwitcherModel::Vr4Hd;
    const auto address = QByteArray::fromHex("000700");
    const auto query = rolandSysEx(model,16,0x11,address+QByteArray::fromHex("000001"));
    CHECK(saveSwitcherControlSettings({}));
    SwitcherController controller(nullptr); auto settings = controller.settings();
    settings.rolandV8HdEnabled = true; settings.rolandModel = model;
    RolandModelSettings active; active.sourceNumber = 2; active.takeProgramWhenOnAir = true;
    setActiveRolandSettings(settings,active); controller.applySettings(settings);
    CHECK(until([&] { return device.bytes().contains(query); }));
    CHECK(!controller.switcherReady());
    CHECK(device.send(rolandSysEx(model,16,0x12,address+QByteArray(1,char(0)))));
    CHECK(until([&] { return controller.switcherReady(); }));
    CHECK(controller.startCheck()); device.clear();
    CHECK(until([&] { return device.bytes().contains(query); }));
    CHECK(!controller.switcherReady());
    CHECK(device.send(rolandSysEx(model,16,0x12,address+QByteArray(1,char(0)))));
    CHECK(until([&] { return controller.switcherReady(); }));
    CHECK(QMetaObject::invokeMethod(&controller,"handleProgramOnAirChanged",Qt::DirectConnection,Q_ARG(bool,true)));
    CHECK(until([&] { return device.bytes().contains(rolandSysEx(model,16,0x12,address+QByteArray(1,char(1)))); }));
    pump(40);
    const auto report = controller.checkSession().report();
    CHECK(report["model"]=="vr4hd" && QJsonDocument(report).toJson().contains("TX"));
    controller.requestShutdown(); CHECK(until([&] { return controller.isShutdownComplete(); }));
    CHECK(!macMidi::sourceReserved(device.source));
  }
  RolandV8HdMidiSwitcher excluded(RolandSwitcherModel::V1Hd,{},true);
  QString error; CHECK(!excluded.open(&error) && error.contains("unavailable"));
  for (auto model : {RolandSwitcherModel::V1Hd,RolandSwitcherModel::V1HdPlus}) {
    SwitcherController controller(nullptr); auto excludedSettings = controller.settings();
    excludedSettings.rolandModel = model; excludedSettings.rolandV8HdEnabled = true;
    controller.applySettings(excludedSettings);
    CHECK(!controller.switcherReady() && controller.switcherConnectionDetail().contains("unavailable"));
    CHECK(!controller.startCheck()); controller.requestShutdown(); CHECK(controller.isShutdownComplete());
  }
  for(auto model:{RolandSwitcherModel::V160Hd,RolandSwitcherModel::V80Hd,RolandSwitcherModel::V1_4K,RolandSwitcherModel::Vr6Hd,RolandSwitcherModel::Vr120Hd}) {
    const auto &p=rolandSwitcherProfile(model);
    VirtualSwitcher device(QStringLiteral("VTR-PON Test ")+QLatin1String(p.name)); CHECK(device.source && device.destination);
    RolandModelSettings settings; settings.useUsbMidi=true; settings.sourceNumber=p.inputs;
    RolandV8HdMidiSwitcher adapter(model,settings,true); int ready=0,errors=0; QList<int> changes;
    QObject::connect(&adapter,&SwitcherDevice::connectionOpened,&app,[&](bool value,const QString &) { if(value) ++ready; });
    QObject::connect(&adapter,&SwitcherDevice::errorOccurred,&app,[&](const QString &) { ++errors; });
    QObject::connect(&adapter,&SwitcherDevice::programInputChanged,&app,[&](int input) { changes.append(input); });
    const auto query=rolandSysEx(model,16,0x11,QByteArray::fromHex(p.programAddress)+QByteArray::fromHex("000001"));
    const auto response=[&](int selected) { return rolandSysEx(model,16,0x12,QByteArray::fromHex(p.programAddress)+char(selected-1)); };
    QString error; CHECK(adapter.open(&error)); CHECK(until([&] { return device.bytes().contains(query); }));
    CHECK(ready==0 && device.send(response(1))); CHECK(until([&] { return ready==1; })); CHECK(changes.isEmpty());
    int queries=device.bytes().count(query);
    CHECK(until([&] { return device.bytes().count(query)>queries; }));
    CHECK(device.send(response(p.inputs))); CHECK(until([&] { return changes==QList<int>{p.inputs}; }));
    CHECK(adapter.setProgramInput(p.inputs,&error));
    CHECK(until([&] { return device.bytes().contains(rolandSysEx(model,16,0x12,QByteArray::fromHex(p.programAddress)+char(p.inputs-1))); }));
    CHECK(!adapter.setFadeToBlack(true,&error));
    CHECK(device.send(response(p.inputs))); pump(50); CHECK(changes.size()==1);
    device.disconnect(); CHECK(until([&] { return errors>0; })); device.clear(); CHECK(device.create());
    CHECK(until([&] { return device.bytes().contains(query); })); CHECK(device.send(response(p.inputs)));
    CHECK(until([&] { return ready==2; })); CHECK(changes.size()==1);
    adapter.close(); CHECK(!macMidi::sourceReserved(device.source));
    std::printf("PASS CoreMIDI %s optional USB: PGM handshake, physical input, send, FTB refusal, reconnect baseline\n",p.name);
  }
  std::puts("MAC_ROLAND_PROFILES result=PASS");
  return 0;
}
