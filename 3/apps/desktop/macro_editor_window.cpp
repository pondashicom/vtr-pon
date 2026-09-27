#include "macro_editor_window.h"

#include "controller_scaling.h"
#include "language_manager.h"
#include "macro_document.h"
#include "macro_runner.h"
#include "network_time_clock.h"
#include "playlist_model.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QTextCursor>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {
constexpr auto kStepMime = "application/x-vtrpon-macro-step";

QString text(bool japanese, const char *ja, const char *en) {
  return QString::fromUtf8(japanese ? ja : en);
}

QString stepTypeText(MacroStepType type, bool japanese) {
  switch (type) {
  case MacroStepType::Cue: return text(japanese, "スタンバイ", "Prepare for ON AIR");
  case MacroStepType::Take: return QStringLiteral("ON AIR");
  case MacroStepType::WaitUntil: return text(japanese, "時刻まで待機", "Wait until");
  case MacroStepType::Delay: return text(japanese, "ディレイ", "Delay");
  case MacroStepType::ItemSettings: return text(japanese, "項目設定変更", "Item settings");
  case MacroStepType::RuntimeSetting: return text(japanese, "運用設定変更", "Runtime setting");
  case MacroStepType::Transport: return text(japanese, "再生操作", "ON AIR transport");
  case MacroStepType::ManualWait: return text(japanese, "手動再開待ち", "Wait for resume");
  case MacroStepType::Note: return text(japanese, "メモ", "Note");
  }
  return {};
}

const QList<MacroStepType> &stepTypes() {
  static const QList<MacroStepType> types = {
      MacroStepType::Cue, MacroStepType::Take, MacroStepType::WaitUntil,
      MacroStepType::Delay, MacroStepType::ItemSettings,
      MacroStepType::RuntimeSetting, MacroStepType::Transport,
      MacroStepType::ManualWait, MacroStepType::Note};
  return types;
}

bool needsTarget(MacroStepType type) {
  return type == MacroStepType::Cue || type == MacroStepType::Take ||
         type == MacroStepType::ItemSettings;
}

QString playlistLabel(PlaylistModel *model, int number, bool japanese) {
  const QString name = model->playlistName(number);
  return name.trimmed().isEmpty()
      ? text(japanese, "プレイリスト %1", "Playlist %1").arg(number)
      : name;
}

QString itemLabel(PlaylistModel *model, quint64 id, bool japanese) {
  const auto item = model->itemState(id);
  if (!item)
    return id ? text(japanese, "参照切れ: %1", "Missing item: %1").arg(id)
              : text(japanese, "対象未指定", "No target");
  return QStringLiteral("%1 / %2 / %3")
      .arg(playlistLabel(model, item->playlistNumber, japanese))
      .arg(item->itemNumber)
      .arg(item->displayName.isEmpty() ? QFileInfo(item->filePath).fileName()
                                     : item->displayName);
}

QString dateText(const QDateTime &date) {
  return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat) +
                             QStringLiteral(" ") + date.timeZoneAbbreviation()
                       : QString();
}

QJsonValue patchValue(const QJsonObject &patch, const QString &path) {
  const QStringList keys = path.split(QLatin1Char('.'));
  return keys.size() == 1 ? patch.value(path)
                         : patch.value(keys[0]).toObject().value(keys[1]);
}

void setPatchValue(QJsonObject &patch, const QString &path,
                   const QJsonValue &value) {
  const QStringList keys = path.split(QLatin1Char('.'));
  if (keys.size() == 1) {
    if (value.isUndefined()) patch.remove(path);
    else patch.insert(path, value);
    return;
  }
  auto nested = patch.value(keys[0]).toObject();
  if (value.isUndefined()) nested.remove(keys[1]);
  else nested.insert(keys[1], value);
  if (nested.isEmpty()) patch.remove(keys[0]);
  else patch.insert(keys[0], nested);
}

// The drag payload carries a private token, so a drop from another table/window
// cannot reorder this definition. Only moves of one complete row are accepted.
class StepTableModel final : public QAbstractTableModel {
public:
  explicit StepTableModel(PlaylistModel *playlist, QObject *parent)
      : QAbstractTableModel(parent), playlist(playlist) {}

  QList<MacroStep> steps;
  bool japanese = false;
  bool locked = false;
  QString activeMacroId;
  QString displayedMacroId;
  int activeRow = -1;
  MacroRunner::State runnerState = MacroRunner::State::Stopped;
  QString rowStatus;
  std::function<void()> edited;
  std::function<void(int)> moved;

  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : int(steps.size());
  }
  int columnCount(const QModelIndex & = {}) const override { return 6; }
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    const QStringList names = {text(japanese, "順番", "#"),
        text(japanese, "有効", "Enabled"), text(japanese, "操作", "Action"),
        text(japanese, "対象", "Target"), text(japanese, "内容", "Details"),
        text(japanese, "状態", "State")};
    return names.value(section);
  }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= steps.size()) return {};
    const auto &step = steps[index.row()];
    if (index.column() == 1 && role == Qt::CheckStateRole)
      return step.enabled ? Qt::Checked : Qt::Unchecked;
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
    switch (index.column()) {
    case 0: return index.row() + 1;
    case 1: return {};
    case 2: return stepTypeText(step.type, japanese);
    case 3: return needsTarget(step.type) ? itemLabel(playlist, step.itemId, japanese) : QString();
    case 4:
      switch (step.type) {
      case MacroStepType::WaitUntil: return dateText(step.scheduledAt);
      case MacroStepType::Delay: return QLocale().toString(step.durationMs / 1000.0, 'f', 3) + QStringLiteral(" s");
      case MacroStepType::Take:
        return (step.crossfade ? QStringLiteral("Crossfade %1 s").arg(step.crossfadeDurationMs / 1000.0)
                               : QStringLiteral("CUT")) +
               (step.requireReady ? text(japanese, " / 準備済み必須", " / ready required") : QString());
      case MacroStepType::ItemSettings:
        return text(japanese, "%1設定を変更", "%1 settings to change").arg(step.settingsPatch.size());
      case MacroStepType::RuntimeSetting: return step.operation + QStringLiteral(" = %1").arg(step.value);
      case MacroStepType::Transport: return step.operation;
      default: return step.label;
      }
    case 5:
      if (!step.enabled) return text(japanese, "無効", "Disabled");
      if (activeMacroId != displayedMacroId) return {};
      if (index.row() == activeRow) return rowStatus;
      if (index.row() < activeRow || runnerState == MacroRunner::State::Completed)
        return text(japanese, "完了", "Done");
      return {};
    }
    return {};
  }
  Qt::ItemFlags flags(const QModelIndex &index) const override {
    if (!index.isValid()) return locked ? Qt::NoItemFlags : Qt::ItemIsDropEnabled;
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!locked) {
      flags |= Qt::ItemIsDragEnabled;
      if (index.column() == 1) flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
  }
  bool setData(const QModelIndex &index, const QVariant &value, int role) override {
    if (locked || !index.isValid() || index.row() >= steps.size() ||
        index.column() != 1 || role != Qt::CheckStateRole) return false;
    steps[index.row()].enabled = value.toInt() == Qt::Checked;
    emit dataChanged(index, index, {Qt::CheckStateRole});
    if (edited) edited();
    return true;
  }
  QStringList mimeTypes() const override { return {QString::fromLatin1(kStepMime)}; }
  QMimeData *mimeData(const QModelIndexList &indexes) const override {
    auto *mime = new QMimeData;
    if (!indexes.isEmpty())
      mime->setData(QString::fromLatin1(kStepMime),
                    token.toUtf8() + ':' + QByteArray::number(indexes.first().row()));
    return mime;
  }
  Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
  Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }
  bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row,
                     int column, const QModelIndex &parent) override {
    if (locked || action != Qt::MoveAction || parent.isValid() || column > 0)
      return false;
    const auto payload = data->data(QString::fromLatin1(kStepMime)).split(':');
    if (payload.size() != 2 || payload[0] != token.toUtf8()) return false;
    bool ok = false;
    const int source = payload[1].toInt(&ok);
    if (!ok || source < 0 || source >= steps.size()) return false;
    const int destination = row < 0 ? int(steps.size()) : row;
    if (destination < 0 || destination > steps.size() || destination == source ||
        destination == source + 1) return false;
    beginMoveRows({}, source, source, {}, destination);
    const int finalRow = destination > source ? destination - 1 : destination;
    steps.move(source, finalRow);
    endMoveRows();
    if (edited) edited();
    if (moved) moved(finalRow);
    return true;
  }
  void resetSteps(const QList<MacroStep> &replacement) {
    beginResetModel(); steps = replacement; endResetModel();
  }
  void refresh() {
    if (!steps.isEmpty()) emit dataChanged(index(0, 0), index(int(steps.size()) - 1, 5));
    emit headerDataChanged(Qt::Horizontal, 0, 5);
  }
