#include "roland_v8hd_midi_switcher.h"

#include <QMetaObject>

#include <algorithm>
#include <utility>

namespace {

bool isRolandModelName(const QString &name, vtrpon::RolandSwitcherModel model) {
  const auto normalized = [](QString value) { value = value.toUpper(); value.remove('-'); value.remove(' '); value.replace("MKII", "MK2"); return value; };
  if (model == vtrpon::RolandSwitcherModel::V1Hd && (name.contains('+') || name.contains("PLUS", Qt::CaseInsensitive))) return false;
  if (model == vtrpon::RolandSwitcherModel::V8Hd && normalized(name).contains("V8HD")) return true;
  return normalized(name).contains(normalized(QLatin1String(vtrpon::rolandSwitcherProfile(model).name)));
}


} // namespace

namespace vtrpon {

RolandV8HdMidiSwitcher::RolandV8HdMidiSwitcher(
    bool receiveProgramInputChanges, QObject *parent)
    : RolandV8HdMidiSwitcher(RolandSwitcherModel::V8Hd, {}, receiveProgramInputChanges, parent) {}

RolandV8HdMidiSwitcher::RolandV8HdMidiSwitcher(RolandSwitcherModel model, RolandModelSettings settings,
                                           bool receiveProgramInputChanges, QObject *parent)
    : SwitcherDevice(parent), model_(model), settings_(normalizeRolandSettings(model, std::move(settings))),
      programQuery_(model), receiveProgramInputChanges_(receiveProgramInputChanges) {
  responseTimeout_.setSingleShot(true); responseTimeout_.setInterval(2500);
  connect(&responseTimeout_, &QTimer::timeout, this, [this] {
    close();
    const QString detail = QStringLiteral("%1 MIDI status response timed out. Check USB MIDI settings and device ID.").arg(QLatin1String(rolandSwitcherProfile(model_).name));
    emit connectionOpened(false, detail); emit errorOccurred(detail);
  });
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
  if (!*rolandSwitcherProfile(model_).midiModelId) {
    if (errorDetail) *errorDetail = QStringLiteral("USB MIDI control is unavailable for this model.");
    return false;
  }
#ifdef _WIN32
  close();
  closing_ = false;

  UINT outDeviceId = MIDI_MAPPER;
  bool foundOut = false;
  const UINT outDeviceCount = midiOutGetNumDevs();
  for (UINT index = 0; index < outDeviceCount; ++index) {
    MIDIOUTCAPSW caps{};
    if (midiOutGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
        isRolandModelName(QString::fromWCharArray(caps.szPname), model_)) {
      outDeviceId = index;
      foundOut = true;
      break;
    }
  }
  if (!foundOut) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("%1 MIDI output was not found.").arg(QLatin1String(rolandSwitcherProfile(model_).name));
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

  { // A valid status reply is required even when receive linking is off.
    const UINT inDeviceCount = midiInGetNumDevs();
    for (UINT index = 0; index < inDeviceCount; ++index) {
      MIDIINCAPSW caps{};
      if (midiInGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
          isRolandModelName(QString::fromWCharArray(caps.szPname), model_)) {
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

    if (!midiIn_) {
      if (errorDetail) *errorDetail = QStringLiteral("Selected switcher's MIDI input could not be opened.");
      close(); return false;
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
      if (!queryPending_) {
        if (errorDetail) *errorDetail = QStringLiteral("Could not send the initial MIDI status query.");
        close(); return false;
      }
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

void RolandV8HdMidiSwitcher::resetReceiveState() {
  responseTimeout_.stop(); queryPending_ = modelReady_ = baselineKnown_ = false;
  localTarget_ = lastProgramInputNumber_ = 0; midiReceiveBuffer_.clear();
  localTargetDeadline_ = QDeadlineTimer(); programQuery_.reset();
  panelModePending_ = fadeAssignPending_ = false; v1SdiFadeAssign_ = -1;
}
void RolandV8HdMidiSwitcher::close() {
#ifdef _WIN32
  closing_ = true; ++inputGeneration_;
#endif
  resetReceiveState();
  tallyPollTimer_.stop(); outputFadeTimer_.stop();
  outputFadeLevel_ = outputFadeTargetLevel_ = 0;
#ifdef _WIN32
  ++outputGeneration_; outputCooldown_ = false;
#endif
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
  if (sourceNumber < 1 || sourceNumber > rolandSwitcherProfile(model_).inputs || !modelReady_) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("%1 requires a ready MIDI connection and input 1-%2.").arg(QLatin1String(rolandSwitcherProfile(model_).name)).arg(rolandSwitcherProfile(model_).inputs);
    }
    return false;
  }
  if (model_ != RolandSwitcherModel::V8Hd) {
    const auto address = QByteArray::fromHex(rolandSwitcherProfile(model_).programAddress);
    const bool result = sendSysEx(rolandSysEx(model_, settings_.midiDeviceId, 0x12, address + QByteArray(1, char(sourceNumber-1))), errorDetail);
    if (result) { lastProgramInputNumber_ = localTarget_ = sourceNumber; localTargetDeadline_.setRemainingTime(5000); }
    return result;
  }
  const bool result = sendSysEx(makeDataSetMessage(0x00, 0x12, 0x00, QByteArray(1, char(sourceNumber - 1))), errorDetail);
  if (result) { lastProgramInputNumber_ = localTarget_ = sourceNumber; localTargetDeadline_.setRemainingTime(5000); }
  return result;
}

bool RolandV8HdMidiSwitcher::setFadeToBlack(bool enabled,
                                            QString *errorDetail) {
  if (!rolandSupportsFade(model_, RolandTransport::Midi)) {
    if (errorDetail) *errorDetail = QStringLiteral("FTB is unavailable over USB MIDI for this model.");
    return false;
  }
  if (model_ != RolandSwitcherModel::V8Hd) {
    if (!modelReady_) { if (errorDetail) *errorDetail = QStringLiteral("MIDI status is not ready."); return false; }
    QByteArray body;
    if (model_ == RolandSwitcherModel::Vr4Hd) {
      if (enabled && !sendSysEx(rolandSysEx(model_, settings_.midiDeviceId, 0x12, QByteArray::fromHex("00061200")), errorDetail)) return false;
      body = QByteArray::fromHex("000705"); body.append(char(enabled ? 1 : 0));
    } else if (model_ == RolandSwitcherModel::V02HdMk2) {
      if (enabled && !sendSysEx(rolandSysEx(model_, settings_.midiDeviceId, 0x12, QByteArray::fromHex("02010400")), errorDetail)) return false;
      body = QByteArray::fromHex("002503"); body.append(char(enabled ? 0 : 64));
    } else {
      if (model_ == RolandSwitcherModel::V1Sdi && (v1SdiFadeAssign_ < 0 || v1SdiFadeAssign_ == 2)) {
        if (errorDetail) *errorDetail = QStringLiteral("V-1SDI OUTPUT FADER must be VIDEO, V & A, or BLACK / A for FTB.");
        return false;
      }
      body = QByteArray::fromHex(model_ == RolandSwitcherModel::V1Hd ? "710006" : "710306");
      body.append(char(enabled ? 0 : 1)); body.append(char(0));
    }
    return sendSysEx(rolandSysEx(model_, settings_.midiDeviceId, 0x12, body), errorDetail);
  }

  if (!modelReady_) { if (errorDetail) *errorDetail = QStringLiteral("MIDI status is not ready."); return false; }
  if (enabled) {
    if (!sendSysEx(makeDataSetMessage(0x02, 0x01, 0x07,
                                      QByteArray(1, char(0x00))),
                   errorDetail)) {
      return false;
    }
  }
  startOutputFadeRamp(enabled);
  return true;
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

  return rolandSysEx(model_, settings_.midiDeviceId, 0x12, addressAndData);
}

QByteArray RolandV8HdMidiSwitcher::currentQueryAddress() const {
  if (panelModePending_) return QByteArray::fromHex(model_ == RolandSwitcherModel::V8Hd ? "020105" :
      model_ == RolandSwitcherModel::V1Hd ? "700115" : "700100");
  if (fadeAssignPending_) return QByteArray::fromHex("700121");
  return programQuery_.address();
}
bool RolandV8HdMidiSwitcher::sendProgramQuery() {
  QString error;
  if (!sendSysEx(rolandSysEx(model_, settings_.midiDeviceId, 0x11,
          currentQueryAddress() + QByteArray::fromHex("000001")), &error)) {
    close(); emit connectionOpened(false, error); emit errorOccurred(error); return false;
  }
  queryPending_ = true; responseTimeout_.start(); return true;
}
void RolandV8HdMidiSwitcher::requestTally() {
  if (queryPending_) return;
  programQuery_.reset();
  panelModePending_ = model_ == RolandSwitcherModel::V8Hd || model_ == RolandSwitcherModel::V1Sdi || model_ == RolandSwitcherModel::V1Hd;
  fadeAssignPending_ = model_ == RolandSwitcherModel::V1Sdi;
  sendProgramQuery();
}

void RolandV8HdMidiSwitcher::receiveMidiBytes(const QByteArray &bytes) {
  for (unsigned char byte : bytes) {
    if (byte >= 0xf8) continue;
    if (byte == 0xf0) { midiReceiveBuffer_.clear(); midiReceiveBuffer_.append(char(byte)); }
    else if (!midiReceiveBuffer_.isEmpty()) {
      midiReceiveBuffer_.append(char(byte));
      if (midiReceiveBuffer_.size() > 1024) midiReceiveBuffer_.clear();
      else if (byte == 0xf7) { const auto message = std::exchange(midiReceiveBuffer_, {}); parseDataSetMessage(message); }
      else if (byte & 0x80) midiReceiveBuffer_.clear();
    }
  }
}

void RolandV8HdMidiSwitcher::parseDataSetMessage(const QByteArray &message) {
  QByteArray address, data;
  if (!decodeRolandSysEx(model_, settings_.midiDeviceId, message, &address, &data) || data.size() != 1) return;
  // Program Change recalls a preset memory. Physical tally entries may include
  // AUX/composite layers and must not be interpreted as logical INPUT numbers.
  if (!queryPending_ || address != currentQueryAddress()) return;
  const int value = static_cast<unsigned char>(data[0]);
  if (panelModePending_) {
    if (value != 1) {
      close();
      const auto error = QStringLiteral("Set the switcher to PGM/PST mode before enabling input linking.");
      emit connectionOpened(false, error); emit errorOccurred(error);
      return;
    }
    emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(message.toHex(' ')));
    panelModePending_ = false; queryPending_ = false; responseTimeout_.stop(); sendProgramQuery(); return;
  }
  if (fadeAssignPending_) {
    if (value > 3) return;
    emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(message.toHex(' ')));
    v1SdiFadeAssign_ = value; fadeAssignPending_ = false; queryPending_ = false; responseTimeout_.stop(); sendProgramQuery(); return;
  }
  const auto result = programQuery_.accept(static_cast<unsigned char>(data[0]));
  if (result == RolandProgramQuery::Result::Invalid) return;
  emit diagnosticEvent(QStringLiteral("RX"), QString::fromLatin1(message.toHex(' ')));
  queryPending_ = false; responseTimeout_.stop();
  if (result == RolandProgramQuery::Result::More) { sendProgramQuery(); return; }
  const int physical = programQuery_.input();
  const int source = rolandHasUsbMidiChoice(model_) ? (physical == settings_.sourceNumber-1 ? physical+1 : 0) : physical+1;
  observeInput(source);
}
void RolandV8HdMidiSwitcher::markMidiReady() {
  if (modelReady_) return;
  modelReady_ = true;
  emit connectionOpened(true, QString::fromLatin1(rolandSwitcherProfile(model_).name) + QStringLiteral(" USB MIDI status received"));
}
void RolandV8HdMidiSwitcher::observeInput(int sourceNumber) {
  if (sourceNumber < 0 || sourceNumber > rolandSwitcherProfile(model_).inputs) return;
  const bool baseline = baselineKnown_;
  baselineKnown_ = true;
  markMidiReady();
  if (!baseline) {
    lastProgramInputNumber_ = sourceNumber;
    emit diagnosticEvent("BASELINE", QStringLiteral("Initial input state %1; no ON AIR request.").arg(sourceNumber));
    return;
  }
  if (localTarget_) {
    if (sourceNumber == localTarget_) {
      localTarget_ = 0; lastProgramInputNumber_ = sourceNumber; return;
    }
    if (!localTargetDeadline_.hasExpired()) return;
    localTarget_ = 0;
    emit diagnosticEvent("OBSERVATION", QStringLiteral("Requested input was not observed before another source became active."));
  }
  if (lastProgramInputNumber_ == sourceNumber) return;
  lastProgramInputNumber_ = sourceNumber;
  if (receiveProgramInputChanges_) emit programInputChanged(sourceNumber);
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

void RolandV8HdMidiSwitcher::handleShortMessage(quint32 message) {
  Q_UNUSED(message);
  // V-8HD Program Change recalls a preset. Poll the actual PGM selection.
  // V-1HD can use bank-dependent Program Change; its PGM is read by SysEx too.
}

#ifdef _WIN32
void CALLBACK RolandV8HdMidiSwitcher::midiInCallback(
    HMIDIIN handle, UINT message, DWORD_PTR instance, DWORD_PTR param1,
    DWORD_PTR param2) {
  Q_UNUSED(param2);
  auto *self = reinterpret_cast<RolandV8HdMidiSwitcher *>(instance);
  if (!self) return;
  const auto generation = self->inputGeneration_.load();
  if (self->closing_) return;
  if (message == MIM_DATA) {
    const DWORD shortMessage = static_cast<DWORD>(param1);
    const QPointer<RolandV8HdMidiSwitcher> guard(self);
    QMetaObject::invokeMethod(
        self,
        [guard, shortMessage, generation, handle] {
          if (guard && !guard->closing_ && guard->inputGeneration_.load() == generation && guard->midiIn_ == handle) {
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
        [guard, payload, generation, handle] {
          if (!guard || guard->closing_ || guard->inputGeneration_.load() != generation || guard->midiIn_ != handle) {
            return;
          }
          guard->receiveMidiBytes(payload);
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
  if (writeForTest_) return writeForTest_(message);
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
  if (closing_ || outputFailed_ || outputPending_ || outputCooldown_) return;
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
          emit diagnosticEvent(QStringLiteral("TX"), QString::fromLatin1(message.bytes.toHex(' ')));
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
    emit diagnosticEvent(QStringLiteral("DRIVER"), QStringLiteral("MIDI short message accepted (not a device acknowledgement)."));
    outputQueue_.pop_front();
  }
}

void RolandV8HdMidiSwitcher::completeOutput() {
  if (!outputPending_ || !midiOut_) return;
  auto &header = outputQueue_.front()->header;
  const auto result = unprepareForTest_ ? unprepareForTest_(&header)
      : midiOutUnprepareHeader(midiOut_, &header, sizeof(header));
  if (result == MIDIERR_STILLPLAYING) return;
  if (result != MMSYSERR_NOERROR) {
    outputFailed_ = true;
    emit errorOccurred(midiErrorText(result));
    return;
  }
  outputTimeout_.stop();
  outputPending_ = false;
  emit diagnosticEvent(QStringLiteral("DRIVER"), QStringLiteral("MIDI buffer returned by driver (not a device acknowledgement)."));
  outputQueue_.pop_front();
  // V-8HD has the same documented minimum as the other SysEx models.
  // State the minimum-delay requirement explicitly instead of relying on
  // the Qt version's short-singleShot default timer selection.
  outputCooldown_ = true; const auto generation = outputGeneration_;
  QTimer::singleShot(20, Qt::PreciseTimer, this, [this, generation] {
    if (generation != outputGeneration_) return;
    outputCooldown_ = false; pumpOutput();
  });
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
#else
bool RolandV8HdMidiSwitcher::sendSysEx(const QByteArray &message, QString *errorDetail) {
  if (writeForTest_) return writeForTest_(message);
  if (errorDetail) *errorDetail = QStringLiteral("WinMM MIDI output is unavailable on this platform.");
  return false;
}
#endif

} // namespace vtrpon
