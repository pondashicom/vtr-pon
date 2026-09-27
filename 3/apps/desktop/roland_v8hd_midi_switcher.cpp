#include "roland_v8hd_midi_switcher.h"

#include <QMetaObject>

#include <algorithm>

namespace {

bool isRolandV8HdName(const QString &name) {
  const QString upper = name.toUpper();
  return upper.contains(QStringLiteral("V-8HD")) ||
         upper.contains(QStringLiteral("V8-HD")) ||
         upper.contains(QStringLiteral("V8HD"));
}

quint8 rolandChecksum(const QByteArray &addressAndData) {
  int sum = 0;
  for (const char byte : addressAndData) {
    sum += static_cast<unsigned char>(byte);
  }
  const int remainder = sum % 128;
  return static_cast<quint8>(remainder == 0 ? 0 : 128 - remainder);
}

} // namespace

namespace vtrpon {

RolandV8HdMidiSwitcher::RolandV8HdMidiSwitcher(
    bool receiveProgramInputChanges, QObject *parent)
    : SwitcherDevice(parent),
      receiveProgramInputChanges_(receiveProgramInputChanges) {
  tallyPollTimer_.setInterval(250);
  connect(&tallyPollTimer_, &QTimer::timeout, this,
          &RolandV8HdMidiSwitcher::requestTally);
  outputFadeTimer_.setInterval(40);
  connect(&outputFadeTimer_, &QTimer::timeout, this,
          &RolandV8HdMidiSwitcher::sendNextOutputFadeStep);
#ifdef _WIN32
  outputTimeout_.setSingleShot(true);
  outputTimeout_.setInterval(2000);
  connect(&outputTimeout_, &QTimer::timeout, this, &RolandV8HdMidiSwitcher::outputTimedOut);
#endif
}

RolandV8HdMidiSwitcher::~RolandV8HdMidiSwitcher() { close(); }

bool RolandV8HdMidiSwitcher::open(QString *errorDetail) {
#ifdef _WIN32
  close();
  closing_ = false;

  UINT outDeviceId = MIDI_MAPPER;
  bool foundOut = false;
  const UINT outDeviceCount = midiOutGetNumDevs();
  for (UINT index = 0; index < outDeviceCount; ++index) {
    MIDIOUTCAPSW caps{};
    if (midiOutGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
        isRolandV8HdName(QString::fromWCharArray(caps.szPname))) {
      outDeviceId = index;
      foundOut = true;
      break;
    }
  }
  if (!foundOut) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("Roland V-8HD MIDI output was not found.");
    }
    return false;
  }

  outputEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!outputEvent_) {
    if (errorDetail) *errorDetail = QStringLiteral("Could not create the MIDI completion event.");
    return false;
  }
  MMRESULT result = midiOutOpen(&midiOut_, outDeviceId,
      reinterpret_cast<DWORD_PTR>(outputEvent_), 0, CALLBACK_EVENT);
  if (result != MMSYSERR_NOERROR) {
    if (errorDetail) {
      *errorDetail = midiErrorText(result);
    }
    midiOut_ = nullptr;
    CloseHandle(outputEvent_);
    outputEvent_ = nullptr;
    return false;
  }
  outputFailed_ = false;
  outputNotifier_ = new QWinEventNotifier(outputEvent_, this);
  connect(outputNotifier_, &QWinEventNotifier::activated, this, [this] { completeOutput(); });

  if (receiveProgramInputChanges_) {
    const UINT inDeviceCount = midiInGetNumDevs();
    for (UINT index = 0; index < inDeviceCount; ++index) {
      MIDIINCAPSW caps{};
      if (midiInGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
          isRolandV8HdName(QString::fromWCharArray(caps.szPname))) {
        result = midiInOpen(&midiIn_, index,
                            reinterpret_cast<DWORD_PTR>(&midiInCallback),
                            reinterpret_cast<DWORD_PTR>(this),
                            CALLBACK_FUNCTION);
        if (result == MMSYSERR_NOERROR) {
          break;
        }
        midiIn_ = nullptr;
      }
    }

    if (midiIn_) {
      if (!prepareInputBuffer(errorDetail)) {
        close();
        return false;
      }
      result = midiInStart(midiIn_);
      if (result != MMSYSERR_NOERROR) {
        if (errorDetail) {
          *errorDetail = midiErrorText(result);
        }
        close();
        return false;
      }
      requestTally();
      tallyPollTimer_.start();
    }
  }
  return true;
#else
  if (errorDetail) {
    *errorDetail = QStringLiteral("Roland V-8HD MIDI control is Windows-only.");
  }
  return false;
#endif
}