private:
  PlaylistModel *playlist;
  const QString token = newMacroId();
};
} // namespace

struct MacroEditorWindow::Private {
  MacroEditorWindow *q;
  PlaylistModel *model;
  MacroRunner *runner;
  QPointer<vtrpon::UiLanguageManager> language;
  QPointer<vtrpon::NetworkTimeClock> clock;
  bool japanese = false;
  bool saving = false;
  bool refreshing = false;
  QList<MacroDefinition> definitions;
  QString selectedMacroId;
  QString diagnostics;
  QWidget *content = nullptr;
  QWidget *editArea = nullptr;
  QWidget *macroBar = nullptr;
  QComboBox *macroCombo = nullptr;
  QLineEdit *name = nullptr;
  QComboBox *playlistCombo = nullptr;
  QLineEdit *search = nullptr;
  QListWidget *sourceList = nullptr;
  QComboBox *addType = nullptr;
  QDateTimeEdit *helperTime = nullptr;
  QComboBox *helperMode = nullptr;
  QTableView *table = nullptr;
  StepTableModel *tableModel = nullptr;
  QScrollArea *details = nullptr;
  QLabel *message = nullptr;
  QLabel *stateLabel = nullptr;
  QLabel *clockLabel = nullptr;
  QLabel *scheduleLabel = nullptr;
  QPlainTextEdit *log = nullptr;
  QPushButton *runButton = nullptr;
  QPushButton *pauseButton = nullptr;
  QPushButton *resumeButton = nullptr;
  QPushButton *stopButton = nullptr;

  QString tr(const char *ja, const char *en) const { return text(japanese, ja, en); }
  QDateTime now() const { return clock ? clock->currentLocalDateTime() : QDateTime::currentDateTime(); }
  int macroIndex() const {
    for (int i = 0; i < definitions.size(); ++i)
      if (definitions[i].id == selectedMacroId) return i;
    return -1;
  }
  int selectedRow() const { return table->currentIndex().row(); }
  MacroStep *selectedStep() {
    const int row = selectedRow();
    return row >= 0 && row < tableModel->steps.size() ? &tableModel->steps[row] : nullptr;
  }
  quint64 sourceItemId() const {
    return sourceList->currentItem() ? sourceList->currentItem()->data(Qt::UserRole).toULongLong() : 0;
  }
  QPushButton *button(QBoxLayout *layout, const char *ja, const char *en,
                      const char *objectName, std::function<void()> action) {
    auto *result = new QPushButton(tr(ja, en), q);
    result->setObjectName(QString::fromLatin1(objectName));
    layout->addWidget(result);
    QObject::connect(result, &QPushButton::clicked, q, std::move(action));
    return result;
  }
  void showMessage(const QString &value) {
    message->setText(value);
    message->setVisible(!value.isEmpty());
  }
  bool persist() {
    if (runner->isActive() || refreshing) return false;
    const int index = macroIndex();
    if (index >= 0) definitions[index].steps = tableModel->steps;
    QString error;
    saving = true;
    const bool ok = model->setMacros(definitions, &error);
    saving = false;
    if (!ok) showMessage(error);
    return ok;
  }
  void stepEdited() {
    if (refreshing || runner->isActive()) return;
    tableModel->refresh();
    persist();
  }
  void build();
  void refreshMacros();
  void selectMacro(const QString &id);
  void refreshSources();
  void refreshPlaylists();
  void refreshDetails();
  void refreshStatus();
  void appendSteps(const QList<MacroStep> &steps);
  void addStep();
  void addTimeCue();
  void newMacro(bool duplicate);
  void deleteMacro();
  void moveStep(int offset);
  void check(bool start);
  void addTarget(QFormLayout *form, const MacroStep &step);
  void addSettings(QFormLayout *form, const MacroStep &step);
};

