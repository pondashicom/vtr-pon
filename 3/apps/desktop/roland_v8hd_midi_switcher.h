#pragma once

#include "switcher_control.h"

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <atomic>
#include <deque>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <QWinEventNotifier>
#endif

namespace vtrpon {

class RolandV8HdMidiSwitcher final : public SwitcherDevice {
  Q_OBJECT

public:
  explicit RolandV8HdMidiSwitcher(bool receiveProgramInputChanges,
                                  QObject *parent = nullptr);
  ~RolandV8HdMidiSwitcher() override;

  bool open(QString *errorDetail = nullptr) override;
  void close() override;
  bool setProgramInput(int sourceNumber,
                       QString *errorDetail = nullptr) override;
  bool setFadeToBlack(bool enabled, QString *errorDetail = nullptr) override;

private:
#ifdef _WIN32
  static void CALLBACK midiInCallback(HMIDIIN handle, UINT message,
                                      DWORD_PTR instance, DWORD_PTR param1,
                                      DWORD_PTR param2);
  void handleShortMessage(DWORD message);
  bool prepareInputBuffer(QString *errorDetail);
  bool sendShortMessage(DWORD message, QString *errorDetail);
  bool sendSysEx(const QByteArray &message, QString *errorDetail);
  bool queueOutput(QByteArray message, DWORD shortMessage, QString *errorDetail);
  void pumpOutput();
  void completeOutput();
  void outputTimedOut();
#endif
  void startOutputFadeRamp(bool enabled);
  void sendNextOutputFadeStep();
  QByteArray makeDataSetMessage(quint8 address1, quint8 address2,
                                quint8 address3,
                                const QByteArray &data) const;
  QByteArray makeDataRequestMessage(quint8 address1, quint8 address2,
                                    quint8 address3, quint8 size1,
                                    quint8 size2, quint8 size3) const;
  void requestTally();
  void parseDataSetMessage(const QByteArray &message);
  QString midiErrorText(unsigned int result) const;

#ifdef _WIN32
  HMIDIOUT midiOut_ = nullptr;
  HMIDIIN midiIn_ = nullptr;
  MIDIHDR inputHeader_{};
  QByteArray inputBuffer_;
  std::atomic_bool closing_ = false;
  struct OutputMessage { QByteArray bytes; DWORD shortMessage = 0; MIDIHDR header{}; };
  std::deque<std::unique_ptr<OutputMessage>> outputQueue_;
  HANDLE outputEvent_ = nullptr;
  QWinEventNotifier *outputNotifier_ = nullptr;
  QTimer outputTimeout_;
  bool outputPending_ = false;
  bool outputFailed_ = false;
#endif
  bool receiveProgramInputChanges_ = false;
  QTimer tallyPollTimer_;
  QTimer outputFadeTimer_;
  int lastProgramInputNumber_ = 0;
  int outputFadeLevel_ = 0;
  int outputFadeTargetLevel_ = 0;
};

} // namespace vtrpon
