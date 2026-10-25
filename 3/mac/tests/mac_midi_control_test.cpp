#include "mac_midi_source.h"
#include "midi_control_input.h"
#include "midi_control_settings_page.h"
#include "midi_device_discovery.h"
#include "controller_operations.h"
#include "controller_settings.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QProcess>
#include <QSocketNotifier>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL mac-midi line %d: %s\n", __LINE__, #value); return 1; } } while (false)
namespace {
void pump(int milliseconds) {
  QElapsedTimer timer; timer.start();
  while (timer.elapsed() < milliseconds) { QCoreApplication::processEvents(); QThread::msleep(1); }
}
template<class F> bool until(F predicate, int timeout = 5500) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < timeout) pump(5);
  return predicate();
}
int sourceMode(QApplication &app) {
  MacMidiSource source(QStringLiteral("VTR-PON Mac MIDI Check"));
  CHECK(source.endpoint);
  std::cout << QJsonDocument(QJsonObject{{"id", source.id()}, {"name", source.name}}).toJson(QJsonDocument::Compact).constData() << std::endl;
  QSocketNotifier input(0, QSocketNotifier::Read);
  QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
    std::string line;
    if (!std::getline(std::cin, line)) { app.quit(); return; }
    std::istringstream command(line); std::string type; unsigned channel = 1, number = 60, value = 127;
    command >> type >> channel >> number >> value;
    bool sent = false;
    if ((type == "note" || type == "cc") && channel >= 1 && channel <= 16 && number <= 127 && value <= 127)
      sent = source.voice((type == "note" ? 0x90 : 0xb0) | (channel - 1), number, value);
    else if (type == "disconnect") { source.disconnect(); sent = true; }
    else if (type == "reconnect") sent = source.create();
    else if (type == "quit") app.quit();
    std::cout << "SOURCE command=" << type << " ok=" << sent << " id=" << source.id().toStdString()
              << " endpoint=" << source.endpoint << std::endl;
  });
  return app.exec();
}
int externalSourceTest(QApplication &app) {
  namespace m = vtrpon::midiControl;
  QTemporaryDir directory; CHECK(directory.isValid());
  app.setProperty("vtrpon.settingsFile", directory.filePath("user.ini"));
  QProcess source;
  source.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--source")});
  struct Cleanup {
    QProcess &source;
    ~Cleanup() {
      source.write("quit\n");
      if (!source.waitForFinished(1000)) { source.kill(); source.waitForFinished(3000); }
    }
  } cleanup{source};
  QByteArray log;
  CHECK(until([&] { log += source.readAllStandardOutput(); return log.contains('\n'); }));
  const auto id = QJsonDocument::fromJson(log.split('\n').first()).object().value("id").toString();
  CHECK(id.startsWith(QStringLiteral("coremidi:")));
  auto settings = vtrpon::makeControllerSettings();
  settings.setValue(m::settingsKey(), QJsonObject{{"enabled", true}, {"device", id},
      {"bindings", QJsonObject{{"TakeOnAir", QJsonObject{{"type", "note"}, {"channel", 1}, {"number", 60}}},
                               {"SetMasterVolume", QJsonObject{{"type", "cc"}, {"channel", 1}, {"number", 7}}}}}}.toVariantMap());
  settings.sync();
  ControllerOperations operations;
  unsigned presses = 0;
  int volume = 0;
  operations.bind<int>(ControllerOperations::Action::SetMasterVolume, &app, [&](int value) { volume = value; });
  // This is the parent's first CoreMIDI use. Enumeration starts on the normal
  // discovery worker, exactly like an app restored from a saved MIDI setting.
  vtrpon::MidiControlInput input(&operations, [&](const QString &) { ++presses; });
  input.setVolumeValueProvider([&](const QString &) { return volume; });
  CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.connected"); }));
  const auto command = [&](const char *text) { source.write(text); source.waitForBytesWritten(1000); };
  command("note 1 60 127\n"); CHECK(until([&] { return presses == 1; }));
  command("note 1 60 0\n"); pump(50);
  for (unsigned cycle = 0; cycle < 2; ++cycle) {
    command("disconnect\n");
    CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.disconnected"); }));
    command("reconnect\n");
    const bool reconnected = until([&] { return input.statusKey() == QStringLiteral("midi.connected"); });
    if (!reconnected) {
      std::cerr << source.readAllStandardOutput().constData() << source.readAllStandardError().constData();
      for (const auto &device : vtrpon::MidiControlInput::devices())
        std::cerr << "Observed UID=" << device.id.toStdString() << " endpoint=" << device.index << std::endl;
    }
    CHECK(reconnected);
    command("note 1 60 127\n"); CHECK(until([&] { return presses == cycle + 2; }));
    command("note 1 60 0\n"); pump(50);
  }
  command("cc 1 7 64\n"); CHECK(until([&] { return volume < 0; }));
  input.stop();
  std::puts("PASS external-process source: background discovery, receive, two same-UID reconnects, saved CC mapping");
  std::puts("MAC_MIDI_EXTERNAL result=PASS");
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (app.arguments().contains(QStringLiteral("--source"))) return sourceMode(app);
  if (app.arguments().contains(QStringLiteral("--external"))) return externalSourceTest(app);
  namespace m = vtrpon::midiControl;
  QTemporaryDir directory; CHECK(directory.isValid());
  app.setProperty("vtrpon.settingsFile", directory.filePath("user.ini"));
  auto settings = vtrpon::makeControllerSettings();
  MacMidiSource first(QStringLiteral("VTR-PON CoreMIDI integration"));
  MacMidiSource second(first.name); CHECK(first.endpoint && second.endpoint && first.uid != second.uid);
  auto devices = vtrpon::MidiControlInput::devices();
  const auto found = [&](const auto &list, const QString &id) {
    return std::find_if(list.begin(), list.end(), [&](const auto &device) { return device.id == id; }) != list.end();
  };
  CHECK(found(devices, first.id()) && found(devices, second.id()));
  std::puts("PASS CoreMIDI lists distinct stable UIDs for identically named sources");

  ControllerOperations operations;
  unsigned actions = 0, received = 0, volumeCalls = 0;
  int volume = 0;
  bool correctThread = true;
  operations.bind(ControllerOperations::Action::TakeSelectedOnAir, &app, [&] { ++actions; });
  operations.bind<int>(ControllerOperations::Action::SetMasterVolume, &app, [&](int value) { volume = value; ++volumeCalls; });
  vtrpon::MidiControlInput input(&operations, [&](const QString &action) {
    if (action == QStringLiteral("TakeOnAir")) operations.executeFrom("MIDI", ControllerOperations::Action::TakeSelectedOnAir);
  });
  input.setVolumeValueProvider([&](const QString &) { return volume; });
  QObject::connect(&input, &vtrpon::MidiControlInput::received, &app, [&](const QString &, int, int, int, bool) {
    ++received; correctThread &= QThread::currentThread() == app.thread();
  });
  input.setConfigurationMode(true);
  QDialog window;
  window.setAttribute(Qt::WA_DontShowOnScreen);
  auto *layout = new QVBoxLayout(&window);
  auto *page = new vtrpon::MidiControlSettingsPage(&window); layout->addWidget(page); page->setInput(&input);
  QObject::connect(page, &vtrpon::MidiControlSettingsPage::saved, &input, &vtrpon::MidiControlInput::reload);
  auto *combo = page->findChild<QComboBox *>("midiInputDevice");
  auto *table = page->findChild<QTableWidget *>("midiBindingsTable");
  auto *learn = page->findChild<QPushButton *>("midiLearnButton");
  auto *save = page->findChild<QPushButton *>("midiSaveButton");
  auto *enabled = page->findChild<QCheckBox *>("midiEnabled");
  CHECK(combo && table && learn && save && enabled);
  window.resize(900, 900); window.show();
  CHECK(until([&] { return combo->findData(first.id()) >= 0 && combo->findData(second.id()) >= 0; }));
  combo->setCurrentIndex(combo->findData(first.id()));
  CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.connected"); }));
  table->setCurrentCell(m::actionIds().indexOf(QStringLiteral("TakeOnAir")), 0); learn->click();
  CHECK(first.voice(0x90, 60, 0)); pump(100); CHECK(learn->isChecked());
  CHECK(first.voice(0x90, 60, 127)); CHECK(until([&] { return !learn->isChecked(); }));
  CHECK(first.voice(0x80, 60, 64)); pump(50); CHECK(actions == 0);
  table->setCurrentCell(m::actionIds().indexOf(QStringLiteral("SetMasterVolume")), 0); learn->click();
  CHECK(first.voice(0xb1, 7, 102)); CHECK(until([&] { return !learn->isChecked(); }));
  CHECK(volumeCalls == 0 && correctThread);
  enabled->setChecked(true); save->click(); settings.sync();
  const auto saved = m::load(settings);
  CHECK(saved.value("enabled").toBool() && saved.value("device").toString() == first.id());
  CHECK(saved.value("bindings").toObject().value("TakeOnAir").toObject().value("number").toInt() == 60);
  CHECK(saved.value("bindings").toObject().value("SetMasterVolume").toObject().value("channel").toInt() == 2);
  window.hide(); input.setConfigurationMode(false);
  CHECK(input.statusKey() == QStringLiteral("midi.connected"));
  std::puts("PASS real CoreMIDI Note/CC Learn, save, configuration suppression and Controller-thread delivery");

  CHECK(first.voice(0x90, 60, 127)); CHECK(until([&] { return actions == 1; }));
  CHECK(first.voice(0x90, 60, 127)); pump(50); CHECK(actions == 1);
  CHECK(first.voice(0x90, 60, 0)); pump(50);
  CHECK(first.voice(0x90, 60, 80)); CHECK(until([&] { return actions == 2; }));
  CHECK(first.voice(0x80, 60, 127)); pump(50);
  const auto beforeOther = received;
  CHECK(second.voice(0x90, 60, 127)); pump(100); CHECK(received == beforeOther && actions == 2);
  for (const auto [raw, expected] : {std::pair{0U, -960}, {102U, 0}, {127U, 60}}) {
    CHECK(first.voice(0xb1, 7, raw)); CHECK(until([&] { return volume == expected && volumeCalls > 0; }));
  }
  std::puts("PASS saved Note actions and full-range CC volume execute; unselected source is ignored");

  input.setConfigurationMode(true);
  CHECK(first.voice(0x90, 60, 127)); pump(100); CHECK(actions == 2);
  input.setConfigurationMode(false);
  CHECK(first.voice(0x90, 60, 127)); pump(100); CHECK(actions == 2);
  CHECK(first.voice(0x80, 60, 0)); pump(50);
  CHECK(first.voice(0x90, 60, 127)); CHECK(until([&] { return actions == 3; }));
  CHECK(first.voice(0x80, 60, 0)); pump(50);
  QDialog modal;
  modal.setAttribute(Qt::WA_DontShowOnScreen); modal.setWindowModality(Qt::ApplicationModal); modal.show(); pump(50);
  CHECK(QApplication::activeModalWidget() == &modal);
  CHECK(first.voice(0x90, 60, 127)); pump(100); CHECK(actions == 3);
  modal.hide(); CHECK(first.voice(0x80, 60, 0)); pump(50);
  std::puts("PASS Settings-held buttons and modal dialogs cannot dispatch actions");

  // SysEx payload contains a fake voice word, which must not be interpreted.
  const auto beforeUnsupported = received;
  CHECK(first.send({0x30000000U, 0x20903c7fU, 0x10f80000U, 0x20e00040U})); pump(100);
  CHECK(received == beforeUnsupported && actions == 3);
  std::atomic<bool> sent{false}, sendOk{true};
  std::thread producer([&] {
    for (unsigned batch = 0; batch < 320; ++batch) {
      std::vector<UInt32> words;
      for (unsigned i = 0; i < 64; ++i) words.push_back(0x20b10700U | ((batch * 64 + i) % 128));
      if (!first.send(words)) sendOk = false;
    }
    first.voice(0x90, 60, 127); first.voice(0x80, 60, 0);
    sent = true;
  });
  const bool responsive = until([&] { return sent.load() && actions == 4; });
  producer.join(); CHECK(responsive && sendOk && correctThread);
  std::puts("PASS unsupported UMP messages ignored; 20,480 CC messages preserve button dispatch");

  input.setConfigurationMode(true); input.monitorDevice(second.id());
  CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.connected"); }));
  pump(100);
  const auto beforeSelection = received;
  CHECK(first.voice(0x90, 60, 127)); pump(100); CHECK(received == beforeSelection);
  CHECK(second.voice(0x90, 61, 127)); CHECK(until([&] { return received > beforeSelection; }));
  input.setConfigurationMode(false);
  CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.connected"); }));
  pump(100);
  first.disconnect();
  CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.disconnected"); }));
  settings.sync(); CHECK(m::load(settings) == saved);
  const auto beforeMissing = received;
  CHECK(second.voice(0x90, 60, 127)); pump(100); CHECK(received == beforeMissing);
  CHECK(first.create()); CHECK(first.id() == saved.value("device").toString());
  CHECK(until([&] { return input.statusKey() == QStringLiteral("midi.connected"); }));
  CHECK(first.voice(0x90, 60, 127)); CHECK(until([&] { return actions == 5; }));
  CHECK(first.voice(0x80, 60, 0)); pump(50);
  std::puts("PASS source selection, disconnect without fallback, saved UID reconnect with a new endpoint");

  input.stop(); const auto stoppedCount = received;
  CHECK(first.voice(0x90, 60, 127)); pump(100); CHECK(received == stoppedCount);
  // Recreate the receiver from the saved INI, like a fresh app session.
  vtrpon::MidiControlInput reopened(&operations, [&](const QString &) { ++actions; });
  CHECK(until([&] { return reopened.statusKey() == QStringLiteral("midi.connected"); }));
  CHECK(first.voice(0x90, 60, 127)); CHECK(until([&] { return actions == 6; }));
  reopened.stop();
  std::atomic<bool> finish{false};
  std::thread duringClose([&] {
    while (!finish.load()) { first.voice(0xb0, 7, 50); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  });
  bool closeCycles = true;
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    auto closing = std::make_unique<vtrpon::MidiControlInput>(nullptr, std::function<void(const QString &)>{});
    if (!until([&] { return closing->statusKey() == QStringLiteral("midi.connected"); })) { closeCycles = false; break; }
    closing.reset();
  }
  finish = true; duringClose.join(); CHECK(closeCycles);
  std::puts("PASS saved settings reopen, stop suppresses late input, receive during receiver destruction");
  std::puts("MAC_MIDI result=PASS");
  return 0;
}