void MacroEditorWindow::Private::build() {
  const int oldRow = table ? selectedRow() : -1;
  const int oldPlaylist = playlistCombo ? playlistCombo->currentData().toInt() : model->activePlaylistNumber();
  const QString oldSearch = search ? search->text() : QString();
  if (content) {
    delete content;
    content = nullptr;
  }
  content = new QWidget(q);
  q->layout()->addWidget(content);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                            vtrpon::scalePx(12), vtrpon::scalePx(12));
  layout->setSpacing(vtrpon::scalePx(8));
  q->setWindowTitle(tr("VTR-PON マクロエディター", "VTR-PON Macro Editor"));

  macroBar = new QWidget(content);
  auto *bar = new QHBoxLayout(macroBar);
  bar->setContentsMargins(0, 0, 0, 0);
  macroCombo = new QComboBox(macroBar);
  macroCombo->setObjectName(QStringLiteral("macroDefinitionCombo"));
  macroCombo->setMinimumContentsLength(14);
  macroCombo->setAccessibleName(tr("マクロ選択", "Select macro"));
  bar->addWidget(macroCombo, 1);
  name = new QLineEdit(macroBar);
  name->setObjectName(QStringLiteral("macroNameEdit"));
  name->setPlaceholderText(tr("マクロ名", "Macro name"));
  name->setAccessibleName(name->placeholderText());
  bar->addWidget(name, 1);
  button(bar, "新規", "New", "macroNewButton", [this] { newMacro(false); });
  button(bar, "複製", "Duplicate", "macroDuplicateButton", [this] { newMacro(true); });
  button(bar, "削除", "Delete", "macroDeleteButton", [this] { deleteMacro(); });
  button(bar, "保存", "Save", "macroSaveButton", [this] {
    if (persist()) emit q->saveRequested();
  });
  button(bar, "実行前チェック", "Preflight", "macroPreflightButton", [this] { check(false); });
  layout->addWidget(macroBar);
  QObject::connect(macroCombo, &QComboBox::currentIndexChanged, q, [this](int) {
    if (!refreshing) selectMacro(macroCombo->currentData().toString());
  });
  QObject::connect(name, &QLineEdit::editingFinished, q, [this] {
    const int index = macroIndex();
    if (index < 0 || runner->isActive() || refreshing) return;
    if (name->text().trimmed().isEmpty()) {
      name->setText(definitions[index].name);
      showMessage(tr("マクロ名を入力してください。", "Enter a macro name."));
      return;
    }
    definitions[index].name = name->text().trimmed();
    macroCombo->setItemText(macroCombo->currentIndex(), definitions[index].name);
    persist();
  });

  auto *splitter = new QSplitter(Qt::Horizontal, content);
  editArea = splitter;
  auto *sources = new QGroupBox(tr("プレイリスト", "Playlist items"), splitter);
  auto *sourceLayout = new QVBoxLayout(sources);
  playlistCombo = new QComboBox(sources);
  playlistCombo->setObjectName(QStringLiteral("macroSourcePlaylistCombo"));
  playlistCombo->setAccessibleName(tr("参照プレイリスト", "Source playlist"));
  sourceLayout->addWidget(playlistCombo);
  search = new QLineEdit(sources);
  search->setObjectName(QStringLiteral("macroSourceSearch"));
  search->setPlaceholderText(tr("項目を検索", "Search items"));
  search->setAccessibleName(search->placeholderText());
  sourceLayout->addWidget(search);
  sourceList = new QListWidget(sources);
  sourceList->setObjectName(QStringLiteral("macroSourceItems"));
  sourceList->setAccessibleName(tr("参照項目一覧", "Source items"));
  sourceLayout->addWidget(sourceList, 1);
  addType = new QComboBox(sources);
  addType->setObjectName(QStringLiteral("macroAddStepTypeCombo"));
  addType->setAccessibleName(tr("追加ステップの種類", "Step type to add"));
  for (auto type : stepTypes()) addType->addItem(stepTypeText(type, japanese), int(type));
  sourceLayout->addWidget(addType);
  button(sourceLayout, "ステップに追加", "Add step", "macroAddStepButton", [this] { addStep(); });
  button(sourceLayout, "このプレイリストにファイル追加", "Add files to this playlist", "macroAddFilesButton", [this] {
    if (!runner->isActive() && playlistCombo->currentIndex() >= 0)
      emit q->addFilesRequested(playlistCombo->currentData().toInt());
  });
  auto *timeGroup = new QGroupBox(tr("時刻指定を追加", "Add timed operation"), sources);
  auto *timeLayout = new QVBoxLayout(timeGroup);
  helperTime = new QDateTimeEdit(now().addSecs(60), timeGroup);
  helperTime->setObjectName(QStringLiteral("macroTimeCueDateTime"));
  helperTime->setCalendarPopup(true);
  helperTime->setDisplayFormat(QLocale().dateFormat(QLocale::ShortFormat) + QStringLiteral(" HH:mm:ss"));
  helperTime->setAccessibleName(tr("実行日と時刻", "Execution date and time"));
  timeLayout->addWidget(helperTime);
  helperMode = new QComboBox(timeGroup);
  helperMode->addItem(tr("その時刻にON AIR", "ON AIR at this time"), true);
  helperMode->addItem(tr("その時刻にスタンバイ", "Prepare for ON AIR at this time"), false);
  timeLayout->addWidget(helperMode);
  button(timeLayout, "時刻指定を追加", "Add timed operation", "macroAddTimeCueButton", [this] { addTimeCue(); });
  sourceLayout->addWidget(timeGroup);
  QObject::connect(playlistCombo, &QComboBox::currentIndexChanged, q, [this](int) {
    if (!refreshing) refreshSources();
  });
  QObject::connect(search, &QLineEdit::textChanged, q, [this] { refreshSources(); });

  auto *stepsGroup = new QGroupBox(tr("ステップ", "Steps"), splitter);
  auto *stepsLayout = new QVBoxLayout(stepsGroup);
  table = new QTableView(stepsGroup);
  table->setObjectName(QStringLiteral("macroStepTable"));
  table->setAccessibleName(tr("実行ステップ一覧", "Execution steps"));
  tableModel = new StepTableModel(model, table);
  tableModel->japanese = japanese;
  tableModel->edited = [this] { stepEdited(); };
  tableModel->moved = [this](int row) { table->selectRow(row); refreshDetails(); };
  table->setModel(tableModel);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setDragDropMode(QAbstractItemView::InternalMove);
  table->setDragDropOverwriteMode(false);
  table->setDefaultDropAction(Qt::MoveAction);
  table->setDropIndicatorShown(true);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setColumnWidth(0, vtrpon::scalePx(40));
  table->setColumnWidth(1, vtrpon::scalePx(60));
  table->setColumnWidth(2, vtrpon::scalePx(115));
  table->setColumnWidth(3, vtrpon::scalePx(180));
  table->setColumnWidth(4, vtrpon::scalePx(190));
  stepsLayout->addWidget(table, 1);
  auto *stepButtons = new QHBoxLayout;
  button(stepButtons, "複製", "Duplicate", "macroDuplicateStepButton", [this] {
    const auto *step = selectedStep();
    if (!step || runner->isActive()) return;
    auto copy = *step; copy.id = newMacroId(); appendSteps({copy});
  });
  button(stepButtons, "削除", "Delete", "macroDeleteStepButton", [this] {
    const int row = selectedRow();
    if (row < 0 || runner->isActive()) return;
    auto steps = tableModel->steps; steps.removeAt(row); tableModel->resetSteps(steps);
    if (!steps.isEmpty()) table->selectRow((std::min)(row, int(steps.size()) - 1));
    stepEdited(); refreshDetails();
  });
  button(stepButtons, "上へ", "Move up", "macroMoveStepUpButton", [this] { moveStep(-1); });
  button(stepButtons, "下へ", "Move down", "macroMoveStepDownButton", [this] { moveStep(1); });
  stepButtons->addStretch();
  stepsLayout->addLayout(stepButtons);
  QObject::connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, q,
                   [this] { if (!refreshing) refreshDetails(); });

  details = new QScrollArea(splitter);
  details->setObjectName(QStringLiteral("macroStepDetails"));
  details->setWidgetResizable(true);
  details->setMinimumWidth(vtrpon::scalePx(300));
  splitter->setChildrenCollapsible(false);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 3);
  splitter->setStretchFactor(2, 2);
  splitter->setSizes({vtrpon::scalePx(260), vtrpon::scalePx(620), vtrpon::scalePx(360)});
  layout->addWidget(splitter, 1);

  message = new QLabel(content);
  message->setObjectName(QStringLiteral("macroEditorMessage"));
  message->setWordWrap(true);
  message->setTextInteractionFlags(Qt::TextSelectableByMouse);
  message->hide();
  layout->addWidget(message);
  auto *transport = new QHBoxLayout;
  runButton = button(transport, "実行", "Run", "macroRunButton", [this] { check(true); });
  pauseButton = button(transport, "一時停止", "Pause", "macroPauseButton", [this] { runner->pause(); });
  resumeButton = button(transport, "再開", "Resume", "macroResumeButton", [this] { runner->resume(); });
  stopButton = button(transport, "停止", "Stop", "macroStopButton", [this] { runner->stop(); });
  stateLabel = new QLabel(content);
  stateLabel->setObjectName(QStringLiteral("macroExecutionStatus"));
  stateLabel->setWordWrap(true);
  transport->addWidget(stateLabel, 1);
  layout->addLayout(transport);
  clockLabel = new QLabel(content);
  clockLabel->setObjectName(QStringLiteral("macroClockStatus"));
  clockLabel->setWordWrap(true);
  scheduleLabel = new QLabel(content);
  scheduleLabel->setObjectName(QStringLiteral("macroNextSchedule"));
  scheduleLabel->setWordWrap(true);
  layout->addWidget(clockLabel);
  layout->addWidget(scheduleLabel);
  log = new QPlainTextEdit(content);
  log->setObjectName(QStringLiteral("macroExecutionLog"));
  log->setReadOnly(true);
  log->setAccessibleName(tr("実行結果ログ", "Execution log"));
  log->setMaximumBlockCount(1000);
  log->setMaximumHeight(vtrpon::scalePx(130));
  layout->addWidget(log);

  refreshMacros();
  refreshPlaylists();
  const int playlistIndex = playlistCombo->findData(oldPlaylist);
  if (playlistIndex >= 0) playlistCombo->setCurrentIndex(playlistIndex);
  search->setText(oldSearch);
  if (oldRow >= 0 && oldRow < tableModel->steps.size()) table->selectRow(oldRow);
  refreshDetails();
  refreshStatus();
}

