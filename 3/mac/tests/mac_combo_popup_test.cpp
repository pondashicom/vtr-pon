#include "mac_widget_style.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QScreen>
#include <QScrollBar>
#include <QThread>
#include <QWheelEvent>
#include <iostream>

#define CHECK(condition, message) do { if (!(condition)) { std::cerr << "FAIL: " << message << '\n'; return 1; } } while (false)
void pump(int ms) {
  QElapsedTimer clock; clock.start();
  while (clock.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}
QString read(const QString &path) {
  QFile file(QStringLiteral(":/vtrpon") + path);
  return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  vtrpon::installMacComboPopupStyle(app);
  const auto screen = app.primaryScreen()->availableGeometry();
  for (const char *skin : {"hardware", "material-dark", "vtr-pon-clasic"}) {
    QString stylesheet;
    if (QString::fromLatin1(skin) == "vtr-pon-clasic")
      stylesheet = read("/apps/desktop/skins/material-dark/controller.qss");
    stylesheet += read(QString("/apps/desktop/skins/%1/controller.qss").arg(skin));
    CHECK(!stylesheet.isEmpty(), "production skin stylesheet loads");
    app.setStyleSheet(stylesheet);
    for (bool settings : {false, true}) {
      QDialog window;
      window.setObjectName("controllerSettingsDialog");
      if (settings) {
        const auto settingsStyle = read("/apps/desktop/settings.qss");
        CHECK(!settingsStyle.isEmpty(), "production settings stylesheet loads");
        window.setStyleSheet(settingsStyle);
      }
      window.resize(350, 100);
      window.move(screen.left() + 30, screen.bottom() - 110);
      QComboBox combo(&window);
      combo.setObjectName(settings ? "languageComboBox" : "inputUvcDeviceComboBox");
      combo.setGeometry(10, 40, 310, 32);
      for (int i = 0; i < 18; ++i) combo.addItem(QString("Device %1").arg(i), i);
      window.show(); pump(40);
      CHECK(!combo.style()->styleHint(QStyle::SH_ComboBox_Popup, nullptr, &combo), "list popup style remains active through skin/local stylesheet");
      combo.showPopup(); pump(80);
      auto *view = combo.view();
      CHECK(view->isVisible(), "popup opens");
      int visible = 0;
      for (int i = 0; i < combo.count(); ++i) {
        const auto rect = view->visualRect(combo.model()->index(i, 0));
        if (!rect.isEmpty() && view->viewport()->rect().contains(rect)) ++visible;
      }
      CHECK(visible >= 8, "bottom-edge popup shows at least eight complete rows");
      CHECK(view->window()->geometry().top() < combo.mapToGlobal(QPoint()).y(), "bottom-edge popup opens above its control");
      CHECK(view->verticalScrollBar()->maximum() > 0, "long lists remain scrollable");
      const auto firstBefore = view->indexAt(QPoint(5, 5)).row();
      QWheelEvent wheel(QPointF(5, 5), view->viewport()->mapToGlobal(QPoint(5, 5)),
                        QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
      QApplication::sendEvent(view->viewport(), &wheel); pump(20);
      const auto firstAfter = view->indexAt(QPoint(5, 5)).row();
      CHECK(firstAfter > firstBefore && firstAfter - firstBefore < visible,
            "one wheel step scrolls a few choices without skipping the visible page");
      view->setCurrentIndex(combo.model()->index(15, 0));
      view->scrollTo(view->currentIndex());
      CHECK(view->viewport()->rect().intersects(view->visualRect(view->currentIndex())), "later choices scroll into view");
      QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(view, &enter); pump(20);
      CHECK(combo.currentData().toInt() == 15, "keyboard selection retains item identity");
      combo.hidePopup();
      std::cout << skin << (settings ? " settings" : " controller") << " visible_rows=" << visible << " PASS\n";
    }
  }
  return 0;
}
