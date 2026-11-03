#include "roland_v8hd_midi_switcher.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QThread>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL WinMM output line %d: %s\n",__LINE__,#x); return false; } } while(false)
#ifdef _WIN32
namespace vtrpon {
class RolandWinMMOutputTest {
public:
  static bool run() {
    for (const auto model : {RolandSwitcherModel::V8Hd, RolandSwitcherModel::V1Hd,
        RolandSwitcherModel::V1Sdi, RolandSwitcherModel::Vr4Hd, RolandSwitcherModel::V02HdMk2,
        RolandSwitcherModel::V160Hd, RolandSwitcherModel::V80Hd, RolandSwitcherModel::V1_4K,
        RolandSwitcherModel::Vr6Hd, RolandSwitcherModel::Vr120Hd}) {
      RolandV8HdMidiSwitcher device(model,{},true);
      // The actual completion function runs; only the unprepare API is replaced.
      // No MIDI device or synthetic handle is passed to the Windows driver.
      const auto cleanup=qScopeGuard([&] {
        device.midiOut_=nullptr; device.midiIn_=nullptr; device.outputPending_=false; device.outputQueue_.clear();
      });
      device.midiOut_=reinterpret_cast<HMIDIOUT>(quintptr(1));
      auto message=std::make_unique<RolandV8HdMidiSwitcher::OutputMessage>();
      message->bytes=QByteArray::fromHex("f041100000006812001200006ef7");
      message->header.lpData=message->bytes.data();
      message->header.dwBufferLength=DWORD(message->bytes.size());
      const auto *payload=message->header.lpData;
      device.outputQueue_.push_back(std::move(message)); device.outputPending_=true;
      bool release=false;
      device.unprepareForTest_=[&](MIDIHDR *header)->unsigned int {
        if(header->lpData!=payload || header->dwBufferLength!=14) return MMSYSERR_ERROR;
        return release ? MMSYSERR_NOERROR : MIDIERR_STILLPLAYING;
      };
      device.completeOutput();
      CHECK(device.outputPending_ && device.outputQueue_.size()==1 && !device.outputCooldown_);
      release=true; QElapsedTimer elapsed; elapsed.start(); device.completeOutput();
      CHECK(!device.outputPending_ && device.outputQueue_.empty() && device.outputCooldown_);
      device.midiOut_=nullptr;
      while(device.outputCooldown_ && elapsed.elapsed()<250) {
        QCoreApplication::processEvents(); QThread::msleep(1);
      }
      CHECK(!device.outputCooldown_ && elapsed.elapsed()>=20);
      // A completion's deferred pump must not affect a subsequently closed session.
      device.midiOut_=reinterpret_cast<HMIDIOUT>(quintptr(1));
      auto next=std::make_unique<RolandV8HdMidiSwitcher::OutputMessage>();
      next->bytes=QByteArray::fromHex("f041100000006812001200006ef7");
      device.outputQueue_.push_back(std::move(next)); device.outputPending_=true;
      device.unprepareForTest_=[](MIDIHDR *)->unsigned int { return MMSYSERR_NOERROR; };
      device.completeOutput(); device.midiOut_=nullptr; device.close();
      device.outputCooldown_=true; elapsed.restart();
      while(elapsed.elapsed()<35) { QCoreApplication::processEvents(); QThread::msleep(1); }
      CHECK(device.outputCooldown_); device.outputCooldown_=false;
      if(model==RolandSwitcherModel::V8Hd) {
        int changes=0;
        QObject::connect(&device,&SwitcherDevice::programInputChanged,&device,[&](int) { ++changes; });
        const auto handle=reinterpret_cast<HMIDIIN>(quintptr(2));
        auto bytes=QByteArray::fromHex("f041100000006812001200036bf7");
        MIDIHDR header{}; header.lpData=bytes.data(); header.dwBytesRecorded=DWORD(bytes.size());
        device.midiIn_=handle;
        RolandV8HdMidiSwitcher::midiInCallback(handle,MIM_LONGDATA,reinterpret_cast<DWORD_PTR>(&device),reinterpret_cast<DWORD_PTR>(&header),0);
        device.midiIn_=nullptr; device.close();
        // Reopen using the same numeric handle; the old queued packet must be ignored.
        device.midiIn_=handle; device.queryPending_=device.modelReady_=device.baselineKnown_=true;
        device.lastProgramInputNumber_=1;
        QCoreApplication::processEvents();
        CHECK(changes==0 && device.queryPending_ && device.lastProgramInputNumber_==1);
        RolandV8HdMidiSwitcher::midiInCallback(handle,MIM_LONGDATA,reinterpret_cast<DWORD_PTR>(&device),reinterpret_cast<DWORD_PTR>(&header),0);
        QCoreApplication::processEvents();
        CHECK(changes==1 && !device.queryPending_ && device.lastProgramInputNumber_==4);
        device.midiIn_=nullptr;
      }

      std::printf("PASS WinMM %s: buffer retention, >=20ms after completion, stale pump cancellation\n",rolandSwitcherProfile(model).name);
    }
    return true;
  }
};
}
bool runRolandWinMMOutputRegression() { return vtrpon::RolandWinMMOutputTest::run(); }
#else
bool runRolandWinMMOutputRegression() { return false; }
#endif