void MacroEditorWindow::Private::refreshMacros() {
  if (saving) return;
  definitions = model->macros();
  refreshing = true;
  macroCombo->clear();
  for (const auto &definition : definitions) macroCombo->addItem(definition.name, definition.id);
  int index = macroCombo->findData(selectedMacroId);
  if (index < 0 && !definitions.isEmpty()) index = 0;
  macroCombo->setCurrentIndex(index);
  refreshing = false;
  selectMacro(index >= 0 ? definitions[index].id : QString());
}

void MacroEditorWindow::Private::selectMacro(const QString &id) {
  selectedMacroId = id;
  const int index = macroIndex();
  const QString stepId = selectedStep() ? selectedStep()->id : QString();
  refreshing = true;
  tableModel->displayedMacroId = id;
  tableModel->resetSteps(index >= 0 ? definitions[index].steps : QList<MacroStep>{});
  name->setText(index >= 0 ? definitions[index].name : QString());
  name->setEnabled(index >= 0);
  int row = tableModel->steps.isEmpty() ? -1 : 0;
  for (int i = 0; i < tableModel->steps.size(); ++i)
    if (tableModel->steps[i].id == stepId) row = i;
  if (row >= 0) table->selectRow(row);
  refreshing = false;
  refreshDetails();
  refreshStatus();
}

void MacroEditorWindow::Private::refreshPlaylists() {
  const int previous = playlistCombo->currentData().toInt();
  const QSignalBlocker blocked(playlistCombo);
  playlistCombo->clear();
  for (const auto &playlist : model->playlists())
    playlistCombo->addItem(playlistLabel(model, playlist.number, japanese), playlist.number);
  int index = playlistCombo->findData(previous);
  if (index < 0) index = playlistCombo->findData(model->activePlaylistNumber());
  playlistCombo->setCurrentIndex(index < 0 && playlistCombo->count() ? 0 : index);
  refreshSources();
  tableModel->refresh();
}

void MacroEditorWindow::Private::refreshSources() {
  const quint64 previous = sourceItemId();
  sourceList->clear();
  const int playlist = playlistCombo->currentData().toInt();
  const QString filter = search->text().trimmed();
  for (int row = 0; row < model->itemCount(playlist); ++row) {
    const quint64 id = model->itemIdAt(playlist, row);
    const QString label = itemLabel(model, id, japanese);
    if (!filter.isEmpty() && !label.contains(filter, Qt::CaseInsensitive)) continue;
    auto *item = new QListWidgetItem(label, sourceList);
    item->setData(Qt::UserRole, QVariant::fromValue(id));
    item->setToolTip(label);
    if (id == previous) sourceList->setCurrentItem(item);
  }
  if (!sourceList->currentItem() && sourceList->count()) sourceList->setCurrentRow(0);
}

void MacroEditorWindow::Private::newMacro(bool duplicate) {
  if (runner->isActive()) return;
  const int index = macroIndex();
  MacroDefinition definition;
  if (duplicate && index >= 0) {
    definition = definitions[index];
    definition.id = newMacroId();
    definition.name += tr(" のコピー", " copy");
    for (auto &step : definition.steps) step.id = newMacroId();
  } else {
    definition.name = tr("マクロ %1", "Macro %1").arg(definitions.size() + 1);
  }
  definitions.append(definition);
  selectedMacroId = definition.id;
  tableModel->resetSteps(definition.steps);
  if (persist()) refreshMacros();
}

void MacroEditorWindow::Private::deleteMacro() {
  const int index = macroIndex();
  if (index < 0 || runner->isActive()) return;
  definitions.removeAt(index);
  selectedMacroId.clear();
  tableModel->resetSteps({});
  if (persist()) refreshMacros();
}

void MacroEditorWindow::Private::appendSteps(const QList<MacroStep> &steps) {
  if (runner->isActive()) return;
  if (macroIndex() < 0) newMacro(false);
  const int row = selectedRow() >= 0 ? selectedRow() + 1 : int(tableModel->steps.size());
  auto updated = tableModel->steps;
  for (int i = 0; i < steps.size(); ++i) updated.insert(row + i, steps[i]);
  tableModel->resetSteps(updated);
  table->selectRow(row);
  stepEdited(); refreshDetails();
}

void MacroEditorWindow::Private::addStep() {
  MacroStep step;
  step.type = static_cast<MacroStepType>(addType->currentData().toInt());
  step.itemId = needsTarget(step.type) ? sourceItemId() : 0;
  step.documentId = needsTarget(step.type) ? model->documentId() : QString();
  if (step.type == MacroStepType::WaitUntil) step.scheduledAt = now().addSecs(60);
  if (step.type == MacroStepType::RuntimeSetting) step.operation = QStringLiteral("masterVolume");
  if (step.type == MacroStepType::Transport) step.operation = QStringLiteral("play");
  appendSteps({step});
}

void MacroEditorWindow::Private::addTimeCue() {
  if (!sourceItemId()) {
    showMessage(tr("左の一覧から対象項目を選んでください。", "Select a target item in the left list."));
    return;
  }
  MacroStep cue;
  cue.type = MacroStepType::Cue;
  cue.itemId = sourceItemId();
  cue.documentId = model->documentId();
  MacroStep wait;
  wait.type = MacroStepType::WaitUntil;
  wait.scheduledAt = helperTime->dateTime();
  // QDateTimeEdit retains the shared clock's time zone (including explicit dates).
  if (helperMode->currentData().toBool()) {
    MacroStep take = cue;
    take.id = newMacroId();
    take.type = MacroStepType::Take;
    take.requireReady = true;
    appendSteps({cue, wait, take});
  } else {
    appendSteps({wait, cue});
  }
}

