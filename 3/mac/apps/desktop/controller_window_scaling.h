#pragma once

#include <QObject>
#include <memory>

class QWidget;

namespace vtrpon {

// Install after the Controller has been constructed and shown at its normal
// display density. Resizes existing widgets, including native video surfaces.
class ControllerWindowScaling final : public QObject {
public:
  explicit ControllerWindowScaling(QWidget *window);
  ~ControllerWindowScaling() override;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vtrpon
