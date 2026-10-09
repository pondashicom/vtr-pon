#include "midi_control_settings_page.h"
#include "midi_control_input.h"
#include "controller_settings.h"
#include "controller_settings_io.h"
#include "settings_style.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace vtrpon {
MidiControlSettingsPage::MidiControlSettingsPage(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("midiControlSettingsPage"));
  setSettingsRole(this, "page");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(scalePx(20), scalePx(20), scalePx(20), scalePx(20));
  layout->setSpacing(scalePx(10));
  title_ = new QLabel(this); setSettingsRole(title_, "pageTitle"); layout->addWidget(title_);
  description_ = new QLabel(this); description_->setWordWrap(true); setSettingsRole(description_, "description"); layout->addWidget(description_);
  enabled_ = new QCheckBox(this); enabled_->setObjectName(QStringLiteral("midiEnabled")); layout->addWidget(enabled_);
  auto *deviceRow = new QHBoxLayout;
  deviceLabel_ = new QLabel(this); deviceRow->addWidget(deviceLabel_);
  device_ = new QComboBox(this); device_->setObjectName(QStringLiteral("midiInputDevice")); deviceLabel_->setBuddy(device_); deviceRow->addWidget(device_, 1);
  refresh_ = new QPushButton(this); deviceRow->addWidget(refresh_); layout->addLayout(deviceRow);
  connection_ = new QLabel(this); connection_->setWordWrap(true); setSettingsRole(connection_, "status"); layout->addWidget(connection_);
  received_ = new QLabel(this); received_->setObjectName(QStringLiteral("midiLastReceived")); layout->addWidget(received_);
  receivedDisplayTimer_ = new QTimer(this);
  receivedDisplayTimer_->setSingleShot(true);
  receivedDisplayTimer_->setInterval(33);
  connect(receivedDisplayTimer_, &QTimer::timeout, this, [this] { received_->setText(lastReceivedText_); });
  table_ = new QTableWidget(ids_.size(), 3, this);
  table_->setObjectName(QStringLiteral("midiBindingsTable"));
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  for (int row = 0; row < ids_.size(); ++row) {
    for (int column = 0; column < 3; ++column) table_->setItem(row, column, new QTableWidgetItem);
    table_->setRowHeight(row, scalePx(32));
  }
  layout->addWidget(table_, 1);
  auto *assignment = new QHBoxLayout;
  type_ = new QComboBox(this); type_->setObjectName(QStringLiteral("midiMessageType"));
  type_->addItem(QString{}, ""); type_->addItem(QStringLiteral("Note"), "note"); type_->addItem(QStringLiteral("CC"), "cc");
  assignment->addWidget(type_);
  channel_ = new QSpinBox(this); channel_->setObjectName(QStringLiteral("midiChannel")); channel_->setRange(1, 16); assignment->addWidget(channel_);
  number_ = new QSpinBox(this); number_->setObjectName(QStringLiteral("midiNumber")); number_->setRange(0, 127); assignment->addWidget(number_);
  learn_ = new QPushButton(this); learn_->setObjectName(QStringLiteral("midiLearnButton")); learn_->setCheckable(true); assignment->addWidget(learn_);
  remove_ = new QPushButton(this); assignment->addWidget(remove_); layout->addLayout(assignment);
  auto *options = new QHBoxLayout;
  mode_ = new QComboBox(this); mode_->setObjectName(QStringLiteral("midiCcMode"));
  for (const auto *mode : {"absolute", "twos-complement", "binary-offset", "signed-bit", "signed-bit-2"}) mode_->addItem(QString{}, mode);
  options->addWidget(mode_, 1);
  pickup_ = new QCheckBox(this); pickup_->setObjectName(QStringLiteral("midiPickup")); options->addWidget(pickup_);
  reverse_ = new QCheckBox(this); options->addWidget(reverse_); layout->addLayout(options);
  auto *range = new QHBoxLayout;
  rangeLabel_ = new QLabel(this); range->addWidget(rangeLabel_);
  minimum_ = new QDoubleSpinBox(this); maximum_ = new QDoubleSpinBox(this);
  for (auto *spin : {minimum_, maximum_}) { spin->setRange(-96.0, 6.0); spin->setDecimals(1); spin->setSuffix(QStringLiteral(" dB")); range->addWidget(spin); }
  // Reserve the same table viewport for button and continuous-control rows.
  for (auto *widget : {static_cast<QWidget *>(mode_), static_cast<QWidget *>(pickup_), static_cast<QWidget *>(reverse_), static_cast<QWidget *>(minimum_), static_cast<QWidget *>(maximum_), static_cast<QWidget *>(rangeLabel_)}) {
    auto policy = widget->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    widget->setSizePolicy(policy);
  }
  range->addStretch(); layout->addLayout(range);
  status_ = new QLabel(this); status_->setWordWrap(true); setSettingsRole(status_, "status"); layout->addWidget(status_);
  status_->setObjectName(QStringLiteral("midiSettingsStatus"));
  status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  status_->setFixedHeight(status_->fontMetrics().lineSpacing() * 3);
  auto *buttons = new QHBoxLayout; buttons->addStretch();
  save_ = new QPushButton(this); save_->setObjectName(QStringLiteral("midiSaveButton")); setSettingsRole(save_, "primaryButton"); buttons->addWidget(save_); layout->addLayout(buttons);
  connect(refresh_, &QPushButton::clicked, this, &MidiControlSettingsPage::refreshDevices);
  connect(device_, &QComboBox::currentIndexChanged, this, [this] {
    learning_ = false; learn_->setChecked(false);
    if (input_) input_->monitorDevice(device_->currentData().toString());
    selectRow();
  });
  connect(table_, &QTableWidget::currentCellChanged, this, [this] { selectRow(); });
  connect(type_, &QComboBox::currentIndexChanged, this, [this] { editBinding(); });
  connect(channel_, &QSpinBox::valueChanged, this, [this] { editBinding(); });
  connect(number_, &QSpinBox::valueChanged, this, [this] { editBinding(); });
  connect(mode_, &QComboBox::currentIndexChanged, this, [this] { editBinding(); });
  connect(pickup_, &QCheckBox::toggled, this, [this] { editBinding(); });
  connect(reverse_, &QCheckBox::toggled, this, [this] { editBinding(); });
  connect(minimum_, &QDoubleSpinBox::valueChanged, this, [this] { editBinding(); });
  connect(maximum_, &QDoubleSpinBox::valueChanged, this, [this] { editBinding(); });
  connect(learn_, &QPushButton::toggled, this, [this](bool active) {
    learning_ = active;
    learningType_ = midiControl::isVolume(selectedId()) || type_->currentData().toString() == "cc" ? QStringLiteral("cc") : QStringLiteral("note");
    status_->setText(active ? uiText(learningType_ == "note" ? "midi.learning_note" : "midi.learning") : QString{});
  });
  connect(remove_, &QPushButton::clicked, this, [this] { type_->setCurrentIndex(0); });
  connect(save_, &QPushButton::clicked, this, &MidiControlSettingsPage::save);
  reload(); retranslate();
}
void MidiControlSettingsPage::setInput(MidiControlInput *input) {
  if (input_) disconnect(input_, nullptr, this, nullptr);
  input_ = input;
  if (input_) {
    connect(input_, &MidiControlInput::connectionChanged, this, &MidiControlSettingsPage::updateConnection);
    connect(input_, &MidiControlInput::received, this, [this](const QString &type, int channel, int number, int value, bool pressed) {
      // A continuous knob must not immediately overwrite the button event in
      // the input monitor. Keep a Note event visible for a short interval.
      if (type == "note" || !noteDisplayHold_.isValid() || noteDisplayHold_.elapsed() >= 1000) {
        lastReceivedText_ = uiText("midi.received").arg(type == "cc" ? QStringLiteral("CC") : pressed ? QStringLiteral("Note On") : QStringLiteral("Note Off")).arg(channel).arg(number).arg(value);
        if (type == "note") noteDisplayHold_.start();
        if (!receivedDisplayTimer_->isActive()) receivedDisplayTimer_->start();
      }
      if (!learning_ || (type == "note" && !pressed)) return;
      if (type != learningType_) return;
      updating_ = true;
      type_->setCurrentIndex(type_->findData(type)); channel_->setValue(channel); number_->setValue(number);
      updating_ = false; const auto replaced = editBinding(true);
      learn_->setChecked(false);
      if (replaced.isEmpty()) status_->setText(uiText("midi.learned"));
      else {
        QStringList labels;
        for (const auto &id : replaced) labels.append(midiControl::actionLabel(id));
        status_->setText(uiText("midi.reassigned").arg(labels.join(QStringLiteral(" / "))).arg(midiControl::actionLabel(selectedId())));
      }
    });
    input_->monitorDevice(device_->currentData().toString());
  }
  updateConnection(); selectRow();
}
void MidiControlSettingsPage::refreshDevices() {
  const auto selected = device_->currentData().toString();
  const QSignalBlocker blocker(device_);
  device_->clear(); device_->addItem(uiText("midi.select_device"), "");
  for (const auto &device : MidiControlInput::devices()) device_->addItem(device.name, device.id);
  if (!selected.isEmpty() && device_->findData(selected) < 0) {
    const auto saved = QJsonDocument::fromJson(selected.toUtf8()).array();
    const auto name = saved.isEmpty() ? QString{} : saved.first().toString();
    device_->addItem(uiText("midi.saved_device").arg(name.isEmpty() ? uiText("midi.device") : name), selected);
  }
  device_->setCurrentIndex(std::max(0, device_->findData(selected)));
  if (input_) input_->monitorDevice(selected);
}
void MidiControlSettingsPage::reload() {
  learning_ = false; learn_->setChecked(false);
  auto settings = makeControllerSettings(); const auto config = midiControl::load(settings);
  enabled_->setChecked(config.value("enabled").toBool());
  bindings_ = config.value("bindings").toObject();
  { const QSignalBlocker blocker(device_); device_->clear(); device_->addItem(QString{}, config.value("device").toString()); }
  refreshDevices(); updateRows();
  table_->setCurrentCell(std::max(0, table_->currentRow()), 0); selectRow(); status_->clear();
}
QString MidiControlSettingsPage::selectedId() const { return ids_.value(table_->currentRow()); }
void MidiControlSettingsPage::selectRow() {
  learning_ = false; learn_->setChecked(false); updating_ = true;
  const auto binding = bindings_.value(selectedId()).toObject();
  type_->setCurrentIndex(std::max(0, type_->findData(binding.value("type").toString())));
  channel_->setValue(binding.value("channel").toInt(1)); number_->setValue(binding.value("number").toInt());
  mode_->setCurrentIndex(std::max(0, mode_->findData(binding.value("mode").toString("absolute"))));
  pickup_->setChecked(binding.value("pickup").toBool(false)); reverse_->setChecked(binding.value("reverse").toBool());
  minimum_->setValue(binding.value("minimum").toInt(audioFaderScale::silence) / 10.0); maximum_->setValue(binding.value("maximum").toInt(audioFaderScale::maximum) / 10.0);
  const bool volume = midiControl::isVolume(selectedId());
  for (auto *widget : {static_cast<QWidget *>(mode_), static_cast<QWidget *>(pickup_), static_cast<QWidget *>(reverse_), static_cast<QWidget *>(minimum_), static_cast<QWidget *>(maximum_), static_cast<QWidget *>(rangeLabel_)}) widget->setVisible(volume);
  pickup_->setEnabled(mode_->currentData().toString() == "absolute");
  learn_->setEnabled(input_ && !selectedId().isEmpty() && !device_->currentData().toString().isEmpty());
  updating_ = false;
}
QStringList MidiControlSettingsPage::editBinding(bool replaceConflicts) {
  if (updating_ || selectedId().isEmpty()) return {};
  if (learning_ && !replaceConflicts) { learning_ = false; learn_->setChecked(false); }
  QStringList replaced;
  auto type = type_->currentData().toString();
  if (midiControl::isVolume(selectedId()) && type == "note") { updating_ = true; type_->setCurrentIndex(type_->findData("cc")); updating_ = false; type = "cc"; }
  if (type.isEmpty()) bindings_.remove(selectedId());
  else {
    QJsonObject binding{{"type", type}, {"channel", channel_->value()}, {"number", number_->value()}};
    if (midiControl::isVolume(selectedId())) {
      binding.insert("scale", "fader");
      binding.insert("mode", mode_->currentData().toString()); binding.insert("pickup", pickup_->isChecked()); binding.insert("reverse", reverse_->isChecked());
      binding.insert("minimum", int(std::lround(minimum_->value() * 10))); binding.insert("maximum", int(std::lround(maximum_->value() * 10)));
    }
    if (replaceConflicts) replaced = midiControl::clearConflictingBindings(bindings_, selectedId(), binding);
    bindings_.insert(selectedId(), binding);
  }
  pickup_->setEnabled(mode_->currentData().toString() == "absolute"); updateRows(); status_->setText(uiText("midi.pending"));
  return replaced;
}
void MidiControlSettingsPage::updateRows() {
  auto settings = makeControllerSettings(); const auto shortcuts = shortcutSettings::load(settings);
  for (int row = 0; row < ids_.size(); ++row) {
    const auto &id = ids_[row]; table_->item(row, 0)->setText(midiControl::actionLabel(id));
    QStringList keys;
    for (const auto &key : shortcuts.value(id).toArray()) keys.append(QKeySequence::fromString(key.toString(), QKeySequence::PortableText).toString(QKeySequence::NativeText));
    for (const auto &definition : shortcutSettings::fixedDefinitions())
      if (definition.id == id) keys.append(definition.key.toString(QKeySequence::NativeText));
    table_->item(row, 1)->setText(keys.join(QStringLiteral(" / ")));
    const auto binding = bindings_.value(id).toObject();
    table_->item(row, 2)->setText(binding.isEmpty() ? uiText("midi.unassigned") : QStringLiteral("%1  Ch %2  #%3").arg(binding.value("type").toString() == "cc" ? QStringLiteral("CC") : QStringLiteral("Note")).arg(binding.value("channel").toInt()).arg(binding.value("number").toInt()));
  }
}
void MidiControlSettingsPage::save() {
  const QJsonObject config{{"enabled", enabled_->isChecked()}, {"device", device_->currentData().toString()}, {"bindings", bindings_}};
  auto error = midiControl::validationError(config);
  if (!error.isEmpty()) {
    const bool duplicate = error.contains(QStringLiteral(" / "));
    for (const auto &id : ids_) error.replace(id, midiControl::actionLabel(id));
    status_->setText(uiText(duplicate ? "midi.duplicate" : "midi.conflict").arg(error)); return;
  }
  if (enabled_->isChecked() && device_->currentData().toString().isEmpty()) { status_->setText(uiText("midi.select_device")); return; }
  auto settings = makeControllerSettings(); auto values = controllerSettingsSnapshot(settings); values.insert(midiControl::settingsKey(), config);
  if (!replaceControllerSettings(settings, values)) { status_->setText(uiText("settings.manage.failed")); return; }
  learning_ = false; learn_->setChecked(false); emit saved(); status_->setText(uiText("midi.saved"));
}
void MidiControlSettingsPage::updateConnection() {
  auto text = input_ ? uiText(input_->statusKey().toUtf8().constData()) : uiText("midi.disabled");
  if (input_ && input_->statusKey() == "midi.open_failed") text = text.arg(input_->errorDetail());
  connection_->setText(text);
}
void MidiControlSettingsPage::retranslate() {
  title_->setText(uiText("midi.title")); description_->setText(uiText("midi.description")); enabled_->setText(uiText("midi.enable")); deviceLabel_->setText(uiText("midi.device"));
  refresh_->setText(uiText("midi.refresh")); learn_->setText(uiText("midi.learn")); remove_->setText(uiText("midi.remove")); save_->setText(uiText("settings.shortcuts.save"));
  type_->setItemText(0, uiText("midi.unassigned")); type_->setAccessibleName(uiText("midi.type"));
  channel_->setPrefix(QStringLiteral("Ch ")); channel_->setAccessibleName(uiText("midi.channel")); number_->setPrefix(QStringLiteral("# ")); number_->setAccessibleName(uiText("midi.number"));
  table_->setHorizontalHeaderLabels({uiText("settings.shortcuts.action"), uiText("settings.category.shortcuts"), uiText("midi.assignment")});
  for (int index = 0; index < mode_->count(); ++index) mode_->setItemText(index, uiText((QStringLiteral("midi.mode.") + mode_->itemData(index).toString()).toUtf8().constData()));
  mode_->setAccessibleName(uiText("midi.cc_mode")); pickup_->setText(uiText("midi.pickup")); reverse_->setText(uiText("midi.reverse")); rangeLabel_->setText(uiText("midi.range"));
  pickup_->setToolTip(uiText("midi.pickup.help"));
  minimum_->setAccessibleName(uiText("midi.minimum")); maximum_->setAccessibleName(uiText("midi.maximum"));
  if (received_->text().isEmpty()) received_->setText(uiText("midi.waiting"));
  updateRows(); refreshDevices(); updateConnection();
}
void MidiControlSettingsPage::changeEvent(QEvent *event) { QWidget::changeEvent(event); if (event->type() == QEvent::LanguageChange) retranslate(); }
void MidiControlSettingsPage::hideEvent(QHideEvent *event) { learning_ = false; learn_->setChecked(false); QWidget::hideEvent(event); }
} // namespace vtrpon