void RolandV8HdMidiSwitcher::close() {
#ifdef _WIN32
  closing_ = true;
  tallyPollTimer_.stop();
  outputFadeTimer_.stop();
  outputTimeout_.stop();
  if (outputNotifier_) {
    outputNotifier_->setEnabled(false);
    delete outputNotifier_;
    outputNotifier_ = nullptr;
  }
  if (midiIn_) {
    midiInStop(midiIn_);
    midiInReset(midiIn_);
    if (inputHeader_.lpData) {
      midiInUnprepareHeader(midiIn_, &inputHeader_, sizeof(inputHeader_));
    }
    midiInClose(midiIn_);
    midiIn_ = nullptr;
  }
  inputHeader_ = MIDIHDR{};
  inputBuffer_.clear();

  if (midiOut_) {
    midiOutReset(midiOut_);
    // A reset returns queued long messages through the completion event. Keep
    // the header, buffer and driver handle alive until it has relinquished them.
    // This runs on the switcher worker, never on the Controller thread.
    if (outputPending_) {
      auto &header = outputQueue_.front()->header;
      while (midiOutUnprepareHeader(midiOut_, &header, sizeof(header)) == MIDIERR_STILLPLAYING) {
        WaitForSingleObject(outputEvent_, INFINITE);
      }
      outputPending_ = false;
    }
    midiOutClose(midiOut_);
    midiOut_ = nullptr;
  }
  outputQueue_.clear();
  if (outputEvent_) { CloseHandle(outputEvent_); outputEvent_ = nullptr; }
  closing_ = false;
  lastProgramInputNumber_ = 0;
#endif
}

bool RolandV8HdMidiSwitcher::setProgramInput(int sourceNumber,
                                             QString *errorDetail) {
  if (sourceNumber < 1 || sourceNumber > 8) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("Roland V-8HD source must be 1-8.");
    }
    return false;
  }
  return sendSysEx(makeDataSetMessage(0x00, 0x12, 0x00,
                                      QByteArray(1, char(sourceNumber - 1))),
                   errorDetail);
}

bool RolandV8HdMidiSwitcher::setFadeToBlack(bool enabled,
                                            QString *errorDetail) {
#ifdef _WIN32
  if (enabled) {
    if (!sendSysEx(makeDataSetMessage(0x02, 0x01, 0x07,
                                      QByteArray(1, char(0x00))),
                   errorDetail)) {
      return false;
    }
  }
  startOutputFadeRamp(enabled);
  return true;
#else
  if (errorDetail) {
    *errorDetail = QStringLiteral("Roland V-8HD MIDI control is Windows-only.");
  }
  return false;
#endif
}

void RolandV8HdMidiSwitcher::startOutputFadeRamp(bool enabled) {
  outputFadeTargetLevel_ = enabled ? 0x3F : 0x00;
  sendNextOutputFadeStep();
  if (outputFadeLevel_ != outputFadeTargetLevel_) {
    outputFadeTimer_.start();
  }
}

void RolandV8HdMidiSwitcher::sendNextOutputFadeStep() {
  if (outputFadeLevel_ < outputFadeTargetLevel_) {
    outputFadeLevel_ = std::min(outputFadeLevel_ + 4, outputFadeTargetLevel_);
  } else if (outputFadeLevel_ > outputFadeTargetLevel_) {
    outputFadeLevel_ = std::max(outputFadeLevel_ - 4, outputFadeTargetLevel_);
  }

#ifdef _WIN32
  QString errorDetail;
  const DWORD status = 0xB0;
  const DWORD counterClockwiseController = 0x22;
  const DWORD clockwiseController = 0x23;
  if (!sendShortMessage(status | (clockwiseController << 8), &errorDetail) ||
      !sendShortMessage(status | (counterClockwiseController << 8) |
                            (static_cast<DWORD>(outputFadeLevel_) << 16),
                        &errorDetail)) {
    outputFadeTimer_.stop();
    emit errorOccurred(errorDetail);
    return;
  }
#endif

  if (outputFadeLevel_ == outputFadeTargetLevel_) {
    outputFadeTimer_.stop();
  }
}

QByteArray RolandV8HdMidiSwitcher::makeDataSetMessage(
    quint8 address1, quint8 address2, quint8 address3,
    const QByteArray &data) const {
  QByteArray addressAndData;
  addressAndData.append(char(address1));
  addressAndData.append(char(address2));
  addressAndData.append(char(address3));
  addressAndData.append(data);

  QByteArray message;
  message.append(char(0xF0));
  message.append(char(0x41));
  message.append(char(0x10));
  message.append(char(0x00));
  message.append(char(0x00));
  message.append(char(0x00));
  message.append(char(0x68));
  message.append(char(0x12));
  message.append(addressAndData);
  message.append(char(rolandChecksum(addressAndData)));
  message.append(char(0xF7));
  return message;
}

