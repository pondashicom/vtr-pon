#pragma once

#include "switcher_control.h"

#include <QLibrary>
#include <QString>
#include <QTimer>

#include <cstdint>

struct VtrPonSwitcherAddonApiV1;

namespace vtrpon {

class AtemAddonSwitcher final : public SwitcherDevice {
  Q_OBJECT

public:
  explicit AtemAddonSwitcher(bool receiveProgramInputChanges,
                             QObject *parent = nullptr);
  ~AtemAddonSwitcher() override;

  static QString addonFilePath();
  static bool addonInstalled();

  bool open(QString *errorDetail = nullptr) override;
  void close() override;
  bool setProgramInput(int sourceNumber,
                       QString *errorDetail = nullptr) override;
  bool setFadeToBlack(bool enabled, QString *errorDetail = nullptr) override;
  QString connectionDetail() const override;

private:
  bool loadAddon(QString *errorDetail);
  bool pollProgramInput(QString *errorDetail = nullptr);
  void failConnection(const QString &detail);

  bool receiveProgramInputChanges_ = false;
  QLibrary library_;
  const VtrPonSwitcherAddonApiV1 *api_ = nullptr;
  void *instance_ = nullptr;
  QTimer pollTimer_;
  int64_t lastProgramInputNumber_ = -1;
  QString productName_;
};

} // namespace vtrpon
