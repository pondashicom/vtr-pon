#pragma once

#include "test_pattern_store.h"
#include "language_text.h"
#include "settings_style.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <functional>

class TestPatternSettingsPage final : public QWidget {
public:
  explicit TestPatternSettingsPage(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("testPatternSettingsPage"));
    vtrpon::setSettingsRole(this, "page");
    auto *layout = new QVBoxLayout(this);
    title_ = new QLabel(this);
    vtrpon::setSettingsRole(title_, "pageTitle");
    description_ = new QLabel(this);
    vtrpon::setSettingsRole(description_, "description");
    description_->setWordWrap(true);
    layout->addWidget(title_);
    layout->addWidget(description_);
    layout = vtrpon::addSettingsGroup(layout, "menu.tools.test_pattern", true);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("testPatternList"));
    layout->addWidget(list_, 1);
    path_ = new QLabel(this);
    vtrpon::setSettingsRole(path_, "description");
    path_->setWordWrap(true);
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path_);
    auto *buttons = new QHBoxLayout;
    const char *ids[]{"test_patterns.add", "test_patterns.remove", "test_patterns.up",
                      "test_patterns.down", "test_patterns.restore"};
    for (const char *id : ids) {
      auto *button = new QPushButton(this);
      button->setProperty("textId", id);
      button->setObjectName(QString::fromLatin1(id));
      buttons_.append(button);
      buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    status_ = new QLabel(this);
    vtrpon::setSettingsRole(status_, "status");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    connect(buttons_[0], &QPushButton::clicked, this, [this] { add(); });
    connect(buttons_[1], &QPushButton::clicked, this, [this] {
      const int row = list_->currentRow();
      if (row < 0) return;
      auto entries = entries_;
      entries.removeAt(row);
      commit(entries, qMin(row, int(entries.size())-1));
    });
    connect(buttons_[2], &QPushButton::clicked, this, [this] { move(-1); });
    connect(buttons_[3], &QPushButton::clicked, this, [this] { move(1); });
    connect(buttons_[4], &QPushButton::clicked, this, [this] {
      auto entries = entries_;
      for (const auto &entry : vtrpon::testPatterns::defaults())
        if (!entries.contains(entry)) entries.append(entry);
      commit(entries, list_->currentRow());
    });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });
    reload();
    retranslate();
  }
  void retranslate() {
    vtrpon::retranslateSettingsGroups(this);
    title_->setText(vtrpon::uiText("menu.tools.test_pattern"));
    description_->setText(vtrpon::uiText("test_patterns.description"));
    list_->setAccessibleName(vtrpon::uiText("menu.tools.test_pattern"));
    for (auto *button : buttons_)
      button->setText(vtrpon::uiText(button->property("textId").toByteArray().constData()));
  }
  void reload() {
    entries_ = vtrpon::testPatterns::load();
    list_->clear();
    for (const auto &entry : entries_) {
      const auto path = vtrpon::testPatterns::path(entry);
      auto *item = new QListWidgetItem(QFileInfo(path).fileName(), list_);
      item->setToolTip(QDir::toNativeSeparators(path));
    }
    updateButtons();
  }
  std::function<void()> changed;
private:
  void updateButtons() {
    const int row = list_->currentRow();
    path_->setText(row >= 0 ? QDir::toNativeSeparators(
        vtrpon::testPatterns::path(entries_.at(row))) : QString());
    buttons_[1]->setEnabled(row >= 0);
    buttons_[2]->setEnabled(row > 0);
    buttons_[3]->setEnabled(row >= 0 && row+1 < list_->count());
  }
  void commit(const QStringList &entries, int selected) {
    if (!vtrpon::testPatterns::save(entries)) {
      status_->setText(vtrpon::uiText("test_patterns.save_failed"));
      return;
    }
    reload();
    list_->setCurrentRow(selected);
    status_->clear();
    if (changed) changed();
  }
  void move(int offset) {
    const int row = list_->currentRow(), target = row+offset;
    if (row < 0 || target < 0 || target >= entries_.size()) return;
    auto entries = entries_;
    entries.swapItemsAt(row, target);
    commit(entries, target);
  }
  void add() {
    const auto files = QFileDialog::getOpenFileNames(this,
        vtrpon::uiText("test_patterns.add"), {},
        vtrpon::uiText("test_patterns.filter"));
    if (files.isEmpty()) return;
    auto entries = entries_;
    QStringList rejected;
    for (const auto &file : files) {
      QFileInfo info(file);
      if (!info.isFile() || !info.isReadable() || !vtrpon::testPatterns::supported(file)) {
        rejected.append(info.fileName());
        continue;
      }
      const QString path = info.canonicalFilePath();
      bool duplicate = false;
      for (const auto &entry : entries)
        if (QFileInfo(vtrpon::testPatterns::path(entry)).canonicalFilePath().compare(
                path, Qt::CaseInsensitive) == 0) duplicate = true;
      if (!duplicate) entries.append(path);
    }
    commit(entries, int(entries.size())-1);
    if (!rejected.isEmpty())
      status_->setText(vtrpon::uiText("test_patterns.rejected").arg(rejected.join(", ")));
  }
  QStringList entries_;
  QLabel *title_, *description_, *status_, *path_;
  QListWidget *list_;
  QList<QPushButton *> buttons_;
};