QByteArray RolandV8HdMidiSwitcher::makeDataRequestMessage(
    quint8 address1, quint8 address2, quint8 address3, quint8 size1,
    quint8 size2, quint8 size3) const {
  QByteArray addressAndSize;
  addressAndSize.append(char(address1));
  addressAndSize.append(char(address2));
  addressAndSize.append(char(address3));
  addressAndSize.append(char(size1));
  addressAndSize.append(char(size2));
  addressAndSize.append(char(size3));

  QByteArray message;
  message.append(char(0xF0));
  message.append(char(0x41));
  message.append(char(0x10));
  message.append(char(0x00));
  message.append(char(0x00));
  message.append(char(0x00));
  message.append(char(0x68));
  message.append(char(0x11));
  message.append(addressAndSize);
  message.append(char(rolandChecksum(addressAndSize)));
  message.append(char(0xF7));
  return message;
}

void RolandV8HdMidiSwitcher::requestTally() {
  if (!receiveProgramInputChanges_) {
    return;
  }
  QString errorDetail;
  if (!sendSysEx(makeDataRequestMessage(0x0C, 0x00, 0x00, 0x00, 0x00, 0x08),
               &errorDetail)) {
    emit errorOccurred(errorDetail);
  }
}

void RolandV8HdMidiSwitcher::parseDataSetMessage(const QByteArray &message) {
  if (message.size() < 13 ||
      static_cast<unsigned char>(message.at(0)) != 0xF0 ||
      static_cast<unsigned char>(message.at(1)) != 0x41 ||
      static_cast<unsigned char>(message.at(7)) != 0x12 ||
      static_cast<unsigned char>(message.at(message.size() - 1)) != 0xF7) {
    return;
  }
  const unsigned char address1 = static_cast<unsigned char>(message.at(8));
  const unsigned char address2 = static_cast<unsigned char>(message.at(9));
  const unsigned char address3 = static_cast<unsigned char>(message.at(10));
  if (address1 == 0x00 && address2 == 0x12 && address3 == 0x00) {
    const int sourceNumber =
        static_cast<unsigned char>(message.at(11)) + 1;
    if (sourceNumber >= 1 && sourceNumber <= 8) {
      if (lastProgramInputNumber_ == sourceNumber) {
        return;
      }
      lastProgramInputNumber_ = sourceNumber;
      emit programInputChanged(sourceNumber);
    }
    return;
  }
  if (address1 == 0x0C && address2 == 0x00 && address3 <= 0x07) {
    const int dataStart = 11;
    const int dataEnd = message.size() - 2;
    for (int index = dataStart; index < dataEnd; ++index) {
      const int sourceNumber = address3 + (index - dataStart) + 1;
      if (sourceNumber < 1 || sourceNumber > 8) {
        continue;
      }
      const int tally = static_cast<unsigned char>(message.at(index));
      if (tally == 0x01) {
        if (lastProgramInputNumber_ == sourceNumber) {
          return;
        }
        lastProgramInputNumber_ = sourceNumber;
        emit programInputChanged(sourceNumber);
        return;
      }
    }
  }
}

QString RolandV8HdMidiSwitcher::midiErrorText(unsigned int result) const {
#ifdef _WIN32
  wchar_t buffer[MAXERRORLENGTH]{};
  if (midiOutGetErrorTextW(result, buffer, MAXERRORLENGTH) ==
      MMSYSERR_NOERROR) {
    return QString::fromWCharArray(buffer);
  }
#endif
  return QStringLiteral("MIDI operation failed (%1).").arg(result);
}

#ifdef _WIN32
void CALLBACK RolandV8HdMidiSwitcher::midiInCallback(
    HMIDIIN handle, UINT message, DWORD_PTR instance, DWORD_PTR param1,
    DWORD_PTR param2) {
  Q_UNUSED(handle);
  Q_UNUSED(param2);
  auto *self = reinterpret_cast<RolandV8HdMidiSwitcher *>(instance);
  if (!self || self->closing_) {
    return;
  }
  if (message == MIM_DATA) {
    const DWORD shortMessage = static_cast<DWORD>(param1);
    const QPointer<RolandV8HdMidiSwitcher> guard(self);
    QMetaObject::invokeMethod(
        self,
        [guard, shortMessage] {
          if (guard && !guard->closing_) {
            guard->handleShortMessage(shortMessage);
          }
        },
        Qt::QueuedConnection);
  } else if (message == MIM_LONGDATA) {
    auto *header = reinterpret_cast<MIDIHDR *>(param1);
    if (!header || !header->lpData || header->dwBytesRecorded == 0) {
      return;
    }
    const QByteArray payload(header->lpData,
                             static_cast<qsizetype>(header->dwBytesRecorded));
    const QPointer<RolandV8HdMidiSwitcher> guard(self);
    QMetaObject::invokeMethod(
        self,
        [guard, payload] {
          if (!guard || guard->closing_) {
            return;
          }
          guard->parseDataSetMessage(payload);
          if (guard->midiIn_ && guard->inputHeader_.lpData &&
              !guard->closing_) {
            guard->inputHeader_.dwBytesRecorded = 0;
            midiInAddBuffer(guard->midiIn_, &guard->inputHeader_,
                            sizeof(guard->inputHeader_));
          }
        },
        Qt::QueuedConnection);
  }
}

