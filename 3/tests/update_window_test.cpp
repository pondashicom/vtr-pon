#include "update_notification.h"

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>

#include <iostream>

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; \
    return 1; \
  } \
} while (false)

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  bool japanese = true;
  vtrpon::UpdateWindow window(QStringLiteral("3.0.0-beta.1"),
                              [&japanese] { return japanese; });
  CHECK(window.windowModality() == Qt::NonModal);
  auto *notes = window.findChild<QPlainTextEdit *>(QStringLiteral("updateReleaseNotes"));
  auto *download = window.findChild<QPushButton *>(QStringLiteral("updateDownloadButton"));
  CHECK(notes && download);
  const vtrpon::UpdateRelease release{QStringLiteral("3.0.0-beta.2"),
                                     QStringLiteral("2026-09-25"),
                                     {QStringLiteral("日本語の変更点")},
                                     {QStringLiteral("English release note")}};
  window.setResult(vtrpon::UpdateCheckStatus::Available, release);
  CHECK(window.windowTitle().contains(QStringLiteral("更新")));
  CHECK(notes->toPlainText().contains(QStringLiteral("日本語の変更点")));
  CHECK(!download->isHidden());
  japanese = false;
  window.retranslate();
  CHECK(notes->toPlainText().contains(QStringLiteral("English release note")));
  japanese = true;
  window.setResult(vtrpon::UpdateCheckStatus::Current, std::nullopt);
  CHECK(window.findChild<QLabel *>(QStringLiteral("updateHeading"))->text() ==
        QStringLiteral("最新のバージョンです"));
  CHECK(download->isHidden());
  CHECK(notes->isHidden());
  japanese = false;
  window.setResult(vtrpon::UpdateCheckStatus::Failed, std::nullopt);
  CHECK(!notes->isHidden());
  CHECK(notes->toPlainText().contains(QStringLiteral("network")));
  return 0;
}
