#include "midi_control_input.h"
#include "midi_control_settings_page.h"
#include "midi_message_queue.h"
#include "controller_operations.h"
#include "controller_settings.h"
#include "controller_settings_io.h"
#include "controller_scaling.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTranslator>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "MIDI check failed at line %d: %s\n", __LINE__, #value); return 1; } } while (false)
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  namespace m = vtrpon::midiControl;
  const bool probe = app.arguments().contains("--probe-config");
  const bool disablePickup = app.arguments().contains("--disable-volume-pickup");
  const int inspectArgument = app.arguments().indexOf(disablePickup ? "--disable-volume-pickup" : probe ? "--probe-config" : "--inspect-config");
  if (inspectArgument >= 0) {
    CHECK(inspectArgument + 1 < app.arguments().size());
    const auto path = app.arguments().at(inspectArgument + 1);
    CHECK(QFileInfo::exists(path));
    QSettings saved(path, QSettings::IniFormat);
    const auto raw = QJsonObject::fromVariantMap(saved.value(m::settingsKey()).toMap());
    const auto normalized = m::normalized(raw);
    const auto error = m::validationError(normalized);
    if (disablePickup) {
      CHECK(error.isEmpty());
      auto updated = normalized;
      auto bindings = updated.value("bindings").toObject();
      QStringList changed;
      for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        if (!m::isVolume(it.key())) continue;
        auto binding = it.value().toObject(); binding.insert("pickup", false); it.value() = binding;
        changed.append(it.key());
      }
      CHECK(!changed.isEmpty());
      updated.insert("bindings", bindings);
      saved.setValue(m::settingsKey(), updated.toVariantMap()); saved.sync();
      CHECK(saved.status() == QSettings::NoError);
      std::printf("Direct volume control applied: %s\n", changed.join(", ").toUtf8().constData());
      return 0;
    }
    if (probe) {
      CHECK(error.isEmpty() && normalized.value("enabled").toBool());
      app.setProperty("vtrpon.settingsFile", path);
      ControllerOperations operations;
      vtrpon::MidiControlInput input(&operations, {});
      CHECK(input.statusKey() == "midi.connected");
      QElapsedTimer cycles; cycles.start();
      for (int pass = 0; pass < 3; ++pass) {
        input.setConfigurationMode(true);
        input.monitorDevice(normalized.value("device").toString());
        CHECK(input.statusKey() == "midi.connected");
        input.setConfigurationMode(false);
        CHECK(input.statusKey() == "midi.connected");
      }
      input.stop();
      std::printf("Saved MIDI input stays connected through three Settings cycles (%lld ms): PASS\n", static_cast<long long>(cycles.elapsed()));
      return 0;
    }
    std::printf("%s\n", QJsonDocument(QJsonObject{{"saved", raw}, {"normalized", normalized}, {"error", error}}).toJson().constData());
    return error.isEmpty() ? 0 : 1;
  }
  if (app.arguments().contains("--list-devices")) {
    const auto devices = vtrpon::MidiControlInput::devices();
    for (const auto &device : devices) std::printf("%s\n", device.name.toUtf8().constData());
    std::printf("MIDI input count: %lld\n", static_cast<long long>(devices.size())); return 0;
  }
  QTemporaryDir directory; CHECK(directory.isValid());
  app.setProperty("vtrpon.settingsFile", directory.filePath("settings.ini"));
  auto settings = vtrpon::makeControllerSettings();
  if (app.arguments().contains("--monitor")) {
    const auto devices = vtrpon::MidiControlInput::devices();
    CHECK(!devices.isEmpty());
    ControllerOperations operations;
    vtrpon::MidiControlInput input(&operations, {});
    input.setConfigurationMode(true); input.monitorDevice(devices.first().id);
    CHECK(input.statusKey() == "midi.connected");
    int received = 0;
    QObject::connect(&input, &vtrpon::MidiControlInput::received, &app,
        [&](const QString &type, int channel, int number, int value, bool pressed) {
          ++received;
          std::printf("%s ch=%d number=%d value=%d pressed=%d\n", type.toUtf8().constData(), channel, number, value, pressed);
          std::fflush(stdout);
        });
    std::printf("Monitoring %s for 20 seconds.\n", devices.first().name.toUtf8().constData()); std::fflush(stdout);
    QTimer::singleShot(20000, &app, &QCoreApplication::quit); app.exec(); input.stop();
    std::printf("Received messages: %d\n", received); return received > 0 ? 0 : 1;
  }
  const QJsonObject note{{"type", "note"}, {"channel", 1}, {"number", 60}};
  m::MessageQueue queue;
  m::MessageQueue::Modes queueModes{};
  queueModes[8] = m::MessageQueue::CcMode::Relative;
  queueModes[9] = m::MessageQueue::CcMode::Button;
  queue.setModes(queueModes);
  for (int packet = 0; packet < 20000; ++packet) queue.push(0xb0 | (7 << 8) | (quint32(packet % 128) << 16));
  queue.push(0x7f3c90); queue.push(0x003c80);
  queue.push(0x7f09b0); queue.push(0x7f09b0); queue.push(0x0009b0);
  for (int step = 0; step < 5; ++step) queue.push(0x0108b0);
  const auto burst = queue.take();
  CHECK(burst.noteCount == 2 && burst.buttonCount == 2 && burst.relativeCount == 5 && burst.absoluteCount == 1);
  CHECK(!burst.noteOverflow && !burst.buttonOverflow && !burst.relativeOverflow);
  CHECK(burst.absolute[0].minimum == 0 && burst.absolute[0].maximum == 127);
  CHECK(m::decode(burst.absolute[0].packed).value == 19999 % 128);
  for (int packet = 0; packet < 2000; ++packet) queue.push(0x0108b0);
  queue.push(0x7f3c90); queue.push(0x003c80);
  const auto overflow = queue.take();
  CHECK(overflow.relativeOverflow && overflow.relativeCount == 0 && overflow.noteCount == 2 && !overflow.noteOverflow);
  QJsonObject cc{{"type", "cc"}, {"channel", 2}, {"number", 7}};
  auto config = m::defaults(); config.insert("bindings", QJsonObject{{"TakeOnAir", note}, {"SetMasterVolume", cc}});
  CHECK(m::validationError(config).isEmpty());
  CHECK(!m::actionIds().contains("ManualTransition"));
  auto previousRange = cc; previousRange.insert("minimum", -600); previousRange.insert("maximum", 0);
  auto previousConfig = m::defaults(); previousConfig.insert("enabled", true); previousConfig.insert("device", "saved-device");
  previousConfig.insert("bindings", QJsonObject{{"TakeOnAir", note}, {"SetMasterVolume", previousRange}, {"ManualTransition", QJsonObject{{"type", "cc"}, {"channel", 1}, {"number", 9}}}});
  settings.setValue(m::settingsKey(), previousConfig.toVariantMap());
  const auto migrated = m::load(settings);
  CHECK(migrated.value("enabled").toBool() && migrated.value("device").toString() == "saved-device");
  CHECK(migrated.value("bindings").toObject().value("TakeOnAir") == note);
  CHECK(!migrated.value("bindings").toObject().contains("ManualTransition"));
  CHECK(migrated.value("bindings").toObject().value("SetMasterVolume").toObject().value("minimum").toInt() == -960);
  CHECK(migrated.value("bindings").toObject().value("SetMasterVolume").toObject().value("maximum").toInt() == 60);
  CHECK(vtrpon::controllerSettingsSnapshot(settings).value(m::settingsKey()).toObject() == migrated);
  CHECK(vtrpon::replaceControllerSettings(settings, QJsonObject{{m::settingsKey(), previousConfig}}));
  CHECK(m::load(settings) == migrated);
  auto limitedRange = previousRange; limitedRange.insert("scale", "fader");
  auto limitedConfig = m::defaults(); limitedConfig.insert("bindings", QJsonObject{{"SetMasterVolume", limitedRange}});
  CHECK(m::normalized(limitedConfig) == limitedConfig);
  auto badScale = limitedRange; badScale.insert("scale", "invalid");
  auto badScaleConfig = m::defaults(); badScaleConfig.insert("bindings", QJsonObject{{"SetMasterVolume", badScale}});
  CHECK(!m::validationError(m::normalized(badScaleConfig)).isEmpty());
  CHECK(vtrpon::replaceControllerSettings(settings, {}));
  config.insert("bindings", QJsonObject{{"TakeOnAir", note}, {"OffAir", note}});
  CHECK(!m::validationError(config).isEmpty());
  config.insert("bindings", QJsonObject{{"SetItemVolume", note}}); CHECK(!m::validationError(config).isEmpty());
  auto bad = cc; bad.insert("channel", 1.5); config.insert("bindings", QJsonObject{{"SetItemVolume", bad}}); CHECK(!m::validationError(config).isEmpty());
  bad = cc; bad.insert("number", 128); config.insert("bindings", QJsonObject{{"SetItemVolume", bad}}); CHECK(!m::validationError(config).isEmpty());
  CHECK(m::decode(0x7f3c90).pressed); CHECK(!m::decode(0x003c90).pressed); CHECK(!m::decode(0x403c80).pressed);
  CHECK(m::decode(0x4007bf).channel == 16); CHECK(m::decode(0x80f890).type.isEmpty()); CHECK(m::decode(0xf8).type.isEmpty());
  m::Router router;
  const QJsonObject bindings{{"TakeOnAir", note}, {"SetMasterVolume", cc}};
  CHECK(router.process(m::decode(0x7f3c90), bindings, true) == "TakeOnAir");
  CHECK(router.process(m::decode(0x7f3c90), bindings, true).isEmpty());
  CHECK(router.process(m::decode(0x003c90), bindings, true).isEmpty());
  CHECK(router.process(m::decode(0x403c90), bindings, false).isEmpty());
  CHECK(router.process(m::decode(0x403c90), bindings, true).isEmpty());
  CHECK(router.process(m::decode(0x003c80), bindings, true).isEmpty());
  CHECK(router.process(m::decode(0x403c90), bindings, true) == "TakeOnAir");
  CHECK(router.process(m::decode(0x4007b1), bindings, true) == "SetMasterVolume");
  auto repeatedSpan = m::decode(0x4007b1); repeatedSpan.minimumValue = 0; repeatedSpan.maximumValue = 127;
  CHECK(router.process(repeatedSpan, bindings, true) == "SetMasterVolume");
  CHECK(router.process(m::decode(0x4007b0), bindings, true).isEmpty());
  const QJsonObject ccButton{{"ToggleFtb", QJsonObject{{"type", "cc"}, {"channel", 1}, {"number", 8}}}};
  CHECK(router.process(m::decode(0x7f08b0), ccButton, true) == "ToggleFtb");
  CHECK(router.process(m::decode(0x6408b0), ccButton, true).isEmpty());
  CHECK(router.process(m::decode(0x0008b0), ccButton, true).isEmpty());
  CHECK(router.process(m::decode(0x4008b0), ccButton, true) == "ToggleFtb");
  m::VolumeMapper volume;
  CHECK(volume.map("direct", 39, cc, 0).has_value());
  cc.insert("pickup", true);
  CHECK(!volume.map("master", 10, cc, 0));
  CHECK(volume.map("master", 102, cc, 0) == 0);
  CHECK(volume.map("master", 127, cc, 0) == 60);
  CHECK(volume.map("master", 64, cc, 60) == -163);
  CHECK(!volume.map("master", 60, cc, -100)); // mouse/automation changed the software value
  CHECK(volume.map("master", 110, cc, -100).has_value());
  cc.insert("pickup", false);
  CHECK(volume.map("master", 0, cc, -100) == -960);
  cc.insert("reverse", true); CHECK(volume.map("master", 0, cc, -100) == 60);
  cc.insert("reverse", false);
  auto pickupBinding = cc; pickupBinding.insert("pickup", true);
  m::VolumeMapper spanVolume;
  CHECK(spanVolume.map("forward", 127, pickupBinding, 0, 0, 127) == 60);
  pickupBinding.insert("reverse", true);
  CHECK(spanVolume.map("reverse", 17, pickupBinding, 0, 10, 27).has_value());
  for (const auto &id : m::actionIds()) {
    if (!m::isVolume(id)) continue;
    CHECK(volume.map(id, 0, cc, 0) == -960);
    CHECK(volume.map(id, 102, cc, -960) == 0);
    CHECK(volume.map(id, 127, cc, 0) == 60);
    int previous = -960;
    for (int raw = 0; raw <= 127; ++raw) {
      const auto mapped = volume.map(id, raw, cc, previous);
      CHECK(mapped && *mapped >= previous && *mapped <= 60);
      previous = *mapped;
    }
  }
  CHECK(m::relativeDelta(1, "twos-complement") == 1 && m::relativeDelta(127, "twos-complement") == -1);
  CHECK(m::relativeDelta(64, "twos-complement") == -64);
  CHECK(m::relativeDelta(65, "binary-offset") == 1 && m::relativeDelta(63, "binary-offset") == -1);
  CHECK(m::relativeDelta(1, "signed-bit") == 1 && m::relativeDelta(65, "signed-bit") == -1);
  CHECK(m::relativeDelta(65, "signed-bit-2") == 1 && m::relativeDelta(1, "signed-bit-2") == -1);
  cc.insert("mode", "twos-complement"); cc.insert("reverse", false);
  CHECK(volume.map("master", 1, cc, -200) == -190); CHECK(volume.map("master", 127, cc, -200) == -210);
  CHECK(volume.map("master", 1, cc, 0) == 10);
  CHECK(volume.map("master", 1, cc, 60) == 60);
  CHECK(volume.map("master", 127, cc, -960) == -960);
  CHECK(volume.map("master", 1, cc, -960) == -950);
  CHECK(volume.map("master", 127, cc, -950) == -960);
  const QJsonObject relativeBindings{{"SetMasterVolume", cc}};
  CHECK(router.process(m::decode(0x0107b1), relativeBindings, true) == "SetMasterVolume");
  CHECK(router.process(m::decode(0x0107b1), relativeBindings, true) == "SetMasterVolume");
  QTranslator translator;
  if (app.arguments().size() > 1) { CHECK(translator.load(app.arguments().at(1))); app.installTranslator(&translator); }
  vtrpon::setUiScale(1.0);
  QDialog window; window.setObjectName("controllerSettingsDialog");
  QFile style("G:/vtr-pon-dev/apps/desktop/settings.qss"); CHECK(style.open(QIODevice::ReadOnly)); window.setStyleSheet(vtrpon::scaledStyleSheet(QString::fromUtf8(style.readAll())));
  auto *layout = new QVBoxLayout(&window); auto *page = new vtrpon::MidiControlSettingsPage(&window); layout->addWidget(page);
  ControllerOperations operations;
  vtrpon::MidiControlInput input(&operations, {}); input.setConfigurationMode(true); page->setInput(&input);
  auto *devices = page->findChild<QComboBox *>("midiInputDevice"); CHECK(devices);
  devices->addItem("Test MIDI input (not connected)", "test-input"); devices->setCurrentIndex(devices->count() - 1);
  auto *table = page->findChild<QTableWidget *>("midiBindingsTable"); auto *learn = page->findChild<QPushButton *>("midiLearnButton");
  auto *save = page->findChild<QPushButton *>("midiSaveButton"); CHECK(table && learn && save);
  window.setAttribute(Qt::WA_DontShowOnScreen);
  window.resize(900, 900); window.show(); app.processEvents();
  const int masterRow = m::actionIds().indexOf("SetMasterVolume");
  table->scrollToItem(table->item(masterRow, 0), QAbstractItemView::PositionAtCenter);
  app.processEvents();
  const auto viewportBefore = table->viewport()->geometry();
  const auto masterRectBefore = table->visualItemRect(table->item(masterRow, 0));
  const int scrollBefore = table->verticalScrollBar()->value();
  table->setCurrentCell(masterRow, 0); app.processEvents();
  CHECK(table->viewport()->geometry() == viewportBefore);
  CHECK(table->visualItemRect(table->item(masterRow, 0)) == masterRectBefore);
  CHECK(table->verticalScrollBar()->value() == scrollBefore);
  CHECK(!page->findChild<QCheckBox *>("midiPickup")->isChecked());
  table->setCurrentCell(m::actionIds().indexOf("TakeOnAir"), 0);
  learn->click();
  for (int value = 0; value < 128; ++value) input.received("cc", 1, 7, value, false);
  CHECK(learn->isChecked());
  input.received("note", 1, 60, 127, true); CHECK(!learn->isChecked());
  save->click(); settings.sync();
  CHECK(m::load(settings).value("bindings").toObject().value("TakeOnAir").toObject() == note);
  table->setCurrentCell(m::actionIds().indexOf("SetMasterVolume"), 0); CHECK(learn->isEnabled());
  learn->click(); input.received("cc", 2, 7, 90, false); CHECK(!learn->isChecked());
  save->click(); settings.sync(); auto stored = m::load(settings);
  CHECK(stored.value("bindings").toObject().value("SetMasterVolume").toObject().value("number").toInt() == 7);
  CHECK(stored.value("bindings").toObject().value("SetMasterVolume").toObject().value("minimum").toInt() == -960);
  CHECK(stored.value("bindings").toObject().value("SetMasterVolume").toObject().value("maximum").toInt() == 60);
  table->setCurrentCell(m::actionIds().indexOf("SetItemVolume"), 0);
  learn->click(); input.received("cc", 2, 7, 90, false); CHECK(!learn->isChecked());
  save->click(); settings.sync();
  auto moved = m::load(settings).value("bindings").toObject();
  CHECK(!moved.contains("SetMasterVolume") && moved.contains("SetItemVolume"));
  CHECK(table->item(m::actionIds().indexOf("SetMasterVolume"), 2)->text() == vtrpon::uiText("midi.unassigned"));
  table->setCurrentCell(m::actionIds().indexOf("SetMasterVolume"), 0);
  learn->click(); input.received("cc", 2, 7, 90, false);
  save->click(); settings.sync(); stored = m::load(settings);
  CHECK(stored.value("bindings").toObject().contains("SetMasterVolume"));
  CHECK(!stored.value("bindings").toObject().contains("SetItemVolume"));
  CHECK(!stored.value("bindings").toObject().value("SetMasterVolume").toObject().value("pickup").toBool());
  const auto snapshot = vtrpon::controllerSettingsSnapshot(settings); CHECK(snapshot.value(m::settingsKey()).toObject() == stored);
  config = stored; config.insert("enabled", true); CHECK(vtrpon::replaceControllerSettings(settings, QJsonObject{{m::settingsKey(), config}}));
  CHECK(m::load(settings).value("enabled").toBool());
  auto invalid = snapshot; invalid.insert(m::settingsKey(), QJsonObject{{"enabled", "yes"}});
  CHECK(!vtrpon::replaceControllerSettings(settings, invalid));
  CHECK(vtrpon::exportControllerSettings(settings, directory.filePath("export.json")));
  CHECK(vtrpon::replaceControllerSettings(settings, {})); CHECK(!m::load(settings).value("enabled").toBool());
  CHECK(vtrpon::importControllerSettings(settings, directory.filePath("export.json"))); CHECK(m::load(settings).value("enabled").toBool());
  if (app.arguments().size() > 2) {
    window.resize(900, 900); window.ensurePolished(); layout->activate(); app.processEvents();
    QImage image(window.size(), QImage::Format_ARGB32); image.fill(Qt::transparent); window.render(&image); CHECK(image.save(app.arguments().at(2)));
  }
  input.stop();
  std::puts("MIDI decode, edges, Learn, CC modes, Pickup, settings and UI passed.");
  return 0;
}