void RolandV8HdMidiSwitcher::handleShortMessage(DWORD message) {
  const int status = message & 0xFF;
  if ((status & 0xF0) == 0xC0) {
    const int program = (message >> 8) & 0x7F;
    if (program >= 0 && program < 8) {
      emit programInputChanged(program + 1);
    }
  }
}

bool RolandV8HdMidiSwitcher::prepareInputBuffer(QString *errorDetail) {
  inputBuffer_.resize(512);
  inputHeader_ = MIDIHDR{};
  inputHeader_.lpData = inputBuffer_.data();
  inputHeader_.dwBufferLength = static_cast<DWORD>(inputBuffer_.size());
  MMRESULT result = midiInPrepareHeader(midiIn_, &inputHeader_,
                                        sizeof(inputHeader_));
  if (result != MMSYSERR_NOERROR) {
    if (errorDetail) {
      *errorDetail = midiErrorText(result);
    }
    return false;
  }
  result = midiInAddBuffer(midiIn_, &inputHeader_, sizeof(inputHeader_));
  if (result != MMSYSERR_NOERROR) {
    if (errorDetail) {
      *errorDetail = midiErrorText(result);
    }
    return false;
  }
  return true;
}

bool RolandV8HdMidiSwitcher::sendShortMessage(DWORD message, QString *errorDetail) {
  return queueOutput({}, message, errorDetail);
}

bool RolandV8HdMidiSwitcher::sendSysEx(const QByteArray &message, QString *errorDetail) {
  return queueOutput(message, 0, errorDetail);
}

bool RolandV8HdMidiSwitcher::queueOutput(QByteArray bytes, DWORD shortMessage, QString *errorDetail) {
  if (!midiOut_ || closing_ || outputFailed_ || outputQueue_.size() >= 128) {
    if (errorDetail) *errorDetail = QStringLiteral("Roland MIDI output is unavailable or its queue is full.");
    return false;
  }
  auto message = std::make_unique<OutputMessage>();
  message->bytes = std::move(bytes);
  message->shortMessage = shortMessage;
  outputQueue_.push_back(std::move(message));
  pumpOutput();
  return !outputFailed_;
}

void RolandV8HdMidiSwitcher::pumpOutput() {
  if (closing_ || outputFailed_ || outputPending_) return;
  while (!outputQueue_.empty()) {
    auto &message = *outputQueue_.front();
    MMRESULT result;
    if (message.bytes.isEmpty()) {
      result = midiOutShortMsg(midiOut_, message.shortMessage);
    } else {
      message.header.lpData = message.bytes.data();
      message.header.dwBufferLength = static_cast<DWORD>(message.bytes.size());
      result = midiOutPrepareHeader(midiOut_, &message.header, sizeof(message.header));
      if (result == MMSYSERR_NOERROR) {
        result = midiOutLongMsg(midiOut_, &message.header, sizeof(message.header));
        if (result == MMSYSERR_NOERROR) {
          outputPending_ = true;
          outputTimeout_.start();
          return;
        }
        midiOutUnprepareHeader(midiOut_, &message.header, sizeof(message.header));
      }
    }
    if (result != MMSYSERR_NOERROR) {
      outputFailed_ = true;
      emit errorOccurred(midiErrorText(result));
      return;
    }
    outputQueue_.pop_front();
  }
}

void RolandV8HdMidiSwitcher::completeOutput() {
  if (!outputPending_ || !midiOut_) return;
  auto &header = outputQueue_.front()->header;
  const auto result = midiOutUnprepareHeader(midiOut_, &header, sizeof(header));
  if (result == MIDIERR_STILLPLAYING) return;
  if (result != MMSYSERR_NOERROR) {
    outputFailed_ = true;
    emit errorOccurred(midiErrorText(result));
    return;
  }
  outputTimeout_.stop();
  outputPending_ = false;
  outputQueue_.pop_front();
  pumpOutput();
}

void RolandV8HdMidiSwitcher::outputTimedOut() {
  if (!outputPending_) return;
  outputFailed_ = true;
  tallyPollTimer_.stop();
  outputFadeTimer_.stop();
  emit errorOccurred(QStringLiteral("Roland MIDI output timed out. Reconnect the switcher to resume."));
  // Cancel the remaining transmission. Completion still owns the header; do
  // not release it just because the deadline expired or reset returned.
  midiOutReset(midiOut_);
  completeOutput();
}
#endif

} // namespace vtrpon
