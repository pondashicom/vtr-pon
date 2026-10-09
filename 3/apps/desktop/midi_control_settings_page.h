#pragma once
#include "midi_control_settings.h"
#include <QWidget>
#include <QPointer>
#include <QElapsedTimer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
namespace vtrpon {
class MidiControlInput;
class MidiControlSettingsPage final : public QWidget {
  Q_OBJECT
public:
  explicit MidiControlSettingsPage(QWidget *parent = nullptr);
  void setInput(MidiControlInput *input);
  void reload();
signals:
  void saved();
protected:
  void changeEvent(QEvent *event) override;
  void hideEvent(QHideEvent *event) override;
private:
  void retranslate();
  void refreshDevices();
  void updateConnection();
  void selectRow();
  QStringList editBinding(bool replaceConflicts = false);
  void updateRows();
  void save();
  QString selectedId() const;
  QPointer<MidiControlInput> input_;
  QStringList ids_ = midiControl::actionIds();
  QJsonObject bindings_;
  QTimer *receivedDisplayTimer_;
  QString lastReceivedText_;
  QString learningType_;
  QElapsedTimer noteDisplayHold_;
  bool updating_ = false, learning_ = false;
  QLabel *title_, *description_, *deviceLabel_, *connection_, *received_, *status_, *rangeLabel_;
  QCheckBox *enabled_, *pickup_, *reverse_;
  QComboBox *device_, *type_, *mode_;
  QSpinBox *channel_, *number_;
  QDoubleSpinBox *minimum_, *maximum_;
  QTableWidget *table_;
  QPushButton *refresh_, *learn_, *remove_, *save_;
};
} // namespace vtrpon