void MacroEditorWindow::Private::moveStep(int offset) {
  const int row = selectedRow();
  const int target = row + offset;
  if (runner->isActive() || row < 0 || target < 0 || target >= tableModel->steps.size()) return;
  auto steps = tableModel->steps;
  steps.move(row, target);
  tableModel->resetSteps(steps);
  table->selectRow(target);
  stepEdited(); refreshDetails();
}

void MacroEditorWindow::Private::addTarget(QFormLayout *form, const MacroStep &step) {
  auto *target = new QComboBox;
  target->setObjectName(QStringLiteral("macroStepTargetCombo"));
  target->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  target->setMinimumContentsLength(12);
  target->addItem(tr("対象未指定", "No target"), QVariant::fromValue(quint64(0)));
  for (const auto &playlist : model->playlists())
    for (int row = 0; row < model->itemCount(playlist.number); ++row) {
      const quint64 id = model->itemIdAt(playlist.number, row);
      target->addItem(itemLabel(model, id, japanese), QVariant::fromValue(id));
    }
  int targetIndex = target->findData(QVariant::fromValue(step.itemId));
  if (targetIndex < 0) {
    target->addItem(itemLabel(model, step.itemId, japanese), QVariant::fromValue(step.itemId));
    targetIndex = target->count() - 1;
  }
  target->setCurrentIndex(targetIndex);
  form->addRow(tr("対象項目", "Target item"), target);
  if (step.documentId != model->documentId() && step.itemId) {
    auto *hint = new QLabel(tr("別文書の参照です。対象を選び直してください。", "This target belongs to another document. Select it again."));
    hint->setWordWrap(true); form->addRow(hint);
  }
  QObject::connect(target, &QComboBox::activated, q, [this, target](int) {
    if (auto *s = selectedStep()) {
      s->itemId = target->currentData().toULongLong();
      s->documentId = model->documentId();
      stepEdited();
      // Defer removal of the emitting combo until its activation returns.
      QTimer::singleShot(0, q, [this] { refreshDetails(); });
    }
  });
}

void MacroEditorWindow::Private::refreshDetails() {
  auto *body = new QWidget;
  body->setObjectName(QStringLiteral("macroStepDetailsBody"));
  auto *layout = new QVBoxLayout(body);
  auto *title = new QLabel(tr("ステップ詳細", "Step details"), body);
  layout->addWidget(title);
  const auto *selected = selectedStep();
  if (!selected) {
    auto *hint = new QLabel(tr("ステップを追加、または選択してください。", "Add or select a step."), body);
    hint->setWordWrap(true); layout->addWidget(hint); layout->addStretch();
    details->setWidget(body);
    return;
  }
  const MacroStep step = *selected;
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  layout->addLayout(form);
  auto *type = new QComboBox(body);
  type->setObjectName(QStringLiteral("macroStepTypeCombo"));
  for (auto value : stepTypes()) type->addItem(stepTypeText(value, japanese), int(value));
  type->setCurrentIndex(type->findData(int(step.type)));
  form->addRow(tr("操作", "Action"), type);
  QObject::connect(type, &QComboBox::activated, q, [this, type](int) {
    if (auto *s = selectedStep()) {
      s->type = static_cast<MacroStepType>(type->currentData().toInt());
      if (s->type != MacroStepType::WaitUntil) s->scheduledAt = {};
      if (needsTarget(s->type) && !s->itemId) {
        s->itemId = sourceItemId(); s->documentId = model->documentId();
      }
      if (s->type == MacroStepType::WaitUntil && !s->scheduledAt.isValid()) s->scheduledAt = now().addSecs(60);
      if (s->type == MacroStepType::Transport) s->operation = QStringLiteral("play");
      if (s->type == MacroStepType::RuntimeSetting) { s->operation = QStringLiteral("masterVolume"); s->value = 0; }
      stepEdited();
      QTimer::singleShot(0, q, [this] { refreshDetails(); });
    }
  });
  auto *label = new QLineEdit(step.label, body);
  label->setObjectName(QStringLiteral("macroStepLabelEdit"));
  form->addRow(tr("説明", "Description"), label);
  QObject::connect(label, &QLineEdit::textEdited, q, [this](const QString &value) {
    if (auto *s = selectedStep()) { s->label = value; stepEdited(); }
  });
  if (needsTarget(step.type)) addTarget(form, step);

  if (step.type == MacroStepType::WaitUntil) {
    auto *date = new QDateTimeEdit(step.scheduledAt.isValid() ? step.scheduledAt : now().addSecs(60));
    date->setObjectName(QStringLiteral("macroStepScheduledAt"));
    date->setCalendarPopup(true);
    date->setDisplayFormat(QLocale().dateFormat(QLocale::ShortFormat) + QStringLiteral(" HH:mm:ss"));
    form->addRow(tr("実行日・時刻", "Date and time"), date);
    auto *zone = new QLabel(tr("タイムゾーン: %1", "Time zone: %1").arg(QString::fromUtf8(date->dateTime().timeZone().id())));
    form->addRow(zone);
    auto *hint = new QLabel(tr("翌日なら日付も変更してください。過去の時刻は実行できません。", "For the next day, change the date too. Past times cannot run."));
    hint->setWordWrap(true); form->addRow(hint);
    QObject::connect(date, &QDateTimeEdit::dateTimeChanged, q, [this](const QDateTime &value) {
      if (auto *s = selectedStep()) { s->scheduledAt = value; stepEdited(); }
    });
  }
  if (step.type == MacroStepType::Delay) {
    auto *seconds = new QDoubleSpinBox;
    seconds->setObjectName(QStringLiteral("macroStepDelaySeconds"));
    seconds->setRange(0, 604800); seconds->setDecimals(3); seconds->setSuffix(QStringLiteral(" s"));
    seconds->setValue(step.durationMs / 1000.0);
    form->addRow(tr("待機秒数", "Wait duration"), seconds);
    QObject::connect(seconds, &QDoubleSpinBox::valueChanged, q, [this](double value) {
      if (auto *s = selectedStep()) { s->durationMs = qRound64(value * 1000); stepEdited(); }
    });
  }
  if (step.type == MacroStepType::Take) {
    auto *transition = new QComboBox;
    transition->addItem(QStringLiteral("CUT"), false);
    transition->addItem(QStringLiteral("Crossfade"), true);
    transition->setCurrentIndex(step.crossfade ? 1 : 0);
    form->addRow(tr("切替方式", "Transition"), transition);
    auto *seconds = new QDoubleSpinBox;
    seconds->setRange(.1, 60); seconds->setDecimals(3); seconds->setSuffix(QStringLiteral(" s"));
    seconds->setValue(step.crossfadeDurationMs / 1000.0); seconds->setEnabled(step.crossfade);
    form->addRow(tr("切替秒数", "Transition duration"), seconds);
    auto *ready = new QCheckBox(tr("スタンバイ完了が必須", "Item must already be ready"));
    ready->setChecked(step.requireReady);
    form->addRow(ready);
    QObject::connect(transition, &QComboBox::currentIndexChanged, q, [this, transition, seconds](int) {
      if (auto *s = selectedStep()) {
        s->crossfade = transition->currentData().toBool(); seconds->setEnabled(s->crossfade); stepEdited();
      }
    });
    QObject::connect(seconds, &QDoubleSpinBox::valueChanged, q, [this](double value) {
      if (auto *s = selectedStep()) { s->crossfadeDurationMs = qRound64(value * 1000); stepEdited(); }
    });
    QObject::connect(ready, &QCheckBox::toggled, q, [this](bool value) {
      if (auto *s = selectedStep()) { s->requireReady = value; stepEdited(); }
    });
  }
  if (step.type == MacroStepType::ItemSettings) addSettings(form, step);
  if (step.type == MacroStepType::RuntimeSetting || step.type == MacroStepType::Transport) {
    auto *operation = new QComboBox;
    operation->setObjectName(QStringLiteral("macroStepOperationCombo"));
    if (step.type == MacroStepType::Transport) {
      operation->addItem(tr("オンエアを再生", "Play ON AIR"), QStringLiteral("play"));
      operation->addItem(tr("オンエアを一時停止", "Pause ON AIR"), QStringLiteral("pause"));
      operation->addItem(QStringLiteral("OFF AIR"), QStringLiteral("offAir"));
    } else {
      operation->addItem(tr("MASTER音量", "MASTER volume"), QStringLiteral("masterVolume"));
      operation->addItem(tr("ITEM音量", "ITEM volume"), QStringLiteral("itemVolume"));
      operation->addItem(QStringLiteral("MUTE"), QStringLiteral("mute"));
      operation->addItem(tr("オンエア再生速度", "ON AIR playback rate"), QStringLiteral("playbackRate"));
    }
    operation->setCurrentIndex(operation->findData(step.operation));
    form->addRow(tr("操作内容", "Operation"), operation);
    QObject::connect(operation, &QComboBox::currentIndexChanged, q, [this, operation](int) {
      if (auto *s = selectedStep()) {
        s->operation = operation->currentData().toString();
        s->value = s->operation == QStringLiteral("playbackRate") ? 1.0 : 0.0;
        stepEdited(); QTimer::singleShot(0, q, [this] { refreshDetails(); });
      }
    });
    if (step.type == MacroStepType::RuntimeSetting) {
      if (step.operation == QStringLiteral("mute") || step.operation == QStringLiteral("playbackRate")) {
        auto *value = new QComboBox;
        if (step.operation == QStringLiteral("mute")) {
          value->addItem(QStringLiteral("OFF"), 0.0); value->addItem(QStringLiteral("ON"), 1.0);
        } else {
          for (double rate : {.5, .75, 1., 1.25, 1.5, 2., 3.})
            value->addItem(QStringLiteral("%1×").arg(rate), rate);
        }
        value->setCurrentIndex(value->findData(step.value));
        form->addRow(tr("設定値", "Value"), value);
        QObject::connect(value, &QComboBox::currentIndexChanged, q, [this, value](int) {
          if (auto *s = selectedStep()) { s->value = value->currentData().toDouble(); stepEdited(); }
        });
      } else {
        auto *value = new QDoubleSpinBox;
        value->setRange(-96, 6); value->setDecimals(1); value->setSingleStep(.1);
        value->setSuffix(QStringLiteral(" dB")); value->setValue(step.value / 10.0);
        form->addRow(tr("音量", "Volume"), value);
        QObject::connect(value, &QDoubleSpinBox::valueChanged, q, [this](double value) {
          if (auto *s = selectedStep()) { s->value = qRound(value * 10); stepEdited(); }
        });
      }
    }
  }
  if (step.type == MacroStepType::ManualWait) {
    auto *hint = new QLabel(tr("この行で待機し、「再開」で次の行へ進みます。送出は継続します。", "Wait here until Resume is pressed. ON AIR output continues."));
    hint->setWordWrap(true); form->addRow(hint);
  }
  layout->addStretch();
  details->setWidget(body);
  body->setEnabled(!runner->isActive());
}

