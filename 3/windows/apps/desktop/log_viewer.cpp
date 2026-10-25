#include "application_log.h"
#include "background_io_queue.h"
#include <QPointer>
#include "language_manager.h"
#include "language_text.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

namespace vtrpon {
void installLogViewer(QMenu *menu, QWidget *window,
                      UiLanguageManager *languageManager, BackgroundIoQueue *fileIo) {
  auto *show = menu->addAction(QString());
  show->setObjectName("showLogAction");
  auto *debug = menu->addAction(QString());
  debug->setObjectName("debugModeAction");
  debug->setCheckable(true);
  debug->setChecked(diagnosticLogging());
  auto *viewer = new QWidget(window, Qt::Window);
  viewer->setObjectName("logViewer");
  if (!fileIo) fileIo = new BackgroundIoQueue(viewer);
  viewer->resize(1000, 550);
  auto *layout = new QVBoxLayout(viewer);
  auto *row = new QHBoxLayout;
  auto *search = new QLineEdit(viewer);
  search->setObjectName("logSearch");
  auto *level = new QComboBox(viewer);
  level->setObjectName("logLevelFilter");
  level->addItems({QString(), QString()});
  auto *follow = new QCheckBox(viewer);
  follow->setChecked(true);
  row->addWidget(search, 1);
  row->addWidget(level);
  row->addWidget(follow);
  layout->addLayout(row);
  auto *text = new QPlainTextEdit(viewer);
  text->setObjectName("logText");
  text->setReadOnly(true);
  text->setMaximumBlockCount(2000);
  text->setLineWrapMode(QPlainTextEdit::NoWrap);
  layout->addWidget(text);
  auto *status = new QLabel(viewer);
  status->setWordWrap(true);
  status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(status);
  auto *actionStatus = new QLabel(viewer);
  actionStatus->setWordWrap(true);
  layout->addWidget(actionStatus);
  auto *buttons = new QHBoxLayout;
  auto *save = new QPushButton(viewer);
  auto *folder = new QPushButton(viewer);
  buttons->addStretch();
  buttons->addWidget(save);
  buttons->addWidget(folder);
  layout->addLayout(buttons);
  auto *timer = new QTimer(viewer);
  timer->setInterval(500);
  auto snapshot = std::make_shared<QStringList>();
  auto previousSnapshot = std::make_shared<QStringList>();
  auto previousFilter = std::make_shared<QString>();
  const auto refresh = [=] {
    if (!viewer->isVisible())
      return;
    const QString error = applicationLogError();
    status->setText(
        error.isEmpty()
            ? uiText("logs.retention").arg(applicationLogDirectory())
            : uiText("logs.failed").arg(error));
    if (follow->isChecked())
      *snapshot = applicationLogSnapshot();
    const auto filterKey =
        QString::number(level->currentIndex()) + ":" + search->text();
    if (*snapshot == *previousSnapshot && filterKey == *previousFilter)
      return;
    *previousSnapshot = *snapshot;
    *previousFilter = filterKey;
    QStringList lines;
    for (const QString &record : *snapshot) {
      const auto obj = QJsonDocument::fromJson(record.toUtf8()).object();
      const QString severity = obj["level"].toString();
      if (level->currentIndex() == 1 && severity != "WARN" &&
          severity != "ERROR")
        continue;
      const auto line =
          QStringLiteral("%1 [%2] %3  %4")
              .arg(obj["time"].toString(), severity, obj["source"].toString(),
                   obj["message"].toString());
      if (line.contains(search->text(), Qt::CaseInsensitive))
        lines.append(line);
    }
    const QString content = lines.join('\n');
    if (text->toPlainText() != content) {
      text->setPlainText(content);
      text->verticalScrollBar()->setValue(text->verticalScrollBar()->maximum());
    }
  };
  QObject::connect(timer, &QTimer::timeout, viewer, refresh);
  QObject::connect(search, &QLineEdit::textChanged, viewer, refresh);
  QObject::connect(level, &QComboBox::currentIndexChanged, viewer, refresh);
  QObject::connect(follow, &QCheckBox::toggled, viewer, refresh);
  timer->start();
  QObject::connect(show, &QAction::triggered, viewer, [=] {
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
    refresh();
  });
  QObject::connect(debug, &QAction::toggled, viewer,
                   [](bool enabled) { setDiagnosticLogging(enabled); });
  QObject::connect(folder, &QPushButton::clicked, viewer, [=] {
    if (!QDesktopServices::openUrl(
            QUrl::fromLocalFile(applicationLogDirectory())))
      actionStatus->setText(
          uiText("logs.failed").arg(applicationLogDirectory()));
  });
  QObject::connect(save, &QPushButton::clicked, viewer, [=] {
    const QString content = text->toPlainText();
    const QString path = QFileDialog::getSaveFileName(viewer, uiText("logs.save"), "vtrpon-log.txt", "Text (*.txt)");
    if (path.isEmpty()) return;
    const auto error = std::make_shared<QString>();
    const QPointer<QLabel> resultLabel(actionStatus);
    const QPointer<QPushButton> saveButton(save);
    save->setEnabled(false);
    if (!fileIo->enqueue([content, path, error] {
      const auto bytes = content.toUtf8();
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        *error = file.errorString();
    }, [resultLabel, saveButton, path, error](const QString &workerError) {
      if (saveButton) saveButton->setEnabled(true);
      const auto detail = workerError.isEmpty() ? *error : workerError;
      if (!detail.isEmpty()) writeApplicationLog("ERROR", "LOGGER", "Export failed: " + detail);
      if (resultLabel) resultLabel->setText(detail.isEmpty() ? uiText("logs.saved").arg(path)
                                                           : uiText("logs.failed").arg(detail));
    })) save->setEnabled(true);
  });
  const auto translate = [=] {
    show->setText(uiText("logs.show"));
    debug->setText(uiText("logs.debug"));
    viewer->setWindowTitle(uiText("logs.show"));
    search->setPlaceholderText(uiText("logs.search"));
    search->setAccessibleName(uiText("logs.search"));
    level->setItemText(0, uiText("logs.all"));
    level->setItemText(1, uiText("logs.warnings"));
    follow->setText(uiText("logs.follow"));
    save->setText(uiText("logs.save"));
    folder->setText(uiText("logs.folder"));
  };
  translate();
  if (languageManager)
    QObject::connect(languageManager, &UiLanguageManager::languageChanged,
                     viewer, translate);
}
} // namespace vtrpon
