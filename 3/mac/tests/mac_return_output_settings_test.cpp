#include "controller_settings_dialog.h"
#include "controller_settings.h"
#include "switcher_control.h"
#include "virtual_camera/output.h"
#include "skins/controller_skin.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QListWidget>
#include <QTranslator>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <cstdio>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"Settings line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int runMacReturnOutputSettingsRegression() {
  auto &application=*qobject_cast<QApplication *>(QCoreApplication::instance());
    QTranslator japanese; CHECK(japanese.load(QCoreApplication::applicationDirPath() + "/vtrpon_ja.qm"));
    application.installTranslator(&japanese);
    vtrpon::reloadVirtualCameraEnabled();
    ControllerSettingsDialog dialog(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *source = dialog.findChild<QComboBox *>("virtualCameraOutputSource");
    auto *camera = dialog.findChild<QCheckBox *>("virtualCameraEnabled");
    auto *returns = dialog.findChild<QCheckBox *>("operatorMonitorVirtualCameraOutput");
    CHECK(source && camera && returns);
    returns->setChecked(true);
    const auto waitFor = [&](auto predicate) {
      QElapsedTimer timer; timer.start();
      while (!predicate() && timer.elapsed() < 2000) { application.processEvents(); QThread::msleep(10); }
      return predicate();
    };
    CHECK(waitFor([&] { return source->currentIndex() == 1 && camera->isChecked(); }));
    source->setCurrentIndex(0); CHECK(!returns->isChecked() && camera->isChecked());
    source->setCurrentIndex(1); CHECK(returns->isChecked());
    camera->setChecked(false); CHECK(!returns->isChecked());
    returns->setChecked(true); CHECK(waitFor([&] { return camera->isChecked() && source->currentIndex() == 1; }));
    const auto captures = qEnvironmentVariable("VTRPON_RETURN_TEST_CAPTURES");
    if (!captures.isEmpty()) {
      CHECK(QDir().mkpath(captures));
      auto *navigation = dialog.findChild<QListWidget *>("settingsNavigationList"); CHECK(navigation);
      for (const QString &skin : {QStringLiteral("hardware"), QStringLiteral("material-dark"), QStringLiteral("vtr-pon-clasic")}) {
        CHECK(vtrpon::applyControllerSkin(application, skin));
        for (const QString &page : {QStringLiteral("operator_monitor"), QStringLiteral("virtual_camera")}) {
          for (int i = 0; i < navigation->count(); ++i)
            if (navigation->item(i)->data(Qt::UserRole).toString() == page) navigation->setCurrentRow(i);
          application.processEvents();
          CHECK(dialog.grab().save(QDir(captures).filePath(skin + "-" + page + ".png")));
        }
      }
    }
    auto *model=dialog.findChild<QComboBox *>("rolandModelComboBox");
    auto *transport=dialog.findChild<QComboBox *>("rolandTransportComboBox");
    auto *lan=dialog.findChild<QWidget *>("rolandLanSettings");
    auto *fade=dialog.findChild<QCheckBox *>("rolandFtbFollowsProgramCheckBox");
    CHECK(model && transport && lan && fade);
    for(const auto &profile:vtrpon::rolandSwitcherProfiles()) if(vtrpon::rolandHasUsbMidiChoice(profile.model)) {
      model->setCurrentIndex(model->findData(static_cast<int>(profile.model)));
      CHECK(!transport->parentWidget()->isHidden() && transport->currentIndex()==0 && !lan->isHidden());
      if(profile.fade) fade->setChecked(true);
      const auto before=vtrpon::activeRolandSettings(dialog.switcherControlSettings());
      transport->setCurrentIndex(1);
      CHECK(lan->isHidden() && !fade->isEnabled() && !fade->isChecked());
      CHECK(vtrpon::activeRolandSettings(vtrpon::loadSwitcherControlSettings()).useUsbMidi);
      dialog.reloadSavedSettings(); CHECK(transport->currentIndex()==1);
      transport->setCurrentIndex(0);
      CHECK(!lan->isHidden() && vtrpon::activeRolandSettings(dialog.switcherControlSettings())==before);
    }
    camera->setChecked(false);
    std::puts("PASS: actual Mac settings dialog links camera/return and preserves per-model LAN/USB and FTB preferences");
    return 0;
}