void MacroEditorWindow::Private::addSettings(QFormLayout *form, const MacroStep &step) {
  const auto target = model->itemState(step.itemId);
  const PlaylistItemSettings current = target ? target->settings : PlaylistItemSettings{};
  auto *hint = new QLabel(tr("変更する欄にチェックを入れてください。未指定の設定は維持します。", "Check each field to change. Other settings are preserved."));
  hint->setWordWrap(true); form->addRow(hint);

  auto addField = [this, form, step](const QString &path, const QString &label,
                                    QWidget *editor, std::function<QJsonValue()> read) {
    auto *change = new QCheckBox(label);
    change->setObjectName(QStringLiteral("macroPatchEnable_") + path);
    editor->setObjectName(QStringLiteral("macroPatchValue_") + path);
    editor->setAccessibleName(label);
    const bool enabled = !patchValue(step.settingsPatch, path).isUndefined();
    change->setChecked(enabled); editor->setEnabled(enabled);
    form->addRow(change, editor);
    QObject::connect(change, &QCheckBox::toggled, q, [this, path, editor, read](bool enabled) {
      editor->setEnabled(enabled);
      if (auto *s = selectedStep()) {
        setPatchValue(s->settingsPatch, path, enabled ? read() : QJsonValue(QJsonValue::Undefined)); stepEdited();
      }
    });
    return [this, change, path, read] {
      if (!change->isChecked()) return;
      if (auto *s = selectedStep()) { setPatchValue(s->settingsPatch, path, read()); stepEdited(); }
    };
  };
  auto valueOr = [&step](const QString &path, QJsonValue fallback) {
    const auto value = patchValue(step.settingsPatch, path);
    return value.isUndefined() ? fallback : value;
  };
  auto enumField = [&](const char *key, const QString &label,
                        const QStringList &values, int currentIndex) {
    const QString path = QString::fromLatin1(key);
    auto *editor = new QComboBox;
    for (const QString &value : values) editor->addItem(value.toUpper(), value);
    const QString initial = valueOr(path, values.value(currentIndex)).toString();
    editor->setCurrentIndex(editor->findData(initial));
    auto save = addField(path, label, editor, [editor] { return QJsonValue(editor->currentData().toString()); });
    QObject::connect(editor, &QComboBox::currentIndexChanged, q, [save](int) { save(); });
  };
  auto boolField = [&](const char *key, const QString &label, bool currentValue) {
    const QString path = QString::fromLatin1(key);
    auto *editor = new QComboBox;
    editor->addItem(QStringLiteral("OFF"), false); editor->addItem(QStringLiteral("ON"), true);
    editor->setCurrentIndex(valueOr(path, currentValue).toBool() ? 1 : 0);
    auto save = addField(path, label, editor, [editor] { return QJsonValue(editor->currentData().toBool()); });
    QObject::connect(editor, &QComboBox::currentIndexChanged, q, [save](int) { save(); });
  };
  auto numberField = [&](const char *key, const QString &label, qint64 currentValue,
                          double minimum, double maximum, double divisor,
                          int decimals, const QString &suffix, bool nullable = false) {
    const QString path = QString::fromLatin1(key);
    auto *editor = new QDoubleSpinBox;
    editor->setDecimals(decimals); editor->setRange(nullable ? minimum - 1 : minimum, maximum);
    editor->setSingleStep(decimals > 0 ? .1 : 1);
    editor->setSuffix(suffix);
    if (nullable) editor->setSpecialValueText(tr("未指定", "Unset"));
    const auto initial = valueOr(path, QJsonValue(currentValue));
    editor->setValue(initial.isNull() ? minimum - 1 : initial.toDouble() / divisor);
    auto save = addField(path, label, editor, [editor, divisor, minimum, nullable] {
      return nullable && editor->value() < minimum ? QJsonValue(QJsonValue::Null)
          : QJsonValue(qRound64(editor->value() * divisor));
    });
    QObject::connect(editor, &QDoubleSpinBox::valueChanged, q, [save](double) { save(); });
  };
  enumField("startMode", QStringLiteral("START MODE"), {QStringLiteral("pause"), QStringLiteral("play")}, int(current.startMode));
  boolField("fadeIn.enabled", QStringLiteral("Fade In"), current.fadeInEnabled);
  numberField("fadeIn.durationMs", tr("Fade In秒数", "Fade In duration"), current.fadeInDurationMs, .1, 60, 1000, 3, QStringLiteral(" s"));
  enumField("endMode", QStringLiteral("END MODE"), {QStringLiteral("stop"), QStringLiteral("pause"), QStringLiteral("repeat"), QStringLiteral("next"), QStringLiteral("goto")}, int(current.endMode));
  boolField("fadeOut.enabled", QStringLiteral("Fade Out"), current.fadeOutEnabled);
  numberField("fadeOut.durationMs", tr("Fade Out秒数", "Fade Out duration"), current.fadeOutDurationMs, .1, 60, 1000, 3, QStringLiteral(" s"));
  boolField("endCrossfade.enabled", QStringLiteral("End Crossfade"), current.endCrossfadeEnabled);
  numberField("endCrossfade.durationMs", tr("End Crossfade秒数", "End Crossfade duration"), current.endCrossfadeDurationMs, .1, 60, 1000, 3, QStringLiteral(" s"));

  auto *repeat = new QSpinBox;
  repeat->setRange(0, 9999); repeat->setSpecialValueText(tr("無限", "Infinite"));
  const auto repeatValue = valueOr(QStringLiteral("repeatCount"), current.repeatCount ? QJsonValue(current.repeatCount) : QJsonValue(QJsonValue::Null));
  repeat->setValue(repeatValue.isNull() ? 0 : repeatValue.toInt());
  auto saveRepeat = addField(QStringLiteral("repeatCount"), tr("REPEAT回数", "REPEAT count"), repeat, [repeat] {
    return repeat->value() ? QJsonValue(repeat->value()) : QJsonValue(QJsonValue::Null);
  });
  QObject::connect(repeat, &QSpinBox::valueChanged, q, [saveRepeat](int) { saveRepeat(); });
  enumField("repeatEndMode", tr("REPEAT終了後", "After REPEAT"), {QStringLiteral("stop"), QStringLiteral("pause"), QStringLiteral("next"), QStringLiteral("goto")}, int(current.repeatEndMode));

  auto *gotoTarget = new QComboBox;
  gotoTarget->setMinimumContentsLength(12);
  gotoTarget->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  gotoTarget->addItem(tr("未指定", "Unset"), QString());
  for (const auto &playlist : model->playlists())
    for (int row = 0; row < model->itemCount(playlist.number); ++row) {
      const quint64 id = model->itemIdAt(playlist.number, row);
      gotoTarget->addItem(itemLabel(model, id, japanese), QString::number(id));
    }
  const auto gotoValue = valueOr(QStringLiteral("gotoTargetItemId"), current.gotoTargetItemId ? QJsonValue(QString::number(current.gotoTargetItemId)) : QJsonValue(QJsonValue::Null));
  const QString gotoId = gotoValue.isNull() ? QString() : gotoValue.toString();
  int gotoIndex = gotoTarget->findData(gotoId);
  if (gotoIndex < 0) {
    gotoTarget->addItem(tr("参照切れ: %1", "Missing item: %1").arg(gotoId), gotoId);
    gotoIndex = gotoTarget->count() - 1;
  }
  gotoTarget->setCurrentIndex(gotoIndex);
  auto saveGoto = addField(QStringLiteral("gotoTargetItemId"), tr("GOTO先", "GOTO target"), gotoTarget, [gotoTarget] {
    const QString id = gotoTarget->currentData().toString();
    return id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(id);
  });
  QObject::connect(gotoTarget, &QComboBox::currentIndexChanged, q, [saveGoto](int) { saveGoto(); });

  numberField("inPointMs", QStringLiteral("In"), current.inPointMs, 0, 604800, 1000, 3, QStringLiteral(" s"));
  numberField("outPointMs", QStringLiteral("Out"), current.outPointMs.value_or(-1000), 0, 604800, 1000, 3, QStringLiteral(" s"), true);
  numberField("nominalVolumeTenthsDb", tr("規定音量", "Nominal volume"), current.nominalVolumeTenthsDb, -96, 6, 10, 1, QStringLiteral(" dB"));
  numberField("nominalAudioDelayFrames", tr("規定音声ディレイ", "Nominal audio delay"), current.nominalAudioDelayFrames, 0, 6, 1, 0, tr(" フレーム", " frames"));
  numberField("nominalEqLowTenthsDb", QStringLiteral("EQ LOW"), current.nominalEqTenthsDb[0], -15, 15, 10, 1, QStringLiteral(" dB"));
  numberField("nominalEqMidTenthsDb", QStringLiteral("EQ MID"), current.nominalEqTenthsDb[1], -15, 15, 10, 1, QStringLiteral(" dB"));
  numberField("nominalEqHighTenthsDb", QStringLiteral("EQ HIGH"), current.nominalEqTenthsDb[2], -15, 15, 10, 1, QStringLiteral(" dB"));

  auto *apply = new QPushButton(tr("項目へ今すぐ適用", "Apply to item now"));
  apply->setObjectName(QStringLiteral("macroApplyItemSettingsButton"));
  apply->setEnabled(target.has_value());
  form->addRow(apply);
  auto *applyHint = new QLabel(tr("この操作はプレイリストを今すぐ変更します。ステップ実行時にも同じ差分を適用します。", "This changes the playlist immediately. The same patch is also applied when this step runs."));
  applyHint->setWordWrap(true); form->addRow(applyHint);
  QObject::connect(apply, &QPushButton::clicked, q, [this] {
    const auto *s = selectedStep();
    if (!s || runner->isActive()) return;
    if (s->documentId != model->documentId()) {
      showMessage(tr("別文書の参照です。対象を選び直してください。", "The target belongs to another document. Select it again."));
      return;
    }
    QString error;
    showMessage(runner->applyItemSettings(s->itemId, s->settingsPatch, &error)
        ? tr("項目設定を適用しました。", "Item settings applied.") : error);
  });
}

void MacroEditorWindow::Private::check(bool start) {
  const int index = macroIndex();
  if (index < 0 || runner->isActive()) return;
  if (!persist()) return;
  const QStringList errors = runner->validate(definitions[index]);
  if (!errors.isEmpty()) {
    diagnostics = errors.join(QLatin1Char('\n'));
    showMessage(tr("実行前チェック: %1件の問題があります。下のログを確認してください。", "Preflight: %1 issues found. See the log below.").arg(errors.size()));
    refreshStatus();
    return;
  }
  QStringList dates;
  for (int i = 0; i < definitions[index].steps.size(); ++i) {
    const auto &step = definitions[index].steps[i];
    if (step.enabled && step.type == MacroStepType::WaitUntil)
      dates.append(tr("ステップ %1: %2", "Step %1: %2").arg(i + 1).arg(dateText(step.scheduledAt)));
  }
  diagnostics = dates.join(QLatin1Char('\n'));
  showMessage(tr("実行前チェック: 問題はありません。", "Preflight: no issues found."));
  if (start) {
    diagnostics.clear();
    if (!runner->start(definitions[index])) showMessage(runner->statusText());
  }
  refreshStatus();
}

void MacroEditorWindow::Private::refreshStatus() {
  const bool active = runner->isActive();
  const auto state = runner->state();
  macroBar->setEnabled(!active);
  editArea->setEnabled(!active);
  if (details->widget()) details->widget()->setEnabled(!active);
  tableModel->locked = active;
  tableModel->activeMacroId = runner->activeMacroId();
  tableModel->activeRow = runner->currentStepIndex();
  tableModel->runnerState = state;
  QString status;
  switch (state) {
  case MacroRunner::State::Stopped: status = tr("停止", "Stopped"); break;
  case MacroRunner::State::Running: status = tr("実行中", "Running"); break;
  case MacroRunner::State::Waiting: status = tr("待機中", "Waiting"); break;
  case MacroRunner::State::Paused: status = tr("一時停止", "Paused"); break;
  case MacroRunner::State::Completed: status = tr("完了", "Completed"); break;
  case MacroRunner::State::Error: status = tr("エラー", "Error"); break;
  }
  tableModel->rowStatus = status;
  tableModel->refresh();
  runButton->setEnabled(!active && macroIndex() >= 0 && !tableModel->steps.isEmpty());
  pauseButton->setEnabled(active && state != MacroRunner::State::Paused);
  resumeButton->setEnabled(active && state == MacroRunner::State::Paused);
  stopButton->setEnabled(active);
  stateLabel->setText(tr("%1 / ステップ %2 / %3", "%1 / Step %2 / %3")
      .arg(status).arg((std::max)(0, runner->currentStepIndex() + 1)).arg(runner->statusText()));
  clockLabel->setText(tr("時計: %1 / %2 / タイムゾーン: %3", "Clock: %1 / %2 / Time zone: %3")
      .arg(dateText(now()))
      .arg(clock && clock->isNetworkSynchronized() ? tr("ネットワーク同期済み", "Network synchronized")
                                                  : tr("ローカル時計", "Local clock"))
      .arg(QString::fromUtf8(now().timeZone().id())));
  QString next;
  const int first = active ? (std::max)(0, runner->currentStepIndex()) : 0;
  for (int i = first; i < tableModel->steps.size(); ++i) {
    const auto &step = tableModel->steps[i];
    if (step.enabled && step.type == MacroStepType::WaitUntil) { next = dateText(step.scheduledAt); break; }
  }
  scheduleLabel->setText(tr("次の予定: %1 / 残り待機: %2 s", "Next schedule: %1 / Wait remaining: %2 s")
      .arg(next.isEmpty() ? tr("なし", "None") : next)
      .arg(QLocale().toString((std::max)(qint64(0), runner->remainingMs()) / 1000.0, 'f', 1)));
  const QString entries = (diagnostics.isEmpty() ? QString() : diagnostics + QLatin1Char('\n')) +
                           runner->logLines().join(QLatin1Char('\n'));
  if (log->toPlainText() != entries) {
    log->setPlainText(entries);
    auto cursor = log->textCursor(); cursor.movePosition(QTextCursor::End); log->setTextCursor(cursor);
  }
}

MacroEditorWindow::MacroEditorWindow(PlaylistModel *model, MacroRunner *runner,
                                     QWidget *parent)
    : QWidget(parent, Qt::Window), d_(std::make_unique<Private>()) {
  Q_ASSERT(model && runner);
  d_->q = this; d_->model = model; d_->runner = runner;
  setObjectName(QStringLiteral("macroEditorWindow"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setAttribute(Qt::WA_StyledBackground, true);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  resize(vtrpon::scaleSize(1380, 840));
  setMinimumSize(vtrpon::scaleSize(980, 620));
  d_->build();
  connect(model, &PlaylistModel::macrosChanged, this, [this] { d_->refreshMacros(); });
  connect(model, &PlaylistModel::modelReset, this, [this] {
    d_->refreshMacros(); d_->refreshPlaylists(); d_->refreshDetails();
  });
  connect(model, &PlaylistModel::playlistAdded, this, [this] { d_->refreshPlaylists(); });
  connect(model, &PlaylistModel::playlistRemoved, this, [this] { d_->refreshPlaylists(); });
  connect(model, &PlaylistModel::playlistOrderChanged, this, [this] { d_->refreshPlaylists(); });
  connect(model, &PlaylistModel::playlistNameChanged, this, [this] { d_->refreshPlaylists(); });
  connect(model, &PlaylistModel::countChanged, this, [this] {
    d_->refreshSources(); d_->tableModel->refresh();
    QTimer::singleShot(0, this, [this] { d_->refreshDetails(); });
  });
  connect(model, &PlaylistModel::itemChanged, this, [this] { d_->refreshSources(); d_->tableModel->refresh(); });
  connect(model, &PlaylistModel::itemMoved, this, [this] { d_->refreshSources(); d_->tableModel->refresh(); });
  connect(model, &PlaylistModel::itemMovedToPlaylist, this, [this] { d_->refreshSources(); d_->tableModel->refresh(); });
  connect(runner, &MacroRunner::stateChanged, this, [this] { d_->refreshStatus(); });
  connect(runner, &MacroRunner::progressChanged, this, [this] { d_->refreshStatus(); });
  connect(runner, &MacroRunner::logAdded, this, [this] { d_->refreshStatus(); });
  auto *timer = new QTimer(this);
  timer->setInterval(250);
  connect(timer, &QTimer::timeout, this, [this] { if (isVisible()) d_->refreshStatus(); });
  timer->start();
}

MacroEditorWindow::~MacroEditorWindow() = default;

void MacroEditorWindow::setLanguageManager(vtrpon::UiLanguageManager *manager) {
  if (d_->language) disconnect(d_->language, nullptr, this, nullptr);
  d_->language = manager;
  if (manager) {
    d_->japanese = manager->currentLanguage() == vtrpon::UiLanguage::Japanese;
    connect(manager, &vtrpon::UiLanguageManager::languageChanged, this,
            [this](vtrpon::UiLanguage language) {
              d_->japanese = language == vtrpon::UiLanguage::Japanese;
              d_->build();
            });
  }
  d_->build();
}

void MacroEditorWindow::setClock(vtrpon::NetworkTimeClock *clock) {
  d_->clock = clock;
  d_->helperTime->setDateTime(d_->now().addSecs(60));
  d_->refreshStatus();
}

void MacroEditorWindow::closeEvent(QCloseEvent *event) {
  hide();
  event->ignore();
}
